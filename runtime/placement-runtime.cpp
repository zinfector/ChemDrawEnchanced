#include "placement-runtime.hpp"
#include "chemistry-worker.hpp"
#include "gpu-preview.hpp"
#include <intrin.h>
#include <string>

namespace cd {
namespace {
struct Cost { uint64_t ticks{},calls{},maximum{}; };
struct Placement {
    Obj doc{},page{};uint64_t revision{},started{},released{},geometry{},settled{};
    PlacementPhase phase{PlacementPhase::Pressed};
    std::array<Cost,size_t(PlacementCost::Count)> costs{};
    unsigned geometryIdlePasses{},deferredChemistry{},batchedRects{},lazyNotifications{};
    bool derivedDeferred{};
    PlacementDisplayTiming display{};
};
static std::unordered_map<HWND,Placement> placements;
static uint64_t nextPlacement{};
static thread_local HWND inputWindow{};
static thread_local Obj idleDocument{};
static thread_local bool idleYielded{};
static thread_local bool synchronousIdle{};
static thread_local bool flushing{};
struct Damage { Obj page{};RectD bounds{}; };
static thread_local std::array<Damage,16> damage;
static thread_local size_t damageCount{};
static thread_local unsigned inputDepth{},chemistryDepth{};
static thread_local uint64_t chemistryStarted{};
static thread_local uint32_t chemistryStartTick{};
static thread_local bool chemistryTickStarted{},chemistryExpired{};
struct ChemistryEpoch { uint64_t changed{},notified{};uint32_t marker{}; };
static std::unordered_map<Obj,ChemistryEpoch> chemistryEpochs;
static thread_local Obj notificationSelection{};
using Modified=void(*)(Obj,const RectD*);
using Chemistry=bool(*)(Obj,bool);
using Validate=int(*)(Obj,long,bool);
using Regenerate=void(*)(Obj,Obj,bool);
using Stereo=void(*)(Obj,Obj);
using Analyzer=Obj(*)(Obj);
static Modified oldModified{};
static Chemistry oldChemistry{};
static Validate oldValidate{};
static Regenerate oldRegenerate{};
static Stereo oldStereo{};
static Analyzer oldAnalyzer{};
static uint32_t (*oldHash)(Obj){};
static void (*oldChemistryChanged)(Obj){};
static uint32_t (*oldTick)(){};

uint64_t ticks() noexcept { LARGE_INTEGER t{};QueryPerformanceCounter(&t);return uint64_t(t.QuadPart); }
uint64_t frequency() noexcept {
    static const auto f=[] { LARGE_INTEGER t{};QueryPerformanceFrequency(&t);return uint64_t(t.QuadPart); }();return f;
}
HWND documentWindow(Obj doc) noexcept {
    Obj port=doc?at<Obj>(doc,0x258):nullptr;
    return port?fn<HWND(*)(Obj)>(0x624a70)(port):nullptr;
}
Placement* current(HWND* window=nullptr) noexcept {
    const HWND w=inputWindow?inputWindow:documentWindow(idleDocument);
    auto i=placements.find(w);if(i==placements.end()||i->second.phase==PlacementPhase::Cancelled) return nullptr;
    if(window) *window=w;return &i->second;
}
struct Report { uint64_t revision{};std::string text; };
static SRWLOCK reportLock=SRWLOCK_INIT;
static SRWLOCK pendingReportLock=SRWLOCK_INIT;
static std::unique_ptr<Report> pendingReport;
static bool reportScheduled{};
static uint64_t latestReport{};
void writeOneReport(const Report& report) noexcept {
    wchar_t folder[32768]{};
    const DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",folder,DWORD(std::size(folder)));
    if(n&&n<std::size(folder)) {
        try {
            const std::wstring dir=std::wstring(folder)+L"\\ChemDrawLatency";
            CreateDirectoryW(dir.c_str(),nullptr);
            const auto path=dir+L"\\placement-profile.txt";
            AcquireSRWLockExclusive(&reportLock);
            if(report.revision>=latestReport) {
                HANDLE file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(file!=INVALID_HANDLE_VALUE) {
                    DWORD written{};WriteFile(file,report.text.data(),DWORD(report.text.size()),&written,nullptr);CloseHandle(file);
                    latestReport=report.revision;
                }
            }
            ReleaseSRWLockExclusive(&reportLock);
        } catch(...) {}
    }
}
DWORD WINAPI writeReport(void*) noexcept {
    for(;;) {
        AcquireSRWLockExclusive(&pendingReportLock);
        auto report=std::move(pendingReport);
        if(!report) reportScheduled=false;
        ReleaseSRWLockExclusive(&pendingReportLock);
        if(!report) return 0;
        writeOneReport(*report);
    }
}
void report(const Placement& p) noexcept {
    try {
        const double ms=1000.0/double(frequency());char line[240]{};
        auto r=std::make_unique<Report>();r->revision=p.revision;
        sprintf_s(line,"Revision 53 placement %llu; phase %u; page objects %llu\r\n",
            static_cast<unsigned long long>(p.revision),unsigned(p.phase),
            static_cast<unsigned long long>(p.page?at<size_t>(p.page,0xd0):0));r->text=line;
        auto elapsed=[&](uint64_t end) { return end&&end>=p.started?double(end-p.started)*ms:0.0; };
        sprintf_s(line,"Press to release callback: %.3f ms; geometry publication: %.3f ms; chemistry completion: %.3f ms\r\n",
            elapsed(p.released),elapsed(p.geometry),elapsed(p.settled));r->text+=line;
        r->text+="Inclusive callback times; nested phases overlap. Release callback time includes time held.\r\n";
        if(p.display.presented) {
            sprintf_s(line,"Physical release to GPU submission: %.3f ms; native queue to GPU submission: %.3f ms\r\n",
                p.display.physicalRelease&&p.display.presented>=p.display.physicalRelease?
                    double(p.display.presented-p.display.physicalRelease)*ms:0.0,
                p.display.nativeQueued&&p.display.presented>=p.display.nativeQueued?
                    double(p.display.presented-p.display.nativeQueued)*ms:0.0);r->text+=line;
            sprintf_s(line,"Placement GPU uploads %.3f ms; frame drawing %.3f ms; discarded frames %llu\r\n",
                double(p.display.uploadTicks)*ms,double(p.display.drawTicks)*ms,
                static_cast<unsigned long long>(p.display.discarded));r->text+=line;
        }
        constexpr const char* names[]={"Native input","Native tracking","Object validation","Analyzer rebuild","Stereochemistry","Drawing preparation","Changed chemistry","Geometry publication","Worker snapshot preparation","Native window idle","Native pixel readback"};
        for(size_t i=0;i<p.costs.size();++i) {
            const auto& c=p.costs[i];sprintf_s(line,"%s: %.3f ms; calls %llu; maximum %.3f ms\r\n",names[i],
                double(c.ticks)*ms,static_cast<unsigned long long>(c.calls),double(c.maximum)*ms);r->text+=line;
        }
        sprintf_s(line,"Deferred idle chemistry callbacks: %u; merged redraw rectangles: %u; lazy change notifications: %u\r\n",
            p.deferredChemistry,p.batchedRects,p.lazyNotifications);r->text+=line;
        r->text+=chemistryWorkerAvailable()?"Isolated chemistry worker available; unsupported graphs use native solver.\r\n":"Isolated chemistry worker unavailable; using native solver.\r\n";
        const auto worker=chemistryWorkerStats();
        sprintf_s(line,"All native UI CIP constructor/getter calls: %llu; %.3f ms; maximum %.3f ms; maximum caller 0x%llx (ChemDrawBase RVA or external address)\r\n",
            static_cast<unsigned long long>(worker.nativeCalls),double(worker.nativeTicks)*ms,
            double(worker.nativeMaximum)*ms,static_cast<unsigned long long>(worker.nativeMaximumCaller));r->text+=line;
        sprintf_s(line,"Worker lifetime totals: jobs %llu; native result reuse %llu; failures %llu; cancellations %llu; %.3f ms; max %.3f ms\r\n",
            static_cast<unsigned long long>(worker.submitted),static_cast<unsigned long long>(worker.hits),
            static_cast<unsigned long long>(worker.failed),static_cast<unsigned long long>(worker.cancelled),
            double(worker.ticks)*ms,double(worker.maximum)*ms);r->text+=line;
        sprintf_s(line,"Worker preflights %llu; groups %llu; native solver misses %llu\r\n",
            static_cast<unsigned long long>(worker.preflights),static_cast<unsigned long long>(worker.groups),
            static_cast<unsigned long long>(worker.misses));r->text+=line;
        sprintf_s(line,"Isolated workers %u; proven non-stereogenic fast paths %llu; exact disconnected-component reuse %llu\r\n",
            worker.workers,static_cast<unsigned long long>(worker.fastPaths),
            static_cast<unsigned long long>(worker.componentReuse));r->text+=line;
        constexpr const char* misses[]={"Unsupported snapshot","Exact snapshot absent","Result pending",
            "Result failed/cancelled","Token allocation"};
        for(size_t i=0;i<worker.missesByReason.size();++i) {
            sprintf_s(line,"Native CIP miss %s: %llu\r\n",misses[i],
                static_cast<unsigned long long>(worker.missesByReason[i]));r->text+=line;
        }
        constexpr const char* rejects[]={"Molecule metadata/properties","Graph size","Atom subclass/label","Atom type","Coordinates","Bond type/connectivity"};
        for(size_t i=0;i<worker.rejected.size();++i) {
            sprintf_s(line,"Worker fallback %s: %llu\r\n",rejects[i],static_cast<unsigned long long>(worker.rejected[i]));r->text+=line;
        }
        constexpr const char* metadata[]={"Name","R-group table","Formula cache","Custom timeout",
            "Molecule properties","Auxiliary records","Parent reaction"};
        for(size_t i=0;i<worker.metadataRejected.size();++i) {
            sprintf_s(line,"Worker metadata fallback %s: %llu\r\n",metadata[i],
                static_cast<unsigned long long>(worker.metadataRejected[i]));r->text+=line;
        }
        // One writer and one coalesced payload. A chemistry-complete report
        // replaces an intermediate report instead of being silently dropped.
        AcquireSRWLockExclusive(&pendingReportLock);
        pendingReport=std::move(r);
        if(!reportScheduled) {
            reportScheduled=true;
            if(!QueueUserWorkItem(writeReport,nullptr,WT_EXECUTEDEFAULT)) { reportScheduled=false;pendingReport.reset(); }
        }
        ReleaseSRWLockExclusive(&pendingReportLock);
    } catch(...) {}
}
void modified(Obj page,const RectD* rect) {
    if(!onUI()||!inputDepth||flushing||!rect||!valid(*rect)||
        !current()||current()->page!=page) { oldModified(page,rect);return; }
    for(size_t i=0;i<damageCount;++i) if(damage[i].page==page) {
        auto& b=damage[i].bounds;b={std::min(b.t,rect->t),std::min(b.l,rect->l),std::max(b.b,rect->b),std::max(b.r,rect->r)};
        if(auto p=current()) ++p->batchedRects;return;
    }
    if(damageCount==damage.size()) flushPlacementDamage();
    damage[damageCount++]={page,*rect};
    // Invalidate immediately once; subsequent calls only expand the damage.
    // Native offscreen validity must reflect mutations even during modal input.
    oldModified(page,rect);
}
int validate(Obj selection,long budget,bool all) {
    PlacementTimer timer(PlacementCost::Validation);
    // Preserve explicit synchronous callers and native return conventions.
    return oldValidate(selection,onUI()&&idleDocument&&budget>1?1:budget,all);
}
void regenerate(Obj selection,Obj result,bool queries) {
    PlacementTimer timer(PlacementCost::Analyzer);oldRegenerate(selection,result,queries);
}
void chemistryChanged(Obj object) {
    oldChemistryChanged(object);
    if(!onUI()||!object) return;
    const Obj page=at<Obj>(object,0x60);if(!page||!at<size_t>(page,0xd58)) return;
    forgetChemistryWorkerPage(page);
    try { ++chemistryEpochs[page].changed; } catch(const std::bad_alloc&) { chemistryEpochs.erase(page); }
}
Obj notificationAnalyzer(Obj selection) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    // This one direct call is followed immediately by GetChemistryHash. It
    // uses no analyzer data, virtual methods, or ownership operations. Pair
    // the calls as a private notification bridge; all analysis callers still
    // receive a real native analyzer and its exact chemical hash.
    if(onUI()&&chemistryDepth&&caller==base+0x718cf2&&selection&&
        !at<Obj>(selection,0x160)) {
        const Obj page=at<Obj>(selection,0x10);
        auto p=current();
        if(page&&p&&p->page==page&&p->phase!=PlacementPhase::ChemistrySettled&&
            !at<unsigned char>(at<Obj>(page,8),0x40c)&&
            selection==static_cast<std::byte*>(page)+0x500) {
            if(auto i=chemistryEpochs.find(page);i!=chemistryEpochs.end()&&i->second.changed) {
                notificationSelection=selection;return selection;
            }
        }
    }
    return oldAnalyzer(selection);
}
uint32_t notificationHash(Obj object) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(onUI()&&chemistryDepth&&caller==base+0x718cfa&&object==notificationSelection&&notificationSelection) {
        notificationSelection=nullptr;
        const Obj page=at<Obj>(object,0x10);
        auto i=chemistryEpochs.find(page);
        if(i!=chemistryEpochs.end()) {
            auto& e=i->second;
            if(e.notified!=e.changed) { e.marker=at<uint32_t>(page,0x1618)+1;e.notified=e.changed; }
            if(auto p=current()) ++p->lazyNotifications;
            return e.marker;
        }
        return oldHash(oldAnalyzer(object));
    }
    return oldHash(object);
}
void stereo(Obj self,Obj objects) {
    PlacementTimer timer(PlacementCost::Stereo);oldStereo(self,objects);
    // Preserve the rest of this component's native transaction, then yield at
    // the queue's next existing boundary instead of starting another component.
    if(chemistryDepth) chemistryExpired=true;
}
uint32_t tick() {
    const uint32_t real=oldTick();
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-base;
    if(!chemistryDepth||caller<0x718c40||caller>=0x719b10) return real;
    if(!chemistryTickStarted) { chemistryTickStarted=true;chemistryStartTick=real;return real; }
    // ProcessSomeChangedChemistry checks its deadline between components. Make
    // that existing boundary yield after 8 ms, including its trailing error pass.
    // Never alter clocks read by ranking, drawing, autosave, or other callers.
    if(chemistryExpired||ticks()-chemistryStarted>=frequency()/125) {
        chemistryExpired=true;return chemistryStartTick+5;
    }
    return real;
}
constexpr UINT_PTR chemistryWakeTimer=0x43445046;
void CALLBACK chemistryWake(HWND w,UINT,UINT_PTR timer,DWORD) {
    KillTimer(w,timer);PostMessageW(w,WM_NULL,0,0);
}
bool postponeChemistry(HWND w,UINT delay) {
    // A deferred idle callback must not keep the managed idle loop spinning.
    // The timer wakes its normal dispatcher; it never calls native idle itself.
    return SetTimer(w,chemistryWakeTimer,std::max<UINT>(delay,8),chemistryWake)==0;
}
bool chemistry(Obj page,bool force) {
    // Guard before the native transaction, including nested input/window idle.
    // Its later CIP constructor is too late to defer safely: UndoResetter and
    // chemistry mutations have already started by then. Explicit drain/export
    // callers retain their synchronous contract.
    if(onUI()&&page&&!force&&!synchronousIdle&&!chemistryDepth) {
        const Obj doc=at<Obj>(page,8);
        const HWND window=documentWindow(doc);
        auto placement=placements.find(window);
        auto p=placement==placements.end()?nullptr:&placement->second;
        if(p&&p->page==page&&p->phase!=PlacementPhase::Cancelled&&p->phase!=PlacementPhase::ChemistrySettled) {
            if(inputDepth||trackingDepth||p->phase==PlacementPhase::Pressed||
                (GetAsyncKeyState(VK_LBUTTON)&0x8000)||
                ((GetQueueStatus(QS_MOUSEBUTTON|QS_KEY)&0xffff0000UL)!=0)||
                at<unsigned char>(doc,0x40c)) {
                ++p->deferredChemistry;p->derivedDeferred=true;
                return postponeChemistry(window,8);
            }
            const bool pendingGeometry=p->phase==PlacementPhase::Pressed||p->phase==PlacementPhase::Released;
            // Real window idle owns port initialization and publication. Keep
            // yielding chemistry until that geometry is actually published,
            // rather than allowing an indivisible solver through on pass two.
            // Retain the bounded native fallback if publication cannot finish.
            const bool geometryFirst=pendingGeometry&&p->geometryIdlePasses<64;
            if(pendingGeometry&&idleDocument==doc) ++p->geometryIdlePasses;
            // The native base idle calls chemistry BEFORE the window draws its
            // offscreen page. Waiting for publication to schedule the worker
            // let that first indivisible solver call through on the UI thread.
            const bool workerPending=!preparePlacementChemistry(page);
            if(geometryFirst||workerPending||(idleDocument==doc&&idleYielded)) {
                ++p->deferredChemistry;p->derivedDeferred=true;
                return postponeChemistry(window,8);
            }
        }
        if(p&&p->page==page) { if(idleDocument==doc) idleYielded=true;p->derivedDeferred=false; }
    }
    PlacementTimer timer(PlacementCost::Chemistry);
    const bool sliced=onUI()&&idleDocument&&!force&&!chemistryDepth;
    if(!sliced) return oldChemistry(page,force);
    ++chemistryDepth;chemistryStarted=ticks();chemistryTickStarted=chemistryExpired=false;
    bool result{};
    try { result=oldChemistry(page,force); } catch(...) {
        forgetChemistryWorkerPage(page);notificationSelection=nullptr;--chemistryDepth;throw;
    }
    forgetChemistryWorkerPage(page);notificationSelection=nullptr;--chemistryDepth;return result;
}
}

PlacementTimer::PlacementTimer(PlacementCost c) noexcept:cost(c) {
    if(onUI()) if(auto p=current(&window)) { revision=p->revision;started=ticks(); }
}
PlacementTimer::~PlacementTimer() {
    if(!revision) return;
    auto i=placements.find(window);if(i==placements.end()||i->second.revision!=revision) return;
    auto& c=i->second.costs[size_t(cost)];const auto elapsed=ticks()-started;c.ticks+=elapsed;++c.calls;c.maximum=std::max(c.maximum,elapsed);
}
PlacementInputScope::PlacementInputScope(HWND w,Obj doc,UINT message):previous(inputWindow) {
    if(!patchEnabled(5)||!onUI()||!doc||documentWindow(doc)!=w||(message!=WM_LBUTTONDOWN&&message!=WM_LBUTTONUP)) return;
    if(message==WM_LBUTTONDOWN&&!inputDepth) {
        Placement p{};p.doc=doc;p.page=mainPage(doc);p.revision=++nextPlacement;p.started=ticks();
        forgetChemistryWorkerPage(p.page);
        try { placements.insert_or_assign(w,p); } catch(const std::bad_alloc&) { return; }
    }
    entered=true;++inputDepth;inputWindow=w;
    if(message==WM_LBUTTONUP) placementReleased(w);
}
PlacementInputScope::~PlacementInputScope() {
    if(!entered) return;
    flushPlacementDamage();--inputDepth;
    if(!inputDepth&&!(GetAsyncKeyState(VK_LBUTTON)&0x8000)) placementReleased(inputWindow);
    // A nested idle may report completion before its enclosing input callback
    // returns. Publish again after InputTimer's destructor recorded that cost.
    if(!inputDepth) if(auto p=current();p&&p->phase>=PlacementPhase::GeometryCommitted) report(*p);
    inputWindow=previous;
}
PlacementIdleScope::PlacementIdleScope(Obj doc,bool allowScheduling,bool synchronous) noexcept:
    previous(idleDocument),previousYield(idleYielded),previousSynchronous(synchronousIdle) {
    idleDocument=allowScheduling?doc:nullptr;idleYielded=false;synchronousIdle=synchronousIdle||synchronous;
}
PlacementIdleScope::~PlacementIdleScope() { idleDocument=previous;idleYielded=previousYield;synchronousIdle=previousSynchronous; }
uint64_t placementRevision(HWND w) noexcept { auto i=placements.find(w);return i==placements.end()?0:i->second.revision; }
void placementReleased(HWND w) noexcept {
    auto i=placements.find(w);if(i==placements.end()) return;auto& p=i->second;
    if(p.phase==PlacementPhase::Pressed) { p.phase=PlacementPhase::Released;p.released=ticks(); }
}
void placementGeometryCommitted(HWND w) noexcept {
    auto i=placements.find(w);if(i==placements.end()) return;auto& p=i->second;
    if(p.phase==PlacementPhase::Pressed||p.phase==PlacementPhase::Released) {
        placementReleased(w);p.phase=PlacementPhase::GeometryCommitted;p.geometry=ticks();
        report(p);
    }
}
void placementDisplayPresented(HWND w,const PlacementDisplayTiming& timing) noexcept {
    auto i=placements.find(w);if(i==placements.end()||i->second.revision!=timing.revision) return;
    i->second.display=timing;report(i->second);
}
void placementChemistrySettled(HWND w) noexcept {
    auto i=placements.find(w);if(i==placements.end()) return;auto& p=i->second;
    if(p.phase==PlacementPhase::GeometryCommitted&&!p.derivedDeferred&&p.page&&
        !at<size_t>(p.page,0xd58)&&!at<unsigned char>(p.doc,0x40c)) {
        p.phase=PlacementPhase::ChemistrySettled;p.settled=ticks();report(p);
    }
}
void cancelPlacement(HWND w) noexcept {
    KillTimer(w,chemistryWakeTimer);
    auto i=placements.find(w);if(i!=placements.end()) { forgetChemistryWorkerPage(i->second.page);placements.erase(i); }
}
void forgetPlacementPage(Obj page) noexcept {
    if(!onUI()) return;
    chemistryEpochs.erase(page);
    forgetChemistryWorkerPage(page);
    for(auto i=placements.begin();i!=placements.end();) {
        if(i->second.page==page) { KillTimer(i->first,chemistryWakeTimer);i=placements.erase(i); } else ++i;
    }
    for(size_t i=0;i<damageCount;) { if(damage[i].page==page) damage[i]=damage[--damageCount];else ++i; }
}
void flushPlacementDamage() {
    if(!onUI()||!damageCount||flushing) return;
    flushing=true;
    try {
        while(damageCount) { const auto d=damage[--damageCount];oldModified(d.page,&d.bounds); }
    } catch(...) { flushing=false;throw; }
    flushing=false;
}
bool placementGeometryPending(Obj doc) noexcept {
    auto i=placements.find(documentWindow(doc));return i!=placements.end()&&i->second.doc==doc&&
        (i->second.phase==PlacementPhase::Pressed||i->second.phase==PlacementPhase::Released);
}
bool placementChemistryPending(Obj doc) noexcept {
    auto i=placements.find(documentWindow(doc));return i!=placements.end()&&i->second.doc==doc&&i->second.phase==PlacementPhase::GeometryCommitted;
}
void installPlacementRuntime() {
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&writeReport),&pinned)) throw std::runtime_error("Placement worker module lifetime");
    hook(0x7183e0,modified,oldModified);
    hook(0x718c40,chemistry,oldChemistry);
    hook(0x260af0,validate,oldValidate);
    hook(0x25ffd0,regenerate,oldRegenerate);
    hook(0x325bc0,chemistryChanged,oldChemistryChanged);
    hook(0x253d20,notificationAnalyzer,oldAnalyzer);
    hook(0x109fa0,notificationHash,oldHash);
    hook(0x4d3b80,stereo,oldStereo);
    hook(0x5d5500,tick,oldTick);
    if (patchEnabled(6)) installChemistryWorker();
}
}

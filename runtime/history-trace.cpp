#include "runtime.hpp"
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace cd {
namespace {
// Passive, bounded metadata only. No object geometry, documents, selection
// changes, replay, input synthesis, or native history calls from the observer.
constexpr size_t eventLimit=192;
std::array<std::array<char,384>,eventLimit> events{};
size_t eventCount{};
unsigned clicks{};
HWND gesture{};
std::mutex reportMutex;
std::condition_variable reportReady;
std::string pendingReport;
bool (*oldFire)(unsigned short,Obj,int,Obj){};
void (*oldStartRecord)(Obj){};
void (*oldAppendCommand)(Obj,Obj,bool){};
using Handler=uint64_t(*)(Obj);
Handler oldUndoHandler{},oldRedoHandler{};

template<class... Args> void event(const char* format,Args... args) {
    if(!onUI())return;
    auto& line=events[eventCount++%eventLimit];
    const int prefix=sprintf_s(line.data(),line.size(),"%llu ",GetTickCount64());
    if(prefix>0)sprintf_s(line.data()+prefix,line.size()-size_t(prefix),format,args...);
}
Obj frontDocument() { return fn<Obj(*)()>(0x5fc0b0)(); }
Obj stackTop(Obj doc,bool redo) {
    const size_t count=at<size_t>(doc,redo?0xe8:0xc0);
    if(!count)return nullptr;
    const size_t index=at<size_t>(doc,redo?0xe0:0xb8)+count-1;
    const auto map=at<Obj**>(doc,redo?0xd0:0xa8);
    const size_t blocks=at<size_t>(doc,redo?0xd8:0xb0);
    return map&&blocks?map[(index>>1)&(blocks-1)][index&1]:nullptr;
}
void record(const char* name,Obj saved) {
    if(!saved) {event("%s none",name);return;}
    const auto first=at<Obj*>(saved,8),last=at<Obj*>(saved,0x10);
    const size_t count=first&&last?size_t(last-first):0;
    event("%s record=%p id=%u label=%d commands=%zu",name,saved,
        at<unsigned>(saved,0),at<int>(saved,0x64),count);
    for(size_t n=0;n<std::min(count,size_t(4));++n)if(first[n])
        event("  command[%zu] vtable=+%llx label=%d owner=%p",n,
            static_cast<unsigned long long>(at<uintptr_t>(first[n],0)-base),
            at<int>(first[n],8),at<Obj>(first[n],0x10));
}
void snapshot(const char* name,Obj doc) {
    if(!onUI())return;
    if(!doc||!fn<bool(*)(Obj)>(0x14cbc0)(doc)) {
        event("%s document=none-or-retired",name);return;
    }
    event("%s doc=%p undo=%zu redo=%zu disable=%d executing=%u history=%u",name,doc,
        at<size_t>(doc,0xc0),at<size_t>(doc,0xe8),at<int>(doc,0x264),
        unsigned(at<unsigned char>(doc,0x260)),unsigned(at<unsigned char>(doc,0x268)));
    record("pending",at<Obj>(doc,0xf0));
    record("undo-top",stackTop(doc,false));record("redo-top",stackTop(doc,true));
}
void publish() noexcept {
    if(!onUI())return;
    try {
        std::string report="Revision 94 passive toolbar/history trace\r\n"
            "Last 192 events; first 64 toolbar gestures per session. Times are monotonic milliseconds.\r\n"
            "Command vtables are ChemDrawBase.dll RVAs; up to four command types per record.\r\n";
        const size_t begin=eventCount>eventLimit?eventCount-eventLimit:0;
        for(size_t n=begin;n<eventCount;++n) {
            report+=events[n%eventLimit].data();report+="\r\n";
        }
        {std::lock_guard guard(reportMutex);pendingReport=std::move(report);}
        reportReady.notify_one();
    } catch(...) {} // Diagnostics cannot change native command execution.
}
bool fire(unsigned short command,Obj text,int flags,Obj listener) {
    if(onUI()&&gesture)event("SFire command=%u listener=%p flags=%d",unsigned(command),listener,flags);
    return oldFire(command,text,flags,listener);
}
void startRecord(Obj doc) {
    if(onUI()&&clicks<64)event("StartNewUndo enter doc=%p pending=%p disable=%d",doc,
        at<Obj>(doc,0xf0),at<int>(doc,0x264));
    oldStartRecord(doc);
    if(onUI()&&clicks<64)record("StartNewUndo result",at<Obj>(doc,0xf0));
}
void appendCommand(Obj doc,Obj command,bool force) {
    if(onUI()&&clicks<64&&command)event("UndoByObject doc=%p type=+%llx label=%d force=%u disable=%d executing=%u",
        doc,static_cast<unsigned long long>(at<uintptr_t>(command,0)-base),at<int>(command,8),
        unsigned(force),at<int>(doc,0x264),unsigned(at<unsigned char>(doc,0x260)));
    oldAppendCommand(doc,command,force);
}
uint64_t handler(Obj context,Handler original,bool redo) {
    if(onUI())snapshot(redo?"Redo handler enter":"Undo handler enter",at<Obj>(context,0x28));
    const auto result=original(context);
    if(onUI()) {snapshot(redo?"Redo handler exit":"Undo handler exit",frontDocument());if(!gesture)publish();}
    return result;
}
uint64_t undoHandler(Obj context) {return handler(context,oldUndoHandler,false);}
uint64_t redoHandler(Obj context) {return handler(context,oldRedoHandler,true);}
}
void traceHistoryButton(HWND w,UINT message,WPARAM flags,LPARAM position,bool before) {
    if (!patchEnabled(12)) return;
    if(!onUI())return;
    if(before&&(message==WM_LBUTTONDOWN||message==WM_LBUTTONDBLCLK)) {
        if(clicks>=64)return;
        ++clicks;gesture=w;event("Toolbar gesture %u begin",clicks);
        snapshot("button before press",frontDocument());
    }
    if(gesture!=w)return;
    if(message!=WM_LBUTTONDOWN&&message!=WM_LBUTTONUP&&message!=WM_LBUTTONDBLCLK&&
        message!=WM_MOUSEMOVE&&message!=WM_CAPTURECHANGED&&message!=WM_CANCELMODE&&
        message!=WM_ENABLE&&message!=WM_KILLFOCUS&&message!=WM_DESTROY&&message!=WM_NCDESTROY)return;
    const auto wrapper=reinterpret_cast<Obj>(GetWindowLongPtrW(w,GWLP_USERDATA));
    int pressed=-1;unsigned command{};
    if(wrapper&&at<uintptr_t>(wrapper,0x58)==base+0x8e2ea0) {
        pressed=at<unsigned char>(wrapper,0x99);
        if(auto model=at<Obj>(wrapper,0xa0))command=at<unsigned short>(model,0x52);
    }
    event("button %s msg=0x%x hwnd=%p flags=%llx xy=(%d,%d) pressed=%d command=%u enabled=%u capture=%p focus=%p active=%p",
        before?"enter":"exit",message,w,static_cast<unsigned long long>(flags),
        int(short(LOWORD(position))),int(short(HIWORD(position))),pressed,command,
        unsigned(IsWindowEnabled(w)!=FALSE),GetCapture(),GetFocus(),GetActiveWindow());
    if(message==WM_LBUTTONUP) {
        snapshot(before?"button before release":"button after release",frontDocument());
        if(!before) {gesture=nullptr;publish();}
    } else if(!before&&(message==WM_CANCELMODE||message==WM_DESTROY||message==WM_NCDESTROY)) {
        gesture=nullptr;publish();
    }
}
void traceHistoryToolbarCommand(HWND w,WPARAM a,LPARAM b,bool before) {
    if (!patchEnabled(12)) return;
    if(onUI()&&gesture)event("toolbar command %s parent=%p id=%u notification=%u child=%p",
        before?"enter":"exit",w,unsigned(LOWORD(a)),unsigned(HIWORD(a)),reinterpret_cast<HWND>(b));
}
void traceHistoryOperation(Obj doc,bool redo,bool before) {
    if (!patchEnabled(12)) return;
    snapshot(redo?(before?"PerformRedo enter":"PerformRedo exit"):
        (before?"PerformUndo enter":"PerformUndo exit"),doc);
}
void traceHistoryPublication(Obj doc,bool redo,bool before) {
    if (!patchEnabled(12)) return;
    if(onUI()&&clicks<64)event("%s %s doc=%p pending=%p undo=%zu redo=%zu",
        redo?"PushRedoStack":"PushUndoStack",before?"enter":"exit",doc,
        at<Obj>(doc,0xf0),at<size_t>(doc,0xc0),at<size_t>(doc,0xe8));
}
void installHistoryTrace() {
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&installHistoryTrace),&pinned))throw std::runtime_error("History trace module lifetime");
    std::thread([] {
        for(;;) {
            std::string report;
            {std::unique_lock lock(reportMutex);reportReady.wait(lock,[]{return !pendingReport.empty();});report.swap(pendingReport);}
            writeHistoryStatus(report.c_str());
        }
    }).detach();
    hook(0x732870,fire,oldFire);
    hook(0x503410,startRecord,oldStartRecord);
    hook(0x503480,appendCommand,oldAppendCommand);
    hook(0x6971d0,undoHandler,oldUndoHandler);hook(0x696dc0,redoHandler,oldRedoHandler);
    event("History trace installed");publish();
}
}

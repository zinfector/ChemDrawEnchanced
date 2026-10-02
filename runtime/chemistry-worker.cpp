#include "chemistry-worker.hpp"
#include "placement-runtime.hpp"
#include <intrin.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace cd {
namespace {
// Pointer-free records. Native ccAtom label/query subclasses and ccMolecule
// properties are deliberately excluded; they keep the original solver.
using AtomRecord=std::array<std::byte,0x40>;
using BondRecord=std::array<std::byte,0x18>;
struct Snapshot {
    std::vector<AtomRecord> atoms;
    std::vector<BondRecord> bonds;
    uint32_t moleculeFlags{};
    uint32_t flags{};
    std::array<std::byte,4> options{};
    uint8_t structureOption{};
    bool operator==(const Snapshot&) const = default;
    size_t bytes() const { return atoms.size()*sizeof(AtomRecord)+bonds.size()*sizeof(BondRecord); }
};
struct Engine {
    HMODULE module{};
    Obj (*moleculeCtor)(Obj){};
    void (*moleculeDtor)(Obj){};
    Obj (*atomCtor)(Obj){};
    Obj (*bondCtor)(Obj){};
    void (*atomDtor)(Obj){};
    void (*bondDtor)(Obj){};
    Obj (*atomClone)(Obj){};
    Obj (*bondClone)(Obj){};
    void (*atomSize)(Obj,int){};
    void (*bondSize)(Obj,int){};
    void (*setAtom)(Obj,int,Obj){};
    void (*setBond)(Obj,int,Obj){};
    Obj (*cipCtor)(Obj,Obj,int,bool,bool,bool){};
    void (*cipDtor)(Obj){};
    int (*atomStereo)(Obj,int,bool){};
    int (*bondStereo)(Obj,int,bool){};
};
static Engine original;
static unsigned workerCount{};
static Obj nativeAtomVtable{},nativeBondVtable{};
static uint64_t preflightCount{},groupCount{},missCount{};
static uint64_t fastCount{},componentReuseCount{};
static std::array<uint64_t,5> missReasons{}; // unsupported, absent, pending, failed, token allocation
static std::array<uint64_t,6> rejectionCounts{};
static std::array<uint64_t,7> metadataRejectionCounts{};
using Cast=Obj(*)(Obj,long,Obj,Obj,int);
static Cast dynamicCast{};
template<class T> void resolve(HMODULE m,const char* name,T& f) {
    f=reinterpret_cast<T>(GetProcAddress(m,name));
    if(!f) throw std::runtime_error("Owned chemistry engine export");
}
Engine engine(HMODULE m) {
    Engine e{};e.module=m;
    resolve(m,"??0ccMolecule@@QEAA@XZ",e.moleculeCtor);
    resolve(m,"??1ccMolecule@@UEAA@XZ",e.moleculeDtor);
    resolve(m,"??0ccAtom@@IEAA@XZ",e.atomCtor);
    resolve(m,"??0ccBond@@IEAA@XZ",e.bondCtor);
    resolve(m,"??1ccAtom@@MEAA@XZ",e.atomDtor);
    resolve(m,"??1ccBond@@MEAA@XZ",e.bondDtor);
    resolve(m,"?Clone@ccAtom@@UEBAPEAV1@XZ",e.atomClone);
    resolve(m,"?Clone@ccBond@@UEBAPEAV1@XZ",e.bondClone);
    resolve(m,"?SetAtomListSize@ccMolecule@@QEAAXH@Z",e.atomSize);
    resolve(m,"?SetBondListSize@ccMolecule@@QEAAXH@Z",e.bondSize);
    resolve(m,"?SetAtom@ccMolecule@@QEAAXHPEAVccAtom@@@Z",e.setAtom);
    resolve(m,"?SetBond@ccMolecule@@QEAAXHPEAVccBond@@@Z",e.setBond);
    resolve(m,"??0ccStereoCIP@@QEAA@AEBVccMolecule@@W4ccCIPCalculateUnspecifiedType@0@_N22@Z",e.cipCtor);
    resolve(m,"??1ccStereoCIP@@QEAA@XZ",e.cipDtor);
    resolve(m,"?GetAtomStereo@ccStereoCIP@@QEBA?AW4ccCIPAtomType@@H_N@Z",e.atomStereo);
    resolve(m,"?GetBondStereo@ccStereoCIP@@QEBA?AW4ccCIPBondType@@H_N@Z",e.bondStereo);
    return e;
}
bool capture(Obj molecule,Snapshot& s) {
    auto reject=[](size_t reason) { ++rejectionCounts[reason];return false; };
    if(!molecule) return reject(0);
    // +0x30 is ccMolFlags, copied below. +0x50 is a modification counter:
    // native copy construction increments it even for a plain molecule. It
    // neither disqualifies a graph nor belongs in its chemical content key.
    // Keep unsupported owned metadata out of the pointer-free worker record.
    const bool metadata[]={at<size_t>(molecule,0x20)!=0,
        at<Obj>(molecule,0x38)!=nullptr,at<Obj>(molecule,0x40)!=nullptr,
        at<Obj>(molecule,0x48)!=nullptr,
        at<Obj>(molecule,0x90)!=at<Obj>(molecule,0x98),
        at<Obj>(molecule,0xa8)!=at<Obj>(molecule,0xb0),at<Obj>(molecule,0xd0)!=nullptr};
    bool unsupported=false;
    for(size_t i=0;i<std::size(metadata);++i) if(metadata[i]) {
        ++metadataRejectionCounts[i];unsupported=true;
    }
    if(unsupported) return reject(0);
    const int na=vf<int(*)(Obj)>(molecule,0x128)(molecule);
    const int nb=vf<int(*)(Obj)>(molecule,0x130)(molecule);
    if(na<0||nb<0||na>65536||nb>131072) return reject(1);
    s.atoms.resize(size_t(na));s.bonds.resize(size_t(nb));
    s.moleculeFlags=at<uint32_t>(molecule,0x30);
    s.flags=at<uint32_t>(molecule,0xd8);memcpy(s.options.data(),static_cast<std::byte*>(molecule)+0xc8,4);
    s.structureOption=at<uint8_t>(molecule,0x110);
    for(int i=0;i<na;++i) {
        const Obj a=vf<Obj(*)(Obj,int)>(molecule,0x188)(molecule,i);
        if(!a||at<Obj>(a,0)!=nativeAtomVtable||at<Obj>(a,0x50)||at<Obj>(a,0x58)) return reject(2);
        const int type=at<int>(a,0x10);
        if(type<=0||type>118) return reject(3);
        memcpy(s.atoms[size_t(i)].data(),static_cast<std::byte*>(a)+0x10,0x40);
        // ccAtom's numeric fields leave alignment padding at +0x14/+0x4c.
        // Allocator bytes must never create a false native result-cache miss.
        memset(s.atoms[size_t(i)].data()+4,0,4);
        memset(s.atoms[size_t(i)].data()+0x3c,0,4);
        for(size_t c=0x18;c<=0x28;c+=8) if(!std::isfinite(at<double>(a,c))) return reject(4);
    }
    for(int i=0;i<nb;++i) {
        const Obj b=vf<Obj(*)(Obj,int)>(molecule,0x198)(molecule,i);
        if(!b||at<Obj>(b,0)!=nativeBondVtable) return reject(5);
        const int a=at<int>(b,0x10),end=at<int>(b,0x14);
        if(a<0||end<0||a>=na||end>=na) return reject(5);
        memcpy(s.bonds[size_t(i)].data(),static_cast<std::byte*>(b)+0x10,0x18);
    }
    return true;
}
struct ExtendedMolecule {
    alignas(16) std::array<std::byte,0x190> storage{};
    bool built{};
    explicit ExtendedMolecule(Obj group) { fn<Obj(*)(Obj,Obj)>(0x4d40a0)(storage.data(),group);built=true; }
    ~ExtendedMolecule() {
        if(!built) return;
        for(size_t offset:{size_t(0x178),size_t(0x168),size_t(0x158),size_t(0x148)}) {
            Obj tree=storage.data()+offset;fn<void(*)(Obj,Obj)>(0x033340)(tree,tree);
        }
        fn<void(*)(Obj)>(0x1899c0)(storage.data()+0x130);
        fn<void(*)(Obj)>(0x1898d0)(storage.data()+0x118);
        original.moleculeDtor(storage.data());
    }
};
struct Job {
    Snapshot snapshot;
    std::vector<int> atoms,bonds;
    std::atomic<unsigned> state{}; // queued, running, ready, failed
    std::atomic<bool> cancelled{};
    std::atomic<HWND> wakeWindow{}; // Notification handle only; no document pointers.
    uint64_t used{};
    struct Part {
        std::shared_ptr<Job> job;
        std::vector<size_t> atoms,bonds; // component-local -> native group indices
    };
    std::vector<Part> parts; // UI-only aggregation; never enqueued on a worker
};
static std::mutex queueMutex;
static std::condition_variable queueEvent;
static std::deque<std::shared_ptr<Job>> queue;
static std::vector<std::shared_ptr<Job>> cache; // UI-owned, bounded
static uint64_t cacheClock{};
struct Batch { std::vector<std::shared_ptr<Job>> jobs; };
static std::unordered_map<Obj,Batch> batches; // native page keys never reach worker
static thread_local std::unordered_map<Obj,std::shared_ptr<Job>> consumers;
static bool available{};
static std::atomic<uint64_t> submittedCount{},hitCount{},failedCount{},cancelledCount{},workerTicks{},workerMaximum{};
static uint64_t nativeCalls{},nativeTicks{},nativeMaximum{},nativeMaximumCaller{};
uint64_t counter() noexcept { LARGE_INTEGER value{};QueryPerformanceCounter(&value);return uint64_t(value.QuadPart); }
struct NativeCallTimer {
    uint64_t started{};uintptr_t caller{};
    explicit NativeCallTimer(uintptr_t address):started(onUI()?counter():0),caller(address) {}
    ~NativeCallTimer() {
        if(!started) return;
        const auto elapsed=counter()-started;++nativeCalls;nativeTicks+=elapsed;
        if(elapsed>nativeMaximum) {
            nativeMaximum=elapsed;
            nativeMaximumCaller=caller>=base&&caller<base+0xf70000?caller-base:caller;
        }
    }
};
template<class T,size_t N> T record(const std::array<std::byte,N>& r,size_t offset) {
    T value{};memcpy(&value,r.data()+offset,sizeof(value));return value;
}
bool simpleHydrocarbon(const Snapshot& s) {
    // Narrow O(V+E) proof: neutral, unlabelled C/H, normal valence, only
    // undecorated single bonds, at most two explicit neighbours per carbon.
    // Every carbon has at least two identical hydrogen ligands. Neither atom
    // chirality nor double-bond/axial chirality is possible in these paths or
    // unsubstituted cycles. Native DetermineParities uses descriptor 1 here;
    // descriptor 0 means unresolved and must not be substituted for it.
    if(s.moleculeFlags||s.structureOption||
        std::any_of(s.options.begin(),s.options.end(),[](auto b){return b!=std::byte{};})) return false;
    for(const auto& a:s.atoms) {
        const int element=record<int>(a,0);
        if(element!=6&&element!=1) return false;
        for(size_t offset=0x20;offset<=0x34;offset+=4) if(record<int>(a,offset)) return false;
        if(record<uint32_t>(a,0x38)!=0x10) return false;
    }
    std::vector<std::array<int,2>> neighbours(s.atoms.size(),std::array<int,2>{-1,-1});
    for(const auto& b:s.bonds) {
        if(record<int>(b,8)!=1||record<int>(b,12)||record<int>(b,16)||record<uint32_t>(b,20)!=0x10) return false;
        const int a=record<int>(b,0),end=record<int>(b,4);
        if(a==end) return false;
        for(const auto pair:{std::pair{a,end},std::pair{end,a}}) {
            auto& n=neighbours[size_t(pair.first)];
            if(n[0]==pair.second||n[1]==pair.second||n[1]!=-1) return false;
            if(n[0]==-1) n[0]=pair.second;
            else {
                if(record<int>(s.atoms[size_t(pair.first)],0)==1) return false;
                n[1]=pair.second;
            }
        }
    }
    return true;
}
void calculate(Job& job,const Engine& isolated) {
    const auto& s=job.snapshot;
    alignas(16) std::array<std::byte,0x118> molecule{};
    isolated.moleculeCtor(molecule.data());
    struct MolGuard { const Engine& e;Obj p;~MolGuard(){ e.moleculeDtor(p); } } guard{isolated,molecule.data()};
    isolated.atomSize(molecule.data(),int(s.atoms.size()));
    isolated.bondSize(molecule.data(),int(s.bonds.size()));
    for(size_t i=0;i<s.atoms.size();++i) {
        if(job.cancelled.load()) return;
        alignas(16) std::array<std::byte,0x60> atom{};
        isolated.atomCtor(atom.data());memcpy(atom.data()+0x10,s.atoms[i].data(),0x40);
        const Obj copy=isolated.atomClone(atom.data());isolated.atomDtor(atom.data());
        if(!copy) throw std::bad_alloc();
        isolated.setAtom(molecule.data(),int(i),copy);
    }
    for(size_t i=0;i<s.bonds.size();++i) {
        if(job.cancelled.load()) return;
        alignas(16) std::array<std::byte,0x28> bond{};
        isolated.bondCtor(bond.data());memcpy(bond.data()+0x10,s.bonds[i].data(),0x18);
        const Obj copy=isolated.bondClone(bond.data());isolated.bondDtor(bond.data());
        if(!copy) throw std::bad_alloc();
        isolated.setBond(molecule.data(),int(i),copy);
    }
    // List construction can call Modified(), masking flags and clearing the
    // scalar options. Restore the complete captured inputs only afterwards,
    // matching native ccMolecule assignment's order. No native cache pointers
    // or revision counters are copied into the private module.
    at<uint32_t>(molecule.data(),0x30)=s.moleculeFlags;
    at<uint32_t>(molecule.data(),0xd8)=s.flags;
    memcpy(molecule.data()+0xc8,s.options.data(),4);
    at<uint8_t>(molecule.data(),0x110)=s.structureOption;
    alignas(16) std::array<std::byte,0x180> stereo{};
    isolated.cipCtor(stereo.data(),molecule.data(),1,false,false,true);
    struct CipGuard { const Engine& e;Obj p;~CipGuard(){e.cipDtor(p);} } cipGuard{isolated,stereo.data()};
    job.atoms.resize(s.atoms.size());job.bonds.resize(s.bonds.size());
    for(size_t i=0;i<job.atoms.size();++i) {
        if(job.cancelled.load()) return;
        job.atoms[i]=isolated.atomStereo(stereo.data(),int(i),true);
    }
    for(size_t i=0;i<job.bonds.size();++i) {
        if(job.cancelled.load()) return;
        job.bonds[i]=isolated.bondStereo(stereo.data(),int(i),true);
    }
}
void run(Engine isolated) noexcept {
    // Each thread owns a separately loaded module, including native CIP rule
    // globals. Never share a ccStereoCIP instance or a live document pointer.
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    for(;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock(queueMutex);queueEvent.wait(lock,[]{return !queue.empty();});
            job=std::move(queue.front());queue.pop_front();
        }
        if(job->cancelled.load()) { job->state.store(3,std::memory_order_release);continue; }
        job->state.store(1,std::memory_order_release);
        const auto started=counter();
        try { calculate(*job,isolated);job->state.store(job->cancelled.load()?3:2,std::memory_order_release); }
        catch(...) { ++failedCount;job->state.store(3,std::memory_order_release); }
        const auto elapsed=counter()-started;workerTicks.fetch_add(elapsed);
        auto maximum=workerMaximum.load();while(maximum<elapsed&&!workerMaximum.compare_exchange_weak(maximum,elapsed)) {}
        // Wake normal UI dispatch exactly when a result becomes available.
        // The worker never invokes native idle or accesses a document.
        if(const HWND w=job->wakeWindow.load()) PostMessageW(w,WM_NULL,0,0);
    }
}
unsigned readyState(Job& job) {
    const auto state=job.state.load(std::memory_order_acquire);
    if(state>=2||job.parts.empty()) return job.cancelled.load()?3:state;
    for(const auto& part:job.parts) {
        const auto child=part.job->state.load(std::memory_order_acquire);
        if(part.job->cancelled.load()||child==3) { job.state.store(3,std::memory_order_release);return 3; }
        if(child<2) return 0;
    }
    // Join only published immutable results. The mapping keeps native atom and
    // bond indices intact even when components finish in a different order.
    job.atoms.resize(job.snapshot.atoms.size());job.bonds.resize(job.snapshot.bonds.size());
    for(const auto& part:job.parts) {
        for(size_t i=0;i<part.atoms.size();++i) job.atoms[part.atoms[i]]=part.job->atoms[i];
        for(size_t i=0;i<part.bonds.size();++i) job.bonds[part.bonds[i]]=part.job->bonds[i];
    }
    job.state.store(2,std::memory_order_release);return 2;
}
std::shared_ptr<Job> find(const Snapshot& s) {
    for(auto& j:cache) if(!j->cancelled.load()&&j->state.load(std::memory_order_acquire)!=3&&j->snapshot==s) {
        j->used=++cacheClock;return j;
    }
    return {};
}
std::shared_ptr<Job> submitSingle(Snapshot s,bool component=false) {
    if(auto j=find(s)) { if(component) ++componentReuseCount;return j; }
    auto j=std::make_shared<Job>();j->snapshot=std::move(s);j->used=++cacheClock;
    if(simpleHydrocarbon(j->snapshot)) {
        j->atoms.assign(j->snapshot.atoms.size(),1);j->bonds.assign(j->snapshot.bonds.size(),1);
        j->state.store(2,std::memory_order_release);++fastCount;
    } else {
        std::lock_guard lock(queueMutex);
        // Prefer the latest edit. Retain running results for exact reuse/undo,
        // but bound queued abandoned requests independently of native queues.
        while(queue.size()>=128) {
            auto abandoned=std::find_if(queue.rbegin(),queue.rend(),[](const auto& old){return old.use_count()<=2;});
            if(abandoned==queue.rend()) break; // active page/aggregate still needs these jobs
            (*abandoned)->cancelled.store(true);++cancelledCount;
            queue.erase(std::next(abandoned).base());
        }
        queue.push_front(j);++submittedCount;
    }
    cache.push_back(j);queueEvent.notify_one();return j;
}
struct ComponentInput { Snapshot snapshot;std::vector<size_t> atoms,bonds; };
std::vector<ComponentInput> components(const Snapshot& s) {
    std::vector<size_t> parents(s.atoms.size());
    for(size_t i=0;i<parents.size();++i) parents[i]=i;
    auto root=[&](size_t i) {
        while(parents[i]!=i) { parents[i]=parents[parents[i]];i=parents[i]; }
        return i;
    };
    std::vector<uint8_t> ranks(parents.size());
    for(const auto& b:s.bonds) {
        size_t a=root(size_t(record<int>(b,0))),end=root(size_t(record<int>(b,4)));
        if(a==end) continue;
        if(ranks[a]<ranks[end]) std::swap(a,end);
        parents[end]=a;if(ranks[a]==ranks[end]) ++ranks[a];
    }
    std::unordered_map<size_t,size_t> indices;
    for(size_t i=0;i<parents.size();++i) {
        const size_t r=root(i);
        if(!indices.contains(r)) indices.emplace(r,indices.size());
    }
    // Native metadata/query subclasses have already been excluded. Only real
    // disconnected connectivity is independent; never slice a connected
    // molecule by distance, which could change remote CIP priorities/rings.
    if(indices.size()<2||indices.size()>64) return {};
    std::vector<ComponentInput> result(indices.size());
    for(auto& c:result) {
        c.snapshot.moleculeFlags=s.moleculeFlags;c.snapshot.flags=s.flags;
        c.snapshot.options=s.options;c.snapshot.structureOption=s.structureOption;
    }
    std::vector<size_t> local(parents.size());
    for(size_t i=0;i<s.atoms.size();++i) {
        auto& c=result[indices.at(root(i))];local[i]=c.atoms.size();
        c.atoms.push_back(i);c.snapshot.atoms.push_back(s.atoms[i]);
    }
    for(size_t i=0;i<s.bonds.size();++i) {
        const int a=record<int>(s.bonds[i],0),end=record<int>(s.bonds[i],4);
        auto& c=result[indices.at(root(size_t(a)))];auto b=s.bonds[i];
        const int mappedA=int(local[size_t(a)]),mappedEnd=int(local[size_t(end)]);
        memcpy(b.data(),&mappedA,4);memcpy(b.data()+4,&mappedEnd,4);
        c.bonds.push_back(i);c.snapshot.bonds.push_back(b);
    }
    return result;
}
std::shared_ptr<Job> submit(Snapshot s) {
    if(auto j=find(s)) return j;
    auto parts=components(s);
    if(parts.empty()) { auto j=submitSingle(std::move(s));trimChemistryWorker();return j; }
    auto j=std::make_shared<Job>();j->snapshot=std::move(s);j->used=++cacheClock;
    for(auto& c:parts) {
        auto child=submitSingle(std::move(c.snapshot),true);
        j->parts.push_back({std::move(child),std::move(c.atoms),std::move(c.bonds)});
    }
    readyState(*j);cache.push_back(j);trimChemistryWorker();return j;
}
Obj construct(Obj self,Obj molecule,int mode,bool a,bool b,bool c) {
    const uintptr_t caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-base;
    if(available&&onUI()&&caller>=0x4d3a00&&caller<0x4d3b80&&mode==1&&!a&&!b&&c) {
        size_t reason=0;
        try {
            Snapshot s;
            if(capture(molecule,s)) {
                auto job=find(s);
                // The same proven non-stereogenic screen also covers explicit
                // native callers that have no asynchronous preflight batch.
                if(!job&&simpleHydrocarbon(s)) job=submitSingle(std::move(s));
                const unsigned state=job?readyState(*job):3;
                reason=!job?1:state<2?2:3;
                if(job&&state==2) {
                    // Only this wrapper's verified getter/destructor consumers
                    // use tokens. Other native CIP callers retain native objects.
                    reason=4;
                    const auto inserted=consumers.emplace(self,std::move(job));
                    if(inserted.second) { ++hitCount;memset(self,0,0x180);at<Obj>(self,0)=molecule;return self; }
                }
            }
        } catch(const std::bad_alloc&) { reason=4; }
        ++missReasons[reason];
        ++missCount;
    }
    NativeCallTimer timer(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return original.cipCtor(self,molecule,mode,a,b,c);
}
void destroy(Obj self) {
    if(onUI()) if(auto i=consumers.find(self);i!=consumers.end()) { consumers.erase(i);return; }
    original.cipDtor(self);
}
int atomStereo(Obj self,int n,bool complete) {
    if(onUI()) if(auto i=consumers.find(self);i!=consumers.end()) {
        const auto& values=i->second->atoms;return n>=0&&size_t(n)<values.size()?values[size_t(n)]:1;
    }
    NativeCallTimer timer(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return original.atomStereo(self,n,complete);
}
int bondStereo(Obj self,int n,bool complete) {
    if(onUI()) if(auto i=consumers.find(self);i!=consumers.end()) {
        const auto& values=i->second->bonds;return n>=0&&size_t(n)<values.size()?values[size_t(n)]:1;
    }
    NativeCallTimer timer(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    return original.bondStereo(self,n,complete);
}
template<class F> void engineHook(F replacement,F& originalFunction) {
    const auto status=MH_CreateHook(reinterpret_cast<void*>(originalFunction),reinterpret_cast<void*>(replacement),reinterpret_cast<void**>(&originalFunction));
    if(status!=MH_OK) throw std::runtime_error("Owned chemistry consumer hook");
}
struct Node { Node* left;Node* parent;Node* right;uint8_t color,nil;uint8_t padding[6];Obj object; };
Node* next(Node* n) {
    if(!n->right->nil) { n=n->right;while(!n->left->nil) n=n->left;return n; }
    Node* p=n->parent;while(!p->nil&&n==p->right) { n=p;p=p->parent; }return p;
}
Obj asAtom(Obj object) {
    return object?dynamicCast(object,0,reinterpret_cast<Obj>(base+0xb46358),reinterpret_cast<Obj>(base+0xb46378),0):nullptr;
}
}
bool preparePlacementChemistry(Obj page) {
    if(!available||!onUI()||!page||!at<size_t>(page,0xd58)) return true;
    PlacementTimer timer(PlacementCost::Preflight);
    ++preflightCount;
    try {
        if(auto i=batches.find(page);i!=batches.end()) {
            for(const auto& j:i->second.jobs) if(readyState(*j)<2) return false;
            return true;
        }
        Obj selected{},atom{};
        const auto head=at<Node*>(page,0xd50);
        for(auto n=head->left;n!=head;n=next(n)) {
            const Obj object=n->object;
            if(!object||at<int>(object,0x20)<=0||at<int>(object,0x24)<=0) continue;
            if(!selected||at<int>(object,0x20)<at<int>(selected,0x20)) selected=object;
            if(asAtom(object)&&(!atom||at<int>(object,0x20)<at<int>(atom,0x20))) atom=object;
        }
        if(atom) selected=atom;
        if(!selected) return true;
        alignas(16) std::array<std::byte,16> set{};
        fn<Obj(*)(Obj,Obj,Obj,bool)>(0x3409a0)(set.data(),selected,nullptr,false);
        struct SetGuard { Obj p;~SetGuard(){fn<void(*)(Obj,Obj)>(0x023670)(p,p);} } guard{set.data()};
        std::vector<Obj> groups;
        const auto setHead=at<Node*>(set.data(),0);
        for(auto n=setHead->left;n!=setHead;n=next(n)) if(asAtom(n->object)) {
            const Obj group=at<Obj>(n->object,0x28);
            if(group&&std::find(groups.begin(),groups.end(),group)==groups.end()) groups.push_back(group);
        }
        if(groups.size()>16) return true;
        groupCount+=groups.size();
        const Obj doc=at<Obj>(page,8),port=doc?at<Obj>(doc,0x258):nullptr;
        const HWND window=port?fn<HWND(*)(Obj)>(0x624a70)(port):nullptr;
        Batch batch;
        for(const Obj group:groups) {
            ExtendedMolecule molecule(group);Snapshot s;
            if(capture(molecule.storage.data(),s)) {
                auto job=submit(std::move(s));job->wakeWindow.store(window);
                for(const auto& part:job->parts) part.job->wakeWindow.store(window);
                batch.jobs.push_back(std::move(job));
            }
        }
        bool ready=true;
        for(const auto& j:batch.jobs) if(readyState(*j)<2) ready=false;
        batches.insert_or_assign(page,std::move(batch));return ready;
    } catch(const std::bad_alloc&) { batches.erase(page);return true; }
}
bool chemistryWorkerPending(Obj page) noexcept {
    if(!onUI()) return false;
    try {
        if(auto i=batches.find(page);i!=batches.end())
            for(const auto& j:i->second.jobs) if(readyState(*j)<2) return true;
    } catch(const std::bad_alloc&) {}
    return false;
}
void forgetChemistryWorkerPage(Obj page) noexcept {
    if(!onUI()) return;
    // Invalidating a page association is not invalidating immutable chemistry.
    // Cancelling here discarded reusable running work and made a subsequent
    // nested native wrapper miss its result. Keep exact results for other
    // fragments/undo; newest-first dispatch and cache/queue limits retire old work.
    batches.erase(page);
}
void trimChemistryWorker() noexcept {
    if(!onUI()) return;
    size_t bytes{};for(const auto& j:cache) bytes+=j->snapshot.bytes()+j->snapshot.atoms.size()*4+j->snapshot.bonds.size()*4;
    while(cache.size()>128||bytes>16*1024*1024) {
        auto oldest=std::min_element(cache.begin(),cache.end(),[](const auto& a,const auto& b){return a->used<b->used;});
        if(oldest==cache.end()) break;
        auto& j=*oldest;bytes-=j->snapshot.bytes()+j->snapshot.atoms.size()*4+j->snapshot.bonds.size()*4;
        // Dropping a cache key must not cancel a result still required by an
        // active composite request or a native consumer token.
        cache.erase(oldest);
    }
}
bool chemistryWorkerAvailable() noexcept { return available; }
ChemistryWorkerStats chemistryWorkerStats() noexcept {
    return {submittedCount.load(),hitCount.load(),failedCount.load(),cancelledCount.load(),workerTicks.load(),workerMaximum.load(),
        preflightCount,groupCount,missCount,rejectionCounts,metadataRejectionCounts,
        nativeCalls,nativeTicks,nativeMaximum,nativeMaximumCaller,
        workerCount,fastCount,componentReuseCount,missReasons};
}
void installChemistryWorker() {
    const HMODULE native=GetModuleHandleW(L"CoreChemistryCommon.dll");
    if(!native) return;
    original=engine(native);
    nativeAtomVtable=reinterpret_cast<Obj>(GetProcAddress(native,"??_7ccAtom@@6B@"));
    nativeBondVtable=reinterpret_cast<Obj>(GetProcAddress(native,"??_7ccBond@@6B@"));
    dynamicCast=reinterpret_cast<Cast>(GetProcAddress(GetModuleHandleW(L"vcruntime140.dll"),"__RTDynamicCast"));
    if(!nativeAtomVtable||!nativeBondVtable||!dynamicCast) return;
    wchar_t path[32768]{};const DWORD count=GetModuleFileNameW(module,path,DWORD(std::size(path)));
    if(!count||count>=std::size(path)) return;
    std::wstring filename(path);const auto slash=filename.find_last_of(L"\\/");
    if(slash==std::wstring::npos) return;
    filename.resize(slash+1);
    const unsigned hardware=std::thread::hardware_concurrency();
    const unsigned requested=std::min(3u,hardware>2?hardware-2:1u);
    std::vector<Engine> engines;
    for(unsigned i=0;i<requested;++i) {
        const auto privateName=filename+L"ChemDrawLatencyChemistry"+
            (i?std::to_wstring(i+1):std::wstring{})+L".dll";
        const HMODULE privateModule=LoadLibraryExW(privateName.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if(!privateModule||privateModule==native) continue;
        engines.push_back(engine(privateModule));
    }
    if(engines.empty()) return;
    engineHook(construct,original.cipCtor);engineHook(destroy,original.cipDtor);
    engineHook(atomStereo,original.atomStereo);engineHook(bondStereo,original.bondStereo);
    for(const auto& owned:engines) {
        try { std::thread(run,owned).detach();++workerCount; }
        catch(const std::exception&) { break; }
    }
    available=workerCount!=0;
}
}

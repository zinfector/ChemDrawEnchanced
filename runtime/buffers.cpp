#include "runtime.hpp"
#include "gpu-ui.hpp"
#include "gpu-scene.hpp"
#include "arrow-path.hpp"
#include "placement-runtime.hpp"
#include "smart-align.hpp"
#include <intrin.h>

namespace cd {
using Work=void(*)(Obj,bool);
using Copy=void(*)(Obj);
using Blit=void(*)(Obj,Obj,const RectI*,const RectI*,int,Obj);
using Track=short(*)(Obj);
using Draw=void(*)(Obj,Obj,Obj,Obj);
using Offscreen=void(*)(Obj,Obj);
using BondGraphic=void(*)(Obj,Obj);
static Work oldWork{};
static Copy oldCopy{};
static Blit oldBlit{};
static Track oldTrack{};
static Draw oldDraw{};
static Offscreen oldOffscreen{};
static BondGraphic oldBondGraphic{};
static void (*oldHideCursor)(){};
using LassoDraw=void(*)(Obj,Obj);
static LassoDraw oldLassoDraw{};
struct CachedSelectionFeedback {
    Obj page{};uint64_t epoch{};double units{};
    Gdiplus::PointF origin{};
    std::shared_ptr<const GpuScene> artwork;
};
static thread_local std::unordered_map<HWND,CachedSelectionFeedback> selectionArtwork;
struct SelectionFeedback {
    Obj tracker{},doc{};
    RectI bounds{};
    Gdiplus::PointF anchor{};
    std::shared_ptr<const GpuScene> artwork;
    Gdiplus::PointF artworkOffset{};
    unsigned cursorRaises{};
};
static thread_local SelectionFeedback selectionFeedback;
static bool selectionDragTracker(Obj tracker) {
    if(!tracker) return false;
    const auto callback=reinterpret_cast<uintptr_t>(vf<Copy>(tracker,0x38));
    if(callback==base+0x16dcd0) return true;
    // TrackingHandlerServiceUI/Win replaces the base vtable. Its OnMouseMove
    // entry is an import thunk, not the address of the native implementation.
    const auto ui=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ChemDrawUI.dll"));
    return ui&&callback==ui+0x379174;
}
static bool selectionDevicePoint(Obj page,Obj port,double x,double y,Gdiplus::PointF& point) {
    const Obj scale=page?at<Obj>(page,0x2a8):nullptr;
    if(!scale||!port) return false;
    point={float(fn<double(*)(Obj,double)>(0x3c6560)(scale,x)),
        float(fn<double(*)(Obj,double)>(0x3c6590)(scale,y))};
    auto* graphics=fn<Gdiplus::Graphics*(*)(Obj)>(0x623140)(port);
    return graphics&&graphics->TransformPoints(Gdiplus::CoordinateSpaceDevice,
        Gdiplus::CoordinateSpaceWorld,&point,1)==Gdiplus::Ok&&
        std::isfinite(point.X)&&std::isfinite(point.Y);
}
static bool selectionAnchor(Obj tracker,Obj doc,Gdiplus::PointF& point) {
    const auto& bounds=at<RectD>(tracker,0x218);
    if(!valid(bounds)) return false;
    return selectionDevicePoint(at<Obj>(tracker,8),at<Obj>(doc,0x258),bounds.l,bounds.t,point);
}
static void drawLasso(Obj page,Obj port) {
    const Obj doc=page?at<Obj>(page,8):nullptr;
    if(!onUI()||!doc||trackingDepth||gpuHoverRecording()||
        *reinterpret_cast<int*>(base+0xb6e610)!=0||!beginGpuCommandCapture(port)) {
        oldLassoDraw(page,port);return;
    }
    try {oldLassoDraw(page,port);}catch(...) {endGpuCommandCapture();throw;}
    auto artwork=endGpuCommandCapture();
    const Obj mainPort=at<Obj>(doc,0x258),scale=at<Obj>(page,0x2a8);
    const HWND window=mainPort?fn<HWND(*)(Obj)>(0x624a70)(mainPort):nullptr;
    Gdiplus::PointF origin;
    if(!window||!scale||!selectionDevicePoint(page,port,0,0,origin))return;
    if(!artwork) {selectionArtwork.erase(window);return;}
    try {
        // Keep at most one small native overlay per open document. No selection
        // enumeration or GPU readback is added to the mouse-down path.
        if(selectionArtwork.size()>=16&&!selectionArtwork.contains(window))selectionArtwork.clear();
        selectionArtwork.insert_or_assign(window,CachedSelectionFeedback{
            page,generation.load(std::memory_order_relaxed),at<double>(scale,8),origin,std::move(artwork)});
    } catch(const std::bad_alloc&) {}
}
static void drawLightweightSelection(Obj tracker,Obj page,Obj port) {
    // Native preparation has already computed these bounds. Draw just their
    // frame and four handles using the native highlighter style, without
    // DrawLasso's cache refresh, fragment walk, or per-object overlays.
    const auto bounds=at<RectD>(tracker,0x218);
    if(!valid(bounds))return;
    struct NativeHighlighter {
        alignas(16) std::byte data[160]{};
        NativeHighlighter(Obj page,Obj port) {fn<Obj(*)(Obj,Obj,Obj)>(0x3ed780)(data,page,port);}
        ~NativeHighlighter() {fn<void(*)(Obj)>(0x143210)(data);}
    } highlighter(page,port);
    for(const Point point:std::array<Point,4>{{{bounds.l,bounds.t,0},{bounds.r,bounds.t,0},
        {bounds.l,bounds.b,0},{bounds.r,bounds.b,0}}}) {
        RectD handle{};
        fn<RectD*(*)(Obj,RectD*,const Point*,double)>(0x447110)(highlighter.data,&handle,&point,0.0);
    }
    fn<void(*)(Obj,const RectD*)>(0x3ede60)(highlighter.data,&bounds);
}
static void hideCursor() {
    oldHideCursor();
    // Balance each native hide locally; the native release still performs its
    // matching show. Remove our compensating raises when this drag ends.
    if(onUI()&&selectionFeedback.tracker) {
        ShowCursor(TRUE);++selectionFeedback.cursorRaises;
    }
}
class SelectionFeedbackScope {
    SelectionFeedback previous;
public:
    SelectionFeedbackScope(Obj tracker,Obj doc,bool gpu,uint64_t epoch):previous(std::move(selectionFeedback)) {
        selectionFeedback={};
        // The native selected-object move tracker has its own preparation and
        // temporary geometry. Drawing and resize trackers keep their behavior.
        if(!gpu||!doc||!selectionDragTracker(tracker)) return;
        selectionFeedback.tracker=tracker;selectionFeedback.doc=doc;
        // PrepareDragLasso has already hidden the grabber before modal tracking.
        // Raise only this drag's display count, never replace the native icon.
        for(unsigned n=0;n<32;++n) {
            const int count=ShowCursor(TRUE);++selectionFeedback.cursorRaises;
            if(count>=0) break;
        }
        try {
            if(!selectionAnchor(tracker,doc,selectionFeedback.anchor)) return;
            const Obj port=at<Obj>(doc,0x258),page=at<Obj>(tracker,8);
            const HWND window=fn<HWND(*)(Obj)>(0x624a70)(port);
            Gdiplus::PointF origin;
            if(auto cached=selectionArtwork.find(window);cached!=selectionArtwork.end()&&
                cached->second.page==page&&cached->second.epoch==epoch&&
                cached->second.units==at<double>(at<Obj>(page,0x2a8),8)&&
                selectionDevicePoint(page,port,0,0,origin)) {
                selectionFeedback.artwork=cached->second.artwork;
                selectionFeedback.artworkOffset={origin.X-cached->second.origin.X,origin.Y-cached->second.origin.Y};
                return;
            }
            if(!beginGpuHover(window,port)) return;
            auto* graphics=fn<Gdiplus::Graphics*(*)(Obj)>(0x623140)(port);
            const auto saved=graphics->Save();
            // Capture the native box/handles once without the initial viewport
            // clipping them. Keep vector commands, including offscreen handles.
            graphics->ResetClip();
            try {drawLightweightSelection(tracker,page,port);}
            catch(...) { graphics->Restore(saved);endGpuHover();throw; }
            graphics->Restore(saved);
            selectionFeedback.artwork=endGpuHover(&selectionFeedback.bounds);
        } catch(...) {
            for(unsigned n=0;n<selectionFeedback.cursorRaises;++n) ShowCursor(FALSE);
            selectionFeedback=std::move(previous);throw;
        }
    }
    ~SelectionFeedbackScope() {
        for(unsigned n=0;n<selectionFeedback.cursorRaises;++n) ShowCursor(FALSE);
        selectionFeedback=std::move(previous);
    }
};
static std::shared_ptr<const GpuScene> withSelectionFeedback(std::shared_ptr<const GpuScene> scene) {
    const auto& feedback=selectionFeedback;
    if(!scene||!feedback.tracker||!feedback.artwork||feedback.artwork->commands.empty()) return scene;
    Gdiplus::PointF anchor;
    if(!selectionAnchor(feedback.tracker,feedback.doc,anchor)) return scene;
    const float dx=anchor.X-feedback.anchor.X+float(feedback.bounds.l)+feedback.artworkOffset.X;
    const float dy=anchor.Y-feedback.anchor.Y+float(feedback.bounds.t)+feedback.artworkOffset.Y;
    auto frame=makeGpuScene();frame->width=scene->width;frame->height=scene->height;
    frame->extent=scene->extent;frame->serial=scene->serial;frame->trackingFrame=true;
    // Replace this unpublished frame description instead of referencing a
    // second scene with the same cache serial, or extending the drag history.
    frame->base=scene->base.load(std::memory_order_acquire);frame->commands=scene->commands;
    for(auto command:feedback.artwork->commands) {
        if(!command.state) continue;
        auto state=std::make_shared<SceneState>(*command.state);
        state->transform.dx+=dx;state->transform.dy+=dy;
        // Captured primitive geometry survives its old dirty clip. The moving
        // feedback belongs to the current viewport, including newly visible
        // handles, rather than the source offscreen port's paint rectangle.
        state->clip={{0,0,float(frame->width),float(frame->height)}};
        state->copy=false;command.state=std::move(state);
        frame->commands.push_back(std::move(command));
    }
    return frame;
}
static thread_local uint64_t backgroundEpoch=1;
static thread_local Obj activeTracker{};
static thread_local Obj trackingDocument{};
static thread_local int phase{};
static thread_local Obj clippedPort{};
static thread_local Gdiplus::Graphics* clippedGraphics{};
static thread_local Gdiplus::GraphicsState savedGraphics{};
static thread_local RectI clipLimit{};
using SetClip=void(*)(Obj,const RectI*);
static SetClip oldSetClip{};
static void setClip(Obj port,const RectI* rect) {
    if(port==clippedPort&&rect) {
        RectI clipped=intersect(*rect,clipLimit);
        clipped.b=std::max(clipped.b,clipped.t);clipped.r=std::max(clipped.r,clipped.l);
        if(valid(clipped)) oldSetClip(port,&clipped);
        else clippedGraphics->SetClip(Gdiplus::Rect(0,0,0,0));
    } else oldSetClip(port,rect);
}
static void restoreClip() {
    if(clippedGraphics) clippedGraphics->Restore(savedGraphics);
    clippedGraphics=nullptr;clippedPort=nullptr;
}
struct BufferState {
    Obj source{},dest{},image{};
    RectI extent{},dirty{},origin{};
    uint64_t epoch{};
    bool ready{},hasDirty{};
    Obj screenSource{},screenDest{};
    RectI screenDirty{};
    ULONGLONG lastPresent{};
    uint64_t screenEpoch{};
    bool pending{};
};
static thread_local std::unordered_map<Obj,BufferState> buffers;
void holdTrackingPresentation() {
    if(onUI()&&trackingDocument&&gpuTrackingActive()) holdGpuDrawing(trackingDocument);
}
void releaseTrackingGhost() {
    if(onUI()&&trackingDocument) startGpuGhostRelease(trackingDocument);
}
void flushTrackingPresentation(bool force) {
    if(!onUI()) return;
    const auto now=GetTickCount64();
    for(auto& item:buffers) {
        auto& s=item.second;
        if(!s.pending) continue;
        if(s.screenEpoch!=backgroundEpoch) { s.pending=false;s.screenDirty={};continue; }
        if(force||now-s.lastPresent>=16) {
            s.pending=false;
            oldBlit(s.screenSource,s.screenDest,&s.screenDirty,&s.screenDirty,0,nullptr);
            s.screenDirty={};s.lastPresent=now;
        }
    }
}
DWORD trackingPresentationDelay() {
    if(!onUI()) return INFINITE;
    DWORD delay=INFINITE;const auto now=GetTickCount64();
    for(const auto& item:buffers) if(item.second.pending) {
        const auto elapsed=now-item.second.lastPresent;
        delay=std::min(delay,DWORD(elapsed>=16?0:16-elapsed));
    }
    return delay;
}

static void blit(Obj source,Obj dest,const RectI* src,const RectI* dst,int mode,Obj extra) {
    if (!onUI()||!activeTracker||!src||!dst) { oldBlit(source,dest,src,dst,mode,extra); return; }
    auto& state=buffers[activeTracker];
    if(gpuTrackingRecording()) {
        if(phase==2&&mode==0&&!extra&&trackingDocument&&
            dest==at<Obj>(trackingDocument,0x258)) {
            // Native CopyBuffer pads logical object bounds by the bond width.
            // Selection halos/handles extend farther than that rectangle.
            // Publish the complete native work image rather than combining a
            // clipped new halo with the old highlighted canvas underneath it.
            const Obj bitmap=at<Obj>(source,0x160);
            auto* image=bitmap?at<Gdiplus::GpImage*>(bitmap,8):nullptr;
            UINT width{},height{};
            struct Origin { int x{},y{},z{}; } origin;
            if(image&&Gdiplus::DllExports::GdipGetImageWidth(image,&width)==Gdiplus::Ok&&
                Gdiplus::DllExports::GdipGetImageHeight(image,&height)==Gdiplus::Ok&&
                width<=INT_MAX/4&&height<=INT_MAX/4) {
                vf<Origin*(*)(Obj,Origin*)>(source,0xc0)(source,&origin);
                RectI viewport{};
                vf<RectI*(*)(Obj,RectI*)>(trackingDocument,0x100)(trackingDocument,&viewport);
                const int dx=dst->l-src->l,dy=dst->t-src->t;
                const RectI available{origin.y,origin.x,origin.y+int(height),origin.x+int(width)};
                const RectI requested{viewport.t-dy,viewport.l-dx,viewport.b-dy,viewport.r-dx};
                const auto fullSource=intersect(available,requested);
                const RectI fullDestination{fullSource.t+dy,fullSource.l+dx,fullSource.b+dy,fullSource.r+dx};
                if(valid(fullSource)) {
                    auto* graphics=fn<Gdiplus::Graphics*(*)(Obj)>(0x623140)(dest);
                    if(graphics) {
                        struct Restore {
                            Gdiplus::Graphics* graphics;Gdiplus::GraphicsState saved;
                            ~Restore() {graphics->Restore(saved);}
                        } restore{graphics,graphics->Save()};
                        // A dirty-region clip left by the tracker would still
                        // crop the expanded publication. Scope the full native
                        // viewport clip to this copy, then restore its state.
                        graphics->SetClip(Gdiplus::Rect(viewport.l,viewport.t,
                            viewport.r-viewport.l,viewport.b-viewport.t));
                        oldBlit(source,dest,&fullSource,&fullDestination,mode,extra);return;
                    }
                }
            }
        }
        // Work surfaces are rebuilt from native static page buffers. Preserve
        // their original restoration/setup copies outside final publication.
        oldBlit(source,dest,src,dst,mode,extra);return;
    }
    if (phase==2) {
        state.dirty=*dst; state.hasDirty=valid(*dst);
        if(same(*src,*dst)&&mode==0&&!extra&&trackingDepth) {
            if(state.screenEpoch!=backgroundEpoch||state.screenSource!=source||state.screenDest!=dest)
                state.screenDirty={};
            state.screenSource=source;state.screenDest=dest;state.screenEpoch=backgroundEpoch;
            state.screenDirty=unite(state.screenDirty,*dst);state.pending=true;
            flushTrackingPresentation();return;
        }
        oldBlit(source,dest,src,dst,mode,extra); return;
    }
    if (phase!=1 || !same(*src,*dst) || mode!=0 || extra) {
        oldBlit(source,dest,src,dst,mode,extra); return;
    }
    Obj image=at<Obj>(source,0x160);
    RectI origin{at<int>(source,0xa8),at<int>(source,0xac),at<int>(dest,0xa8),at<int>(dest,0xac)};
    const bool incremental=state.ready && state.hasDirty && state.source==source &&
        state.dest==dest && state.image==image && same(state.extent,*src) &&
        same(state.origin,origin) && state.epoch==backgroundEpoch;
    state.source=source; state.dest=dest; state.image=image;
    state.extent=*src; state.origin=origin; state.epoch=backgroundEpoch; state.ready=true;
    if (incremental) {
        // The work bitmap retains the static page. Only the previous preview has
        // contaminated it. CopyBuffer's native padded rectangle covers that ink.
        RectI region=intersect(*src,state.dirty);
        // Also constrain repeated guide/overlay drawing in UseWorkBuffer. Simply
        // shrinking the copy would repeatedly alpha-blend unchanged guides.
        if(!clippedGraphics) {
            auto graphics=fn<Gdiplus::Graphics*(*)(Obj)>(0x623140)(dest);
            if(graphics) {
                clippedPort=dest;clippedGraphics=graphics;savedGraphics=graphics->Save();
                clipLimit=region;
                const int x=region.l-at<int>(dest,0xa8),y=region.t-at<int>(dest,0xac);
                graphics->SetClip(Gdiplus::Rect(x,y,std::max(0,region.r-region.l),
                    std::max(0,region.b-region.t)),Gdiplus::CombineModeIntersect);
            }
        }
        if(!clippedGraphics) oldBlit(source,dest,src,dst,mode,extra);
        else if (valid(region)) oldBlit(source,dest,&region,&region,mode,extra);
    } else oldBlit(source,dest,src,dst,mode,extra);
}
static void useWork(Obj tracker,bool flag) {
    if (!onUI()) { oldWork(tracker,flag); return; }
    flushPlacementDamage();flushTrackingPresentation();
    // Native UseWorkBuffer first calls its +0x58 preparation method, which
    // creates the work port lazily. A first-use null port is not drawable yet;
    // let native setup create it rather than calling GetGraphics on null.
    if(gpuTrackingRecording()) if(Obj port=at<Obj>(tracker,0x100)) resetGpuWork(port);
    Obj saved=activeTracker; int savedPhase=phase;
    activeTracker=tracker; phase=flag?0:1;
    try { oldWork(tracker,flag); } catch (...) { restoreClip();activeTracker=saved; phase=savedPhase; throw; }
    restoreClip();
    activeTracker=saved; phase=savedPhase;
}
static void copyWork(Obj tracker) {
    if (!onUI()) { oldCopy(tracker); return; }
    Obj saved=activeTracker; int savedPhase=phase;
    Obj doc=trackingDepth?vf<Obj(*)(Obj)>(tracker,0x80)(tracker):nullptr;
    if(gpuTrackingRecording()&&doc) resetGpuTrackingDestination(doc);
    activeTracker=tracker; phase=2;
    try { oldCopy(tracker); } catch (...) { activeTracker=saved; phase=savedPhase; throw; }
    activeTracker=saved; phase=savedPhase;
    if(doc&&gpuTrackingActive()) {
        updateGpuGhostGesture(doc);
        // OnMouseMove has now finished positioning its temporary endpoint.
        // Publish its circle separately, before the updated drawing scene.
        updateGpuPlacementEndpoint(tracker);
        auto scene=withSelectionFeedback(currentGpuTrackingScene());
          if(!scene||!publishGpuCanvasScene(doc,scene,smartAlignmentFeedback(tracker))) disableGpuCanvas(doc);
    }
}
static short performTrack(Obj tracker) {
    if (!onUI()) return oldTrack(tracker);
    PlacementTimer timer(PlacementCost::Tracking);
    Obj doc=vf<Obj(*)(Obj)>(tracker,0x80)(tracker);
    const auto selectionEpoch=generation.load(std::memory_order_relaxed);
    beginGpuDrawing(doc);
    buffers.erase(tracker);clearHighlights();bumpGeneration();++trackingDepth;
    Obj savedDocument=trackingDocument;trackingDocument=doc;
    const bool gpu=beginGpuTracking(doc,arrowPathTrackingObject(tracker));
    short result{};
    try {
        SelectionFeedbackScope feedback(tracker,doc,gpu,selectionEpoch);
        beginSmartAlignment(tracker,doc,gpu);
        result=oldTrack(tracker);
    }
    catch (...) {
        endSmartAlignment();
        if(gpu) { queueGpuDrawingCommit(doc);endGpuTracking(); }
        trackingDocument=savedDocument;--trackingDepth;buffers.erase(tracker);bumpGeneration();throw;
    }
    endSmartAlignment();
    flushTrackingPresentation(true);
    if(gpu) {
        queueGpuDrawingCommit(doc);endGpuTracking();
        if(doc)invalidateCanvas(fn<HWND(*)(Obj)>(0x624a70)(at<Obj>(doc,0x258)));
    }
    trackingDocument=savedDocument;
    --trackingDepth; buffers.erase(tracker); bumpGeneration(); return result;
}
static void drawOffscreen(Obj page,Obj selection) {
    flushPlacementDamage();
    ++backgroundEpoch;beginGpuBackground(page);
    try { oldOffscreen(page,selection); } catch(...) { endGpuBackground();throw; }
    endGpuBackground();
}
static void drawObjects(Obj page,Obj port,Obj selection,Obj options) {
    flushPlacementDamage();PlacementTimer timer(PlacementCost::Drawing);
    ++drawingDepth;
    try { oldDraw(page,port,selection,options); }
    catch (...) { --drawingDepth; throw; }
    --drawingDepth;
}
static void drawBondGraphic(Obj bond,Obj port) {
    if(!onUI()||(!gpuCanvasRecording()&&!gpuTrackingRecording()&&!gpuBackgroundRecording())) {
        oldBondGraphic(bond,port);return;
    }
    // DrawGraphic uses native outlines for temporary bonds, but an integer
    // centerline fast path for ordinary repaint. Keep the native outline path
    // across that transition; scope the flag to this one bond's drawing call.
    auto& outlines=*reinterpret_cast<bool*>(base+0xb6dd34);
    const bool saved=outlines;outlines=true;
    try { oldBondGraphic(bond,port); }
    catch(...) { outlines=saved;throw; }
    outlines=saved;
}

// The native renderer snapshots a red/black tree on every draw. Reuse its
// temporary 40-byte nodes without changing iteration, reentry or object lifetime.
// Only the root/snapshot-cloner call sites participate; persistent document trees
// and allocations elsewhere still use ChemDraw's allocator unchanged.
using Allocate=void*(*)(size_t);
using Deallocate=void(*)(void*);
static Allocate oldNew{};
static Deallocate oldDelete{};
static std::atomic<uintptr_t> poolMemory{};
static void* freeNodes{};
static SRWLOCK poolLock=SRWLOCK_INIT;
static unsigned blockCount{};
constexpr size_t blockBytes=65536, stride=48, maxBlocks=64;
constexpr size_t poolBytes=blockBytes*maxBlocks;
static void* acquireNode() {
    AcquireSRWLockExclusive(&poolLock);
    if (!freeNodes && blockCount<maxBlocks) {
        auto start=poolMemory.load(std::memory_order_relaxed);
        if(!start) {
            start=reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr,poolBytes,MEM_RESERVE,PAGE_READWRITE));
            if(start) poolMemory.store(start,std::memory_order_release);
        }
        if(start) {
            auto* memory=reinterpret_cast<std::byte*>(start+size_t(blockCount)*blockBytes);
            if(VirtualAlloc(memory,blockBytes,MEM_COMMIT,PAGE_READWRITE)) {
                ++blockCount;
                for(size_t i=0;i+stride<=blockBytes;i+=stride) {
                    *reinterpret_cast<void**>(memory+i)=freeNodes;freeNodes=memory+i;
                }
            }
        }
    }
    void* node=freeNodes;
    if (node) freeNodes=*static_cast<void**>(node);
    ReleaseSRWLockExclusive(&poolLock); return node;
}
static void* allocate(size_t bytes) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress())-base;
    if (drawingDepth && bytes==0x28 &&
        ((caller>=0x70da50 && caller<0x70e351)||(caller>=0x384a0 && caller<0x385b0))) {
        if (void* p=acquireNode()) return p;
    }
    return oldNew(bytes);
}
static void deallocate(void* p) {
    const auto ptr=reinterpret_cast<uintptr_t>(p);
    const auto start=poolMemory.load(std::memory_order_acquire);
    // The delete hook is global. Ordinary application memory must bypass the
    // pool without a lock or a walk over all rendering allocation blocks.
    if(!start||ptr<start||ptr-start>=poolBytes) { oldDelete(p);return; }
    const size_t offset=(ptr-start)%blockBytes;
    if(offset>=(blockBytes/stride)*stride||offset%stride!=0) { oldDelete(p);return; }
    AcquireSRWLockExclusive(&poolLock);
    *static_cast<void**>(p)=freeNodes;freeNodes=p;
    ReleaseSRWLockExclusive(&poolLock);
}
void installBuffers() {
    hook(0x6d0c90,useWork,oldWork); hook(0x6d02f0,copyWork,oldCopy);
    hook(0x607950,blit,oldBlit); hook(0x6d0640,performTrack,oldTrack);
    hook(0x70e360,drawOffscreen,oldOffscreen); hook(0x70da50,drawObjects,oldDraw);
    hook(0x2ea7e0,drawBondGraphic,oldBondGraphic);
    if (patchEnabled(3)) { hook(0x8469e4,allocate,oldNew); hook(0x12dbc0,deallocate,oldDelete); }
    hook(0x626c80,setClip,oldSetClip);
    hook(0x59fea0,hideCursor,oldHideCursor);
    hook(0x419560,drawLasso,oldLassoDraw);
}
}

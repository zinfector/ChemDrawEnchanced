#include "runtime.hpp"
#include "gpu-ui.hpp"
#include "curved-arrow.hpp"
#include "arrow-path.hpp"
#include "gpu-preview.hpp"
#include "placement-runtime.hpp"
#include "chemistry-worker.hpp"
#include "smart-align.hpp"
#include <intrin.h>

namespace cd {
using Graphics=Gdiplus::Graphics;
using Paint=uintptr_t(*)(Obj,Obj);
using Feedback=void(*)(Obj);
using AtomFeedback=void(*)(Obj,Obj);
using Hover=uintptr_t(*)(Obj);
using Zoom=void(*)(Obj,const Point*,double,bool);
using Magnify=bool(*)(Obj,const Point*);
using WndProc=LRESULT(WINAPI*)(HWND,UINT,WPARAM,LPARAM);
using Scrolling=void(*)(Obj,Obj,const int*,const int*);
using SetSize=void(*)(Obj,int,int);
using Idle=bool(*)(Obj);
using Contents=void(*)(Obj,Obj);
static Paint oldPaint{};
static Feedback oldHotKeyFeedback{};
static Feedback oldMouseHighlights{};
using ClipHighlight=void(*)(Obj,Obj,RectI*,bool);
static ClipHighlight oldClipHighlight{};
static AtomFeedback oldAtomHotKeyFeedback{};
static Hover oldHover{};
static Zoom oldZoom{};
static Magnify oldZoomIn{},oldZoomOut{};
static WndProc oldWndProc{};
static Scrolling oldScrolling{};
static SetSize oldSetSize{};
static Feedback oldScrollVisibility{};
static Idle oldIdle{};
static Idle oldOffscreenValid{};
static Contents oldContents{};
static Contents oldPaperBackground{};
static thread_local Obj paperBorderCaptureDoc{};
static thread_local Obj vectorContentsDoc{};
static thread_local unsigned scrollDepth{},sizeDepth{},workspaceDepth{},cameraCommitDepth{},historyPrepareDepth{};
static thread_local Obj scrollCenterDoc{};
static thread_local Point scrollCenter{};
struct CounterScope {
    unsigned& value;
    explicit CounterScope(unsigned& v):value(v) { ++value; }
    ~CounterScope() { --value; }
};
struct CenterCommit {
    Obj savedDoc;Point savedCenter;
    CenterCommit(Obj doc,Point center):savedDoc(scrollCenterDoc),savedCenter(scrollCenter) {
        scrollCenterDoc=doc;scrollCenter=center;
    }
    ~CenterCommit() { scrollCenterDoc=savedDoc;scrollCenter=savedCenter; }
};
static ULONG_PTR gdiplusToken{};
static thread_local unsigned paintDepth{},idleDepth{},fixedUiPaintDepth{};
static thread_local Obj historyPublishDoc{};
static thread_local bool historyPublished{};
static thread_local HWND dispatchWindow{};
static thread_local HWND updateWindow{};
static thread_local RectI updateBounds{};
static thread_local double wheelNotches{};
static thread_local POINT wheelCursor{};
static thread_local bool haveWheelCursor{};
constexpr UINT_PTR zoomTimer=0x43445A31;
constexpr UINT_PTR panTimer=0x43445031;
constexpr UINT_PTR drawingCommitTimer=0x43444340;
constexpr ULONGLONG frameMs=16, zoomSettleMs=80;

struct Surface {
    HDC dc{}; HBITMAP bitmap{}; HGDIOBJ previous{}; void* bits{}; int w{},h{};
    Surface()=default;
    Surface(const Surface&)=delete;
    Surface& operator=(const Surface&)=delete;
    ~Surface() { reset(); }
    void reset() {
        if(dc && previous) SelectObject(dc,previous);
        if(bitmap) DeleteObject(bitmap);
        if(dc) DeleteDC(dc);
        dc=nullptr; bitmap=nullptr; previous=nullptr; bits=nullptr; w=h=0;
    }
    bool size(int width,int height) {
        if(width==w&&height==h&&dc) return true;
        reset();
        if(width<=0||height<=0||uint64_t(width)*height>64000000) return false;
        BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
        info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
        dc=CreateCompatibleDC(nullptr);
        bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!dc||!bitmap) { reset(); return false; }
        previous=SelectObject(dc,bitmap); w=width; h=height;
        memset(bits,0,size_t(w)*size_t(h)*4); return true;
    }
    void clear(RectI rect) {
        rect=intersect(rect,{0,0,h,w});
        if(!valid(rect)) return;
        for(int y=rect.t;y<rect.b;++y)
            memset(static_cast<std::byte*>(bits)+(size_t(y)*w+rect.l)*4,0,size_t(rect.r-rect.l)*4);
    }
};
// Buffer the native page and feedback together. No child highlight window takes
// part in cursor dispatch, hit testing, or mouse capture.
struct PaintFrame {
    HWND owner{}; Surface pixels;
    std::unique_ptr<Gdiplus::Bitmap> image;
    std::unique_ptr<Graphics> graphics;
    float dpiX{},dpiY{};
    bool prepare(HWND w,Graphics* source) {
        owner=w;const auto rect=clientRect(w);
        if(pixels.w!=rect.r||pixels.h!=rect.b||dpiX!=source->GetDpiX()||dpiY!=source->GetDpiY()) {
            graphics.reset();image.reset();
        }
        if(!pixels.size(rect.r,rect.b)) return false;
        if(!image) {
            image=std::make_unique<Gdiplus::Bitmap>(pixels.w,pixels.h,pixels.w*4,
                PixelFormat32bppPARGB,static_cast<BYTE*>(pixels.bits));
            dpiX=source->GetDpiX();dpiY=source->GetDpiY();image->SetResolution(dpiX,dpiY);
            graphics.reset(Graphics::FromImage(image.get()));
        }
        return image->GetLastStatus()==Gdiplus::Ok&&graphics&&graphics->GetLastStatus()==Gdiplus::Ok;
    }
};
static std::unordered_map<HWND,std::unique_ptr<PaintFrame>> frames;
static std::unordered_map<HWND,std::unique_ptr<PaintFrame>> fixedUiFrames;
static std::unordered_map<HWND,std::unique_ptr<PaintFrame>> pageCaptureFrames;

struct NavigationFrame {
    Surface snapshot,presented;
    std::shared_ptr<GpuPreview> gpu;
    std::unique_ptr<Gdiplus::Bitmap> sourceImage,targetImage;
    std::unique_ptr<Graphics> graphics;
    PreviewPaper paper{};
    PreviewView initialView{};
    Point sourceOrigin{};double sourceUnits{};
    Gdiplus::SolidBrush workspaceBrush{Gdiplus::Color(255,221,221,221)};
    Gdiplus::SolidBrush paperBrush{Gdiplus::Color(255,255,255,255)};
    Gdiplus::Pen borderPen{Gdiplus::Color(255,0,0,0),1.0f};
    HBRUSH workspaceGdi{},paperGdi{};
    Gdiplus::RectF drawn{};
    int panX{},panY{};
    bool hasZoomFrame{},hasPanFrame{true};
    ~NavigationFrame() {
        if(workspaceGdi) DeleteObject(workspaceGdi);
        if(paperGdi) DeleteObject(paperGdi);
    }
    bool prepareCpu() {
        if(presented.dc) return true;
        if(!snapshot.dc||!presented.size(snapshot.w,snapshot.h)) return false;
        return BitBlt(presented.dc,0,0,snapshot.w,snapshot.h,snapshot.dc,0,0,SRCCOPY)!=FALSE;
    }
    bool prepareZoom(RectI pane) {
        if(graphics) return true;
        if(!prepareCpu()) return false;
        sourceImage=std::make_unique<Gdiplus::Bitmap>(snapshot.w,snapshot.h,snapshot.w*4,
            PixelFormat32bppRGB,static_cast<BYTE*>(snapshot.bits));
        targetImage=std::make_unique<Gdiplus::Bitmap>(presented.w,presented.h,presented.w*4,
            PixelFormat32bppRGB,static_cast<BYTE*>(presented.bits));
        if(sourceImage->GetLastStatus()!=Gdiplus::Ok||targetImage->GetLastStatus()!=Gdiplus::Ok) return false;
        graphics.reset(Graphics::FromImage(targetImage.get()));
        if(!graphics||graphics->GetLastStatus()!=Gdiplus::Ok) return false;
        graphics->SetPageUnit(Gdiplus::UnitPixel);
        graphics->SetClip(Gdiplus::Rect(pane.l,pane.t,pane.r-pane.l,pane.b-pane.t));
        graphics->SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
        graphics->SetInterpolationMode(Gdiplus::InterpolationModeBilinear);
        graphics->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        return true;
    }
};
struct ZoomState {
    Obj doc{};Point center{};bool refresh{},cpuStarted{};double initial{},target{};
    PreviewView shownView{},targetView{};RectI pane{};NavigationFrame preview;
    ULONGLONG lastInput{},lastFrame{}; uint64_t epoch{};
};
static std::unordered_map<HWND,std::unique_ptr<ZoomState>> zooms;
struct PanState {
    Obj doc{},horizontal{},vertical{}; RectI pane{}; NavigationFrame preview;
    double baseX{},baseY{},targetX{},targetY{},shownX{},shownY{};
    ULONGLONG lastInput{},lastFrame{}; uint64_t epoch{};
};
static std::unordered_map<HWND,std::unique_ptr<PanState>> pans;
static std::unordered_map<HWND,std::pair<double,double>> panRemainders;
static std::unordered_map<HWND,std::shared_ptr<GpuPreview>> gpuPreviews,gpuHandoffs;
static std::unordered_map<HWND,bool> gpuCanvasDisabled;
struct DrawingCommit {
    Obj doc{};bool posted{},awaitingIdle{},refreshing{},painted{};
    bool idleObserved{},idleCacheReady{};
    uint64_t revision{};
};
static std::unordered_map<HWND,DrawingCommit> drawingCommits;
struct PlacementHighlight {
    uint64_t source{},endpoint{};
    RectD sourceCircle{},endpointCircle{};
};
static std::unordered_map<HWND,PlacementHighlight> placementHighlights;
struct GhostGesture { POINT pressed{};bool releasing{}; };
static std::unordered_map<HWND,GhostGesture> ghostGestures;
static std::unordered_map<HWND,PreviewPaper> canvasPapers;
struct WorkspaceBars { Obj doc{};HWND horizontal{},vertical{}; };
static std::unordered_map<HWND,WorkspaceBars> workspaceBars;
struct UndoCameraRequest { Obj doc{},page{};RectD bounds{};bool posted{},reveal{},awaitingIdle{}; };
static std::unordered_map<HWND,UndoCameraRequest> undoCameras;
static UINT undoCameraMessage() {
    static const UINT message=RegisterWindowMessageW(L"ChemDrawLatency.UndoCamera.71");
    return message;
}
static void postUndoCamera(HWND w) {
    auto request=undoCameras.find(w);
    if(request!=undoCameras.end()&&!request->second.posted&&!request->second.awaitingIdle&&dispatchWindow!=w&&!trackingDepth)
        request->second.posted=PostMessageW(w,undoCameraMessage(),0,0)!=FALSE;
}
static void drawZoom(HWND,ZoomState&,HDC supplied=nullptr);
static void drawPan(HWND,PanState&,HDC supplied=nullptr);
static void finishPan(HWND);
static void finishZoom(HWND);
static void alignGpuToNative(Obj,NavigationFrame&,RectI);
static void refreshGhost(Obj);
static PreviewPaper canvasPaper(HWND,Obj,RectI,Graphics* inkGraphics=nullptr);
static std::shared_ptr<const GpuScene> captureWholePage(HWND,Obj,RectI,const PreviewPaper&);
static void clearGhost(HWND);
static void refreshHighlight(Obj);
static void refreshArrowHighlight(Obj);
static void clearHoverHighlight(HWND);
static bool mouseInPlacementCircle(Obj,HWND,const PlacementHighlight&,Point&);
static void clearFeedback(HWND,bool immediate=false);
static void finishPlacementHover(Obj);
static bool ghostPlacementActive(HWND);
static void cancelGhostGesture(HWND);
bool gpuOwnsPlacementHighlight(Obj atom) {
    // Explicit hover capture is the only place allowed to redraw an owned
    // source. Its transparent scene replaces the retained image atomically.
    if(!onUI()||!atom||gpuHoverRecording()) return false;
    Obj page=at<Obj>(atom,0x60),doc=page?at<Obj>(page,8):nullptr;
    Obj port=doc?at<Obj>(doc,0x258):nullptr;
    const HWND w=port?fn<HWND(*)(Obj)>(0x624a70)(port):nullptr;
    auto owned=placementHighlights.find(w);auto gpu=gpuPreviews.find(w);
    if(owned==placementHighlights.end()||gpu==gpuPreviews.end()||!gpu->second||!gpu->second->usable()) return false;
    const auto identity=at<uint64_t>(atom,0xb0)+1;
    RectD circle{};
    return owned->second.source==identity||owned->second.endpoint==identity||
        (gpuTrackingActive()&&vertexHitCircle(atom,circle));
}
bool gpuOwnsHoverHighlight(Obj object,Obj highlighter) {
    // Only native mouse hit boxes belong to the animated hover layer. Lasso,
    // persistent document highlights and other highlighter classes remain native.
    // Allow the original artwork while explicitly recording that layer.
    if(!onUI()||!object||!highlighter||gpuHoverRecording()||
        at<uintptr_t>(highlighter,0)!=base+0x8a1d68) return false;
    RectD circle{};
    if(at<uintptr_t>(object,0)!=base+0x8babc0&&!vertexHitCircle(object,circle)) return false;
    const Obj page=at<Obj>(object,0x60),doc=page?at<Obj>(page,8):nullptr;
    const Obj port=doc?at<Obj>(doc,0x258):nullptr;
    const HWND w=port?fn<HWND(*)(Obj)>(0x624a70)(port):nullptr;
    const auto gpu=gpuPreviews.find(w);
    const auto disabled=gpuCanvasDisabled.find(w);
    return (disabled==gpuCanvasDisabled.end()||!disabled->second)&&gpu!=gpuPreviews.end()&&gpu->second&&
        gpu->second->usable()&&(gpuCanvasRecording()||gpu->second->hasScene());
}
static HWND windowFor(Obj doc) {
    Obj port=doc?at<Obj>(doc,0x258):nullptr;
    return port?fn<HWND(*)(Obj)>(0x624a70)(port):nullptr;
}
static bool ghostPlacementActive(HWND w) {
    const auto gpu=gpuPreviews.find(w);
    return gpu!=gpuPreviews.end()&&gpu->second&&gpu->second->ghostPlacementActive();
}
static void cancelGhostGesture(HWND w) {
    ghostGestures.erase(w);
    if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second) gpu->second->cancelGhostPlacement();
}
void startGpuGhostRelease(Obj doc) {
    if(!onUI()||!doc) return;
    const auto gpu=gpuPreviews.find(windowFor(doc));
    if(gpu!=gpuPreviews.end()&&gpu->second) gpu->second->startGhostRelease();
}
void updateGpuGhostGesture(Obj doc) {
    if(!onUI()||!doc) return;
    const HWND w=windowFor(doc);const auto gesture=ghostGestures.find(w);
    if(gesture==ghostGestures.end()||gesture->second.releasing||!(GetAsyncKeyState(VK_LBUTTON)&0x8000)) return;
    POINT mouse{};if(!GetCursorPos(&mouse)) return;
    UINT dpi=GetDpiForWindow(w);if(!dpi) dpi=GetDpiForSystem();
    const double dx=double(mouse.x)-gesture->second.pressed.x,dy=double(mouse.y)-gesture->second.pressed.y;
    if(std::abs(dx)>std::max(1,GetSystemMetricsForDpi(SM_CXDRAG,dpi))||
        std::abs(dy)>std::max(1,GetSystemMetricsForDpi(SM_CYDRAG,dpi))) cancelGhostGesture(w);
}
static UINT drawingCommitMessage() {
    static const UINT message=RegisterWindowMessageW(L"ChemDrawLatency.DrawingCommitted.22");
    return message;
}
void holdGpuDrawing(Obj doc) {
    if(!onUI()||!doc) return;
    const HWND w=windowFor(doc);auto gpu=gpuPreviews.find(w);
    clearGhost(w);
    if(!placementHighlights.contains(w)) clearFeedback(w,true);
    if(gpu==gpuPreviews.end()||!gpu->second||!gpu->second->usable()) return;
    if(drawingCommits.emplace(w,DrawingCommit{doc,false,false,false,false,false,false,placementRevision(w)}).second) gpu->second->holdScenes(true);
}
void beginGpuDrawing(Obj doc) {
    if(!onUI()||!doc) return;
    const HWND w=windowFor(doc);
    clearGhost(w);
    if(!placementHighlights.contains(w)) clearFeedback(w,true);
    if(!drawingCommits.erase(w)) return;
    if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)
        gpu->second->holdScenes(false,false);
    // A new gesture may start before idle. Its own complete work-buffer copy
    // can replace the previous preview; do not expose the staged cleanup scene.
}
static void postDrawingCommit(HWND w) {
    auto commit=drawingCommits.find(w);
    if(commit!=drawingCommits.end()&&!commit->second.posted&&!commit->second.awaitingIdle)
        commit->second.posted=PostMessageW(w,drawingCommitMessage(),0,0)!=FALSE;
}
static bool drawingPageReady(Obj doc,bool more) {
    const Obj page=doc?mainPage(doc):nullptr;
    // DocumentBase::OnIdle reports work for every page. Only geometry and
    // chemistry belonging to this edit must finish before its image can appear.
    return page&&!at<unsigned char>(doc,0x40c)&&!at<size_t>(page,0xd58)&&
        (!more||!at<unsigned char>(page,0xd60)||fn<bool(*)(Obj)>(0x7160a0)(page));
}
static bool drawingGeometryReady(Obj doc) {
    // Only an initialized, valid native offscreen surface is publishable. A
    // chemistry queue may remain; unfinished geometry validation may not.
    return doc&&mainPage(doc)&&!at<unsigned char>(doc,0x40c)&&oldOffscreenValid(doc);
}
void queueGpuDrawingCommit(Obj doc) {
    holdGpuDrawing(doc);if(onUI()&&doc) postDrawingCommit(windowFor(doc));
}
static void completeDrawingCommit(HWND w,bool idleFinished=false) {
    PlacementTimer timer(PlacementCost::Publication);
    auto commit=drawingCommits.find(w);if(commit==drawingCommits.end()) return;
    if(commit->second.revision&&commit->second.revision!=placementRevision(w)) {
        // A new edit/cancellation superseded this callback. Keep its retained
        // scene held until a fresh native idle has produced current geometry.
        const Obj doc=commit->second.doc;
        commit->second=DrawingCommit{doc,false,true,false,false,false,false,placementRevision(w)};
        return;
    }
    if(commit->second.refreshing) return;
    if(trackingDepth||dispatchWindow==w||GetCapture()||(GetAsyncKeyState(VK_LBUTTON)&0x8000)) {
        // A nested message pump must not reveal a partially committed edit.
        // The outer input handler will post again after its native caller ends.
        commit->second.posted=false;return;
    }
    const Obj doc=commit->second.doc;
    if(!idleFinished) {
        commit->second.awaitingIdle=true;
        if(idleDepth) return;
        // Native window idle owns offscreen-port setup as well as chemistry.
        // Observe a real completed callback; never invoke its base class from
        // a button/timer handler or skip window-specific initialization.
        if(!commit->second.idleObserved||!drawingGeometryReady(doc)) return;
    }
    KillTimer(w,drawingCommitTimer);
    commit=drawingCommits.find(w);if(commit==drawingCommits.end()) return;
    commit->second.refreshing=true;commit->second.painted=false;
    auto gpu=gpuPreviews.find(w);
    const auto held=gpu==gpuPreviews.end()?std::shared_ptr<GpuPreview>{}:gpu->second;
    // The mouse handler can return before native idle finishes the geometry.
    // Discard its temporary offscreen cache only after that work has drained.
    try {
        if(IsWindow(w)) {
            // A real native idle callback may already have produced a complete
            // GPU offscreen page. Keep that cache instead of throwing it away
            // and traversing all objects a second time for the release frame.
            if(!commit->second.idleCacheReady||!oldOffscreenValid(doc)) {
                commit->second.idleCacheReady=false;
                vf<void(*)(Obj)>(doc,0x110)(doc);vf<void(*)(Obj)>(doc,0x118)(doc);
            }
            redrawCanvas(w);
        }
    } catch(...) {
        if(auto pending=drawingCommits.find(w);pending!=drawingCommits.end())
            pending->second.refreshing=false;
        throw;
    }
    commit=drawingCommits.find(w);if(commit==drawingCommits.end()) return;
    commit->second.refreshing=false;
    if(commit->second.painted||!held||!held->usable()||gpuCanvasDisabled[w]||!IsWindow(w)) {
        drawingCommits.erase(commit);
        placementGeometryCommitted(w);
        if(held) {
            if(auto gesture=ghostGestures.find(w);gesture!=ghostGestures.end()) {
                gesture->second.releasing=true;held->releaseGhostPlacement();
            }
            held->holdScenes(false);
        }
    } else SetTimer(w,drawingCommitTimer,UINT(frameMs),nullptr);
}
static bool documentIdle(Obj doc) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    const bool synchronous=caller>=base+0x14a630&&caller<base+0x14a655;
    // Geometry has already passed through a complete native window idle. While
    // its owned chemistry job runs, repeated SaveUndo/cache/paint/hover passes
    // add no new geometry and can flood the GPU with replacement frames. Sleep
    // this regular idle path until the worker posts WM_NULL on completion.
    // Explicit synchronous callers, input, tracking and invalid geometry keep
    // their native semantics and port initialization.
    if(onUI()&&!synchronous&&!dispatchWindow&&!trackingDepth&&!paintDepth&&!GetCapture()&&
        placementChemistryPending(doc)&&drawingGeometryReady(doc)&&chemistryWorkerPending(mainPage(doc)))
        return false;
    PlacementIdleScope placementIdle(doc,!synchronous&&!dispatchWindow&&!trackingDepth&&!paintDepth&&!GetCapture(),synchronous);
    PlacementTimer idleTimer(PlacementCost::NativeIdle);
    bool more{},cacheReady{};
    {
        CounterScope processing(idleDepth);
        const bool recording=onUI()&&doc&&!trackingDepth&&beginGpuIdle(doc);
        try { more=oldIdle(doc); }
        catch(...) { if(recording) endGpuIdle();throw; }
        if(recording) {
            if(endGpuIdle()) cacheReady=oldOffscreenValid(doc);
            else disableGpuCanvas(doc);
        }
    }
    if(onUI()&&doc&&!trackingDepth) if(auto commit=drawingCommits.find(windowFor(doc));commit!=drawingCommits.end()) {
        commit->second.idleObserved=true;commit->second.idleCacheReady=cacheReady;
    }
    if(onUI()&&doc&&!trackingDepth&&drawingGeometryReady(doc)) {
        const HWND w=windowFor(doc);
        if(auto request=undoCameras.find(w);request!=undoCameras.end()&&request->second.awaitingIdle) {
            request->second.awaitingIdle=false;postUndoCamera(w);
        }
        if(auto gesture=ghostGestures.find(w);gesture!=ghostGestures.end()&&!gesture->second.releasing&&
            !drawingCommits.contains(w)&&dispatchWindow!=w&&!GetCapture()&&!(GetAsyncKeyState(VK_LBUTTON)&0x8000))
            queueGpuDrawingCommit(doc);
        auto commit=drawingCommits.find(w);
        if(commit!=drawingCommits.end()&&commit->second.awaitingIdle)
            completeDrawingCommit(w,true);
        if(dispatchWindow!=w&&!drawingCommits.contains(w)) finishPlacementHover(doc);
        refreshGhost(doc);
        refreshHighlight(doc);
    }
    if(onUI()&&doc&&!trackingDepth&&drawingPageReady(doc,more)&&placementChemistryPending(doc))
        placementChemistrySettled(windowFor(doc));
    if(onUI()&&doc&&!more&&!synchronous&&!dispatchWindow&&!paintDepth&&!trackingDepth&&
        !GetCapture()&&drawingGeometryReady(doc))warmSmartAlignmentPage(doc);
    return more;
}
static bool offscreenValid(Obj doc) {
    // GPU viewport geometry uses one raster origin throughout scrolling and
    // release. Native cache maintenance and tracker buffers keep their normal
    // validity checks, including the nested Page::DrawOffscreen call.
    const bool valid=oldOffscreenValid(doc);
    if(valid&&onUI()&&doc==vectorContentsDoc&&gpuCanvasRecording()&&!gpuBackgroundRecording())
        return gpuOffscreenAvailable(doc);
    return valid;
}
static void documentContents(Obj doc,Obj port) {
    if(fixedUiPaintDepth)return; // Fixed UI capture never traverses the molecule.
    const Obj saved=vectorContentsDoc;
    if(onUI()&&gpuCanvasRecording()&&port==at<Obj>(doc,0x258)) vectorContentsDoc=doc;
    try { oldContents(doc,port); }
    catch(...) { vectorContentsDoc=saved;throw; }
    vectorContentsDoc=saved;
}
static void paperBackground(Obj doc,Obj port) {
    // This native routine emits rounded workspace rectangles and page-edge
    // artwork. The viewport compositor owns those independently of document
    // updates, so they must not also enter its retained content scene.
    if(onUI()&&doc&&doc==paperBorderCaptureDoc&&gpuCanvasRecording()&&
        !fixedUiPaintDepth&&port==at<Obj>(doc,0x258))return;
    oldPaperBackground(doc,port);
}
static RectI canvasViewport(HWND w,Obj doc) {
    RectI pane{};vf<RectI*(*)(Obj,RectI*)>(doc,0x100)(doc,&pane);
    Obj port=at<Obj>(doc,0x258);
    const int ox=at<int>(port,0xa8),oy=at<int>(port,0xac);
    pane.l-=ox;pane.r-=ox;pane.t-=oy;pane.b-=oy;
    return intersect(pane,clientRect(w));
}
struct GhostInput { Obj target{};Point mouse{};uint64_t generation{};GhostTool tool{};POINT cursor{}; };
static std::unordered_map<HWND,GhostInput> ghostInputs;
bool displayedBondPlacement(HWND w,uint64_t identity,Point& start,Point& end) {
    if(!onUI()) return false;
    auto gpu=gpuPreviews.find(w);GhostBond ghost{};
    if(gpu==gpuPreviews.end()||!gpu->second||!gpu->second->displayedGhost(ghost)||
        !ghost.placement||ghost.toolKey!=bondToolKey()||ghost.identity!=identity) return false;
    start=ghost.start;end=ghost.end;return true;
}
static void clearGhost(HWND w) {
    if(ghostPlacementActive(w)) return;
    if(w) clearBondPlacement(w);
    ghostInputs.erase(w);
    if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)
        gpu->second->ghost({});
}
static void refreshGhost(Obj doc) {
    if(!onUI()||!doc) return;
    const HWND w=windowFor(doc);auto gpu=gpuPreviews.find(w);
    if(!w||gpu==gpuPreviews.end()||!gpu->second) return;
    if(gpu->second->ghostPlacementActive()) return;
    PreviewView shown{};PreviewPaper animatedPaper{};RectI animatedPane{};
    const bool animated=gpu->second->cameraFrame(shown,animatedPaper,animatedPane);
    const bool moving=zooms.contains(w)||pans.contains(w)||gpuHandoffs.contains(w)||cameraCommitDepth;
    auto hide=[&] { clearGhost(w); };
    // Predict unmodified placement without creating temporary document objects.
    if(trackingDepth||GetCapture()||drawingCommits.contains(w)||placementHighlights.contains(w)||
        !IsWindowVisible(w)||!IsWindowEnabled(w)||!gpu->second->usable()||!gpu->second->hasScene()||
        ((GetAsyncKeyState(VK_RBUTTON)|GetAsyncKeyState(VK_MBUTTON)|GetAsyncKeyState(VK_MENU))&0x8000)||
        (!moving&&((GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_CONTROL))&0x8000))) {
        hide();return;
    }
    // Hardware state can precede dispatch of WM_LBUTTONDOWN. Leave the visible
    // preview intact until that message transfers ownership to the press latch.
    if(GetAsyncKeyState(VK_LBUTTON)&0x8000) return;
    POINT cursor{};
    if(!GetCursorPos(&cursor)||GetAncestor(w,GA_ROOT)!=GetForegroundWindow()) { hide();return; }
    const HWND hit=WindowFromPoint(cursor);
    if((hit!=w&&!IsChild(w,hit))||!ScreenToClient(w,&cursor)) { hide();return; }
    const auto pane=animated?animatedPane:canvasViewport(w,doc);
    if(cursor.x<pane.l||cursor.x>=pane.r||cursor.y<pane.t||cursor.y>=pane.b) { hide();return; }
    const Obj page=mainPage(doc);
    Obj scale=page?at<Obj>(page,0x2a8):nullptr,port=at<Obj>(doc,0x258);
    if(!scale||!port) { hide();return; }
    Point mouse{fn<double(*)(Obj,double)>(0x3c5d80)(scale,double(cursor.x)+at<int>(port,0xa8)),
        fn<double(*)(Obj,double)>(0x3c5da0)(scale,double(cursor.y)+at<int>(port,0xac)),0};
    const double sx=animated?shown.w/(pane.r-pane.l):1,sy=animated?shown.h/(pane.b-pane.t):1;
    if(animated)mouse={animatedPaper.sourceOrigin.x+(cursor.x-pane.l-shown.x)*animatedPaper.sourceUnits/sx,
        animatedPaper.sourceOrigin.y+(cursor.y-pane.t-shown.y)*animatedPaper.sourceUnits/sy,0};
    if(!std::isfinite(mouse.x)||!std::isfinite(mouse.y)) { hide();return; }
    // Workspace beside the physical page is navigable but cannot accept atoms.
    const auto paper=animated?animatedPaper:canvasPaper(w,doc,pane);
    const double px=animated?(cursor.x-pane.l-shown.x)/sx:cursor.x-pane.l;
    const double py=animated?(cursor.y-pane.t-shown.y)/sy:cursor.y-pane.t;
    if(px<paper.bounds.l||px>paper.bounds.r||py<paper.bounds.t||py>paper.bounds.b) { hide();return; }
    try {
        GhostTool tool{};if(!readGhostTool(page,tool)) { hide();return; }
        Obj target=findToolGhostTarget(page,mouse);
        if(target&&(!at<uint8_t>(target,0x34)||!vf<bool(*)(Obj)>(target,0xb8)(target))) { hide();return; }
        const auto epoch=generation.load(std::memory_order_relaxed);
        if(auto cached=ghostInputs.find(w);cached!=ghostInputs.end()) {
            if(cached->second.target==target&&cached->second.generation==epoch&&cached->second.tool==tool&&
                cached->second.mouse.x==mouse.x&&cached->second.mouse.y==mouse.y&&
                cached->second.cursor.x==cursor.x&&cached->second.cursor.y==cursor.y) return;
            if(cached->second.tool.key!=tool.key) clearBondPlacement(w);
        }
        GhostBond ghost{};
        if(!makeToolGhost(page,target,mouse,tool,ghost)) { hide();return; }
        ghost.cursorAnchor=cursor;ghost.cursorAnchored=target==nullptr;
        ghostInputs.insert_or_assign(w,GhostInput{target,mouse,epoch,tool,cursor});
        gpu->second->ghost(ghost);
        TRACKMOUSEEVENT leave{sizeof(leave),TME_LEAVE,w,0};TrackMouseEvent(&leave);
    } catch(const std::bad_alloc&) { hide(); }
}
static PreviewPaper canvasPaper(HWND w,Obj doc,RectI pane,Graphics* inkGraphics) {
    auto paper=canvasPapers[w];
    UINT dpi=GetDpiForWindow(w);if(!dpi) dpi=GetDpiForSystem();
    paper.marginX=double(GetSystemMetricsForDpi(SM_CXSMICON,dpi))*2.0;
    paper.marginY=double(GetSystemMetricsForDpi(SM_CYSMICON,dpi))*2.0;
    Obj page=mainPage(doc),scale=page?at<Obj>(page,0x2a8):nullptr;
    if(scale) {
        Obj port=at<Obj>(doc,0x258);
        const int ox=at<int>(port,0xa8),oy=at<int>(port,0xac);
        paper.sourceUnits=at<double>(scale,8);
        paper.sourceOrigin={fn<double(*)(Obj,double)>(0x3c5d80)(scale,double(pane.l)+ox),
            fn<double(*)(Obj,double)>(0x3c5da0)(scale,double(pane.t)+oy),0};
        paper.inkTransform={};
        // Native vector ink goes through the port's Graphics transform and
        // pixel grid; the cursor camera alone omits both. Copy that mapping
        // into the immutable scene instead of guessing an offset at draw time.
        auto* graphics=inkGraphics?inkGraphics:fn<Graphics*(*)(Obj)>(0x623140)(port);
        if(graphics) {
            const float x=float(pane.l+ox),y=float(pane.t+oy);
            Gdiplus::PointF basis[3]={{x,y},{x+1,y},{x,y+1}};
            if(graphics->TransformPoints(Gdiplus::CoordinateSpaceDevice,Gdiplus::CoordinateSpaceWorld,basis,3)==Gdiplus::Ok) {
                const auto mode=graphics->GetPixelOffsetMode();
                const float offset=mode==Gdiplus::PixelOffsetModeHalf||mode==Gdiplus::PixelOffsetModeHighQuality?0.0f:0.5f;
                paper.inkTransform={basis[1].X-basis[0].X,basis[1].Y-basis[0].Y,
                    basis[2].X-basis[0].X,basis[2].Y-basis[0].Y,
                    basis[0].X-float(pane.l)+offset,basis[0].Y-float(pane.t)+offset};
            }
        }
        // Keep the page boundary in the same unrounded camera coordinates as
        // the vectors. Raster alignment happens once, at final presentation;
        // native repaint/handoff must not introduce another rounding origin.
        const auto& pageRect=at<RectD>(doc,0x230);
        if(std::isfinite(paper.sourceUnits)&&paper.sourceUnits>0) {
            paper.bounds={(pageRect.t-paper.sourceOrigin.y)/paper.sourceUnits,
                (pageRect.l-paper.sourceOrigin.x)/paper.sourceUnits,
                (pageRect.b-paper.sourceOrigin.y)/paper.sourceUnits,
                (pageRect.r-paper.sourceOrigin.x)/paper.sourceUnits};
        }
    }
    return paper;
}
static void clearFeedback(HWND w,bool immediate) {
    if(ghostPlacementActive(w)) return;
    placementHighlights.erase(w);
    if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)
        gpu->second->clearFeedback(immediate);
}
static void clearHoverHighlight(HWND w) {
    placementHighlights.erase(w);
    if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)
        gpu->second->clearHoverHighlight();
}
static uint64_t mouseHighlightIdentity(Obj doc) {
    if(!onUI()||!doc||trackingDepth||GetCapture()||
        ((GetAsyncKeyState(VK_LBUTTON)|GetAsyncKeyState(VK_RBUTTON)|GetAsyncKeyState(VK_MBUTTON))&0x8000)) return 0;
    const HWND w=windowFor(doc);
    if(!w||!IsWindowVisible(w)||!IsWindowEnabled(w)||zooms.contains(w)||pans.contains(w)||drawingCommits.contains(w)) return 0;
    POINT cursor{};
    if(!GetCursorPos(&cursor)||GetAncestor(w,GA_ROOT)!=GetForegroundWindow()) return 0;
    const HWND hit=WindowFromPoint(cursor);
    if((hit!=w&&!IsChild(w,hit))||!ScreenToClient(w,&cursor)) return 0;
    const auto pane=canvasViewport(w,doc);
    if(cursor.x<pane.l||cursor.x>=pane.r||cursor.y<pane.t||cursor.y>=pane.b) return 0;
    Obj object=at<Obj>(doc,0x2e8);if(!object) return 0;
    const auto table=at<uintptr_t>(object,0);
    // Complex atom labels, selection handles and keyboard feedback keep their
    // native route. Ordinary vertex circles and bond hover artwork animate.
    RectD circle{};
    if(table!=base+0x8babc0&&!vertexHitCircle(object,circle)) return 0;
    Obj scale=at<Obj>(object,0x68),port=at<Obj>(doc,0x258);
    if(!scale||!port) return 0;
    const Point mouse{fn<double(*)(Obj,double)>(0x3c5d80)(scale,double(cursor.x)+at<int>(port,0xa8)),
        fn<double(*)(Obj,double)>(0x3c5da0)(scale,double(cursor.y)+at<int>(port,0xac)),0};
    if(!std::isfinite(mouse.x)||!std::isfinite(mouse.y)||
        !vf<bool(*)(Obj,const Point*)>(object,0x278)(object,&mouse)) return 0;
    return at<uint64_t>(object,0xb0)+1;
}
static void refreshHighlight(Obj doc) {
    if(!onUI()||!doc) return;
    const HWND w=windowFor(doc);
    // Press/tracker ownership prevents flicker while drawing. Once released,
    // departure is a cursor decision, not a native idle/capture-completion job.
    if(trackingDepth||gpuTrackingActive()||GetCapture()||
        ((GetAsyncKeyState(VK_LBUTTON)|GetAsyncKeyState(VK_RBUTTON)|GetAsyncKeyState(VK_MBUTTON))&0x8000)) return;
    if(nativeArrowTool(fn<int(*)()>(0x4f4ba0)())) {refreshArrowHighlight(doc);return;}
    if(auto owned=placementHighlights.find(w);owned!=placementHighlights.end()) {
        Point mouse{};
        if(!mouseInPlacementCircle(doc,w,owned->second,mouse))clearHoverHighlight(w);
        return;
    }
    if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)
        gpu->second->retainHighlight(mouseHighlightIdentity(doc));
}
static void clipHighlight(Obj port,Obj primitives,RectI* bounds,bool obliterate) {
    if(gpuHoverRecording()) {
        // Render native hover primitives into their isolated transparent scene
        // immediately; the normal DrawingCanvasPort queue would bake them into
        // the page later, after capture has ended.
        fn<ClipHighlight>(0x3ca820)(port,primitives,bounds,obliterate);return;
    }
    oldClipHighlight(port,primitives,bounds,obliterate);
}
static bool captureMouseHighlight(Obj state,Obj doc,HWND w,uint64_t identity,HoverHighlight& next,
    bool requireMouseTarget=true,bool chemistryOnly=false) {
    if(!state||!identity||at<unsigned char>(state,0x10)||(requireMouseTarget&&at<Obj>(state,8)!=at<Obj>(doc,0x2e8))||
        !beginGpuHover(w,at<Obj>(doc,0x258))) return false;
    try {
        if(chemistryOnly) {
            // The native arrow cursor can suppress HighlightState's current
            // object. Capture its existing HitBoxHighlighter artwork directly,
            // without changing document selection, mouse state or active tool.
            struct PortScope {
                alignas(8) std::byte data[16]{};
                PortScope(Obj port,Obj page) {fn<void(*)(void*,Obj,Obj)>(0x3ca2d0)(data,port,page);}
                ~PortScope() {fn<void(*)(void*)>(0x3ca340)(data);}
            } portScope(at<Obj>(doc,0x258),at<Obj>(at<Obj>(state,8),0x60));
            struct Highlighter {
                alignas(8) std::byte data[96]{};
                Highlighter(Obj page,Obj port) {
                    fn<void(*)(void*,Obj,Obj)>(0x446db0)(data,page,port);
                    at<uintptr_t>(data,0)=base+0x8a1d68;
                }
                ~Highlighter() {fn<void(*)(void*)>(0x446f20)(data);}
            } highlighter(at<Obj>(at<Obj>(state,8),0x60),at<Obj>(doc,0x258));
            const Obj target=at<Obj>(state,8);
            vf<void(*)(Obj,Obj)>(target,0x3e0)(target,highlighter.data);
        } else oldMouseHighlights(state);
    }
    catch(...) { endGpuHover();throw; }
    RectI bounds{};auto scene=endGpuHover(&bounds);
    if(!scene||scene->commands.empty()) return false;
    const auto pane=canvasViewport(w,doc);
    const auto paper=canvasPaper(w,doc,pane);
    if(!valid(pane)||!valid(bounds)||!std::isfinite(paper.sourceUnits)||paper.sourceUnits<=0) return false;
    next={std::move(scene),pane,paper,identity,bounds};
    next.vertexCircle=vertexHitCircle(at<Obj>(state,8),next.circle);return true;
}
struct MouseHighlightSnapshot { Obj doc{},object{};std::array<unsigned char,8> flags{}; };
void updateArrowTargetHighlight(Obj doc,Obj target) {
    if(!onUI()||!doc) return;
    const HWND w=windowFor(doc);const auto gpu=gpuPreviews.find(w);
    if(gpu==gpuPreviews.end()||!gpu->second||!gpu->second->usable()||!gpu->second->hasScene()) return;
    RectD circle{};
    if(!target||!at<uint8_t>(target,0x34)||!vf<bool(*)(Obj)>(target,0xb8)(target)||
        (at<uintptr_t>(target,0)!=base+0x8babc0&&!vertexHitCircle(target,circle))) {
        gpu->second->retainHighlight(0);return;
    }
    MouseHighlightSnapshot state{doc,target};HoverHighlight next{};
    try {
        const auto identity=at<uint64_t>(target,0xb0)+1;
        if(captureMouseHighlight(&state,doc,w,identity,next,false,true)) gpu->second->highlight(next);
        else gpu->second->retainHighlight(identity);
    } catch(const std::bad_alloc&) {return;}
}
static void refreshArrowHighlight(Obj doc) {
    const auto input=ghostInputs.find(windowFor(doc));
    Obj target{};
    if(input!=ghostInputs.end()&&nativeArrowTool(input->second.tool.tool)&&
        input->second.generation==generation.load(std::memory_order_relaxed)) target=input->second.target;
    updateArrowTargetHighlight(doc,target);
}
void updateGpuPlacementEndpoint(Obj tracker) {
    if(!onUI()||!tracker||!gpuTrackingActive()) return;
    // Read extended fields only for the analyzed native bond tracker. Other
    // tracker classes continue using their original feedback paths.
    const auto ui=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ChemDrawUI.dll"));
    if(!ui||reinterpret_cast<uintptr_t>(vf<void(*)(Obj)>(tracker,0x38))!=ui+0x268290) return;
    Obj doc=vf<Obj(*)(Obj)>(tracker,0x80)(tracker);
    const HWND w=windowFor(doc);auto owned=placementHighlights.find(w);auto found=gpuPreviews.find(w);
    if(owned==placementHighlights.end()||found==gpuPreviews.end()||!found->second) return;
    Obj atom=at<Obj>(tracker,0x170);RectD circle{};
    if(!atom||!vertexHitCircle(atom,circle)) return;
    const auto identity=at<uint64_t>(atom,0xb0)+1;
    if(identity==owned->second.source) {
        if(found->second->placementEndpoint({})) { owned->second.endpoint=0;owned->second.endpointCircle={}; }
        return;
    }
    if(identity==owned->second.endpoint&&memcmp(&circle,&owned->second.endpointCircle,sizeof(circle))==0) return;
    MouseHighlightSnapshot state{doc,atom};HoverHighlight next{};
    try {
        if(!captureMouseHighlight(&state,doc,w,identity,next,false)||!found->second->placementEndpoint(next)) return;
    } catch(const std::bad_alloc&) { return; }
    owned->second.endpoint=identity;owned->second.endpointCircle=circle;
}
static bool mouseInPlacementCircle(Obj doc,HWND w,const PlacementHighlight& owned,Point& mouse) {
    POINT cursor{};
    if(!IsWindowVisible(w)||!IsWindowEnabled(w)||!GetCursorPos(&cursor)||GetAncestor(w,GA_ROOT)!=GetForegroundWindow()) return false;
    const HWND hit=WindowFromPoint(cursor);
    if((hit!=w&&!IsChild(w,hit))||!ScreenToClient(w,&cursor)) return false;
    PreviewView shown{};PreviewPaper paper{};RectI animatedPane{};
    const auto gpu=gpuPreviews.find(w);
    const bool animated=gpu!=gpuPreviews.end()&&gpu->second&&gpu->second->cameraFrame(shown,paper,animatedPane);
    const auto pane=animated?animatedPane:canvasViewport(w,doc);
    if(cursor.x<pane.l||cursor.x>=pane.r||cursor.y<pane.t||cursor.y>=pane.b) return false;
    Obj page=mainPage(doc),scale=page?at<Obj>(page,0x2a8):nullptr,port=at<Obj>(doc,0x258);
    if(!scale||!port) return false;
    mouse={fn<double(*)(Obj,double)>(0x3c5d80)(scale,double(cursor.x)+at<int>(port,0xa8)),
        fn<double(*)(Obj,double)>(0x3c5da0)(scale,double(cursor.y)+at<int>(port,0xac)),0};
    if(animated) {
        const double sx=shown.w/(pane.r-pane.l),sy=shown.h/(pane.b-pane.t);
        mouse={paper.sourceOrigin.x+(cursor.x-pane.l-shown.x)*paper.sourceUnits/sx,
            paper.sourceOrigin.y+(cursor.y-pane.t-shown.y)*paper.sourceUnits/sy,0};
    }
    if(!std::isfinite(mouse.x)||!std::isfinite(mouse.y)) return false;
    auto inside=[&](const RectD& c) {
        const double radius=(c.r-c.l)*0.5;
        return radius>0&&std::hypot(mouse.x-(c.l+c.r)*0.5,mouse.y-(c.t+c.b)*0.5)<=radius;
    };
    return inside(owned.sourceCircle)||(owned.endpoint&&inside(owned.endpointCircle));
}
static void finishPlacementHover(Obj doc) {
    const HWND w=windowFor(doc);
    if(!placementHighlights.contains(w)||trackingDepth||gpuTrackingActive()||GetCapture()||
        dispatchWindow==w||drawingCommits.contains(w)||gpuHoverRecording()||gpuCanvasRecording()||
        ((GetAsyncKeyState(VK_LBUTTON)|GetAsyncKeyState(VK_RBUTTON)|GetAsyncKeyState(VK_MBUTTON))&0x8000)) return;
    auto found=gpuPreviews.find(w);
    if(found==gpuPreviews.end()||!found->second||!found->second->usable()||gpuCanvasDisabled[w]) {
        clearFeedback(w,true);return;
    }
    const auto gpu=found->second;
    // Idle has finalized the edit and published its page. Refresh the native
    // target before interpreting an empty tracker-era HighlightState as leave.
    oldHover(doc);
    if(!placementHighlights.contains(w)) return;
    Point releasedMouse{};
    if(!mouseInPlacementCircle(doc,w,placementHighlights.at(w),releasedMouse)) {
        // Empty/stale native HighlightState is irrelevant after a confirmed
        // departure. It must not keep the placement circle latched forever.
        clearHoverHighlight(w);return;
    }
    HoverHighlight next{};
    try {
        auto identity=mouseHighlightIdentity(doc);
        if(fn<int(*)()>(0x4f4ba0)()==3&&
            !((GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_CONTROL)|GetAsyncKeyState(VK_MENU))&0x8000)) {
            Point mouse{};
            if(mouseInPlacementCircle(doc,w,placementHighlights.at(w),mouse)) {
                // Native cursor suppression can report no object, or the new
                // bond can temporarily precede its vertex in drawing order.
                // Resolve a live atom through native hit testing, preserving
                // vertex priority inside the source/endpoint hit circle.
                Obj atom=fn<Obj(*)(Obj,const Point*,Obj)>(0x7107c0)(mainPage(doc),&mouse,nullptr);RectD circle{};
                if(vertexHitCircle(atom,circle)&&at<unsigned char>(atom,0x34)&&vf<bool(*)(Obj)>(atom,0xb8)(atom)&&
                    vf<bool(*)(Obj,const Point*)>(atom,0x278)(atom,&mouse)) {
                    fn<void(*)(Obj,Obj)>(0x157d80)(doc,atom);
                    identity=mouseHighlightIdentity(doc);
                    if(!identity) return;
                }
            }
        }
        Obj state=fn<Obj(*)(Obj)>(0x14f220)(doc);
        if(!state||at<unsigned char>(state,0x10)||at<Obj>(state,8)!=at<Obj>(doc,0x2e8)) return;
        if(identity&&!captureMouseHighlight(state,doc,w,identity,next)) return;
    } catch(const std::bad_alloc&) { return; }
    // Keep the old circle on capture failure. Successful publication removes
    // the placement latch and sets the replacement under the same worker lock.
    if(gpu->finishPlacementHighlight(next)) placementHighlights.erase(w);
}
static void mouseHighlights(Obj state) {
    Obj doc=state?at<Obj>(state,0):nullptr;
    const HWND w=onUI()&&doc?windowFor(doc):nullptr;
    auto gpu=gpuPreviews.find(w);
    Obj object=state?at<Obj>(state,8):nullptr;RectD circle{};
    const bool supported=onUI()&&object&&(at<uintptr_t>(object,0)==base+0x8babc0||vertexHitCircle(object,circle));
    if(supported&&doc&&nativeArrowTool(fn<int(*)()>(0x4f4ba0)())&&gpu!=gpuPreviews.end()&&
        gpu->second&&gpu->second->usable()&&(gpuCanvasRecording()||gpu->second->hasScene())) {
        // Chemistry snap targets have their own isolated hover capture.
        // Arrow resize handles still belong to the native feedback route.
        return;
    }
    GpuObjectScope arrowFeedback(onUI()&&object&&at<uintptr_t>(object,0)==base+0x8b2cf0?object:nullptr);
    const bool animated=supported&&gpu!=gpuPreviews.end()&&gpu->second&&gpu->second->usable()&&
        (gpuCanvasRecording()||gpu->second->hasScene());
    if(animated&&placementHighlights.contains(w)) return;
    if(animated&&!trackingDepth&&!GetCapture()&&(GetAsyncKeyState(VK_LBUTTON)&0x8000)) return;
    const auto identity=mouseHighlightIdentity(doc);
    // Tracker cleanup can draw an obsolete state after the live target has
    // changed. Never bake that circle into the committed page underneath the
    // isolated GPU hover scene; the next coherent hover draw supplies artwork.
    if(animated&&at<Obj>(state,8)!=at<Obj>(doc,0x2e8)&&!trackingDepth&&!GetCapture()) return;
    // A stale native mouse object can remain after leaving the canvas or a
    // camera change. Keep its outgoing GPU fade out of the cached page.
    if(!identity&&onUI()&&doc&&!trackingDepth&&!GetCapture()&&gpu!=gpuPreviews.end()&&gpu->second&&
        gpu->second->usable()&&(gpuCanvasRecording()||gpu->second->hasScene())&&
        at<Obj>(state,8)==at<Obj>(doc,0x2e8)) {
        if(supported) {
            gpu->second->retainHighlight(0);return;
        }
    }
    if(!identity||at<Obj>(state,8)!=at<Obj>(doc,0x2e8)||gpu==gpuPreviews.end()||!gpu->second||
        !gpu->second->usable()||(!gpuCanvasRecording()&&!gpu->second->hasScene())) {
        if(gpu!=gpuPreviews.end()&&gpu->second) gpu->second->retainHighlight(identity);
        oldMouseHighlights(state);return;
    }
    HoverHighlight next{};
    if(!captureMouseHighlight(state,doc,w,identity,next)) {
        gpu->second->retainHighlight(identity);
        // A supported hover must never fall back into the retained page. Keep
        // its previous matching overlay; a later hover draw can retry capture.
        if(!animated)oldMouseHighlights(state);
        return;
    }
    gpu->second->highlight(next);
}
static void displayWorkspaceBars(HWND w,Obj doc,RectI pane,const PreviewPaper& paper,PreviewView shown) {
    if(!onUI()||!doc||workspaceDepth||!IsWindowVisible(w)||!IsWindowEnabled(w)||!valid(pane))return;
    const double width=pane.r-pane.l,height=pane.b-pane.t;
    const double sx=shown.w/width,sy=shown.h/height;
    const double pw=(paper.bounds.r-paper.bounds.l)*sx,ph=(paper.bounds.b-paper.bounds.t)*sy;
    if(!valid(paper.bounds)||pw<=0||ph<=0) return;
    CounterScope updating(workspaceDepth);
    WorkspaceBars bars;bars.doc=doc;
    auto install=[&](Obj bar,double viewport,double page,double left,double margin,HWND& out) {
        HWND handle=bar?at<HWND>(bar,0):nullptr;
        // Native suspension and embedded-view rules own control visibility.
        if(!handle||!IsWindowVisible(handle)) return;
        const auto axis=previewPanRange(viewport,page,margin);
        const int extent=int(std::clamp(std::ceil(axis.high-axis.low),1.0,double(INT_MAX/4)));
        const UINT pageSize=UINT(std::clamp(std::ceil(viewport),1.0,double(INT_MAX/4)));
        SCROLLINFO info{};info.cbSize=sizeof(info);info.fMask=SIF_RANGE|SIF_PAGE|SIF_POS;
        info.nMin=0;info.nMax=extent+int(pageSize)-1;info.nPage=pageSize;
        info.nPos=int(std::lround(std::clamp(axis.high-left,0.0,double(extent))));
        SCROLLINFO current{};current.cbSize=sizeof(current);current.fMask=info.fMask;
        const bool changed=!GetScrollInfo(handle,SB_CTL,&current)||current.nMin!=info.nMin||
            current.nMax!=info.nMax||current.nPage!=info.nPage||current.nPos!=info.nPos;
        if(changed) SetScrollInfo(handle,SB_CTL,&info,TRUE);
        if(!IsWindowEnabled(handle)) EnableWindow(handle,TRUE);
        if(changed) EnableScrollBar(handle,SB_CTL,ESB_ENABLE_BOTH);
        out=handle;
    };
    install(at<Obj>(doc,0x560),width,pw,shown.x+paper.bounds.l*sx,paper.marginX,bars.horizontal);
    install(at<Obj>(doc,0x570),height,ph,shown.y+paper.bounds.t*sy,paper.marginY,bars.vertical);
    if(bars.horizontal||bars.vertical) workspaceBars[w]=bars;else workspaceBars.erase(w);
}
static void refreshWorkspace(Obj doc) {
    if(!onUI()||!doc||workspaceDepth||sizeDepth||scrollDepth)return;
    HWND w=windowFor(doc);
    if(!w||!mainPage(doc)||zooms.contains(w)||pans.contains(w))return;
    const auto pane=canvasViewport(w,doc);
    displayWorkspaceBars(w,doc,pane,canvasPaper(w,doc,pane),{0,0,double(pane.r-pane.l),double(pane.b-pane.t)});
}
static void scrollVisibility(Obj doc) {
    oldScrollVisibility(doc);refreshWorkspace(doc);
}
static void setSize(Obj doc,int width,int height) {
    if(onUI()&&doc&&!sizeDepth) {
        HWND w=windowFor(doc);if(w) { finishZoom(w);finishPan(w); }
    }
    { CounterScope resizing(sizeDepth);oldSetSize(doc,width,height); }
    refreshWorkspace(doc);
}
static void scrolling(Obj doc,Obj port,const int* x,const int* y) {
    HWND w=onUI()&&doc?windowFor(doc):nullptr;
    if(w) cancelGhostGesture(w);
    if(w&&!cameraCommitDepth) clearGhost(w);
    if(w&&!cameraCommitDepth) clearFeedback(w,true);
    // Scrollbar callbacks may enter native scrolling directly from the child
    // control, without first dispatching WM_HSCROLL to the document window.
    if(w&&!sizeDepth&&!scrollDepth&&!workspaceDepth) { finishZoom(w);finishPan(w); }
    auto found=workspaceBars.find(w);
    if(!w||scrollDepth||workspaceDepth||!mainPage(doc)||
        (found==workspaceBars.end()&&scrollCenterDoc!=doc)) {
        oldScrolling(doc,port,x,y);refreshWorkspace(doc);return;
    }
    const auto pane=canvasViewport(w,doc);
    const auto paper=canvasPaper(w,doc,pane);
    if(!valid(pane)||!valid(paper.bounds)||paper.bounds.r<=paper.bounds.l||paper.bounds.b<=paper.bounds.t) {
        oldScrolling(doc,port,x,y);return;
    }
    const double width=pane.r-pane.l,height=pane.b-pane.t;
    const auto ax=previewPanRange(width,paper.bounds.r-paper.bounds.l,paper.marginX);
    const auto ay=previewPanRange(height,paper.bounds.b-paper.bounds.t,paper.marginY);
    const WorkspaceBars bars=found==workspaceBars.end()?WorkspaceBars{}:found->second;
    auto requested=[](double current,const int* delta,HWND bar,PreviewAxis axis) {
        if(delta) return current-double(*delta);
        if(bar&&IsWindowVisible(bar)) return axis.high-double(GetScrollPos(bar,SB_CTL));
        return current;
    };
    // SetMySize has just replaced the normalized bars with native ranges.
    // Their positions cannot describe our camera until refreshWorkspace runs.
    // Preserve the current page position on those null-delta resize calls.
    double left=sizeDepth&&!x?paper.bounds.l:requested(paper.bounds.l,x,bars.horizontal,ax);
    double top=sizeDepth&&!y?paper.bounds.t:requested(paper.bounds.t,y,bars.vertical,ay);
    if(scrollCenterDoc==doc) {
        // Native ZoomChanged may discard its center when the page fits. Apply
        // the requested anchored camera through explicit native scroll deltas.
        Obj ds=at<Obj>(mainPage(doc),0x2a8),screen=at<Obj>(doc,0x258);
        const double units=at<double>(ds,8);
        if(std::isfinite(units)&&units>0) {
            const double px=(scrollCenter.x-at<double>(ds,0x20))/units+
                at<int>(ds,0x10)-at<int>(screen,0xa8)-pane.l;
            const double py=(scrollCenter.y-at<double>(ds,0x28))/units+
                at<int>(ds,0x14)-at<int>(screen,0xac)-pane.t;
            left=paper.bounds.l-(px-width*0.5);top=paper.bounds.t-(py-height*0.5);
        }
    }
    left=std::clamp(left,ax.low,ax.high);top=std::clamp(top,ay.low,ay.high);
    const int dx=int(std::lround(paper.bounds.l-left)),dy=int(std::lround(paper.bounds.t-top));
    { CounterScope moving(scrollDepth);oldScrolling(doc,port,&dx,&dy); }
    refreshWorkspace(doc);
}
static void retireGpuAfterPaint(HWND w,bool complete=true,bool recorded=false) {
    if(!complete) return;
    auto it=gpuHandoffs.find(w);if(it==gpuHandoffs.end()) return;
    if(recorded) {
        // Recording is only a queued replacement. The worker acknowledges it
        // after composition commit and the next display-ready frame slot.
        it->second->finish();return;
    }
    GdiFlush();it->second->finish();gpuHandoffs.erase(it);
}
static uintptr_t nativePaint(Obj doc,Obj port,HWND w) {
    // Native compatibility painting must own the viewport once entered. A
    // resident opaque GPU scene would otherwise hide new CPU interaction ink.
    if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second&&!gpuCanvasDisabled[w])
        disableGpuCanvas(doc);
    const auto result=oldPaint(doc,port);
    if(gpuHandoffs.contains(w)) {
        auto* graphics=fn<Graphics*(*)(Obj)>(0x623140)(port);
        if(graphics) graphics->Flush(Gdiplus::FlushIntentionSync);
        retireGpuAfterPaint(w);
    }
    return result;
}
static void handoffGpu(HWND w,Obj doc,NavigationFrame& preview,RectI pane) {
    if(!preview.gpu) return;
    // Undo will publish different content immediately after execution. Commit
    // the native camera once, without redrawing/capturing the obsolete page.
    if(historyPrepareDepth) return;
    // ZoomChanged/DoScrolling may clamp scroll positions, recenter a small
    // page, or change the port origin. Read their resulting mapping rather
    // than handing off at the speculative wheel target.
    alignGpuToNative(doc,preview,pane);
    gpuHandoffs[w]=preview.gpu;
    // Finish the settled page underneath the last complete GPU frame, before
    // a modal bond tracker can begin while a native paint is still queued.
    if(IsWindow(w)&&IsWindowVisible(w)) {
        vf<void(*)(Obj)>(doc,0x110)(doc);vf<void(*)(Obj)>(doc,0x118)(doc);
        redrawCanvas(w);
    }
}
void clearHighlights() {
    if(!onUI()) return;
    for(auto& pair:gpuPreviews) if(pair.second&&!placementHighlights.contains(pair.first)) pair.second->clearFeedback(true);
    clearBondPlacement(nullptr);
    ghostInputs.clear();
    for(auto& pair:frames) invalidateCanvas(pair.first);
}
void forgetPresentation(HWND w) {
    KillTimer(w,zoomTimer);KillTimer(w,panTimer);KillTimer(w,drawingCommitTimer);
    zooms.erase(w);pans.erase(w);panRemainders.erase(w);frames.erase(w);
    fixedUiFrames.erase(w);
    pageCaptureFrames.erase(w);
    undoCameras.erase(w);
    gpuHandoffs.erase(w);gpuPreviews.erase(w);gpuCanvasDisabled.erase(w);canvasPapers.erase(w);workspaceBars.erase(w);drawingCommits.erase(w);
    forgetGpuCanvas(w);
    ghostInputs.erase(w);
    ghostGestures.erase(w);
    clearBondPlacement(w);
    placementHighlights.erase(w);
}
bool publishGpuCanvasScene(Obj doc,std::shared_ptr<const GpuScene> scene,std::shared_ptr<const AlignmentFeedback> alignment) {
    HWND w=windowFor(doc);if(!w||!scene) return false;
    auto& gpu=gpuPreviews[w];if(!gpu) gpu=GpuPreview::create(w);
    const auto pane=canvasViewport(w,doc);
    return gpu&&gpu->scene(std::move(scene),pane,canvasPaper(w,doc,pane),false,std::move(alignment));
}
void clearGpuAlignment(Obj doc) noexcept {
    if(!onUI()||!doc)return;
    const auto i=gpuPreviews.find(windowFor(doc));
    if(i!=gpuPreviews.end()&&i->second)i->second->clearAlignment();
}
void disableGpuCanvas(Obj doc) {
    HWND w=windowFor(doc);if(!w||gpuCanvasDisabled[w]) return;
    cancelGhostGesture(w);
    clearGhost(w);
    gpuCanvasDisabled[w]=true;discardGpuCanvas(w);
    if(auto i=gpuPreviews.find(w);i!=gpuPreviews.end()&&i->second) {
        i->second->clearScene();
        // A failed canvas worker must not permanently disable independent
        // bitmap zoom/pan. Canvas recording stays disabled for this window.
        if(!i->second->usable()) i->second=GpuPreview::create(w);
    }
    // GPU-backed native cache bitmaps contain state/geometry, not CPU pixels.
    // Recreate those caches before a compatibility repaint or tracker uses them.
    vf<void(*)(Obj)>(doc,0x110)(doc);vf<void(*)(Obj)>(doc,0x118)(doc);
    InvalidateRect(w,nullptr,FALSE);
}
static bool fixedUiDamaged(RectI damage,RectI pane,RectI client) {
    damage=intersect(damage,client);
    return valid(damage)&&(damage.l<pane.l||damage.t<pane.t||damage.r>pane.r||damage.b>pane.b);
}
static void configurePaintTarget(Graphics* target,Graphics* source) {
    target->ResetTransform();target->ResetClip();
    target->SetPageUnit(source->GetPageUnit());target->SetPageScale(source->GetPageScale());
    Gdiplus::Matrix transform;source->GetTransform(&transform);target->SetTransform(&transform);
    target->SetSmoothingMode(source->GetSmoothingMode());
    target->SetPixelOffsetMode(source->GetPixelOffsetMode());
    target->SetTextRenderingHint(source->GetTextRenderingHint());
    target->SetCompositingMode(source->GetCompositingMode());
    target->SetCompositingQuality(source->GetCompositingQuality());
}
static bool paintFixedUi(HWND w,Obj doc,RectI damage) {
    if(!onUI()||!doc||paintDepth||fixedUiPaintDepth)return false;
    const auto client=clientRect(w),pane=canvasViewport(w,doc);
    if(!valid(client)||!valid(pane))return false;
    if(same(client,pane))return true;
    auto& gpu=gpuPreviews[w];if(!gpu)gpu=GpuPreview::create(w);
    if(!gpu||!gpu->usable())return false;
    if(gpu->hasFixedUi(pane,client)&&!fixedUiDamaged(damage,pane,client))return true;
    const auto port=at<Obj>(doc,0x258);
    auto* source=fn<Graphics*(*)(Obj)>(0x623140)(port);if(!source)return false;
    try {
        auto& slot=fixedUiFrames[w];if(!slot)slot=std::make_unique<PaintFrame>();
        if(!slot->prepare(w,source))return false;
        auto* target=slot->graphics.get();const auto saved=target->Save();
        configurePaintTarget(target,source);
        // Capture all fixed strips together whenever they change. This makes
        // coalescing UI requests safe without replaying a page-cache history.
        Gdiplus::Matrix transform;target->GetTransform(&transform);target->ResetTransform();
        const auto unit=target->GetPageUnit();const auto scale=target->GetPageScale();
        target->SetPageUnit(Gdiplus::UnitPixel);target->SetPageScale(1);
        target->SetClip(Gdiplus::Rect(0,0,client.r,client.b));
        target->ExcludeClip(Gdiplus::Rect(pane.l,pane.t,pane.r-pane.l,pane.b-pane.t));
        target->SetPageUnit(unit);target->SetPageScale(scale);
        target->SetTransform(&transform);
        if(!beginGpuFixedUi(w,port,target)) {target->Restore(saved);return false;}
        auto* previous=at<Graphics*>(port,0x148);at<Graphics*>(port,0x148)=target;
        ++paintDepth;++fixedUiPaintDepth;
        try {oldPaint(doc,port);}
        catch(...) {
            --fixedUiPaintDepth;--paintDepth;at<Graphics*>(port,0x148)=previous;
            endGpuFixedUi();target->Restore(saved);throw;
        }
        --fixedUiPaintDepth;--paintDepth;at<Graphics*>(port,0x148)=previous;
        auto scene=endGpuFixedUi();target->Restore(saved);
        return scene&&gpu->fixedUi(std::move(scene),pane);
    } catch(const std::bad_alloc&) {return false;}
}
static uintptr_t paintDocument(Obj doc,Obj port) {
    if(!onUI()||!doc||!port||paintDepth||port!=at<Obj>(doc,0x258))
        return oldPaint(doc,port);
    HWND w=windowFor(doc);
    if(!w) return oldPaint(doc,port);
    markGpuDocument(w);
    RectI damage=clientRect(w);
    if(updateWindow==w)damage=intersect(damage,updateBounds);
    const bool fixedUiPainted=paintFixedUi(w,doc,damage);
    if(trackingDepth&&!gpuTrackingRecording()) return nativePaint(doc,port,w);
    if(auto it=zooms.find(w);it!=zooms.end()) { drawZoom(w,*it->second);return 0; }
    if(auto it=pans.find(w);it!=pans.end()) { drawPan(w,*it->second);return 0; }
    refreshWorkspace(doc);
    auto* source=fn<Graphics*(*)(Obj)>(0x623140)(port);
    if(!source) return nativePaint(doc,port,w);
    PaintFrame* frame{};
    try {
        auto& slot=frames[w];if(!slot) slot=std::make_unique<PaintFrame>();frame=slot.get();
        if(!frame->prepare(w,source)) return nativePaint(doc,port,w);
    } catch(const std::bad_alloc&) { return nativePaint(doc,port,w); }
    RectI copyArea=clientRect(w);
    if(updateWindow==w) copyArea=intersect(copyArea,updateBounds);
    if(!valid(copyArea)) return 0;
    auto* target=frame->graphics.get();
    auto& gpu=gpuPreviews[w];if(!gpu) gpu=GpuPreview::create(w);
    const auto pane=canvasViewport(w,doc);
    const auto canvasArea=intersect(copyArea,pane);
    if(!valid(canvasArea)&&fixedUiPainted)return 0;
    const bool complete=copyArea.l<=pane.l&&copyArea.t<=pane.t&&copyArea.r>=pane.r&&copyArea.b>=pane.b;
    const bool recording=valid(canvasArea)&&gpu&&gpu->usable()&&beginGpuCanvas(w,port,target,!complete);
    if(recording)copyArea=canvasArea;
    if(!recording&&!gpuCanvasDisabled[w]) disableGpuCanvas(doc);
    if(!recording) {
        HDC screen=GetDC(w);
        const BOOL copied=screen&&BitBlt(frame->pixels.dc,copyArea.l,copyArea.t,
            copyArea.r-copyArea.l,copyArea.b-copyArea.t,screen,copyArea.l,copyArea.t,SRCCOPY);
        if(screen) ReleaseDC(w,screen);
        if(!copied) return nativePaint(doc,port,w);
    }
    GdiFlush();
    const auto saved=target->Save();
    target->ResetTransform();target->ResetClip();
    target->SetPageUnit(source->GetPageUnit());target->SetPageScale(source->GetPageScale());
    Gdiplus::Matrix transform;source->GetTransform(&transform);target->SetTransform(&transform);
    target->SetSmoothingMode(source->GetSmoothingMode());
    target->SetPixelOffsetMode(source->GetPixelOffsetMode());
    target->SetTextRenderingHint(source->GetTextRenderingHint());
    target->SetCompositingMode(source->GetCompositingMode());
    target->SetCompositingQuality(source->GetCompositingQuality());
    // Swap the port's graphics pointer as well as the drawing destination; some
    // native path operations read this member without calling GetGraphics().
    auto* savedGraphics=at<Graphics*>(port,0x148);at<Graphics*>(port,0x148)=target;
    struct PaperBorderCapture {
        Obj previous{paperBorderCaptureDoc};
        explicit PaperBorderCapture(Obj owner) {paperBorderCaptureDoc=owner;}
        ~PaperBorderCapture() {paperBorderCaptureDoc=previous;}
    } paperCapture(recording?doc:nullptr);
    ++paintDepth;uintptr_t result{};
    try { result=oldPaint(doc,port); }
    catch(...) { --paintDepth;at<Graphics*>(port,0x148)=savedGraphics;target->Restore(saved);
        if(recording) { endGpuCanvas();disableGpuCanvas(doc); }throw; }
    --paintDepth;
    const auto capturedPaper=recording?canvasPaper(w,doc,pane,target):PreviewPaper{};
    at<Graphics*>(port,0x148)=savedGraphics;
    if(recording) {
        auto scene=endGpuCanvas();target->Restore(saved);
        // Rebuild once at camera handoff with the same flat, complete page
        // capture used by the animation. Older offscreen histories can contain
        // raster compatibility/depth checkpoints that would lower its quality.
        // Ordinary drawing and partial damage retain their incremental path.
        if(scene&&complete&&gpuHandoffs.contains(w)&&historyPublishDoc!=doc)
            if(auto whole=captureWholePage(w,doc,pane,canvasPaper(w,doc,pane)))scene=std::move(whole);
        if(scene&&gpu->scene(scene,pane,capturedPaper,historyPublishDoc==doc)) {
            if(historyPublishDoc==doc)historyPublished=true;
            if(complete) if(auto commit=drawingCommits.find(w);commit!=drawingCommits.end()&&commit->second.refreshing)
                commit->second.painted=true;
            retireGpuAfterPaint(w,complete,true);return result;
        }
        disableGpuCanvas(doc);return nativePaint(doc,port,w);
    }
    target->Flush(Gdiplus::FlushIntentionSync);target->Restore(saved);
    HDC destination=source->GetHDC();
    if(destination) {
        BitBlt(destination,copyArea.l,copyArea.t,copyArea.r-copyArea.l,copyArea.b-copyArea.t,
            frame->pixels.dc,copyArea.l,copyArea.t,SRCCOPY);
        source->ReleaseHDC(destination);
        retireGpuAfterPaint(w);
    } else {
        InvalidateRect(w,nullptr,FALSE);
    }
    return result;
}
static bool mouseBondFeedback(Obj doc) {
    if(!onUI()||!doc||trackingDepth||fn<int(*)()>(0x4f4ba0)()!=3) return false;
    Obj port=at<Obj>(doc,0x258);
    if(!port) return false;
    struct MousePoint { int x,y,z; } point{};
    vf<MousePoint*(*)(Obj,MousePoint*)>(port,0x398)(port,&point);
    const RectI area=drawingRect(doc);
    return point.x>=area.l&&point.x<area.r&&point.y>=area.t&&point.y<area.b;
}
static void atomHotKeyFeedback(Obj atom,Obj highlighter) {
    if(gpuOwnsHoverHighlight(atom,highlighter)) return;
    Obj page=atom?at<Obj>(atom,0x60):nullptr;
    Obj doc=page?at<Obj>(page,8):nullptr;
    if(mouseBondFeedback(doc)) {
        // DrawHighlightedObjectHighlights also takes the keyboard-feedback
        // branch for a CURRENT hovered atom. That branch draws a predicted
        // growth position, not the source atom's mouse attachment rectangle.
        fn<AtomFeedback>(0x2c39d0)(atom,highlighter);
        return;
    }
    oldAtomHotKeyFeedback(atom,highlighter);
}
static void hotKeyFeedback(Obj state) {
    // A retained keyboard object is not necessarily the mouse target. With the
    // bond tool, avoid showing its hit box as though a drag there will attach.
    if(onUI()&&state&&mouseBondFeedback(at<Obj>(state,0))) return;
    oldHotKeyFeedback(state);
}
static uintptr_t hover(Obj doc) {
    if(onUI()&&doc&&(zooms.contains(windowFor(doc))||pans.contains(windowFor(doc)))) {
        refreshGhost(doc);return 1;
    }
    // The cursor and active object must be updated in the same input dispatch.
    const auto result=oldHover(doc);refreshGhost(doc);refreshHighlight(doc);return result;
}
static std::shared_ptr<const GpuScene> captureWholePage(HWND w,Obj doc,RectI pane,const PreviewPaper& paper) {
    if(trackingDepth||gpuTrackingActive()||!valid(paper.bounds)) return {};
    Obj page=mainPage(doc),port=at<Obj>(doc,0x258);if(!page||!port)return {};
    auto* source=fn<Graphics*(*)(Obj)>(0x623140)(port);if(!source)return {};
    struct PageDrawOptions { int mode{};bool screen{true};BYTE padding[3]{};RectD clip{};Obj status{}; };
    PageDrawOptions options;
    fn<RectD*(*)(Obj,RectD*)>(0x7132d0)(page,&options.clip);
    if(!valid(options.clip))return {};
    try {
        auto& slot=pageCaptureFrames[w];if(!slot)slot=std::make_unique<PaintFrame>();
        if(!slot->prepare(w,source))return {};
        auto* target=slot->graphics.get();const auto saved=target->Save();
        configurePaintTarget(target,source);
        // Set the clip in device coordinates without moving the native camera.
        // Capture commands beyond the window, rather than relying on the
        // viewport-limited native cache or manufacturing a huge render target.
        Gdiplus::Matrix transform;target->GetTransform(&transform);
        const auto unit=target->GetPageUnit();const auto scale=target->GetPageScale();
        const SceneRect extent{float(pane.l+paper.bounds.l),float(pane.t+paper.bounds.t),
            float(paper.bounds.r-paper.bounds.l),float(paper.bounds.b-paper.bounds.t)};
        target->ResetTransform();target->SetPageUnit(Gdiplus::UnitPixel);target->SetPageScale(1);
        const auto clipped=target->SetClip(Gdiplus::RectF(extent.x,extent.y,extent.w,extent.h));
        target->SetPageUnit(unit);target->SetPageScale(scale);target->SetTransform(&transform);
        if(clipped!=Gdiplus::Ok||!beginGpuFixedUi(w,port,target)) {target->Restore(saved);return {};}
        auto* previous=at<Graphics*>(port,0x148);at<Graphics*>(port,0x148)=target;
        ++paintDepth;
        try {fn<void(*)(Obj,Obj,const PageDrawOptions*)>(0x70da40)(page,port,&options);}
        catch(...) {
            --paintDepth;at<Graphics*>(port,0x148)=previous;endGpuFixedUi();target->Restore(saved);throw;
        }
        --paintDepth;at<Graphics*>(port,0x148)=previous;
        auto captured=endGpuFixedUi();target->Restore(saved);
        if(!captured)return {};
        auto scene=std::const_pointer_cast<GpuScene>(captured);scene->extent=extent;
        return scene;
    } catch(const std::exception&) {return {};}
}
static bool captureNavigation(HWND w,Obj doc,RectI& pane,NavigationFrame& preview,bool fresh=false,
        std::shared_ptr<const GpuScene> capturedPage={}) {
    RectI client=clientRect(w);
    // GetDrawingPaneRect (slot 0x68) clips to the paper. Native scrolling,
    // zoom centering and page-background drawing use the full viewport.
    pane=canvasViewport(w,doc);
    Obj port=at<Obj>(doc,0x258);
    const int ox=at<int>(port,0xa8),oy=at<int>(port,0xac);
    Obj page=mainPage(doc),scale=page?at<Obj>(page,0x2a8):nullptr;
    if(!scale) return false;
    preview.sourceUnits=at<double>(scale,8);
    if(!std::isfinite(preview.sourceUnits)||preview.sourceUnits<=0) return false;
    preview.sourceOrigin={fn<double(*)(Obj,double)>(0x3c5d80)(scale,double(pane.l)+ox),
        fn<double(*)(Obj,double)>(0x3c5da0)(scale,double(pane.t)+oy),0};
    preview.paper=canvasPaper(w,doc,pane);
    preview.initialView={0,0,double(pane.r-pane.l),double(pane.b-pane.t)};
    if(capturedPage) {
        // History publication and its optional camera reveal share one fresh
        // vector capture. Neither a second traversal nor screen pixel readback
        // is needed to begin that animation.
        const auto gpu=gpuPreviews.find(w);
        if(!valid(pane)||gpu==gpuPreviews.end()||!gpu->second||
            !gpu->second->begin(nullptr,0,pane,preview.paper,preview.initialView,std::move(capturedPage)))return false;
        preview.gpu=gpu->second;
        preview.sourceOrigin=preview.paper.sourceOrigin;preview.sourceUnits=preview.paper.sourceUnits;
        return true;
    }
    auto& snapshot=preview.snapshot;
    if(!valid(pane)||!snapshot.size(client.r,client.b)) return false;
    HDC dc=GetDC(w);
    const BOOL copied=dc&&BitBlt(snapshot.dc,0,0,client.r,client.b,dc,0,0,SRCCOPY);
    if(dc) ReleaseDC(w,dc);
    GdiFlush();
    if(copied) {
        auto& gpu=gpuPreviews[w];if(!gpu) gpu=GpuPreview::create(w);
        // GetDC reads the native backing window, not the composition visual.
        // Its colors are authoritative only when no GPU scene owns the canvas.
        if(!gpu||!gpu->hasScene()) {
        // Use the native viewport's most frequent blank colors. This preserves
        // custom page backgrounds and view-only colors without reading the
        // private CDColor class layout. Ignore the one-pixel paper border.
        std::unordered_map<uint32_t,unsigned> paperColors,workspaceColors;
        const RectD bounds=preview.paper.bounds;
        for(int row=0;row<24;++row) for(int col=0;col<24;++col) {
            const int x=pane.l+(pane.r-pane.l)*(2*col+1)/48;
            const int y=pane.t+(pane.b-pane.t)*(2*row+1)/48;
            const double px=x-pane.l,py=y-pane.t;
            const uint32_t color=static_cast<const uint32_t*>(snapshot.bits)[size_t(y)*snapshot.w+x]&0xffffff;
            if(px>bounds.l+2&&px<bounds.r-2&&py>bounds.t+2&&py<bounds.b-2) ++paperColors[color];
            else if(px<bounds.l-2||px>bounds.r+2||py<bounds.t-2||py>bounds.b+2) ++workspaceColors[color];
        }
        auto blankColor=[](const auto& colors,uint32_t fallback) {
            unsigned count{};for(const auto& entry:colors)
                if(entry.second>count) { count=entry.second;fallback=entry.first; }
            return fallback;
        };
        preview.paper.paper=blankColor(paperColors,preview.paper.paper);
        preview.paper.workspace=blankColor(workspaceColors,preview.paper.workspace);
        canvasPapers[w]=preview.paper;
        }
        auto fullPage=gpu&&gpu->usable()?captureWholePage(w,doc,pane,preview.paper):nullptr;
        // Undo has changed the content. Never animate the pre-undo resident
        // scene if a fresh vector capture could not be obtained.
        if(fresh&&gpu&&gpu->usable()&&!fullPage) return false;
        if(gpu&&gpu->begin(snapshot.bits,snapshot.w*4,pane,preview.paper,preview.initialView,std::move(fullPage))) {
            preview.gpu=gpu;
            // Bounds, scale and origin must come from the same immutable scene,
            // even if a fresh native camera has not published its repaint yet.
            preview.sourceOrigin=preview.paper.sourceOrigin;preview.sourceUnits=preview.paper.sourceUnits;
        }
        preview.paperBrush.SetColor(Gdiplus::Color(0xff000000|preview.paper.paper));
        preview.workspaceBrush.SetColor(Gdiplus::Color(0xff000000|preview.paper.workspace));
        auto colorRef=[](uint32_t rgb) { return RGB((rgb>>16)&255,(rgb>>8)&255,rgb&255); };
        preview.workspaceGdi=CreateSolidBrush(colorRef(preview.paper.workspace));
        preview.paperGdi=CreateSolidBrush(colorRef(preview.paper.paper));
    }
    return copied&&(preview.gpu||preview.prepareCpu());
}
static void alignGpuToNative(Obj doc,NavigationFrame& preview,RectI pane) {
    Obj page=mainPage(doc),port=at<Obj>(doc,0x258);
    Obj scale=page?at<Obj>(page,0x2a8):nullptr;
    if(!scale||!port||!preview.gpu) return;
    const double units=at<double>(scale,8);
    if(!std::isfinite(units)||units<=0) { preview.gpu->hide();return; }
    // Keep the continuous affine mapping here. Native vector coordinates round
    // each point separately; rounding our origin would shift the whole bitmap.
    const double x=(preview.sourceOrigin.x-at<double>(scale,0x20))/units+
        at<int>(scale,0x10)-at<int>(port,0xa8)-pane.l;
    const double y=(preview.sourceOrigin.y-at<double>(scale,0x28))/units+
        at<int>(scale,0x14)-at<int>(port,0xac)-pane.t;
    const double ratio=preview.sourceUnits/units;
    preview.gpu->move(x,y,(pane.r-pane.l)*ratio,(pane.b-pane.t)*ratio);
}
static void presentNavigation(HWND w,NavigationFrame& preview,RectI pane,HDC supplied=nullptr) {
    auto& presented=preview.presented;
    if(preview.gpu&&preview.gpu->usable()) {
        // Keep the previous composition frame while its successor is prepared.
        // CPU resampling here stalls input before the worker can present.
        // The compositor retains a fixed UI layer. Never restore old native
        // snapshot strips over current ruler, margin or control drawing.
        return;
    }
    if(!preview.prepareCpu()) return;
    HDC dc=supplied?supplied:GetDC(w); if(!dc) return;
    if(supplied) BitBlt(dc,0,0,presented.w,presented.h,presented.dc,0,0,SRCCOPY);
    else BitBlt(dc,pane.l,pane.t,pane.r-pane.l,pane.b-pane.t,presented.dc,pane.l,pane.t,SRCCOPY);
    if(!supplied) ReleaseDC(w,dc);
}
static bool renderZoom(NavigationFrame& preview,RectI pane,
        double left,double top,double width,double height) {
    if(!std::isfinite(left+top+width+height)||width<=0||height<=0) return false;
    const Gdiplus::RectF next{float(left),float(top),float(width),float(height)};
    const auto& previous=preview.drawn;
    if(preview.hasZoomFrame&&next.X==previous.X&&next.Y==previous.Y&&
        next.Width==previous.Width&&next.Height==previous.Height) return false;
    auto& graphics=*preview.graphics;
    // Keep the non-canvas pixels captured at gesture start. Clear and resample
    // only the canvas, using persistent graphics over the DIB's pixel memory.
    graphics.FillRectangle(&preview.workspaceBrush,pane.l,pane.t,pane.r-pane.l,pane.b-pane.t);
    const auto paper=preview.paper.bounds;
    const double sx=width/(pane.r-pane.l),sy=height/(pane.b-pane.t);
    const Gdiplus::RectF page{float(left+paper.l*sx),float(top+paper.t*sy),
        float((paper.r-paper.l)*sx),float((paper.b-paper.t)*sy)};
    graphics.FillRectangle(&preview.paperBrush,page);
    const auto clipped=graphics.Save();
    graphics.SetClip(page,Gdiplus::CombineModeIntersect);
    const auto& initial=preview.initialView;
    const double ix=width/initial.w,iy=height/initial.h;
    const Gdiplus::RectF image{float(left-initial.x*ix),float(top-initial.y*iy),
        float((pane.r-pane.l)*ix),float((pane.b-pane.t)*iy)};
    graphics.DrawImage(preview.sourceImage.get(),image,float(pane.l),float(pane.t),
        float(pane.r-pane.l),float(pane.b-pane.t),Gdiplus::UnitPixel);
    graphics.Restore(clipped);
    graphics.DrawRectangle(&preview.borderPen,page);
    graphics.Flush(Gdiplus::FlushIntentionSync);
    preview.drawn=next;preview.hasZoomFrame=true;
    return true;
}
static bool renderPan(NavigationFrame& preview,RectI pane,double movementX,double movementY) {
    const int width=pane.r-pane.l,height=pane.b-pane.t;
    const int dx=int(std::lround(std::clamp(movementX,-double(width),double(width))));
    const int dy=int(std::lround(std::clamp(movementY,-double(height),double(height))));
    if(preview.hasPanFrame&&dx==preview.panX&&dy==preview.panY) return false;
    auto& target=preview.presented;
    RECT viewport=winRect(pane);
    FillRect(target.dc,&viewport,preview.workspaceGdi);
    const auto paper=preview.paper.bounds;
    const auto& initial=preview.initialView;
    const double sx=initial.w/width,sy=initial.h/height;
    const RectI paperRect{pane.t+dy+int(std::floor(initial.y+paper.t*sy)),
        pane.l+dx+int(std::floor(initial.x+paper.l*sx)),
        pane.t+dy+int(std::ceil(initial.y+paper.b*sy)),
        pane.l+dx+int(std::ceil(initial.x+paper.r*sx))};
    RECT blank=winRect(intersect(pane,paperRect));
    if(blank.right>blank.left&&blank.bottom>blank.top) FillRect(target.dc,&blank,preview.paperGdi);
    const RectI shifted{pane.t+dy,pane.l+dx,pane.b+dy,pane.r+dx};
    const RectI covered=intersect(intersect(pane,shifted),paperRect);
    if(valid(covered)) {
        // Panning changes position, not scale. A direct copy avoids resampling
        // and does not blur the cached page. Fractional input remains in state.
        BitBlt(target.dc,covered.l,covered.t,covered.r-covered.l,covered.b-covered.t,
            preview.snapshot.dc,covered.l-dx,covered.t-dy,SRCCOPY);
    }
    preview.panX=dx;preview.panY=dy;preview.hasPanFrame=true;
    return true;
}
static double animationBlend(ULONGLONG& lastFrame,ULONGLONG now) {
    const double elapsed=double(now-lastFrame);lastFrame=now;
    return 1.0-std::exp(-elapsed/24.0);
}
static void publishZoom(ZoomState& state,double cursorX,double cursorY) {
    const double width=state.pane.r-state.pane.l,height=state.pane.b-state.pane.t;
    const double ratio=state.initial/state.target;
    state.targetView=constrainPreview(anchorPreview(state.shownView,cursorX,cursorY,width*ratio,height*ratio),
        state.preview.paper,width,height);
    if(state.preview.gpu&&state.preview.gpu->usable())
        state.preview.gpu->zoomAt(cursorX,cursorY,width*ratio,height*ratio);
}
static void publishPan(PanState& state) {
    const auto& initial=state.preview.initialView;
    if(state.preview.gpu) state.preview.gpu->move(initial.x+state.baseX-state.targetX,
        initial.y+state.baseY-state.targetY,initial.w,initial.h);
}
static void drawZoom(HWND w,ZoomState& state,HDC supplied) {
    if(state.preview.gpu&&state.preview.gpu->usable()) {
        if(supplied) presentNavigation(w,state.preview,state.pane,supplied);
        return;
    }
    if(!state.cpuStarted) {
        if(state.preview.gpu) state.preview.gpu->view(state.shownView,state.targetView);
        state.cpuStarted=true;
    }
    const auto now=GetTickCount64();
    if(now-state.lastFrame<frameMs) {
        if(supplied) presentNavigation(w,state.preview,state.pane,supplied);
        return;
    }
    const double width=state.pane.r-state.pane.l,height=state.pane.b-state.pane.t;
    const double blend=animationBlend(state.lastFrame,now);
    state.shownView=easePreview(state.shownView,state.targetView,blend);
    state.shownView=constrainPreview(state.shownView,state.preview.paper,width,height);
    const RectI pane=state.pane;
    if(!state.preview.prepareZoom(pane)) return;
    const auto& view=state.shownView;
    if(renderZoom(state.preview,pane,pane.l+view.x,pane.t+view.y,view.w,view.h)||supplied)
        presentNavigation(w,state.preview,pane,supplied);
    displayWorkspaceBars(w,state.doc,pane,state.preview.paper,view);
}
static void finishZoom(HWND w) {
    auto it=zooms.find(w); if(it==zooms.end()) return;
    auto state=std::move(it->second); zooms.erase(it); KillTimer(w,zoomTimer);
    clearBondPlacement(w);ghostInputs.erase(w);bumpGeneration();
    PreviewView shown=state->shownView,target=state->targetView;
    if(state->preview.gpu) state->preview.gpu->view(shown,target);
    const double width=state->pane.r-state->pane.l,height=state->pane.b-state->pane.t;
    // Native DoZoom centers its argument. Supply the document point that lies
    // at the center of the anchored target, so its result matches GPU pixels.
    state->center={state->preview.sourceOrigin.x+(width*0.5-target.x)*width/target.w*state->initial,
        state->preview.sourceOrigin.y+(height*0.5-target.y)*height/target.h*state->initial,0};
    { CounterScope committing(cameraCommitDepth);CenterCommit camera(state->doc,state->center);
      oldZoom(state->doc,&state->center,state->target,state->refresh); }
    refreshWorkspace(state->doc);
    invalidateCanvas(w);
    handoffGpu(w,state->doc,state->preview,state->pane);
}
static void CALLBACK zoomTick(HWND w,UINT,UINT_PTR id,DWORD) {
    // On the GPU this timer only detects input inactivity. Easing and frame
    // pacing belong entirely to the display-ready rendering worker.
    KillTimer(w,id);
    auto it=zooms.find(w);if(it==zooms.end()) return;
    if(!IsWindow(w)) { forgetPresentation(w);return; }
    if(it->second->epoch!=generation.load(std::memory_order_relaxed)) { finishZoom(w);return; }
    const auto& s=*it->second;
    const auto idle=GetTickCount64()-s.lastInput;
    UINT delay{};
    if(s.preview.gpu&&s.preview.gpu->usable()) {
        if(idle<zoomSettleMs) delay=UINT(zoomSettleMs-idle);
        else if(s.preview.gpu->settled()) { finishZoom(w);return; }
        else return; // The worker posts completion when the latest target settles.
    } else {
        // GDI compatibility rendering still needs UI-thread paint scheduling.
        drawZoom(w,*it->second);
        const auto& a=s.shownView;const auto& b=s.targetView;
        const bool settled=std::max({std::abs(a.x-b.x),std::abs(a.y-b.y),std::abs(a.w-b.w),std::abs(a.h-b.h)})<0.25;
        if(idle>=zoomSettleMs&&(settled||idle>=240)) { finishZoom(w);return; }
        delay=UINT(frameMs);
    }
    if(!SetTimer(w,zoomTimer,delay,zoomTick)) finishZoom(w);
}
void prepareUndoCamera(Obj doc) {
    const HWND w=windowFor(doc);if(!w) return;
    PreviewView shown{};PreviewPaper paper{};RectI pane{};
    const auto preview=gpuPreviews.find(w);
    if(preview!=gpuPreviews.end()&&preview->second&&preview->second->freezeCamera(shown,paper,pane)) {
        // Use the same immutable camera/scene pair that reached the display.
        // Committing the outstanding target here used to move the camera even
        // when the object was already visible in the current animation frame.
        const double width=pane.r-pane.l,height=pane.b-pane.t;
        const double scale=paper.sourceUnits*width/shown.w;
        const Point center{paper.sourceOrigin.x+(width*0.5-shown.x)*scale,
            paper.sourceOrigin.y+(height*0.5-shown.y)*paper.sourceUnits*height/shown.h,0};
        const Point origin{paper.sourceOrigin.x-shown.x*scale,
            paper.sourceOrigin.y-shown.y*paper.sourceUnits*height/shown.h,0};
        const auto native=canvasPaper(w,doc,pane);
        const bool alreadyCommitted=native.sourceUnits>0&&
            std::abs(native.sourceUnits/scale-1)<1.0e-10&&
            std::abs(native.sourceOrigin.x-origin.x)/scale<1.0e-6&&
            std::abs(native.sourceOrigin.y-origin.y)/scale<1.0e-6;
        KillTimer(w,zoomTimer);KillTimer(w,panTimer);
        zooms.erase(w);pans.erase(w);panRemainders.erase(w);gpuHandoffs.erase(w);
        if(!alreadyCommitted) {
            CounterScope preparing(historyPrepareDepth);CounterScope committing(cameraCommitDepth);
            CenterCommit camera(doc,center);oldZoom(doc,&center,scale,true);
            // ZoomChanged's integer scroll deltas lose the subpixel part of
            // the frozen display origin. Retain it through the native offset
            // setter after normal native sizing/cache invalidation has run.
            Obj page=mainPage(doc),ds=page?at<Obj>(page,0x2a8):nullptr,port=at<Obj>(doc,0x258);
            if(ds&&port) {
                const auto viewport=canvasViewport(w,doc);
                const double units=at<double>(ds,8);
                const double x=origin.x-(viewport.l+at<int>(port,0xa8)-at<int>(ds,0x10))*units;
                const double y=origin.y-(viewport.t+at<int>(port,0xac)-at<int>(ds,0x14))*units;
                if(std::isfinite(x+y)&&std::isfinite(units)&&units>0) {
                    fn<void(*)(Obj,double,double)>(0x1bb5e0)(ds,x,y);
                    vf<void(*)(Obj)>(doc,0x110)(doc);vf<void(*)(Obj)>(doc,0x118)(doc);
                }
            }
        }
        refreshWorkspace(doc);invalidateCanvas(w);
    } else {
        CounterScope preparing(historyPrepareDepth);finishZoom(w);finishPan(w);
    }
    undoCameras.erase(w);
    cancelGhostGesture(w);clearGhost(w);clearFeedback(w,true);
    placementHighlights.erase(w);
    // Undo supersedes a pending release preview. Keep the last complete image
    // visible, but allow a fresh post-undo whole-page capture to replace it.
    beginGpuDrawing(doc);
    if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)
        gpu->second->holdScenes(true);
    KillTimer(w,drawingCommitTimer);
    cancelPlacement(w);
    flushPlacementDamage();
}
void queueUndoCamera(Obj doc,Obj page,const RectD& bounds,bool reveal) {
    const HWND w=windowFor(doc);if(!w) return;
    try {
        undoCameras.insert_or_assign(w,UndoCameraRequest{doc,page,bounds,false,reveal&&valid(bounds),false});
        postUndoCamera(w);
    } catch(const std::bad_alloc&) {
        undoCameras.erase(w);
        if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second) {
            gpu->second->holdScenes(false,false);gpu->second->finish();
        }
        invalidateCanvas(w);
    }
}
static void focusUndoCamera(HWND w) {
    auto found=undoCameras.find(w);if(found==undoCameras.end()) return;
    found->second.posted=false;
    if(dispatchWindow==w||trackingDepth) return;
    const auto request=found->second;
    // Resolve the live document from the HWND before touching native memory;
    // queued requests can outlive document closure or a page switch.
    Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(!doc||doc!=request.doc||!request.page||windowFor(doc)!=w||mainPage(doc)!=request.page||!IsWindowVisible(w)) {
        undoCameras.erase(found);
        if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)gpu->second->holdScenes(false,false);
        return;
    }
    // Never manufacture native idle or capture partially validated geometry.
    // A completed window idle reposts this request if undo left work pending.
    if(at<unsigned char>(doc,0x40c)||idleDepth||paintDepth||gpuTrackingActive()) {
        found->second.awaitingIdle=true;return;
    }
    undoCameras.erase(found);
    finishZoom(w);finishPan(w);
    Obj ds=at<Obj>(request.page,0x2a8);
    const auto pane=canvasViewport(w,doc);
    const double width=pane.r-pane.l,height=pane.b-pane.t;
    const double current=ds?at<double>(ds,8):0;
    if(!valid(pane)||!std::isfinite(current)||current<=0) {
        if(auto preview=gpuPreviews.find(w);preview!=gpuPreviews.end()&&preview->second)
            preview->second->holdScenes(false,false);
        invalidateCanvas(w);return;
    }
    const auto paper=canvasPaper(w,doc,pane);
    auto previewEntry=gpuPreviews.find(w);
    std::shared_ptr<const GpuScene> fresh;
    bool published{};
    if(previewEntry!=gpuPreviews.end()&&previewEntry->second&&previewEntry->second->usable()) {
        // History content uses the same native port/cache transfer as ordinary
        // drawing. Switching to direct full-page drawing on the first undo
        // changed the pixel origin/coverage of every unchanged object.
        struct HistoryPublication {
            Obj previousDoc{historyPublishDoc};bool previousPublished{historyPublished};
            explicit HistoryPublication(Obj doc) {historyPublishDoc=doc;historyPublished=false;}
            ~HistoryPublication() {historyPublishDoc=previousDoc;historyPublished=previousPublished;}
        } publishing(doc);
        redrawCanvas(w);published=historyPublished;
    }
    if(!published&&previewEntry!=gpuPreviews.end()&&previewEntry->second&&previewEntry->second->usable()) {
        fresh=captureWholePage(w,doc,pane,paper);
        published=fresh&&previewEntry->second->scene(fresh,pane,paper,true);
    }
    if(published)gpuHandoffs.erase(w);
    else {
        if(previewEntry!=gpuPreviews.end()&&previewEntry->second) {
            previewEntry->second->holdScenes(false,false);previewEntry->second->finish();gpuHandoffs[w]=previewEntry->second;
        }
        redrawCanvas(w);
    }
    if(!request.reveal)return;
    const auto& bounds=request.bounds;
    Obj port=at<Obj>(doc,0x258);
    const Point origin{fn<double(*)(Obj,double)>(0x3c5d80)(ds,double(pane.l)+at<int>(port,0xa8)),
        fn<double(*)(Obj,double)>(0x3c5da0)(ds,double(pane.t)+at<int>(port,0xac)),0};
    // Reveal only an entirely offscreen target. Full containment wrongly
    // treated partly visible objects as absent. One physical pixel absorbs
    // the native coordinate rounding at the edge without imposing padding.
    if(bounds.r>=origin.x-current&&bounds.b>=origin.y-current&&
        bounds.l<=origin.x+(width+1)*current&&bounds.t<=origin.y+(height+1)*current)return;
    const double fit=std::max((bounds.r-bounds.l)/width,(bounds.b-bounds.t)/height);
    const double normal=*reinterpret_cast<double*>(base+0xb3ea10);
    const int maxPercent=*reinterpret_cast<int*>(base+0xb3f344);
    const int minPercent=*reinterpret_cast<int*>(base+0xb3f320);
    if(maxPercent<=0||minPercent<=0||!std::isfinite(normal)||normal<=0||!std::isfinite(fit+origin.x+origin.y)) return;
    const double minimum=normal*100.0/maxPercent,maximum=normal*100.0/minPercent;
    if(maximum<minimum) return;
    // Zoom out only if the object itself cannot fit. Padding shrinks for
    // almost viewport-sized objects rather than forcing an extra zoom change.
    const double scale=std::clamp(fit>current?fit/0.9:current,minimum,maximum);
    auto revealAxis=[](double originValue,double viewport,double oldScale,double newScale,double low,double high) {
        const double extent=viewport*newScale;
        const double spare=extent-(high-low);
        if(spare<0) return low+(high-low-extent)*0.5; // Native minimum zoom limit.
        const double pad=std::min(viewport*0.05*newScale,spare*0.5);
        // Keep the current center when changing scale, then choose the nearest
        // permitted origin. A pan moves only the overflowing axis and distance.
        return std::clamp(originValue+viewport*(oldScale-newScale)*0.5,
            high-extent+pad,low-pad);
    };
    const Point targetOrigin{revealAxis(origin.x,width,current,scale,bounds.l,bounds.r),
        revealAxis(origin.y,height,current,scale,bounds.t,bounds.b),0};
    const Point center{targetOrigin.x+width*scale*0.5,targetOrigin.y+height*scale*0.5,0};
    auto nativeFocus=[&] {
        CounterScope committing(cameraCommitDepth);CenterCommit camera(doc,center);
        oldZoom(doc,&center,scale,true);
        // Native DoZoom centers even when the scale is unchanged.
        refreshWorkspace(doc);invalidateCanvas(w);
    };
    try {
        auto state=std::make_unique<ZoomState>();state->doc=doc;state->center=center;state->target=scale;state->refresh=true;
        if(!published||!captureNavigation(w,doc,state->pane,state->preview,true,fresh)||!state->preview.gpu||!state->preview.gpu->usable()) {
            if(state->preview.gpu) state->preview.gpu->hide();
            nativeFocus();return;
        }
        state->initial=state->preview.sourceUnits;
        state->shownView=state->preview.initialView;
        // Minimal reveal target, independent of the wheel cursor. The center
        // here describes the resulting camera, not the center of the object.
        state->targetView=constrainPreview({width*0.5-(center.x-state->preview.sourceOrigin.x)/scale,
            height*0.5-(center.y-state->preview.sourceOrigin.y)/scale,
            width*state->initial/scale,height*state->initial/scale},state->preview.paper,width,height);
        state->lastInput=GetTickCount64();state->lastFrame=state->lastInput;
        state->epoch=generation.load(std::memory_order_relaxed);
        const auto target=state->targetView;
        auto gpu=state->preview.gpu;
        zooms.insert_or_assign(w,std::move(state));
        gpu->move(target.x,target.y,target.w,target.h);
        if(!SetTimer(w,zoomTimer,UINT(zoomSettleMs),zoomTick)) finishZoom(w);
    } catch(const std::bad_alloc&) {
        finishZoom(w);
        if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second) gpu->second->hide();
        nativeFocus();
    }
}
static void requestZoom(Obj doc,const Point* center,double scale,bool refresh) {
    if(!onUI()||!zoomDispatch||trackingDepth||!center||!std::isfinite(scale)||scale<=0) {
        oldZoom(doc,center,scale,refresh); return;
    }
    HWND w=windowFor(doc); Obj page=mainPage(doc);
    if(!w||!page||!IsWindowVisible(w)) { oldZoom(doc,center,scale,refresh); return; }
    finishPan(w);
    try {
        auto it=zooms.find(w);
        if(it==zooms.end()) {
            clearBondPlacement(w);ghostInputs.erase(w);
            auto state=std::make_unique<ZoomState>(); state->doc=doc;
            Obj ds=at<Obj>(page,0x2a8); state->initial=at<double>(ds,8);
            if(!captureNavigation(w,doc,state->pane,state->preview)||
                (!state->preview.gpu&&!state->preview.prepareZoom(state->pane))) {
                if(state->preview.gpu) state->preview.gpu->hide();
                oldZoom(doc,center,scale,refresh); return;
            }
            state->initial=state->preview.sourceUnits;
            state->shownView=state->targetView=state->preview.initialView;
            state->lastFrame=GetTickCount64()-frameMs;
            it=zooms.emplace(w,std::move(state)).first;
        }
        auto& state=*it->second;
        state.center=*center;state.target=scale;state.refresh=refresh;
        state.lastInput=GetTickCount64();state.epoch=generation.load(std::memory_order_relaxed);
        double cursorX=(state.pane.r-state.pane.l)*0.5,cursorY=(state.pane.b-state.pane.t)*0.5;
        if(haveWheelCursor&&wheelCursor.x>=state.pane.l&&wheelCursor.x<state.pane.r&&
            wheelCursor.y>=state.pane.t&&wheelCursor.y<state.pane.b) {
            cursorX=wheelCursor.x-state.pane.l;cursorY=wheelCursor.y-state.pane.t;
        }
        publishZoom(state,cursorX,cursorY);
        const bool gpuZoom=state.preview.gpu&&state.preview.gpu->usable();
        if(!gpuZoom) drawZoom(w,state);
        // Every delta updates the ongoing animation immediately. Restart only
        // the idle/commit deadline; never start a new timed animation segment.
        if(!SetTimer(w,zoomTimer,UINT(gpuZoom?zoomSettleMs:frameMs),zoomTick)) finishZoom(w);
    } catch(const std::bad_alloc&) {
        finishZoom(w);
        if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second) gpu->second->hide();
        oldZoom(doc,center,scale,refresh);
    }
}
static bool magnify(Obj doc,const Point* center,Magnify original) {
    if(!onUI()||!zoomDispatch||trackingDepth||!center) return original(doc,center);
    HWND w=windowFor(doc);auto it=zooms.find(w);Obj page=mainPage(doc);
    if(!page) return original(doc,center);
    const double current=it==zooms.end()?at<double>(at<Obj>(page,0x2a8),8):it->second->target;
    const double normal=*reinterpret_cast<double*>(base+0xb3ea10);
    const double minimum=normal*100.0/double(*reinterpret_cast<int*>(base+0xb3f344));
    const double maximum=normal*100.0/double(*reinterpret_cast<int*>(base+0xb3f320));
    if(!std::isfinite(current)||current<=0||minimum<=0||maximum<minimum) return original(doc,center);
    // Use every wheel fraction instead of choosing the next percentage preset.
    const double next=std::clamp(current*std::exp(-wheelNotches*0.14),minimum,maximum);
    if(next==current) return false;
    requestZoom(doc,center,next,true);
    return true;
}
static bool zoomIn(Obj d,const Point* p) { return magnify(d,p,oldZoomIn); }
static bool zoomOut(Obj d,const Point* p) { return magnify(d,p,oldZoomOut); }
static void drawPan(HWND w,PanState& state,HDC supplied) {
    if(state.preview.gpu&&state.preview.gpu->usable()) {
        if(supplied) presentNavigation(w,state.preview,state.pane,supplied);
        return;
    }
    const auto now=GetTickCount64();
    if(now-state.lastFrame<frameMs) {
        if(supplied) presentNavigation(w,state.preview,state.pane,supplied);
        return;
    }
    const double blend=animationBlend(state.lastFrame,now);
    state.shownX+=(state.targetX-state.shownX)*blend;
    state.shownY+=(state.targetY-state.shownY)*blend;
    const RectI pane=state.pane;
    if(!state.preview.prepareCpu()) return;
    if(renderPan(state.preview,pane,state.baseX-state.shownX,state.baseY-state.shownY)||supplied)
        presentNavigation(w,state.preview,pane,supplied);
    const auto initial=state.preview.initialView;
    displayWorkspaceBars(w,state.doc,pane,state.preview.paper,
        {initial.x+state.baseX-state.shownX,initial.y+state.baseY-state.shownY,initial.w,initial.h});
}
static bool scrollInfo(Obj scrollbar,SCROLLINFO& info) {
    info={sizeof(SCROLLINFO),SIF_RANGE|SIF_PAGE|SIF_POS};
    return scrollbar&&GetScrollInfo(at<HWND>(scrollbar,0),SB_CTL,&info)!=FALSE;
}
static void finishPan(HWND w) {
    auto it=pans.find(w);if(it==pans.end()) return;
    auto state=std::move(it->second);pans.erase(it);KillTimer(w,panTimer);
    SCROLLINFO horizontal{},vertical{};
    const bool hasX=scrollInfo(state->horizontal,horizontal),hasY=scrollInfo(state->vertical,vertical);
    const int x=int(std::lround(state->targetX)),y=int(std::lround(state->targetY));
    if(hasX) SetScrollPos(at<HWND>(state->horizontal,0),SB_CTL,x,TRUE);
    if(hasY) SetScrollPos(at<HWND>(state->vertical,0),SB_CTL,y,TRUE);
    // Retain subpixel input across separate bursts instead of throwing it away.
    panRemainders[w]={hasX?state->targetX-x:0,hasY?state->targetY-y:0};
    const int dx=int(std::lround(state->targetX-state->baseX));
    const int dy=int(std::lround(state->targetY-state->baseY));
    fn<void(*)(Obj,Obj)>(0x157d80)(state->doc,nullptr);
    { CounterScope committing(cameraCommitDepth);scrolling(state->doc,nullptr,&dx,&dy); }
    Obj page=mainPage(state->doc);
    if(page) fn<void(*)(Obj)>(0x715aa0)(page);
    fn<void(*)()>(0x59ff00)();
    bumpGeneration();invalidateCanvas(w);
    handoffGpu(w,state->doc,state->preview,state->pane);
}
static void CALLBACK panTick(HWND w,UINT,UINT_PTR id,DWORD) {
    KillTimer(w,id);
    auto it=pans.find(w);if(it==pans.end()) { KillTimer(w,id);return; }
    if(!IsWindow(w)) { forgetPresentation(w);return; }
    if(it->second->epoch!=generation.load(std::memory_order_relaxed)) { finishPan(w);return; }
    const auto& s=*it->second;
    const auto idle=GetTickCount64()-s.lastInput;
    UINT delay{};
    if(s.preview.gpu&&s.preview.gpu->usable()) {
        if(idle<zoomSettleMs) delay=UINT(zoomSettleMs-idle);
        else if(s.preview.gpu->settled()) { finishPan(w);return; }
        else return;
    } else {
        drawPan(w,*it->second);
        const bool settled=std::abs(s.targetX-s.shownX)<0.25&&std::abs(s.targetY-s.shownY)<0.25;
        if(idle>=zoomSettleMs&&(settled||idle>=240)) { finishPan(w);return; }
        delay=UINT(frameMs);
    }
    if(!SetTimer(w,panTimer,delay,panTick)) finishPan(w);
}
static bool wheelPan(HWND w,UINT message,WPARAM parameter) {
    if(trackingDepth) return false;
    Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(!doc||windowFor(doc)!=w||!mainPage(doc)) return false;
    const int delta=short(HIWORD(parameter));if(!delta) return true;
    const bool horizontal=message==WM_MOUSEHWHEEL||(LOWORD(parameter)&MK_SHIFT)!=0;
    finishZoom(w);refreshWorkspace(doc);
    Obj scrollbar=at<Obj>(doc,horizontal?0x560:0x570);
    SCROLLINFO info{};if(!scrollInfo(scrollbar,info)) return false;
    const double low=info.nMin,high=std::max(double(info.nMin),
        double(info.nMax)-std::max(0.0,double(info.nPage)-1.0));
    if(high<=low) return true;
    UINT lines=3;
    SystemParametersInfoW(horizontal?SPI_GETWHEELSCROLLCHARS:SPI_GETWHEELSCROLLLINES,0,&lines,0);
    if(!lines) return true;
    try {
        auto it=pans.find(w);
        if(it==pans.end()) {
            auto state=std::make_unique<PanState>();state->doc=doc;
            state->horizontal=at<Obj>(doc,0x560);state->vertical=at<Obj>(doc,0x570);
            SCROLLINFO x{},y{};
            if(scrollInfo(state->horizontal,x)) state->baseX=x.nPos;
            if(scrollInfo(state->vertical,y)) state->baseY=y.nPos;
            state->shownX=state->targetX=state->baseX;state->shownY=state->targetY=state->baseY;
            auto remainder=panRemainders.find(w);
            if(remainder!=panRemainders.end()) {
                state->targetX+=remainder->second.first;state->targetY+=remainder->second.second;
            }
            clearBondPlacement(w);ghostInputs.erase(w);
            if(!captureNavigation(w,doc,state->pane,state->preview)) return false;
            state->lastFrame=GetTickCount64()-frameMs;
            it=pans.emplace(w,std::move(state)).first;
        }
        auto& s=*it->second;
        UINT dpi=GetDpiForWindow(w);if(!dpi) dpi=GetDpiForSystem();
        const double lineExtent=double(GetSystemMetricsForDpi(horizontal?SM_CXSMICON:SM_CYSMICON,dpi))*0.625;
        const double distance=lines==WHEEL_PAGESCROLL?
            double(horizontal?s.pane.r-s.pane.l:s.pane.b-s.pane.t):lineExtent*double(lines);
        const double movement=double(delta)/WHEEL_DELTA*distance*(message==WM_MOUSEHWHEEL?1.0:-1.0);
        double& target=horizontal?s.targetX:s.targetY;
        target=std::clamp(target+movement,low,high);
        s.lastInput=GetTickCount64();s.epoch=generation.load(std::memory_order_relaxed);
        publishPan(s);
        if(s.lastInput-s.lastFrame>=frameMs) drawPan(w,s);
        const bool gpuPan=s.preview.gpu&&s.preview.gpu->usable();
        if(!SetTimer(w,panTimer,UINT(gpuPan?zoomSettleMs:frameMs),panTick)) finishPan(w);
        return true;
    } catch(const std::bad_alloc&) {
        finishPan(w);
        if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second) gpu->second->hide();
        return false;
    }
}
static LRESULT WINAPI documentProc(HWND w,UINT m,WPARAM a,LPARAM b) {
    if(!onUI()) return oldWndProc(w,m,a,b);
    // Clear outgoing hover before native input/idle work can delay departure.
    // Hit-test only the current object or copied placement circles; no page
    // traversal or chemistry work is required to remove a highlight.
    if(m==WM_MOUSEMOVE) {
        Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(doc&&windowFor(doc)==w)refreshHighlight(doc);
    } else if(m==WM_MOUSELEAVE&&!trackingDepth&&!gpuTrackingActive()&&!GetCapture()&&
        !((GetAsyncKeyState(VK_LBUTTON)|GetAsyncKeyState(VK_RBUTTON)|GetAsyncKeyState(VK_MBUTTON))&0x8000)) {
        clearHoverHighlight(w);
    }
    Obj placementDoc=(m==WM_LBUTTONDOWN||m==WM_LBUTTONUP)?
        fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA)):nullptr;
    PlacementInputScope placementInput(w,placementDoc,m);
    PlacementTimer inputTimer(PlacementCost::Input);
    if(m==WM_LBUTTONUP) placementReleased(w);
    if(m==WM_CANCELMODE||m==WM_CLOSE||m==WM_NCDESTROY||(m==WM_COMMAND&&!trackingDepth)||
        m==WM_MDIACTIVATE||(m==WM_KILLFOCUS&&!trackingDepth)||m==WM_KEYDOWN||m==WM_SYSKEYDOWN)
        cancelPlacement(w);
    if(m==WM_LBUTTONUP) if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)
        gpu->second->startGhostRelease();
    if(m==WM_TIMER&&a==drawingCommitTimer) {
        if(drawingCommits.contains(w)) completeDrawingCommit(w);
        else KillTimer(w,drawingCommitTimer);
        return 0;
    }
    if(m==WM_LBUTTONDOWN||m==WM_RBUTTONDOWN||m==WM_MBUTTONDOWN||m==WM_MOUSEWHEEL||m==WM_MOUSEHWHEEL||
        m==WM_HSCROLL||m==WM_VSCROLL||m==WM_CANCELMODE||m==WM_CLOSE||m==WM_SIZE||(m==WM_COMMAND&&!trackingDepth)||
        m==WM_SYSCOMMAND||m==WM_KEYDOWN||m==WM_SYSKEYDOWN||m==WM_MDIACTIVATE||
        m==WM_DPICHANGED||m==WM_DISPLAYCHANGE||(m==WM_KILLFOCUS&&!trackingDepth)) cancelGhostGesture(w);
    MouseBondPlacementScope placement(w,m,b);
    if(m==WM_LBUTTONDOWN) {
        // Validate the actual submitted preview against live hit testing. Native
        // hover invalidations change the cache generation without changing what
        // is visible; a queued newer page must not prevent press ownership.
        if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second) {
            GhostBond shown{};
            Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
            Obj page=doc?mainPage(doc):nullptr,scale=page?at<Obj>(page,0x2a8):nullptr;
            Obj port=doc?at<Obj>(doc,0x258):nullptr;
            if(doc&&windowFor(doc)==w&&scale&&port&&gpu->second->displayedGhost(shown)&&
                !((GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_CONTROL)|GetAsyncKeyState(VK_MENU))&0x8000)) {
                try {
                    GhostTool tool{};
                    const Point mouse{fn<double(*)(Obj,double)>(0x3c5d80)(scale,double(short(LOWORD(b)))+at<int>(port,0xa8)),
                        fn<double(*)(Obj,double)>(0x3c5da0)(scale,double(short(HIWORD(b)))+at<int>(port,0xac)),0};
                    if(std::isfinite(mouse.x)&&std::isfinite(mouse.y)&&readGhostTool(page,tool)&&tool.key==shown.toolKey&&
                        !nativeArrowTool(tool.tool)) {
                        Obj target=findToolGhostTarget(page,mouse);
                        // Never dereference the old hover cache's native pointer.
                        const bool matches=shown.identity?target&&at<uint8_t>(target,0x34)&&
                            vf<bool(*)(Obj)>(target,0xb8)(target)&&at<uint64_t>(target,0xb0)+1==shown.identity:
                            !target&&std::hypot(mouse.x-shown.pressPoint.x,mouse.y-shown.pressPoint.y)<=at<double>(scale,8);
                        POINT pressed{LONG(short(LOWORD(b))),LONG(short(HIWORD(b)))};
                        if(matches&&ClientToScreen(w,&pressed)&&gpu->second->beginGhostPlacement(shown)) {
                            ghostGestures.insert_or_assign(w,GhostGesture{pressed});
                            UINT dpi=GetDpiForWindow(w);if(!dpi) dpi=GetDpiForSystem();
                            gpu->second->armPlacementRelease(placementRevision(w),pressed,
                                GetSystemMetricsForDpi(SM_CXDRAG,dpi),GetSystemMetricsForDpi(SM_CYDRAG,dpi));
                        }
                    }
                } catch(const std::bad_alloc&) { gpu->second->cancelGhostPlacement(); }
            }
        }
        Obj atom=mouseBondPlacementVertex();
        if(!atom&&ordinaryBondTool()) {
            // Highlight ownership does not depend on the optional ghost's
            // direction cache being populated before the button event.
            Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
            Obj candidate=doc?at<Obj>(doc,0x2e8):nullptr;RectD circle{};
            if(doc&&windowFor(doc)==w&&vertexHitCircle(candidate,circle)) {
                Obj scale=at<Obj>(candidate,0x68),port=at<Obj>(doc,0x258);
                const Point mouse{fn<double(*)(Obj,double)>(0x3c5d80)(scale,double(short(LOWORD(b)))+at<int>(port,0xa8)),
                    fn<double(*)(Obj,double)>(0x3c5da0)(scale,double(short(HIWORD(b)))+at<int>(port,0xac)),0};
                if(vf<bool(*)(Obj,const Point*)>(candidate,0x278)(candidate,&mouse)) atom=candidate;
            }
        }
        if(atom) if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second) {
            const auto identity=at<uint64_t>(atom,0xb0)+1;
            // Keep the same hover image and fade phase through native tracking.
            // Store before enabling ownership so an allocation failure cannot
            // leave native feedback suppressed without its GPU replacement.
            try {
                RectD circle{};vertexHitCircle(atom,circle);
                placementHighlights.insert_or_assign(w,PlacementHighlight{identity,0,circle});
                if(!gpu->second->beginPlacementHighlight(identity)) {
                    Obj page=at<Obj>(atom,0x60),doc=page?at<Obj>(page,8):nullptr;
                    MouseHighlightSnapshot state{doc,atom};HoverHighlight next{};
                    if(!doc||!captureMouseHighlight(&state,doc,w,identity,next,false)) placementHighlights.erase(w);
                    else {
                        gpu->second->highlight(next);
                        if(!gpu->second->beginPlacementHighlight(identity)) placementHighlights.erase(w);
                    }
                }
            } catch(const std::bad_alloc&) { placementHighlights.erase(w); }
        }
    }
    if(m==WM_LBUTTONDOWN||m==WM_RBUTTONDOWN||m==WM_MBUTTONDOWN||
        m==WM_MOUSELEAVE||m==WM_HSCROLL||m==WM_VSCROLL||m==WM_KILLFOCUS||m==WM_CANCELMODE||
        m==WM_CAPTURECHANGED||m==WM_COMMAND||m==WM_SYSCOMMAND||m==WM_CLOSE||m==WM_SIZE||
        m==WM_KEYDOWN||m==WM_KEYUP||m==WM_SYSKEYDOWN||m==WM_SYSKEYUP||m==WM_MDIACTIVATE||
        m==WM_DPICHANGED||m==WM_DISPLAYCHANGE) clearGhost(w);
    if(m==WM_MOUSELEAVE||m==WM_KILLFOCUS||m==WM_COMMAND||m==WM_MDIACTIVATE) clearFeedback(w);
    if((m==WM_LBUTTONDOWN&&!placementHighlights.contains(w))||m==WM_RBUTTONDOWN||m==WM_MBUTTONDOWN||
        m==WM_HSCROLL||m==WM_VSCROLL||m==WM_CANCELMODE||m==WM_SIZE||m==WM_CLOSE||
        m==WM_DPICHANGED||m==WM_DISPLAYCHANGE) clearFeedback(w,true);
    if(m&&m==drawingCommitMessage()) { completeDrawingCommit(w);return 0; }
    if(m&&m==undoCameraMessage()) { focusUndoCamera(w);return 0; }
    if(m&&m==gpuGhostCommittedMessage()) {
        if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)
            placementDisplayPresented(w,gpu->second->placementDisplayTiming());
        if(!ghostPlacementActive(w)) {
            ghostGestures.erase(w);
            Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
            if(doc&&windowFor(doc)==w) { finishPlacementHover(doc);refreshGhost(doc);refreshHighlight(doc); }
        }
        return 0;
    }
    if(m&&m==gpuSettledMessage()) { zoomTick(w,0,zoomTimer,0);panTick(w,0,panTimer,0);return 0; }
    if(m&&m==gpuCameraFrameMessage()) {
        auto found=gpuPreviews.find(w);
        PreviewView shown{};PreviewPaper paper{};RectI pane{};
        if(found!=gpuPreviews.end()&&found->second&&found->second->cameraFrame(shown,paper,pane,true)) {
            Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
            if(doc&&windowFor(doc)==w) {displayWorkspaceBars(w,doc,pane,paper,shown);refreshGhost(doc);}
        }
        return 0;
    }
    if(m&&m==gpuPagePresentedMessage()) {
        if(auto handoff=gpuHandoffs.find(w);handoff!=gpuHandoffs.end()&&handoff->second->nativeScenePresented())
            gpuHandoffs.erase(handoff);
        Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(doc&&windowFor(doc)==w) {refreshWorkspace(doc);refreshGhost(doc);}
        return 0;
    }
    if(m==WM_NCDESTROY) { forgetPresentation(w);bumpGeneration();return oldWndProc(w,m,a,b); }
    const bool wheelZoom=m==WM_MOUSEWHEEL&&fn<bool(*)(char)>(0x5be3c0)(0x12);
    const bool wheel=m==WM_MOUSEWHEEL||m==WM_MOUSEHWHEEL;
    if(wheel&&!wheelZoom&&wheelPan(w,m,a)) return 0;
    if(m==WM_ERASEBKGND&&(zooms.contains(w)||pans.contains(w))) return 1;
    if(m==WM_PAINT) {
        auto repaintNavigationUi=[&](Obj doc) {
            RECT damage{};
            if(GetUpdateRect(w,&damage,FALSE))paintFixedUi(w,doc,cdRect(damage));
        };
        auto it=zooms.find(w);
        if(it!=zooms.end()) { repaintNavigationUi(it->second->doc);PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);presentNavigation(w,it->second->preview,it->second->pane,dc);EndPaint(w,&ps);return 0; }
        auto pan=pans.find(w);
        if(pan!=pans.end()) { repaintNavigationUi(pan->second->doc);PAINTSTRUCT ps{};HDC dc=BeginPaint(w,&ps);presentNavigation(w,pan->second->preview,pan->second->pane,dc);EndPaint(w,&ps);return 0; }
    }
    const bool input=(m>=WM_KEYFIRST&&m<=WM_KEYLAST)||(m>=WM_MOUSEFIRST&&m<=WM_MOUSELAST)||
        m==WM_COMMAND||m==WM_SYSCOMMAND||m==WM_CLOSE||m==WM_SIZE||m==WM_HSCROLL||m==WM_VSCROLL||
        m==WM_KILLFOCUS||m==WM_CANCELMODE||m==WM_DPICHANGED||m==WM_DISPLAYCHANGE||m==WM_MDIACTIVATE;
    const bool historyKey=m==WM_KEYDOWN&&(a=='Z'||a=='Y')&&(GetAsyncKeyState(VK_CONTROL)&0x8000);
    if(input&&!wheelZoom&&m!=WM_MOUSEMOVE&&!historyKey) {
        finishZoom(w);finishPan(w);
        if(m!=WM_KEYUP&&m!=WM_SYSKEYUP) bumpGeneration();
    }
    if(input&&!trackingDepth) completeDrawingCommit(w);
    HWND saved=dispatchWindow;dispatchWindow=w;
    HWND savedUpdateWindow=updateWindow;const RectI savedUpdateBounds=updateBounds;
    if(m==WM_PAINT) {
        RECT region{};
        if(GetUpdateRect(w,&region,FALSE)) { updateWindow=w;updateBounds=cdRect(region); }
        else { updateWindow=nullptr;updateBounds={}; }
    }
    const double savedNotches=wheelNotches;const POINT savedCursor=wheelCursor;
    const bool savedHaveCursor=haveWheelCursor;
    if(wheelZoom) {
        wheelNotches=double(short(HIWORD(a)))/WHEEL_DELTA;
        // Wheel lParam contains signed screen coordinates, including monitors
        // left of the primary display. Use the event position, not a later poll.
        wheelCursor={LONG(short(LOWORD(b))),LONG(short(HIWORD(b)))};
        haveWheelCursor=ScreenToClient(w,&wheelCursor)!=FALSE;
    }
    if(wheelZoom) ++zoomDispatch;
    LRESULT result{};
    try { result=oldWndProc(w,m,a,b); }
    catch(...) { if(wheelZoom)--zoomDispatch;wheelNotches=savedNotches;wheelCursor=savedCursor;haveWheelCursor=savedHaveCursor;dispatchWindow=saved;
        updateWindow=savedUpdateWindow;updateBounds=savedUpdateBounds;throw; }
    if(wheelZoom) --zoomDispatch;wheelNotches=savedNotches;wheelCursor=savedCursor;haveWheelCursor=savedHaveCursor;dispatchWindow=saved;
    updateWindow=savedUpdateWindow;updateBounds=savedUpdateBounds;
    if((m==WM_LBUTTONDOWN||m==WM_LBUTTONUP)&&ghostGestures.contains(w)&&!trackingDepth&&!GetCapture()&&
        !(GetAsyncKeyState(VK_LBUTTON)&0x8000)) if(auto gpu=gpuPreviews.find(w);gpu!=gpuPreviews.end()&&gpu->second)
            gpu->second->startGhostRelease();
    if(m==WM_MOUSEMOVE) {
        Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(doc&&windowFor(doc)==w) {updateGpuGhostGesture(doc);refreshHighlight(doc);}
    }
    if(m==WM_LBUTTONDOWN&&ghostGestures.contains(w)&&!trackingDepth&&!GetCapture()&&
        !(GetAsyncKeyState(VK_LBUTTON)&0x8000)&&!drawingCommits.contains(w)) {
        Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(w,GWLP_USERDATA));
        if(doc&&windowFor(doc)==w) queueGpuDrawingCommit(doc);
    }
    if(!trackingDepth&&dispatchWindow!=w) postDrawingCommit(w);
    postUndoCamera(w);
    return result;
}
void installPresentation() {
    Gdiplus::GdiplusStartupInput input;
    if(Gdiplus::GdiplusStartup(&gdiplusToken,&input,nullptr)!=Gdiplus::Ok) throw std::runtime_error("GDI+ startup failed");
    installGpuCanvas();
    if (patchEnabled(4)) installBondPlacement();
    hook(0x5f84d0,paintDocument,oldPaint);
    hook(0x5f88d0,documentContents,oldContents);
    hook(0x5f92b0,paperBackground,oldPaperBackground);
    hook(0x645a30,offscreenValid,oldOffscreenValid);
    hook(0x19d130,hotKeyFeedback,oldHotKeyFeedback);
    hook(0x19d090,mouseHighlights,oldMouseHighlights);
    hook(0x16ff00,clipHighlight,oldClipHighlight);
    hook(0x2b9380,atomHotKeyFeedback,oldAtomHotKeyFeedback);
    hook(0x6336c0,hover,oldHover);hook(0x14a6d0,requestZoom,oldZoom);
    hook(0x643bc0,zoomIn,oldZoomIn);hook(0x643c60,zoomOut,oldZoomOut);
    hook(0x631730,documentProc,oldWndProc);
    hook(0x5f7c90,scrolling,oldScrolling);hook(0x5ff4d0,setSize,oldSetSize);
    hook(0x600080,scrollVisibility,oldScrollVisibility);
    hook(0x5fe480,documentIdle,oldIdle);
}
}

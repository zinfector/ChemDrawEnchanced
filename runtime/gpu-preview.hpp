#pragma once
#include "runtime.hpp"
#include "gpu-scene.hpp"

namespace cd {
inline UINT gpuSettledMessage() noexcept {
    static const UINT message=RegisterWindowMessageW(L"ChemDrawLatency.NavigationSettled.94");
    return message;
}
struct PreviewPaper {
    RectD bounds{}; // Paper bounds relative to the captured viewport, in pixels.
    Point sourceOrigin{};double sourceUnits{}; // Native camera belonging to this scene.
    SceneMatrix inkTransform{}; // Native world-to-device transform and vector pixel coverage, relative to the viewport.
    uint32_t workspace{0xdddddd},paper{0xffffff},border{};
    double marginX{},marginY{};
};
inline ScenePoint previewInkPoint(const PreviewPaper& paper,Point point) {
    const double x=(point.x-paper.sourceOrigin.x)/paper.sourceUnits;
    const double y=(point.y-paper.sourceOrigin.y)/paper.sourceUnits;
    const auto& m=paper.inkTransform;
    return {float(x*m.m11+y*m.m21+m.dx),float(x*m.m12+y*m.m22+m.dy)};
}
struct PreviewView { double x{},y{},w{},h{}; };
struct PlacementDisplayTiming {
    uint64_t revision{},physicalRelease{},nativeQueued{},presented{},uploadTicks{},drawTicks{},discarded{};
};
struct GhostStroke { Point start{},end{};double width{}; };
struct GhostPath { std::vector<Point> points;std::vector<BYTE> types;double width{};bool filled{}; };
struct GhostArtwork { std::vector<GhostStroke> strokes;std::vector<GhostPath> paths; };
struct GhostBond {
    Point start{},end{}; // Document coordinates; never a native object pointer.
    Point pressPoint{}; // Hover position for validating a blank-paper press.
    double width{};
    uint32_t ink{0xff000000};
    bool visible{};
    uint64_t identity{};
    uint64_t toolKey{};
    bool placement{}; // Only vertex bond extrusion participates in click handoff.
    POINT cursorAnchor{};bool cursorAnchored{}; // Client pixels; free preview follows the cursor during navigation.
    std::shared_ptr<const GhostArtwork> artwork;
};
struct GhostTool {
    int tool{},subtype{},order{},type{},chainAtoms{},ringCode{};
    double length{},width{},spacing{},chainAngle{},dashSpacing{};
    bool sproutRings{};
    uint32_t ink{0xff000000};
    uint64_t key{};
    bool operator==(const GhostTool&) const = default;
};
bool readGhostTool(Obj page,GhostTool&);
Obj findToolGhostTarget(Obj page,const Point& mouse);
bool makeToolGhost(Obj page,Obj target,Point mouse,const GhostTool&,GhostBond&);
void updateArrowTargetHighlight(Obj doc,Obj target);
struct HoverHighlight {
    std::shared_ptr<const GpuScene> scene;
    RectI pane{};PreviewPaper paper{};
    uint64_t identity{};
    RectI bounds{}; // Full-client pixel bounds of the cropped transparent scene.
    RectD circle{};bool vertexCircle{}; // Geometry survives temporary-object ID changes.
};
struct AlignmentStroke { ScenePoint start{},end{};bool arrows{}; };
struct AlignmentFeedback {
    std::vector<AlignmentStroke> strokes;
    float dip{1};
};
inline PreviewView anchorPreview(PreviewView shown,double cursorX,double cursorY,double width,double height) {
    return {cursorX-(cursorX-shown.x)*width/shown.w,
        cursorY-(cursorY-shown.y)*height/shown.h,width,height};
}
struct PreviewAxis { double low{},high{}; };
inline PreviewAxis previewPanRange(double viewport,double page,double margin) {
    // Keep a useful portion of the paper visible while allowing substantial
    // workspace beside it. A fixed edge margin collapses that workspace as
    // soon as the zoomed paper fills the viewport.
    const double visible=std::min(page,std::max(viewport*0.25,std::clamp(margin,0.0,viewport)));
    return {visible-page,viewport-visible};
}
inline UINT gpuCameraFrameMessage() noexcept {
    static const UINT message=RegisterWindowMessageW(L"ChemDrawLatency.CameraFrame.94");
    return message;
}
inline UINT gpuPagePresentedMessage() noexcept {
    static const UINT message=RegisterWindowMessageW(L"ChemDrawLatency.PagePresented.94");
    return message;
}
inline UINT gpuGhostCommittedMessage() noexcept {
    static const UINT message=RegisterWindowMessageW(L"ChemDrawLatency.GhostCommitted.94");
    return message;
}
inline PreviewView constrainPreview(PreviewView view,const PreviewPaper& paper,double width,double height) {
    if(!valid(paper.bounds)) return view;
    const double sx=view.w/width,sy=view.h/height;
    const double pw=(paper.bounds.r-paper.bounds.l)*sx,ph=(paper.bounds.b-paper.bounds.t)*sy;
    if(pw>0) {
        const auto x=previewPanRange(width,pw,paper.marginX);
        view.x=std::clamp(view.x+paper.bounds.l*sx,x.low,x.high)-paper.bounds.l*sx;
    }
    if(ph>0) {
        const auto y=previewPanRange(height,ph,paper.marginY);
        view.y=std::clamp(view.y+paper.bounds.t*sy,y.low,y.high)-paper.bounds.t*sy;
    }
    return view;
}
inline PreviewView easePreview(PreviewView shown,PreviewView target,double blend) {
    const double width=std::exp(std::log(shown.w)+(std::log(target.w)-std::log(shown.w))*blend);
    const double height=std::exp(std::log(shown.h)+(std::log(target.h)-std::log(shown.h))*blend);
    // Use the scale fraction for translation so the cursor's fixed point stays
    // fixed at every eased scale, rather than moving toward the viewport center.
    const double bx=std::abs(target.w-shown.w)>1.0e-8?(width-shown.w)/(target.w-shown.w):blend;
    const double by=std::abs(target.h-shown.h)>1.0e-8?(height-shown.h)/(target.h-shown.h):blend;
    shown.x+=(target.x-shown.x)*bx;shown.y+=(target.y-shown.y)*by;
    shown.w=width;shown.h=height;return shown;
}
// UI methods publish view state only. Direct3D, Direct2D and DXGI belong to
// the rendering thread; it never accesses ChemDraw objects or dispatches input.
class GpuPreview {
    struct Impl;
    std::shared_ptr<Impl> impl;
    explicit GpuPreview(std::shared_ptr<Impl> state):impl(std::move(state)) {}
public:
    static std::shared_ptr<GpuPreview> create(HWND) noexcept;
    ~GpuPreview();
    bool begin(const void* pixels,int stride,RectI pane,PreviewPaper&,PreviewView& initialView,
        std::shared_ptr<const GpuScene> fullPage={}) noexcept;
    bool scene(std::shared_ptr<const GpuScene>,RectI,const PreviewPaper&,bool replaceNavigation=false,
        std::shared_ptr<const AlignmentFeedback> alignment={}) noexcept;
    void clearAlignment() noexcept;
    bool fixedUi(std::shared_ptr<const GpuScene>,RectI pane) noexcept;
    bool hasFixedUi(RectI pane,RectI client) const noexcept;
    bool nativeScenePresented() const noexcept;
    void ghost(const GhostBond&) noexcept;
    bool displayedGhost(GhostBond&) const noexcept;
    bool beginGhostPlacement(const GhostBond&) noexcept;
    void armPlacementRelease(uint64_t,POINT,int,int) noexcept;
    PlacementDisplayTiming placementDisplayTiming() const noexcept;
    bool ghostPlacementActive() const noexcept;
    void startGhostRelease() noexcept;
    void releaseGhostPlacement() noexcept;
    void cancelGhostPlacement() noexcept;
    void highlight(const HoverHighlight&) noexcept;
    void retainHighlight(uint64_t) noexcept;
    void clearHoverHighlight() noexcept;
    void clearFeedback(bool immediate=false) noexcept;
    bool beginPlacementHighlight(uint64_t) noexcept;
    bool placementEndpoint(const HoverHighlight&) noexcept;
    bool finishPlacementHighlight(const HoverHighlight&) noexcept;
    void holdScenes(bool,bool publish=true) noexcept;
    bool usable() const noexcept;
    bool hasScene() const noexcept;
    void move(double left,double top,double width,double height) noexcept;
    bool zoomAt(double cursorX,double cursorY,double width,double height) noexcept;
    bool view(PreviewView& shown,PreviewView& target) const noexcept;
    bool cameraFrame(PreviewView&,PreviewPaper&,RectI&,bool consumeNotification=false) const noexcept;
    bool freezeCamera(PreviewView&,PreviewPaper&,RectI&) noexcept;
    bool active() const noexcept;
    bool settled() const noexcept;
    // The native page has finished painting. Retire only after presenting the
    // final native-coordinate transform, without blocking the message thread.
    void finish() noexcept;
    void clearScene() noexcept;
    void hide() noexcept;
};
}

#pragma once
#include "runtime.hpp"

namespace cd {
struct ScenePoint { float x{},y{}; };
struct SceneRect { float x{},y{},w{},h{}; };
struct SceneMatrix { float m11{1},m12{},m21{},m22{1},dx{},dy{}; };
struct ScenePath {
    std::vector<ScenePoint> points;
    std::vector<BYTE> types;
    bool winding{};
};
struct ScenePixels { int width{},height{};std::vector<BYTE> bytes; };
struct SceneStop { float position{};uint32_t color{}; };
struct SceneGradient {
    ScenePath boundary;
    std::vector<uint32_t> surround;
    std::vector<SceneStop> stops;
    std::vector<ScenePoint> blend;
    ScenePoint center{},focus{};
    uint32_t color{};
    bool gamma{};
};
struct SceneEffect {
    enum Kind { Lookup,Matrix,Gamma,Threshold,ColorKey,Remap,Blur } kind{Lookup};
    std::array<float,25> matrix{};
    std::array<float,25> grayMatrix{};
    std::array<std::array<float,256>,4> table{};
    std::vector<std::array<uint32_t,2>> remap;
    uint32_t low{},high{};
    float amount{};
    int flags{};
};
struct SceneMetafile { std::vector<BYTE> bytes;SceneRect bounds; };
struct SceneBrush {
    enum Kind { Solid,Linear,Radial,Texture,PathGradient } kind{Solid};
    uint32_t color{0xff000000};
    std::vector<SceneStop> stops;
    ScenePoint start{},end{},center{},focus{};
    float radiusX{},radiusY{};SceneMatrix transform{};
    bool gamma{},clamp{};
    std::shared_ptr<const ScenePixels> texture;
    std::shared_ptr<const SceneGradient> gradient;
    int wrap{};
};
struct SceneState {
    SceneMatrix transform{};
    std::vector<SceneRect> clip;
    float pixelOffset{};
    int interpolation{int(Gdiplus::InterpolationModeBilinear)};
    bool antialias{true},copy{};
};
struct GpuScene;
struct SceneCommand {
    enum Kind { Path,Image,Metafile,RasterOp } kind{Path};
    std::shared_ptr<const SceneState> state;
    uint64_t objectOwner{},objectGesture{};
    ScenePath path;SceneBrush brush;
    std::shared_ptr<const ScenePath> sharedPath;
    bool stroked{},fixedStroke{};
    float strokeWidth{},miterLimit{10},dashOffset{};
    int startCap{},endCap{},dashCap{},lineJoin{},dashStyle{};
    std::vector<float> dashes;
    std::shared_ptr<const GpuScene> image;
    std::shared_ptr<const ScenePixels> pixels;
    std::shared_ptr<const SceneMetafile> metafile;
    std::vector<std::shared_ptr<const SceneEffect>> effects;
    uint32_t rasterTruth{12};
    SceneRect source{},destination{};
};
struct GpuScene {
    int width{},height{};uint64_t serial{};
    bool trackingFrame{}; // Complete native work frame, presented at its original resolution.
    // Optional full-page extent in client coordinates. It can start outside
    // the window; command-list capture does not allocate a page-sized bitmap.
    SceneRect extent{};
    GpuScene* retireNext{};
    // Previous immutable canvas plus a small update; worker caches its GPU
    // bitmap. Tracking always uses its original base, preventing frame chains.
    // Keep the immutable source description for explicit native pixel access
    // on the independent hardware readback context.
    mutable std::atomic<std::shared_ptr<const GpuScene>> base;
    std::vector<SceneCommand> commands;
};
inline std::shared_ptr<GpuScene> makeGpuScene() {
    // Deleting a history must not recursively destroy its shared ancestors.
    // Each thread drains an intrusive queue; the deleter allocates no memory.
    return std::shared_ptr<GpuScene>(new GpuScene,[](GpuScene* scene) noexcept {
        static thread_local GpuScene* pending{};
        static thread_local bool draining{};
        scene->retireNext=pending;pending=scene;
        if(draining) return;
        draining=true;
        while(pending) {
            auto* next=pending;pending=next->retireNext;delete next;
        }
        draining=false;
    });
}
void installGpuCanvas();
bool beginGpuCanvas(HWND,Obj,Gdiplus::Graphics*,bool preserve);
std::shared_ptr<const GpuScene> endGpuCanvas();
bool beginGpuFixedUi(HWND,Obj,Gdiplus::Graphics*);
std::shared_ptr<const GpuScene> endGpuFixedUi() noexcept;
bool gpuCanvasRecording();
bool gpuBackgroundRecording();
bool gpuOffscreenAvailable(Obj);
bool beginGpuIdle(Obj);
bool endGpuIdle();
bool beginGpuHover(HWND,Obj);
std::shared_ptr<const GpuScene> endGpuHover(RectI* bounds=nullptr) noexcept;
bool gpuHoverRecording();
// Observe commands from an existing paint without redirecting it or redrawing.
bool beginGpuCommandCapture(Obj);
std::shared_ptr<const GpuScene> endGpuCommandCapture() noexcept;
bool gpuTrackingRecording();
bool gpuTrackingActive();
struct AlignmentFeedback;
bool publishGpuCanvasScene(Obj,std::shared_ptr<const GpuScene>,std::shared_ptr<const AlignmentFeedback> alignment={});
void disableGpuCanvas(Obj);
void beginGpuBackground(Obj);
void endGpuBackground();
class GpuObjectScope {
    uint64_t previousOwner{},previousGesture{};
public:
    GpuObjectScope(Obj,bool live=false);
    ~GpuObjectScope();
    GpuObjectScope(const GpuObjectScope&)=delete;
    GpuObjectScope& operator=(const GpuObjectScope&)=delete;
};
bool beginGpuTracking(Obj,uint64_t excludedObject=0);
void resetGpuWork(Obj);
void resetGpuTrackingDestination(Obj);
std::shared_ptr<const GpuScene> currentGpuTrackingScene();
void endGpuTracking();
void forgetGpuCanvas(HWND);
void discardGpuCanvas(HWND);
// Synchronous hardware rendering is reserved for explicit native pixel access
// and compatibility segments, never ordinary presentation.
std::shared_ptr<const ScenePixels> materializeGpuScene(const std::shared_ptr<const GpuScene>&);
std::vector<SceneCommand> translateGpuMetafile(const SceneCommand&,int,int,uint64_t&);
}

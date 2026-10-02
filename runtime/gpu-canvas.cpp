#include "gpu-scene.hpp"
#include <gdiplusflat.h>
#include <string>
#include <functional>
#include <cfloat>
#include <type_traits>
#include <list>

namespace cd {
using namespace Gdiplus;
using namespace Gdiplus::DllExports;
namespace {
struct Target {
    HWND window{};GpGraphics* graphics{};GpImage* image{};int width{},height{};
    std::shared_ptr<const GpuScene> base,snapshot;
    std::vector<SceneCommand> commands;
    std::shared_ptr<const SceneState> capturedState;
    uint64_t backgroundEpoch{};
    uint64_t nativeSerial{};
    bool nativeDirty{};
    SceneRect compatibilityBounds{};
    SceneRect hoverBounds{};size_t hoverMeasured{};
};
static std::unordered_map<GpGraphics*,std::shared_ptr<Target>> targets;
static std::unordered_map<GpImage*,std::weak_ptr<Target>> imageTargets;
static std::unordered_map<GpImage*,std::shared_ptr<const ScenePixels>> pixels;
static std::unordered_map<GpImage*,std::shared_ptr<const GpuScene>> imageVersions;
static std::unordered_map<GpImage*,std::shared_ptr<const SceneMetafile>> metafileImages;
static std::unordered_map<GpImage*,uint64_t> nativeVersions;
static thread_local unsigned nativeAccess{};
struct NativeAccess { NativeAccess(){++nativeAccess;} ~NativeAccess(){--nativeAccess;} };
static decltype(&GdipBitmapLockBits) oldLockBits{};
static decltype(&GdipBitmapUnlockBits) oldUnlockBits{};
static decltype(&GdipBitmapApplyEffect) oldApplyEffect{};
struct AttributeState {
    std::array<std::unordered_map<int,std::shared_ptr<const SceneEffect>>,2> effects;
    std::array<int,2> noop{{-1,-1}};
    bool unsupported{};
};
static std::unordered_map<const GpImageAttributes*,AttributeState> imageAttributes;
struct EffectState { GUID id{};std::vector<BYTE> parameters;std::shared_ptr<const SceneEffect> description; };
static std::unordered_map<CGpEffect*,EffectState> effects;
static std::unordered_map<HWND,std::shared_ptr<Target>> canvases;
static std::unordered_map<HWND,bool> disabled;
static thread_local HWND recordingWindow{},backgroundWindow{},idleWindow{};
static thread_local Obj trackingDoc{};
static thread_local std::shared_ptr<const GpuScene> trackingBase;
static uint64_t nextScene{};
static thread_local uint64_t objectOwner{},objectGesture{},excludedObject{},trackingGesture{};
static uint64_t nextTrackingGesture{};
using FilteredScene=std::pair<std::shared_ptr<const GpuScene>,std::shared_ptr<const GpuScene>>;
static thread_local std::unordered_map<const GpuScene*,FilteredScene> trackingScenes;
std::shared_ptr<const GpuScene> trackedInk(const std::shared_ptr<const GpuScene>& root) {
    // Arrow previews contain only the live object. Native work-buffer restores
    // include paper and molecules sampled through a different pixel grid.
    // Keep their transfer transforms, but discard that restored background.
    if(!root||!excludedObject) return {};
    struct Visit {std::shared_ptr<const GpuScene> scene,previous;bool finish{};};
    std::unordered_map<const GpuScene*,std::shared_ptr<const GpuScene>> filtered;
    std::vector<Visit> visits{{root}};
    while(!visits.empty()) {
        auto v=std::move(visits.back());visits.pop_back();
        if(filtered.contains(v.scene.get())) continue;
        if(!v.finish) {
            v.previous=v.scene->base.load(std::memory_order_acquire);v.finish=true;visits.push_back(v);
            if(v.previous) visits.push_back({v.previous});
            for(const auto& command:v.scene->commands) if(command.image) visits.push_back({command.image});
            continue;
        }
        auto layer=makeGpuScene();layer->width=v.scene->width;layer->height=v.scene->height;
        layer->extent=v.scene->extent;layer->serial=++nextScene;
        if(v.previous) layer->base=filtered.at(v.previous.get());
        for(auto command:v.scene->commands) {
            const bool live=command.objectOwner==excludedObject&&command.objectGesture==trackingGesture;
            if(command.image) {
                command.image=filtered.at(command.image.get());
                if(!command.image) continue;
            } else if(!live) continue;
            // Transparent preview ink must composite over the retained page.
            // Source-copy wrappers would erase molecules beneath the arc.
            if(command.state&&command.state->copy) {
                auto state=std::make_shared<SceneState>(*command.state);state->copy=false;command.state=std::move(state);
            }
            layer->commands.push_back(std::move(command));
        }
        filtered.emplace(v.scene.get(),layer->base.load(std::memory_order_acquire)||!layer->commands.empty()?layer:nullptr);
    }
    return filtered.at(root.get());
}
std::shared_ptr<const GpuScene> withoutTrackedOriginal(const std::shared_ptr<const GpuScene>& root) {
    if(!root||!excludedObject) return root;
    struct Visit {std::shared_ptr<const GpuScene> scene,previous;bool finish{};};
    std::vector<Visit> visits{{root}};
    while(!visits.empty()) {
        auto v=std::move(visits.back());visits.pop_back();
        if(trackingScenes.contains(v.scene.get())) continue;
        if(!v.finish) {
            v.previous=v.scene->base.load(std::memory_order_acquire);v.finish=true;
            visits.push_back(v);
            if(v.previous) visits.push_back({v.previous});
            for(const auto& c:v.scene->commands) if(c.image) visits.push_back({c.image});
            continue;
        }
        auto previous=v.previous?trackingScenes.at(v.previous.get()).second:std::shared_ptr<const GpuScene>{};
        bool changed=previous!=v.previous;
        for(const auto& c:v.scene->commands) {
            if(c.objectOwner==excludedObject&&c.objectGesture!=trackingGesture) changed=true;
            if(c.image&&trackingScenes.at(c.image.get()).second!=c.image) changed=true;
        }
        std::shared_ptr<const GpuScene> result=v.scene;
        if(changed) {
            auto copy=makeGpuScene();copy->width=v.scene->width;copy->height=v.scene->height;
            copy->extent=v.scene->extent;copy->trackingFrame=v.scene->trackingFrame;
            copy->serial=++nextScene;copy->base=previous;copy->commands.reserve(v.scene->commands.size());
            for(auto c:v.scene->commands) {
                if(c.objectOwner==excludedObject&&c.objectGesture!=trackingGesture) continue;
                if(c.image) c.image=trackingScenes.at(c.image.get()).second;
                copy->commands.push_back(std::move(c));
            }
            result=std::move(copy);
        }
        trackingScenes.emplace(v.scene.get(),FilteredScene{v.scene,std::move(result)});
    }
    return trackingScenes.at(root.get()).second;
}
static thread_local bool failed{};
static thread_local std::shared_ptr<Target> hoverTarget;
static thread_local Obj hoverPort{};
static thread_local std::unordered_map<GpGraphics*,std::shared_ptr<Target>> hoverBindings;
static thread_local std::shared_ptr<Target> fixedUiTarget;
static thread_local std::unordered_map<GpGraphics*,std::shared_ptr<Target>> fixedUiBindings;
static thread_local bool fixedUiPreviousFailure{};
static thread_local bool hoverPreviousFailure{};
static uint64_t backgroundEpoch{};
static thread_local std::shared_ptr<Target> commandCaptureTarget;
static thread_local std::vector<SceneCommand> commandCapture;
static thread_local bool commandCaptureFailed{};
using GetGraphics=Graphics*(*)(Obj);
static GetGraphics oldGetGraphics{};
static void (*oldNativePolygon)(Obj,const Vec*){};
struct InternedPath { uint64_t key{},bytes{};std::shared_ptr<const ScenePath> path; };
static std::list<InternedPath> internedPaths;
static std::unordered_multimap<uint64_t,std::list<InternedPath>::iterator> internedKeys;
static uint64_t internedBytes{};
void check(GpStatus s) { if(s!=Ok) throw std::runtime_error("Unsupported native graphics state"); }
[[noreturn]] void reject() { throw std::runtime_error("Native graphics operation requires compatibility rendering"); }
HWND docWindow(Obj doc) {
    Obj port=doc?at<Obj>(doc,0x258):nullptr;
    return port?fn<HWND(*)(Obj)>(0x624a70)(port):nullptr;
}
HWND contextWindow() {
    return fixedUiTarget?fixedUiTarget->window:hoverTarget?hoverTarget->window:recordingWindow?recordingWindow:backgroundWindow?backgroundWindow:
        trackingDoc?docWindow(trackingDoc):idleWindow;
}
void seedNativeTarget(const std::shared_ptr<Target>&);
std::shared_ptr<const GpuScene> snapshot(const std::shared_ptr<Target>& t) {
    if(t->nativeDirty) seedNativeTarget(t);
    if(t->snapshot) return t->snapshot;
    if(t->commands.empty()&&t->base&&t->base->width==t->width&&t->base->height==t->height)
        return t->snapshot=t->base;
    auto scene=makeGpuScene();scene->width=t->width;scene->height=t->height;
    scene->serial=++nextScene;scene->base=t->base;
    if(t==hoverTarget) { scene->commands=t->commands;t->snapshot=scene;return scene; }
    scene->commands=std::move(t->commands);
    // Publish only the new commands. Future snapshots extend this immutable
    // version rather than copying every earlier path on the UI thread.
    t->commands.clear();t->base=scene;t->snapshot=scene;return scene;
}
void reset(const std::shared_ptr<Target>& t,std::shared_ptr<const GpuScene> previous={}) {
    if(!t) reject();
    t->base=std::move(previous);t->commands.clear();t->snapshot.reset();t->capturedState.reset();t->nativeDirty=false;
}
std::shared_ptr<Target> bind(Obj port,Graphics* graphics,HWND window,bool primary=false) {
    if(!graphics||!window) return {};
    auto raw=at<GpGraphics*>(graphics,0);std::shared_ptr<Target> t;
    if(primary) {
        if(fixedUiTarget&&fixedUiTarget->window==window) {
            t=fixedUiTarget;
            if(!fixedUiBindings.contains(raw)) {
                auto previous=targets.find(raw);
                fixedUiBindings.emplace(raw,previous==targets.end()?nullptr:previous->second);
            }
        } else if(hoverTarget&&hoverTarget->window==window) {
            // Native hover can replace/reacquire the screen Graphics. Every
            // primary context used by this capture belongs to its transparent
            // layer, rather than the retained page associated with the HWND.
            t=hoverTarget;
            if(!hoverBindings.contains(raw)) {
                auto previous=targets.find(raw);
                hoverBindings.emplace(raw,previous==targets.end()?nullptr:previous->second);
            }
        } else {
            auto& canvas=canvases[window];if(!canvas) canvas=std::make_shared<Target>();t=canvas;
        }
    } else if(auto i=targets.find(raw);i!=targets.end()) t=i->second;
    else t=std::make_shared<Target>();
    if(t->graphics!=raw) t->capturedState.reset();
    const int previousWidth=t->width,previousHeight=t->height;
    t->window=window;t->graphics=raw;
    const auto previousImage=t->image;
    Obj image=port?at<Obj>(port,0x160):nullptr;
    // Gdiplus::Bitmap inherits Image's vtable; its native image is at +8.
    t->image=image?at<GpImage*>(image,8):nullptr;
    if(t->image&&!primary) {
        UINT w{},h{};check(GdipGetImageWidth(t->image,&w));check(GdipGetImageHeight(t->image,&h));
        t->width=int(w);t->height=int(h);imageTargets[t->image]=t;
        if(!t->base&&t->commands.empty()) if(auto version=imageVersions.find(t->image);version!=imageVersions.end()) t->base=version->second;
        if(previousImage!=t->image&&!t->base&&t->commands.empty()) t->nativeDirty=true;
    } else { auto r=clientRect(window);t->width=r.r;t->height=r.b; }
    if(t->width!=previousWidth||t->height!=previousHeight) t->capturedState.reset();
    targets[raw]=t;return t;
}
Graphics* getGraphics(Obj port) {
    if(!port) return nullptr;
    auto* graphics=oldGetGraphics(port);
    if(!graphics)return nullptr;
    if(!nativeAccess&&onUI()&&contextWindow()&&(fixedUiTarget||!disabled[contextWindow()])) {
        try {
            // A nested toolbar/menu repaint is native UI, not a page buffer.
            // Never assign a foreign window's graphics to the active document.
            const HWND owner=at<HWND>(port,0xb8);
            if(owner&&owner!=contextWindow()) return graphics;
            if(!owner&&!at<Obj>(port,0x70)&&!backgroundWindow&&!trackingDoc) {
                const auto known=targets.find(at<GpGraphics*>(graphics,0));
                if(known==targets.end()||known->second->window!=contextWindow())return graphics;
            }
            const bool primary=(hoverTarget&&port==hoverPort)||
                (at<HWND>(port,0xb8)==contextWindow()&&at<Obj>(port,0x160)==nullptr);
            auto t=bind(port,graphics,contextWindow(),primary);
            if(t&&!primary&&backgroundWindow&&t->backgroundEpoch!=backgroundEpoch) {
                auto previous=(t->base||!t->commands.empty())?snapshot(t):std::shared_ptr<const GpuScene>{};
                reset(t,previous);t->backgroundEpoch=backgroundEpoch;
            }
        } catch(...) { failed=true; }
    }
    return graphics;
}
std::shared_ptr<Target> target(GpGraphics* g) {
    if(nativeAccess||!onUI()||!contextWindow()||failed||(!fixedUiTarget&&disabled[contextWindow()])) return {};
    if(fixedUiTarget&&fixedUiTarget->graphics==g)return fixedUiTarget;
    if(hoverTarget&&hoverTarget->graphics==g) return hoverTarget;
    auto i=targets.find(g);
    if(i==targets.end()) return {};
    if(i->second->window!=contextWindow()) return {};
    if(i->second->nativeDirty) {
        try { seedNativeTarget(i->second); } catch(...) { failed=true;return {}; }
    }
    if(i->second->graphics!=g) i->second->capturedState.reset();
    i->second->graphics=g;return i->second;
}
SceneMatrix transform(GpGraphics* g) {
    PointF basis[3]={{0,0},{1,0},{0,1}};
    check(GdipTransformPoints(g,CoordinateSpaceDevice,CoordinateSpaceWorld,basis,3));
    return {basis[1].X-basis[0].X,basis[1].Y-basis[0].Y,
        basis[2].X-basis[0].X,basis[2].Y-basis[0].Y,basis[0].X,basis[0].Y};
}
struct NativePath {
    GpPath* value{};
    NativePath() { check(GdipCreatePath(FillModeAlternate,&value)); }
    ~NativePath() { if(value) GdipDeletePath(value); }
    void clone(GpPath* p) { GdipDeletePath(value);value=nullptr;check(GdipClonePath(p,&value)); }
};
struct NativeMatrix {
    GpMatrix* value{};
    NativeMatrix() { check(GdipCreateMatrix(&value)); }
    ~NativeMatrix() { if(value) GdipDeleteMatrix(value); }
};
struct NativeRegion {
    GpRegion* value{};
    NativeRegion() { check(GdipCreateRegion(&value)); }
    ~NativeRegion() { if(value) GdipDeleteRegion(value); }
};
std::vector<SceneRect> regionScans(GpRegion* region,SceneMatrix m) {
    NativeMatrix matrix;
    check(GdipSetMatrixElements(matrix.value,m.m11,m.m12,m.m21,m.m22,m.dx,m.dy));
    UINT count{};check(GdipGetRegionScansCount(region,&count,matrix.value));
    if(count>100000) reject();
    std::vector<RectF> rects(count);INT written=int(count);
    if(count) check(GdipGetRegionScans(region,rects.data(),&written,matrix.value));
    std::vector<SceneRect> result;result.reserve(written);
    for(int i=0;i<written;++i) result.push_back({rects[i].X,rects[i].Y,rects[i].Width,rects[i].Height});
    return result;
}
std::shared_ptr<const SceneState> state(const std::shared_ptr<Target>& t) {
    if(t->capturedState) return t->capturedState;
    auto s=std::make_shared<SceneState>();s->transform=transform(t->graphics);
    NativeRegion clip;check(GdipGetClip(t->graphics,clip.value));
    BOOL infinite{};check(GdipIsInfiniteRegion(clip.value,t->graphics,&infinite));
    if(infinite) s->clip={{0,0,float(t->width),float(t->height)}};
    else s->clip=regionScans(clip.value,s->transform);
    SmoothingMode smoothing{};CompositingMode mode{};PixelOffsetMode offset{};InterpolationMode interpolation{};
    check(GdipGetSmoothingMode(t->graphics,&smoothing));check(GdipGetCompositingMode(t->graphics,&mode));
    check(GdipGetPixelOffsetMode(t->graphics,&offset));check(GdipGetInterpolationMode(t->graphics,&interpolation));
    s->antialias=smoothing==SmoothingModeHighQuality||smoothing==SmoothingModeAntiAlias||
        smoothing==SmoothingModeAntiAlias8x8;
    // Direct2D pixels are centered at n+0.5. GDI+'s None/Default grid is
    // centered at n, while Half/HighQuality already matches Direct2D.
    s->pixelOffset=offset==PixelOffsetModeHalf||offset==PixelOffsetModeHighQuality?0.0f:0.5f;
    s->interpolation=int(interpolation);s->copy=mode==CompositingModeSourceCopy;
    t->capturedState=s;return s;
}
void invalidateState(GpGraphics* graphics) noexcept {
    if(!onUI()) return;
    if(auto found=targets.find(graphics);found!=targets.end()) found->second->capturedState.reset();
    if(hoverTarget&&hoverTarget->graphics==graphics) hoverTarget->capturedState.reset();
}
// Track state mutations even outside recording. Published commands keep their
// immutable old state; subsequent primitives lazily read the new state once.
#define STATE(name,params,args) \
    static decltype(&Gdiplus::DllExports::name) old_##name{}; \
    static GpStatus WINGDIPAPI gpu_##name params { \
        auto result=old_##name args;if(result==Ok) invalidateState(g);return result; \
    }
STATE(GdipSetWorldTransform,(GpGraphics* g,GpMatrix* m),(g,m))
STATE(GdipResetWorldTransform,(GpGraphics* g),(g))
STATE(GdipMultiplyWorldTransform,(GpGraphics* g,const GpMatrix* m,GpMatrixOrder order),(g,m,order))
STATE(GdipTranslateWorldTransform,(GpGraphics* g,REAL x,REAL y,GpMatrixOrder order),(g,x,y,order))
STATE(GdipScaleWorldTransform,(GpGraphics* g,REAL x,REAL y,GpMatrixOrder order),(g,x,y,order))
STATE(GdipRotateWorldTransform,(GpGraphics* g,REAL angle,GpMatrixOrder order),(g,angle,order))
STATE(GdipSetPageUnit,(GpGraphics* g,GpUnit unit),(g,unit))
STATE(GdipSetPageScale,(GpGraphics* g,REAL scale),(g,scale))
STATE(GdipSetClipGraphics,(GpGraphics* g,GpGraphics* source,CombineMode mode),(g,source,mode))
STATE(GdipSetClipRect,(GpGraphics* g,REAL x,REAL y,REAL w,REAL h,CombineMode mode),(g,x,y,w,h,mode))
STATE(GdipSetClipRectI,(GpGraphics* g,INT x,INT y,INT w,INT h,CombineMode mode),(g,x,y,w,h,mode))
STATE(GdipSetClipPath,(GpGraphics* g,GpPath* p,CombineMode mode),(g,p,mode))
STATE(GdipSetClipRegion,(GpGraphics* g,GpRegion* r,CombineMode mode),(g,r,mode))
STATE(GdipSetClipHrgn,(GpGraphics* g,HRGN r,CombineMode mode),(g,r,mode))
STATE(GdipResetClip,(GpGraphics* g),(g))
STATE(GdipTranslateClip,(GpGraphics* g,REAL x,REAL y),(g,x,y))
STATE(GdipTranslateClipI,(GpGraphics* g,INT x,INT y),(g,x,y))
STATE(GdipSetSmoothingMode,(GpGraphics* g,SmoothingMode mode),(g,mode))
STATE(GdipSetCompositingMode,(GpGraphics* g,CompositingMode mode),(g,mode))
STATE(GdipSetPixelOffsetMode,(GpGraphics* g,PixelOffsetMode mode),(g,mode))
STATE(GdipSetInterpolationMode,(GpGraphics* g,InterpolationMode mode),(g,mode))
STATE(GdipRestoreGraphics,(GpGraphics* g,GraphicsState saved),(g,saved))
STATE(GdipBeginContainer,(GpGraphics* g,const GpRectF* dest,const GpRectF* source,GpUnit unit,GraphicsContainer* saved),
    (g,dest,source,unit,saved))
STATE(GdipBeginContainerI,(GpGraphics* g,const GpRect* dest,const GpRect* source,GpUnit unit,GraphicsContainer* saved),
    (g,dest,source,unit,saved))
STATE(GdipBeginContainer2,(GpGraphics* g,GraphicsContainer* saved),(g,saved))
STATE(GdipEndContainer,(GpGraphics* g,GraphicsContainer saved),(g,saved))
#undef STATE
ScenePath path(GpPath* p) {
    INT count{};check(GdipGetPointCount(p,&count));
    if(count<0||count>1000000) reject();
    std::vector<PointF> points(count);ScenePath result;result.types.resize(count);result.points.reserve(count);
    if(count) { check(GdipGetPathPoints(p,points.data(),count));check(GdipGetPathTypes(p,result.types.data(),count)); }
    for(auto point:points) result.points.push_back({point.X,point.Y});
    FillMode fill{};check(GdipGetPathFillMode(p,&fill));result.winding=fill==FillModeWinding;return result;
}
SceneMatrix brushTransform(GpMatrix* m) {
    REAL v[6]{};check(GdipGetMatrixElements(m,v));return {v[0],v[1],v[2],v[3],v[4],v[5]};
}
std::shared_ptr<const ScenePixels> imagePixels(GpImage* image) {
    if(auto i=pixels.find(image);i!=pixels.end()) return i->second;
    ImageType type{};check(GdipGetImageType(image,&type));if(type!=ImageTypeBitmap) reject();
    UINT w{},h{};check(GdipGetImageWidth(image,&w));check(GdipGetImageHeight(image,&h));
    if(!w||!h||uint64_t(w)*h>64000000) reject();
    auto result=std::make_shared<ScenePixels>();result->width=int(w);result->height=int(h);result->bytes.resize(size_t(w)*h*4);
    Rect area(0,0,int(w),int(h));BitmapData data{};
    check(oldLockBits(reinterpret_cast<GpBitmap*>(image),&area,ImageLockModeRead,PixelFormat32bppPARGB,&data));
    for(UINT y=0;y<h;++y) memcpy(result->bytes.data()+size_t(y)*w*4,
        static_cast<const BYTE*>(data.Scan0)+ptrdiff_t(y)*data.Stride,size_t(w)*4);
    oldUnlockBits(reinterpret_cast<GpBitmap*>(image),&data);pixels[image]=result;return result;
}
void seedNativeTarget(const std::shared_ptr<Target>& t) {
    if(!t->nativeDirty||!t->image) return;
    // A batch of native CPU writes is already authoritative in this bitmap.
    // Capture it once when it next becomes a GPU input, never after each bond.
    NativeAccess suspended;check(GdipFlush(t->graphics,FlushIntentionSync));
    auto source=imagePixels(t->image);auto scene=makeGpuScene();scene->serial=++nextScene;
    scene->width=t->width;scene->height=t->height;
    SceneCommand command;command.kind=SceneCommand::Image;command.pixels=source;
    auto s=std::make_shared<SceneState>();s->copy=true;s->clip={{0,0,float(t->width),float(t->height)}};
    command.state=s;command.source=command.destination={0,0,float(t->width),float(t->height)};
    scene->commands.push_back(std::move(command));t->base=scene;t->snapshot=scene;t->commands.clear();
    t->nativeDirty=false;t->nativeSerial=scene->serial;
}
SceneBrush brush(GpBrush* b) {
    SceneBrush result;BrushType type{};check(GdipGetBrushType(b,&type));
    if(type==BrushTypeSolidColor) {
        ARGB value{};check(GdipGetSolidFillColor(static_cast<GpSolidFill*>(b),&value));result.color=value;return result;
    }
    if(type==BrushTypeHatchFill) {
        // A tiny, immutable native hatch tile is a texture resource, rather than
        // rerasterizing the document. It retains all native hatch patterns.
        const HWND saved=recordingWindow,background=backgroundWindow;Obj doc=trackingDoc;
        recordingWindow=backgroundWindow=nullptr;trackingDoc=nullptr;
        GpBitmap* image{};GpGraphics* g{};
        auto r=std::make_shared<ScenePixels>();r->width=r->height=8;r->bytes.resize(8*8*4);
        GpStatus code=GdipCreateBitmapFromScan0(8,8,32,PixelFormat32bppPARGB,r->bytes.data(),&image);
        if(code==Ok) code=GdipGetImageGraphicsContext(image,&g);
        if(code==Ok) code=GdipFillRectangle(g,b,0,0,8,8);
        if(g) GdipDeleteGraphics(g);if(image) GdipDisposeImage(image);
        recordingWindow=saved;backgroundWindow=background;trackingDoc=doc;check(code);
        result.kind=SceneBrush::Texture;result.texture=r;return result;
    }
    if(type==BrushTypeTextureFill) {
        GpImage* image{};check(GdipGetTextureImage(static_cast<GpTexture*>(b),&image));
        try { result.texture=imagePixels(image); } catch(...) { GdipDisposeImage(image);throw; }
        pixels.erase(image);GdipDisposeImage(image);
        NativeMatrix m;check(GdipGetTextureTransform(static_cast<GpTexture*>(b),m.value));
        result.kind=SceneBrush::Texture;result.transform=brushTransform(m.value);
        WrapMode wrap{};check(GdipGetTextureWrapMode(static_cast<GpTexture*>(b),&wrap));
        result.wrap=int(wrap);result.clamp=wrap==WrapModeClamp;return result;
    }
    if(type==BrushTypeLinearGradient) {
        auto line=static_cast<GpLineGradient*>(b);RectF r{};ARGB colors[2]{};INT count{};
        check(GdipGetLineRect(line,&r));check(GdipGetLineColors(line,colors));
        result.kind=SceneBrush::Linear;result.start={r.X,r.Y+r.Height*.5f};result.end={r.X+r.Width,r.Y+r.Height*.5f};
        NativeMatrix m;check(GdipGetLineTransform(line,m.value));result.transform=brushTransform(m.value);
        BOOL gamma{};check(GdipGetLineGammaCorrection(line,&gamma));result.gamma=gamma!=0;
        WrapMode wrap{};check(GdipGetLineWrapMode(line,&wrap));result.wrap=int(wrap);result.clamp=wrap==WrapModeClamp;
        check(GdipGetLinePresetBlendCount(line,&count));
        if(count>0) {
            std::vector<ARGB> values(count);std::vector<REAL> positions(count);
            check(GdipGetLinePresetBlend(line,values.data(),positions.data(),count));
            for(int i=0;i<count;++i) result.stops.push_back({positions[i],values[i]});
        } else {
            INT blendCount{};check(GdipGetLineBlendCount(line,&blendCount));
            if(blendCount>1) {
                std::vector<REAL> factors(blendCount),positions(blendCount);check(GdipGetLineBlend(line,factors.data(),positions.data(),blendCount));
                for(int i=0;i<blendCount;++i) {
                    auto mix=[&](int shift) { return uint32_t(std::lround(float((colors[0]>>shift)&255)*(1-factors[i])+float((colors[1]>>shift)&255)*factors[i]))<<shift; };
                    result.stops.push_back({positions[i],mix(0)|mix(8)|mix(16)|mix(24)});
                }
            } else result.stops={{0,colors[0]},{1,colors[1]}};
        }
        return result;
    }
    if(type==BrushTypePathGradient) {
        auto native=static_cast<GpPathGradient*>(b);auto value=std::make_shared<SceneGradient>();
        NativePath outline;check(GdipGetPathGradientPath(native,outline.value));value->boundary=path(outline.value);
        // Multiple figures and curved boundaries retain native segment rendering;
        // polygon fans preserve the original boundary-color correspondence.
        if(value->boundary.points.size()<3||value->boundary.points.size()>256) reject();
        for(size_t i=0;i<value->boundary.types.size();++i) {
            auto pointType=value->boundary.types[i]&PathPointTypePathTypeMask;
            if(pointType==PathPointTypeBezier||(i&&pointType==PathPointTypeStart)) reject();
        }
        PointF center{};check(GdipGetPathGradientCenterPoint(native,&center));value->center={center.X,center.Y};
        ARGB centerColor{};check(GdipGetPathGradientCenterColor(native,&centerColor));value->color=uint32_t(centerColor);
        check(GdipGetPathGradientFocusScales(native,&value->focus.x,&value->focus.y));
        BOOL gamma{};check(GdipGetPathGradientGammaCorrection(native,&gamma));value->gamma=gamma!=0;
        NativeMatrix m;check(GdipGetPathGradientTransform(native,m.value));result.transform=brushTransform(m.value);
        WrapMode wrap{};check(GdipGetPathGradientWrapMode(native,&wrap));if(wrap!=WrapModeClamp) reject();
        // The shader maps rays from the center. A center outside the polygon or
        // a non-star-shaped boundary needs the native compatibility segment.
        float sign{};
        for(size_t i=0;i<value->boundary.points.size();++i) {
            auto a=value->boundary.points[i],c=value->boundary.points[(i+1)%value->boundary.points.size()];
            float cross=(c.x-a.x)*(center.Y-a.y)-(c.y-a.y)*(center.X-a.x);
            if(std::abs(cross)<1e-6f) continue;
            if(sign&&cross*sign<0) reject();sign=cross;
        }
        if(!sign) reject();
        INT count{};check(GdipGetPathGradientSurroundColorCount(native,&count));if(count<1||count>256) reject();
        std::vector<ARGB> surround(count);check(GdipGetPathGradientSurroundColorsWithCount(native,surround.data(),&count));
        value->surround.assign(surround.begin(),surround.begin()+count);
        check(GdipGetPathGradientPresetBlendCount(native,&count));if(count>64) reject();
        if(count>0) {
            std::vector<ARGB> colors(count);std::vector<REAL> positions(count);check(GdipGetPathGradientPresetBlend(native,colors.data(),positions.data(),count));
            for(int i=0;i<count;++i) value->stops.push_back({positions[i],colors[i]});
        } else {
            check(GdipGetPathGradientBlendCount(native,&count));if(count>64) reject();
            if(count>1) {
                std::vector<REAL> factors(count),positions(count);check(GdipGetPathGradientBlend(native,factors.data(),positions.data(),count));
                for(int i=0;i<count;++i) value->blend.push_back({positions[i],factors[i]});
            } else value->blend={{0,0},{1,1}};
        }
        result.kind=SceneBrush::PathGradient;result.gradient=value;return result;
    }
    reject();
}
void append(const std::shared_ptr<Target>& t,SceneCommand command) {
    if(objectOwner) {command.objectOwner=objectOwner;command.objectGesture=objectGesture;}
    if(t==commandCaptureTarget&&!commandCaptureFailed) {
        // Retain only small vector feedback. Compatibility/image paths and
        // large object overlays must not add unbounded work to ordinary paint.
        if(command.kind!=SceneCommand::Path||commandCapture.size()>=256) commandCaptureFailed=true;
        else try {commandCapture.push_back(command);}catch(...) {commandCaptureFailed=true;}
    }
    t->commands.push_back(std::move(command));t->snapshot.reset();
    if(t->image) { imageVersions.erase(t->image);pixels.erase(t->image); }
}
void pathBounds(const std::shared_ptr<Target>& t,GpPath* p,GpPen* pen=nullptr) {
    auto x=transform(t->graphics);NativeMatrix m;
    check(GdipSetMatrixElements(m.value,x.m11,x.m12,x.m21,x.m22,x.dx,x.dy));
    RectF r{};check(GdipGetPathWorldBounds(p,&r,m.value,pen));
    t->compatibilityBounds={r.X-4,r.Y-4,r.Width+8,r.Height+8};
    if(t==hoverTarget) {
        const auto next=t->compatibilityBounds;auto& area=t->hoverBounds;
        if(!t->hoverMeasured) area=next;
        else {
            const float left=std::min(area.x,next.x),top=std::min(area.y,next.y);
            const float right=std::max(area.x+area.w,next.x+next.w),bottom=std::max(area.y+area.h,next.y+next.h);
            area={left,top,right-left,bottom-top};
        }
        ++t->hoverMeasured;
    }
}
void fill(const std::shared_ptr<Target>& t,GpBrush* b,GpPath* p) {
    // Bounds are needed for the hover crop or a compatibility segment only.
    // Ordinary GPU fills already carry exact native paths and clip state.
    if(t==hoverTarget) pathBounds(t,p);
    try {
    SceneCommand command;command.state=state(t);command.path=path(p);command.brush=brush(b);
    const auto& s=*command.state;const auto& m=s.transform;
    if(command.brush.kind==SceneBrush::Solid&&(command.brush.color>>24)==255&&command.path.points.size()==4&&
        s.clip.size()==1&&s.clip[0].x<=0&&s.clip[0].y<=0&&s.clip[0].x+s.clip[0].w>=t->width&&s.clip[0].y+s.clip[0].h>=t->height&&m.m12==0&&m.m21==0) {
        // Opaque full-buffer clears supersede older vector histories. This
        // retains reconstructible versions without accumulating old repaints.
        const auto& points=command.path.points;
        bool rectangle=points[0].y==points[1].y&&points[1].x==points[2].x&&points[2].y==points[3].y&&points[3].x==points[0].x;
        float x1=points[0].x*m.m11+m.dx,x2=points[2].x*m.m11+m.dx;
        float y1=points[0].y*m.m22+m.dy,y2=points[2].y*m.m22+m.dy;
        if(rectangle&&std::min(x1,x2)<=0&&std::max(x1,x2)>=t->width&&std::min(y1,y2)<=0&&std::max(y1,y2)>=t->height) reset(t);
    }
    append(t,std::move(command));
    } catch(...) { if(t!=hoverTarget) pathBounds(t,p);throw; }
}
std::shared_ptr<const ScenePath> internPolygon(const ScenePath& path) {
    uint64_t key=14695981039346656037ULL;
    const auto* bytes=reinterpret_cast<const BYTE*>(path.points.data());
    for(size_t i=0;i<path.points.size()*sizeof(ScenePoint);++i) { key^=bytes[i];key*=1099511628211ULL; }
    const auto range=internedKeys.equal_range(key);
    for(auto i=range.first;i!=range.second;++i) {
        auto entry=i->second;const auto& old=*entry->path;
        if(old.points.size()==path.points.size()&&old.types==path.types&&old.winding==path.winding&&
            (old.points.empty()||!memcmp(old.points.data(),path.points.data(),path.points.size()*sizeof(ScenePoint)))) {
            internedPaths.splice(internedPaths.end(),internedPaths,entry);return entry->path;
        }
    }
    auto shared=std::make_shared<const ScenePath>(path);
    const uint64_t size=sizeof(InternedPath)+path.points.size()*sizeof(ScenePoint)+path.types.size();
    constexpr uint64_t budget=8ULL*1024*1024;
    if(size>budget) return shared;
    while(!internedPaths.empty()&&(internedBytes+size>budget||internedPaths.size()>=16384)) {
        auto entry=internedPaths.begin();const auto entries=internedKeys.equal_range(entry->key);
        for(auto i=entries.first;i!=entries.second;++i) if(i->second==entry) { internedKeys.erase(i);break; }
        internedBytes-=entry->bytes;internedPaths.erase(entry);
    }
    internedPaths.push_back({key,size,shared});auto entry=std::prev(internedPaths.end());
    try { internedKeys.emplace(key,entry); } catch(...) { internedPaths.erase(entry);throw; }
    internedBytes+=size;return shared;
}
void nativePolygon(Obj port,const Vec* points) {
    // Preserve all native state changes surrounding PaintPolygon. Replace only
    // its per-polygon point allocation and the temporary GDI+ path round-trip.
    if(!onUI()||!points||!port||hoverTarget||!contextWindow()) { oldNativePolygon(port,points);return; }
    auto capture=[&]() {
        auto* graphics=getGraphics(port);auto t=graphics?target(at<GpGraphics*>(graphics,0)):nullptr;
        const auto begin=reinterpret_cast<uintptr_t>(points->first),end=reinterpret_cast<uintptr_t>(points->last);
        if(!t||end<begin||(end-begin)%sizeof(Point)||(end-begin)/sizeof(Point)>65536||(!begin&&end!=begin)) {
            return false;
        }
        const auto count=(end-begin)/sizeof(Point);if(count<2) return true;
        const Obj scale=fn<Obj(*)(Obj)>(0x3cb550)(port);if(!scale) return false;
        static thread_local ScenePath scratch;
        scratch.points.resize(count);scratch.types.assign(count,BYTE(PathPointTypeLine));
        scratch.types.front()=BYTE(PathPointTypeStart);scratch.types.back()|=BYTE(PathPointTypeCloseSubpath);scratch.winding=false;
        const auto* first=reinterpret_cast<const Point*>(points->first);
        for(size_t i=0;i<count;++i) {
            const double x=fn<double(*)(Obj,double)>(0x3c6560)(scale,first[i].x);
            const double y=fn<double(*)(Obj,double)>(0x3c6590)(scale,first[i].y);
            if(!std::isfinite(x)||!std::isfinite(y)||std::abs(x)>FLT_MAX||std::abs(y)>FLT_MAX) reject();
            scratch.points[i]={float(x),float(y)};
        }
        const Obj foreground=fn<Obj(*)(Obj)>(0x622bb0)(port);if(!foreground) reject();
        SceneCommand command;command.state=state(t);command.brush=brush(at<GpBrush*>(foreground,8));
        command.sharedPath=internPolygon(scratch);append(t,std::move(command));return true;
    };
    try { if(capture()) return; } catch(...) {
        // No command was published before capture completed. The original
        // primitive still supplies compatibility bounds and fallback handling.
    }
    oldNativePolygon(port,points);
}
void stroke(const std::shared_ptr<Target>& t,GpPen* pen,GpPath* p) {
    if(t==hoverTarget) pathBounds(t,p,pen);
    try {
    PenAlignment alignment{};LineCap start{},end{};INT compound{};Unit unit{};
    check(GdipGetPenMode(pen,&alignment));check(GdipGetPenStartCap(pen,&start));check(GdipGetPenEndCap(pen,&end));
    check(GdipGetPenCompoundCount(pen,&compound));check(GdipGetPenUnit(pen,&unit));
    NativeMatrix penMatrix;check(GdipGetPenTransform(pen,penMatrix.value));
    const auto m=brushTransform(penMatrix.value);
    const bool identity=m.m11==1&&m.m12==0&&m.m21==0&&m.m22==1&&m.dx==0&&m.dy==0;
    if(alignment==PenAlignmentCenter&&compound==0&&int(start)<=3&&int(end)<=3&&identity&&
        (unit==UnitPixel||unit==UnitWorld)) {
        SceneCommand command;command.state=state(t);command.path=path(p);
        command.stroked=true;command.fixedStroke=unit==UnitPixel;
        command.startCap=int(start);command.endCap=int(end);
        DashCap dashCap{};LineJoin join{};DashStyle dashStyle{};
        check(GdipGetPenWidth(pen,&command.strokeWidth));check(GdipGetPenMiterLimit(pen,&command.miterLimit));
        check(GdipGetPenDashOffset(pen,&command.dashOffset));check(GdipGetPenDashCap197819(pen,&dashCap));
        check(GdipGetPenLineJoin(pen,&join));check(GdipGetPenDashStyle(pen,&dashStyle));
        command.dashCap=int(dashCap);command.lineJoin=int(join);command.dashStyle=int(dashStyle);
        if(dashStyle==DashStyleCustom) {
            INT count{};check(GdipGetPenDashCount(pen,&count));command.dashes.resize(count);
            if(count) check(GdipGetPenDashArray(pen,command.dashes.data(),count));
        }
        GpBrush* b{};check(GdipGetPenBrushFill(pen,&b));
        try { command.brush=brush(b); } catch(...) { GdipDeleteBrush(b);throw; }
        GdipDeleteBrush(b);if(command.brush.kind==SceneBrush::PathGradient) reject();append(t,std::move(command));return;
    }
    NativePath outline;outline.clone(p);
    // Complex native caps and compound/inset pens retain native outline
    // construction; Direct2D still rasterizes their resulting geometry.
    check(GdipWidenPath(outline.value,pen,nullptr,.25f));
    GpBrush* b{};check(GdipGetPenBrushFill(pen,&b));
    try { fill(t,b,outline.value); } catch(...) { GdipDeleteBrush(b);throw; }
    GdipDeleteBrush(b);
    } catch(...) { if(t!=hoverTarget) pathBounds(t,p,pen);throw; }
}
std::shared_ptr<const GpuScene> imageScene(GpImage* source) {
    if(auto i=imageVersions.find(source);i!=imageVersions.end()) return i->second;
    if(auto i=imageTargets.find(source);i!=imageTargets.end()) {
        auto owner=i->second.lock();if(owner&&(owner->base||!owner->commands.empty())) return snapshot(owner);
    }
    return {};
}
void materializeNative(GpImage* image) {
    if(nativeAccess||!onUI()) return;
    if(auto found=imageTargets.find(image);found!=imageTargets.end())
        if(auto owner=found->second.lock();owner&&owner->nativeDirty) return;
    auto scene=imageScene(image);if(!scene) return;
    if(nativeVersions[image]==scene->serial) return;
    if(auto i=imageTargets.find(image);i!=imageTargets.end()) {
        if(auto owner=i->second.lock();owner&&owner->nativeSerial==scene->serial) return;
    }
    auto raster=materializeGpuScene(scene);NativeAccess suspended;
    Rect area(0,0,raster->width,raster->height);BitmapData data{};
    check(oldLockBits(reinterpret_cast<GpBitmap*>(image),&area,ImageLockModeWrite,PixelFormat32bppPARGB,&data));
    for(int y=0;y<raster->height;++y) memcpy(static_cast<BYTE*>(data.Scan0)+ptrdiff_t(y)*data.Stride,
        raster->bytes.data()+size_t(y)*raster->width*4,size_t(raster->width)*4);
    check(oldUnlockBits(reinterpret_cast<GpBitmap*>(image),&data));pixels[image]=raster;nativeVersions[image]=scene->serial;
    if(auto i=imageTargets.find(image);i!=imageTargets.end()) if(auto owner=i->second.lock()) owner->nativeSerial=scene->serial;
}
std::shared_ptr<const SceneMetafile> copyMetafile(HENHMETAFILE handle) {
    auto value=std::make_shared<SceneMetafile>();UINT length=GetEnhMetaFileBits(handle,0,nullptr);
    if(!length||length>64000000) reject();value->bytes.resize(length);
    if(GetEnhMetaFileBits(handle,length,value->bytes.data())!=length) reject();
    ENHMETAHEADER header{};if(!GetEnhMetaFileHeader(handle,sizeof(header),&header)) reject();
    value->bounds={float(header.rclBounds.left),float(header.rclBounds.top),
        float(header.rclBounds.right-header.rclBounds.left+1),float(header.rclBounds.bottom-header.rclBounds.top+1)};
    // Repeated HDC text segments often contain identical records. Retain the
    // device-side metafile cache across those frames without owning old scenes.
    static std::unordered_map<uint64_t,std::weak_ptr<const SceneMetafile>> interned;
    uint64_t hash=1469598103934665603ULL;
    for(auto byte:value->bytes) { hash^=byte;hash*=1099511628211ULL; }
    if(auto i=interned.find(hash);i!=interned.end()) if(auto existing=i->second.lock();existing&&existing->bytes==value->bytes) return existing;
    for(auto i=interned.begin();i!=interned.end();) { if(i->second.expired()) i=interned.erase(i);else ++i; }
    interned[hash]=value;
    return value;
}
std::shared_ptr<const SceneMetafile> imageMetafile(GpImage* source) {
    if(auto i=metafileImages.find(source);i!=metafileImages.end()) return i->second;
    HENHMETAFILE handle{};check(GdipGetHemfFromMetafile(reinterpret_cast<GpMetafile*>(source),&handle));
    std::shared_ptr<const SceneMetafile> result;
    try { result=copyMetafile(handle); } catch(...) { DeleteEnhMetaFile(handle);throw; }
    DeleteEnhMetaFile(handle);metafileImages[source]=result;return result;
}
void applyAttributes(SceneCommand& command,const GpImageAttributes* a) {
    if(!a) return;auto found=imageAttributes.find(a);if(found==imageAttributes.end()||found->second.unsupported) reject();
    const auto& value=found->second;if((value.noop[1]>=0?value.noop[1]:value.noop[0])==1) return;
    // Category-specific settings override default settings individually.
    for(int key:{int(SceneEffect::Remap),int(SceneEffect::ColorKey),int(SceneEffect::Matrix),100,
            int(SceneEffect::Gamma),int(SceneEffect::Threshold)}) {
        auto bitmap=value.effects[1].find(key),general=value.effects[0].find(key);
        if(bitmap!=value.effects[1].end()) { if(bitmap->second) command.effects.push_back(bitmap->second); }
        else if(general!=value.effects[0].end()&&general->second) command.effects.push_back(general->second);
    }
}
std::shared_ptr<const SceneEffect> describeEffect(CGpEffect* effect) {
    auto found=effects.find(effect);if(found==effects.end()) reject();auto& info=found->second;
    if(info.description) return info.description;
    auto value=std::make_shared<SceneEffect>();
    if(info.id==ColorMatrixEffectGuid&&info.parameters.size()==sizeof(ColorMatrix)) {
        value->kind=SceneEffect::Matrix;memcpy(value->matrix.data(),info.parameters.data(),sizeof(ColorMatrix));
    } else if(info.id==ColorLUTEffectGuid&&info.parameters.size()==sizeof(ColorLUTParams)) {
        auto* lut=reinterpret_cast<const ColorLUTParams*>(info.parameters.data());
        for(int i=0;i<256;++i) { value->table[0][i]=lut->lutR[i]/255.f;value->table[1][i]=lut->lutG[i]/255.f;
            value->table[2][i]=lut->lutB[i]/255.f;value->table[3][i]=lut->lutA[i]/255.f; }
    } else if(info.id==BrightnessContrastEffectGuid||info.id==LevelsEffectGuid||info.id==ColorCurveEffectGuid) {
        // Native transfer-table generation preserves these channel-separable
        // algorithms without processing a document-sized image on the CPU.
        NativeAccess suspended;std::array<uint32_t,256> ramp{};
        for(uint32_t i=0;i<256;++i) ramp[i]=0xff000000|(i<<16)|(i<<8)|i;
        GpBitmap* bitmap{};check(GdipCreateBitmapFromScan0(256,1,1024,PixelFormat32bppARGB,
            reinterpret_cast<BYTE*>(ramp.data()),&bitmap));
        auto code=oldApplyEffect(bitmap,effect,nullptr,FALSE,nullptr,nullptr);
        BitmapData data{};Rect area(0,0,256,1);
        if(code==Ok) code=oldLockBits(bitmap,&area,ImageLockModeRead,PixelFormat32bppARGB,&data);
        if(code==Ok) {
            auto row=static_cast<const BYTE*>(data.Scan0);
            for(int i=0;i<256;++i) { value->table[0][i]=row[i*4+2]/255.f;value->table[1][i]=row[i*4+1]/255.f;
                value->table[2][i]=row[i*4]/255.f;value->table[3][i]=i/255.f; }
            oldUnlockBits(bitmap,&data);
        }
        GdipDisposeImage(bitmap);check(code);
    } else reject();
    info.description=value;return value;
}
void image(const std::shared_ptr<Target>& t,GpImage* source,SceneRect dest,SceneRect src,
        GpUnit unit,const GpImageAttributes* attributes,DrawImageAbort callback) {
    try {
    auto transformValue=transform(t->graphics);float left=FLT_MAX,top=FLT_MAX,right=-FLT_MAX,bottom=-FLT_MAX;
    for(auto p:std::array<ScenePoint,4>{{{dest.x,dest.y},{dest.x+dest.w,dest.y},{dest.x,dest.y+dest.h},{dest.x+dest.w,dest.y+dest.h}}}) {
        float x=p.x*transformValue.m11+p.y*transformValue.m21+transformValue.dx;
        float y=p.x*transformValue.m12+p.y*transformValue.m22+transformValue.dy;
        left=std::min(left,x);top=std::min(top,y);right=std::max(right,x);bottom=std::max(bottom,y);
    }
    t->compatibilityBounds={left-4,top-4,right-left+8,bottom-top+8};
    if(callback) reject();
    if(unit!=UnitPixel) {
        REAL dx{},dy{};check(GdipGetImageHorizontalResolution(source,&dx));check(GdipGetImageVerticalResolution(source,&dy));
        auto scale=[](GpUnit u,float dpi) { switch(u) { case UnitPoint:return dpi/72;case UnitInch:return dpi;
            case UnitDocument:return dpi/300;case UnitMillimeter:return dpi/25.4f;case UnitDisplay:return dpi/75;default:reject(); } };
        float x=scale(unit,dx),y=scale(unit,dy);src={src.x*x,src.y*y,src.w*x,src.h*y};
    }
    if(dest.w<=0||dest.h<=0||src.w<=0||src.h<=0) reject();
    SceneCommand command;command.kind=SceneCommand::Image;command.state=state(t);
    command.destination=dest;command.source=src;
    ImageType type{};check(GdipGetImageType(source,&type));
    if(type==ImageTypeMetafile) {
        if(attributes) reject();command.kind=SceneCommand::Metafile;command.metafile=imageMetafile(source);
        UINT w{},h{};check(GdipGetImageWidth(source,&w));check(GdipGetImageHeight(source,&h));
        command.source={src.x/float(w),src.y/float(h),src.w/float(w),src.h/float(h)};
        auto commands=translateGpuMetafile(command,t->width,t->height,nextScene);
        for(auto& item:commands) append(t,std::move(item));return;
    }
    command.image=imageScene(source);
    if(!command.image) command.pixels=imagePixels(source);
    applyAttributes(command,attributes);
    append(t,std::move(command));
    } catch(...) { materializeNative(source);throw; }
}
GpStatus compatibility(const std::shared_ptr<Target>& t,const std::function<GpStatus(GpGraphics*)>& operation) {
    auto captured=state(t);auto area=t->compatibilityBounds;
    const float right=std::min(float(t->width),std::ceil(area.x+area.w)),bottom=std::min(float(t->height),std::ceil(area.y+area.h));
    area.x=std::max(0.f,std::floor(area.x));area.y=std::max(0.f,std::floor(area.y));area.w=right-area.x;area.h=bottom-area.y;
    if(area.w<=0||area.h<=0) return Ok;
    auto crop=makeGpuScene();crop->serial=++nextScene;crop->width=int(area.w);crop->height=int(area.h);
    SceneCommand background;background.kind=SceneCommand::Image;background.image=snapshot(t);
    auto full=std::make_shared<SceneState>();full->copy=true;full->clip={{0,0,area.w,area.h}};background.state=full;
    background.source=area;background.destination={0,0,area.w,area.h};crop->commands.push_back(std::move(background));
    auto source=materializeGpuScene(crop);
    auto raster=std::make_shared<ScenePixels>(*source);NativeAccess suspended;
    GpBitmap* bitmap{};GpGraphics* g{};GpRegion* clip{};GpStatus result{};
    try {
        check(GdipCreateBitmapFromScan0(raster->width,raster->height,raster->width*4,PixelFormat32bppPARGB,raster->bytes.data(),&bitmap));
        check(GdipGetImageGraphicsContext(bitmap,&g));NativeMatrix m;
        check(GdipSetPageUnit(g,UnitPixel));
        auto transform=captured->transform;check(GdipSetMatrixElements(m.value,transform.m11,transform.m12,transform.m21,transform.m22,transform.dx-area.x,transform.dy-area.y));
        check(GdipCreateRegion(&clip));check(GdipSetEmpty(clip));
        for(auto r:captured->clip) { RectF bounds(r.x-area.x,r.y-area.y,r.w,r.h);check(GdipCombineRegionRect(clip,&bounds,CombineModeUnion)); }
        check(GdipSetClipRegion(g,clip,CombineModeReplace));check(GdipSetWorldTransform(g,m.value));
        check(GdipSetCompositingMode(g,captured->copy?CompositingModeSourceCopy:CompositingModeSourceOver));
        SmoothingMode smoothing{};InterpolationMode interpolation{};PixelOffsetMode offset{};TextRenderingHint text{};
        GdipGetSmoothingMode(t->graphics,&smoothing);GdipGetInterpolationMode(t->graphics,&interpolation);
        GdipGetPixelOffsetMode(t->graphics,&offset);GdipGetTextRenderingHint(t->graphics,&text);
        GdipSetSmoothingMode(g,smoothing);GdipSetInterpolationMode(g,interpolation);GdipSetPixelOffsetMode(g,offset);GdipSetTextRenderingHint(g,text);
        result=operation(g);GdipFlush(g,FlushIntentionSync);
    } catch(...) { if(clip) GdipDeleteRegion(clip);if(g) GdipDeleteGraphics(g);if(bitmap) GdipDisposeImage(bitmap);throw; }
    if(clip) GdipDeleteRegion(clip);if(g) GdipDeleteGraphics(g);if(bitmap) GdipDisposeImage(bitmap);
    if(result==Ok) {
        SceneCommand command;command.kind=SceneCommand::Image;auto s=std::make_shared<SceneState>();
        s->copy=true;s->clip={area};command.state=s;command.pixels=raster;
        command.source={0,0,area.w,area.h};command.destination=area;append(t,std::move(command));
    }
    return result;
}
void fail(const char* name) {
    if(!failed) { char message[180]{};sprintf_s(message,"GPU canvas compatibility fallback at %s.\r\n",name);writeGpuStatus(message); }
    failed=true;
}
void finishNativeGraphics(GpGraphics*);
template<class T> void prepareNativeArgument(T value) {
    if constexpr(std::is_same_v<T,GpImage*>) materializeNative(value);
    else if constexpr(std::is_same_v<T,GpGraphics*>) {
        if(auto found=targets.find(value);found!=targets.end()&&found->second->image) materializeNative(found->second->image);
    }
}
template<class... T> void prepareNativeArguments(T... values) { if(onUI()&&!nativeAccess) (prepareNativeArgument(values),...); }
#define DRAW(name,params,args,...) \
    static decltype(&Gdiplus::DllExports::name) old_##name{}; \
    static GpStatus WINGDIPAPI gpu_##name params { \
        auto t=target(g);if(!t) { \
            try { prepareNativeArguments args; } catch(...) { return GenericError; } \
            auto result=old_##name args;if(result==Ok&&onUI()&&!nativeAccess) finishNativeGraphics(g);return result; } \
        t->compatibilityBounds={0,0,float(t->width),float(t->height)}; \
        try { __VA_ARGS__;return Ok; } catch(...) { \
            try { return compatibility(t,[&](GpGraphics* replacement) { auto g=replacement;return old_##name args; }); } \
            catch(...) { fail(#name);return old_##name args; } } \
    }
DRAW(GdipDrawPath,(GpGraphics* g,GpPen* pen,GpPath* p),(g,pen,p),stroke(t,pen,p))
DRAW(GdipFillPath,(GpGraphics* g,GpBrush* b,GpPath* p),(g,b,p),fill(t,b,p))
DRAW(GdipDrawLine,(GpGraphics* g,GpPen* pen,REAL x,REAL y,REAL x2,REAL y2),(g,pen,x,y,x2,y2),
    NativePath p;check(GdipAddPathLine(p.value,x,y,x2,y2));stroke(t,pen,p.value))
DRAW(GdipDrawLineI,(GpGraphics* g,GpPen* pen,INT x,INT y,INT x2,INT y2),(g,pen,x,y,x2,y2),
    NativePath p;check(GdipAddPathLineI(p.value,x,y,x2,y2));stroke(t,pen,p.value))
DRAW(GdipDrawLines,(GpGraphics* g,GpPen* pen,const GpPointF* v,INT n),(g,pen,v,n),
    NativePath p;check(GdipAddPathLine2(p.value,v,n));stroke(t,pen,p.value))
DRAW(GdipDrawBeziers,(GpGraphics* g,GpPen* pen,const GpPointF* v,INT n),(g,pen,v,n),
    NativePath p;check(GdipAddPathBeziers(p.value,v,n));stroke(t,pen,p.value))
DRAW(GdipDrawArc,(GpGraphics* g,GpPen* pen,REAL x,REAL y,REAL w,REAL h,REAL start,REAL sweep),(g,pen,x,y,w,h,start,sweep),
    NativePath p;check(GdipAddPathArc(p.value,x,y,w,h,start,sweep));stroke(t,pen,p.value))
#define RECTANGLE(name,object,type,add,render) \
    DRAW(name,(GpGraphics* g,object* b,type x,type y,type w,type h),(g,b,x,y,w,h), \
        NativePath p;check(add(p.value,x,y,w,h));render(t,b,p.value))
RECTANGLE(GdipDrawRectangle,GpPen,REAL,GdipAddPathRectangle,stroke)
RECTANGLE(GdipDrawRectangleI,GpPen,INT,GdipAddPathRectangleI,stroke)
RECTANGLE(GdipFillRectangle,GpBrush,REAL,GdipAddPathRectangle,fill)
RECTANGLE(GdipFillRectangleI,GpBrush,INT,GdipAddPathRectangleI,fill)
RECTANGLE(GdipDrawEllipse,GpPen,REAL,GdipAddPathEllipse,stroke)
RECTANGLE(GdipFillEllipse,GpBrush,REAL,GdipAddPathEllipse,fill)
DRAW(GdipDrawPolygon,(GpGraphics* g,GpPen* pen,const GpPointF* v,INT n),(g,pen,v,n),
    NativePath p;check(GdipAddPathPolygon(p.value,v,n));stroke(t,pen,p.value))
DRAW(GdipFillPolygon,(GpGraphics* g,GpBrush* b,const GpPointF* v,INT n,GpFillMode mode),(g,b,v,n,mode),
    NativePath p;check(GdipSetPathFillMode(p.value,mode));check(GdipAddPathPolygon(p.value,v,n));fill(t,b,p.value))
DRAW(GdipFillPolygonI,(GpGraphics* g,GpBrush* b,const GpPoint* v,INT n,GpFillMode mode),(g,b,v,n,mode),
    NativePath p;check(GdipSetPathFillMode(p.value,mode));check(GdipAddPathPolygonI(p.value,v,n));fill(t,b,p.value))
DRAW(GdipFillClosedCurve,(GpGraphics* g,GpBrush* b,const GpPointF* v,INT n),(g,b,v,n),
    NativePath p;check(GdipAddPathClosedCurve(p.value,v,n));fill(t,b,p.value))
DRAW(GdipFillRegion,(GpGraphics* g,GpBrush* b,GpRegion* r),(g,b,r),
    NativePath p;auto scans=regionScans(r,SceneMatrix{});for(auto s:scans) check(GdipAddPathRectangle(p.value,s.x,s.y,s.w,s.h));fill(t,b,p.value))
DRAW(GdipGraphicsClear,(GpGraphics* g,ARGB c),(g,c),
    reset(t);NativePath p;check(GdipAddPathRectangle(p.value,0,0,REAL(t->width),REAL(t->height)));
    SceneCommand cmd;auto s=std::make_shared<SceneState>();s->clip={{0,0,REAL(t->width),REAL(t->height)}};s->copy=true;
    cmd.state=s;cmd.path=path(p.value);cmd.brush.color=c;append(t,std::move(cmd)))
DRAW(GdipDrawImageRectRect,(GpGraphics* g,GpImage* i,REAL x,REAL y,REAL w,REAL h,REAL sx,REAL sy,REAL sw,REAL sh,
    GpUnit u,const GpImageAttributes* a,DrawImageAbort cb,VOID* data),(g,i,x,y,w,h,sx,sy,sw,sh,u,a,cb,data),
    image(t,i,{x,y,w,h},{sx,sy,sw,sh},u,a,cb))
DRAW(GdipDrawImageRectRectI,(GpGraphics* g,GpImage* i,INT x,INT y,INT w,INT h,INT sx,INT sy,INT sw,INT sh,
    GpUnit u,const GpImageAttributes* a,DrawImageAbort cb,VOID* data),(g,i,x,y,w,h,sx,sy,sw,sh,u,a,cb,data),
    image(t,i,{REAL(x),REAL(y),REAL(w),REAL(h)},{REAL(sx),REAL(sy),REAL(sw),REAL(sh)},u,a,cb))
DRAW(GdipDrawImageRect,(GpGraphics* g,GpImage* i,REAL x,REAL y,REAL w,REAL h),(g,i,x,y,w,h),
    UINT sw{},sh{};check(GdipGetImageWidth(i,&sw));check(GdipGetImageHeight(i,&sh));image(t,i,{x,y,w,h},{0,0,REAL(sw),REAL(sh)},UnitPixel,nullptr,nullptr))
DRAW(GdipDrawImageRectI,(GpGraphics* g,GpImage* i,INT x,INT y,INT w,INT h),(g,i,x,y,w,h),
    UINT sw{},sh{};check(GdipGetImageWidth(i,&sw));check(GdipGetImageHeight(i,&sh));image(t,i,{REAL(x),REAL(y),REAL(w),REAL(h)},{0,0,REAL(sw),REAL(sh)},UnitPixel,nullptr,nullptr))
DRAW(GdipDrawImageI,(GpGraphics* g,GpImage* i,INT x,INT y),(g,i,x,y),
    UINT sw{},sh{};check(GdipGetImageWidth(i,&sw));check(GdipGetImageHeight(i,&sh));image(t,i,{REAL(x),REAL(y),REAL(sw),REAL(sh)},{0,0,REAL(sw),REAL(sh)},UnitPixel,nullptr,nullptr))
DRAW(GdipDrawImage,(GpGraphics* g,GpImage* i,REAL x,REAL y),(g,i,x,y),
    UINT sw{},sh{};check(GdipGetImageWidth(i,&sw));check(GdipGetImageHeight(i,&sh));image(t,i,{x,y,REAL(sw),REAL(sh)},{0,0,REAL(sw),REAL(sh)},UnitPixel,nullptr,nullptr))
DRAW(GdipDrawImageFX,(GpGraphics* g,GpImage* i,RectF* source,GpMatrix* mapping,CGpEffect* effect,GpImageAttributes* a,GpUnit unit),
    (g,i,source,mapping,effect,a,unit),
    std::shared_ptr<const SceneEffect> description;
    try { if(effect) description=describeEffect(effect); } catch(...) { materializeNative(i);throw; }
    if(unit!=UnitPixel||!source) { materializeNative(i);reject(); }
    ImageType type{};check(GdipGetImageType(i,&type));if(type!=ImageTypeBitmap) reject();
    auto nativeTransform=mapping?brushTransform(mapping):SceneMatrix{};
    image(t,i,{source->X,source->Y,source->Width,source->Height},{source->X,source->Y,source->Width,source->Height},unit,a,nullptr);
    auto& command=t->commands.back();auto updated=std::make_shared<SceneState>(*command.state);auto original=updated->transform;
    updated->transform={nativeTransform.m11*original.m11+nativeTransform.m12*original.m21,
        nativeTransform.m11*original.m12+nativeTransform.m12*original.m22,
        nativeTransform.m21*original.m11+nativeTransform.m22*original.m21,
        nativeTransform.m21*original.m12+nativeTransform.m22*original.m22,
        nativeTransform.dx*original.m11+nativeTransform.dy*original.m21+original.dx,
        nativeTransform.dx*original.m12+nativeTransform.dy*original.m22+original.dy};
    command.state=updated;if(description) command.effects.insert(command.effects.begin(),description);t->snapshot.reset())
float unitSize(Unit u,float dpi) {
    switch(u) { case UnitWorld:case UnitPixel:case UnitDisplay:return 1;case UnitPoint:return dpi/72;
        case UnitInch:return dpi;case UnitDocument:return dpi/300;case UnitMillimeter:return dpi/25.4f;default:reject(); }
}
DRAW(GdipDrawString,(GpGraphics* g,const WCHAR* text,INT n,const GpFont* font,const RectF* layout,const GpStringFormat* format,const GpBrush* b),
    (g,text,n,font,layout,format,b),
    GpFontFamily* family{};REAL size{},dpi{};INT style{};Unit fontUnit{},pageUnit{};
    auto nativeFont=const_cast<GpFont*>(font);check(GdipGetFamily(nativeFont,&family));
    try { check(GdipGetFontSize(nativeFont,&size));check(GdipGetFontStyle(nativeFont,&style));check(GdipGetFontUnit(nativeFont,&fontUnit));
        check(GdipGetDpiY(g,&dpi));check(GdipGetPageUnit(g,&pageUnit));
        NativePath p;check(GdipAddPathString(p.value,text,n,family,style,size*unitSize(fontUnit,dpi)/unitSize(pageUnit,dpi),layout,format));
        fill(t,const_cast<GpBrush*>(b),p.value);
    } catch(...) { GdipDeleteFontFamily(family);throw; } GdipDeleteFontFamily(family))
static decltype(&GdipGetDC) old_GdipGetDC{};
static decltype(&GdipReleaseDC) old_GdipReleaseDC{};
struct DcSegment { GpGraphics* graphics{};HDC native{};std::shared_ptr<Target> owner;std::shared_ptr<const SceneState> state; };
static std::unordered_map<HDC,DcSegment> dcSegments;
static GpStatus WINGDIPAPI gpu_GdipGetDC(GpGraphics* g,HDC* dc) {
    auto owner=target(g);if(!owner||!dc) {
        if(onUI()&&!nativeAccess) if(auto found=targets.find(g);found!=targets.end()&&found->second->image) {
            try { materializeNative(found->second->image); } catch(...) { return GenericError; }
        }
        return old_GdipGetDC(g,dc);
    }
    HDC native{},recording{};GpStatus result{};
    try {
        auto captured=std::make_shared<SceneState>(*state(owner));captured->transform={};
        // GDI+ exposes HDC coordinates in device space, independent of the
        // GDI+ world/page transform. Native callers already subtract port origin.
        result=old_GdipGetDC(g,&native);if(result!=Ok) return result;
        recording=CreateEnhMetaFileW(native,nullptr,nullptr,nullptr);if(!recording) reject();
        for(UINT type:{OBJ_PEN,OBJ_BRUSH,OBJ_FONT,OBJ_PAL}) {
            if(auto selected=GetCurrentObject(native,type)) SelectObject(recording,selected);
        }
        SetBkMode(recording,GetBkMode(native));SetBkColor(recording,GetBkColor(native));SetTextColor(recording,GetTextColor(native));
        SetTextAlign(recording,GetTextAlign(native));SetPolyFillMode(recording,GetPolyFillMode(native));SetROP2(recording,GetROP2(native));
        SetMapMode(recording,GetMapMode(native));POINT origin{},position{};SIZE extent{};
        if(GetWindowOrgEx(native,&origin)) SetWindowOrgEx(recording,origin.x,origin.y,nullptr);
        if(GetWindowExtEx(native,&extent)) SetWindowExtEx(recording,extent.cx,extent.cy,nullptr);
        if(GetViewportOrgEx(native,&origin)) SetViewportOrgEx(recording,origin.x,origin.y,nullptr);
        if(GetViewportExtEx(native,&extent)) SetViewportExtEx(recording,extent.cx,extent.cy,nullptr);
        if(GetBrushOrgEx(native,&origin)) SetBrushOrgEx(recording,origin.x,origin.y,nullptr);
        SetGraphicsMode(recording,GetGraphicsMode(native));XFORM world{};
        if(GetGraphicsMode(native)==GM_ADVANCED&&GetWorldTransform(native,&world)) SetWorldTransform(recording,&world);
        if(GetCurrentPositionEx(native,&position)) MoveToEx(recording,position.x,position.y,nullptr);
        dcSegments[recording]={g,native,owner,captured};*dc=recording;return Ok;
    } catch(...) {
        if(recording) { auto file=CloseEnhMetaFile(recording);if(file) DeleteEnhMetaFile(file); }
        if(native) old_GdipReleaseDC(g,native);fail("GdipGetDC capture");return old_GdipGetDC(g,dc);
    }
}
static GpStatus WINGDIPAPI gpu_GdipReleaseDC(GpGraphics* g,HDC dc) {
    invalidateState(g);
    auto found=dcSegments.find(dc);if(found==dcSegments.end()) {
        auto result=old_GdipReleaseDC(g,dc);if(result==Ok&&onUI()&&!nativeAccess) finishNativeGraphics(g);return result;
    }
    auto segment=std::move(found->second);dcSegments.erase(found);auto handle=CloseEnhMetaFile(dc);
    auto result=old_GdipReleaseDC(segment.graphics,segment.native);
    if(!handle) { fail("HDC record finalization");return GenericError; }
    try {
        ENHMETAHEADER header{};GetEnhMetaFileHeader(handle,sizeof(header),&header);
        if(header.rclBounds.right>=header.rclBounds.left&&header.rclBounds.bottom>=header.rclBounds.top) {
            SceneCommand command;command.kind=SceneCommand::Metafile;command.state=segment.state;
            command.metafile=copyMetafile(handle);command.destination=command.metafile->bounds;command.source={0,0,1,1};
            auto commands=translateGpuMetafile(command,segment.owner->width,segment.owner->height,nextScene);
            for(auto& item:commands) append(segment.owner,std::move(item));
        }
    } catch(...) {
        try {
            segment.owner->compatibilityBounds={0,0,float(segment.owner->width),float(segment.owner->height)};
            compatibility(segment.owner,[&](GpGraphics* replacement) {
                HDC native{};auto status=old_GdipGetDC(replacement,&native);if(status!=Ok) return status;
                ENHMETAHEADER header{};GetEnhMetaFileHeader(handle,sizeof(header),&header);
                RECT dest={header.rclBounds.left,header.rclBounds.top,header.rclBounds.right+1,header.rclBounds.bottom+1};
                BOOL drawn=PlayEnhMetaFile(native,handle,&dest);auto released=old_GdipReleaseDC(replacement,native);
                return drawn?released:GenericError;
            });
        } catch(...) { fail("HDC compatibility segment"); }
    }
    DeleteEnhMetaFile(handle);return result;
}
static decltype(&GdipEnumerateMetafileDestPointI) old_GdipEnumerateMetafileDestPointI{};
static GpStatus WINGDIPAPI gpu_GdipEnumerateMetafileDestPointI(GpGraphics* g,const GpMetafile* m,
        const Gdiplus::Point& p,EnumerateMetafileProc cb,VOID* data,const GpImageAttributes* attributes) {
    if(auto owner=target(g)) {
        try { return compatibility(owner,[&](GpGraphics* replacement) { return old_GdipEnumerateMetafileDestPointI(replacement,m,p,cb,data,attributes); }); }
        catch(...) { fail("GdipEnumerateMetafileDestPointI"); }
    }
    return old_GdipEnumerateMetafileDestPointI(g,m,p,cb,data,attributes);
}
#undef RECTANGLE
#undef DRAW
static decltype(&GdipDisposeImage) oldDisposeImage{};
static GpStatus WINGDIPAPI disposeImage(GpImage* i) {
    if(onUI()) { imageTargets.erase(i);pixels.erase(i);imageVersions.erase(i);metafileImages.erase(i);nativeVersions.erase(i); }return oldDisposeImage(i);
}
static decltype(&GdipDeleteGraphics) oldDeleteGraphics{};
static GpStatus WINGDIPAPI deleteGraphics(GpGraphics* g) {
    if(onUI()) {
        targets.erase(g);hoverBindings.erase(g);fixedUiBindings.erase(g);
    }
    return oldDeleteGraphics(g);
}
static std::unordered_map<BitmapData*,std::pair<GpBitmap*,UINT>> lockedImages;
static GpStatus WINGDIPAPI lockBits(GpBitmap* image,const Rect* r,UINT flags,INT format,BitmapData* data) {
    try { materializeNative(reinterpret_cast<GpImage*>(image)); } catch(...) { return GenericError; }
    auto result=oldLockBits(image,r,flags,format,data);if(result==Ok&&onUI()&&!nativeAccess) lockedImages[data]={image,flags};return result;
}
void invalidateImage(GpImage* image) {
    pixels.erase(image);imageVersions.erase(image);metafileImages.erase(image);nativeVersions.erase(image);
    if(auto i=imageTargets.find(image);i!=imageTargets.end()) if(auto owner=i->second.lock()) {
        reset(owner);owner->nativeSerial=0;owner->nativeDirty=true;
    }
}
void finishNativeGraphics(GpGraphics* graphics) {
    if(auto found=targets.find(graphics);found!=targets.end()&&found->second->image) {
        try { invalidateImage(found->second->image); }
        catch(...) { pixels.erase(found->second->image);imageVersions.erase(found->second->image);
            imageTargets.erase(found->second->image);reset(found->second); }
    }
}
static GpStatus WINGDIPAPI unlockBits(GpBitmap* image,BitmapData* data) {
    auto result=oldUnlockBits(image,data);
    if(result==Ok&&onUI()&&!nativeAccess) {
        if(auto found=lockedImages.find(data);found!=lockedImages.end()) {
            if(found->second.second&ImageLockModeWrite) invalidateImage(reinterpret_cast<GpImage*>(image));lockedImages.erase(found);
        }
    }
    return result;
}
static decltype(&GdipCreateImageAttributes) oldCreateAttributes{};
static GpStatus WINGDIPAPI createAttributes(GpImageAttributes** out) {
    auto r=oldCreateAttributes(out);if(r==Ok&&onUI()) imageAttributes[*out]={};return r;
}
static decltype(&GdipCloneImageAttributes) oldCloneAttributes{};
static GpStatus WINGDIPAPI cloneAttributes(const GpImageAttributes* a,GpImageAttributes** out) {
    auto r=oldCloneAttributes(a,out);if(r==Ok&&onUI()) {
        if(auto i=imageAttributes.find(a);i!=imageAttributes.end()) imageAttributes[*out]=i->second;else imageAttributes[*out].unsupported=true;
    }return r;
}
static decltype(&GdipDisposeImageAttributes) oldDisposeAttributes{};
static GpStatus WINGDIPAPI disposeAttributes(GpImageAttributes* a) { if(onUI()) imageAttributes.erase(a);return oldDisposeAttributes(a); }
int attributeCategory(ColorAdjustType category) { return category==ColorAdjustTypeDefault?0:category==ColorAdjustTypeBitmap?1:-1; }
#define ATTRIBUTE(name,params,args,...) \
    static decltype(&name) old_##name{}; \
    static GpStatus WINGDIPAPI gpu_##name params { auto r=old_##name args; \
        if(r==Ok&&onUI()) { int slot=attributeCategory(category);if(slot>=0) { auto& value=imageAttributes[a]; __VA_ARGS__; } }return r; }
ATTRIBUTE(GdipSetImageAttributesColorMatrix,(GpImageAttributes* a,ColorAdjustType category,BOOL enable,const ColorMatrix* matrix,const ColorMatrix* gray,ColorMatrixFlags flags),
    (a,category,enable,matrix,gray,flags),
    if(enable) {
        auto effect=std::make_shared<SceneEffect>();effect->kind=SceneEffect::Matrix;memcpy(effect->matrix.data(),matrix,sizeof(ColorMatrix));effect->flags=int(flags);
        value.effects[slot][SceneEffect::Matrix]=effect;
        if(flags==ColorMatrixFlagsAltGray&&gray) memcpy(effect->grayMatrix.data(),gray,sizeof(ColorMatrix));
        else if(flags==ColorMatrixFlagsAltGray) effect->grayMatrix=effect->matrix;
        value.effects[slot][100]={};
    } else { value.effects[slot][SceneEffect::Matrix]={};value.effects[slot][100]={}; })
ATTRIBUTE(GdipSetImageAttributesGamma,(GpImageAttributes* a,ColorAdjustType category,BOOL enable,REAL gamma),(a,category,enable,gamma),
    auto effect=std::make_shared<SceneEffect>();effect->kind=SceneEffect::Gamma;effect->amount=gamma;value.effects[slot][SceneEffect::Gamma]=enable?effect:nullptr)
ATTRIBUTE(GdipSetImageAttributesThreshold,(GpImageAttributes* a,ColorAdjustType category,BOOL enable,REAL threshold),(a,category,enable,threshold),
    auto effect=std::make_shared<SceneEffect>();effect->kind=SceneEffect::Threshold;effect->amount=threshold;value.effects[slot][SceneEffect::Threshold]=enable?effect:nullptr)
ATTRIBUTE(GdipSetImageAttributesColorKeys,(GpImageAttributes* a,ColorAdjustType category,BOOL enable,ARGB low,ARGB high),(a,category,enable,low,high),
    auto effect=std::make_shared<SceneEffect>();effect->kind=SceneEffect::ColorKey;effect->low=low;effect->high=high;value.effects[slot][SceneEffect::ColorKey]=enable?effect:nullptr)
ATTRIBUTE(GdipSetImageAttributesRemapTable,(GpImageAttributes* a,ColorAdjustType category,BOOL enable,UINT count,const ColorMap* map),(a,category,enable,count,map),
    auto effect=std::make_shared<SceneEffect>();effect->kind=SceneEffect::Remap;
    if(enable&&count>64) value.unsupported=true;
    else { if(enable) for(UINT i=0;i<count;++i) effect->remap.push_back({map[i].oldColor.GetValue(),map[i].newColor.GetValue()});
        value.effects[slot][SceneEffect::Remap]=enable?effect:nullptr; })
ATTRIBUTE(GdipResetImageAttributes,(GpImageAttributes* a,ColorAdjustType category),(a,category),value.effects[slot].clear();value.noop[slot]=-1)
ATTRIBUTE(GdipSetImageAttributesToIdentity,(GpImageAttributes* a,ColorAdjustType category),(a,category),
    value.effects[slot].clear();for(int key:{int(SceneEffect::Remap),int(SceneEffect::ColorKey),int(SceneEffect::Matrix),100,int(SceneEffect::Gamma),int(SceneEffect::Threshold)}) value.effects[slot][key]={};value.noop[slot]=false)
ATTRIBUTE(GdipSetImageAttributesNoOp,(GpImageAttributes* a,ColorAdjustType category,BOOL enable),(a,category,enable),value.noop[slot]=enable!=0)
ATTRIBUTE(GdipSetImageAttributesOutputChannel,(GpImageAttributes* a,ColorAdjustType category,BOOL enable,ColorChannelFlags flags),(a,category,enable,flags),if(enable) value.unsupported=true)
ATTRIBUTE(GdipSetImageAttributesOutputChannelColorProfile,(GpImageAttributes* a,ColorAdjustType category,BOOL enable,const WCHAR* file),(a,category,enable,file),if(enable) value.unsupported=true)
#undef ATTRIBUTE
static decltype(&GdipSetImageAttributesWrapMode) oldWrapAttributes{};
static GpStatus WINGDIPAPI wrapAttributes(GpImageAttributes* a,WrapMode wrap,ARGB color,BOOL clamp) {
    auto r=oldWrapAttributes(a,wrap,color,clamp);if(r==Ok&&onUI()&&wrap!=WrapModeClamp) imageAttributes[a].unsupported=true;return r;
}
static decltype(&GdipSetImageAttributesICMMode) oldIcmAttributes{};
static GpStatus WINGDIPAPI icmAttributes(GpImageAttributes* a,BOOL enabled) {
    auto r=oldIcmAttributes(a,enabled);if(r==Ok&&onUI()&&enabled) imageAttributes[a].unsupported=true;return r;
}
static decltype(&GdipCreateEffect) oldCreateEffect{};
static Status WINGDIPAPI createEffect(const GUID id,CGpEffect** out) {
    auto r=oldCreateEffect(id,out);if(r==Ok&&onUI()) effects[*out].id=id;return r;
}
static decltype(&GdipDeleteEffect) oldDeleteEffect{};
static Status WINGDIPAPI deleteEffect(CGpEffect* effect) { if(onUI()) effects.erase(effect);return oldDeleteEffect(effect); }
static decltype(&GdipSetEffectParameters) oldSetEffect{};
static Status WINGDIPAPI setEffect(CGpEffect* effect,const VOID* parameters,const UINT size) {
    auto r=oldSetEffect(effect,parameters,size);if(r==Ok&&onUI()) {
        auto& info=effects[effect];info.description.reset();info.parameters.resize(size);if(size) memcpy(info.parameters.data(),parameters,size);
    }return r;
}
static GpStatus WINGDIPAPI applyEffect(GpBitmap* bitmap,CGpEffect* effect,RECT* roi,BOOL aux,VOID** data,INT* size) {
    auto image=reinterpret_cast<GpImage*>(bitmap);
    if(onUI()&&!nativeAccess&&!aux) {
        try {
            auto description=describeEffect(effect);UINT w{},h{};check(GdipGetImageWidth(image,&w));check(GdipGetImageHeight(image,&h));
            auto version=makeGpuScene();version->serial=++nextScene;version->width=int(w);version->height=int(h);
            SceneCommand original;original.kind=SceneCommand::Image;original.image=imageScene(image);if(!original.image) original.pixels=imagePixels(image);
            auto full=std::make_shared<SceneState>();full->copy=true;full->clip={{0,0,float(w),float(h)}};
            original.state=full;original.source=original.destination={0,0,float(w),float(h)};version->commands.push_back(original);
            auto filtered=original;auto clipped=std::make_shared<SceneState>(*full);
            if(roi) clipped->clip={{float(roi->left),float(roi->top),float(roi->right-roi->left),float(roi->bottom-roi->top)}};
            filtered.state=clipped;filtered.effects.push_back(description);version->commands.push_back(std::move(filtered));
            imageVersions[image]=version;pixels.erase(image);
            if(auto i=imageTargets.find(image);i!=imageTargets.end()) if(auto owner=i->second.lock()) reset(owner,version);
            if(data) *data=nullptr;if(size) *size=0;return Ok;
        } catch(...) { /* Preserve the exact native algorithm for unsupported effects. */ }
    }
    try { materializeNative(image); } catch(...) { return GenericError; }
    auto r=oldApplyEffect(bitmap,effect,roi,aux,data,size);if(r==Ok&&onUI()) invalidateImage(image);return r;
}
static decltype(&GdipBitmapGetPixel) oldGetPixel{};
static GpStatus WINGDIPAPI getPixel(GpBitmap* bitmap,INT x,INT y,ARGB* color) {
    try { materializeNative(reinterpret_cast<GpImage*>(bitmap)); } catch(...) { return GenericError; }return oldGetPixel(bitmap,x,y,color);
}
static decltype(&GdipBitmapSetPixel) oldSetPixel{};
static GpStatus WINGDIPAPI setPixel(GpBitmap* bitmap,INT x,INT y,ARGB color) {
    auto image=reinterpret_cast<GpImage*>(bitmap);try { materializeNative(image); } catch(...) { return GenericError; }
    auto r=oldSetPixel(bitmap,x,y,color);if(r==Ok&&onUI()) invalidateImage(image);return r;
}
#define IMAGE_READ(name,params,args) \
    static decltype(&name) old_##name{}; \
    static GpStatus WINGDIPAPI gpu_##name params { try { materializeNative(i); } catch(...) { return GenericError; }return old_##name args; }
IMAGE_READ(GdipSaveImageToFile,(GpImage* i,const WCHAR* file,const CLSID* encoder,const EncoderParameters* parameters),(i,file,encoder,parameters))
IMAGE_READ(GdipSaveImageToStream,(GpImage* i,IStream* stream,const CLSID* encoder,const EncoderParameters* parameters),(i,stream,encoder,parameters))
IMAGE_READ(GdipCreateHBITMAPFromBitmap,(GpBitmap* i,HBITMAP* output,ARGB background),(i,output,background))
#undef IMAGE_READ
static decltype(&GdipCloneImage) oldCloneImage{};
static GpStatus WINGDIPAPI cloneImage(GpImage* source,GpImage** output) {
    auto r=oldCloneImage(source,output);if(r==Ok&&onUI()) if(auto version=imageScene(source)) imageVersions[*output]=version;return r;
}
static decltype(&GdipCloneBitmapAreaI) oldCloneAreaI{};
static decltype(&GdipCloneBitmapArea) oldCloneArea{};
void cloneVersion(GpBitmap* source,GpBitmap* output,float x,float y,float w,float h) {
    auto version=imageScene(reinterpret_cast<GpImage*>(source));if(!version) return;
    UINT width{},height{};check(GdipGetImageWidth(reinterpret_cast<GpImage*>(output),&width));check(GdipGetImageHeight(reinterpret_cast<GpImage*>(output),&height));
    auto cropped=makeGpuScene();cropped->serial=++nextScene;cropped->width=int(width);cropped->height=int(height);
    SceneCommand command;command.kind=SceneCommand::Image;command.image=version;
    auto s=std::make_shared<SceneState>();s->copy=true;s->clip={{0,0,float(width),float(height)}};command.state=s;
    command.source={x,y,w,h};command.destination={0,0,float(width),float(height)};cropped->commands.push_back(std::move(command));
    imageVersions[reinterpret_cast<GpImage*>(output)]=cropped;
}
static GpStatus WINGDIPAPI cloneAreaI(INT x,INT y,INT w,INT h,INT format,GpBitmap* source,GpBitmap** output) {
    const bool preserve=format==PixelFormat32bppARGB||format==PixelFormat32bppPARGB;
    if(!preserve) try { materializeNative(reinterpret_cast<GpImage*>(source)); } catch(...) { return GenericError; }
    auto r=oldCloneAreaI(x,y,w,h,format,source,output);
    if(r==Ok&&onUI()&&preserve) try { cloneVersion(source,*output,float(x),float(y),float(w),float(h)); } catch(...) {
        GdipDisposeImage(reinterpret_cast<GpImage*>(*output));*output=nullptr;return GenericError;
    }return r;
}
static GpStatus WINGDIPAPI cloneArea(REAL x,REAL y,REAL w,REAL h,INT format,GpBitmap* source,GpBitmap** output) {
    const bool preserve=format==PixelFormat32bppARGB||format==PixelFormat32bppPARGB;
    if(!preserve) try { materializeNative(reinterpret_cast<GpImage*>(source)); } catch(...) { return GenericError; }
    auto r=oldCloneArea(x,y,w,h,format,source,output);
    if(r==Ok&&onUI()&&preserve) try { cloneVersion(source,*output,x,y,w,h); } catch(...) {
        GdipDisposeImage(reinterpret_cast<GpImage*>(*output));*output=nullptr;return GenericError;
    }return r;
}
static decltype(&GdipGetImageGraphicsContext) oldImageGraphics{};
static GpStatus WINGDIPAPI imageGraphics(GpImage* image,GpGraphics** output) {
    // Arbitrary native consumers receive coherent pixels. During a document
    // capture the image stays GPU-authoritative and its drawing is intercepted.
    if(onUI()&&!nativeAccess&&!contextWindow()) {
        try { materializeNative(image); } catch(...) { return GenericError; }
    }
    auto result=oldImageGraphics(image,output);
    if(result==Ok&&onUI()&&!nativeAccess) {
        auto version=imageScene(image);
        if(contextWindow()||version) {
            std::shared_ptr<Target> owner;
            if(auto found=imageTargets.find(image);found!=imageTargets.end()) owner=found->second.lock();
            if(!owner) { owner=std::make_shared<Target>();owner->base=version; }
            owner->graphics=*output;owner->image=image;if(contextWindow()) owner->window=contextWindow();
            UINT w{},h{};GdipGetImageWidth(image,&w);GdipGetImageHeight(image,&h);owner->width=int(w);owner->height=int(h);
            targets[*output]=owner;imageTargets[image]=owner;
        }
    }
    return result;
}
static decltype(&GdipBitmapCreateApplyEffect) oldCreateApplyEffect{};
static GpStatus WINGDIPAPI createApplyEffect(GpBitmap** input,INT count,CGpEffect* effect,RECT* roi,
        RECT* outputRect,GpBitmap** output,BOOL auxiliary,VOID** data,INT* size) {
    try { if(input) for(int i=0;i<count;++i) materializeNative(reinterpret_cast<GpImage*>(input[i])); }
    catch(...) { return GenericError; }
    return oldCreateApplyEffect(input,count,effect,roi,outputRect,output,auxiliary,data,size);
}
template<class F> void intercept(const char* name,F replacement,F& original) {
    HMODULE library=GetModuleHandleW(L"gdiplus.dll");
    auto address=GetProcAddress(library,name);
    if(!address) throw std::runtime_error(std::string("GPU primitive export unavailable: ")+name);
    auto status=MH_CreateHook(address,replacement,reinterpret_cast<void**>(&original));
    if(status!=MH_OK) throw std::runtime_error(std::string("GPU primitive hook: ")+name+": "+MH_StatusToString(status));
}
template<class F> void interceptIfExported(const char* name,F replacement,F& original) {
    // Some flat APIs are declared in the SDK but absent from Windows GDI+.
    // Without an export no native caller can use them, so no hook is needed.
    auto address=GetProcAddress(GetModuleHandleW(L"gdiplus.dll"),name);if(!address) return;
    auto status=MH_CreateHook(address,replacement,reinterpret_cast<void**>(&original));
    if(status==MH_ERROR_UNSUPPORTED_FUNCTION) return; // SDK compatibility stub, with no drawing implementation.
    if(status!=MH_OK) throw std::runtime_error(std::string("GPU optional hook: ")+name+": "+MH_StatusToString(status));
}
}
bool beginGpuCanvas(HWND w,Obj port,Graphics* destination,bool preserve) {
    if(disabled[w]||recordingWindow||(trackingDoc&&docWindow(trackingDoc)!=w)) return false;
    failed=false;recordingWindow=w;
    try {
        std::shared_ptr<const GpuScene> previous;
        const auto client=clientRect(w);
        if(preserve) if(auto old=canvases.find(w);old!=canvases.end()&&
            old->second->width==client.r&&old->second->height==client.b)
            previous=snapshot(old->second);
        bind(port,oldGetGraphics(port),w,true);
        auto t=bind(nullptr,destination,w,true);if(!t) { recordingWindow=nullptr;return false; }
        // A clipped native repaint changes only its dirty region. Retain the
        // immutable GPU background elsewhere instead of publishing alpha holes.
        reset(t,std::move(previous));return true;
    } catch(...) { recordingWindow=nullptr;failed=true;return false; }
}
std::shared_ptr<const GpuScene> endGpuCanvas() {
    const HWND w=recordingWindow;recordingWindow=nullptr;
    if(failed) { discardGpuCanvas(w);return {}; }
    auto scene=snapshot(canvases.at(w));
    // A nested native repaint can contain the current drag highlight. Never
    // turn that transient frame into the background restored on every move.
    return scene;
}
bool gpuCanvasRecording() { return recordingWindow!=nullptr; }
bool beginGpuFixedUi(HWND w,Obj port,Graphics* destination) {
    if(!onUI()||!w||!port||!destination||fixedUiTarget||hoverTarget||recordingWindow)return false;
    try {
        auto layer=std::make_shared<Target>();const auto rect=clientRect(w);
        layer->window=w;layer->graphics=at<GpGraphics*>(destination,0);
        layer->width=rect.r;layer->height=rect.b;
        fixedUiPreviousFailure=failed;fixedUiBindings.clear();failed=false;
        fixedUiTarget=std::move(layer);return true;
    } catch(...) {return false;}
}
std::shared_ptr<const GpuScene> endGpuFixedUi() noexcept {
    std::shared_ptr<const GpuScene> scene;
    if(fixedUiTarget&&!failed)try {scene=snapshot(fixedUiTarget);}catch(...) {}
    for(auto& [graphics,previous]:fixedUiBindings) {
        const auto current=targets.find(graphics);
        // A native delete may have removed this Graphics during capture.
        // Never recreate its stale binding or allocate during noexcept cleanup.
        if(current==targets.end()||current->second!=fixedUiTarget)continue;
        if(previous)current->second=std::move(previous);else targets.erase(current);
    }
    fixedUiBindings.clear();fixedUiTarget.reset();failed=fixedUiPreviousFailure;
    return scene;
}
bool gpuBackgroundRecording() { return backgroundWindow!=nullptr; }
bool gpuOffscreenAvailable(Obj doc) {
    if(!onUI()||!doc||failed) return false;
    // The getter is the same native +0x120 accessor used by DrawContents' blit.
    // Never manufacture an offscreen port or bypass its native validity checks.
    Obj port=vf<Obj(*)(Obj)>(doc,0x120)(doc);if(!port) return false;
    Obj image=at<Obj>(port,0x160);if(!image) return false;
    auto found=imageTargets.find(at<GpImage*>(image,8));if(found==imageTargets.end()) return false;
    auto owner=found->second.lock();
    return owner&&owner->width>0&&owner->height>0&&
        (owner->nativeDirty||owner->base||!owner->commands.empty());
}
bool beginGpuIdle(Obj doc) {
    if(!onUI()||!doc||contextWindow()||failed) return false;
    const HWND w=docWindow(doc);
    if(!w||disabled[w]||!canvases.contains(w)) return false;
    // Native idle still creates and configures every port itself. Only its
    // graphics operations change destination, including DrawLassoOffscreen.
    idleWindow=w;return true;
}
bool endGpuIdle() {
    idleWindow=nullptr;return !failed;
}
static void restoreHoverBindings() noexcept {
    for(auto& [graphics,previous]:hoverBindings) {
        const auto current=targets.find(graphics);
        // Graphics deleted during capture must stay absent. Cleanup restores
        // existing entries only and cannot allocate or resurrect a dead port.
        if(current==targets.end()||current->second!=hoverTarget)continue;
        if(previous)current->second=std::move(previous);else targets.erase(current);
    }
    hoverBindings.clear();
}
bool beginGpuHover(HWND w,Obj port) {
    if(!onUI()||!w||!port||hoverTarget||fixedUiTarget||(trackingDoc&&docWindow(trackingDoc)!=w)||backgroundWindow||disabled[w]||failed) return false;
    if(recordingWindow&&recordingWindow!=w) return false;
    try {
        auto* graphics=oldGetGraphics(port);if(!graphics) return false;
        auto layer=std::make_shared<Target>();const auto r=clientRect(w);
        layer->window=w;layer->graphics=at<GpGraphics*>(graphics,0);layer->width=r.r;layer->height=r.b;
        hoverPreviousFailure=failed;hoverBindings.clear();hoverTarget=std::move(layer);hoverPort=port;
        bind(port,graphics,w,true);return true;
    } catch(const std::bad_alloc&) {
        if(hoverTarget) {restoreHoverBindings();hoverTarget.reset();hoverPort=nullptr;failed=hoverPreviousFailure;}
        return false;
    }
}
std::shared_ptr<const GpuScene> endGpuHover(RectI* bounds) noexcept {
    restoreHoverBindings();
    hoverPort=nullptr;
    auto layer=std::move(hoverTarget);const bool captureFailed=failed;failed=hoverPreviousFailure;
    if(!layer||captureFailed) return {};
    try {
        RectI crop{0,0,layer->height,layer->width};const auto area=layer->hoverBounds;
        // Tiny hover shapes should not allocate or blend a canvas-sized image
        // on every animation frame. Retain full bounds for unknown commands.
        bool paths=layer->hoverMeasured==layer->commands.size()&&layer->hoverMeasured>0&&
            std::isfinite(area.x)&&std::isfinite(area.y)&&std::isfinite(area.w)&&std::isfinite(area.h);
        for(const auto& cmd:layer->commands) paths=paths&&cmd.kind==SceneCommand::Path&&bool(cmd.state);
        if(paths) crop={int(std::clamp(std::floor(double(area.y)),0.0,double(layer->height))),
            int(std::clamp(std::floor(double(area.x)),0.0,double(layer->width))),
            int(std::clamp(std::ceil(double(area.y)+area.h),0.0,double(layer->height))),
            int(std::clamp(std::ceil(double(area.x)+area.w),0.0,double(layer->width)))};
        if(!valid(crop)) { crop={0,0,1,1};layer->commands.clear(); }
        auto scene=makeGpuScene();scene->serial=++nextScene;
        scene->width=crop.r-crop.l;scene->height=crop.b-crop.t;scene->commands=std::move(layer->commands);
        if(crop.l||crop.t) for(auto& cmd:scene->commands) {
            auto state=std::make_shared<SceneState>(*cmd.state);
            state->transform.dx-=float(crop.l);state->transform.dy-=float(crop.t);
            for(auto& clip:state->clip) { clip.x-=float(crop.l);clip.y-=float(crop.t); }
            cmd.state=std::move(state);
        }
        if(bounds) *bounds=crop;
        return scene;
    } catch(...) { return {}; }
}
bool gpuHoverRecording() { return bool(hoverTarget); }
bool beginGpuCommandCapture(Obj port) {
    if(!onUI()||!port||!contextWindow()||commandCaptureTarget||hoverTarget||fixedUiTarget||trackingDoc||failed)
        return false;
    try {
        auto* graphics=getGraphics(port);if(!graphics)return false;
        auto found=targets.find(at<GpGraphics*>(graphics,0));
        if(found==targets.end()||found->second->window!=contextWindow())return false;
        commandCapture.clear();commandCaptureFailed=false;commandCaptureTarget=found->second;
        return true;
    } catch(...) {return false;}
}
std::shared_ptr<const GpuScene> endGpuCommandCapture() noexcept {
    auto target=std::move(commandCaptureTarget);
    if(!target||failed||commandCaptureFailed||commandCapture.empty()) {commandCapture.clear();return {};}
    try {
        auto scene=makeGpuScene();scene->width=target->width;scene->height=target->height;
        scene->serial=++nextScene;scene->commands=std::move(commandCapture);return scene;
    } catch(...) {commandCapture.clear();return {};}
}
bool gpuTrackingRecording() { return trackingDoc!=nullptr&&!failed&&!disabled[docWindow(trackingDoc)]; }
bool gpuTrackingActive() { return trackingDoc!=nullptr; }
void beginGpuBackground(Obj page) {
    if(recordingWindow||trackingDoc||idleWindow) { backgroundWindow=docWindow(at<Obj>(page,8));++backgroundEpoch; }
}
void endGpuBackground() { backgroundWindow=nullptr; }
GpuObjectScope::GpuObjectScope(Obj object,bool live):previousOwner(objectOwner),previousGesture(objectGesture) {
    // Temporary splines inside CDArrow::Draw inherit the arrow's owner.
    if(onUI()&&object&&!objectOwner) {
        objectOwner=at<uint64_t>(object,0xb0);
        objectGesture=live||(trackingDoc&&!backgroundWindow&&objectOwner==excludedObject)?trackingGesture:0;
    }
}
GpuObjectScope::~GpuObjectScope() {objectOwner=previousOwner;objectGesture=previousGesture;}
bool beginGpuTracking(Obj doc,uint64_t excluded) {
    auto w=docWindow(doc);if(!w||disabled[w]||!canvases.contains(w)) return false;
    trackingDoc=doc;failed=false;excludedObject=excluded;trackingGesture=++nextTrackingGesture;
    trackingScenes.clear();trackingBase=withoutTrackedOriginal(snapshot(canvases.at(w)));return true;
}
void resetGpuWork(Obj port) {
    if(!trackingDoc||!port) return;
    try { reset(bind(port,oldGetGraphics(port),docWindow(trackingDoc))); } catch(...) { failed=true; }
}
void resetGpuTrackingDestination(Obj doc) {
    if(!trackingDoc||failed||!doc) return;
    const Obj port=at<Obj>(doc,0x258);if(!port) { failed=true;return; }
    try { auto t=bind(port,oldGetGraphics(port),docWindow(doc),true);reset(t,trackingBase); }
    catch(...) { failed=true; }
}
std::shared_ptr<const GpuScene> currentGpuTrackingScene() {
    if(!trackingDoc||failed) return {};
    if(excludedObject) {
        auto live=trackedInk(snapshot(canvases.at(docWindow(trackingDoc))));
        auto frame=makeGpuScene();frame->width=trackingBase->width;frame->height=trackingBase->height;
        frame->extent=trackingBase->extent;frame->serial=++nextScene;frame->trackingFrame=true;frame->base=trackingBase;
        if(live) {
            SceneCommand overlay;overlay.kind=SceneCommand::Image;overlay.image=std::move(live);
            auto state=std::make_shared<SceneState>();state->clip={{0,0,float(frame->width),float(frame->height)}};
            overlay.state=std::move(state);
            overlay.source=overlay.destination={0,0,float(frame->width),float(frame->height)};
            frame->commands.push_back(std::move(overlay));
        }
        return frame;
    }
    auto source=withoutTrackedOriginal(snapshot(canvases.at(docWindow(trackingDoc))));
    if(!source)return {};
    auto frame=makeGpuScene();frame->width=source->width;frame->height=source->height;
    frame->extent=source->extent;frame->serial=++nextScene;frame->trackingFrame=true;
    frame->base=source->base.load(std::memory_order_acquire);frame->commands=source->commands;
    return frame;
}
void endGpuTracking() {
    const HWND w=docWindow(trackingDoc);trackingDoc=nullptr;trackingBase.reset();excludedObject=0;trackingScenes.clear();
    if(failed&&w) discardGpuCanvas(w);
}
void discardGpuCanvas(HWND w) {
    if(trackingDoc&&docWindow(trackingDoc)==w) { trackingDoc=nullptr;trackingBase.reset();excludedObject=0;trackingScenes.clear(); }
    forgetGpuCanvas(w);disabled[w]=true;
}
void forgetGpuCanvas(HWND w) {
    disabled.erase(w);
    canvases.erase(w);
    for(auto i=targets.begin();i!=targets.end();) {
        if(i->second->window==w) { if(i->second->image) imageTargets.erase(i->second->image);i=targets.erase(i); }
        else ++i;
    }
}
void installGpuCanvas() {
    hook(0x623140,getGraphics,oldGetGraphics);
    hook(0x624ed0,nativePolygon,oldNativePolygon);
#define INSTALL(name) intercept(#name,gpu_##name,old_##name)
    INSTALL(GdipSetWorldTransform);INSTALL(GdipResetWorldTransform);INSTALL(GdipMultiplyWorldTransform);
    INSTALL(GdipTranslateWorldTransform);INSTALL(GdipScaleWorldTransform);INSTALL(GdipRotateWorldTransform);
    INSTALL(GdipSetPageUnit);INSTALL(GdipSetPageScale);
    INSTALL(GdipSetClipGraphics);INSTALL(GdipSetClipRect);INSTALL(GdipSetClipRectI);INSTALL(GdipSetClipPath);
    INSTALL(GdipSetClipRegion);INSTALL(GdipSetClipHrgn);INSTALL(GdipResetClip);INSTALL(GdipTranslateClip);INSTALL(GdipTranslateClipI);
    INSTALL(GdipSetSmoothingMode);INSTALL(GdipSetCompositingMode);INSTALL(GdipSetPixelOffsetMode);INSTALL(GdipSetInterpolationMode);
    INSTALL(GdipRestoreGraphics);INSTALL(GdipBeginContainer);INSTALL(GdipBeginContainerI);INSTALL(GdipBeginContainer2);INSTALL(GdipEndContainer);
    INSTALL(GdipDrawPath);INSTALL(GdipFillPath);INSTALL(GdipDrawLine);INSTALL(GdipDrawLineI);
    INSTALL(GdipDrawLines);INSTALL(GdipDrawBeziers);INSTALL(GdipDrawArc);
    INSTALL(GdipDrawRectangle);INSTALL(GdipDrawRectangleI);INSTALL(GdipFillRectangle);INSTALL(GdipFillRectangleI);
    INSTALL(GdipDrawEllipse);INSTALL(GdipFillEllipse);INSTALL(GdipDrawPolygon);INSTALL(GdipFillPolygon);INSTALL(GdipFillPolygonI);
    INSTALL(GdipFillClosedCurve);INSTALL(GdipFillRegion);INSTALL(GdipGraphicsClear);INSTALL(GdipDrawString);
    INSTALL(GdipDrawImageRectRect);INSTALL(GdipDrawImageRectRectI);INSTALL(GdipDrawImageRect);INSTALL(GdipDrawImageRectI);INSTALL(GdipDrawImageI);
    INSTALL(GdipDrawImage);INSTALL(GdipDrawImageFX);
    INSTALL(GdipGetDC);
    INSTALL(GdipReleaseDC);
    INSTALL(GdipEnumerateMetafileDestPointI);
    INSTALL(GdipSetImageAttributesColorMatrix);INSTALL(GdipSetImageAttributesGamma);INSTALL(GdipSetImageAttributesThreshold);
    INSTALL(GdipSetImageAttributesColorKeys);INSTALL(GdipSetImageAttributesRemapTable);INSTALL(GdipSetImageAttributesNoOp);
    INSTALL(GdipResetImageAttributes);INSTALL(GdipSetImageAttributesToIdentity);
    INSTALL(GdipSetImageAttributesOutputChannel);INSTALL(GdipSetImageAttributesOutputChannelColorProfile);
    INSTALL(GdipSaveImageToFile);INSTALL(GdipSaveImageToStream);INSTALL(GdipCreateHBITMAPFromBitmap);
#undef INSTALL
    intercept("GdipDisposeImage",disposeImage,oldDisposeImage);intercept("GdipDeleteGraphics",deleteGraphics,oldDeleteGraphics);
    intercept("GdipBitmapUnlockBits",unlockBits,oldUnlockBits);
    intercept("GdipBitmapLockBits",lockBits,oldLockBits);
    intercept("GdipCreateImageAttributes",createAttributes,oldCreateAttributes);
    intercept("GdipCloneImageAttributes",cloneAttributes,oldCloneAttributes);
    intercept("GdipDisposeImageAttributes",disposeAttributes,oldDisposeAttributes);
    intercept("GdipSetImageAttributesWrapMode",wrapAttributes,oldWrapAttributes);
    interceptIfExported("GdipSetImageAttributesICMMode",icmAttributes,oldIcmAttributes);
    intercept("GdipCreateEffect",createEffect,oldCreateEffect);intercept("GdipDeleteEffect",deleteEffect,oldDeleteEffect);
    intercept("GdipSetEffectParameters",setEffect,oldSetEffect);intercept("GdipBitmapApplyEffect",applyEffect,oldApplyEffect);
    intercept("GdipBitmapGetPixel",getPixel,oldGetPixel);intercept("GdipBitmapSetPixel",setPixel,oldSetPixel);
    intercept("GdipCloneImage",cloneImage,oldCloneImage);
    intercept("GdipCloneBitmapAreaI",cloneAreaI,oldCloneAreaI);intercept("GdipCloneBitmapArea",cloneArea,oldCloneArea);
    intercept("GdipGetImageGraphicsContext",imageGraphics,oldImageGraphics);
    intercept("GdipBitmapCreateApplyEffect",createApplyEffect,oldCreateApplyEffect);
}
}

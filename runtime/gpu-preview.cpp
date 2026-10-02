#include "gpu-preview.hpp"
#include "gpu-scene-renderer.hpp"
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <mutex>
#include <thread>
#include <numbers>

namespace cd {
using Microsoft::WRL::ComPtr;
namespace {
struct Failure { HRESULT code;const char* operation; };
void require(HRESULT code,const char* operation) {
    if(FAILED(code)) throw Failure{code,operation};
}
struct Handle {
    HANDLE value{};
    ~Handle() { if(value) CloseHandle(value); }
    void reset(HANDLE next=nullptr) { if(value) CloseHandle(value);value=next; }
};
using Transform=PreviewView;
struct Snapshot {
    RectI pane{};uint64_t serial{};
    PreviewPaper paper{};
    PreviewView initialView{};
    std::shared_ptr<const GpuScene> scene;
    std::shared_ptr<const GpuScene> nativeBase;
    bool nativeFrame{};
    std::vector<std::byte> pixels;
};
enum class GhostPhase { None,Held,Commit };
struct FixedUiFrame {
    std::shared_ptr<const GpuScene> scene;
    RectI pane{};uint64_t serial{};
};
struct Control {
    std::shared_ptr<Snapshot> snapshot;
    std::shared_ptr<FixedUiFrame> fixedUi;
    Transform target{};
    uint64_t targetRevision{};double targetUpdatedAt{};
    bool visible{},retiring{};
    bool navigation{},anchoredZoom{},scenesHeld{};double cursorX{},cursorY{};
    std::shared_ptr<const GpuScene> scene;
    RectI scenePane{};PreviewPaper scenePaper{};
    GhostBond ghost{};uint64_t ghostRevision{};
    std::shared_ptr<const AlignmentFeedback> alignment;uint64_t alignmentRevision{};
    HoverHighlight highlight{},endpointHighlight{};uint64_t highlightRevision{},feedbackReset{};
    bool placementHighlight{};
    GhostPhase ghostPhase{};bool ghostReleased{};
    GhostBond placementGhost{};double placementOpacity{},placementReleaseAt{};
    std::shared_ptr<Snapshot> placementBase;
    uint64_t placementRevision{};
    uint64_t inputRevision{};POINT pressScreen{};int dragX{},dragY{};bool monitorRelease{};
};
void resetGhostPlacement(Control& c) {
    c.ghostPhase=GhostPhase::None;c.ghostReleased=false;c.monitorRelease=false;c.inputRevision=0;
    c.placementGhost={};c.placementBase.reset();c.placementReleaseAt=0;++c.placementRevision;
}
double seconds() {
    static const double frequency=[] { LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return double(f.QuadPart); }();
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);return double(now.QuadPart)/frequency;
}
double distance(Transform a,Transform b) {
    return std::max({std::abs(a.x-b.x),std::abs(a.y-b.y),std::abs(a.w-b.w),std::abs(a.h-b.h)});
}
uint64_t displayTicks() noexcept {
    LARGE_INTEGER now{};QueryPerformanceCounter(&now);return uint64_t(now.QuadPart);
}
bool samePaper(const PreviewPaper& a,const PreviewPaper& b) noexcept {
    return a.bounds.t==b.bounds.t&&a.bounds.l==b.bounds.l&&a.bounds.b==b.bounds.b&&a.bounds.r==b.bounds.r&&
        a.sourceOrigin.x==b.sourceOrigin.x&&a.sourceOrigin.y==b.sourceOrigin.y&&a.sourceOrigin.z==b.sourceOrigin.z&&
        a.sourceUnits==b.sourceUnits&&a.workspace==b.workspace&&a.paper==b.paper&&a.border==b.border&&
        a.inkTransform.m11==b.inkTransform.m11&&a.inkTransform.m12==b.inkTransform.m12&&
        a.inkTransform.m21==b.inkTransform.m21&&a.inkTransform.m22==b.inkTransform.m22&&
        a.inkTransform.dx==b.inkTransform.dx&&a.inkTransform.dy==b.inkTransform.dy&&
        a.marginX==b.marginX&&a.marginY==b.marginY;
}
Transform advanceCamera(Transform shown,Transform target,double elapsed) {
    shown=easePreview(shown,target,1.0-std::exp(-std::max(elapsed,0.0)/0.024));
    return distance(shown,target)<0.05?target:shown;
}
struct Fade {
    double from{},to{},started{};
    double value(double now) const {
        const double duration=to>from?0.160:0.120;
        const double t=std::clamp((now-started)/duration,0.0,1.0);
        return from+(to-from)*t*t;
    }
    void target(double next,double now) {
        if(to==next) return;
        from=value(now);to=next;started=now;
    }
    bool active(double now) const {
        return from!=to&&now-started<(to>from?0.160:0.120);
    }
};
bool present(const GhostBond& x) { return x.visible; }
bool present(const HoverHighlight& x) { return bool(x.scene); }
bool sameFeedback(const GhostBond& a,const GhostBond& b) {
    return a.identity==b.identity&&a.toolKey==b.toolKey&&a.placement==b.placement&&
        (!a.placement||(a.start.x==b.start.x&&a.start.y==b.start.y));
}
bool sameFeedback(const HoverHighlight& a,const HoverHighlight& b) {
    if(a.identity==b.identity) return true;
    if(!a.vertexCircle||!b.vertexCircle) return false;
    const double tolerance=std::max(a.circle.r-a.circle.l,b.circle.r-b.circle.l)*1.0e-6;
    return std::max({std::abs(a.circle.t-b.circle.t),std::abs(a.circle.l-b.circle.l),
        std::abs(a.circle.b-b.circle.b),std::abs(a.circle.r-b.circle.r)})<=tolerance;
}
template<class T> struct FeedbackAnimation {
    struct Layer { T data{};Fade fade{};double appeared{}; };
    Layer current{},outgoing{};
    void reset() { current={};outgoing={}; }
    void update(const T& next,double now,bool retainFade=false) {
        if(!present(next)) { current.fade.target(0,now);outgoing.fade.target(0,now);return; }
        if(retainFade&&present(current.data)) {
            // Retargeting through a moving document changes geometry instantly.
            // Keep its opacity/breathing phase and discard the old target trail.
            current.data=next;current.fade.target(1,now);outgoing={};return;
        }
        if(present(current.data)&&sameFeedback(current.data,next)) {
            current.data=next;current.fade.target(1,now);return;
        }
        if(present(outgoing.data)&&sameFeedback(outgoing.data,next)) std::swap(current,outgoing);
        else { outgoing=std::move(current);current={};current.appeared=now; }
        outgoing.fade.target(0,now);
        current.data=next;current.fade.target(1,now);
    }
    void expire(double now) {
        for(auto* layer:{&current,&outgoing})
            if(present(layer->data)&&layer->fade.to==0&&!layer->fade.active(now)) *layer={};
    }
    bool active(double now,bool breathe=false) const {
        return current.fade.active(now)||outgoing.fade.active(now)||
            (breathe&&present(current.data)&&current.fade.to>0);
    }
};
struct AnimatedFeedback {
    FeedbackAnimation<GhostBond> ghosts;
    FeedbackAnimation<HoverHighlight> highlights,endpoints;
    uint64_t ghostRevision{UINT64_MAX},highlightRevision{UINT64_MAX},resetRevision{UINT64_MAX};
    bool allowed{},placing{},navigation{};
    GhostPhase ghostPhase{};
    uint64_t placementRevision{UINT64_MAX};
    GhostBond placementGhost{};double placementOpacity{},placementReleaseAt{};
    std::shared_ptr<Snapshot> placementBase;
    double releaseBlend(double now) const {
        if(placementReleaseAt<=0) return 0;
        const double t=std::clamp((now-placementReleaseAt)/0.160,0.0,1.0);
        return t*t;
    }
    void update(const Control& request,double now) {
        navigation=request.navigation;
        const bool enabled=(request.snapshot->nativeFrame||request.navigation)&&
            (!request.scenesHeld||request.placementHighlight||request.ghostPhase!=GhostPhase::None);
        if(resetRevision!=request.feedbackReset||!enabled) {
            ghosts.reset();highlights.reset();endpoints.reset();placing=false;ghostRevision=highlightRevision=UINT64_MAX;
            resetRevision=request.feedbackReset;
        }
        allowed=enabled;
        if(!enabled) return;
        if(placementRevision!=request.placementRevision) {
            ghostPhase=request.ghostPhase;placementRevision=request.placementRevision;
            placementGhost=request.placementGhost;placementOpacity=request.placementOpacity;
            placementReleaseAt=request.placementReleaseAt;
            placementBase=request.placementBase;
        }
        if(ghostPhase!=GhostPhase::None) {
            ghosts.reset();ghostRevision=UINT64_MAX;
        } else if(ghostRevision!=request.ghostRevision) {
            ghosts.update(request.ghost,now,request.navigation);ghostRevision=request.ghostRevision;
        }
        if(highlightRevision!=request.highlightRevision) {
            // Transfer the already visible endpoint into the ordinary hover
            // layer at commit. Restarting its fade under a new native ID would
            // darken it during the overlap with the outgoing tracker circle.
            if(placing&&!request.placementHighlight&&present(request.highlight)) {
                for(auto* layer:{&endpoints.current,&endpoints.outgoing})
                    if(present(layer->data)&&sameFeedback(layer->data,request.highlight)) {
                        highlights.outgoing=std::move(highlights.current);
                        highlights.outgoing.fade.target(0,now);
                        highlights.current=std::move(*layer);endpoints.reset();break;
                    }
            }
            highlights.update(request.highlight,now);highlightRevision=request.highlightRevision;
            endpoints.update(request.endpointHighlight,now);
        }
        placing=request.placementHighlight;
        ghosts.expire(now);highlights.expire(now);endpoints.expire(now);
    }
    bool active(double now) const {
        return allowed&&(ghosts.active(now,true)||highlights.active(now)||endpoints.active(now)||
            (ghostPhase==GhostPhase::Held&&placementReleaseAt>0&&releaseBlend(now)<1));
    }
};

struct Renderer {
    HWND window{};
    ComPtr<ID3D11Device> device;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<ID2D1Factory1> d2dFactory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> target,chromeTarget;
    ComPtr<IDCompositionVisual> visual,chromeVisual;
    ComPtr<IDXGISwapChain1> chromeSwap;
    ComPtr<ID2D1Bitmap1> chromeBuffer;
    int chromeWidth{},chromeHeight{};bool chromeChanged{},chromeAttached{};
    ComPtr<ID2D1Bitmap1> fixedUiImage;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<IDXGISwapChain2> swap2;
    ComPtr<ID2D1Bitmap1> backBuffer,image,presentedImage,placementImage;
    ComPtr<ID2D1Image> navigationImage,presentedVector,placementVector;
    D2D1_RECT_F vectorSource{};
    uint64_t presentedImageSerial{},placementImageSerial{};
    std::shared_ptr<const GpuScene> presentedScene;
    ComPtr<ID2D1SolidColorBrush> paperBrush,borderBrush,ghostBrush,placementBrush;
    PreviewPaper paper{};
    SceneRenderer scenes;
    RectI pane{};bool nativeFrame{},contentChanged{};
    Handle frameReady;
    int width{},height{};bool attached{};

    ~Renderer() { detach(true); }
    void preparePlacement(const Control& request) {
        const auto& source=request.placementBase;
        if(request.ghostPhase==GhostPhase::None||!source) {
            placementImage.Reset();placementVector.Reset();placementImageSerial=0;return;
        }
        if(placementImageSerial==source->serial&&placementImage) return;
        // Pin the bitmap actually submitted before uploading the final page.
        // Cache eviction must not make every fade frame replay page history.
        placementImage=presentedImageSerial==source->serial&&presentedImage?
            presentedImage:scenes.render(source->scene);
        placementVector=presentedImageSerial==source->serial?presentedVector:nullptr;
        placementImageSerial=source->serial;
    }
    void detach(bool all=false) noexcept {
        if(composition) {
            bool changed=false;
            if(target&&attached) {target->SetRoot(nullptr);attached=false;changed=true;}
            if(all&&chromeTarget&&chromeAttached) {chromeTarget->SetRoot(nullptr);chromeAttached=false;changed=true;}
            if(changed)composition->Commit();
        }
    }
    void initialize(HWND w) {
        window=w;
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0};
        require(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,UINT(std::size(levels)),D3D11_SDK_VERSION,
            device.GetAddressOf(),nullptr,nullptr),"hardware Direct3D device");
        require(device.As(&dxgi),"DXGI device");
        ComPtr<IDXGIAdapter> adapter;
        require(dxgi->GetAdapter(adapter.GetAddressOf()),"display adapter");
        require(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())),"DXGI factory");
        require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),
            nullptr,reinterpret_cast<void**>(d2dFactory.GetAddressOf())),"Direct2D factory");
        require(d2dFactory->CreateDevice(dxgi.Get(),d2dDevice.GetAddressOf()),"GPU Direct2D device");
        require(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.GetAddressOf()),
            "GPU drawing context");
        context->SetDpi(96.0f,96.0f); // Snapshot/pane coordinates are already physical pixels.
        scenes.initialize(context.Get(),d2dFactory.Get());
        require(context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),paperBrush.GetAddressOf()),"paper brush");
        require(context->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::Black),borderBrush.GetAddressOf()),"page border brush");
        require(context->CreateSolidColorBrush(D2D1::ColorF(0.22f,0.40f,0.65f,0.45f),ghostBrush.GetAddressOf()),"ghost bond brush");
        require(context->CreateSolidColorBrush(D2D1::ColorF(0.22f,0.40f,0.65f,0.45f),placementBrush.GetAddressOf()),"placement ink brush");
        require(DCompositionCreateDevice(dxgi.Get(),__uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(composition.GetAddressOf())),"DirectComposition device");
        require(composition->CreateTargetForHwnd(w,TRUE,target.GetAddressOf()),"document composition target");
        require(composition->CreateTargetForHwnd(w,FALSE,chromeTarget.GetAddressOf()),"fixed document UI target");
        require(composition->CreateVisual(visual.GetAddressOf()),"canvas visual");
        require(composition->CreateVisual(chromeVisual.GetAddressOf()),"fixed document UI visual");
        writeGpuStatus("Revision 94 hardware GPU canvas/navigation backend initialized; paper fill, clip and outside outline share one physical-pixel boundary; free ghosts use render-frame cursor anchoring and navigation retargets keep opacity; live navigation feedback and coalesced camera-frame UI notifications; full-page navigation extent and consistent settled-frame vector replay; camera frames replay retained vectors at display resolution; independent persistent fixed UI; vector Direct2D over Direct3D11/DirectComposition.\r\n");
    }
    void updateFixedUi(const FixedUiFrame& frame) {
        // Only an explicit, complete fixed-UI capture may replace this image.
        // Page, tracker and navigation snapshots never enter this path.
        fixedUiImage=scenes.render(frame.scene);
        const int uiWidth=frame.scene->width,uiHeight=frame.scene->height;
        if(!chromeSwap||chromeWidth!=uiWidth||chromeHeight!=uiHeight) {
            context->SetTarget(nullptr);chromeBuffer.Reset();chromeSwap.Reset();
            DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=UINT(uiWidth);desc.Height=UINT(uiHeight);
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
            desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
            desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
            require(factory->CreateSwapChainForComposition(device.Get(),&desc,nullptr,chromeSwap.GetAddressOf()),"fixed UI swap chain");
            ComPtr<IDXGISurface> surface;require(chromeSwap->GetBuffer(0,IID_PPV_ARGS(surface.GetAddressOf())),"fixed UI buffer");
            auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            require(context->CreateBitmapFromDxgiSurface(surface.Get(),&properties,chromeBuffer.GetAddressOf()),"fixed UI target");
            chromeWidth=uiWidth;chromeHeight=uiHeight;
        }
        context->SetTarget(chromeBuffer.Get());context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->BeginDraw();context->Clear(D2D1::ColorF(0,0.0f));
        const auto uiPane=frame.pane;
        const std::array<RectI,4> strips{{{0,0,uiPane.t,uiWidth},{uiPane.b,0,uiHeight,uiWidth},
            {uiPane.t,0,uiPane.b,uiPane.l},{uiPane.t,uiPane.r,uiPane.b,uiWidth}}};
        for(auto r:strips) if(valid(r)) {
            const auto rect=D2D1::RectF(float(r.l),float(r.t),float(r.r),float(r.b));
            context->DrawBitmap(fixedUiImage.Get(),&rect,1,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,&rect);
        }
        require(context->EndDraw(),"fixed GPU UI drawing");context->SetTarget(backBuffer.Get());chromeChanged=true;
    }
    bool commitFixedUi() {
        if(!chromeChanged)return true;
        const auto result=chromeSwap->Present(0,DXGI_PRESENT_DO_NOT_WAIT);
        if(result==DXGI_ERROR_WAS_STILL_DRAWING)return false;
        require(result,"independent fixed UI presentation");
        require(chromeVisual->SetContent(chromeSwap.Get()),"fixed UI content");
        if(!chromeAttached) {
            require(chromeTarget->SetRoot(chromeVisual.Get()),"attach independent fixed UI frame");
            chromeAttached=true;
        }
        require(composition->Commit(),"commit independent fixed UI frame");
        chromeChanged=false;return true;
    }
    bool upload(const Snapshot& snapshot) {
        // The displayed bitmap can outlive its LRU entry. Restore that entry
        // before rendering a new incremental scene based on the displayed page.
        scenes.remember(presentedScene,presentedImage);
        paper=snapshot.paper;
        pane=snapshot.pane;nativeFrame=snapshot.nativeFrame;
        paperBrush->SetColor(D2D1::ColorF(paper.paper));
        borderBrush->SetColor(D2D1::ColorF(paper.border));
        const int w=snapshot.pane.r-snapshot.pane.l,h=snapshot.pane.b-snapshot.pane.t;
        if(UINT(w)>context->GetMaximumBitmapSize()||UINT(h)>context->GetMaximumBitmapSize())
            throw Failure{E_INVALIDARG,"canvas exceeds GPU bitmap limit"};
        const bool newChain=!swap||w!=width||h!=height;
        if(newChain) {
            // Leave the previous visual content attached while its successor
            // is prepared. Native CPU pixels may deliberately be out of date.
            context->SetTarget(nullptr);backBuffer.Reset();
            frameReady.reset();swap2.Reset();swap.Reset();
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width=UINT(w);desc.Height=UINT(h);desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount=2;desc.Scaling=DXGI_SCALING_STRETCH;
            desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
            desc.Flags=DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            require(factory->CreateSwapChainForComposition(device.Get(),&desc,nullptr,swap.GetAddressOf()),
                "composition flip swap chain");
            require(swap.As(&swap2),"waitable swap chain");
            require(swap2->SetMaximumFrameLatency(1),"one-frame presentation latency");
            frameReady.reset(swap2->GetFrameLatencyWaitableObject());
            if(!frameReady.value) throw Failure{E_FAIL,"frame readiness handle"};
            ComPtr<IDXGISurface> surface;
            require(swap->GetBuffer(0,IID_PPV_ARGS(surface.GetAddressOf())),"GPU back buffer");
            const auto properties=D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96.0f,96.0f);
            require(context->CreateBitmapFromDxgiSurface(surface.Get(),&properties,backBuffer.GetAddressOf()),
                "GPU render target");
            context->SetTarget(backBuffer.Get());
            width=w;height=h;
        }
        image.Reset();navigationImage.Reset();
        if(snapshot.scene) {
            auto scene=snapshot.nativeFrame&&placementImageSerial==snapshot.serial&&placementImage?
                placementImage:scenes.render(snapshot.scene);
            // Both camera movement and the finalized native-coordinate page
            // use the same vector replay. Native-frame retirement changes the
            // camera/source scene, never the rendering quality.
            // A drag frame is already rendered by Direct2D at the native
            // viewport resolution. Recompiling all its static dependencies as
            // command lists on every mouse move duplicates work and adds lag.
            // Camera navigation still recompiles vectors at the animated scale.
            try {
                if(!nativeFrame||!snapshot.scene->trackingFrame)
                    navigationImage=scenes.navigation(snapshot.scene);
            }
            catch(const std::exception&) {
                writeGpuStatus("Page vector replay unavailable; retained full-resolution bitmap used.\r\n");
            }
            const auto bounds=snapshot.scene->extent;
            vectorSource=bounds.w>0&&bounds.h>0?
                D2D1::RectF(bounds.x,bounds.y,bounds.x+bounds.w,bounds.y+bounds.h):
                D2D1::RectF(float(pane.l),float(pane.t),float(pane.r),float(pane.b));
            if(nativeFrame) image=scene;
            else {
                const auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
                require(context->CreateBitmap(D2D1::SizeU(w,h),nullptr,0,&properties,image.GetAddressOf()),"GPU viewport snapshot");
                context->SetTarget(image.Get());context->SetTransform(D2D1::Matrix3x2F::Identity());
                context->BeginDraw();context->Clear(D2D1::ColorF(0,0.0f));
                const auto source=D2D1::RectF(float(pane.l),float(pane.t),float(pane.r),float(pane.b));
                const auto destination=D2D1::RectF(0,0,float(w),float(h));
                context->DrawBitmap(scene.Get(),&destination,1.0f,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,&source);
                require(context->EndDraw(),"GPU viewport snapshot copy");context->SetTarget(backBuffer.Get());
            }
        } else {
        // Captured GDI pixels have undefined alpha; treat them as opaque.
        const auto sourceProperties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE),96.0f,96.0f);
        require(context->CreateBitmap(D2D1::SizeU(UINT(w),UINT(h)),snapshot.pixels.data(),UINT(w)*4,
            &sourceProperties,image.GetAddressOf()),"cached canvas texture upload");
        }
        contentChanged=true;scenes.prune();
        return newChain;
    }
    void draw(Transform view,const AnimatedFeedback& feedback,double now,const AlignmentFeedback* alignment) {
        ComPtr<ID2D1Bitmap1> placementBase;
        if(feedback.allowed&&feedback.ghostPhase==GhostPhase::Held&&feedback.placementBase&&
            placementImageSerial==feedback.placementBase->serial) placementBase=placementImage;
        std::array<ComPtr<ID2D1Bitmap1>,4> highlights;
        const auto& h=feedback.highlights;
        const auto& e=feedback.endpoints;
        const auto& g=feedback.ghosts;
        const std::array<const FeedbackAnimation<HoverHighlight>::Layer*,4> layers{&h.outgoing,&h.current,&e.outgoing,&e.current};
        if(feedback.allowed) for(size_t i=0;i<layers.size();++i)
            if(layers[i]->data.scene&&layers[i]->fade.value(now)>0) highlights[i]=scenes.render(layers[i]->data.scene);
        const D2D1_RECT_F rect=D2D1::RectF(float(view.x),float(view.y),float(view.x+view.w),float(view.y+view.h));
        context->BeginDraw();
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        // The viewport has a single opaque owner. Transparent dirty regions
        // must never reveal a stale native/GDI document beneath the GPU.
        context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        context->Clear(D2D1::ColorF(paper.workspace));
        // The bitmap contains only the initially visible portion of the page.
        // Extend the paper underneath it so newly exposed blank page areas and
        // the paper/workspace boundary move with the molecule during zoom/pan.
        const double sx=view.w/width,sy=view.h/height;
        // Resolve a single physical-pixel boundary for the paper fill, content
        // clip and outline. A centered antialiased stroke over an independently
        // rounded native border changes coverage during partial scene updates.
        const auto page=D2D1::RectF(float(std::round(view.x+paper.bounds.l*sx)),
            float(std::round(view.y+paper.bounds.t*sy)),
            float(std::round(view.x+paper.bounds.r*sx)),
            float(std::round(view.y+paper.bounds.b*sy)));
        const auto pageAntialias=context->GetAntialiasMode();
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        context->FillRectangle(page,paperBrush.Get());
        context->SetAntialiasMode(pageAntialias);
        const auto source=D2D1::RectF(float(pane.l),float(pane.t),float(pane.r),float(pane.b));
        // Native scenes retain full-client coordinates, but only the viewport
        // belongs to this visual. Its size/offset stay unchanged at handoff.
        // Cached native backgrounds can include white padding outside the
        // editable paper. That padding must never become an animated page.
        context->PushAxisAlignedClip(page,D2D1_ANTIALIAS_MODE_ALIASED);
        if(placementBase) {
            const auto oldPane=feedback.placementBase->pane;
            const auto oldSource=D2D1::RectF(float(oldPane.l),float(oldPane.t),float(oldPane.r),float(oldPane.b));
            if(placementVector) {
                context->SetTransform(D2D1::Matrix3x2F::Translation(float(-oldPane.l),float(-oldPane.t))*
                    D2D1::Matrix3x2F::Scale(float(sx),float(sy))*
                    D2D1::Matrix3x2F::Translation(float(view.x),float(view.y)));
                const auto offset=D2D1::Point2F(float(oldPane.l),float(oldPane.t));
                context->DrawImage(placementVector.Get(),&offset,&oldSource,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
                context->SetTransform(D2D1::Matrix3x2F::Identity());
            } else context->DrawBitmap(placementBase.Get(),&rect,1.0f,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,&oldSource);
            const auto& oldPaper=feedback.placementBase->paper;
            auto project=[&](Point p) {
                const auto ink=previewInkPoint(oldPaper,p);
                return D2D1::Point2F(float(view.x+ink.x*sx),float(view.y+ink.y*sy));
            };
            // Retain the submitted ghost in fixed document coordinates while
            // native geometry is pending. Only its ink opacity/color changes.
            const auto before=context->GetAntialiasMode();
            context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            const auto& ghost=feedback.placementGhost;
            const double blend=feedback.releaseBlend(now);
            const D2D1::ColorF ink(ghost.ink&0xffffff,float((ghost.ink>>24)&0xff)/255.0f);
            auto mix=[&](double from,double to) { return float(from+(to-from)*blend); };
            placementBrush->SetColor(D2D1::ColorF(mix(0.22,ink.r),mix(0.40,ink.g),mix(0.65,ink.b),
                mix(0.45*feedback.placementOpacity,ink.a)));
            placementBrush->SetOpacity(1.0f);
            if(ghost.artwork) {
                for(const auto& line:ghost.artwork->strokes)
                    context->DrawLine(project(line.start),project(line.end),placementBrush.Get(),
                        float(line.width/oldPaper.sourceUnits*sx));
                for(const auto& path:ghost.artwork->paths) {
                    ScenePath projected;projected.types=path.types;
                    for(auto point:path.points) { auto p=project(point);projected.points.push_back({p.x,p.y}); }
                    scenes.feedbackPath(projected,placementBrush.Get(),float(path.width/oldPaper.sourceUnits*sx),path.filled);
                }
            }
            else context->DrawLine(project(ghost.start),project(ghost.end),placementBrush.Get(),
                float(ghost.width/oldPaper.sourceUnits*sx));
            context->SetAntialiasMode(before);
        }
        if(!placementBase) {
            // The finalized native page replaces the retained placement in a
            // single frame. Do not dissolve two versions of bond geometry.
            if(navigationImage) {
                const auto mapping=D2D1::Matrix3x2F::Translation(float(-pane.l),float(-pane.t))*
                    D2D1::Matrix3x2F::Scale(float(sx),float(sy))*
                    D2D1::Matrix3x2F::Translation(float(view.x),float(view.y));
                context->SetTransform(mapping);
                const auto offset=D2D1::Point2F(vectorSource.left,vectorSource.top);
                const bool unscaled=std::abs(sx-1)<0.00001&&std::abs(sy-1)<0.00001;
                context->DrawImage(navigationImage.Get(),&offset,&vectorSource,unscaled?
                    D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR:D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                context->SetTransform(D2D1::Matrix3x2F::Identity());
            } else {
                const bool unscaled=std::abs(sx-1)<0.00001&&std::abs(sy-1)<0.00001;
                context->DrawBitmap(image.Get(),&rect,1.0f,nativeFrame||unscaled?
                    D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR:D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                    nativeFrame?&source:nullptr,nullptr);
            }
        }
        if(feedback.allowed&&paper.sourceUnits>0) {
            for(size_t i=0;i<layers.size();++i) if(highlights[i]) {
                const auto& layer=*layers[i];const auto& sourcePaper=layer.data.paper;
                const auto r=layer.data.bounds,sourcePane=layer.data.pane;
                const double scale=sourcePaper.sourceUnits/paper.sourceUnits;
                const double x=view.x+(sourcePaper.sourceOrigin.x-paper.sourceOrigin.x)/paper.sourceUnits*sx+
                    (r.l-sourcePane.l)*scale*sx;
                const double y=view.y+(sourcePaper.sourceOrigin.y-paper.sourceOrigin.y)/paper.sourceUnits*sy+
                    (r.t-sourcePane.t)*scale*sy;
                const auto dst=D2D1::RectF(float(x),float(y),float(x+(r.r-r.l)*scale*sx),float(y+(r.b-r.t)*scale*sy));
                const auto src=D2D1::RectF(0,0,float(r.r-r.l),float(r.b-r.t));
                context->DrawBitmap(highlights[i].Get(),&dst,float(layer.fade.value(now)),
                    D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,&src,nullptr);
            }
            // Compose transient feedback after the immutable page image. It
            // never becomes an input to navigation capture or drag buffers.
            const auto before=context->GetAntialiasMode();
            context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            auto project=[&](Point p) {
                const auto ink=previewInkPoint(paper,p);
                return D2D1::Point2F(float(view.x+ink.x*sx),float(view.y+ink.y*sy));
            };
            for(const auto* layer:{&g.outgoing,&g.current}) if(layer->data.visible&&layer->fade.value(now)>0) {
                const auto& ghost=layer->data;
                D2D1_POINT_2F offset{};
                if(feedback.navigation&&ghost.cursorAnchored&&!ghost.identity) {
                    // The UI selected this free preview against an earlier
                    // submitted camera. Anchor its grab point with this exact
                    // render-frame camera, without waiting for another UI round trip.
                    const auto grab=project(ghost.pressPoint);
                    offset={float(ghost.cursorAnchor.x-pane.l)-grab.x,
                        float(ghost.cursorAnchor.y-pane.t)-grab.y};
                }
                auto previewPoint=[&](Point p) {
                    auto result=project(p);result.x+=offset.x;result.y+=offset.y;return result;
                };
                // Vary opacity only, keeping geometry and hit areas fixed.
                const double breath=0.96+0.04*std::cos((now-layer->appeared)*2.0*std::numbers::pi/2.4);
                ghostBrush->SetOpacity(float(layer->fade.value(now)*breath));
                if(layer->data.artwork) {
                    for(const auto& line:layer->data.artwork->strokes)
                        context->DrawLine(previewPoint(line.start),previewPoint(line.end),ghostBrush.Get(),
                            float(line.width/paper.sourceUnits*sx));
                    for(const auto& path:layer->data.artwork->paths) {
                        ScenePath projected;projected.types=path.types;
                        for(auto point:path.points) { auto p=previewPoint(point);projected.points.push_back({p.x,p.y}); }
                        scenes.feedbackPath(projected,ghostBrush.Get(),float(path.width/paper.sourceUnits*sx),path.filled);
                    }
                } else context->DrawLine(previewPoint(layer->data.start),previewPoint(layer->data.end),ghostBrush.Get(),
                    float(layer->data.width/paper.sourceUnits*sx));
            }
            context->SetAntialiasMode(before);
        }
        if(alignment&&nativeFrame&&std::abs(sx-1)<1e-5&&std::abs(sy-1)<1e-5) {
            const auto before=context->GetAntialiasMode();
            context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            placementBrush->SetColor(D2D1::ColorF(0xcf3849,0.92f));placementBrush->SetOpacity(1);
            const float dip=alignment->dip,stroke=std::max(1.0f,dip);
            for(const auto& line:alignment->strokes) {
                const auto a=D2D1::Point2F(line.start.x-float(pane.l),line.start.y-float(pane.t));
                const auto b=D2D1::Point2F(line.end.x-float(pane.l),line.end.y-float(pane.t));
                const float dx=b.x-a.x,dy=b.y-a.y,length=std::hypot(dx,dy);
                if(length<.5f)continue;
                const float ux=dx/length,uy=dy/length;
                if(line.arrows) {
                    context->DrawLine(a,b,placementBrush.Get(),stroke);
                    const float size=std::min(4*dip,length*.25f);
                    for(int end=0;end<2;++end) {
                        const auto p=end?b:a;const float sign=end?-1.0f:1.0f;
                        const auto c=D2D1::Point2F(p.x+sign*ux*size,p.y+sign*uy*size);
                        context->DrawLine(p,D2D1::Point2F(c.x-uy*size*.55f,c.y+ux*size*.55f),placementBrush.Get(),stroke);
                        context->DrawLine(p,D2D1::Point2F(c.x+uy*size*.55f,c.y-ux*size*.55f),placementBrush.Get(),stroke);
                    }
                } else for(float at=0;at<length;at+=4*dip) {
                    const auto p=D2D1::Point2F(a.x+ux*at,a.y+uy*at);
                    context->FillEllipse(D2D1::Ellipse(p,stroke*.55f,stroke*.55f),placementBrush.Get());
                }
            }
            context->SetAntialiasMode(before);
        }
        context->PopAxisAlignedClip();
        // Keep the outline outside the editable paper and one physical pixel
        // thick at every zoom. All four strips share the fill/clip boundary.
        context->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        context->FillRectangle(D2D1::RectF(page.left-1,page.top-1,page.right+1,page.top),borderBrush.Get());
        context->FillRectangle(D2D1::RectF(page.left-1,page.bottom,page.right+1,page.bottom+1),borderBrush.Get());
        context->FillRectangle(D2D1::RectF(page.left-1,page.top,page.left,page.bottom),borderBrush.Get());
        context->FillRectangle(D2D1::RectF(page.right,page.top,page.right+1,page.bottom),borderBrush.Get());
        context->SetAntialiasMode(pageAntialias);
        require(context->EndDraw(),"GPU canvas draw");
    }
    void commit() {
        if(contentChanged) {
            require(visual->SetContent(swap.Get()),"swap-chain visual content");
            require(visual->SetOffsetX(float(pane.l)),"canvas horizontal offset");
            require(visual->SetOffsetY(float(pane.t)),"canvas vertical offset");contentChanged=false;
            const auto clip=D2D1::RectF(0,0,float(width),float(height));
            require(visual->SetClip(clip),"canvas ownership clip");
        }
        if(!attached) {
            require(target->SetRoot(visual.Get()),"attach completed canvas frame");
            require(composition->Commit(),"show GPU canvas");attached=true;
        } else require(composition->Commit(),"commit canvas position");
    }
};
}

struct GpuPreview::Impl {
    HWND window{};Handle changed,stopped;
    std::mutex mutex;Control control;
    std::atomic<bool> ready{},failed{};
    uint64_t nextSerial{},settledRevision{},presentedSerial{};Transform presented{};GhostBond presentedGhost{};
    bool cameraNotificationPending{};
    double presentedGhostOpacity{};
    std::shared_ptr<Snapshot> presentedSnapshot;
    PlacementDisplayTiming displayTiming{};
    explicit Impl(HWND w):window(w) {
        changed.value=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        stopped.value=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!changed.value||!stopped.value) throw std::runtime_error("GPU worker events");
    }
    Control read() { std::lock_guard guard(mutex);return control; }
    void observePlacementRelease(const Control& request) noexcept {
        if(!request.monitorRelease||request.ghostPhase!=GhostPhase::Held||request.placementReleaseAt>0) return;
        POINT cursor{};if(!GetCursorPos(&cursor)) return;
        const bool moved=std::abs(double(cursor.x)-request.pressScreen.x)>request.dragX||
            std::abs(double(cursor.y)-request.pressScreen.y)>request.dragY;
        const bool released=(GetAsyncKeyState(VK_LBUTTON)&0x8000)==0;
        if(!moved&&!released) return;
        std::lock_guard guard(mutex);auto& c=control;
        if(!c.monitorRelease||c.inputRevision!=request.inputRevision||
            c.placementRevision!=request.placementRevision||c.ghostPhase!=GhostPhase::Held) return;
        c.monitorRelease=false;
        if(moved) {
            // A drag has different authoritative geometry. Let its native work
            // scenes take over; never fade the original click ghost into ink.
            resetGhostPlacement(c);
        } else {
            c.placementReleaseAt=seconds();++c.placementRevision;
            if(displayTiming.revision==c.inputRevision&&!displayTiming.physicalRelease)
                displayTiming.physicalRelease=displayTicks();
        }
        SetEvent(changed.value);
    }
    bool resolveZoom(uint64_t serial,uint64_t revision,Transform target) {
        std::lock_guard guard(mutex);
        if(!control.visible||!control.snapshot||control.snapshot->serial!=serial||
            control.targetRevision!=revision||!control.anchoredZoom) return false;
        control.target=target;return true;
    }
    // Recheck after rendering, but never call the display driver while holding
    // the state mutex shared with the UI thread.
    enum class Submission { Stale,Busy,Shown };
    void discarded(const Control& request) {
        if(!request.inputRevision) return;
        std::lock_guard guard(mutex);
        if(displayTiming.revision==request.inputRevision&&!displayTiming.presented) ++displayTiming.discarded;
    }
    void recordGpuWork(const Control& request,uint64_t elapsed,bool drawing) {
        if(!request.inputRevision) return;
        std::lock_guard guard(mutex);
        if(displayTiming.revision!=request.inputRevision||displayTiming.presented) return;
        (drawing?displayTiming.drawTicks:displayTiming.uploadTicks)+=elapsed;
    }
    Submission submit(Renderer& renderer,uint64_t serial,const Control& request) {
        const auto revision=request.targetRevision;
        {
            std::lock_guard guard(mutex);
            if(!control.visible||!control.snapshot||control.snapshot->serial!=serial||
                control.targetRevision!=revision||control.feedbackReset!=request.feedbackReset||
                control.placementRevision!=request.placementRevision||
                control.alignmentRevision!=request.alignmentRevision) return Submission::Stale;
        }
        const auto result=renderer.swap->Present(1,DXGI_PRESENT_DO_NOT_WAIT);
        if(result==DXGI_ERROR_WAS_STILL_DRAWING) return Submission::Busy;
        require(result,"nonblocking refresh-synchronized presentation");
        renderer.presentedImage=renderer.image;renderer.presentedImageSerial=serial;
        renderer.presentedVector=renderer.navigationImage;
        renderer.presentedScene=request.snapshot->nativeFrame?request.snapshot->scene:nullptr;
        return Submission::Shown;
    }
    void acknowledge(uint64_t serial,const Control& request,Transform shown,const AnimatedFeedback& feedback,double now) {
        const auto revision=request.targetRevision;
        std::lock_guard guard(mutex);
        if(!control.visible)
            return;
        presented=shown;presentedSerial=serial;
        // A new scene can be queued while Present is submitting this one.
        // Record the submitted scene, not the latest UI request, for press ownership.
        presentedSnapshot=request.snapshot;
        if(request.ghostPhase==GhostPhase::Commit&&displayTiming.revision==request.inputRevision&&!displayTiming.presented)
            displayTiming.presented=displayTicks();
        const auto& ghost=feedback.ghosts.current;
        presentedGhost=feedback.allowed&&ghost.fade.value(now)>0?ghost.data:GhostBond{};
        presentedGhostOpacity=ghost.fade.value(now)*(0.96+0.04*std::cos((now-ghost.appeared)*2.0*std::numbers::pi/2.4));
        ready.store(true,std::memory_order_release);
        // One pending UI notification is enough: its handler reads the most
        // recently submitted camera instead of queueing one message per frame.
        if((request.navigation||request.retiring)&&!cameraNotificationPending) {
            cameraNotificationPending=true;
            if(!PostMessageW(window,gpuCameraFrameMessage(),0,0))cameraNotificationPending=false;
        }
        // Completion refers to the actual presented target, not a separate UI
        // animation clock. A newer wheel delta invalidates this completion.
        if(control.targetRevision==revision&&distance(shown,control.target)==0.0&&settledRevision!=control.targetRevision) {
            settledRevision=control.targetRevision;
            if(control.navigation&&!control.retiring)
                PostMessageW(window,gpuSettledMessage(),0,0);
        }
    }
    void finishGhostCommit(const Control& request,const AnimatedFeedback& feedback) {
        if(feedback.ghostPhase!=GhostPhase::Commit) return;
        {
            std::lock_guard guard(mutex);
            if(control.ghostPhase!=GhostPhase::Commit||control.placementRevision!=request.placementRevision) return;
            resetGhostPlacement(control);
        }
        SetEvent(changed.value);PostMessageW(window,gpuGhostCommittedMessage(),0,0);
    }
    bool retire(uint64_t serial,Transform shown) {
        std::lock_guard guard(mutex);
        if(control.scenesHeld||!control.retiring||!control.visible||!control.snapshot||
            control.snapshot->serial!=serial||distance(shown,control.target)>0.0) return false;
        if(control.snapshot->nativeFrame) {
            if(presentedSerial!=serial)return false;
            control.retiring=false;
            PostMessageW(window,gpuPagePresentedMessage(),0,0);return true;
        }
        if(control.scene) {
            // The scene captured before zoom has the old coordinates. Wait for
            // a newly painted native scene instead of switching back to it.
            if(control.scene==control.snapshot->scene||control.scene==control.snapshot->nativeBase) return false;
            auto snapshot=std::make_shared<Snapshot>();snapshot->scene=control.scene;snapshot->nativeFrame=true;
            snapshot->pane=control.scenePane;snapshot->paper=control.scenePaper;snapshot->serial=++nextSerial;
            control.snapshot=snapshot;control.target={0,0,double(snapshot->pane.r-snapshot->pane.l),double(snapshot->pane.b-snapshot->pane.t)};
            ++control.targetRevision;control.targetUpdatedAt=seconds();
            control.navigation=control.anchoredZoom=false;
        } else control.visible=false;
        return true;
    }
    void run() noexcept {
        const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        try {
            require(com,"GPU worker COM initialization");
            Renderer renderer;
            uint64_t serial{},uiSerial{},resolvedRevision{},ghostRevision{};Transform shown{},desired{};
            uint64_t highlightRevision{},feedbackReset{},placementRevision{},alignmentRevision{};
            AnimatedFeedback feedback;bool feedbackWasAnimating{};
            double lastDraw{};bool dirty{},moving{},available{};
            for(;;) {
                if(WaitForSingleObject(stopped.value,0)==WAIT_OBJECT_0||!IsWindow(window)) break;
                auto request=read();
                observePlacementRelease(request);request=read();
                if(request.fixedUi) {
                    if(!renderer.device)renderer.initialize(window);
                    if(uiSerial!=request.fixedUi->serial) {
                        renderer.updateFixedUi(*request.fixedUi);uiSerial=request.fixedUi->serial;
                    }
                    renderer.commitFixedUi();
                }
                // Breathing/hover frames must not postpone the page handoff.
                // Acknowledge its completed frame slot independently of them.
                if(available&&request.retiring&&request.snapshot&&request.snapshot->nativeFrame&&
                    request.snapshot->serial==serial&&retire(serial,shown))continue;
                if(!request.visible||!request.snapshot) {
                    renderer.detach();ready.store(false,std::memory_order_release);
                    renderer.presentedImage.Reset();renderer.placementImage.Reset();
                    renderer.presentedVector.Reset();renderer.placementVector.Reset();
                    renderer.presentedScene.reset();
                    renderer.presentedImageSerial=renderer.placementImageSerial=0;
                    moving=false;dirty=false;
                    feedback={};feedbackWasAnimating=false;
                    HANDLE events[]={stopped.value,changed.value};
                    const DWORD result=WaitForMultipleObjects(2,events,FALSE,renderer.chromeChanged?1:request.monitorRelease?8:INFINITE);
                    if(result==WAIT_OBJECT_0) break;
                    if(result==WAIT_FAILED) throw Failure{HRESULT_FROM_WIN32(GetLastError()),"idle GPU wait"};
                    continue;
                }
                if(!renderer.device) renderer.initialize(window);
                const auto uploadStarted=displayTicks();
                renderer.preparePlacement(request);
                if(serial!=request.snapshot->serial) {
                    if(renderer.upload(*request.snapshot)) available=false;
                    serial=request.snapshot->serial;
                    shown=request.snapshot->nativeFrame?request.target:request.snapshot->initialView;
                    desired=request.target;lastDraw=request.targetUpdatedAt;dirty=true;moving=!request.snapshot->nativeFrame;
                    // Texture creation copied the bytes. Keep no redundant CPU
                    // canvas in the worker between gestures.
                    std::vector<std::byte>().swap(request.snapshot->pixels);
                }
                recordGpuWork(request,displayTicks()-uploadStarted,false);
                if(request.anchoredZoom&&resolvedRevision!=request.targetRevision) {
                    // Re-anchor from the worker's displayed view, never from
                    // ChemDraw's stale native layout or the prior zoom target.
                    auto anchored=anchorPreview(shown,request.cursorX,request.cursorY,request.target.w,request.target.h);
                    anchored=constrainPreview(anchored,request.snapshot->paper,renderer.width,renderer.height);
                    if(!resolveZoom(serial,request.targetRevision,anchored)) continue;
                    request.target=anchored;resolvedRevision=request.targetRevision;
                }
                if(distance(desired,request.target)>0.0) {
                    if(!moving) lastDraw=request.targetUpdatedAt;
                    desired=request.target;moving=true;
                }
                if(ghostRevision!=request.ghostRevision) dirty=true;
                if(highlightRevision!=request.highlightRevision||feedbackReset!=request.feedbackReset) dirty=true;
                if(placementRevision!=request.placementRevision) dirty=true;
                if(alignmentRevision!=request.alignmentRevision) dirty=true;
                feedback.update(request,seconds());
                // Include the terminal zero-opacity frame; stationary blue
                // highlights sleep once their transition has completed.
                if(feedbackWasAnimating||feedback.active(seconds())) dirty=true;
                if(!dirty&&!moving) {
                    if(request.retiring) {
                        // The final Present has been queued. Wait for the next
                        // frame slot before removing the overlay, and re-read
                        // control so a new gesture cannot retire its successor.
                        if(!available) {
                            HANDLE events[]={stopped.value,renderer.frameReady.value,changed.value};
                            const DWORD result=WaitForMultipleObjects(3,events,FALSE,renderer.chromeChanged?1:INFINITE);
                            if(result==WAIT_OBJECT_0) break;
                            if(result==WAIT_FAILED) throw Failure{HRESULT_FROM_WIN32(GetLastError()),"settled GPU frame wait"};
                            available=result==WAIT_OBJECT_0+1;continue;
                        }
                        if(retire(serial,shown)) continue;
                    }
                    HANDLE events[]={stopped.value,changed.value};
                    const DWORD result=WaitForMultipleObjects(2,events,FALSE,renderer.chromeChanged?1:request.monitorRelease?8:INFINITE);
                    if(result==WAIT_OBJECT_0) break;
                    if(result==WAIT_FAILED) throw Failure{HRESULT_FROM_WIN32(GetLastError()),"stationary GPU wait"};
                    continue;
                }
                if(!available) {
                    HANDLE events[]={stopped.value,renderer.frameReady.value,changed.value};
                    const DWORD result=WaitForMultipleObjects(3,events,FALSE,renderer.chromeChanged?1:request.monitorRelease?8:INFINITE);
                    if(result==WAIT_OBJECT_0) break;
                    if(result==WAIT_FAILED) throw Failure{HRESULT_FROM_WIN32(GetLastError()),"GPU frame wait"};
                    available=result==WAIT_OBJECT_0+1;
                    // Re-read the newest target/visibility even when the frame
                    // signal and new input arrive together.
                    continue;
                }
                // Scene upload can outlast a newer UI publication. Coalesce
                // it before presentation instead of showing an obsolete frame.
                const auto latest=read();
                if(!latest.visible||!latest.snapshot||latest.snapshot->serial!=serial||
                    latest.targetRevision!=request.targetRevision||latest.feedbackReset!=request.feedbackReset||
                    latest.placementRevision!=request.placementRevision) { discarded(request);continue; }
                // Hover updates do not invalidate a completed native page.
                // Adopt their newest state before drawing instead of endlessly
                // discarding frames when mouse/idle updates outpace refresh.
                request=latest;
                const double now=seconds();
                // Easing belongs to camera navigation. Native drawing frames
                // and placement handoffs always use their exact coordinates.
                auto next=request.navigation?advanceCamera(shown,desired,now-lastDraw):desired;
                if(request.anchoredZoom)
                    next=constrainPreview(next,request.snapshot->paper,renderer.width,renderer.height);
                feedback.update(request,now);
                const auto drawStarted=displayTicks();renderer.draw(next,feedback,now,request.alignment.get());
                recordGpuWork(request,displayTicks()-drawStarted,true);
                const auto submission=submit(renderer,serial,request);
                if(submission==Submission::Stale) { discarded(request);dirty=true;continue; }
                if(submission==Submission::Busy) {
                    // A busy GPU is not a device failure. Keep the displayed
                    // camera and retry after giving input/device work a chance.
                    HANDLE events[]={stopped.value,changed.value};
                    const auto result=WaitForMultipleObjects(2,events,FALSE,1);
                    if(result==WAIT_OBJECT_0) break;
                    if(result==WAIT_FAILED) throw Failure{HRESULT_FROM_WIN32(GetLastError()),"busy GPU retry"};
                    dirty=true;continue;
                }
                renderer.commit();acknowledge(serial,request,next,feedback,now);
                shown=next;lastDraw=now;available=false;dirty=false;
                finishGhostCommit(request,feedback);
                ghostRevision=request.ghostRevision;
                highlightRevision=request.highlightRevision;feedbackReset=request.feedbackReset;
                placementRevision=request.placementRevision;
                alignmentRevision=request.alignmentRevision;
                feedbackWasAnimating=feedback.active(now);
                moving=distance(shown,desired)>0.0;
            }
        } catch(const Failure& failure) {
            failed.store(true,std::memory_order_release);ready.store(false,std::memory_order_release);
            char message[256]{};
            sprintf_s(message,"CPU navigation fallback: %s failed (0x%08lx).\r\n",failure.operation,
                static_cast<unsigned long>(failure.code));writeGpuStatus(message);
            InvalidateRect(window,nullptr,FALSE);
            PostMessageW(window,gpuSettledMessage(),0,0);
        } catch(const std::exception& error) {
            failed.store(true,std::memory_order_release);ready.store(false,std::memory_order_release);
            char message[300]{};sprintf_s(message,"CPU canvas fallback: %s.\r\n",error.what());writeGpuStatus(message);
            InvalidateRect(window,nullptr,FALSE);
            PostMessageW(window,gpuSettledMessage(),0,0);
        } catch(...) {
            failed.store(true,std::memory_order_release);ready.store(false,std::memory_order_release);
            writeGpuStatus("CPU navigation fallback: GPU worker could not allocate or initialize resources.\r\n");
            InvalidateRect(window,nullptr,FALSE);
            PostMessageW(window,gpuSettledMessage(),0,0);
        }
        ready.store(false,std::memory_order_release);
        if(SUCCEEDED(com)) CoUninitialize();
    }
};

std::shared_ptr<GpuPreview> GpuPreview::create(HWND w) noexcept {
    try {
        auto state=std::make_shared<Impl>(w);
        auto preview=std::shared_ptr<GpuPreview>(new GpuPreview(state));
        // The worker owns its state until it exits. Never join from the message
        // thread: DXGI presentation is allowed to wait for that thread.
        std::thread([state] { state->run(); }).detach();
        return preview;
    } catch(...) {
        writeGpuStatus("CPU navigation fallback: GPU worker could not be started.\r\n");return {};
    }
}
GpuPreview::~GpuPreview() {
    impl->ready.store(false,std::memory_order_release);SetEvent(impl->stopped.value);
}
bool GpuPreview::begin(const void* pixels,int stride,RectI pane,PreviewPaper& paper,PreviewView& initialView,
        std::shared_ptr<const GpuScene> fullPage) noexcept {
    if(!usable()||!valid(pane)||pane.l<0||pane.t<0) return false;
    try {
        auto snapshot=std::make_shared<Snapshot>();snapshot->pane=pane;snapshot->paper=paper;
        const size_t width=size_t(pane.r-pane.l),height=size_t(pane.b-pane.t);
        if(width*height>64000000) return false;
        snapshot->initialView={0,0,double(width),double(height)};
        PreviewView nativeView=snapshot->initialView;
        {
            std::lock_guard guard(impl->mutex);
            const auto& c=impl->control;
            // The full-page capture is a distinct scene from the resident
            // native viewport. Retirement must still wait for a new native
            // publication, rather than treating that older viewport as fresh.
            snapshot->nativeBase=c.scene;
            // A release hold keeps the last complete drag scene on screen.
            // Navigation must capture that scene, not staged cleanup geometry.
            const bool held=c.scenesHeld&&c.snapshot&&c.snapshot->scene;
            const auto scene=held?c.snapshot->scene:fullPage?fullPage:c.scene;
            const auto sourcePane=held?c.snapshot->pane:fullPage?pane:c.scenePane;
            const auto sourcePaper=held?c.snapshot->paper:fullPage?paper:c.scenePaper;
            if(scene&&same(sourcePane,pane)&&sourcePaper.sourceUnits>0&&paper.sourceUnits>0) {
                snapshot->scene=scene;snapshot->paper=sourcePaper;
                const double ratio=sourcePaper.sourceUnits/paper.sourceUnits;
                snapshot->initialView={(sourcePaper.sourceOrigin.x-paper.sourceOrigin.x)/paper.sourceUnits,
                    (sourcePaper.sourceOrigin.y-paper.sourceOrigin.y)/paper.sourceUnits,
                    double(width)*ratio,double(height)*ratio};
            }
            nativeView=snapshot->initialView;
            if(c.navigation&&c.snapshot&&same(c.snapshot->pane,pane)&&
                impl->presentedSerial==c.snapshot->serial&&c.snapshot->paper.sourceUnits>0&&
                snapshot->paper.sourceUnits>0) {
                // A new wheel burst can arrive while the previous native
                // handoff is still easing. Preserve its displayed camera,
                // remapped into the new scene, rather than jump to nativeView.
                const auto& old=c.snapshot->paper;const auto& current=snapshot->paper;
                const auto shown=impl->presented;
                snapshot->initialView={shown.x+(current.sourceOrigin.x-old.sourceOrigin.x)/old.sourceUnits*shown.w/width,
                    shown.y+(current.sourceOrigin.y-old.sourceOrigin.y)/old.sourceUnits*shown.h/height,
                    shown.w*current.sourceUnits/old.sourceUnits,shown.h*current.sourceUnits/old.sourceUnits};
            }
        }
        // A resident vector scene already contains the canvas. Do not allocate
        // or copy a second CPU image that the rendering worker will discard.
        if(!snapshot->scene) {
            if(!pixels||stride<=0||size_t(pane.r)*4>size_t(stride)) return false;
            snapshot->pixels.resize(width*height*4);
            for(size_t y=0;y<height;++y)
                memcpy(snapshot->pixels.data()+y*width*4,
                    static_cast<const std::byte*>(pixels)+(size_t(pane.t)+y)*size_t(stride)+size_t(pane.l)*4,width*4);
        }
        {
            std::lock_guard guard(impl->mutex);snapshot->serial=++impl->nextSerial;
            impl->ready.store(false,std::memory_order_release);
            impl->control.snapshot=snapshot;impl->control.target=nativeView;
            ++impl->control.targetRevision;impl->control.targetUpdatedAt=seconds();
            impl->presented=snapshot->initialView;impl->presentedSerial=snapshot->serial;
            impl->presentedSnapshot.reset();impl->presentedGhost={};impl->presentedGhostOpacity=0;
            impl->control.anchoredZoom=false;
            resetGhostPlacement(impl->control);
            // Keep the ghost and its existing fade through camera capture.
            // Native hover circles can fade away while live targeting catches up.
            impl->control.highlight={};impl->control.endpointHighlight={};++impl->control.highlightRevision;
            impl->control.placementHighlight=false;
            impl->control.visible=true;impl->control.retiring=false;impl->control.navigation=true;
        }
        paper=snapshot->paper;initialView=nativeView;
        SetEvent(impl->changed.value);return true;
    } catch(...) { hide();return false; }
}
bool GpuPreview::fixedUi(std::shared_ptr<const GpuScene> scene,RectI pane) noexcept {
    if(!scene||!usable()||scene->width<=0||scene->height<=0)return false;
    try {
        auto frame=std::make_shared<FixedUiFrame>();frame->scene=std::move(scene);frame->pane=pane;
        {
            std::lock_guard guard(impl->mutex);frame->serial=++impl->nextSerial;
            impl->control.fixedUi=std::move(frame);
        }
        SetEvent(impl->changed.value);return true;
    } catch(...) {return false;}
}
bool GpuPreview::hasFixedUi(RectI pane,RectI client) const noexcept {
    std::lock_guard guard(impl->mutex);const auto& frame=impl->control.fixedUi;
    return frame&&same(frame->pane,pane)&&frame->scene->width==client.r&&frame->scene->height==client.b;
}
bool GpuPreview::nativeScenePresented() const noexcept {
    std::lock_guard guard(impl->mutex);const auto& c=impl->control;
    return usable()&&impl->ready.load(std::memory_order_acquire)&&c.visible&&!c.navigation&&!c.retiring&&
        c.snapshot&&c.snapshot->nativeFrame&&impl->presentedSerial==c.snapshot->serial;
}
bool GpuPreview::scene(std::shared_ptr<const GpuScene> scene,RectI pane,const PreviewPaper& paper,bool replaceNavigation,
    std::shared_ptr<const AlignmentFeedback> alignment) noexcept {
    if(!scene||!usable()) return false;
    pane=intersect(pane,{0,0,scene->height,scene->width});
    if(!valid(pane)) return false;
    try {
        std::lock_guard guard(impl->mutex);impl->control.scene=scene;
        impl->control.scenePane=pane;impl->control.scenePaper=paper;
        auto& c=impl->control;
        if(replaceNavigation||(!c.navigation&&!c.scenesHeld&&(c.ghostPhase!=GhostPhase::Held||c.ghostReleased))) {
            // Immutable page, viewport and camera are already queued. A native
            // no-op repaint must not assign a newer serial that starves Present.
            if(!replaceNavigation&&c.visible&&!c.retiring&&c.snapshot&&c.snapshot->nativeFrame&&
                c.snapshot->scene==scene&&same(c.snapshot->pane,pane)&&samePaper(c.snapshot->paper,paper)&&
                c.ghostPhase!=GhostPhase::Held) return true;
            auto snapshot=std::make_shared<Snapshot>();snapshot->nativeFrame=true;snapshot->scene=scene;
            snapshot->pane=pane;snapshot->paper=paper;snapshot->serial=++impl->nextSerial;
            if(replaceNavigation) {
                // A completed history edit supersedes the old camera image.
                // Replace content and ownership in one publication, without
                // waiting for that image's easing/retirement acknowledgement.
                c.navigation=c.retiring=c.scenesHeld=false;
                resetGhostPlacement(c);
            }
            c.alignment=std::move(alignment);++c.alignmentRevision;
            impl->control.snapshot=snapshot;impl->control.target={0,0,double(pane.r-pane.l),double(pane.b-pane.t)};
            ++impl->control.targetRevision;impl->control.targetUpdatedAt=seconds();
            impl->control.visible=true;impl->control.anchoredZoom=false;
            if(c.ghostPhase==GhostPhase::Held) {
                c.ghostPhase=GhostPhase::Commit;++c.placementRevision;
                if(impl->displayTiming.revision==c.inputRevision&&!impl->displayTiming.nativeQueued)
                    impl->displayTiming.nativeQueued=displayTicks();
            }
        }
        SetEvent(impl->changed.value);return true;
    } catch(...) { hide();return false; }
}
void GpuPreview::clearAlignment() noexcept {
    {std::lock_guard guard(impl->mutex);auto& c=impl->control;if(!c.alignment)return;c.alignment.reset();++c.alignmentRevision;}
    SetEvent(impl->changed.value);
}
void GpuPreview::ghost(const GhostBond& next) noexcept {
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(c.ghostPhase!=GhostPhase::None) return;
        const auto& old=c.ghost;
        if(old.visible==next.visible&&(!next.visible||
            (old.start.x==next.start.x&&old.start.y==next.start.y&&old.end.x==next.end.x&&
             old.end.y==next.end.y&&old.width==next.width&&old.identity==next.identity&&
             old.toolKey==next.toolKey&&old.placement==next.placement&&old.artwork==next.artwork&&
             old.cursorAnchored==next.cursorAnchored&&old.cursorAnchor.x==next.cursorAnchor.x&&
             old.cursorAnchor.y==next.cursorAnchor.y&&
             old.ink==next.ink&&
             old.pressPoint.x==next.pressPoint.x&&old.pressPoint.y==next.pressPoint.y))) return;
        c.ghost=next;++c.ghostRevision;
    }
    SetEvent(impl->changed.value);
}
bool GpuPreview::displayedGhost(GhostBond& next) const noexcept {
    std::lock_guard guard(impl->mutex);const auto& c=impl->control;
    if(!usable()||!c.visible||!impl->presentedSnapshot||!impl->presentedSnapshot->nativeFrame||
        c.navigation||c.scenesHeld||c.ghostPhase!=GhostPhase::None||c.placementHighlight||
        !impl->presentedGhost.visible||impl->presentedGhostOpacity<=0) return false;
    next=impl->presentedGhost;return true;
}
bool GpuPreview::beginGhostPlacement(const GhostBond& expected) noexcept {
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        const auto& frame=impl->presentedSnapshot;const auto& shown=impl->presentedGhost;
        if(!usable()||!c.visible||!frame||!frame->nativeFrame||!frame->scene||c.navigation||c.scenesHeld||
            c.ghostPhase!=GhostPhase::None||!expected.visible||!shown.visible||
            expected.identity!=shown.identity||expected.toolKey!=shown.toolKey||
            expected.artwork!=shown.artwork||expected.start.x!=shown.start.x||expected.start.y!=shown.start.y||
            expected.end.x!=shown.end.x||expected.end.y!=shown.end.y||expected.placement!=shown.placement||
            expected.pressPoint.x!=shown.pressPoint.x||expected.pressPoint.y!=shown.pressPoint.y||
            impl->presentedGhostOpacity<=0||frame->paper.sourceUnits<=0) return false;
        c.placementGhost=shown;c.placementOpacity=impl->presentedGhostOpacity;
        c.placementBase=frame;c.snapshot=frame;c.target=impl->presented;
        ++c.targetRevision;c.targetUpdatedAt=seconds();
        c.ghostReleased=false;c.placementReleaseAt=0;c.ghostPhase=GhostPhase::Held;++c.placementRevision;
        c.ghost={};++c.ghostRevision;
    }
    SetEvent(impl->changed.value);return true;
}
bool GpuPreview::ghostPlacementActive() const noexcept {
    std::lock_guard guard(impl->mutex);
    return usable()&&impl->control.ghostPhase!=GhostPhase::None;
}
void GpuPreview::armPlacementRelease(uint64_t revision,POINT pressed,int dragX,int dragY) noexcept {
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(c.ghostPhase!=GhostPhase::Held||!revision) return;
        c.inputRevision=revision;c.pressScreen=pressed;c.dragX=std::max(1,dragX);c.dragY=std::max(1,dragY);
        impl->displayTiming={};impl->displayTiming.revision=revision;
        c.monitorRelease=true;
    }
    SetEvent(impl->changed.value);
}
PlacementDisplayTiming GpuPreview::placementDisplayTiming() const noexcept {
    std::lock_guard guard(impl->mutex);return impl->displayTiming;
}
void GpuPreview::startGhostRelease() noexcept {
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(c.ghostPhase!=GhostPhase::Held||c.placementReleaseAt>0) return;
        // This is visual feedback only. Scene publication remains held until
        // native idle has finalized geometry, preserving the stable handoff.
        c.monitorRelease=false;c.placementReleaseAt=seconds();++c.placementRevision;
        if(impl->displayTiming.revision==c.inputRevision&&!impl->displayTiming.physicalRelease)
            impl->displayTiming.physicalRelease=displayTicks();
    }
    SetEvent(impl->changed.value);
}
void GpuPreview::releaseGhostPlacement() noexcept {
    std::lock_guard guard(impl->mutex);
    if(impl->control.ghostPhase==GhostPhase::Held) impl->control.ghostReleased=true;
}
void GpuPreview::cancelGhostPlacement() noexcept {
    try {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(c.ghostPhase==GhostPhase::None) return;
        if(c.ghostPhase==GhostPhase::Held&&c.scene&&!c.navigation&&!c.scenesHeld) {
            // A real drag resumes its latest native tracker scene atomically;
            // never expose a blank retained page between the two owners.
            auto snapshot=std::make_shared<Snapshot>();snapshot->nativeFrame=true;snapshot->scene=c.scene;
            snapshot->pane=c.scenePane;snapshot->paper=c.scenePaper;snapshot->serial=++impl->nextSerial;
            c.snapshot=snapshot;c.target={0,0,double(c.scenePane.r-c.scenePane.l),double(c.scenePane.b-c.scenePane.t)};
            ++c.targetRevision;c.targetUpdatedAt=seconds();
        }
        resetGhostPlacement(c);c.ghost={};++c.ghostRevision;
    } catch(...) { hide();return; }
    SetEvent(impl->changed.value);
}
void GpuPreview::highlight(const HoverHighlight& next) noexcept {
    if(next.scene&&(!valid(next.pane)||!valid(next.bounds)||!std::isfinite(next.paper.sourceUnits)||next.paper.sourceUnits<=0)) return;
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(c.highlight.scene==next.scene&&c.highlight.identity==next.identity) return;
        c.highlight=next;++c.highlightRevision;
    }
    SetEvent(impl->changed.value);
}
void GpuPreview::retainHighlight(uint64_t identity) noexcept {
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(c.placementHighlight||c.ghostPhase!=GhostPhase::None) return;
        if(!c.highlight.scene||(identity&&c.highlight.identity==identity)) return;
        c.highlight={};++c.highlightRevision;
    }
    SetEvent(impl->changed.value);
}
void GpuPreview::clearHoverHighlight() noexcept {
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(!c.placementHighlight&&!c.highlight.scene&&!c.endpointHighlight.scene)return;
        // Cursor departure can end the blue hover independently of the held
        // drawing/ghost handoff. Do not cancel its page, ghost or fade phase.
        c.placementHighlight=false;c.highlight={};c.endpointHighlight={};++c.highlightRevision;
    }
    SetEvent(impl->changed.value);
}
void GpuPreview::clearFeedback(bool immediate) noexcept {
    bool changed=immediate;
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(c.ghostPhase!=GhostPhase::None) return;
        c.placementHighlight=false;
        if(c.endpointHighlight.scene) { c.endpointHighlight={};++c.highlightRevision;changed=true; }
        if(c.ghost.visible) { c.ghost={};++c.ghostRevision;changed=true; }
        if(c.highlight.scene) { c.highlight={};++c.highlightRevision;changed=true; }
        if(immediate) ++c.feedbackReset;
    }
    if(changed) SetEvent(impl->changed.value);
}
bool GpuPreview::beginPlacementHighlight(uint64_t identity) noexcept {
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(!usable()||!c.visible||!c.snapshot||!c.snapshot->nativeFrame||c.navigation||
            !c.highlight.scene||c.highlight.identity!=identity) return false;
        c.placementHighlight=true;c.endpointHighlight={};
        if(c.ghostPhase==GhostPhase::None) { c.ghost={};++c.ghostRevision; }
        ++c.highlightRevision;
    }
    SetEvent(impl->changed.value);return true;
}
bool GpuPreview::placementEndpoint(const HoverHighlight& next) noexcept {
    if(next.scene&&(!valid(next.pane)||!valid(next.bounds)||!std::isfinite(next.paper.sourceUnits)||next.paper.sourceUnits<=0)) return false;
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(!usable()||!c.placementHighlight||c.navigation) return false;
        if(c.endpointHighlight.scene==next.scene&&c.endpointHighlight.identity==next.identity) return true;
        c.endpointHighlight=next;++c.highlightRevision;
    }
    SetEvent(impl->changed.value);return true;
}
bool GpuPreview::finishPlacementHighlight(const HoverHighlight& next) noexcept {
    if(next.scene&&(!valid(next.pane)||!valid(next.bounds)||!std::isfinite(next.paper.sourceUnits)||next.paper.sourceUnits<=0)) return false;
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(!usable()||!c.placementHighlight||c.scenesHeld||c.navigation) return false;
        // A same-identity update preserves the current fade; a new target
        // crossfades directly from the source without an intervening empty state.
        c.highlight=next;c.endpointHighlight={};c.placementHighlight=false;++c.highlightRevision;
    }
    SetEvent(impl->changed.value);return true;
}
void GpuPreview::holdScenes(bool hold,bool publish) noexcept {
    std::shared_ptr<const GpuScene> next;RectI pane{};PreviewPaper paper{};
    {
        std::lock_guard guard(impl->mutex);impl->control.scenesHeld=hold;
        if(hold) {
            if(impl->control.ghostPhase==GhostPhase::None) { impl->control.ghost={};++impl->control.ghostRevision; }
            if(!impl->control.placementHighlight&&impl->control.ghostPhase==GhostPhase::None) {
                impl->control.highlight={};impl->control.endpointHighlight={};++impl->control.highlightRevision;++impl->control.feedbackReset;
            }
            SetEvent(impl->changed.value);
        }
        if(!hold&&publish) { next=impl->control.scene;pane=impl->control.scenePane;paper=impl->control.scenePaper; }
    }
    if(next) scene(std::move(next),pane,paper);
}
bool GpuPreview::usable() const noexcept { return !impl->failed.load(std::memory_order_acquire); }
bool GpuPreview::hasScene() const noexcept {
    std::lock_guard guard(impl->mutex);return usable()&&bool(impl->control.scene);
}
void GpuPreview::move(double left,double top,double width,double height) noexcept {
    constexpr double limit=double(std::numeric_limits<float>::max())*0.25;
    if(!std::isfinite(left+top+width+height)||width<=0||height<=0||
        std::max({std::abs(left),std::abs(top),width,height})>limit) return;
    if(impl->failed.load(std::memory_order_acquire)) return;
    {
        std::lock_guard guard(impl->mutex);
        const Transform target{left,top,width,height};impl->control.anchoredZoom=false;
        if(distance(impl->control.target,target)==0.0) return;
        impl->control.target=target;
        ++impl->control.targetRevision;impl->control.targetUpdatedAt=seconds();
    }
    SetEvent(impl->changed.value);
}
bool GpuPreview::zoomAt(double cursorX,double cursorY,double width,double height) noexcept {
    constexpr double limit=double(std::numeric_limits<float>::max())*0.25;
    if(!usable()||!std::isfinite(cursorX+cursorY+width+height)||width<=0||height<=0||
        std::max({std::abs(cursorX),std::abs(cursorY),width,height})>limit) return false;
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        if(!c.visible||!c.navigation||c.retiring||!c.snapshot) return false;
        const auto pane=c.snapshot->pane;
        const double w=pane.r-pane.l,h=pane.b-pane.t;
        const Transform shown=impl->presentedSerial==c.snapshot->serial?impl->presented:Transform{0,0,w,h};
        c.target=constrainPreview(anchorPreview(shown,cursorX,cursorY,width,height),c.snapshot->paper,w,h);
        c.cursorX=cursorX;c.cursorY=cursorY;c.anchoredZoom=true;
        ++c.targetRevision;c.targetUpdatedAt=seconds();
    }
    SetEvent(impl->changed.value);return true;
}
bool GpuPreview::view(PreviewView& shown,PreviewView& target) const noexcept {
    std::lock_guard guard(impl->mutex);const auto& c=impl->control;
    if(!c.navigation||!c.snapshot||impl->presentedSerial!=c.snapshot->serial) return false;
    shown=impl->presented;target=c.target;return true;
}
bool GpuPreview::cameraFrame(PreviewView& shown,PreviewPaper& paper,RectI& pane,bool consumeNotification) const noexcept {
    std::lock_guard guard(impl->mutex);
    if(consumeNotification)impl->cameraNotificationPending=false;
    const auto& c=impl->control;const auto& frame=impl->presentedSnapshot;
    // The displayed native scene can still precede a newly submitted native
    // camera/scene. Ghost targeting must use this immutable pair at rest too.
    if(!usable()||!c.visible||!frame||impl->presentedSerial!=frame->serial)return false;
    shown=impl->presented;paper=frame->paper;pane=frame->pane;
    return shown.w>0&&shown.h>0&&paper.sourceUnits>0&&valid(pane);
}
bool GpuPreview::active() const noexcept { return impl->ready.load(std::memory_order_acquire); }
bool GpuPreview::freezeCamera(PreviewView& shown,PreviewPaper& paper,RectI& pane) noexcept {
    {
        std::lock_guard guard(impl->mutex);auto& c=impl->control;
        const auto& frame=impl->presentedSnapshot;
        if(!usable()||!c.visible||!c.navigation||!frame||!c.snapshot||
            c.snapshot->serial!=frame->serial||impl->presentedSerial!=frame->serial)return false;
        shown=impl->presented;paper=frame->paper;pane=frame->pane;
        if(shown.w<=0||shown.h<=0||paper.sourceUnits<=0||!valid(pane)||
            !std::isfinite(shown.x+shown.y+shown.w+shown.h+paper.sourceUnits))return false;
        // Undo interrupts navigation where it is displayed, rather than
        // completing the wheel/scrollbar target before checking visibility.
        c.target=shown;c.anchoredZoom=false;
        ++c.targetRevision;c.targetUpdatedAt=seconds();
    }
    SetEvent(impl->changed.value);return true;
}
bool GpuPreview::settled() const noexcept {
    std::lock_guard guard(impl->mutex);
    return usable()&&impl->ready.load(std::memory_order_acquire)&&
        impl->control.visible&&impl->settledRevision==impl->control.targetRevision;
}
void GpuPreview::finish() noexcept {
    {
        std::lock_guard guard(impl->mutex);impl->control.retiring=true;
    }
    SetEvent(impl->changed.value);
}
void GpuPreview::hide() noexcept {
    {
        std::lock_guard guard(impl->mutex);impl->control.visible=false;impl->control.navigation=false;
        impl->presentedSnapshot.reset();impl->presentedGhost={};impl->presentedGhostOpacity=0;
        resetGhostPlacement(impl->control);
        impl->control.ghost={};++impl->control.ghostRevision;
        impl->control.highlight={};impl->control.endpointHighlight={};++impl->control.highlightRevision;++impl->control.feedbackReset;
        impl->control.placementHighlight=false;
        impl->ready.store(false,std::memory_order_release);
    }
    SetEvent(impl->changed.value);
}
void GpuPreview::clearScene() noexcept {
    {
        std::lock_guard guard(impl->mutex);impl->control.scene.reset();impl->control.navigation=false;impl->control.scenesHeld=false;
        impl->presentedSnapshot.reset();impl->presentedGhost={};impl->presentedGhostOpacity=0;
        resetGhostPlacement(impl->control);
        impl->control.ghost={};++impl->control.ghostRevision;
        impl->control.highlight={};impl->control.endpointHighlight={};++impl->control.highlightRevision;++impl->control.feedbackReset;
        impl->control.placementHighlight=false;
        impl->control.visible=false;impl->ready.store(false,std::memory_order_release);
    }
    SetEvent(impl->changed.value);
}
}

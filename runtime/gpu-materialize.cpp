#include "gpu-scene-renderer.hpp"
#include "placement-runtime.hpp"
#include <d3d11.h>
#include <dxgi.h>
#include <d2d1_1helper.h>
namespace cd {
using Microsoft::WRL::ComPtr;
std::shared_ptr<const ScenePixels> materializeGpuScene(const std::shared_ptr<const GpuScene>& scene) {
    if(!scene) return {};
    PlacementTimer timer(PlacementCost::PixelReadback);
    // This context belongs exclusively to the native UI thread. It never waits
    // for a presentation worker whose swap chain may itself require that thread.
    struct NativePixels {
        ComPtr<ID3D11Device> d3d;ComPtr<IDXGIDevice> dxgi;
        ComPtr<ID2D1Factory1> factory;ComPtr<ID2D1Device> device;
        ComPtr<ID2D1DeviceContext> context;SceneRenderer renderer;
        void initialize() {
            auto require=[](HRESULT r) { if(FAILED(r)) throw std::runtime_error("Hardware native-pixel bridge failed"); };
            require(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr,0,D3D11_SDK_VERSION,d3d.GetAddressOf(),nullptr,nullptr));
            require(d3d.As(&dxgi));
            require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),nullptr,
                reinterpret_cast<void**>(factory.GetAddressOf())));
            require(factory->CreateDevice(dxgi.Get(),device.GetAddressOf()));
            require(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.GetAddressOf()));
            context->SetDpi(96,96);renderer.initialize(context.Get(),factory.Get());
        }
    };
    static thread_local std::unique_ptr<NativePixels> bridge;
    if(!bridge) { auto value=std::make_unique<NativePixels>();value->initialize();bridge=std::move(value); }
    auto source=bridge->renderer.render(scene);auto result=std::make_shared<ScenePixels>();
    result->width=scene->width;result->height=scene->height;result->bytes.resize(size_t(scene->width)*scene->height*4);
    auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    ComPtr<ID2D1Bitmap1> staging;
    HRESULT r=bridge->context->CreateBitmap(D2D1::SizeU(scene->width,scene->height),nullptr,0,&properties,staging.GetAddressOf());
    if(SUCCEEDED(r)) r=staging->CopyFromBitmap(nullptr,source.Get(),nullptr);
    D2D1_MAPPED_RECT mapped{};if(SUCCEEDED(r)) r=staging->Map(D2D1_MAP_OPTIONS_READ,&mapped);
    if(FAILED(r)) throw std::runtime_error("Native pixel readback failed");
    for(int y=0;y<scene->height;++y) memcpy(result->bytes.data()+size_t(y)*scene->width*4,mapped.bits+size_t(y)*mapped.pitch,size_t(scene->width)*4);
    staging->Unmap();bridge->renderer.prune();return result;
}
}

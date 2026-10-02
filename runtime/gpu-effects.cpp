#include "gpu-effects.hpp"
#include <d2d1effectauthor.h>
#include <d2d1effects.h>
#include <d2d1_1helper.h>
#include "gradient-shader.hpp"
#include "image-effect-shader.hpp"
#include "raster-op-shader.hpp"
namespace cd {
using Microsoft::WRL::ComPtr;
namespace {
const GUID gradientId={0x46e1bc12,0xb957,0x4b67,{0xa8,0x41,0x6e,0x89,0xb5,0x5c,0x9d,0x01}};
const GUID imageId={0x46e1bc12,0xb957,0x4b67,{0xa8,0x41,0x6e,0x89,0xb5,0x5c,0x9d,0x02}};
const GUID rasterId={0x46e1bc12,0xb957,0x4b67,{0xa8,0x41,0x6e,0x89,0xb5,0x5c,0x9d,0x03}};
struct F4 { float x{},y{},z{},w{}; };
struct GradientData { F4 bounds,inverse0,inverse1,center,color,options;F4 vertices[256],colors[256],blend[64],stops[64]; };
struct ImageData { F4 bounds,options,low,high,matrix[5],gray[5],table[256],from[64],to[64]; };
F4 rgba(uint32_t c) { return {float((c>>16)&255)/255,float((c>>8)&255)/255,float(c&255)/255,float(c>>24)/255}; }
void checked(HRESULT r,const char* operation="Direct2D effect operation") {
    if(FAILED(r)) {
        char message[180]{};
        sprintf_s(message,"%s failed (0x%08lx)",operation,static_cast<unsigned long>(r));
        throw std::runtime_error(message);
    }
}
class ShaderEffect final : public ID2D1EffectImpl,public ID2D1DrawTransform {
    std::atomic<ULONG> references{1};bool gradient,raster;
    std::vector<BYTE> data;
    ComPtr<ID2D1DrawInfo> info;
public:
    explicit ShaderEffect(int kind):gradient(kind==0),raster(kind==2),data(kind==0?sizeof(GradientData):kind==2?sizeof(F4):sizeof(ImageData)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out) override {
        if(!out) return E_POINTER;*out=nullptr;
        if(id==__uuidof(IUnknown)||id==__uuidof(ID2D1EffectImpl)) *out=static_cast<ID2D1EffectImpl*>(this);
        else if(id==__uuidof(ID2D1DrawTransform)||id==__uuidof(ID2D1Transform)||id==__uuidof(ID2D1TransformNode))
            *out=static_cast<ID2D1DrawTransform*>(this);
        if(!*out) return E_NOINTERFACE;AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
    ULONG STDMETHODCALLTYPE Release() override { auto n=--references;if(!n) delete this;return n; }
    HRESULT STDMETHODCALLTYPE Initialize(ID2D1EffectContext* c,ID2D1TransformGraph* graph) override {
        HRESULT r=c->LoadPixelShader(gradient?gradientId:raster?rasterId:imageId,
            gradient?gradientShader:raster?rasterOpShader:imageEffectShader,
            gradient?UINT(sizeof(gradientShader)):raster?UINT(sizeof(rasterOpShader)):UINT(sizeof(imageEffectShader)));
        return FAILED(r)?r:graph->SetSingleTransformNode(this);
    }
    HRESULT STDMETHODCALLTYPE PrepareForRender(D2D1_CHANGE_TYPE) override {
        return info?info->SetPixelShaderConstantBuffer(data.data(),UINT(data.size())):E_UNEXPECTED;
    }
    HRESULT STDMETHODCALLTYPE SetGraph(ID2D1TransformGraph*) override { return E_NOTIMPL; }
    UINT32 STDMETHODCALLTYPE GetInputCount() const override { return gradient?0:raster?2:1; }
    HRESULT STDMETHODCALLTYPE MapOutputRectToInputRects(const D2D1_RECT_L* r,D2D1_RECT_L* inputs,UINT n) const override {
        if(n!=GetInputCount()) return E_INVALIDARG;for(UINT i=0;i<n;++i) inputs[i]=*r;return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MapInputRectsToOutputRect(const D2D1_RECT_L* inputs,const D2D1_RECT_L*,UINT n,
        D2D1_RECT_L* output,D2D1_RECT_L* opaque) override {
        if(n!=GetInputCount()) return E_INVALIDARG;
        if(gradient) {
            auto b=reinterpret_cast<const F4*>(data.data());
            *output={LONG(std::floor(b->x)),LONG(std::floor(b->y)),LONG(std::ceil(b->z)),LONG(std::ceil(b->w))};
        } else *output=inputs[0];
        *opaque={};return S_OK;
    }
    HRESULT STDMETHODCALLTYPE MapInvalidRect(UINT,D2D1_RECT_L r,D2D1_RECT_L* out) const override { *out=r;return S_OK; }
    HRESULT STDMETHODCALLTYPE SetDrawInfo(ID2D1DrawInfo* i) override {
        info=i;return i->SetPixelShader(gradient?gradientId:raster?rasterId:imageId,D2D1_PIXEL_OPTIONS_NONE);
    }
    static HRESULT CALLBACK set(IUnknown* object,const BYTE* source,UINT n) {
        auto effect=static_cast<ShaderEffect*>(static_cast<ID2D1EffectImpl*>(object));
        if(!source||n!=effect->data.size()) return E_INVALIDARG;
        memcpy(effect->data.data(),source,n);return S_OK;
    }
    static HRESULT CALLBACK get(const IUnknown* object,BYTE* out,UINT n,UINT* actual) {
        auto effect=static_cast<const ShaderEffect*>(static_cast<const ID2D1EffectImpl*>(object));
        if(actual) *actual=UINT(effect->data.size());if(!out) return S_OK;
        if(n<effect->data.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
        memcpy(out,effect->data.data(),effect->data.size());return S_OK;
    }
    static HRESULT CALLBACK createGradient(IUnknown** out) { *out=static_cast<ID2D1EffectImpl*>(new ShaderEffect(0));return S_OK; }
    static HRESULT CALLBACK createImage(IUnknown** out) { *out=static_cast<ID2D1EffectImpl*>(new ShaderEffect(1));return S_OK; }
    static HRESULT CALLBACK createRaster(IUnknown** out) { *out=static_cast<ID2D1EffectImpl*>(new ShaderEffect(2));return S_OK; }
};
}
void registerGpuEffects(ID2D1Factory1* factory) {
    const D2D1_PROPERTY_BINDING binding={L"Data",ShaderEffect::set,ShaderEffect::get};
    const WCHAR* gradientXml=LR"(<?xml version='1.0'?><Effect><Property name='DisplayName' type='string' value='ChemDraw path gradient'/><Property name='Author' type='string' value='ChemDrawLatency'/><Property name='Category' type='string' value='Drawing'/><Property name='Description' type='string' value='Native path gradient'/><Inputs/><Property name='Data' type='blob'><Property name='DisplayName' type='string' value='Shader constants'/></Property></Effect>)";
    const WCHAR* imageXml=LR"(<?xml version='1.0'?><Effect><Property name='DisplayName' type='string' value='ChemDraw image attributes'/><Property name='Author' type='string' value='ChemDrawLatency'/><Property name='Category' type='string' value='Drawing'/><Property name='Description' type='string' value='Native image attributes'/><Inputs><Input name='Source'/></Inputs><Property name='Data' type='blob'><Property name='DisplayName' type='string' value='Shader constants'/></Property></Effect>)";
    checked(factory->RegisterEffectFromString(gradientId,gradientXml,&binding,1,ShaderEffect::createGradient),"Register gradient shader effect");
    checked(factory->RegisterEffectFromString(imageId,imageXml,&binding,1,ShaderEffect::createImage),"Register image shader effect");
    const WCHAR* rasterXml=LR"(<?xml version='1.0'?><Effect><Property name='DisplayName' type='string' value='ChemDraw GDI raster operation'/><Property name='Author' type='string' value='ChemDrawLatency'/><Property name='Category' type='string' value='Drawing'/><Property name='Description' type='string' value='Ordered bitwise GPU drawing'/><Inputs><Input name='Destination'/><Input name='Ink'/></Inputs><Property name='Data' type='blob'><Property name='DisplayName' type='string' value='Shader constants'/></Property></Effect>)";
    checked(factory->RegisterEffectFromString(rasterId,rasterXml,&binding,1,ShaderEffect::createRaster),"Register raster shader effect");
}
ComPtr<ID2D1Effect> gpuGradient(ID2D1DeviceContext* c,const SceneBrush& brush,SceneRect bounds) {
    auto& value=*brush.gradient;GradientData d{};
    d.bounds={bounds.x,bounds.y,bounds.x+bounds.w,bounds.y+bounds.h};
    auto inverse=D2D1::Matrix3x2F(brush.transform.m11,brush.transform.m12,brush.transform.m21,brush.transform.m22,brush.transform.dx,brush.transform.dy);
    if(!inverse.Invert()) throw std::runtime_error("Singular gradient transform");
    d.inverse0={inverse._11,inverse._21,inverse._31,0};d.inverse1={inverse._12,inverse._22,inverse._32,0};
    d.center={value.center.x,value.center.y,value.focus.x,value.focus.y};d.color=rgba(value.color);
    d.options={float(value.boundary.points.size()),float(value.stops.empty()?value.blend.size():value.stops.size()),value.stops.empty()?0.f:1.f,value.gamma?1.f:0.f};
    if(value.boundary.points.size()>256||value.blend.size()>64||value.stops.size()>64) throw std::runtime_error("Gradient shader capacity");
    for(size_t i=0;i<value.boundary.points.size();++i) {
        auto p=value.boundary.points[i];d.vertices[i]={p.x,p.y,0,0};d.colors[i]=rgba(value.surround[i%value.surround.size()]);
    }
    for(size_t i=0;i<value.blend.size();++i) d.blend[i]={value.blend[i].x,value.blend[i].y,0,0};
    for(size_t i=0;i<value.stops.size();++i) { d.blend[i].x=value.stops[i].position;d.stops[i]=rgba(value.stops[i].color); }
    ComPtr<ID2D1Effect> effect;checked(c->CreateEffect(gradientId,effect.GetAddressOf()),"Create gradient shader effect");
    checked(effect->SetValue(0,D2D1_PROPERTY_TYPE_BLOB,reinterpret_cast<const BYTE*>(&d),sizeof(d)));return effect;
}
ComPtr<ID2D1Effect> gpuImageEffect(ID2D1DeviceContext* c,ID2D1Image* input,const SceneEffect& value) {
    ComPtr<ID2D1Effect> effect;
    if(value.kind==SceneEffect::Blur) {
        checked(c->CreateEffect(CLSID_D2D1GaussianBlur,effect.GetAddressOf()));effect->SetInput(0,input);
        checked(effect->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION,value.amount/3.f));
        checked(effect->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE,D2D1_BORDER_MODE_HARD));return effect;
    }
    ImageData d{};d.options={float(value.kind),value.amount,float(value.flags),0};d.low=rgba(value.low);d.high=rgba(value.high);
    for(int i=0;i<5;++i) d.matrix[i]={value.matrix[i*5],value.matrix[i*5+1],value.matrix[i*5+2],value.matrix[i*5+3]};
    for(int i=0;i<5;++i) d.gray[i]={value.grayMatrix[i*5],value.grayMatrix[i*5+1],value.grayMatrix[i*5+2],value.grayMatrix[i*5+3]};
    for(int i=0;i<256;++i) d.table[i]={value.table[0][i],value.table[1][i],value.table[2][i],value.table[3][i]};
    if(value.remap.size()>64) throw std::runtime_error("Image remap shader capacity");
    if(value.kind==SceneEffect::Remap) d.options.y=float(value.remap.size());
    for(size_t i=0;i<value.remap.size();++i) { d.from[i]=rgba(value.remap[i][0]);d.to[i]=rgba(value.remap[i][1]); }
    checked(c->CreateEffect(imageId,effect.GetAddressOf()),"Create image shader effect");effect->SetInput(0,input);
    checked(effect->SetValue(0,D2D1_PROPERTY_TYPE_BLOB,reinterpret_cast<const BYTE*>(&d),sizeof(d)));return effect;
}
ComPtr<ID2D1Effect> gpuRasterEffect(ID2D1DeviceContext* c,ID2D1Image* destination,ID2D1Image* ink,uint32_t truth) {
    ComPtr<ID2D1Effect> effect;checked(c->CreateEffect(rasterId,effect.GetAddressOf()),"Create raster shader effect");
    effect->SetInput(0,destination);effect->SetInput(1,ink);F4 data{float(truth),0,0,0};
    checked(effect->SetValue(0,D2D1_PROPERTY_TYPE_BLOB,reinterpret_cast<const BYTE*>(&data),sizeof(data)));return effect;
}
}

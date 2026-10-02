#include "gpu-scene-renderer.hpp"
#include <d2d1_1helper.h>
#include <d2d1_3.h>
#include "gpu-effects.hpp"

namespace cd {
using Microsoft::WRL::ComPtr;
namespace {
void requireScene(HRESULT result) {
    if(FAILED(result)) {
        char message[100]{};sprintf_s(message,"GPU vector scene rendering failed (0x%08lx)",static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}
D2D1::Matrix3x2F matrix(SceneMatrix m) { return {m.m11,m.m12,m.m21,m.m22,m.dx,m.dy}; }
D2D1_COLOR_F color(uint32_t c) { return D2D1::ColorF(c&0xffffff,float(c>>24)/255.0f); }
D2D1_RECT_F rect(SceneRect r) { return {r.x,r.y,r.x+r.w,r.y+r.h}; }
}
void SceneRenderer::initialize(ID2D1DeviceContext* c,ID2D1Factory1* f) {
    shapes.clear();shapeKeys.clear();sharedShapes.clear();shapeBytes=0;
    navigationImages.clear();navigationSource.reset();retainedNavigation.Reset();navigationRecording=false;
    context=c;factory=f;effectsRegistered=false;
}
ComPtr<ID2D1GdiMetafile> SceneRenderer::metafile(const std::shared_ptr<const SceneMetafile>& source) {
    if(auto i=metafiles.find(source.get());i!=metafiles.end()&&!i->second.source.expired()) return i->second.metafile;
    HGLOBAL storage=GlobalAlloc(GMEM_MOVEABLE,source->bytes.size());
    if(!storage) throw std::bad_alloc();
    void* bytes=GlobalLock(storage);if(!bytes) { GlobalFree(storage);throw std::bad_alloc(); }
    memcpy(bytes,source->bytes.data(),source->bytes.size());GlobalUnlock(storage);
    ComPtr<IStream> stream;auto result=CreateStreamOnHGlobal(storage,TRUE,stream.GetAddressOf());
    if(FAILED(result)) GlobalFree(storage);requireScene(result);
    ComPtr<ID2D1GdiMetafile> value;requireScene(factory->CreateGdiMetafile(stream.Get(),value.GetAddressOf()));
    metafiles[source.get()]={source,value};return value;
}
ComPtr<ID2D1Bitmap1> SceneRenderer::bitmap(const std::shared_ptr<const ScenePixels>& pixels) {
    ComPtr<ID2D1Bitmap1> result;
    if(!pixels) return result;
    if(auto i=textures.find(pixels.get());i!=textures.end()&&!i->second.pixels.expired()) return i->second.bitmap;
    auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    requireScene(context->CreateBitmap(D2D1::SizeU(pixels->width,pixels->height),pixels->bytes.data(),
        pixels->width*4,&properties,result.GetAddressOf()));
    textures[pixels.get()]={pixels,result};return result;
}
ComPtr<ID2D1Brush> SceneRenderer::brush(const SceneBrush& value) {
    ComPtr<ID2D1Brush> result;
    const auto properties=D2D1::BrushProperties(1.0f,matrix(value.transform));
    if(value.kind==SceneBrush::Solid) {
        // Direct2D records brush state for each draw. Reuse one brush instead
        // of creating a device resource for every bond outline on each repaint.
        if(!solidBrush) requireScene(context->CreateSolidColorBrush(color(value.color),solidBrush.GetAddressOf()));
        solidBrush->SetColor(color(value.color));solidBrush->SetTransform(matrix(value.transform));
        solidBrush->SetOpacity(1.0f);solidBrush.As(&result);
    } else if(value.kind==SceneBrush::Texture) {
        auto image=bitmap(value.texture);ComPtr<ID2D1BitmapBrush1> b;
        auto mode=value.clamp?D2D1_EXTEND_MODE_CLAMP:D2D1_EXTEND_MODE_WRAP;
        auto mx=(value.wrap==1||value.wrap==3)?D2D1_EXTEND_MODE_MIRROR:mode;
        auto my=(value.wrap==2||value.wrap==3)?D2D1_EXTEND_MODE_MIRROR:mode;
        auto options=D2D1::BitmapBrushProperties1(mx,my,D2D1_INTERPOLATION_MODE_LINEAR);
        requireScene(context->CreateBitmapBrush(image.Get(),&options,&properties,b.GetAddressOf()));b.As(&result);
    } else {
        std::vector<D2D1_GRADIENT_STOP> stops;
        for(auto s:value.stops) stops.push_back({s.position,color(s.color)});
        ComPtr<ID2D1GradientStopCollection> collection;
        requireScene(context->CreateGradientStopCollection(stops.data(),UINT(stops.size()),
            value.gamma?D2D1_GAMMA_1_0:D2D1_GAMMA_2_2,
            value.clamp?D2D1_EXTEND_MODE_CLAMP:(value.wrap?D2D1_EXTEND_MODE_MIRROR:D2D1_EXTEND_MODE_WRAP),collection.GetAddressOf()));
        if(value.kind==SceneBrush::Linear) {
            ComPtr<ID2D1LinearGradientBrush> b;
            auto options=D2D1::LinearGradientBrushProperties({value.start.x,value.start.y},{value.end.x,value.end.y});
            requireScene(context->CreateLinearGradientBrush(&options,&properties,collection.Get(),b.GetAddressOf()));b.As(&result);
        } else {
            ComPtr<ID2D1RadialGradientBrush> b;
            auto options=D2D1::RadialGradientBrushProperties({value.center.x,value.center.y},
                {value.focus.x,value.focus.y},value.radiusX,value.radiusY);
            requireScene(context->CreateRadialGradientBrush(&options,&properties,collection.Get(),b.GetAddressOf()));b.As(&result);
        }
    }
    return result;
}
void SceneRenderer::feedbackPath(const ScenePath& path,ID2D1Brush* brush,float width,bool filled) {
    auto shape=geometry(path);
    if(filled) context->FillGeometry(shape.Get(),brush);
    else context->DrawGeometry(shape.Get(),brush,width);
}
ComPtr<ID2D1PathGeometry> SceneRenderer::geometry(const ScenePath& path,const std::shared_ptr<const ScenePath>& shared) {
    if(shared) if(auto i=sharedShapes.find(shared.get());i!=sharedShapes.end()) {
        const auto entry=i->second;
        if(entry->owner.lock()==shared) { shapes.splice(shapes.end(),shapes,entry);return entry->shape; }
        sharedShapes.erase(i);
    }
    auto rememberShared=[&](std::list<CachedGeometry>::iterator entry) {
        if(!shared) return;
        if(entry->ownerKey) if(auto i=sharedShapes.find(entry->ownerKey);i!=sharedShapes.end()&&i->second==entry) sharedShapes.erase(i);
        entry->owner=shared;entry->ownerKey=shared.get();
        try { sharedShapes.insert_or_assign(shared.get(),entry); }
        catch(const std::bad_alloc&) { entry->owner.reset();entry->ownerKey=nullptr; }
    };
    uint64_t key=14695981039346656037ULL;
    auto hash=[&](const void* data,size_t bytes) {
        auto source=static_cast<const BYTE*>(data);
        for(size_t i=0;i<bytes;++i) { key^=source[i];key*=1099511628211ULL; }
    };
    const size_t pointCount=path.points.size(),typeCount=path.types.size();
    hash(&path.winding,sizeof(path.winding));hash(&pointCount,sizeof(pointCount));hash(&typeCount,sizeof(typeCount));
    hash(path.points.data(),pointCount*sizeof(ScenePoint));hash(path.types.data(),typeCount);
    auto range=shapeKeys.equal_range(key);
    for(auto i=range.first;i!=range.second;++i) {
        auto entry=i->second;const auto& old=entry->path;
        // Content equality resolves collisions and avoids native object/path
        // pointers. Transform, clipping and color remain live command state.
        if(old.winding==path.winding&&old.points.size()==pointCount&&old.types==path.types&&
            (!pointCount||memcmp(old.points.data(),path.points.data(),pointCount*sizeof(ScenePoint))==0)) {
            shapes.splice(shapes.end(),shapes,entry);rememberShared(entry);return entry->shape;
        }
    }
    auto result=buildGeometry(path);
    constexpr uint64_t budget=32ULL*1024*1024;
    const uint64_t bytes=sizeof(CachedGeometry)+uint64_t(pointCount)*96+typeCount;
    if(bytes>budget) return result;
    while(!shapes.empty()&&(shapeBytes+bytes>budget||shapes.size()>=32768)) {
        auto oldest=shapes.begin();auto entries=shapeKeys.equal_range(oldest->key);
        for(auto i=entries.first;i!=entries.second;++i) if(i->second==oldest) { shapeKeys.erase(i);break; }
        if(oldest->ownerKey) if(auto i=sharedShapes.find(oldest->ownerKey);i!=sharedShapes.end()&&i->second==oldest) sharedShapes.erase(i);
        shapeBytes-=oldest->bytes;shapes.erase(oldest);
    }
    shapes.push_back({key,bytes,path,result});auto entry=std::prev(shapes.end());
    try { shapeKeys.emplace(key,entry); } catch(...) { shapes.erase(entry);throw; }
    shapeBytes+=bytes;rememberShared(entry);return result;
}
ComPtr<ID2D1PathGeometry> SceneRenderer::buildGeometry(const ScenePath& path) {
    ComPtr<ID2D1PathGeometry> result;requireScene(factory->CreatePathGeometry(result.GetAddressOf()));
    ComPtr<ID2D1GeometrySink> sink;requireScene(result->Open(sink.GetAddressOf()));
    sink->SetFillMode(path.winding?D2D1_FILL_MODE_WINDING:D2D1_FILL_MODE_ALTERNATE);
    bool open{};
    for(size_t i=0;i<path.points.size();++i) {
        auto p=path.points[i];const BYTE type=path.types[i]&Gdiplus::PathPointTypePathTypeMask;
        if(type==Gdiplus::PathPointTypeStart) {
            if(open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->BeginFigure({p.x,p.y},D2D1_FIGURE_BEGIN_FILLED);open=true;
        } else if(type==Gdiplus::PathPointTypeBezier) {
            if(!open||i+2>=path.points.size()) throw std::runtime_error("Invalid native Bezier path");
            auto b=path.points[i+1],c=path.points[i+2];
            sink->AddBezier(D2D1::BezierSegment({p.x,p.y},{b.x,b.y},{c.x,c.y}));i+=2;
        } else {
            if(!open) throw std::runtime_error("Invalid native path figure");
            sink->AddLine({p.x,p.y});
        }
        if(path.types[i]&Gdiplus::PathPointTypeCloseSubpath) { sink->EndFigure(D2D1_FIGURE_END_CLOSED);open=false; }
    }
    if(open) sink->EndFigure(D2D1_FIGURE_END_OPEN);
    requireScene(sink->Close());return result;
}
void SceneRenderer::command(const SceneCommand& value) {
    if(value.state->clip.empty()) return;
    // Optional shader registration must not prevent plain bitmap navigation
    // or ordinary vector drawing from initializing.
    const bool needsEffects=value.kind==SceneCommand::RasterOp||
        (value.kind==SceneCommand::Path&&value.brush.kind==SceneBrush::PathGradient)||!value.effects.empty();
    if(needsEffects&&!effectsRegistered) { registerGpuEffects(factory);effectsRegistered=true; }
    ComPtr<ID2D1Effect> raster;
    if(value.kind==SceneCommand::RasterOp) {
        // A GPU destination snapshot creates the ordering barrier required by
        // XOR/AND/OR. The target is never sampled while it is being written.
        requireScene(context->EndDraw());ComPtr<ID2D1Image> current;context->GetTarget(current.GetAddressOf());
        ComPtr<ID2D1Bitmap1> target;requireScene(current.As(&target));auto size=target->GetPixelSize();
        auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
        ComPtr<ID2D1Bitmap1> before;requireScene(context->CreateBitmap(size,nullptr,0,&properties,before.GetAddressOf()));
        requireScene(before->CopyFromBitmap(nullptr,target.Get(),nullptr));
        auto ink=images.at(value.image->serial).bitmap;raster=gpuRasterEffect(context,before.Get(),ink.Get(),value.rasterTruth);
        context->BeginDraw();
    }
    context->SetTransform(D2D1::Matrix3x2F::Identity());
    const bool rectangular=value.state->clip.size()==1;
    ComPtr<ID2D1PathGeometry> mask;
    if(rectangular) context->PushAxisAlignedClip(rect(value.state->clip.front()),D2D1_ANTIALIAS_MODE_ALIASED);
    else {
        // Region scans describe the native device clip as disjoint rectangles.
        // A mask draws translucent ink once even across a compound clip.
        ScenePath clip;clip.winding=true;
        clip.points.reserve(value.state->clip.size()*4);clip.types.reserve(value.state->clip.size()*4);
        for(auto r:value.state->clip) {
            clip.points.insert(clip.points.end(),{{r.x,r.y},{r.x+r.w,r.y},{r.x+r.w,r.y+r.h},{r.x,r.y+r.h}});
            clip.types.insert(clip.types.end(),{Gdiplus::PathPointTypeStart,Gdiplus::PathPointTypeLine,
                Gdiplus::PathPointTypeLine,BYTE(Gdiplus::PathPointTypeLine|Gdiplus::PathPointTypeCloseSubpath)});
        }
        mask=geometry(clip);
        auto layer=D2D1::LayerParameters1(D2D1::InfiniteRect(),mask.Get(),D2D1_ANTIALIAS_MODE_ALIASED);
        context->PushLayer(layer,nullptr);
    }
    struct ClipScope {
        ID2D1DeviceContext* context;bool rectangle;
        ~ClipScope() { context->SetTransform(D2D1::Matrix3x2F::Identity());
            if(rectangle) context->PopAxisAlignedClip();else context->PopLayer();
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER); }
    } clipScope{context,rectangular};
    auto drawingTransform=value.state->transform;
    // Shift vector coverage in device coordinates, independently of the
    // native world scale. Raster buffer copies already address physical pixels.
    if(value.kind==SceneCommand::Path) {
        drawingTransform.dx+=value.state->pixelOffset;drawingTransform.dy+=value.state->pixelOffset;
    }
    context->SetTransform(matrix(drawingTransform));
    context->SetAntialiasMode(value.state->antialias?D2D1_ANTIALIAS_MODE_PER_PRIMITIVE:D2D1_ANTIALIAS_MODE_ALIASED);
    // Preserve native source-copy restoration as well as translucent overlays.
    context->SetPrimitiveBlend(value.state->copy?D2D1_PRIMITIVE_BLEND_COPY:D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
    if(value.kind==SceneCommand::RasterOp) {
        context->SetTransform(D2D1::Matrix3x2F::Identity());
        context->DrawImage(raster.Get(),nullptr,nullptr,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,D2D1_COMPOSITE_MODE_SOURCE_COPY);
    } else if(value.kind==SceneCommand::Path) {
        auto shape=geometry(value.sharedPath?*value.sharedPath:value.path,value.sharedPath);
        if(value.brush.kind==SceneBrush::PathGradient) {
            D2D1_RECT_F bounds{};requireScene(shape->GetBounds(nullptr,&bounds));
            auto effect=gpuGradient(context,value.brush,{bounds.left,bounds.top,bounds.right-bounds.left,bounds.bottom-bounds.top});
            auto layer=D2D1::LayerParameters1(D2D1::InfiniteRect(),shape.Get(),value.state->antialias?D2D1_ANTIALIAS_MODE_PER_PRIMITIVE:D2D1_ANTIALIAS_MODE_ALIASED);
            context->PushLayer(layer,nullptr);
            D2D1_POINT_2F offset={bounds.left,bounds.top};
            context->DrawImage(effect.Get(),&offset,&bounds,D2D1_INTERPOLATION_MODE_LINEAR,
                value.state->copy?D2D1_COMPOSITE_MODE_SOURCE_COPY:D2D1_COMPOSITE_MODE_SOURCE_OVER);
            context->PopLayer();
        } else if(value.stroked) {
            auto fill=brush(value.brush);
            auto properties=D2D1::StrokeStyleProperties1(D2D1_CAP_STYLE(value.startCap),
                D2D1_CAP_STYLE(value.endCap),D2D1_CAP_STYLE(value.dashCap),D2D1_LINE_JOIN(value.lineJoin),
                value.miterLimit,D2D1_DASH_STYLE(value.dashStyle),value.dashOffset,
                value.fixedStroke?D2D1_STROKE_TRANSFORM_TYPE_FIXED:D2D1_STROKE_TRANSFORM_TYPE_NORMAL);
            ComPtr<ID2D1StrokeStyle1> style;
            requireScene(factory->CreateStrokeStyle(&properties,value.dashes.data(),UINT(value.dashes.size()),style.GetAddressOf()));
            context->DrawGeometry(shape.Get(),fill.Get(),value.strokeWidth,style.Get());
        } else { auto fill=brush(value.brush);context->FillGeometry(shape.Get(),fill.Get()); }
    } else if(value.kind==SceneCommand::Metafile) {
        auto source=metafile(value.metafile);ComPtr<ID2D1DeviceContext2> c2;requireScene(context->QueryInterface(IID_PPV_ARGS(c2.GetAddressOf())));
        D2D1_RECT_F bounds{};ComPtr<ID2D1GdiMetafile1> source1;
        if(SUCCEEDED(source.As(&source1))) requireScene(source1->GetSourceBounds(&bounds));else requireScene(source->GetBounds(&bounds));
        auto s=value.source;const float w=bounds.right-bounds.left,h=bounds.bottom-bounds.top;
        bounds={bounds.left+s.x*w,bounds.top+s.y*h,bounds.left+(s.x+s.w)*w,bounds.top+(s.y+s.h)*h};auto dest=rect(value.destination);
        c2->DrawGdiMetafile(source.Get(),&dest,&bounds);
    } else {
        if(navigationRecording&&value.image&&value.effects.empty()) {
            // Native work/page buffers retain vector commands. Replay them as
            // images rather than sampling their already-rasterized bitmaps.
            const auto& d=value.destination;const auto& s=value.source;
            if(s.w<=0||s.h<=0||d.w<=0||d.h<=0) return;
            auto mapping=D2D1::Matrix3x2F::Translation(-s.x,-s.y)*
                D2D1::Matrix3x2F::Scale(d.w/s.w,d.h/s.h)*D2D1::Matrix3x2F::Translation(d.x,d.y);
            context->SetTransform(matrix(value.state->transform));
            // Source-copy DrawImage extends transparent ink to its clip. Limit
            // it to the transfer destination, as the native DrawBitmap does.
            context->PushAxisAlignedClip(rect(d),D2D1_ANTIALIAS_MODE_ALIASED);
            context->SetTransform(mapping*D2D1::Matrix3x2F(matrix(value.state->transform)));
            const auto source=rect(s);const auto offset=D2D1::Point2F(s.x,s.y);
            context->DrawImage(navigationImages.at(value.image->serial).Get(),&offset,&source,
                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
                value.state->copy?D2D1_COMPOSITE_MODE_SOURCE_COPY:D2D1_COMPOSITE_MODE_SOURCE_OVER);
            context->PopAxisAlignedClip();return;
        }
        auto image=value.image?images.at(value.image->serial).bitmap:bitmap(value.pixels);
        context->SetTransform(matrix(value.state->transform));
        context->SetPrimitiveBlend(value.state->copy?D2D1_PRIMITIVE_BLEND_COPY:D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
        auto dest=rect(value.destination),source=rect(value.source);
        auto sampling=D2D1_INTERPOLATION_MODE_LINEAR;
        switch(value.state->interpolation) {
        case Gdiplus::InterpolationModeNearestNeighbor:sampling=D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;break;
        case Gdiplus::InterpolationModeBicubic:sampling=D2D1_INTERPOLATION_MODE_CUBIC;break;
        case Gdiplus::InterpolationModeHighQualityBicubic:sampling=D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC;break;
        case Gdiplus::InterpolationModeHighQualityBilinear:sampling=D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR;break;
        }
        const auto& m=value.state->transform;const auto& copyDest=value.destination;const auto& copySource=value.source;
        auto integral=[](float x) { return std::abs(x-std::round(x))<0.0001f; };
        // Work/page buffers are pixel transfers. Multiple filtered copies can
        // change an edge's apparent position before the final vector repaint.
        if(value.image&&m.m11==1&&m.m22==1&&m.m12==0&&m.m21==0&&
            copyDest.w==copySource.w&&copyDest.h==copySource.h&&
            integral(copyDest.x+m.dx-copySource.x)&&integral(copyDest.y+m.dy-copySource.y))
            sampling=D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR;
        if(value.effects.empty()) context->DrawBitmap(image.Get(),&dest,1.0f,sampling,&source);
        else {
            ComPtr<ID2D1Image> filtered;image.As(&filtered);std::vector<ComPtr<ID2D1Effect>> chain;
            for(const auto& effect:value.effects) { auto node=gpuImageEffect(context,filtered.Get(),*effect);node->GetOutput(filtered.ReleaseAndGetAddressOf());chain.push_back(node); }
            const auto& d=value.destination;const auto& s=value.source;
            auto mapping=D2D1::Matrix3x2F::Translation(-s.x,-s.y)*D2D1::Matrix3x2F::Scale(d.w/s.w,d.h/s.h)*D2D1::Matrix3x2F::Translation(d.x,d.y);
            context->SetTransform(mapping*D2D1::Matrix3x2F(matrix(value.state->transform)));
            D2D1_POINT_2F offset={s.x,s.y};
            context->DrawImage(filtered.Get(),&offset,&source,D2D1_INTERPOLATION_MODE_LINEAR,
                value.state->copy?D2D1_COMPOSITE_MODE_SOURCE_COPY:D2D1_COMPOSITE_MODE_SOURCE_OVER);
        }
    }
}
ComPtr<ID2D1Image> SceneRenderer::navigation(const std::shared_ptr<const GpuScene>& scene) {
    if(!scene) throw std::runtime_error("Missing navigation scene");
    if(navigationSource.lock()==scene&&retainedNavigation) return retainedNavigation;
    // Compile once per captured camera scene, on the GPU worker. A postorder
    // walk resolves native offscreen transfers without recursive C++ traversal.
    struct Work {
        std::shared_ptr<const GpuScene> scene,previous;
        size_t next{};bool entered{},raster{};unsigned depth{};
    };
    std::vector<Work> stack{{scene,{}}},ordered;
    std::unordered_map<uint64_t,unsigned char> marks;
    size_t commands{};
    while(!stack.empty()) {
        auto& work=stack.back();
        if(!work.entered) {
            marks[work.scene->serial]=1;work.entered=true;
            work.previous=work.scene->base.load(std::memory_order_acquire);
            commands+=work.scene->commands.size();
            // Destination-dependent operations need the existing bitmap
            // barrier. Keep that segment raster, preserving its exact ordering.
            work.raster=work.depth>=96||std::any_of(work.scene->commands.begin(),work.scene->commands.end(),
                [](const auto& c) { return c.kind==SceneCommand::RasterOp; });
            // Bound retained history/resource growth on pathological documents.
            if(marks.size()>4096||commands>100000) return render(scene);
        }
        bool descend{};
        while(!work.raster&&work.next<=work.scene->commands.size()) {
            auto dependency=work.next==0?work.previous:work.scene->commands[work.next-1].image;
            ++work.next;if(!dependency) continue;
            const auto mark=marks[dependency->serial];
            if(mark==1) throw std::runtime_error("Cyclic navigation scene dependency");
            if(mark==2) continue;
            const unsigned depth=work.depth+1;
            stack.push_back({std::move(dependency),{},0,false,false,depth});descend=true;break;
        }
        if(descend) continue;
        marks[work.scene->serial]=2;ordered.push_back(std::move(work));stack.pop_back();
    }
    ComPtr<ID2D1Image> saved;context->GetTarget(saved.GetAddressOf());
    D2D1_MATRIX_3X2_F savedTransform;context->GetTransform(&savedTransform);
    const auto savedBlend=context->GetPrimitiveBlend();const auto savedAA=context->GetAntialiasMode();
    bool drawing{},clipped{};
    auto restore=[&] {
        navigationRecording=false;navigationImages.clear();context->SetTarget(saved.Get());
        context->SetTransform(savedTransform);context->SetPrimitiveBlend(savedBlend);context->SetAntialiasMode(savedAA);
    };
    try {
        for(const auto& work:ordered) {
            if(work.raster) { navigationImages.emplace(work.scene->serial,render(work.scene));continue; }
            // Effects can retain a raster input while the rest of this page
            // stays vector. Resolve those inputs before opening a command list.
            for(const auto& c:work.scene->commands) if(c.image&&!c.effects.empty()) render(c.image);
            ComPtr<ID2D1CommandList> list;requireScene(context->CreateCommandList(list.GetAddressOf()));
            context->SetTarget(list.Get());context->SetTransform(D2D1::Matrix3x2F::Identity());
            context->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            context->BeginDraw();drawing=true;
            const auto bounds=work.scene->extent;
            const auto extent=bounds.w>0&&bounds.h>0?rect(bounds):
                D2D1::RectF(0,0,float(work.scene->width),float(work.scene->height));
            context->PushAxisAlignedClip(extent,D2D1_ANTIALIAS_MODE_ALIASED);clipped=true;
            if(work.previous) {
                const auto offset=D2D1::Point2F(0,0);
                context->DrawImage(navigationImages.at(work.previous->serial).Get(),&offset,&extent,
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            }
            navigationRecording=true;
            for(const auto& c:work.scene->commands) command(c);
            navigationRecording=false;
            context->SetTransform(D2D1::Matrix3x2F::Identity());context->PopAxisAlignedClip();clipped=false;
            const auto result=context->EndDraw();drawing=false;requireScene(result);
            context->SetTarget(saved.Get());
            requireScene(list->Close());navigationImages.emplace(work.scene->serial,list);
        }
        auto result=navigationImages.at(scene->serial);restore();
        navigationSource=scene;retainedNavigation=result;return result;
    } catch(...) {
        if(drawing) {
            if(clipped) { context->SetTransform(D2D1::Matrix3x2F::Identity());context->PopAxisAlignedClip(); }
            context->EndDraw();
        }
        restore();throw;
    }
}
void SceneRenderer::remember(const std::shared_ptr<const GpuScene>& scene,ComPtr<ID2D1Bitmap1> image) {
    if(!scene||!image||images.contains(scene->serial)) return;
    const auto size=image->GetPixelSize();
    if(size.width!=UINT(scene->width)||size.height!=UINT(scene->height)) return;
    const uint64_t bytes=uint64_t(size.width)*size.height*4;
    images.emplace(scene->serial,Cached{scene,std::move(image),bytes,++cacheClock});imageBytes+=bytes;
}
ComPtr<ID2D1Bitmap1> SceneRenderer::render(const std::shared_ptr<const GpuScene>& scene) {
    if(!scene) throw std::runtime_error("Missing GPU scene");
    if(auto i=images.find(scene->serial);i!=images.end()) {
        i->second.used=++cacheClock;return i->second.bitmap;
    }
    // Heap-backed postorder traversal keeps stack usage independent of history
    // depth. Resolve every dependency before drawing on its parent's target.
    struct Work {
        std::shared_ptr<const GpuScene> scene,previous;
        size_t next{};bool entered{};
    };
    std::vector<Work> stack{{scene,{}}},ordered;
    std::unordered_map<uint64_t,unsigned char> marks;
    std::unordered_map<uint64_t,size_t> consumers;
    while(!stack.empty()) {
        auto& work=stack.back();
        if(!work.entered) {
            marks[work.scene->serial]=1;work.entered=true;
            work.previous=work.scene->base.load(std::memory_order_acquire);
        }
        bool descend{};
        while(work.next<=work.scene->commands.size()) {
            auto dependency=work.next==0?work.previous:work.scene->commands[work.next-1].image;
            ++work.next;if(!dependency) continue;
            ++consumers[dependency->serial];
            if(auto cached=images.find(dependency->serial);cached!=images.end()) {
                cached->second.used=++cacheClock;continue;
            }
            const auto mark=marks[dependency->serial];
            if(mark==1) throw std::runtime_error("Cyclic GPU scene dependency");
            if(mark==2) continue;
            stack.push_back({std::move(dependency),{}});descend=true;break;
        }
        if(descend) continue;
        marks[work.scene->serial]=2;ordered.push_back(std::move(work));stack.pop_back();
    }
    trimImages(consumers,scene->serial);
    for(const auto& work:ordered) {
        renderOne(work.scene,work.previous);
        auto consumed=[&](const std::shared_ptr<const GpuScene>& dependency) {
            if(dependency) --consumers.at(dependency->serial);
        };
        consumed(work.previous);
        for(const auto& command:work.scene->commands) consumed(command.image);
        trimImages(consumers,scene->serial);
    }
    return images.at(scene->serial).bitmap;
}
void SceneRenderer::renderOne(const std::shared_ptr<const GpuScene>& scene,const std::shared_ptr<const GpuScene>& previous) {
    ComPtr<ID2D1Image> saved;context->GetTarget(saved.GetAddressOf());
    D2D1_MATRIX_3X2_F savedTransform;context->GetTransform(&savedTransform);
    const auto savedBlend=context->GetPrimitiveBlend();
    ComPtr<ID2D1Bitmap1> output;
    auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
    requireScene(context->CreateBitmap(D2D1::SizeU(scene->width,scene->height),nullptr,0,&properties,output.GetAddressOf()));
    context->SetTarget(output.Get());context->SetTransform(D2D1::Matrix3x2F::Identity());
    context->BeginDraw();context->Clear(D2D1::ColorF(0,0.0f));
    if(previous) {
        auto image=images.at(previous->serial).bitmap;
        context->DrawBitmap(image.Get(),nullptr,1.0f,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    }
    try { for(const auto& c:scene->commands) command(c); }
    catch(...) {
        context->EndDraw();context->SetTarget(saved.Get());context->SetTransform(savedTransform);context->SetPrimitiveBlend(savedBlend);throw;
    }
    const auto result=context->EndDraw();
    context->SetTarget(saved.Get());context->SetTransform(savedTransform);context->SetPrimitiveBlend(savedBlend);
    requireScene(result);
    const auto bytes=uint64_t(scene->width)*uint64_t(scene->height)*4;
    images.emplace(scene->serial,Cached{scene,output,bytes,++cacheClock});imageBytes+=bytes;
    // Native pixel consumers can reconstruct this immutable version on their
    // own device. Releasing its base here would lose earlier drawing there.
}
void SceneRenderer::trimImages(const std::unordered_map<uint64_t,size_t>& consumers,uint64_t root) {
    constexpr uint64_t budget=128ULL*1024*1024;
    while(imageBytes>budget||images.size()>64) {
        auto oldest=images.end();
        for(auto i=images.begin();i!=images.end();++i) {
            if(i->first==root) continue;
            if(auto use=consumers.find(i->first);use!=consumers.end()&&use->second) continue;
            if(oldest==images.end()||i->second.used<oldest->second.used) oldest=i;
        }
        // Required parent inputs remain pinned until that parent is complete.
        if(oldest==images.end()) break;
        imageBytes-=oldest->second.bytes;images.erase(oldest);
    }
}
void SceneRenderer::prune() {
    for(auto i=images.begin();i!=images.end();) {
        if(i->second.scene.expired()) { imageBytes-=i->second.bytes;i=images.erase(i); }else ++i;
    }
    trimImages({},0);
    for(auto i=textures.begin();i!=textures.end();) {
        if(i->second.pixels.expired()) i=textures.erase(i);else ++i;
    }
    for(auto i=metafiles.begin();i!=metafiles.end();) {
        if(i->second.source.expired()) i=metafiles.erase(i);else ++i;
    }
}
}

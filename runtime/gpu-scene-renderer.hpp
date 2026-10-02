#pragma once
#include "gpu-scene.hpp"
#include <d2d1_1.h>
#include <wrl/client.h>
#include <list>

namespace cd {
class SceneRenderer {
    struct Cached {
        std::weak_ptr<const GpuScene> scene;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
        uint64_t bytes{},used{};
    };
    std::unordered_map<uint64_t,Cached> images;
    uint64_t imageBytes{},cacheClock{};
    // Populated only while compiling a navigation frame. The returned command
    // list retains its dependencies; ordinary page/placement caches stay raster.
    std::unordered_map<uint64_t,Microsoft::WRL::ComPtr<ID2D1Image>> navigationImages;
    bool navigationRecording{};
    std::weak_ptr<const GpuScene> navigationSource;
    Microsoft::WRL::ComPtr<ID2D1Image> retainedNavigation;
    struct CachedPixels {
        std::weak_ptr<const ScenePixels> pixels;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    };
    std::unordered_map<const ScenePixels*,CachedPixels> textures;
    struct CachedMetafile {
        std::weak_ptr<const SceneMetafile> source;
        Microsoft::WRL::ComPtr<ID2D1GdiMetafile> metafile;
    };
    std::unordered_map<const SceneMetafile*,CachedMetafile> metafiles;
    struct CachedGeometry {
        uint64_t key{},bytes{};ScenePath path;
        Microsoft::WRL::ComPtr<ID2D1PathGeometry> shape;
        std::weak_ptr<const ScenePath> owner;const ScenePath* ownerKey{};
    };
    std::list<CachedGeometry> shapes;
    std::unordered_multimap<uint64_t,std::list<CachedGeometry>::iterator> shapeKeys;
    std::unordered_map<const ScenePath*,std::list<CachedGeometry>::iterator> sharedShapes;
    uint64_t shapeBytes{};
    ID2D1DeviceContext* context{};ID2D1Factory1* factory{};bool effectsRegistered{};
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> solidBrush;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap(const std::shared_ptr<const ScenePixels>&);
    Microsoft::WRL::ComPtr<ID2D1Brush> brush(const SceneBrush&);
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry(const ScenePath&,const std::shared_ptr<const ScenePath>& shared={});
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> buildGeometry(const ScenePath&);
    void command(const SceneCommand&);
    void renderOne(const std::shared_ptr<const GpuScene>&,const std::shared_ptr<const GpuScene>&);
    void trimImages(const std::unordered_map<uint64_t,size_t>&,uint64_t);
    Microsoft::WRL::ComPtr<ID2D1GdiMetafile> metafile(const std::shared_ptr<const SceneMetafile>&);
public:
    void feedbackPath(const ScenePath&,ID2D1Brush*,float width,bool filled);
    void initialize(ID2D1DeviceContext*,ID2D1Factory1*);
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> render(const std::shared_ptr<const GpuScene>&);
    Microsoft::WRL::ComPtr<ID2D1Image> navigation(const std::shared_ptr<const GpuScene>&);
    void remember(const std::shared_ptr<const GpuScene>&,Microsoft::WRL::ComPtr<ID2D1Bitmap1>);
    void prune();
};
}

#pragma once
#include "gpu-scene.hpp"
#include <d2d1_1.h>
#include <wrl/client.h>
namespace cd {
void registerGpuEffects(ID2D1Factory1*);
Microsoft::WRL::ComPtr<ID2D1Effect> gpuGradient(ID2D1DeviceContext*,const SceneBrush&,SceneRect);
Microsoft::WRL::ComPtr<ID2D1Effect> gpuImageEffect(ID2D1DeviceContext*,ID2D1Image*,const SceneEffect&);
Microsoft::WRL::ComPtr<ID2D1Effect> gpuRasterEffect(ID2D1DeviceContext*,ID2D1Image*,ID2D1Image*,uint32_t);
}

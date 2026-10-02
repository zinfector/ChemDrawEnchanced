#pragma once
#include "runtime.hpp"
#include "gpu-preview.hpp"
namespace cd {
void installSmartAlignment();
void removeSmartAlignment();
void beginSmartAlignment(Obj tracker,Obj doc,bool gpu) noexcept;
void warmSmartAlignmentPage(Obj doc) noexcept;
void smartAlignmentGeometryInvalidated(Obj object) noexcept;
void endSmartAlignment() noexcept;
bool cancelSmartAlignment(HWND) noexcept;
void smartAlignmentReleased(HWND,const MSG&) noexcept;
void smartAlignmentPageGone(Obj) noexcept;
void smartAlignmentObjectChanged(Obj) noexcept;
std::shared_ptr<const AlignmentFeedback> smartAlignmentFeedback(Obj tracker) noexcept;
void clearGpuAlignment(Obj doc) noexcept;
}

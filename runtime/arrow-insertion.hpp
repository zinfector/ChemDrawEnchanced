#pragma once
#include "gpu-scene.hpp"
namespace cd {
struct ArrowInsertionTarget {Obj object{};RectD bounds{};};
void writeArrowInsertionStatus(const char*);
void beginArrowInsertionTrace(Obj tracker,bool gpu) noexcept;
void arrowInsertionStage(const char*) noexcept;
void endArrowInsertionTrace() noexcept;
void installArrowInsertion();
void beginArrowInsertion(Obj tracker,Obj doc,Obj molecule,const RectD&,
    const std::vector<ArrowInsertionTarget>&) noexcept;
bool constrainArrowInsertion(Obj tracker,Point&,double unitsPerDip) noexcept;
void prepareArrowInsertionFinish(Obj tracker,bool samePage,bool commit) noexcept;
void finishArrowInsertion(Obj tracker) noexcept;
void cancelArrowInsertion() noexcept;
void arrowInsertionReleased(HWND) noexcept;
void arrowInsertionObjectChanged(Obj) noexcept;
std::shared_ptr<const GpuScene> arrowInsertionPreview(const std::shared_ptr<const GpuScene>&) noexcept;
bool moleculeArrowAnchor(Obj,Point&) noexcept;
bool arrowInsertionAttachment(Obj arrow,bool tail,Point&) noexcept;
}

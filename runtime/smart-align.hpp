#pragma once
#include "runtime.hpp"
#include "gpu-preview.hpp"
namespace cd {
void installSmartAlignment();
void removeSmartAlignment();
void beginSmartAlignment(Obj tracker,Obj doc,bool gpu) noexcept;
void warmSmartAlignmentPage(Obj doc) noexcept;
void smartAlignmentDrawingMove(Obj tracker) noexcept;
bool smartAlignmentDrawingArrow(Obj arrow,const Point& head,const Point& tail,Point& snappedHead,Point& snappedTail) noexcept;
void smartAlignmentGhost(Obj page,Obj target,const GhostTool&,GhostBond&,double unitsPerDip) noexcept;
void beginSmartGhostPlacement(Obj page,const GhostBond&,POINT pressed) noexcept;
void endSmartGhostPlacement() noexcept;
bool smartAlignmentGhostArrowEndpoints(Obj page,Point& tail,Point& head) noexcept;
bool smartAlignmentPlacementPoint(Obj page,const Point&,Point&) noexcept;
void smartAlignmentGeometryInvalidated(Obj object) noexcept;
void endSmartAlignment() noexcept;
bool cancelSmartAlignment(HWND) noexcept;
void smartAlignmentReleased(HWND,const MSG&) noexcept;
void smartAlignmentPageGone(Obj) noexcept;
void smartAlignmentObjectChanged(Obj) noexcept;
std::shared_ptr<const AlignmentFeedback> smartAlignmentFeedback(Obj tracker) noexcept;
void clearGpuAlignment(Obj doc) noexcept;
}

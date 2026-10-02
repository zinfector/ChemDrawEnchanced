#pragma once
#include "runtime.hpp"

namespace cd {
enum class PlacementPhase { Pressed,Released,GeometryCommitted,ChemistrySettled,Cancelled };
enum class PlacementCost : size_t { Input,Tracking,Validation,Analyzer,Stereo,Drawing,Chemistry,Publication,Preflight,NativeIdle,PixelReadback,Count };
struct PlacementDisplayTiming;
class PlacementTimer {
    PlacementCost cost;uint64_t revision{},started{};HWND window{};
public:
    explicit PlacementTimer(PlacementCost) noexcept;
    ~PlacementTimer();
};
class PlacementInputScope {
    HWND previous{};bool entered{};
public:
    PlacementInputScope(HWND,Obj,UINT);
    ~PlacementInputScope();
};
class PlacementIdleScope {
    Obj previous{};bool previousYield{},previousSynchronous{};
public:
    explicit PlacementIdleScope(Obj,bool allowScheduling=true,bool synchronous=false) noexcept;
    ~PlacementIdleScope();
};
void installPlacementRuntime();
uint64_t placementRevision(HWND) noexcept;
void placementReleased(HWND) noexcept;
void placementGeometryCommitted(HWND) noexcept;
void placementDisplayPresented(HWND,const PlacementDisplayTiming&) noexcept;
void placementChemistrySettled(HWND) noexcept;
void cancelPlacement(HWND) noexcept;
void forgetPlacementPage(Obj) noexcept;
void flushPlacementDamage();
bool placementGeometryPending(Obj) noexcept;
bool placementChemistryPending(Obj) noexcept;
}

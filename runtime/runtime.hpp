#pragma once
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include "MinHook.h"
#include "version-resolver.hpp"
#include "patch-options.hpp"

namespace cd {
using Obj = void*;
struct Point { double x, y, z; };
// ChemDraw rectangles are top, left, bottom, right (not Win32 RECT order).
struct RectI { int t, l, b, r; };
struct RectD { double t, l, b, r; };
struct Vec { std::byte* first; std::byte* last; std::byte* limit; };
extern uintptr_t base;
extern DWORD uiThread;
extern HMODULE module;
extern std::atomic<uint64_t> generation;
extern thread_local unsigned drawingDepth;
extern thread_local Obj hoverDocument;
extern thread_local unsigned trackingDepth;
extern thread_local unsigned zoomDispatch;

template<class T> T& at(Obj p, size_t offset) {
    return *reinterpret_cast<T*>(static_cast<std::byte*>(p) + offset);
}
template<class F> F fn(size_t rva) { return reinterpret_cast<F>(base + rva); }
template<class F> F vf(Obj p, size_t slot) {
    return reinterpret_cast<F>(at<uintptr_t*>(p, 0)[slot / sizeof(void*)]);
}
template<class F> void hook(size_t rva, F replacement, F& original) {
    const auto status = MH_CreateHook(reinterpret_cast<void*>(resolveDetour(L"ChemDrawBase.dll",uint32_t(rva))),
        reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(&original));
    if (status != MH_OK) {
        char message[128]{};
        sprintf_s(message,"Hook +0x%llx: %s",static_cast<unsigned long long>(rva),MH_StatusToString(status));
        throw std::runtime_error(message);
    }
}
inline bool onUI() { return GetCurrentThreadId() == uiThread; }
inline Obj mainPage(Obj doc) {
    const int n = at<int>(doc, 0x2f8);
    const auto first = at<Obj*>(doc, 0x188), last = at<Obj*>(doc, 0x190);
    return n >= 0 && first && size_t(n) < size_t(last - first) ? first[n] : nullptr;
}
inline HWND portWindow(Obj port) { return at<HWND>(port, 0xb8); }
inline bool valid(RectI a) { return a.r > a.l && a.b > a.t; }
inline bool valid(RectD a) {
    return std::isfinite(a.t) && std::isfinite(a.l) && std::isfinite(a.b) &&
        std::isfinite(a.r) && a.r >= a.l && a.b >= a.t;
}
inline RectI intersect(RectI a, RectI b) {
    return {std::max(a.t,b.t),std::max(a.l,b.l),std::min(a.b,b.b),std::min(a.r,b.r)};
}
inline RectI unite(RectI a, RectI b) {
    if (!valid(a)) return b;
    if (!valid(b)) return a;
    return {std::min(a.t,b.t),std::min(a.l,b.l),std::max(a.b,b.b),std::max(a.r,b.r)};
}
inline RECT winRect(RectI r) { return {r.l,r.t,r.r,r.b}; }
inline RectI cdRect(RECT r) { return {r.top,r.left,r.bottom,r.right}; }
inline RectI clientRect(HWND w) { RECT r{}; GetClientRect(w,&r); return cdRect(r); }
inline bool same(RectI a, RectI b) { return !memcmp(&a,&b,sizeof(a)); }
inline RectI drawingRect(Obj doc) {
    RectI r{}; vf<RectI*(*)(Obj,RectI*)>(doc,0x68)(doc,&r); return r;
}

void installPresentation();
void installUndoCamera();
void installNavigationUndo();
void prepareUndoCamera(Obj);
void queueUndoCamera(Obj,Obj,const RectD&,bool reveal=true);
void installBondPlacement();
Point* previewBondPlacement(Point*,const Point*,char*,Obj);
bool ordinaryBondTool();
uint64_t bondToolKey();
void clearBondPlacement(HWND);
Obj mouseBondPlacementVertex();
bool displayedBondPlacement(HWND,uint64_t,Point&,Point&);
bool gpuOwnsPlacementHighlight(Obj);
bool gpuOwnsHoverHighlight(Obj,Obj);
void updateGpuPlacementEndpoint(Obj);
void updateGpuGhostGesture(Obj);
class MouseBondPlacementScope {
    HWND previousWindow{};Obj previousAtom{};uint64_t previousId{},previousToolKey{};
    Point previousStart{},previousDirection{};bool previousFrozen{},entered{};
public:
    MouseBondPlacementScope(HWND,UINT,LPARAM);
    ~MouseBondPlacementScope();
    MouseBondPlacementScope(const MouseBondPlacementScope&)=delete;
    MouseBondPlacementScope& operator=(const MouseBondPlacementScope&)=delete;
};
void installSpatial();
bool vertexHitCircle(Obj,RectD&);
void installBuffers();
void forgetPage(Obj);
void clearHighlights();
void forgetPresentation(HWND);
void bumpGeneration();
void writeStatus(const char*);
void writeGpuStatus(const char*);
void writeGpuUiStatus(const char*);
void writeToolbarStatus(const char*);
void writeHistoryStatus(const char*);
void installHistoryTrace();
void traceHistoryButton(HWND,UINT,WPARAM,LPARAM,bool);
void traceHistoryToolbarCommand(HWND,WPARAM,LPARAM,bool);
void traceHistoryOperation(Obj,bool,bool);
void traceHistoryPublication(Obj,bool,bool);
void installToolbarPaint();
DWORD trackingPresentationDelay();
void flushTrackingPresentation(bool force=false);
void holdTrackingPresentation();
void releaseTrackingGhost();
void startGpuGhostRelease(Obj);
void holdGpuDrawing(Obj);
void beginGpuDrawing(Obj);
void queueGpuDrawingCommit(Obj);
}

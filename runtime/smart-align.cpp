#include "smart-align.hpp"
#include "smart-align-math.hpp"
#include "arrow-path.hpp"
#include "curved-arrow.hpp"
#include "arrow-insertion.hpp"
#include "startup.hpp"
#include <unordered_set>
#include <numeric>

namespace cd {
namespace {
using Box=alignMath::Box;
using Relation=alignMath::Relation;
using Kind=alignMath::Kind;
using Constrain=Point*(*)(Obj,Point*);
using Preview=void(*)(Obj);
using Finish=void(*)(Obj,bool,bool);
using Snap=Point*(*)(Obj,Point*,const Point*);
Constrain oldConstrain{};Preview oldPreview{};Finish oldFinish{};
Snap oldArrowSnap{},oldGraphicSnap{};
using SetPoint=void(*)(Obj,const Point*);
struct PortPoint {int x,y,z;};
using SetPortPoint=void(*)(Obj,const PortPoint*);
using CalcRing=void(*)(Obj,bool);
SetPoint oldSetPoint{},oldSetAtomPos{};
SetPortPoint oldSetPortPoint{};
using ConvertPoint=Obj(*)(Obj,const PortPoint*,Point*,bool);
ConvertPoint oldConvertPoint{};
using RingFinish=short(*)(Obj);
RingFinish oldRingFinish{};
CalcRing oldCalcRing{};
Preview oldRingMove{};
Preview oldGraphicMove{},oldBondMove{},oldGenericMove{},oldChainMove{};
enum class Drawing {None,Graphic,Arrow,ElectronArrow,Bond,Ring,Generic,Chain};
bool installed{},enabled=true;
HHOOK keyboardHook{};
struct Node {Node *left,*parent,*right;uint8_t color,nil,padding[6];Obj object;};
struct Reference {Obj object{};Box bounds{};};
struct ArrowRay {Obj object{},root{};alignMath::Ray ray{};};
struct Gesture {
    Obj tracker{},page{},doc{};HWND window{};
    bool eligible{},moving{},finishing{},cancelled{},released{},nativeX{},nativeY{},inOriginal{},resolved{};
    Point down{},previousEffective{},effective{};
    Box initial{};
    Drawing drawing{Drawing::None};
    Box drawingBounds{};
    Point ringShift{};
    bool settingPoint{},ringActive{};
    Relation x{},y{};
    bool molecule{};
    alignMath::RayRelation ray{};
    std::vector<alignMath::Ray> rays;
    std::vector<Reference> references;
    std::vector<Box> bounds;
    std::vector<int> orderX,orderY;
    std::unordered_set<Obj> movingObjects;
};
thread_local Gesture gesture;
struct CachedMember {
    Obj object{};bool leaf{};
    std::vector<Obj> ancestors;
};
struct CachedRoot {Reference reference;std::vector<CachedMember> members;bool molecule{};};
struct PageAlignmentIndex {
    Node* head{};size_t count{};uint64_t revision{};bool ready{};
    std::vector<CachedRoot> roots;
    std::unordered_map<Obj,size_t> rootForObject;
    std::vector<int> drawOrder,orderX,orderY;
    std::vector<ArrowRay> rays;
};
thread_local std::unordered_map<Obj,PageAlignmentIndex> pageIndices;
thread_local bool warmingIndex{};
struct GhostSnap {Obj page{},target{};uint64_t tool{},revision{};Relation x{},y{};alignMath::RayRelation ray{};};
thread_local GhostSnap ghostSnap;
struct GhostPlacement {Obj page{};Point press{},offset{},tail{},head{};POINT cursor{};HWND window{};int tool{};bool arrow{},free{},dragged{};};
thread_local GhostPlacement ghostPlacement;
using DropFirst=void(*)(Obj,Obj,const Point*,Obj*,Obj*,char*,char*,bool);
using DropRing=short(*)(Obj,Point,bool,bool,bool);
DropFirst oldDropFirst{};DropRing oldDropRing{};
struct UndoOff {
    alignas(8) std::byte storage[16]{};
    explicit UndoOff(Obj doc) {fn<void(*)(void*,Obj)>(0x501fd0)(storage,doc);}
    ~UndoOff() {fn<void(*)(void*)>(0x5023f0)(storage);}
};
template<class F> void each(Node* head,F action) {
    if(!head)return;
    for(Node* n=head->left;n!=head;) {
        action(n->object);
        if(!n->right->nil) {n=n->right;while(!n->left->nil)n=n->left;}
        else {Node* p=n->parent;while(p!=head&&n==p->right){n=p;p=p->parent;}n=p;}
    }
}

bool nativeTracker(Obj tracker) {
    if(!tracker)return false;
    const uintptr_t table=at<uintptr_t>(tracker,0);
    const auto ui=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ChemDrawUI.dll"));
    const auto callback=reinterpret_cast<uintptr_t>(vf<Preview>(tracker,0x38));
    return table==base+0x8a3820||(ui&&table==ui+0x428cb8)||
        callback==base+0x16dcd0||(ui&&callback==ui+0x379174);
}
Drawing drawingTracker(Obj tracker) {
    const auto ui=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ChemDrawUI.dll"));
    if(!ui||!tracker)return Drawing::None;
    const uintptr_t table=at<uintptr_t>(tracker,0)-ui;
    switch(table) {
    case 0x428b98:return Drawing::Graphic;
    case 0x427dd8:return Drawing::Arrow;
    case 0x427e70:return Drawing::ElectronArrow;
    case 0x4285d0:case 0x428488:return Drawing::Bond;
    case 0x429780:return Drawing::Ring;
    case 0x4278f0:return Drawing::Generic;
    case 0x428998:return Drawing::Chain;
    default:
        // Derived trackers share the native drawing callback even when their
        // vtable differs from the palette's ordinary tracker.
        switch(reinterpret_cast<uintptr_t>(vf<Preview>(tracker,0x38))-ui) {
        case 0x275360:return Drawing::Graphic;
        case 0x25cd40:return Drawing::Arrow;
        case 0x25d3d0:return Drawing::ElectronArrow;
        case 0x268290:return Drawing::Bond;
        case 0x287ea0:return Drawing::Ring;
        case 0x250730:return Drawing::Generic;
        case 0x26dab0:return Drawing::Chain;
        default:return Drawing::None;
        }
    }
}
Box inkBounds(Obj o) {
    RectD r{};vf<RectD*(*)(Obj,RectD*)>(o,0x1d8)(o,&r);
    if(at<uintptr_t>(o,0)==base+0x8b2cf0&&std::abs(at<double>(o,0x130))<1e-9) {
        Point tail{},head{};
        if(arrowWorldEndpoints(o,tail,head)) {
            // Arrowhead ink can be asymmetric about the shaft. Align the
            // actual shaft coordinates rather than the padded ink midpoint.
            const double dx=head.x-tail.x,dy=head.y-tail.y;
            if(std::abs(dy)<=std::abs(dx)*1e-9)r.t=r.b=(tail.y+head.y)*.5;
            if(std::abs(dx)<=std::abs(dy)*1e-9)r.l=r.r=(tail.x+head.x)*.5;
        }
    }
    return {r.l,r.t,r.r,r.b};
}
Obj rootOf(Obj o,Obj page) {
    for(unsigned n=0;n<32;++n) {
        Obj parent=at<Obj>(o,0x28);
        if(!parent||parent==o||at<Obj>(parent,0x60)!=page)return o;
        o=parent;
    }
    return nullptr;
}
void clearRelations() {gesture.x={};gesture.y={};gesture.ray={};clearGpuAlignment(gesture.doc);}
double drawingUnit() {
    const Obj scale=at<Obj>(gesture.page,0x2a8);
    UINT dpi=GetDpiForWindow(gesture.window);if(!dpi)dpi=96;
    return scale?at<double>(scale,8)*double(dpi)/96:0;
}
Box endpointBounds(const Point& fixed,const Point& end) {
    return {std::min(fixed.x,end.x),std::min(fixed.y,end.y),std::max(fixed.x,end.x),std::max(fixed.y,end.y)};
}
Point fixedDrawingPoint() {
    switch(gesture.drawing) {
    case Drawing::Arrow:return at<Point>(gesture.tracker,0x148);
    case Drawing::ElectronArrow: {
        Point tail{},head{};
        if(arrowWorldEndpoints(at<Obj>(gesture.tracker,0x138),tail,head))return tail;
        return gesture.down;
    }
    case Drawing::Bond: {
        Obj source=at<Obj>(gesture.tracker,0x160);
        return source?at<Point>(source,0x1f8):gesture.down;
    }
    case Drawing::Chain:return at<Point>(gesture.tracker,0x120);
    default:return gesture.down;
    }
}
bool ghostPlacementHeld(Obj page) {
    if(!onUI()||!enabled||ghostPlacement.page!=page||ghostPlacement.dragged)return false;
    POINT cursor{};if(!GetCursorPos(&cursor))return false;
    UINT dpi=GetDpiForWindow(ghostPlacement.window);if(!dpi)dpi=96;
    if(((GetAsyncKeyState(VK_MENU)|GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_CONTROL))&0x8000)||
        fn<int(*)()>(0x4f4ba0)()!=ghostPlacement.tool||
        std::abs(cursor.x-ghostPlacement.cursor.x)>std::max(1,GetSystemMetricsForDpi(SM_CXDRAG,dpi))||
        std::abs(cursor.y-ghostPlacement.cursor.y)>std::max(1,GetSystemMetricsForDpi(SM_CYDRAG,dpi))) {
        ghostPlacement.dragged=true;return false;
    }
    return true;
}
Point ghostPlacementCenter() {
    return {ghostPlacement.press.x+ghostPlacement.offset.x,
        ghostPlacement.press.y+ghostPlacement.offset.y,ghostPlacement.press.z};
}
Obj convertPlacementPoint(Obj page,const PortPoint* cursor,Point* result,bool cells) {
    Obj actual=oldConvertPoint(page,cursor,result,cells);
    if(actual==page&&result&&ghostPlacement.free&&ghostPlacementHeld(page))
        *result=ghostPlacementCenter();
    return actual;
}
Point drawingPoint(Point point) {
    if(!gesture.eligible||!enabled||gesture.cancelled||gesture.drawing==Drawing::None||
        !std::isfinite(point.x+point.y))return point;
    const Point fixed=fixedDrawingPoint();
    gesture.drawingBounds=endpointBounds(fixed,point);
    const double unit=drawingUnit();
    if(!std::isfinite(unit)||unit<=0)return point;
    if(std::max(gesture.drawingBounds.r-gesture.drawingBounds.l,
        gesture.drawingBounds.b-gesture.drawingBounds.t)<2*unit){gesture.x={};gesture.y={};return point;}
    if((GetAsyncKeyState(VK_MENU)|GetAsyncKeyState(VK_SHIFT))&0x8000) {gesture.x={};gesture.y={};return point;}
    // An actual chemistry attachment takes precedence over page alignment.
    if(gesture.drawing==Drawing::Bond||gesture.drawing==Drawing::Arrow||gesture.drawing==Drawing::ElectronArrow) {
        Obj ignored=gesture.drawing==Drawing::Bond?at<Obj>(gesture.tracker,0x178):nullptr;
        Obj hit=fn<Obj(*)(Obj,const Point*,Obj)>(0x7107c0)(gesture.page,&point,ignored);
        if(hit&&!gesture.movingObjects.contains(hit)) {gesture.x={};gesture.y={};return point;}
        if(gesture.drawing==Drawing::Arrow||gesture.drawing==Drawing::ElectronArrow) {
            Obj arrow=at<Obj>(gesture.tracker,0x138);
            const bool tail=gesture.drawing==Drawing::Arrow&&at<uint8_t>(gesture.tracker,0x140);
            if(arrow&&at<int>(arrow,tail?0x1f0:0x1f4)>=0){gesture.x={};gesture.y={};return point;}
        }
    }
    const bool arrow=gesture.drawing==Drawing::Arrow||gesture.drawing==Drawing::ElectronArrow;
    const Box box=arrow?endpointBounds(point,point):gesture.drawingBounds;
    const double infinity=std::numeric_limits<double>::infinity();
    // A drawing resizes about its press point: the opposite edge is fixed,
    // while its center moves by half of the endpoint's displacement.
    gesture.x=alignMath::axis(box,gesture.bounds,gesture.orderX,false,unit,gesture.x,-infinity,infinity,
        point.x,arrow?1:(point.x<fixed.x?1:0),arrow?1:(point.x>=fixed.x?1:0));
    gesture.y=alignMath::axis(box,gesture.bounds,gesture.orderY,true,unit,gesture.y,-infinity,infinity,
        point.y,arrow?1:(point.y<fixed.y?1:0),arrow?1:(point.y>=fixed.y?1:0));
    Point corrected{point.x+gesture.x.delta,point.y+gesture.y.delta,point.z};
    Box final=arrow?endpointBounds(corrected,corrected):endpointBounds(fixed,corrected);
    if(!alignMath::fitsLane(gesture.x,final,gesture.bounds,false,unit))gesture.x={};
    final=arrow?endpointBounds(corrected,corrected):endpointBounds(fixed,{point.x+gesture.x.delta,point.y+gesture.y.delta,point.z});
    if(!alignMath::fitsLane(gesture.y,final,gesture.bounds,true,unit))gesture.y={};
    corrected={point.x+gesture.x.delta,point.y+gesture.y.delta,point.z};
    gesture.drawingBounds=arrow?endpointBounds(corrected,corrected):endpointBounds(fixed,corrected);gesture.effective=corrected;
    return corrected;
}
bool chooseRay(Gesture& state,Box box,double unit,Point& shift) {
    const auto prior=state.ray;
    auto ray=alignMath::rayAxis(box,state.rays,unit,prior);
    state.ray={};
    if(ray.index<0)return false;
    const auto& direction=state.rays[ray.index];
    const bool horizontal=std::abs(direction.dx)>=std::abs(direction.dy);
    const Relation parallel=horizontal?state.x:state.y;
    const Relation normal=horizontal?state.y:state.x;
    Point correction{ray.dx,ray.dy,0};
    const double cx=(box.l+box.r)*.5,cy=(box.t+box.b)*.5;
    if(parallel.kind!=Kind::None) {
        // Intersect the arrow extension with the chosen spacing/edge guide.
        // Projection alone discards spacing even for a microscopic shaft slope.
        if(horizontal) {
            correction.x=parallel.delta;
            correction.y=direction.y+(cx+correction.x-direction.x)*direction.dy/direction.dx-cy;
        }else {
            correction.y=parallel.delta;
            correction.x=direction.x+(cy+correction.y-direction.y)*direction.dx/direction.dy-cx;
        }
        const double across=horizontal?correction.y:correction.x;
        const double limit=prior.index==ray.index?10*unit:6*unit;
        if(!std::isfinite(across)||std::abs(across)>limit)return false;
        const double along=(cx+correction.x-direction.x)*direction.dx+(cy+correction.y-direction.y)*direction.dy;
        const double radius=(box.r-box.l)*.5*std::abs(direction.dx)+(box.b-box.t)*.5*std::abs(direction.dy);
        if(along<radius+4*unit)return false;
        const double margin=(prior.index==ray.index?2:0)*unit+unit*1e-6;
        if(normal.kind!=Kind::None&&std::abs(across)>std::abs(normal.delta)+margin)return false;
        ray.along=along;
        if(horizontal)state.y={};else state.x={};
    }else {
        const bool axes=state.x.kind!=Kind::None||state.y.kind!=Kind::None;
        const double margin=(prior.index==ray.index?2:0)*unit+unit*1e-6;
        if(axes&&std::hypot(ray.dx,ray.dy)>std::hypot(shift.x,shift.y)+margin)return false;
        state.x={};state.y={};
    }
    state.ray=ray;shift=correction;return true;
}

void alignDrawingTrackerPoint(Obj tracker) {
    if(!onUI()||gesture.tracker!=tracker||gesture.settingPoint||gesture.drawing==Drawing::None||
        gesture.drawing==Drawing::Ring||gesture.drawing==Drawing::Bond)return;
    gesture.settingPoint=true;
    try {const Point corrected=drawingPoint(at<Point>(tracker,0x50));oldSetPoint(tracker,&corrected);}
    catch(...){gesture.settingPoint=false;throw;}
    gesture.settingPoint=false;
}
void setDrawingPoint(Obj tracker,const Point* point) {
    oldSetPoint(tracker,point);
    alignDrawingTrackerPoint(tracker);
}
void setDrawingPortPoint(Obj tracker,const PortPoint* point) {
    oldSetPortPoint(tracker,point);
    alignDrawingTrackerPoint(tracker);
}
void setDrawingAtom(Obj atom,const Point* point) {
    if(!onUI()||gesture.drawing!=Drawing::Bond||!gesture.eligible||
        atom!=at<Obj>(gesture.tracker,0x178)||
        (at<Obj>(gesture.tracker,0x170)&&at<Obj>(gesture.tracker,0x170)!=atom)) {oldSetAtomPos(atom,point);return;}
    const Point corrected=drawingPoint(*point);
    at<Point>(gesture.tracker,0x50)=corrected;
    oldSetAtomPos(atom,&corrected);
}
void restoreRingShift() {
    if(gesture.drawing!=Drawing::Ring||(!gesture.ringShift.x&&!gesture.ringShift.y))return;
    const Point shift=gesture.ringShift;gesture.ringShift={};
    auto& vector=at<Vec>(gesture.tracker,0x138);
    for(auto p=reinterpret_cast<Point*>(vector.first);p!=reinterpret_cast<Point*>(vector.last);++p) {
        p->x-=shift.x;p->y-=shift.y;
    }
    Obj source=at<Obj>(gesture.tracker,0x110);
    Point point=at<Point>(source,0x1f8);point.x-=shift.x;point.y-=shift.y;
    UndoOff off(gesture.doc);oldSetAtomPos(source,&point);
}
void calcDrawingRing(Obj vector,bool alternate) {
    oldCalcRing(vector,alternate);
    if(!onUI()||gesture.drawing!=Drawing::Ring||!gesture.ringActive||!gesture.eligible||
        vector!=static_cast<std::byte*>(gesture.tracker)+0x138)return;
    auto& points=at<Vec>(vector,0);
    const size_t count=size_t(points.last-points.first)/sizeof(Point);
    if(count<3||count>256)return;
    const auto first=reinterpret_cast<Point*>(points.first),last=reinterpret_cast<Point*>(points.last);
    Box box{first->x,first->y,first->x,first->y};
    for(auto p=first;p!=last;++p)box={std::min(box.l,p->x),std::min(box.t,p->y),std::max(box.r,p->x),std::max(box.b,p->y)};
    gesture.drawingBounds=box;
    const double unit=drawingUnit();
    if(!box.valid()||!std::isfinite(unit)||unit<=0)return;
    if((GetAsyncKeyState(VK_MENU)|GetAsyncKeyState(VK_SHIFT))&0x8000){gesture.x={};gesture.y={};gesture.ray={};return;}
    const double infinity=std::numeric_limits<double>::infinity();
    auto x=alignMath::axis(box,gesture.bounds,gesture.orderX,false,unit,gesture.x,-infinity,infinity,first->x);
    auto y=alignMath::axis(box,gesture.bounds,gesture.orderY,true,unit,gesture.y,-infinity,infinity,first->y);
    // Attached and fused rings retain their native vertices and bond lengths.
    const bool free=at<uint8_t>(gesture.tracker,0x118)&&!at<Obj>(gesture.tracker,0x120)&&!alternate;
    if(!free){if(std::abs(x.delta)>.25*unit)x={};if(std::abs(y.delta)>.25*unit)y={};}
    Point shift{free?x.delta:0,free?y.delta:0,0};
    if(!alignMath::fitsLane(x,box.moved(shift.x,shift.y),gesture.bounds,false,unit)){x={};shift.x=0;}
    if(!alignMath::fitsLane(y,box.moved(shift.x,shift.y),gesture.bounds,true,unit)){y={};shift.y=0;}
    gesture.x=x;gesture.y=y;
    if(free)chooseRay(gesture,box,unit,shift);else gesture.ray={};
    gesture.drawingBounds=box.moved(shift.x,shift.y);gesture.ringShift=shift;
    if(!shift.x&&!shift.y)return;
    for(auto p=first;p!=last;++p){p->x+=shift.x;p->y+=shift.y;}
    Obj source=at<Obj>(gesture.tracker,0x110);
    Point position=at<Point>(source,0x1f8);position.x+=shift.x;position.y+=shift.y;
    UndoOff off(gesture.doc);oldSetAtomPos(source,&position);
}
void ringDrawingMove(Obj tracker) {
    smartAlignmentDrawingMove(tracker);
    if(!onUI()||gesture.tracker!=tracker||gesture.drawing!=Drawing::Ring){oldRingMove(tracker);return;}
    restoreRingShift();gesture.ringActive=true;
    try {oldRingMove(tracker);}catch(...){gesture.ringActive=false;throw;}
    gesture.ringActive=false;
}
short finishDrawingRing(Obj tracker) {
    Obj page=at<Obj>(tracker,8);
    if(ghostPlacement.free&&ghostPlacement.tool==7&&ghostPlacementHeld(page)&&at<uint8_t>(tracker,0x118)) {
        // Native movement can switch a click to PlaceRing and rotate/recenter
        // the preview. A held blank-paper ghost owns a default DropRing click.
        if(gesture.tracker==tracker)restoreRingShift();
        if(Obj source=at<Obj>(tracker,0x110)) {
            const Point center=ghostPlacementCenter();
            UndoOff off(at<Obj>(page,8));oldSetAtomPos(source,&center);
            at<Point>(tracker,0x50)=center;
            at<uint8_t>(tracker,0x130)=0;
        }
    }
    return oldRingFinish(tracker);
}
void graphicDrawingMove(Obj tracker) {smartAlignmentDrawingMove(tracker);oldGraphicMove(tracker);}
void bondDrawingMove(Obj tracker) {smartAlignmentDrawingMove(tracker);oldBondMove(tracker);}
void genericDrawingMove(Obj tracker) {smartAlignmentDrawingMove(tracker);oldGenericMove(tracker);}
void chainDrawingMove(Obj tracker) {smartAlignmentDrawingMove(tracker);oldChainMove(tracker);}
Point* snapNative(Snap original,Obj object,Point* result,const Point* delta) {
    Point* returned=original(object,result,delta);
    if(gesture.inOriginal&&std::isfinite(result->x+result->y)) {
        gesture.nativeX|=std::abs(result->x)>1e-8;
        gesture.nativeY|=std::abs(result->y)>1e-8;
    }
    return returned;
}
Point* arrowSnap(Obj o,Point* out,const Point* delta) {return snapNative(oldArrowSnap,o,out,delta);}
Point* graphicSnap(Obj o,Point* out,const Point* delta) {return snapNative(oldGraphicSnap,o,out,delta);}
Point* constrain(Obj tracker,Point* result) {
    if(!onUI()||gesture.tracker!=tracker||(!gesture.moving&&!gesture.finishing))return oldConstrain(tracker,result);
    if(gesture.cancelled) {*result=gesture.down;gesture.effective=*result;gesture.resolved=true;return result;}
    gesture.nativeX=gesture.nativeY=false;gesture.inOriginal=true;
    Point* returned{};
    try {returned=oldConstrain(tracker,result);}catch(...){gesture.inOriginal=false;throw;}
    gesture.inOriginal=false;gesture.effective=*result;gesture.resolved=true;
    Obj insertionScale=at<Obj>(gesture.page,0x2a8);
    const double insertionUnit=insertionScale?at<double>(insertionScale,8)*double(GetDpiForWindow(gesture.window))/96:0;
    if(constrainArrowInsertion(tracker,*result,insertionUnit)) {
        gesture.x={};gesture.y={};gesture.ray={};gesture.effective=*result;return returned;
    }
    if(!enabled||!gesture.eligible||at<uint8_t>(tracker,0x238)||
        (GetAsyncKeyState(VK_MENU)&0x8000)||!std::isfinite(result->x+result->y)) {gesture.x={};gesture.y={};gesture.ray={};return returned;}
    Obj scale=at<Obj>(gesture.page,0x2a8);
    const double dip=double(GetDpiForWindow(gesture.window))/96;
    double unit=scale?at<double>(scale,8)*dip:0;
    if(!std::isfinite(unit)||unit<=0)return returned;
    // Cap attraction when the entire drawing is smaller than the nominal radius.
    const double size=std::max(gesture.initial.r-gesture.initial.l,gesture.initial.b-gesture.initial.t);
    if(size>0)unit=std::min(unit,size/24);
    const auto& l=at<RectD>(tracker,0x198);const auto& s=at<RectD>(tracker,0x1f8);
    if(!valid(l)||!valid(s))return returned;
    const double px=(s.r-s.l)*.8,py=(s.b-s.t)*.8;
    const Box m=gesture.initial.moved(result->x-gesture.down.x,result->y-gesture.down.y);
    // Native centerline snapping constrains its corrected coordinate only;
    // it must not disable equal spacing on the perpendicular coordinate.
    bool lockX=gesture.nativeX,lockY=gesture.nativeY;
    if(GetAsyncKeyState(VK_SHIFT)&0x8000) {
        const Point raw=at<Point>(tracker,0x50);
        lockX=std::abs(raw.x-gesture.down.x)<=std::abs(raw.y-gesture.down.y);lockY=!lockX;
    }
    gesture.x=lockX?Relation{}:alignMath::axis(m,gesture.bounds,gesture.orderX,false,unit,gesture.x,l.l-px,l.r+px,result->x);
    gesture.y=lockY?Relation{}:alignMath::axis(m,gesture.bounds,gesture.orderY,true,unit,gesture.y,l.t-py,l.b+py,result->y);
    Box final=m.moved(gesture.x.delta,gesture.y.delta);
    if(!alignMath::fitsLane(gesture.x,final,gesture.bounds,false,unit))gesture.x={};
    final=m.moved(gesture.x.delta,gesture.y.delta);
    if(!alignMath::fitsLane(gesture.y,final,gesture.bounds,true,unit))gesture.y={};
    Point shift{gesture.x.delta,gesture.y.delta,0};
    if(gesture.molecule&&!lockX&&!lockY) {
        const auto x=gesture.x,y=gesture.y;
        if(chooseRay(gesture,m,unit,shift)&&
            (result->x+shift.x<l.l-px||result->x+shift.x>l.r+px||
             result->y+shift.y<l.t-py||result->y+shift.y>l.b+py)) {
            gesture.ray={};gesture.x=x;gesture.y=y;shift={x.delta,y.delta,0};
        }
    }else gesture.ray={};
    result->x+=shift.x;result->y+=shift.y;gesture.effective=*result;
    return returned;
}
void preview(Obj tracker) {
    // The UI drag factory can prepare the atom/object move sets after modal
    // entry. Reconcile once at the first native preview, before it transforms
    // any document geometry, rather than leaving an empty entry snapshot.
    if(onUI()&&trackingDepth&&nativeTracker(tracker)&&gesture.tracker!=tracker) {
        Obj doc=vf<Obj(*)(Obj)>(tracker,0x80)(tracker);
        beginSmartAlignment(tracker,doc,gpuTrackingActive());
    }
    if(!onUI()||gesture.tracker!=tracker){oldPreview(tracker);return;}
    const Point savedPrevious=at<Point>(tracker,0x68);
    // Native damage is incremental, while native artwork is translated from down.
    at<Point>(tracker,0x68)=gesture.previousEffective;gesture.moving=true;gesture.resolved=false;
    try {oldPreview(tracker);}catch(...){at<Point>(tracker,0x68)=savedPrevious;gesture.moving=false;throw;}
    at<Point>(tracker,0x68)=savedPrevious;gesture.moving=false;
    if(gesture.resolved)gesture.previousEffective=gesture.effective;
}
void finish(Obj tracker,bool arg,bool commit) {
    if(!onUI()||gesture.tracker!=tracker){oldFinish(tracker,arg,commit);return;}
    clearGpuAlignment(gesture.doc);gesture.finishing=true;
    // Preview transforms have already been reversed. Zero final translation
    // cancels the move through the normal native finalizer and selection cleanup.
    if(gesture.cancelled)at<Point>(tracker,0x50)=gesture.down;
    prepareArrowInsertionFinish(tracker,!arg,commit&&!gesture.cancelled);
    try {oldFinish(tracker,arg,commit);}catch(...){finishArrowInsertion(tracker);gesture.finishing=false;throw;}
    finishArrowInsertion(tracker);
    gesture.finishing=false;gesture.eligible=false;gesture.x={};gesture.y={};
}
LRESULT CALLBACK shortcuts(int code,WPARAM removal,LPARAM value) {
    if(code>=0&&removal==PM_REMOVE&&value&&!trackingDepth) {
        auto& message=*reinterpret_cast<MSG*>(value);
        if((message.message==WM_KEYDOWN||message.message==WM_SYSKEYDOWN)&&message.wParam=='A'&&(GetKeyState(VK_CONTROL)&0x8000)&&
            (GetKeyState(VK_MENU)&0x8000)) {
            if(!(message.lParam&(1LL<<30))) {
                // Activation is idempotent: repeating the enable shortcut must
                // never leave every gesture and idle index preparation disabled.
                enabled=(GetKeyState(VK_SHIFT)&0x8000)==0;
                bumpGeneration();
                if(enabled)PostThreadMessageW(uiThread,WM_NULL,0,0);
                OutputDebugStringA(enabled?"ChemDraw smart alignment enabled.\n":"ChemDraw smart alignment disabled.\n");
            }
            message.message=WM_NULL;message.wParam=0;message.lParam=0;
        }
    }
    return CallNextHookEx(keyboardHook,code,removal,value);
}
}
static void prepareAlignmentPage(Obj doc,bool immediate) noexcept {
    if(!onUI()||!installed||!doc||warmingIndex)return;
    if(!immediate&&(trackingDepth||GetCapture()||
        (HIWORD(GetQueueStatus(QS_INPUT))&QS_INPUT)||
        ((GetAsyncKeyState(VK_LBUTTON)|GetAsyncKeyState(VK_RBUTTON)|GetAsyncKeyState(VK_MBUTTON))&0x8000)))return;
    Obj page=mainPage(doc);if(!page||at<Obj>(page,0x10))return;
    try {
        if(pageIndices.size()>=16&&!pageIndices.contains(page))pageIndices.clear();
        auto& cached=pageIndices[page];
        if(cached.ready&&cached.head==at<Node*>(page,0xc8)&&cached.count==at<size_t>(page,0xd0))return;
        const auto revision=cached.revision;
        PageAlignmentIndex next;next.head=at<Node*>(page,0xc8);next.count=at<size_t>(page,0xd0);
        if(next.count>16384)return;
        std::unordered_set<Obj> parents;
        warmingIndex=true;
        struct EndWarm {~EndWarm(){warmingIndex=false;}} cleanup;
        UndoOff undo(doc);
        each(next.head,[&](Obj o){if(Obj parent=at<Obj>(o,0x28))parents.insert(parent);});
        each(next.head,[&](Obj o) {
            if(!vf<bool(*)(Obj)>(o,0xb8)(o))return;
            Obj root=rootOf(o,page);if(!root)return;
            auto slot=next.rootForObject.find(root);
            size_t id;
            if(slot==next.rootForObject.end()) {
                id=next.roots.size();next.rootForObject.emplace(root,id);
                next.roots.push_back({Reference{root,{}}});
            } else id=slot->second;
            CachedMember member{o,!parents.contains(o),{}};
            for(Obj parent=at<Obj>(o,0x28);parent&&at<Obj>(parent,0x60)==page;) {
                member.ancestors.push_back(parent);next.rootForObject.emplace(parent,id);
                if(parent==root||member.ancestors.size()>=32)break;
                parent=at<Obj>(parent,0x28);
            }
            next.rootForObject.insert_or_assign(o,id);
            const uintptr_t table=at<uintptr_t>(o,0);
            next.roots[id].molecule|=table==base+0x8b34f8||table==base+0x8babc0;
            if(table==base+0x8b2cf0) {
                Point tail{},head{},tailOut{},headOut{};
                if(arrowEndpointTangents(o,tail,head,tailOut,headOut)) {
                    next.rays.push_back({o,root,{tail.x,tail.y,tailOut.x,tailOut.y}});
                    next.rays.push_back({o,root,{head.x,head.y,headOut.x,headOut.y}});
                }
            }
            next.roots[id].members.push_back(std::move(member));
        });
        // Prefer the idle snapshot; a missed idle must not disable the gesture.
        // The one-time fallback runs before preview changes any geometry.
        for(auto& root:next.roots) {
            if(!immediate&&(HIWORD(GetQueueStatus(QS_INPUT))&QS_INPUT))return;
            root.reference.bounds=inkBounds(root.reference.object);
        }
        next.drawOrder.resize(next.roots.size());std::iota(next.drawOrder.begin(),next.drawOrder.end(),0);
        std::sort(next.drawOrder.begin(),next.drawOrder.end(),[&](int a,int b){
            return at<int>(next.roots[a].reference.object,0x20)<at<int>(next.roots[b].reference.object,0x20);});
        next.orderX=next.drawOrder;next.orderY=next.drawOrder;
        for(bool y:{false,true}) {
            auto& order=y?next.orderY:next.orderX;
            std::stable_sort(order.begin(),order.end(),[&](int a,int b){
                const auto aa=next.roots[a].reference.bounds,bb=next.roots[b].reference.bounds;
                return aa.valid()!=bb.valid()?aa.valid():aa.valid()&&alignMath::low(aa,y)<alignMath::low(bb,y);
            });
        }
        if((!immediate&&cached.revision!=revision)||next.head!=at<Node*>(page,0xc8)||next.count!=at<size_t>(page,0xd0))return;
        next.revision=cached.revision;next.ready=true;cached=std::move(next);
    }catch(...) {warmingIndex=false;}
}
static void appendArrowEndpoints(Gesture& state,const PageAlignmentIndex& index) {
    for(const auto& r:index.rays) {
        if(state.movingObjects.contains(r.object)||state.movingObjects.contains(r.root))continue;
        const Box point{r.ray.x,r.ray.y,r.ray.x,r.ray.y,false};
        state.references.push_back({r.object,point});state.bounds.push_back(point);
    }
    state.orderX.resize(state.bounds.size());std::iota(state.orderX.begin(),state.orderX.end(),0);
    state.orderY=state.orderX;
    for(bool y:{false,true}) {
        auto& order=y?state.orderY:state.orderX;
        std::stable_sort(order.begin(),order.end(),[&](int a,int b){return alignMath::low(state.bounds[a],y)<alignMath::low(state.bounds[b],y);});
    }
}
void warmSmartAlignmentPage(Obj doc) noexcept {prepareAlignmentPage(doc,false);}
void beginSmartAlignment(Obj tracker,Obj doc,bool gpu) noexcept {
    cancelArrowInsertion();
    gesture={};
    (void)gpu; // Native geometry snapping also works without a GPU work buffer.
    arrowInsertionStage("selection gesture entry");
    if(!installed||!doc||!tracker){arrowInsertionStage("selection feature or document unavailable");return;}
    const Drawing drawing=drawingTracker(tracker);
    if(drawing!=Drawing::None&&!patchEnabled(18))return;
    if(drawing==Drawing::None&&(!nativeTracker(tracker)||at<uint8_t>(tracker,0x238))) {
        arrowInsertionStage("not a native selection move, or copy drag");return;
    }
    Obj page=at<Obj>(tracker,8),port=at<Obj>(doc,0x258);
    if(!page||!port||page!=mainPage(doc)||at<Obj>(page,0x10)) {
        arrowInsertionStage("selection move is not on the main canvas page");return;
    }
    try {
        gesture.tracker=tracker;gesture.page=page;gesture.doc=doc;gesture.window=portWindow(port);
        gesture.drawing=drawing;
        gesture.down=at<Point>(tracker,0x38);gesture.previousEffective=gesture.down;gesture.effective=gesture.down;
        auto found=pageIndices.find(page);
        const bool needsIndex=found==pageIndices.end()||found->second.roots.empty()||
            (drawing==Drawing::None&&(!found->second.ready||
                found->second.head!=at<Node*>(page,0xc8)||found->second.count!=at<size_t>(page,0xd0)));
        if(needsIndex){prepareAlignmentPage(doc,true);found=pageIndices.find(page);}
        if(drawing!=Drawing::None) {
            arrowInsertionStage("drawing tracker, not molecule selection drag");
            Obj object{};
            switch(drawing) {
            case Drawing::Graphic:case Drawing::Arrow:case Drawing::ElectronArrow:object=at<Obj>(tracker,0x138);break;
            case Drawing::Bond:object=at<Obj>(tracker,0x160);break;
            case Drawing::Ring:case Drawing::Chain:object=at<Obj>(tracker,0x110);break;
            default:break;
            }
            if(object) {
                gesture.movingObjects.insert(object);
                if(Obj root=rootOf(object,page))gesture.movingObjects.insert(root);
            }
            if(drawing==Drawing::Bond) {
                if(Obj endpoint=at<Obj>(tracker,0x178))gesture.movingObjects.insert(endpoint);
                if(Obj bond=at<Obj>(tracker,0x180))gesture.movingObjects.insert(bond);
                if(Obj bond=at<Obj>(tracker,0x188))gesture.movingObjects.insert(bond);
            }
            // Creation edits one endpoint, not the whole source molecule. Keep
            // its stationary leaves as targets and omit temporary preview ink.
            std::unordered_set<Obj> parents,seen;
            each(at<Node*>(page,0xc8),[&](Obj o){if(Obj parent=at<Obj>(o,0x28))parents.insert(parent);});
            UndoOff undo(doc);
            each(at<Node*>(page,0xc8),[&](Obj o) {
                if(!vf<bool(*)(Obj)>(o,0xb8)(o))return;
                Obj root=rootOf(o,page);if(!root)return;
                Obj reference=gesture.movingObjects.contains(root)?o:root;
                if(gesture.movingObjects.contains(reference)||
                    (reference==o&&parents.contains(o))||!seen.insert(reference).second)return;
                const Box box=inkBounds(reference);if(!box.valid())return;
                gesture.references.push_back({reference,box});gesture.bounds.push_back(box);
            });
            if(gesture.references.empty()||gesture.references.size()>4096){gesture={};return;}
            gesture.orderX.resize(gesture.bounds.size());std::iota(gesture.orderX.begin(),gesture.orderX.end(),0);
            gesture.orderY=gesture.orderX;
            for(bool y:{false,true}) {
                auto& order=y?gesture.orderY:gesture.orderX;
                std::stable_sort(order.begin(),order.end(),[&](int a,int b){return alignMath::low(gesture.bounds[a],y)<alignMath::low(gesture.bounds[b],y);});
            }
            if(found!=pageIndices.end())for(const auto& r:found->second.rays)
                if(!gesture.movingObjects.contains(r.object)&&!gesture.movingObjects.contains(r.root))gesture.rays.push_back(r.ray);
            if(found!=pageIndices.end())appendArrowEndpoints(gesture,found->second);
            gesture.initial=endpointBounds(gesture.down,gesture.down);gesture.drawingBounds=gesture.initial;
            gesture.eligible=true;return;
        }
        arrowInsertionStage("waiting for page membership index");
        if(found==pageIndices.end()||!found->second.ready||
            found->second.head!=at<Node*>(page,0xc8)||found->second.count!=at<size_t>(page,0xd0))return;
        const auto& index=found->second;
        arrowInsertionStage("reading native atom, bond and object move sets");
        each(at<Node*>(tracker,0x140),[](Obj o){gesture.movingObjects.insert(o);});
        each(at<Node*>(tracker,0x120),[](Obj o){gesture.movingObjects.insert(o);});
        each(at<Node*>(tracker,0x130),[](Obj o){gesture.movingObjects.insert(o);});
        if(gesture.movingObjects.empty()){arrowInsertionStage("native move sets are empty");gesture={};return;}
        std::unordered_set<size_t> movingRoots;
        for(Obj o:gesture.movingObjects)if(auto root=index.rootForObject.find(o);root!=index.rootForObject.end())
            movingRoots.insert(root->second);
        bool first=true;
        for(size_t id:movingRoots) {
            const auto& r=index.roots[id];bool moving{},stationary{};
            for(const auto& member:r.members) {
                bool selected=gesture.movingObjects.contains(member.object);
                for(Obj parent:member.ancestors)if(!selected)selected=gesture.movingObjects.contains(parent);
                if(selected)moving=true;else if(member.leaf)stationary=true;
            }
            if(moving&&stationary){arrowInsertionStage("partial molecule or group selection");gesture={};return;}
            const Box bounds=r.reference.bounds;if(!moving||!bounds.valid())continue;
            {
                gesture.movingObjects.insert(r.reference.object);gesture.molecule|=r.molecule;
                if(first){gesture.initial=bounds;first=false;}
                else {auto& m=gesture.initial;m={std::min(m.l,bounds.l),std::min(m.t,bounds.t),std::max(m.r,bounds.r),std::max(m.b,bounds.b)};}
            }
        }
        if(first){arrowInsertionStage("no complete moving root in page membership index");gesture={};return;}
        // Stable native draw order keeps equal-distance choices deterministic.
        std::vector<int> remap(index.roots.size(),-1);
        for(int id:index.drawOrder)if(!movingRoots.contains(size_t(id))&&index.roots[id].reference.bounds.valid()) {
            remap[id]=int(gesture.references.size());gesture.references.push_back(index.roots[id].reference);
            gesture.bounds.push_back(index.roots[id].reference.bounds);
        }
        if(gesture.references.empty()||gesture.references.size()>4096){arrowInsertionStage("stationary references unavailable or excessive");gesture={};return;}
        for(const auto& r:index.rays)
            if(!gesture.movingObjects.contains(r.object)&&!gesture.movingObjects.contains(r.root))gesture.rays.push_back(r.ray);
        for(int id:index.orderX)if(remap[id]>=0)gesture.orderX.push_back(remap[id]);
        for(int id:index.orderY)if(remap[id]>=0)gesture.orderY.push_back(remap[id]);
        appendArrowEndpoints(gesture,index);
        arrowInsertionStage(!gpu?"GPU selection work buffer unavailable":"multiple moving selection roots");
        if(gpu&&movingRoots.size()==1) {
            const auto& moving=index.roots[*movingRoots.begin()];
            std::unordered_set<Obj> fragments;
            auto includeFragment=[&](Obj object) {
                if(object&&at<uintptr_t>(object,0)==base+0x89a468)fragments.insert(object);
            };
            includeFragment(moving.reference.object);
            for(const auto& member:moving.members) {
                includeFragment(member.object);
                for(Obj ancestor:member.ancestors)includeFragment(ancestor);
            }
            if(fragments.size()==1) {
                Obj molecule=*fragments.begin();
                std::vector<ArrowInsertionTarget> targets;targets.reserve(index.roots.size());
                std::unordered_set<Obj> seen;
                auto includeTarget=[&](Obj object) {
                    if(!object||!seen.insert(object).second)return;
                    RectD bounds{};vf<RectD*(*)(Obj,RectD*)>(object,0x1d8)(object,&bounds);
                    if(valid(bounds))targets.push_back({object,bounds});
                };
                // Native arrow/path members can sit inside a larger reaction
                // group. Root-only enumeration hides their individual shafts.
                for(int id:index.drawOrder)if(!movingRoots.contains(size_t(id))) {
                    const auto& stationary=index.roots[id];
                    includeTarget(stationary.reference.object);
                    for(const auto& member:stationary.members) {
                        if(at<uintptr_t>(member.object,0)==base+0x8b2cf0)includeTarget(member.object);
                        for(Obj ancestor:member.ancestors)includeTarget(ancestor);
                    }
                }
                RectD bounds{};vf<RectD*(*)(Obj,RectD*)>(molecule,0x1d8)(molecule,&bounds);
                beginArrowInsertion(tracker,doc,molecule,bounds,targets);
            }else {
                arrowInsertionStage(fragments.empty()?"selection contains no molecule fragment":
                    "selection contains multiple molecule fragments");
            }
        }
        gesture.eligible=true;
    }catch(...){arrowInsertionStage("exception during selection gesture preparation");gesture={};}
}
void smartAlignmentDrawingMove(Obj tracker) noexcept {
    if (!patchEnabled(18)) return;
    if(!onUI()||!installed||!enabled||!trackingDepth||drawingTracker(tracker)==Drawing::None)return;
    // PrepareTracking may create or replace the endpoint after modal entry.
    if(gesture.tracker!=tracker||(!gesture.eligible&&!gesture.cancelled&&!gesture.released)) {
        Obj doc=vf<Obj(*)(Obj)>(tracker,0x80)(tracker);
        beginSmartAlignment(tracker,doc,true);
    }
    if(gesture.tracker==tracker)alignDrawingTrackerPoint(tracker);
}
bool smartAlignmentDrawingArrow(Obj arrow,const Point& head,const Point& tail,Point& snappedHead,Point& snappedTail) noexcept {
    if (!patchEnabled(18)) return false;
    if(!onUI()||!gesture.eligible||gesture.settingPoint||
        (gesture.drawing!=Drawing::Arrow&&gesture.drawing!=Drawing::ElectronArrow)||
        at<Obj>(gesture.tracker,0x138)!=arrow)return false;
    try {
        // Native arrow constraints finish before SetStartAndEnd consumes the
        // endpoint. Substitute there so they cannot undo the page alignment.
        const bool movingTail=gesture.drawing==Drawing::Arrow&&at<uint8_t>(gesture.tracker,0x140);
        const Point raw=movingTail?tail:head,current=at<Point>(gesture.tracker,0x50);
        if(std::hypot(raw.x-current.x,raw.y-current.y)>drawingUnit()*.01)return false;
        const Point corrected=drawingPoint(raw);
        snappedHead=movingTail?head:corrected;snappedTail=movingTail?corrected:tail;
        bool fixedChanged=false;
        if(gesture.drawing==Drawing::Arrow&&std::abs(at<double>(arrow,0x130))<1e-9&&
            at<int>(arrow,movingTail?0x1f4:0x1f0)<0) {
            Point& source=movingTail?snappedHead:snappedTail;
            const double unit=drawingUnit();
            // Near-cardinal arrows align their whole shaft with the endpoint
            // guide. A subpixel press offset must not leave the tail tilted.
            if(gesture.y.kind==Kind::Align&&std::abs(head.y-tail.y)<=6*unit&&
                std::abs(head.x-tail.x)>=24*unit) {
                fixedChanged=source.y!=corrected.y;source.y=corrected.y;
                at<Point>(gesture.tracker,0x148).y=source.y;
            }else if(gesture.x.kind==Kind::Align&&std::abs(head.x-tail.x)<=6*unit&&
                std::abs(head.y-tail.y)>=24*unit) {
                fixedChanged=source.x!=corrected.x;source.x=corrected.x;
                at<Point>(gesture.tracker,0x148).x=source.x;
            }
        }
        at<Point>(gesture.tracker,0x50)=corrected;
        return fixedChanged||corrected.x!=raw.x||corrected.y!=raw.y;
    }catch(...){return false;}
}
void endSmartAlignment() noexcept {endArrowInsertionTrace();cancelArrowInsertion();if(gesture.doc)clearGpuAlignment(gesture.doc);gesture={};}
bool cancelSmartAlignment(HWND w) noexcept {
    if(ghostPlacement.window==w)ghostPlacement.dragged=true;
    if(!gesture.tracker||gesture.window!=w)return false;
    cancelArrowInsertion();
    if(gesture.drawing!=Drawing::None) {
        try {restoreRingShift();}catch(...){}
        gesture.eligible=false;clearRelations();return false;
    }
    gesture.cancelled=true;gesture.released=false;clearRelations();return true;
}
void smartAlignmentReleased(HWND w,const MSG& message) noexcept {
    arrowInsertionReleased(w);
    if(!gesture.tracker||gesture.window!=w||gesture.cancelled)return;
    gesture.released=true;
    POINT client=message.pt;
    if(!ScreenToClient(w,&client)){gesture.eligible=false;clearRelations();return;}
    Obj port=at<Obj>(gesture.doc,0x258);
    struct PointI {int x,y,z;} cursor{client.x+at<int>(port,0xa8),client.y+at<int>(port,0xac),0};
    fn<void(*)(Obj,const PointI*)>(0x4fa660)(gesture.tracker,&cursor);
    Point internal{};
    Obj actual=fn<Obj(*)(Obj,const PointI*,Point*,bool)>(0x3e3570)(gesture.page,&cursor,&internal,false);
    if(actual!=gesture.page||!std::isfinite(internal.x+internal.y)){gesture.eligible=false;clearRelations();return;}
    at<Point>(gesture.tracker,0x50)=internal;
    // Reconcile while the native work port is still valid. CopyBuffer publishes
    // the release frame through its existing path, with guides cleared on drop.
    try {
        if(gesture.drawing!=Drawing::None) {
            if(gesture.drawing!=Drawing::Ring&&gesture.drawing!=Drawing::Bond)setDrawingPoint(gesture.tracker,&internal);
            vf<void(*)(Obj,bool)>(gesture.tracker,8)(gesture.tracker,false);
            vf<void(*)(Obj)>(gesture.tracker,0x70)(gesture.tracker);
            vf<void(*)(Obj)>(gesture.tracker,0x38)(gesture.tracker);
        } else preview(gesture.tracker);
        vf<void(*)(Obj)>(gesture.tracker,0x10)(gesture.tracker);
    }catch(...){gesture.eligible=false;}
    clearGpuAlignment(gesture.doc);
}
void smartAlignmentPageGone(Obj page) noexcept {
    if(onUI())pageIndices.erase(page);
    if(onUI()&&ghostSnap.page==page)ghostSnap={};
    if(onUI()&&ghostPlacement.page==page)ghostPlacement={};
    if(onUI()&&gesture.page==page){cancelArrowInsertion();clearGpuAlignment(gesture.doc);gesture={};}
}
void smartAlignmentGeometryInvalidated(Obj object) noexcept {
    if(onUI()&&object)if(auto cached=pageIndices.find(at<Obj>(object,0x60));cached!=pageIndices.end()) {
        cached->second.ready=false;++cached->second.revision;
    }
}
void smartAlignmentObjectChanged(Obj object) noexcept {
    if(onUI())arrowInsertionObjectChanged(object);
    smartAlignmentGeometryInvalidated(object);
    if(!onUI()||!gesture.eligible||!object||gesture.movingObjects.contains(object))return;
    if(at<Obj>(object,0x60)!=gesture.page)return;
    const Obj root=rootOf(object,gesture.page);
    if(gesture.movingObjects.contains(root))return;
    for(const auto& r:gesture.references)if(r.object==object||r.object==root){gesture.eligible=false;clearRelations();break;}
}
static std::shared_ptr<const AlignmentFeedback> makeAlignmentFeedback(const Gesture& state,bool documentSpace) noexcept {
    if(!state.eligible||state.released||state.cancelled||
        (state.x.kind==Kind::None&&state.y.kind==Kind::None&&state.ray.index<0))return {};
    try {
        auto feedback=std::make_shared<AlignmentFeedback>();
        feedback->dip=float(GetDpiForWindow(state.window))/96;if(feedback->dip<=0)feedback->dip=1;
        Obj port=at<Obj>(state.doc,0x258),scale=at<Obj>(state.page,0x2a8);
        auto* graphics=documentSpace?nullptr:fn<Gdiplus::Graphics*(*)(Obj)>(0x623140)(port);if(!documentSpace&&!graphics)return {};
        feedback->documentSpace=documentSpace;
        const double marginUnit=documentSpace?at<double>(scale,8)*feedback->dip:feedback->dip;
        const Box m=state.drawing==Drawing::None?
            state.initial.moved(state.effective.x-state.down.x,state.effective.y-state.down.y):state.drawingBounds;
        auto project=[&](double x,double y) {
            if(documentSpace)return ScenePoint{float(x),float(y)};
            Gdiplus::PointF p{float(fn<double(*)(Obj,double)>(0x3c6560)(scale,x)),float(fn<double(*)(Obj,double)>(0x3c6590)(scale,y))};
            graphics->TransformPoints(Gdiplus::CoordinateSpaceDevice,Gdiplus::CoordinateSpaceWorld,&p,1);
            return ScenePoint{p.X,p.Y};
        };
        if(state.ray.index>=0&&size_t(state.ray.index)<state.rays.size()) {
            const auto& ray=state.rays[state.ray.index];
            const double cx=(m.l+m.r)*.5,cy=(m.t+m.b)*.5;
            const double along=(cx-ray.x)*ray.dx+(cy-ray.y)*ray.dy;
            const double unit=at<double>(scale,8)*feedback->dip;
            feedback->strokes.push_back({project(ray.x,ray.y),
                project(ray.x+ray.dx*(along+8*unit),ray.y+ray.dy*(along+8*unit)),false});
        }
        auto screenBox=[&](Box b) {auto a=project(b.l,b.t),z=project(b.r,b.b);return Box{a.x,a.y,z.x,z.y};};
        for(bool y:{false,true}) {
            const Relation c=y?state.y:state.x;if(c.kind==Kind::None)continue;
            const Box a=state.bounds[c.a];
            if(c.kind==Kind::Align) {
                const double v=alignMath::anchor(a,y,c.anchor);
                auto from=y?project(std::min(a.l,m.l),v):project(v,std::min(a.t,m.t));
                auto to=y?project(std::max(a.r,m.r),v):project(v,std::max(a.b,m.b));
                const float margin=float(8*marginUnit);
                if(y){from.x-=margin;to.x+=margin;}else{from.y-=margin;to.y+=margin;}
                feedback->strokes.push_back({from,to,false});
            }else {
                const Box sa=screenBox(a),sb=screenBox(state.bounds[c.b]),sm=screenBox(m);
                const double edge=std::max({alignMath::high(sa,!y),alignMath::high(sb,!y),alignMath::high(sm,!y)});
                RectI viewport{};vf<RectI*(*)(Obj,RectI*)>(state.doc,0x100)(state.doc,&viewport);
                const double rail=documentSpace?edge+10*marginUnit:
                    std::min(edge+10*marginUnit,double(y?viewport.r:viewport.b)-6*marginUnit);
                auto segment=[&](double lo,double hi,Box first,Box second) {
                    auto p=[&](double along,double across){return ScenePoint{float(y?across:along),float(y?along:across)};};
                    feedback->strokes.push_back({p(lo,rail),p(hi,rail),true});
                    feedback->strokes.push_back({p(lo,alignMath::high(first,!y)),p(lo,rail+4*marginUnit),false});
                    feedback->strokes.push_back({p(hi,alignMath::high(second,!y)),p(hi,rail+4*marginUnit),false});
                };
                if(c.kind==Kind::Between) {segment(alignMath::high(sa,y),alignMath::low(sm,y),sa,sm);segment(alignMath::high(sm,y),alignMath::low(sb,y),sm,sb);}
                else {segment(alignMath::high(sa,y),alignMath::low(sb,y),sa,sb);
                    if(c.kind==Kind::After)segment(alignMath::high(sb,y),alignMath::low(sm,y),sb,sm);
                    else segment(alignMath::high(sm,y),alignMath::low(sa,y),sm,sa);}
            }
        }
        return feedback;
    }catch(...){return {};}
}
std::shared_ptr<const AlignmentFeedback> smartAlignmentFeedback(Obj tracker) noexcept {
    if(gesture.tracker!=tracker)return {};
    return makeAlignmentFeedback(gesture,false);
}
void smartAlignmentGhost(Obj page,Obj target,const GhostTool& tool,GhostBond& ghost,double unit) noexcept {
    if (!patchEnabled(18)) return;
    if(!onUI()||!installed||!enabled||!page||!ghost.artwork||!ghost.visible||trackingDepth||
        !std::isfinite(unit)||unit<=0||(GetAsyncKeyState(VK_MENU)&0x8000))return;
    try {
        Obj doc=at<Obj>(page,8),port=doc?at<Obj>(doc,0x258):nullptr;if(!port)return;
        auto found=pageIndices.find(page);
        if(found==pageIndices.end()||!found->second.ready) {
            prepareAlignmentPage(doc,true);found=pageIndices.find(page);
        }
        if(found==pageIndices.end()||!found->second.ready)return;
        const auto& index=found->second;
        if(ghostSnap.page!=page||ghostSnap.target!=target||ghostSnap.tool!=tool.key||ghostSnap.revision!=index.revision)
            ghostSnap={page,target,tool.key,index.revision};
        Gesture state;state.page=page;state.doc=doc;state.window=portWindow(port);
        state.eligible=true;state.drawing=Drawing::Generic;
        std::vector<int> remap(index.roots.size(),-1);
        for(int id:index.drawOrder) {
            const auto& r=index.roots[id].reference;if(!r.bounds.valid())continue;
            remap[id]=int(state.bounds.size());state.bounds.push_back(r.bounds);
        }
        if(state.bounds.empty()||state.bounds.size()>4096)return;
        for(int id:index.orderX)if(remap[id]>=0)state.orderX.push_back(remap[id]);
        for(int id:index.orderY)if(remap[id]>=0)state.orderY.push_back(remap[id]);
        appendArrowEndpoints(state,index);
        Box box{};bool first=true;
        auto include=[&](Point p) {
            if(!std::isfinite(p.x+p.y))return;
            if(first){box={p.x,p.y,p.x,p.y};first=false;}
            else box={std::min(box.l,p.x),std::min(box.t,p.y),std::max(box.r,p.x),std::max(box.b,p.y)};
        };
        for(const auto& line:ghost.artwork->strokes){include(line.start);include(line.end);}
        for(const auto& path:ghost.artwork->paths)for(Point p:path.points)include(p);
        if(first||!box.valid())return;
        const double infinity=std::numeric_limits<double>::infinity();
        // A vertex bond ghost resizes about its attachment; blank-paper ghosts
        // translate as complete objects, retaining their native shape and length.
        const bool arrow=nativeArrowTool(tool.tool);
        const bool endpoint=target&&(arrow||(tool.tool==3&&ghost.placement));
        if(arrow)box=endpointBounds(ghost.end,ghost.end);
        else if(endpoint)box=endpointBounds(ghost.start,ghost.end);
        const double lx=endpoint&&!arrow?(ghost.end.x<ghost.start.x?1:0):1;
        const double hx=endpoint&&!arrow?(ghost.end.x>=ghost.start.x?1:0):1;
        const double ly=endpoint&&!arrow?(ghost.end.y<ghost.start.y?1:0):1;
        const double hy=endpoint&&!arrow?(ghost.end.y>=ghost.start.y?1:0):1;
        state.x=alignMath::axis(box,state.bounds,state.orderX,false,unit,ghostSnap.x,-infinity,infinity,ghost.end.x,lx,hx);
        state.y=alignMath::axis(box,state.bounds,state.orderY,true,unit,ghostSnap.y,-infinity,infinity,ghost.end.y,ly,hy);
        // Fused rings and chemical arrow attachments keep native connectivity.
        // They still show guides when their constrained geometry already aligns.
        if(target&&!endpoint) {
            if(std::abs(state.x.delta)>.25*unit)state.x={};
            if(std::abs(state.y.delta)>.25*unit)state.y={};
            state.x.delta=state.y.delta=0;
        }
        Point shift{state.x.delta,state.y.delta,0};
        auto shiftedBox=[&] {
            return endpoint&&!arrow?endpointBounds(ghost.start,{ghost.end.x+shift.x,ghost.end.y+shift.y,ghost.end.z}):box.moved(shift.x,shift.y);
        };
        if(!alignMath::fitsLane(state.x,shiftedBox(),state.bounds,false,unit)){state.x={};shift.x=0;}
        if(!alignMath::fitsLane(state.y,shiftedBox(),state.bounds,true,unit)){state.y={};shift.y=0;}
        if((endpoint||arrow)&&fn<Obj(*)(Obj,const Point*,Obj)>(0x7107c0)(page,&ghost.end,target)) {
            state.x={};state.y={};shift={};
        }
        if(!target&&!arrow) {
            for(const auto& r:index.rays)state.rays.push_back(r.ray);
            state.ray=ghostSnap.ray;chooseRay(state,box,unit,shift);
        }
        state.drawingBounds=shiftedBox();ghostSnap.x=state.x;ghostSnap.y=state.y;ghostSnap.ray=state.ray;
        if(shift.x||shift.y) {
            auto artwork=std::make_shared<GhostArtwork>(*ghost.artwork);
            const double dx=ghost.end.x-ghost.start.x,dy=ghost.end.y-ghost.start.y,den=dx*dx+dy*dy;
            const double real=endpoint&&den>0?((dx+shift.x)*dx+(dy+shift.y)*dy)/den:1;
            const double imag=endpoint&&den>0?((dy+shift.y)*dx-(dx+shift.x)*dy)/den:0;
            auto move=[&](Point& p) {
                if(endpoint){const double x=p.x-ghost.start.x,y=p.y-ghost.start.y;
                    p.x=ghost.start.x+x*real-y*imag;p.y=ghost.start.y+x*imag+y*real;
                }else {p.x+=shift.x;p.y+=shift.y;}
            };
            for(auto& line:artwork->strokes){move(line.start);move(line.end);}
            for(auto& path:artwork->paths)for(auto& p:path.points)move(p);
            if(!endpoint) {move(ghost.start);ghost.alignmentOffset=shift;}
            ghost.end.x+=shift.x;ghost.end.y+=shift.y;
            ghost.artwork=std::move(artwork);
        }
        ghost.alignment=makeAlignmentFeedback(state,true);
    }catch(...) {ghostSnap={};}
}
void beginSmartGhostPlacement(Obj page,const GhostBond& ghost,POINT pressed) noexcept {
    if (!patchEnabled(18)) return;
    ghostPlacement={};
    if(onUI()&&installed&&enabled&&page&&ghost.visible) {
        const bool arrow=nativeArrowTool(int(ghost.toolKey>>48));
        if(!ghost.identity||arrow) {
            ghostPlacement.page=page;ghostPlacement.press=ghost.pressPoint;
            ghostPlacement.offset=ghost.identity?Point{}:ghost.alignmentOffset;
            ghostPlacement.tail=ghost.start;ghostPlacement.head=ghost.end;ghostPlacement.arrow=arrow;
            Obj doc=at<Obj>(page,8),port=doc?at<Obj>(doc,0x258):nullptr;
            ghostPlacement.window=port?portWindow(port):nullptr;
            ghostPlacement.tool=int(ghost.toolKey>>48);ghostPlacement.free=!ghost.identity;
            ghostPlacement.cursor=pressed;
            if(!ghostPlacement.window||!ClientToScreen(ghostPlacement.window,&ghostPlacement.cursor))ghostPlacement={};
        }
    }
}
void endSmartGhostPlacement() noexcept {ghostPlacement={};}
bool smartAlignmentPlacementPoint(Obj page,const Point& point,Point& snapped) noexcept {
    if (!patchEnabled(18)) return false;
    if(!ghostPlacement.free||!ghostPlacementHeld(page))return false;
    Obj scale=at<Obj>(page,0x2a8);if(!scale)return false;
    const double unit=at<double>(scale,8),tolerance=unit*.001;
    const Point exact{ghostPlacement.press.x+ghostPlacement.offset.x,
        ghostPlacement.press.y+ghostPlacement.offset.y,point.z};
    // Nested native factories can see an already corrected source point.
    if(std::hypot(point.x-exact.x,point.y-exact.y)<=tolerance)return false;
    // This scope owns a stationary click. Native factories may already have
    // rounded, hit-snapped or translated their temporary source; use the
    // displayed position instead of applying the offset to those new points.
    snapped=exact;return true;
}
bool smartAlignmentGhostArrowEndpoints(Obj page,Point& tail,Point& head) noexcept {
    if (!patchEnabled(18)) return false;
    if(!ghostPlacement.arrow||!ghostPlacementHeld(page))return false;
    tail=ghostPlacement.tail;head=ghostPlacement.head;return true;
}
static void dropAlignedFirst(Obj page,Obj source,const Point* point,Obj* a,Obj* b,char* ca,char* cb,bool alternate) {
    Point corrected{};
    if(!source&&point&&smartAlignmentPlacementPoint(page,*point,corrected))point=&corrected;
    oldDropFirst(page,source,point,a,b,ca,cb,alternate);
}
static short dropAlignedRing(Obj page,Point point,bool a,bool b,bool c) {
    Point corrected{};if(smartAlignmentPlacementPoint(page,point,corrected))point=corrected;
    return oldDropRing(page,point,a,b,c);
}

void installSmartAlignment() {
    if(!supportedStartupHash(reinterpret_cast<HMODULE>(base),"0bf203d7ddf0c700bf9d44fd156425d8df274277fdaea58fa511cfbddc51861a")||
        !supportedStartupHash(GetModuleHandleW(L"ChemDrawUI.dll"),"b9af968d2e75822fc47a4e5a4d2f348ea636804c712763b73b3536a30268a4f2"))return;
    const BYTE prologue[]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x48,0x89,0x70,0x20};
    if(memcmp(reinterpret_cast<void*>(base+0x16b660),prologue,sizeof(prologue)))return;
    // Alignment is on at each launch. An old persisted toggle must not silently
    // disable both reference preparation and all native gesture hooks.
    enabled=true;
    wchar_t option[8]{};if(GetEnvironmentVariableW(L"CHEMDRAW_SMART_ALIGN",option,8))enabled=wcscmp(option,L"0")!=0;
    hook(0x16b660,constrain,oldConstrain);hook(0x16bb50,preview,oldPreview);hook(0x16ca20,finish,oldFinish);
    hook(0x2a8480,arrowSnap,oldArrowSnap);hook(0x320440,graphicSnap,oldGraphicSnap);
    if (patchEnabled(18)) {
    hook(0x3e3570,convertPlacementPoint,oldConvertPoint);
    hook(0x4fa700,setDrawingPoint,oldSetPoint);hook(0x4fa660,setDrawingPortPoint,oldSetPortPoint);
    hook(0x2cdc40,setDrawingAtom,oldSetAtomPos);
    hook(0x4feb40,calcDrawingRing,oldCalcRing);
    hook(0x4fb990,dropAlignedFirst,oldDropFirst);hook(0x4feef0,dropAlignedRing,oldDropRing);
    auto drawingHook=[&](size_t rva,Preview replacement,Preview& original) {
        if(createHook(reinterpret_cast<void*>(resolveDetour(L"ChemDrawUI.dll",uint32_t(rva))),reinterpret_cast<void*>(replacement),
            reinterpret_cast<void**>(&original))!=MH_OK)throw std::runtime_error("Could not install drawing alignment callback");
    };
    drawingHook(0x275360,graphicDrawingMove,oldGraphicMove);
    drawingHook(0x268290,bondDrawingMove,oldBondMove);
    drawingHook(0x250730,genericDrawingMove,oldGenericMove);
    drawingHook(0x26dab0,chainDrawingMove,oldChainMove);
    if(createHook(reinterpret_cast<void*>(resolveDetour(L"ChemDrawUI.dll",0x287ea0)),reinterpret_cast<void*>(ringDrawingMove),
        reinterpret_cast<void**>(&oldRingMove))!=MH_OK)throw std::runtime_error("Could not install ring alignment");
    if(createHook(reinterpret_cast<void*>(resolveDetour(L"ChemDrawUI.dll",0x287c30)),reinterpret_cast<void*>(finishDrawingRing),
        reinterpret_cast<void**>(&oldRingFinish))!=MH_OK)throw std::runtime_error("Could not install ring placement alignment");
    }
    keyboardHook=SetWindowsHookExW(WH_GETMESSAGE,shortcuts,module,uiThread);
    if(!keyboardHook)throw std::runtime_error("Could not install alignment shortcut");
    installed=true;
}
void removeSmartAlignment() {if(keyboardHook)UnhookWindowsHookEx(keyboardHook);keyboardHook=nullptr;installed=false;endSmartAlignment();}
}

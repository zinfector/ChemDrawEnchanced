#include "runtime.hpp"
#include <intrin.h>
#include <numbers>

namespace cd {
using Endpoint=Point*(*)(Point*,const Point*,char*,bool,Obj);
static Endpoint oldEndpoint{};
constexpr double tau=std::numbers::pi*2.0;
struct Candidates { std::array<Point,256> points{};size_t count{}; };
struct Choice { Obj atom{};uint64_t id{};Point start{},direction{};uint64_t toolKey{}; };
struct PlacementState { Choice choice{};Candidates candidates{}; };
static std::unordered_map<HWND,PlacementState> placements;
static thread_local HWND mousePlacementWindow{};
static thread_local Choice mouseChoice{};
static thread_local bool mouseChoiceFrozen{};

static HWND atomWindow(Obj atom) {
    Obj page=atom?at<Obj>(atom,0x60):nullptr,doc=page?at<Obj>(page,8):nullptr;
    Obj port=doc?at<Obj>(doc,0x258):nullptr;
    return port?fn<HWND(*)(Obj)>(0x624a70)(port):nullptr;
}
uint64_t bondToolKey() {
    return (uint64_t(uint16_t(fn<short(*)()>(0x4f4bb0)()))<<32)|(uint64_t(3)<<48);
}
bool ordinaryBondTool() {
    if(fn<int(*)()>(0x4f4ba0)()!=3) return false;
    const int order=fn<int(*)()>(0x4fad40)();
    const auto type=uint8_t(fn<char(*)(bool)>(0x4fad80)(false));
    return ((order==1&&(type==1||type==3))||(order==2&&(type&0xc4)==0)||(order==3&&type==1))&&
        !((GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_CONTROL)|GetAsyncKeyState(VK_MENU))&0x8000);
}
static bool finite(Point p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
static double angleDistance(double a,double b) { return std::abs(std::remainder(a-b,tau)); }
static bool sameSource(const Choice& c,Obj atom,Point start) {
    return c.atom==atom&&c.id==at<uint64_t>(atom,0xb0)&&c.toolKey==bondToolKey()&&
        c.start.x==start.x&&c.start.y==start.y&&c.start.z==start.z;
}
static Candidates directions(Obj atom,Point start,Point native,Point mouse) {
    Candidates result{};
    const double length=std::hypot(native.x-start.x,native.y-start.y);
    if(!finite(start)||!finite(native)||!std::isfinite(length)||length<=0) return result;
    struct Ray { double x,y,angle; };
    std::array<Ray,64> rays{};size_t count{};
    auto first=at<Obj*>(atom,0x120),last=at<Obj*>(atom,0x128);
    const auto a=reinterpret_cast<uintptr_t>(first),b=reinterpret_cast<uintptr_t>(last);
    if(b<a||(b-a)%sizeof(Obj)||(b-a)/sizeof(Obj)>rays.size()||(!first&&b!=a)) return result;
    for(auto bond=first;bond!=last;++bond) {
        if(!*bond) return {};
        // Native cumulative multiple-bond rules may require linear growth.
        // Leave triple/aromatic/special orders entirely on their native path.
        const int order=at<int>(*bond,0xdc);if(order!=1&&order!=2) return {};
        Obj left=at<Obj>(*bond,0xc0),right=at<Obj>(*bond,0xc8);
        Obj other=left==atom?right:(right==atom?left:nullptr);if(!other) return {};
        Point p{};p=*vf<Point*(*)(Obj,Point*)>(other,0x1f0)(other,&p);
        const double dx=p.x-start.x,dy=p.y-start.y,d=std::hypot(dx,dy);
        if(!finite(p)||!std::isfinite(d)||d<=0) return {};
        double angle=std::atan2(dy,dx);if(angle<0) angle+=tau;
        rays[count++]={dx/d,dy/d,angle};
    }
    const double step=*reinterpret_cast<const double*>(base+0x8d5748);
    if(!std::isfinite(step)||step<=0||step>std::numbers::pi) return {};
    const bool locked=*reinterpret_cast<const uint8_t*>(base+0xb6dd33)!=0;
    const double tolerance=fn<double(*)(Obj)>(0x327890)(atom);
    const double separation=std::max(locked?step*0.45:0.0,
        std::isfinite(tolerance)&&tolerance>0?std::min(step,std::atan2(tolerance,length)):0.0);
    auto add=[&](Point ray) {
        if(result.count==result.points.size()||!finite(ray)) return;
        Point end{};
        // Includes MSVC's hidden Point return parameter. The three native
        // arguments are the ray endpoint, vertex origin and bond length.
        fn<Point*(*)(Point*,const Point*,const Point*,const double*)>(0x4f9980)(
            &end,&ray,&start,&length);
        if(!finite(end)) return;
        const double angle=std::atan2(end.y-start.y,end.x-start.x);
        for(size_t i=0;i<count;++i) if(angleDistance(angle,rays[i].angle)<=separation) return;
        for(size_t i=0;i<result.count;++i)
            if(std::hypot(end.x-result.points[i].x,end.y-result.points[i].y)<=length*1.0e-8) return;
        result.points[result.count++]=end;
    };
    auto addAngle=[&](double angle) {
        add({start.x+std::cos(angle)*length,start.y+std::sin(angle)*length,start.z});
    };
    add(native);
    if(count==1) {
        // Reflect the exact native chain extension across its parent axis.
        // This retains native chain-angle/growth-mode selection and length.
        const double dx=native.x-start.x,dy=native.y-start.y;
        const double dot=dx*rays[0].x+dy*rays[0].y;
        add({start.x+2*dot*rays[0].x-dx,start.y+2*dot*rays[0].y-dy,start.z});
    } else if(count>1) {
        const double chain=fn<double(*)(Obj)>(0x327f20)(atom)*std::numbers::pi/180.0*
            std::abs(double(fn<int(*)()>(0x1a9fc0)()));
        if(std::isfinite(chain)) for(size_t i=0;i<count;++i) {
            addAngle(rays[i].angle+chain);addAngle(rays[i].angle-chain);
        }
        std::sort(rays.begin(),rays.begin()+count,[](Ray x,Ray y) { return x.angle<y.angle; });
        // Give every open angular gap a candidate, rather than only the
        // largest gap selected by CalcBestPosition.
        for(size_t i=0;i<count;++i) {
            const double next=i+1<count?rays[i+1].angle:rays[0].angle+tau;
            addAngle((rays[i].angle+next)*0.5);
        }
    } else if(locked) {
        const double slots=std::ceil(tau/step);
        if(slots<=result.points.size()) for(size_t i=0;i<size_t(slots);++i) addAngle(double(i)*step);
    } else {
        const double d=std::hypot(mouse.x-start.x,mouse.y-start.y);
        if(std::isfinite(d)&&d>0) addAngle(std::atan2(mouse.y-start.y,mouse.x-start.x));
    }
    return result;
}
static void selectEndpoint(Point& endpoint,const Point& mouse,Obj atom,bool freeze) {
    if(!onUI()||!ordinaryBondTool()) return;
    const HWND w=atomWindow(atom);if(!w||(freeze&&w!=mousePlacementWindow)) return;
    Point start{};start=*vf<Point*(*)(Obj,Point*)>(atom,0x1f0)(atom,&start);
    if(freeze&&mouseChoiceFrozen&&sameSource(mouseChoice,atom,start)) {
        // A click commit passes the source center after destroying its tracker
        // objects. Its native candidates/growth state can differ from hover.
        // Use the complete frozen endpoint, without rebuilding or re-snapping.
        const Point chosen{start.x+mouseChoice.direction.x,start.y+mouseChoice.direction.y,
            start.z+mouseChoice.direction.z};
        if(finite(chosen)) endpoint=chosen;
        return;
    }
    RectD circle{};if(!vertexHitCircle(atom,circle)) return;
    const double radius=(circle.r-circle.l)*0.5;
    if(!finite(start)||!finite(mouse)||std::hypot(mouse.x-start.x,mouse.y-start.y)>radius) return;
    const auto candidates=directions(atom,start,endpoint,mouse);
    if(!candidates.count) return;
    Choice previous{};
    if(freeze&&mouseChoiceFrozen&&sameSource(mouseChoice,atom,start)) previous=mouseChoice;
    else if(auto it=placements.find(w);it!=placements.end()&&sameSource(it->second.choice,atom,start))
        previous=it->second.choice;
    const double dx=mouse.x-start.x,dy=mouse.y-start.y,distance=std::hypot(dx,dy);
    const bool centered=distance<=radius*0.1;
    const bool latched=freeze&&mouseChoiceFrozen&&sameSource(mouseChoice,atom,start);
    const double requested=previous.atom&&(centered||latched)?
        std::atan2(previous.direction.y,previous.direction.x):std::atan2(dy,dx);
    size_t chosen{};double best=std::numeric_limits<double>::infinity();
    size_t retained=candidates.count;double oldCost{};
    for(size_t i=0;i<candidates.count;++i) {
        const auto p=candidates.points[i];const double angle=std::atan2(p.y-start.y,p.x-start.x);
        // At a fresh exact-center hover, keep the native candidate first.
        const double cost=centered&&!previous.atom?double(i):angleDistance(angle,requested);
        if(cost<best) { best=cost;chosen=i; }
        if(previous.atom&&angleDistance(angle,std::atan2(previous.direction.y,previous.direction.x))<1.0e-7) {
            retained=i;oldCost=cost;
        }
    }
    if(retained<candidates.count&&retained!=chosen) {
        const auto p=candidates.points[retained],q=candidates.points[chosen];
        const double between=angleDistance(std::atan2(p.y-start.y,p.x-start.x),
            std::atan2(q.y-start.y,q.x-start.x));
        const double step=*reinterpret_cast<const double*>(base+0x8d5748);
        if(oldCost-best<std::min(step,between)*0.15) chosen=retained;
    }
    endpoint=candidates.points[chosen];
    const Choice choice{atom,at<uint64_t>(atom,0xb0),start,
        {endpoint.x-start.x,endpoint.y-start.y,endpoint.z-start.z},bondToolKey()};
    // UI-only candidate storage; no native pointers reach the GPU worker.
    try { placements.insert_or_assign(w,PlacementState{choice,candidates}); }
    catch(const std::bad_alloc&) { /* Selection itself needs no allocation. */ }
    if(freeze) { mouseChoice=choice;mouseChoiceFrozen=true; }
}
static Point* mouseEndpoint(Point* out,const Point* mouse,char* attachment,bool alternate,Obj atom) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    if(onUI()&&mousePlacementWindow&&caller>=base+0x4fb990&&caller<base+0x4fbd20&&
        !alternate&&out&&mouse&&attachment&&atom&&mouseChoiceFrozen) {
        RectD circle{};Point start{};
        if(vertexHitCircle(atom,circle)) {
            start=*vf<Point*(*)(Obj,Point*)>(atom,0x1f0)(atom,&start);
            if(sameSource(mouseChoice,atom,start)) {
                const Point chosen{start.x+mouseChoice.direction.x,start.y+mouseChoice.direction.y,start.z+mouseChoice.direction.z};
                if(finite(chosen)) {
                    // Plain unlabeled vertices have no character attachment.
                    // Commit the already displayed endpoint before doing native
                    // growth/collision selection that will only be overwritten.
                    *attachment=-1;*out=chosen;return out;
                }
            }
        }
    }
    Point* result=oldEndpoint(out,mouse,attachment,alternate,atom);
    // ReplaceLeavingGroups also calls this helper. Only intercept the mouse
    // DropFirstBond caller, preserving keyboard and other chemistry operations.
    if(mousePlacementWindow&&caller>=base+0x4fb990&&caller<base+0x4fbd20&&
        !alternate&&out&&mouse&&attachment&&*attachment<0&&atom)
        selectEndpoint(*out,*mouse,atom,true);
    return result;
}
Point* previewBondPlacement(Point* out,const Point* mouse,char* attachment,Obj atom) {
    const auto endpoint=oldEndpoint?oldEndpoint:fn<Endpoint>(0x4fbd20);
    Point* result=endpoint(out,mouse,attachment,false,atom);
    if(patchEnabled(4)&&out&&mouse&&attachment&&*attachment<0&&atom) selectEndpoint(*out,*mouse,atom,false);
    return result;
}
void clearBondPlacement(HWND w) {
    if(!onUI()) return;
    if(w) placements.erase(w);else placements.clear();
}
Obj mouseBondPlacementVertex() { return mouseChoiceFrozen?mouseChoice.atom:nullptr; }
MouseBondPlacementScope::MouseBondPlacementScope(HWND w,UINT message,LPARAM position) {
    if(!patchEnabled(4)||message!=WM_LBUTTONDOWN) return;
    entered=true;previousWindow=mousePlacementWindow;previousAtom=mouseChoice.atom;
    previousId=mouseChoice.id;previousStart=mouseChoice.start;previousDirection=mouseChoice.direction;
    previousToolKey=mouseChoice.toolKey;
    previousFrozen=mouseChoiceFrozen;
    mousePlacementWindow=w;mouseChoice={};mouseChoiceFrozen=false;
    // Freeze preview geometry before clearing hover state. Re-running native
    // growth selection at mouse-down can flip the ghost's chosen direction.
    auto it=placements.find(w);if(it==placements.end()) return;
    const Choice preview=it->second.choice;
    Obj atom=preview.atom;
    Obj page=at<Obj>(atom,0x60),doc=page?at<Obj>(page,8):nullptr;
    Obj scale=at<Obj>(atom,0x68),port=doc?at<Obj>(doc,0x258):nullptr;
    if(!scale||!port||atomWindow(atom)!=w||!ordinaryBondTool()) return;
    const Point mouse{fn<double(*)(Obj,double)>(0x3c5d80)(scale,double(short(LOWORD(position)))+at<int>(port,0xa8)),
        fn<double(*)(Obj,double)>(0x3c5da0)(scale,double(short(HIWORD(position)))+at<int>(port,0xac)),0};
    Point start{};start=*vf<Point*(*)(Obj,Point*)>(atom,0x1f0)(atom,&start);
    RectD circle{};
    if(!sameSource(preview,atom,start)||!vertexHitCircle(atom,circle)||!finite(mouse)||
        std::hypot(mouse.x-start.x,mouse.y-start.y)>(circle.r-circle.l)*0.5) return;
    mouseChoice=preview;
    Point shownStart{},shownEnd{};
    // The renderer may lag the newest hover request by a refresh. Prefer the
    // geometry from its last submitted visible ghost over a queued direction.
    if(displayedBondPlacement(w,preview.id+1,shownStart,shownEnd)&&finite(shownStart)&&finite(shownEnd)&&
        shownStart.x==start.x&&shownStart.y==start.y&&shownStart.z==start.z)
        mouseChoice.direction={shownEnd.x-start.x,shownEnd.y-start.y,shownEnd.z-start.z};
    mouseChoiceFrozen=true;
}
MouseBondPlacementScope::~MouseBondPlacementScope() {
    if(!entered) return;
    mousePlacementWindow=previousWindow;
    mouseChoice={previousAtom,previousId,previousStart,previousDirection,previousToolKey};mouseChoiceFrozen=previousFrozen;
}
void installBondPlacement() { hook(0x4fbd20,mouseEndpoint,oldEndpoint); }
}

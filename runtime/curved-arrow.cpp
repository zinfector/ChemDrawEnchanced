#include "curved-arrow.hpp"
#include "gpu-ui.hpp"
#include "gpu-preview.hpp"
#include "arrow-snap-math.hpp"
#include "arrow-path.hpp"
#include <bcrypt.h>

namespace cd {
namespace {
bool enabled=true,installed{};
uintptr_t uiBase{};
HHOOK shortcutHook{};
struct Query { Obj page{},arrow{};int source{-1},incumbent{-1},selected{-1}; };
thread_local Query* query{};
thread_local bool resolving{};
struct QueryScope {
    Query* previous;
    explicit QueryScope(Query& next):previous(query) { query=&next; }
    ~QueryScope() { query=previous; }
};
struct Gesture { Obj tracker{},page{};HWND window{};int target{-1};bool released{};Point release{}; };
thread_local Gesture gesture;
// IDs are re-resolved on every query; a hover never owns an object pointer.
std::unordered_map<Obj,int> hoverTargets;
using Factory=Obj*(*)(Obj*,Obj,uintptr_t,Point*,uintptr_t,uintptr_t,uint8_t);
Factory oldFactory{};
using Move=void(*)(Obj);
Move oldMove{};
using Finish=short(*)(Obj);
Finish oldFinish{};
using Align=void(*)(Obj,const Point*,const Point*);
Align oldAlign{};
Align oldEndpoints{};
using Angle=void(*)(Obj,double);
Angle oldAngle{};
thread_local Obj angleOwner{};
thread_local double preservedAngle{};
struct EndpointOverride { Obj arrow{};Point head{},tail{}; };
thread_local EndpointOverride exactEndpoints;

bool supportedHash(HMODULE handle,const char* expected) {
    wchar_t path[32768]{};
    if(!GetModuleFileNameW(handle,path,DWORD(std::size(path)))) return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok) ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    std::array<uint8_t,65536> data{};DWORD read{};
    while(ok) {
        if(!ReadFile(file,data.data(),DWORD(data.size()),&read,nullptr)) { ok=false;break; }
        if(!read) break;
        ok=BCryptHashData(hash,data.data(),read,0)>=0;
    }
    std::array<uint8_t,32> digest{};
    if(ok) ok=BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0;
    if(hash) BCryptDestroyHash(hash);
    if(algorithm) BCryptCloseAlgorithmProvider(algorithm,0);
    CloseHandle(file);
    char hex[65]{};
    for(size_t i=0;i<digest.size();++i) sprintf_s(hex+2*i,3,"%02x",unsigned(digest[i]));
    return ok&&strcmp(hex,expected)==0;
}
Obj byID(Obj page,int id) { return id>=0?fn<Obj(*)(Obj,int)>(0x712f80)(page,id):nullptr; }
bool eligible(Obj object,const Point& point) {
    return object&&at<uint8_t>(object,0x34)&&vf<bool(*)(Obj)>(object,0xb8)(object)&&
        vf<bool(*)(Obj,int,short,const Point*,bool,bool)>(object,0x2c0)(object,0x27,-1,&point,false,false);
}
struct BondInk {Point first{},last{};double width{};};
bool doubleBondInk(Obj bond,std::vector<BondInk>& ink) {
    if(!bond||at<uintptr_t>(bond,0)!=base+0x8babc0||at<int>(bond,0xdc)!=2) return false;
    // The same lazy vector consumed by CDBond::DrawBondPoly. Its calculator
    // owns spacing, centered/inside placement, bold widths and clipped ends.
    if(!at<uint8_t>(bond,0x4b0)) fn<void(*)(void*)>(0xde530)(static_cast<std::byte*>(bond)+0x498);
    const auto first=at<uintptr_t>(bond,0x498),last=at<uintptr_t>(bond,0x4a0);
    if(!first||last<=first||(last-first)%0x58||(last-first)/0x58>64) return false;
    for(auto p=first;p<last;p+=0x58) {
        const Point a=at<Point>(reinterpret_cast<Obj>(p),0),b=at<Point>(reinterpret_cast<Obj>(p),0x18);
        const double width=at<double>(reinterpret_cast<Obj>(p),0x30);
        if(!arrowMath::finite({a.x,a.y})||!arrowMath::finite({b.x,b.y})||
            !std::isfinite(a.z)||!std::isfinite(b.z)||!std::isfinite(width)||width<0) continue;
        ink.push_back({a,b,width});
    }
    return ink.size()>=2;
}
double unitsPerDip(Obj page) {
    Obj scale=at<Obj>(page,0x2a8),doc=at<Obj>(page,8),port=doc?at<Obj>(doc,0x258):nullptr;
    UINT dpi=port?GetDpiForWindow(portWindow(port)):0;if(!dpi) dpi=96;
    return scale?at<double>(scale,8)*double(dpi)/96:0;
}
Obj nearest(Query& q,const Point& point) {
    const double unit=unitsPerDip(q.page);
    if(!std::isfinite(unit)||unit<=0||!arrowMath::finite({point.x,point.y})) return nullptr;
    std::vector<Obj> objects;
    if(!spatialCandidatesInRadius(q.page,point,14*unit,objects)) return nullptr;
    std::vector<arrowMath::Candidate> candidates;candidates.reserve(objects.size());
    for(Obj o:objects) {
        if(o==q.arrow||at<int>(o,0x24)==q.source) continue;
        const uintptr_t table=at<uintptr_t>(o,0);
        const bool atom=table==base+0x8b34f8,bond=table==base+0x8babc0;
        if((!atom&&!bond)||!eligible(o,point)) continue;
        double distance{};
        if(atom) {
            const Point& p=at<Point>(o,0x1f8);
            distance=std::hypot(point.x-p.x,point.y-p.y);
            // Labeled atoms can be hit on their visible text as well as their vertex.
            if(at<Obj>(o,0xc8)) {
                RectD r{};vf<RectD*(*)(Obj,RectD*)>(o,0x1d0)(o,&r);
                if(valid(r)) distance=std::min(distance,std::hypot(point.x-std::clamp(point.x,r.l,r.r),
                    point.y-std::clamp(point.y,r.t,r.b)));
            }
        } else {
            Obj a=at<Obj>(o,0xc0),b=at<Obj>(o,0xc8);if(!a||!b) continue;
            const Point& first=at<Point>(a,0x1f8);const Point& last=at<Point>(b,0x1f8);
            distance=arrowMath::segmentDistance({point.x,point.y},{first.x,first.y},{last.x,last.y});
            std::vector<BondInk> ink;
            if(doubleBondInk(o,ink)) {
                distance=std::numeric_limits<double>::infinity();
                for(const auto& stroke:ink) distance=std::min(distance,std::max(0.0,
                    arrowMath::segmentDistance({point.x,point.y},{stroke.first.x,stroke.first.y},
                        {stroke.last.x,stroke.last.y})-stroke.width*.5));
            }
        }
        // The bond penalty must exceed the switch margin: otherwise a retained
        // adjacent bond beats its atom even when the cursor reaches that vertex.
        candidates.push_back({at<int>(o,0x24),distance/unit,bond?4.0:0.0});
    }
    q.selected=arrowMath::choose(candidates,q.incumbent);
    return byID(q.page,q.selected);
}
HWND windowForPage(Obj page) {
    Obj doc=page?at<Obj>(page,8):nullptr,port=doc?at<Obj>(doc,0x258):nullptr;
    return port?portWindow(port):nullptr;
}
Obj* factory(Obj* result,Obj page,uintptr_t p3,Point* point,uintptr_t p5,uintptr_t p6,uint8_t p7) {
    if(!onUI()||!enabled) return oldFactory(result,page,p3,point,p5,p6,p7);
    const auto previous=hoverTargets.find(page);
    Query q{page,nullptr,-1,previous==hoverTargets.end()?-1:previous->second};QueryScope scope(q);
    Obj* returned=oldFactory(result,page,p3,point,p5,p6,p7);
    if(result&&result[0]) {gesture={result[0],page,windowForPage(page)};arrowPathDrawingStarted();}
    return returned;
}
void move(Obj tracker) {
    if(!onUI()||!enabled) { oldMove(tracker);return; }
    Obj page=at<Obj>(tracker,8),arrow=at<Obj>(tracker,0x138);
    if(gesture.tracker!=tracker) gesture={tracker,page,windowForPage(page)};
    Query q{page,arrow,at<int>(arrow,0x1f0),gesture.target};QueryScope scope(q);
    oldMove(tracker);
    gesture.target=at<int>(arrow,0x1f4);
    updateArrowTargetHighlight(at<Obj>(page,8),byID(page,gesture.target));
}
void setAngle(Obj arrow,double value) {
    // Substitution occurs before native SetStartAndTail, within its undo and
    // fixed-angle guards. Restoring the angle afterward would corrupt arc geometry.
    double native{};
    if(onUI()&&nativeArrowAngle(arrow,native)) value=native;
    else if(onUI()&&arrow==angleOwner) value=preservedAngle;
    oldAngle(arrow,value);
}
Point linkedAnchor(Obj object,const Point& fallback) {
    if(!object) return fallback;
    const uintptr_t table=at<uintptr_t>(object,0);
    Point point=fallback;
    if(table==base+0x8b34f8) point=at<Point>(object,0x1f8);
    else if(table==base+0x8babc0) {
        Obj first=at<Obj>(object,0xc0),last=at<Obj>(object,0xc8);
        if(first&&last) {
            const Point& a=at<Point>(first,0x1f8);const Point& b=at<Point>(last,0x1f8);
            point={(a.x+b.x)*0.5,(a.y+b.y)*0.5,(a.z+b.z)*0.5};
        }
    }
    return arrowMath::finite({point.x,point.y})&&std::isfinite(point.z)?point:fallback;
}
#include "native-arrow-tools.inc"
void setEndpoints(Obj arrow,const Point* head,const Point* tail) {
    if(onUI()&&exactEndpoints.arrow==arrow) {
        // Keep the native attachment IDs, but leave a small visible clearance
        // along the arc's tangents inside native alignment's undo guards.
        Point visibleTail=exactEndpoints.tail,visibleHead=exactEndpoints.head;
        insetArrowEndpoints(arrow,visibleTail,visibleHead,at<double>(arrow,0x130));
        if(!setMirroredArrowEndpoints(arrow,&visibleHead,&visibleTail)) oldEndpoints(arrow,&visibleHead,&visibleTail);
    } else if(!onUI()||!enabled||flippingArrow==arrow||
        (at<int>(arrow,0x1f0)<0&&at<int>(arrow,0x1f4)<0)||!setMirroredArrowEndpoints(arrow,head,tail))
        oldEndpoints(arrow,head,tail);
}
void align(Obj arrow,const Point* source,const Point* target) {
    if(onUI()&&flippingArrow==arrow) return;
    const Obj previous=angleOwner;const double old=preservedAngle;
    const EndpointOverride previousEndpoints=exactEndpoints;
    const double current=at<double>(arrow,0x130);
    const bool linked=onUI()&&enabled&&(at<int>(arrow,0x1f0)!=-1||at<int>(arrow,0x1f4)!=-1);
    double stored{};const bool standard=standardArrowAttachments(arrow);
    const bool explicitBend=!standard&&storedArrowAngle(arrow,stored);
    if(linked&&(standard||explicitBend||(std::isfinite(current)&&current!=0))) {
        angleOwner=arrow;preservedAngle=explicitBend?stored:current;
    }
    Point tail=*source,head=*target;
    if(linked) {
        Obj page=at<Obj>(arrow,0x60);
        if(page) {
            tail=linkedAnchor(byID(page,at<int>(arrow,0x1f0)),tail);
            head=linkedAnchor(byID(page,at<int>(arrow,0x1f4)),head);
        }
        exactEndpoints={arrow,head,tail};
    }
    struct Restore {
        Obj owner;double value;EndpointOverride endpoints;
        ~Restore(){angleOwner=owner;preservedAngle=value;exactEndpoints=endpoints;}
    } restore{previous,old,previousEndpoints};
    oldAlign(arrow,&tail,&head);
}
Point center(Obj o,size_t slot) {
    RectD r{};vf<RectD*(*)(Obj,RectD*)>(o,slot)(o,&r);
    return {(r.l+r.r)*0.5,(r.t+r.b)*0.5,0};
}
void finalTarget(Obj tracker,const Point& point) {
    Obj page=at<Obj>(tracker,8),arrow=at<Obj>(tracker,0x138);
    Query q{page,arrow,at<int>(arrow,0x1f0),gesture.target};QueryScope scope(q);
    Obj target=fn<Obj(*)(Obj,const Point*,Obj)>(0x711500)(page,&point,arrow);
    if(!eligible(target,point)||at<int>(target,0x24)==q.source) { at<int>(arrow,0x1f4)=-1;return; }
    Obj source=byID(page,q.source);if(!source) { at<int>(arrow,0x1f4)=-1;return; }
    at<int>(arrow,0x1f4)=at<int>(target,0x24);
    vf<void(*)(Obj,bool)>(arrow,0x4a0)(arrow,false);
    // CDColor is a native 24-byte return-by-value object (hidden RDX result).
    std::array<std::byte,24> color{};
    fn<void(*)(Obj,void*)>(0x14e380)(at<Obj>(page,8),color.data());
    vf<void(*)(Obj,const void*)>(arrow,0x320)(arrow,color.data());
    const Point tail=center(source,0x1d0),head=center(target,0x1c8);
    fn<Align>(0x296490)(arrow,&tail,&head);
}
short finish(Obj tracker) {
    if(!onUI()||!enabled) return oldFinish(tracker);
    Obj arrow=at<Obj>(tracker,0x138);
    // Never draw into the work port here: native tracking has already retired it.
    if(gesture.tracker==tracker&&gesture.released) finalTarget(tracker,gesture.release);
    else if(gesture.tracker==tracker) at<int>(arrow,0x1f4)=-1; // Capture loss is cancellation.
    const int source=at<int>(arrow,0x1f0),target=at<int>(arrow,0x1f4);
    const double angle=at<double>(arrow,0x130);
    gesture={};
    const short result=oldFinish(tracker); // Native deletion / UndoCreateObject owner.
    if(target>=0) rememberEditableArrow(arrow);
    char status[256]{};sprintf_s(status,"Smart arrows enabled. Last gesture: %s; source=%d target=%d angle=%.6f.\r\n",
        target>=0?"committed":"cancelled",source,target,angle);writeArrowStatus(status);
    return result;
}
template<class F> void uiHook(size_t rva,F replacement,F& original) {
    const auto result=MH_CreateHook(reinterpret_cast<void*>(resolveDetour(L"ChemDrawUI.dll",uint32_t(rva))),reinterpret_cast<void*>(replacement),
        reinterpret_cast<void**>(&original));
    if(result!=MH_OK) throw std::runtime_error("Could not install native electron-arrow tracker hook");
}
LRESULT CALLBACK shortcutMessages(int code,WPARAM removal,LPARAM value) {
    if(code>=0&&removal==PM_REMOVE&&value) {
        auto& message=*reinterpret_cast<MSG*>(value);
        // MFC/CLR accelerator translation runs before the drawing-window proc.
        // Intercept retrieved keys on this UI thread before either consumes them.
        if(message.hwnd&&(arrowPathQueuedInput(message)||arrowPathShortcut(message.hwnd,message.message,message.wParam,message.lParam)||
            arrowShortcut(message.hwnd,message.message,message.wParam,message.lParam))) {
            message.message=WM_NULL;message.wParam=0;message.lParam=0;
        }
    }
    return CallNextHookEx(shortcutHook,code,removal,value);
}
}
Point arrowAttachmentPoint(Obj target,Point anchor,Point outward,double clearance) {
    const double length=std::hypot(outward.x,outward.y);
    if(!onUI()||!target||!at<uint8_t>(target,0x34)||!std::isfinite(length)||length<=1e-8||
        !std::isfinite(clearance)||clearance<0) return anchor;
    outward={outward.x/length,outward.y/length,0};
    std::vector<BondInk> ink;
    if(doubleBondInk(target,ink)) {
        Obj a=at<Obj>(target,0xc0),b=at<Obj>(target,0xc8);
        if(a&&b) {
            const Point p=at<Point>(a,0x1f8),q=at<Point>(b,0x1f8);
            const double bondLength=std::hypot(q.x-p.x,q.y-p.y);
            if(std::isfinite(bondLength)&&bondLength>1e-8) {
                const Point normal{-(q.y-p.y)/bondLength,(q.x-p.x)/bondLength,0};
                const double side=normal.x*outward.x+normal.y*outward.y;
                double best=-std::numeric_limits<double>::infinity();Point selected=anchor;
                for(const auto& stroke:ink) {
                    Point middle{(stroke.first.x+stroke.last.x)*.5,(stroke.first.y+stroke.last.y)*.5,
                        (stroke.first.z+stroke.last.z)*.5};
                    const double offset=normal.x*(middle.x-p.x)+normal.y*(middle.y-p.y);
                    // Select the outer visible stroke on the departure side.
                    // Parallel departures choose the stroke nearest the anchor.
                    const double score=std::abs(side)>1e-6?
                        (side>0?offset:-offset)+stroke.width*.5:
                        -std::hypot(middle.x-anchor.x,middle.y-anchor.y);
                    if(score>best) {
                        best=score;selected=middle;
                        if(std::abs(side)>1e-6) {
                            const double edge=(side>0?1:-1)*stroke.width*.5;
                            selected.x+=normal.x*edge;selected.y+=normal.y*edge;
                        }
                    }
                }
                anchor=selected;
            }
        }
    }
    return {anchor.x+outward.x*clearance,anchor.y+outward.y*clearance,anchor.z};
}
bool regularArrowEndpoints(Obj arrow,Point& tail,Point& head) {
    if(!onUI()||!regular.running||regular.arrow!=arrow||
        (at<int>(arrow,0x1f0)<0&&at<int>(arrow,0x1f4)<0)) return false;
    const Point moving=regular.incumbent>=0?regular.moving:at<Point>(regular.tracker,0x50);
    tail=regular.tail?moving:regular.fixed;head=regular.tail?regular.fixed:moving;
    return arrowMath::finite({tail.x,tail.y})&&arrowMath::finite({head.x,head.y});
}
bool regularArrowPreview(Obj arrow) {return onUI()&&regular.running&&regular.arrow==arrow;}
bool regularArrowTracker(Obj tracker) {return onUI()&&enabled&&regular.tracker==tracker;}
bool arrowChemistryTarget(Obj page,const Point& point) {
    return arrowSnapTarget(page,point)!=nullptr;
}
Obj arrowSnapTarget(Obj page,const Point& point) {
    if(!onUI()||!installed||!enabled||!page) return nullptr;
    Query press{page,nullptr};return nearest(press,point);
}
void maintainArrowAttachments(Obj arrow) {
    if(!onUI()||!installed||!enabled||maintaining||undoArrow||flippingArrow==arrow||regularArrowPreview(arrow)) return;
    // Guard the complete operation, including native getters and tag access.
    // Those getters may enter EnsureValid before returning their coordinates.
    maintaining=true;
    struct RestoreMaintenance {~RestoreMaintenance(){maintaining=false;}} restore;
    if(!standardArrowAttachments(arrow)||!at<uint8_t>(arrow,0x34)) return;
    const int source=at<int>(arrow,0x1f0),target=at<int>(arrow,0x1f4);
    if(source<0&&target<0) return;
    Point originalTail{},originalHead{};if(!arrowWorldEndpoints(arrow,originalTail,originalHead)) return;
    Obj page=at<Obj>(arrow,0x60);
    Point tail=linkedAnchor(byID(page,source),originalTail),head=linkedAnchor(byID(page,target),originalHead);
    insetArrowEndpoints(arrow,tail,head,at<double>(arrow,0x130));
    const double tolerance=std::max(unitsPerDip(page)*1e-5,1e-7);
    if(std::hypot(tail.x-originalTail.x,tail.y-originalTail.y)<=tolerance&&
        std::hypot(head.x-originalHead.x,head.y-originalHead.y)<=tolerance) return;
    {UndoDisabled off(arrow);fn<Align>(0x2a8260)(arrow,&head,&tail);}
}
bool smartArrowHit(Obj page,const Point* point,Obj exclude,Obj& result) {
    if(!onUI()||!installed||!enabled||!page||!point||resolving) return false;
    Query hover{page,nullptr};
    Query* active=query;
    if(!active) {
        const int tool=fn<int(*)()>(0x4f4ba0)();
        if(exclude||trackingDepth||!nativeArrowTool(tool)) return false;
        if(auto found=hoverTargets.find(page);found!=hoverTargets.end()) hover.incumbent=found->second;
        active=&hover;
    }
    if(active->page!=page||active->arrow!=exclude) return false;
    resolving=true;
    struct ResolveEnd { ~ResolveEnd(){resolving=false;} } end;
    try {
        result=nearest(*active,*point);
        if(active==&hover) hoverTargets.insert_or_assign(page,result?at<int>(result,0x24):-1);
    } catch(const std::bad_alloc&) { return false; }
    // Native orbital hits remain available when no atom/bond is in snap range.
    return result!=nullptr;
}
void forgetArrowPage(Obj page) {
    forgetArrowPaths(page);
    hoverTargets.erase(page);if(gesture.page==page) gesture={};if(regular.page==page) regular={};
}
bool cancelArrowTracking(HWND window) {
    const bool nativeCancelled=cancelArrowPathTracking(window);
    if(regular.tracker&&regular.window==window) {regular.cancelled=true;return true;}
    if(!gesture.tracker||gesture.window!=window) return nativeCancelled;
    gesture.released=false;return true;
}
void arrowMouseReleased(HWND window,const MSG& msg) {
    const bool ordinary=regular.tracker&&regular.window==window;
    if(!ordinary&&(!gesture.tracker||gesture.window!=window)) return;
    Obj trackingPage=ordinary?regular.page:gesture.page;
    if(ordinary) regular.released=false;else gesture.released=false;
    Obj doc=at<Obj>(trackingPage,8),port=doc?at<Obj>(doc,0x258):nullptr;
    Obj page=doc?mainPage(doc):nullptr;if(!port||!page) return;
    POINT client=msg.pt;if(!ScreenToClient(window,&client)) return;
    struct PointI { int x,y,z; } cursor{client.x+at<int>(port,0xa8),client.y+at<int>(port,0xac),0};
    Point internal{};
    Obj actual=fn<Obj(*)(Obj,const PointI*,Point*,bool)>(0x3e3570)(page,&cursor,&internal,false);
    if(actual!=trackingPage||!arrowMath::finite({internal.x,internal.y})) return;
    if(ordinary) {regular.release=internal;regular.released=true;}
    else {gesture.release=internal;gesture.released=true;}
}
bool arrowShortcut(HWND window,UINT message,WPARAM key,LPARAM flags) {
    if(!installed||trackingDepth||(message!=WM_KEYDOWN&&message!=WM_SYSKEYDOWN)||key!='E'||
        !(GetKeyState(VK_CONTROL)&0x8000)||!(GetKeyState(VK_MENU)&0x8000)) return false;
    if(flags&(1LL<<30)) return true;
    if(GetKeyState(VK_SHIFT)&0x8000) enabled=!enabled;
    else {
        enabled=true;
        if(fn<int(*)()>(0x4f4ba0)()!=1) fn<void(*)(int,short)>(0x4f70c0)(1,0);
    }
    hoverTargets.clear();clearHighlights();bumpGeneration();invalidateCanvas(window);
    writeArrowStatus(enabled?"Snapping enabled for the native arrow palette. Ctrl+Alt+Shift+E toggles enhancements.\r\n":
        "Arrow snapping disabled; native arrow tools remain available.\r\n");
    return true;
}
void installCurvedArrows() {
    auto ui=GetModuleHandleW(L"ChemDrawUI.dll");
    if(!ui||!supportedHash(reinterpret_cast<HMODULE>(base),"0bf203d7ddf0c700bf9d44fd156425d8df274277fdaea58fa511cfbddc51861a")||
        !supportedHash(ui,"b9af968d2e75822fc47a4e5a4d2f348ea636804c712763b73b3536a30268a4f2")) {
        writeArrowStatus("Inactive: exact supported ChemDrawBase/ChemDrawUI hashes did not match or UI module not loaded.\r\n");
        return;
    }
    wchar_t option[8]{};GetEnvironmentVariableW(L"CHEMDRAW_SMART_ARROWS",option,DWORD(std::size(option)));
    enabled=wcscmp(option,L"0")!=0;
    uiBase=reinterpret_cast<uintptr_t>(ui);
    uiHook(0x25edc0,factory,oldFactory);uiHook(0x25d3d0,move,oldMove);uiHook(0x25b3d0,finish,oldFinish);
    uiHook(0x25e1c0,regularFactory,oldRegularFactory);uiHook(0x25a3c0,regularCtor,oldRegularCtor);
    uiHook(0x25dcd0,arrowPress,oldArrowPress);uiHook(0x25f3f0,otherArrowSnap,oldOtherArrowSnap);
    uiHook(0x25cd40,regularMove,oldRegularMove);uiHook(0x25b270,regularFinish,oldRegularFinish);
    uiHook(0x25af20,regularDestructor,oldRegularDestructor);
    hook(0x295af0,undoCtor,oldUndoCtor);hook(0x2a6680,undoExecute,oldUndoExecute);
    hook(0x2961f0,undoDestructor,oldUndoDestructor);
    hook(0x29e970,arrowFlip,oldArrowFlip);
    hook(0x296490,align,oldAlign);hook(0x2a6c00,setAngle,oldAngle);
    hook(0x2a8260,setEndpoints,oldEndpoints);
    installArrowPaths();
    installed=true;
    shortcutHook=SetWindowsHookExW(WH_GETMESSAGE,shortcutMessages,module,uiThread);
    if(!shortcutHook) throw std::runtime_error("Could not install smart-arrow keyboard message hook");
    writeArrowStatus(enabled?"Revision 94: curved ghost sweep matches native arrows, including its head tangent and attachment clearance; native arrow resize handles restored in arrow mode and retained in transparent resize previews; straight-shaft sagitta follows the pointer; double-bond snapping and endpoint clearance use native visible stroke geometry; arrow palette ID corrected; transparent arrow tracking overlays and native atom/bond target highlights; native arrow, cyclopentadiene and benzene cursor ghosts; atom/bond presses start fresh arrows; shaft editing requires a real shaft hit; flipped arcs rebuild attachment clearance in the mirrored plane; attachment maintenance runs after native validation and is protected from re-entry; snapping, native attachments and endpoint clearance integrated into the normal arrow palette. Native arrow subtypes and free drawing retained. Drag endpoints to reattach; select an arc and drag its midpoint. Double-click a shaft to edit its native Pen path; S toggles smooth/corner; Delete removes an interior knot. Native undo includes attachment changes.\r\n":
        "Smart arrows installed, disabled by CHEMDRAW_SMART_ARROWS=0.\r\n");
}
void removeArrowShortcuts() {
    clearNativeArrowEditing();
    if(shortcutHook) { UnhookWindowsHookEx(shortcutHook);shortcutHook=nullptr; }
    installed=false;
    regular={};attachmentUndo.clear();
}
}

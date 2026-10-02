#include "smart-align.hpp"
#include "smart-align-math.hpp"
#include "arrow-path.hpp"
#include <bcrypt.h>
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
using CalcRing=void(*)(Obj,bool);
SetPoint oldSetPoint{},oldSetAtomPos{};
CalcRing oldCalcRing{};
Preview oldRingMove{};
enum class Drawing {None,Graphic,Arrow,ElectronArrow,Bond,Ring,Generic,Chain};
bool installed{},enabled=true;
HHOOK keyboardHook{};
struct Node {Node *left,*parent,*right;uint8_t color,nil,padding[6];Obj object;};
struct Reference {Obj object{};Box bounds{};};
struct Gesture {
    Obj tracker{},page{},doc{};HWND window{};
    bool eligible{},moving{},finishing{},cancelled{},released{},nativeSnap{},inOriginal{},resolved{};
    Point down{},previousEffective{},effective{};
    Box initial{};
    Drawing drawing{Drawing::None};
    Box drawingBounds{};
    Point ringShift{};
    bool settingPoint{},ringActive{};
    Relation x{},y{};
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
struct CachedRoot {Reference reference;std::vector<CachedMember> members;};
struct PageAlignmentIndex {
    Node* head{};size_t count{};uint64_t revision{};bool ready{};
    std::vector<CachedRoot> roots;
    std::unordered_map<Obj,size_t> rootForObject;
    std::vector<int> drawOrder,orderX,orderY;
};
thread_local std::unordered_map<Obj,PageAlignmentIndex> pageIndices;
thread_local bool warmingIndex{};
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
bool supportedHash(HMODULE moduleHandle,const char* expected) {
    wchar_t path[32768]{};if(!GetModuleFileNameW(moduleHandle,path,DWORD(std::size(path))))return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    bool ok=BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok)ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    std::array<BYTE,65536> bytes{};DWORD count{};
    while(ok) {if(!ReadFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr)){ok=false;break;}
        if(!count)break;ok=BCryptHashData(hash,bytes.data(),count,0)>=0;}
    std::array<BYTE,32> digest{};if(ok)ok=BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);CloseHandle(file);
    char hex[65]{};for(size_t i=0;i<digest.size();++i)sprintf_s(hex+i*2,3,"%02x",unsigned(digest[i]));
    return ok&&strcmp(hex,expected)==0;
}
bool nativeTracker(Obj tracker) {
    if(!tracker)return false;
    const uintptr_t table=at<uintptr_t>(tracker,0);
    const auto ui=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ChemDrawUI.dll"));
    return table==base+0x8a3820||(ui&&table==ui+0x428cb8);
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
    default:return Drawing::None;
    }
}
Box inkBounds(Obj o) {
    RectD r{};vf<RectD*(*)(Obj,RectD*)>(o,0x1d8)(o,&r);return {r.l,r.t,r.r,r.b};
}
Obj rootOf(Obj o,Obj page) {
    for(unsigned n=0;n<32;++n) {
        Obj parent=at<Obj>(o,0x28);
        if(!parent||parent==o||at<Obj>(parent,0x60)!=page)return o;
        o=parent;
    }
    return nullptr;
}
void clearRelations() {gesture.x={};gesture.y={};clearGpuAlignment(gesture.doc);}
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
Point drawingPoint(Point point) {
    if(!gesture.eligible||!enabled||gesture.cancelled||gesture.drawing==Drawing::None||
        !std::isfinite(point.x+point.y))return point;
    const Point fixed=fixedDrawingPoint();
    gesture.drawingBounds=endpointBounds(fixed,point);
    const double unit=drawingUnit();
    if(!std::isfinite(unit)||unit<=0)return point;
    if((GetAsyncKeyState(VK_MENU)|GetAsyncKeyState(VK_SHIFT))&0x8000) {gesture.x={};gesture.y={};return point;}
    // An actual chemistry attachment takes precedence over page alignment.
    if(gesture.drawing==Drawing::Bond||gesture.drawing==Drawing::Arrow||gesture.drawing==Drawing::ElectronArrow) {
        Obj hit=fn<Obj(*)(Obj,const Point*,Obj)>(0x7107c0)(gesture.page,&point,nullptr);
        if(hit&&!gesture.movingObjects.contains(hit)) {gesture.x={};gesture.y={};return point;}
        if(gesture.drawing==Drawing::Arrow||gesture.drawing==Drawing::ElectronArrow) {
            Obj arrow=at<Obj>(gesture.tracker,0x138);
            if(arrow&&at<int>(arrow,0x1f4)>=0){gesture.x={};gesture.y={};return point;}
        }
    }
    const Box box=gesture.drawingBounds;
    const double infinity=std::numeric_limits<double>::infinity();
    // A drawing resizes about its press point: the opposite edge is fixed,
    // while its center moves by half of the endpoint's displacement.
    gesture.x=alignMath::axis(box,gesture.bounds,gesture.orderX,false,unit,gesture.x,-infinity,infinity,
        point.x,point.x<fixed.x?1:0,point.x>=fixed.x?1:0);
    gesture.y=alignMath::axis(box,gesture.bounds,gesture.orderY,true,unit,gesture.y,-infinity,infinity,
        point.y,point.y<fixed.y?1:0,point.y>=fixed.y?1:0);
    Point corrected{point.x+gesture.x.delta,point.y+gesture.y.delta,point.z};
    Box final=endpointBounds(fixed,corrected);
    if(!alignMath::fitsLane(gesture.x,final,gesture.bounds,false,unit))gesture.x={};
    final=endpointBounds(fixed,{point.x+gesture.x.delta,point.y+gesture.y.delta,point.z});
    if(!alignMath::fitsLane(gesture.y,final,gesture.bounds,true,unit))gesture.y={};
    corrected={point.x+gesture.x.delta,point.y+gesture.y.delta,point.z};
    gesture.drawingBounds=endpointBounds(fixed,corrected);gesture.effective=corrected;
    return corrected;
}
void setDrawingPoint(Obj tracker,const Point* point) {
    oldSetPoint(tracker,point);
    if(!onUI()||gesture.tracker!=tracker||gesture.settingPoint||gesture.drawing==Drawing::None||
        gesture.drawing==Drawing::Ring||gesture.drawing==Drawing::Bond)return;
    gesture.settingPoint=true;
    try {const Point corrected=drawingPoint(at<Point>(tracker,0x50));oldSetPoint(tracker,&corrected);}
    catch(...){gesture.settingPoint=false;throw;}
    gesture.settingPoint=false;
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
    if((GetAsyncKeyState(VK_MENU)|GetAsyncKeyState(VK_SHIFT))&0x8000){gesture.x={};gesture.y={};return;}
    const double infinity=std::numeric_limits<double>::infinity();
    auto x=alignMath::axis(box,gesture.bounds,gesture.orderX,false,unit,gesture.x,-infinity,infinity,first->x);
    auto y=alignMath::axis(box,gesture.bounds,gesture.orderY,true,unit,gesture.y,-infinity,infinity,first->y);
    // Attached and fused rings retain their native vertices and bond lengths.
    const bool free=at<uint8_t>(gesture.tracker,0x118)&&!at<Obj>(gesture.tracker,0x120)&&!alternate;
    if(!free){if(std::abs(x.delta)>.25*unit)x={};if(std::abs(y.delta)>.25*unit)y={};}
    Point shift{free?x.delta:0,free?y.delta:0,0};
    if(!alignMath::fitsLane(x,box.moved(shift.x,shift.y),gesture.bounds,false,unit)){x={};shift.x=0;}
    if(!alignMath::fitsLane(y,box.moved(shift.x,shift.y),gesture.bounds,true,unit)){y={};shift.y=0;}
    gesture.x=x;gesture.y=y;gesture.drawingBounds=box.moved(shift.x,shift.y);gesture.ringShift=shift;
    if(!shift.x&&!shift.y)return;
    for(auto p=first;p!=last;++p){p->x+=shift.x;p->y+=shift.y;}
    Obj source=at<Obj>(gesture.tracker,0x110);
    Point position=at<Point>(source,0x1f8);position.x+=shift.x;position.y+=shift.y;
    UndoOff off(gesture.doc);oldSetAtomPos(source,&position);
}
void ringDrawingMove(Obj tracker) {
    if(!onUI()||gesture.tracker!=tracker||gesture.drawing!=Drawing::Ring){oldRingMove(tracker);return;}
    restoreRingShift();gesture.ringActive=true;
    try {oldRingMove(tracker);}catch(...){gesture.ringActive=false;throw;}
    gesture.ringActive=false;
}
Point* snapNative(Snap original,Obj object,Point* result,const Point* delta) {
    Point* returned=original(object,result,delta);
    if(gesture.inOriginal&&std::isfinite(result->x+result->y)&&
        (std::abs(result->x)>1e-8||std::abs(result->y)>1e-8))gesture.nativeSnap=true;
    return returned;
}
Point* arrowSnap(Obj o,Point* out,const Point* delta) {return snapNative(oldArrowSnap,o,out,delta);}
Point* graphicSnap(Obj o,Point* out,const Point* delta) {return snapNative(oldGraphicSnap,o,out,delta);}
Point* constrain(Obj tracker,Point* result) {
    if(!onUI()||gesture.tracker!=tracker||(!gesture.moving&&!gesture.finishing))return oldConstrain(tracker,result);
    if(gesture.cancelled) {*result=gesture.down;gesture.effective=*result;gesture.resolved=true;return result;}
    gesture.nativeSnap=false;gesture.inOriginal=true;
    Point* returned{};
    try {returned=oldConstrain(tracker,result);}catch(...){gesture.inOriginal=false;throw;}
    gesture.inOriginal=false;gesture.effective=*result;gesture.resolved=true;
    if(!enabled||!gesture.eligible||at<uint8_t>(tracker,0x238)||gesture.nativeSnap||
        (GetAsyncKeyState(VK_MENU)&0x8000)||!std::isfinite(result->x+result->y)) {gesture.x={};gesture.y={};return returned;}
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
    bool lockX{},lockY{};
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
    result->x+=gesture.x.delta;result->y+=gesture.y.delta;gesture.effective=*result;
    return returned;
}
void preview(Obj tracker) {
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
    try {oldFinish(tracker,arg,commit);}catch(...){gesture.finishing=false;throw;}
    gesture.finishing=false;gesture.eligible=false;gesture.x={};gesture.y={};
}
LRESULT CALLBACK shortcuts(int code,WPARAM removal,LPARAM value) {
    if(code>=0&&removal==PM_REMOVE&&value&&!trackingDepth) {
        auto& message=*reinterpret_cast<MSG*>(value);
        if((message.message==WM_KEYDOWN||message.message==WM_SYSKEYDOWN)&&message.wParam=='A'&&(GetKeyState(VK_CONTROL)&0x8000)&&
            (GetKeyState(VK_MENU)&0x8000)) {
            if(!(message.lParam&(1LL<<30))) {
                enabled=!enabled;DWORD setting=enabled;
                RegSetKeyValueW(HKEY_CURRENT_USER,L"Software\\ChemDrawLatency",L"SmartAlignment",REG_DWORD,&setting,sizeof(setting));
                OutputDebugStringA(enabled?"ChemDraw smart alignment enabled.\n":"ChemDraw smart alignment disabled.\n");
            }
            message.message=WM_NULL;message.wParam=0;message.lParam=0;
        }
    }
    return CallNextHookEx(keyboardHook,code,removal,value);
}
}
void warmSmartAlignmentPage(Obj doc) noexcept {
    if(!onUI()||!installed||!enabled||!doc||warmingIndex||trackingDepth||GetCapture()||
        (HIWORD(GetQueueStatus(QS_INPUT))&QS_INPUT)||
        ((GetAsyncKeyState(VK_LBUTTON)|GetAsyncKeyState(VK_RBUTTON)|GetAsyncKeyState(VK_MBUTTON))&0x8000))return;
    Obj page=mainPage(doc);if(!page||at<Obj>(page,0x10))return;
    try {
        if(pageIndices.size()>=16&&!pageIndices.contains(page))pageIndices.clear();
        auto& cached=pageIndices[page];
        if(cached.ready&&cached.head==at<Node*>(page,0xc8)&&cached.count==at<size_t>(page,0xd0))return;
        const auto revision=cached.revision;
        PageAlignmentIndex next;next.head=at<Node*>(page,0xc8);next.count=at<size_t>(page,0xd0);
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
            next.roots[id].members.push_back(std::move(member));
        });
        // Bounds are read only after native geometry idle, never from input or
        // the modal tracking loop. Native pointers remain on the UI thread.
        for(auto& root:next.roots) {
            if(HIWORD(GetQueueStatus(QS_INPUT))&QS_INPUT)return;
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
        if(cached.revision!=revision||next.head!=at<Node*>(page,0xc8)||next.count!=at<size_t>(page,0xd0))return;
        next.revision=revision;next.ready=true;cached=std::move(next);
    }catch(...) {warmingIndex=false;}
}
void beginSmartAlignment(Obj tracker,Obj doc,bool gpu) noexcept {
    gesture={};
    if(!installed||!enabled||!gpu||!doc||!nativeTracker(tracker)||at<uint8_t>(tracker,0x238))return;
    Obj page=at<Obj>(tracker,8),port=at<Obj>(doc,0x258);
    if(!page||!port||page!=mainPage(doc)||at<Obj>(page,0x10))return;
    try {
        gesture.tracker=tracker;gesture.page=page;gesture.doc=doc;gesture.window=portWindow(port);
        gesture.down=at<Point>(tracker,0x38);gesture.previousEffective=gesture.down;gesture.effective=gesture.down;
        const auto found=pageIndices.find(page);
        if(found==pageIndices.end()||!found->second.ready||
            found->second.head!=at<Node*>(page,0xc8)||found->second.count!=at<size_t>(page,0xd0))return;
        const auto& index=found->second;
        each(at<Node*>(tracker,0x140),[](Obj o){gesture.movingObjects.insert(o);});
        each(at<Node*>(tracker,0x120),[](Obj o){gesture.movingObjects.insert(o);});
        each(at<Node*>(tracker,0x130),[](Obj o){gesture.movingObjects.insert(o);});
        if(gesture.movingObjects.empty()){gesture={};return;}
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
            if(moving&&stationary){gesture={};return;} // Partial molecule/group.
            const Box bounds=r.reference.bounds;if(!moving||!bounds.valid())continue;
            {
                gesture.movingObjects.insert(r.reference.object);
                if(first){gesture.initial=bounds;first=false;}
                else {auto& m=gesture.initial;m={std::min(m.l,bounds.l),std::min(m.t,bounds.t),std::max(m.r,bounds.r),std::max(m.b,bounds.b)};}
            }
        }
        if(first){gesture={};return;}
        // Stable native draw order keeps equal-distance choices deterministic.
        std::vector<int> remap(index.roots.size(),-1);
        for(int id:index.drawOrder)if(!movingRoots.contains(size_t(id))&&index.roots[id].reference.bounds.valid()) {
            remap[id]=int(gesture.references.size());gesture.references.push_back(index.roots[id].reference);
            gesture.bounds.push_back(index.roots[id].reference.bounds);
        }
        if(gesture.references.empty()||gesture.references.size()>4096){gesture={};return;}
        for(int id:index.orderX)if(remap[id]>=0)gesture.orderX.push_back(remap[id]);
        for(int id:index.orderY)if(remap[id]>=0)gesture.orderY.push_back(remap[id]);
        gesture.eligible=true;
    }catch(...){gesture={};}
}
void endSmartAlignment() noexcept {if(gesture.doc)clearGpuAlignment(gesture.doc);gesture={};}
bool cancelSmartAlignment(HWND w) noexcept {
    if(!gesture.tracker||gesture.window!=w)return false;
    gesture.cancelled=true;gesture.released=false;clearRelations();return true;
}
void smartAlignmentReleased(HWND w,const MSG& message) noexcept {
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
    try {preview(gesture.tracker);vf<void(*)(Obj)>(gesture.tracker,0x10)(gesture.tracker);}catch(...){gesture.eligible=false;}
    clearGpuAlignment(gesture.doc);
}
void smartAlignmentPageGone(Obj page) noexcept {
    if(onUI())pageIndices.erase(page);
    if(onUI()&&gesture.page==page){clearGpuAlignment(gesture.doc);gesture={};}
}
void smartAlignmentGeometryInvalidated(Obj object) noexcept {
    if(onUI()&&object)if(auto cached=pageIndices.find(at<Obj>(object,0x60));cached!=pageIndices.end()) {
        cached->second.ready=false;++cached->second.revision;
    }
}
void smartAlignmentObjectChanged(Obj object) noexcept {
    smartAlignmentGeometryInvalidated(object);
    if(!onUI()||!gesture.eligible||!object||gesture.movingObjects.contains(object))return;
    if(at<Obj>(object,0x60)!=gesture.page)return;
    const Obj root=rootOf(object,gesture.page);
    if(gesture.movingObjects.contains(root))return;
    for(const auto& r:gesture.references)if(r.object==object||r.object==root){gesture.eligible=false;clearRelations();break;}
}
std::shared_ptr<const AlignmentFeedback> smartAlignmentFeedback(Obj tracker) noexcept {
    if(gesture.tracker!=tracker||!gesture.eligible||gesture.released||gesture.cancelled||
        (gesture.x.kind==Kind::None&&gesture.y.kind==Kind::None))return {};
    try {
        auto feedback=std::make_shared<AlignmentFeedback>();
        feedback->dip=float(GetDpiForWindow(gesture.window))/96;if(feedback->dip<=0)feedback->dip=1;
        Obj port=at<Obj>(gesture.doc,0x258),scale=at<Obj>(gesture.page,0x2a8);
        auto* graphics=fn<Gdiplus::Graphics*(*)(Obj)>(0x623140)(port);if(!graphics)return {};
        const Box m=gesture.initial.moved(gesture.effective.x-gesture.down.x,gesture.effective.y-gesture.down.y);
        auto project=[&](double x,double y) {
            Gdiplus::PointF p{float(fn<double(*)(Obj,double)>(0x3c6560)(scale,x)),float(fn<double(*)(Obj,double)>(0x3c6590)(scale,y))};
            graphics->TransformPoints(Gdiplus::CoordinateSpaceDevice,Gdiplus::CoordinateSpaceWorld,&p,1);
            return ScenePoint{p.X,p.Y};
        };
        auto screenBox=[&](Box b) {auto a=project(b.l,b.t),z=project(b.r,b.b);return Box{a.x,a.y,z.x,z.y};};
        for(bool y:{false,true}) {
            const Relation c=y?gesture.y:gesture.x;if(c.kind==Kind::None)continue;
            const Box a=gesture.bounds[c.a];
            if(c.kind==Kind::Align) {
                const double v=alignMath::anchor(a,y,c.anchor);
                auto from=y?project(std::min(a.l,m.l),v):project(v,std::min(a.t,m.t));
                auto to=y?project(std::max(a.r,m.r),v):project(v,std::max(a.b,m.b));
                if(y){from.x-=8*feedback->dip;to.x+=8*feedback->dip;}else{from.y-=8*feedback->dip;to.y+=8*feedback->dip;}
                feedback->strokes.push_back({from,to,false});
            }else {
                const Box sa=screenBox(a),sb=screenBox(gesture.bounds[c.b]),sm=screenBox(m);
                const double edge=std::max({alignMath::high(sa,!y),alignMath::high(sb,!y),alignMath::high(sm,!y)});
                RectI viewport{};vf<RectI*(*)(Obj,RectI*)>(gesture.doc,0x100)(gesture.doc,&viewport);
                const double rail=std::min(edge+10*feedback->dip,double(y?viewport.r:viewport.b)-6*feedback->dip);
                auto segment=[&](double lo,double hi,Box first,Box second) {
                    auto p=[&](double along,double across){return ScenePoint{float(y?across:along),float(y?along:across)};};
                    feedback->strokes.push_back({p(lo,rail),p(hi,rail),true});
                    feedback->strokes.push_back({p(lo,alignMath::high(first,!y)),p(lo,rail+4*feedback->dip),false});
                    feedback->strokes.push_back({p(hi,alignMath::high(second,!y)),p(hi,rail+4*feedback->dip),false});
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
void installSmartAlignment() {
    if(!supportedHash(reinterpret_cast<HMODULE>(base),"0bf203d7ddf0c700bf9d44fd156425d8df274277fdaea58fa511cfbddc51861a")||
        !supportedHash(GetModuleHandleW(L"ChemDrawUI.dll"),"b9af968d2e75822fc47a4e5a4d2f348ea636804c712763b73b3536a30268a4f2"))return;
    const BYTE prologue[]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x48,0x89,0x70,0x20};
    if(memcmp(reinterpret_cast<void*>(base+0x16b660),prologue,sizeof(prologue)))return;
    DWORD setting=1,size=sizeof(setting);
    RegGetValueW(HKEY_CURRENT_USER,L"Software\\ChemDrawLatency",L"SmartAlignment",RRF_RT_REG_DWORD,nullptr,&setting,&size);enabled=setting!=0;
    wchar_t option[8]{};if(GetEnvironmentVariableW(L"CHEMDRAW_SMART_ALIGN",option,8))enabled=wcscmp(option,L"0")!=0;
    hook(0x16b660,constrain,oldConstrain);hook(0x16bb50,preview,oldPreview);hook(0x16ca20,finish,oldFinish);
    hook(0x2a8480,arrowSnap,oldArrowSnap);hook(0x320440,graphicSnap,oldGraphicSnap);
    keyboardHook=SetWindowsHookExW(WH_GETMESSAGE,shortcuts,module,uiThread);
    if(!keyboardHook)throw std::runtime_error("Could not install alignment shortcut");
    installed=true;
}
void removeSmartAlignment() {if(keyboardHook)UnhookWindowsHookEx(keyboardHook);keyboardHook=nullptr;installed=false;endSmartAlignment();}
}

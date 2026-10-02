// Included inside curved-arrow.cpp. Extend native ArrowTracker without changing
// ApplySubtype, its palette, its work port, or its native transaction owner.
using RegularFactory=Obj*(*)(Obj*,Obj,Point*,Obj,uint8_t);
using RegularCtor=Obj(*)(Obj,Obj,uint8_t);
using Destructor=Obj(*)(Obj,unsigned);
RegularFactory oldRegularFactory{};
using ArrowPress=Obj*(*)(Obj*,Obj,Obj,const Point*,unsigned,uint8_t,uint8_t,uint8_t,uint8_t,uint8_t*);
using OtherArrowSnap=Obj(*)(Obj,Point*);
ArrowPress oldArrowPress{};
OtherArrowSnap oldOtherArrowSnap{};
RegularCtor oldRegularCtor{};
Move oldRegularMove{};
Finish oldRegularFinish{};
Destructor oldRegularDestructor{};
struct RegularGesture {
    Obj tracker{},arrow{},page{};HWND window{};
    Point fixed{},moving{},release{},initialTail{},initialHead{};
    RectD initialBounds{};
    int source{-1},target{-1},incumbent{-1};double angle{};
    bool tail{},existing{},running{},released{},cancelled{};
};
thread_local RegularGesture regular;
thread_local Query* regularPress{};
thread_local bool maintaining{};
using ArrowFlip=void(*)(Obj,int,const Point*,bool);
ArrowFlip oldArrowFlip{};
thread_local Obj flippingArrow{};
struct AttachmentIDs {int source{-1},target{-1};};
std::unordered_map<Obj,AttachmentIDs> attachmentUndo;
using UndoCtor=Obj(*)(Obj,Obj,int);
UndoCtor oldUndoCtor{};
Move oldUndoExecute{};
Destructor oldUndoDestructor{};
thread_local Obj undoArrow{};
thread_local AttachmentIDs inverseAttachments;
struct UndoDisabled {
    alignas(8) std::byte storage[16]{};
    explicit UndoDisabled(Obj object) {fn<void(*)(void*,Obj)>(0x501ff0)(storage,object);}
    ~UndoDisabled() {fn<void(*)(void*)>(0x5023f0)(storage);}
};
void arrowFlip(Obj arrow,int type,const Point* center,bool z) {
    if(!onUI()||!enabled) {oldArrowFlip(arrow,type,center,z);return;}
    const Obj previous=flippingArrow;
    struct RestoreFlip {Obj previous;~RestoreFlip(){flippingArrow=previous;}} restore{previous};
    flippingArrow=arrow;
    // Native Flip changes head/center first and plane axes afterward. Defer
    // attachment reconciliation until both halves of that operation complete.
    oldArrowFlip(arrow,type,center,z);
    flippingArrow=previous;
    arrowPathFlipped(arrow);
    vf<void(*)(Obj)>(arrow,0xa8)(arrow);
    vf<void(*)(Obj)>(arrow,0xb0)(arrow);
}
Obj undoCtor(Obj command,Obj arrow,int kind) {
    Obj result=oldUndoCtor(command,arrow,kind);
    if(!onUI()) return result;
    AttachmentIDs ids{at<int>(arrow,0x1f0),at<int>(arrow,0x1f4)};
    if(undoArrow==arrow) ids=inverseAttachments;
    else if(regular.running&&regular.existing&&regular.arrow==arrow) ids={regular.source,regular.target};
    attachmentUndo.insert_or_assign(command,ids);
    return result;
}
void undoExecute(Obj command) {
    if(!onUI()) {oldUndoExecute(command);return;}
    auto saved=attachmentUndo.find(command);
    if(saved==attachmentUndo.end()) {oldUndoExecute(command);return;}
    Obj arrow=at<Obj>(command,0x18),previous=undoArrow;
    const AttachmentIDs prior=inverseAttachments;
    inverseAttachments={at<int>(arrow,0x1f0),at<int>(arrow,0x1f4)};undoArrow=arrow;
    at<int>(arrow,0x1f0)=saved->second.source;at<int>(arrow,0x1f4)=saved->second.target;
    oldUndoExecute(command);
    undoArrow=previous;inverseAttachments=prior;
}
Obj undoDestructor(Obj command,unsigned flags) {
    if(onUI()) attachmentUndo.erase(command);
    return oldUndoDestructor(command,flags);
}
Obj* regularFactory(Obj* result,Obj page,Point* point,Obj existing,uint8_t adjust) {
    Point aligned{};
    if(!existing&&!adjust&&point&&smartAlignmentPlacementPoint(page,*point,aligned))point=&aligned;
    if(!onUI()||!enabled||!point) return oldRegularFactory(result,page,point,existing,adjust);
    Query press{page,existing};
    if(!adjust&&!existing) {
        if(auto previous=hoverTargets.find(page);previous!=hoverTargets.end()) press.incumbent=previous->second;
        nearest(press,*point);
    }
    Query* previous=regularPress;regularPress=&press;
    QueryScope scope(press);
    Obj* returned=oldRegularFactory(result,page,point,existing,adjust);
    regularPress=previous;
    return returned;
}
Obj* arrowPress(Obj* result,Obj page,Obj existing,const Point* point,unsigned clicks,
    uint8_t modifier,uint8_t alternate,uint8_t eighth,uint8_t ninth,uint8_t* track) {
    if(onUI()&&enabled&&point&&fn<int(*)()>(0x4f4ba0)()==1&&arrowChemistryTarget(page,*point)) {
        // Native dispatch can identify an existing arrow independently of the
        // atom/bond hit, then select it or return without creating a tracker.
        // An ordinary palette press on chemistry always starts a fresh arrow.
        Point cursor=*point;if(track) *track=1;
        return regularFactory(result,page,&cursor,nullptr,0);
    }
    return oldArrowPress(result,page,existing,point,clicks,modifier,alternate,eighth,ninth,track);
}
Obj otherArrowSnap(Obj objects,Point* point) {
    if(onUI()&&enabled&&regular.running&&regular.tracker&&
        objects==static_cast<std::byte*>(regular.tracker)+0x188&&
        (at<int>(regular.arrow,0x1f0)>=0||at<int>(regular.arrow,0x1f4)>=0)) return nullptr;
    return oldOtherArrowSnap(objects,point);
}
Obj regularCtor(Obj tracker,Obj arrow,uint8_t tail) {
    Obj result=oldRegularCtor(tracker,arrow,tail);
    if(!onUI()||!enabled) return result;
    arrowPathDrawingStarted();
    regular={};regular.tracker=tracker;regular.arrow=arrow;regular.page=at<Obj>(arrow,0x60);
    regular.window=windowForPage(regular.page);regular.tail=tail!=0;
    regular.existing=at<uint8_t>(tracker,0x180)!=0;
    regular.source=at<int>(arrow,0x1f0);regular.target=at<int>(arrow,0x1f4);
    regular.angle=at<double>(arrow,0x130);
    arrowWorldEndpoints(arrow,regular.initialTail,regular.initialHead);
    vf<RectD*(*)(Obj,RectD*)>(arrow,0x1c8)(arrow,&regular.initialBounds);
    regular.fixed=regular.tail?regular.initialHead:regular.initialTail;
    regular.moving=regular.tail?regular.initialTail:regular.initialHead;
    if(!regular.existing) {
        Query press{regular.page,arrow};
        if(regularPress) press.incumbent=regularPress->selected;
        Obj source=nearest(press,at<Point>(tracker,0x148));
        if(source) {
            regular.fixed=linkedAnchor(source,regular.fixed);
            at<int>(arrow,regular.tail?0x1f4:0x1f0)=at<int>(source,0x24);
            at<Point>(tracker,0x148)=regular.fixed;
        }
    } else {
        regular.fixed=linkedAnchor(byID(regular.page,at<int>(arrow,regular.tail?0x1f4:0x1f0)),regular.fixed);
    }
    regular.incumbent=at<int>(arrow,regular.tail?0x1f0:0x1f4);
    // This tag contains no IDs and suppresses the electron-arrow auto-bend rule.
    {UndoDisabled off(arrow);rememberStandardArrow(arrow);}
    return result;
}
void regularTarget(const Point& point) {
    Obj arrow=regular.arrow;
    const size_t fixedID=regular.tail?0x1f4:0x1f0,movingID=regular.tail?0x1f0:0x1f4;
    Query q{regular.page,arrow,at<int>(arrow,fixedID),regular.incumbent};
    Obj target=nearest(q,point);
    regular.incumbent=target?at<int>(target,0x24):-1;
    at<int>(arrow,movingID)=regular.incumbent;
    regular.fixed=linkedAnchor(byID(regular.page,at<int>(arrow,fixedID)),regular.fixed);
    regular.moving=linkedAnchor(target,point);
}
void regularMove(Obj tracker) {
    smartAlignmentDrawingMove(tracker);
    if(!onUI()||!enabled||regular.tracker!=tracker) {oldRegularMove(tracker);return;}
    RectD previous{};vf<RectD*(*)(Obj,RectD*)>(regular.arrow,0x1c8)(regular.arrow,&previous);
    regularTarget(at<Point>(tracker,0x50));
    if(regular.incumbent>=0) at<Point>(tracker,0x50)=regular.moving;
    at<Point>(tracker,0x148)=regular.fixed;
    regular.running=true;oldRegularMove(tracker);regular.running=false;
    if(at<int>(regular.arrow,regular.tail?0x1f4:0x1f0)<0)regular.fixed=at<Point>(tracker,0x148);
    updateArrowTargetHighlight(at<Obj>(regular.page,8),byID(regular.page,regular.incumbent));
    // Damage covers the original and intermediate shafts; GPU owner filtering
    // retains only the current native tracker preview.
    RectD current{};vf<RectD*(*)(Obj,RectD*)>(regular.arrow,0x1c8)(regular.arrow,&current);
    fn<void(*)(Obj,const RectD*)>(0x1464e0)(tracker,&regular.initialBounds);
    fn<void(*)(Obj,const RectD*)>(0x1464e0)(tracker,&previous);
    fn<void(*)(Obj,const RectD*)>(0x1464e0)(tracker,&current);
}
short regularFinish(Obj tracker) {
    if(regular.tracker!=tracker) return oldRegularFinish(tracker);
    Obj arrow=regular.arrow;const bool cancelled=regular.cancelled||!regular.released;
    if(cancelled) {
        at<int>(arrow,0x1f0)=regular.source;at<int>(arrow,0x1f4)=regular.target;
        if(regular.existing) {
            UndoDisabled off(arrow);
            fn<Align>(0x2a8260)(arrow,&regular.initialHead,&regular.initialTail);
        }
    } else {
        regularTarget(regular.release);
        const bool linked=at<int>(arrow,0x1f0)>=0||at<int>(arrow,0x1f4)>=0;
        const bool wasLinked=regular.source>=0||regular.target>=0;
        if(linked||wasLinked) {
            at<Point>(tracker,0x50)=regular.moving;
            Point tail=regular.tail?regular.moving:regular.fixed;
            Point head=regular.tail?regular.fixed:regular.moving;
            regular.running=true;
            insetArrowEndpoints(arrow,tail,head,regular.angle);
            fn<Align>(0x2a8260)(arrow,&head,&tail);
            regular.running=false;
        }
    }
    if(!cancelled&&!regular.existing) {
        Point tail{},head{};
        if(smartAlignmentGhostArrowEndpoints(regular.page,tail,head)) {
            UndoDisabled off(arrow);
            if(!setMirroredArrowEndpoints(arrow,&head,&tail))oldEndpoints(arrow,&head,&tail);
            at<Point>(tracker,0x50)=regular.tail?tail:head;
        }
    }
    regular={};
    const short result=oldRegularFinish(tracker);
    if(!cancelled&&at<uint8_t>(tracker,0x141)) rememberEditableArrow(arrow);
    return result;
}
Obj regularDestructor(Obj tracker,unsigned flags) {
    if(regular.tracker==tracker) regular={};
    return oldRegularDestructor(tracker,flags);
}

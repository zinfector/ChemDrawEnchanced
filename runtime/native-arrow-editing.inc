// Included inside arrow-path.cpp's anonymous namespace. No windows or dialogs:
// visible paths are page-owned CDSplines and their handles use SplineTracker.
const std::string pairTag="Zuonik/SmartArrow/NativePath/v2";
const std::string cornersTag="Zuonik/SmartArrow/Corners/v2";
const std::string standardTag="Zuonik/SmartArrow/NativePalette/v1";
struct Pair { Obj group{},arrow{},curve{}; };
using SetPoints=void(*)(Obj,const std::vector<Point>*);
using Endpoints=void(*)(Obj,const Point*,const Point*);
using Adjustment=void(*)(Obj,bool,bool,Obj);
using SubHit=int(*)(Obj,const Point*);
using Move=void(*)(Obj);
using Finish=short(*)(Obj);
using SplineCtor=Obj(*)(Obj,const Point*,Obj,int,bool,const Point*);
using Destructor=Obj(*)(Obj,unsigned);
SetPoints oldSetPoints{};
Endpoints oldStartEnd{};
Adjustment oldAdjustment{};
using DrawAdjustment=bool(*)(Obj,Obj,bool,bool);
DrawAdjustment oldDrawAdjustment{};
SubHit oldSubHit{};
Unary oldCurveValidate{},oldDelete{};
Draw oldCurveDraw{};
bool(*oldCanUngroup)(Obj){};
Move oldArcMove{},oldSplineMove{};
Finish oldArcFinish{},oldSplineFinish{};
SplineCtor oldSplineCtor{};
Destructor oldArcDestructor{},oldSplineDestructor{};
uintptr_t nativeUI{};
thread_local bool syncing{},deletingPair{},redirectingEnds{};
thread_local Obj movingCurve{};
struct ArcDrag {
    Obj tracker{},arrow{};HWND window{};
    Point tail{},head{},localTail{},localHead{};
    RectD initialBounds{};
    double initial{},angle{},sign{1};
    bool running{},cancelled{};
};
thread_local ArcDrag arcDrag;
struct CurveDrag {
    Obj tracker{},curve{};HWND window{};
    std::vector<Point> original;
    int index{-1};bool cancelled{};
};
thread_local CurveDrag curveDrag;
std::unordered_map<Obj,int> pickedPoints;
struct ShaftClick {HWND window{};DWORD time{};POINT point{};bool valid{};};
thread_local ShaftClick shaftClick;

bool isSpline(Obj o) {
    return o&&at<uintptr_t*>(o,0)[0x2b0/8]==base+0x3484b0;
}
std::string tagString(Obj o,const std::string& name) {
    Obj t=fn<Obj(*)(Obj,const std::string*)>(0x328390)(o,&name);
    if(!t||at<int>(t,0x218)!=3) return {};
    const auto* value=fn<const std::string*(*)(Obj)>(0x32db70)(t);
    return value&&value->size()<200000?*value:std::string{};
}
void setTag(Obj o,const std::string& name,const std::string& value) {
    Obj tag{};
    {
        UndoOff off(o);Obj memory=fn<Obj(*)(size_t)>(0x8469e4)(0x2a0);
        if(!memory) throw std::bad_alloc();
        tag=fn<Obj(*)(Obj,Obj,const std::string*)>(0x32c800)(memory,at<Obj>(o,0x60),&name);
        fn<void(*)(Obj,const std::string*)>(0x32e870)(tag,&value);
        fn<void(*)(Obj,bool)>(0xa5b00)(tag,true);
        fn<void(*)(Obj,bool)>(0xa5c30)(tag,false);
    }
    fn<void(*)(Obj,const std::string*,Obj,bool)>(0x32b900)(o,&name,tag,false);
}
bool pairedGroup(Obj g) { return g&&!at<uint8_t>(g,0x30)&&tagString(g,pairTag)=="2"; }
TreeNode* nextNode(TreeNode* n) {
    if(!n->right->nil) {n=n->right;while(!n->left->nil) n=n->left;return n;}
    auto p=n->parent;while(!p->nil&&n==p->right) {n=p;p=p->parent;}return p;
}
Pair findPair(Obj o) {
    if(!o) return {};
    Obj group=at<Obj>(o,0x28);
    if(!pairedGroup(group)) {if(!pairedGroup(o)) return {};group=o;}
    Pair p{group};auto head=at<TreeNode*>(group,0xc0);size_t count=at<size_t>(group,0xc8);
    if(!head||count>8) return {};
    for(auto n=head->left;n!=head&&count--;n=nextNode(n)) {
        if(at<uint8_t>(n->object,0x30)) continue;
        if(isArrow(n->object)) p.arrow=n->object;
        else if(isSpline(n->object)) p.curve=n->object;
    }
    return p.arrow&&p.curve?p:Pair{};
}
std::vector<Point> curvePoints(Obj curve) {
    auto first=at<Point*>(curve,0x118),last=at<Point*>(curve,0x120);
    if(!first||!last||last-first<6||last-first>ptrdiff_t(maxNodes*3)||(last-first)%3) return {};
    return {first,last};
}
HWND drawingWindow(Obj page) {
    Obj doc=page?at<Obj>(page,8):nullptr,port=doc?at<Obj>(doc,0x258):nullptr;
    return port?portWindow(port):nullptr;
}
double dipUnits(Obj page) {
    Obj scale=at<Obj>(page,0x2a8);UINT dpi=GetDpiForWindow(drawingWindow(page));
    return scale?at<double>(scale,8)*double(dpi?dpi:96)/96:1;
}
void redraw(Obj object) {
    fn<Unary>(0x328a90)(object);bumpGeneration();
    HWND w=drawingWindow(at<Obj>(object,0x60));if(w) InvalidateRect(w,nullptr,FALSE);
}
struct UndoRecord {
    Obj doc{};bool owns{};
    explicit UndoRecord(Obj object):doc(at<Obj>(at<Obj>(object,0x60),8)),owns(!at<Obj>(doc,0xf0)) {
        if(owns) fn<Unary>(0x502820)(doc);
    }
    ~UndoRecord() {if(owns) fn<Unary>(0x502c40)(doc);}
};
void recordCreate(Obj o) {
    Obj memory=fn<Obj(*)(size_t)>(0x8469e4)(0x30);if(!memory) throw std::bad_alloc();
    Obj command=fn<Obj(*)(Obj,Obj)>(0x4612f0)(memory,o);
    vf<void(*)(Obj,Obj,bool)>(o,0x1c0)(o,command,true);
}
void copyArrowStyle(Obj arrow,Obj curve) {
    fn<void(*)(Obj,Obj)>(0x32aef0)(curve,static_cast<std::byte*>(arrow)+0x80);
    fn<void(*)(Obj,bool)>(0x349a80)(curve,at<uint8_t>(arrow,0x160)!=0);
    fn<void(*)(Obj,bool)>(0x349be0)(curve,at<uint8_t>(arrow,0x161)!=0);
    fn<void(*)(Obj,bool)>(0x349b20)(curve,false);
    fn<void(*)(Obj,int)>(0x349a00)(curve,fn<int(*)(Obj)>(0xdc440)(arrow));
    fn<void(*)(Obj,int)>(0x34a260)(curve,fn<int(*)(Obj)>(0xdc520)(arrow));
    fn<void(*)(Obj,int)>(0x34a190)(curve,fn<int(*)(Obj)>(0x8e5f0)(arrow));
    fn<void(*)(Obj,double)>(0x34a330)(curve,fn<double(*)(Obj)>(0xdc530)(arrow));
    fn<void(*)(Obj,double)>(0x34a100)(curve,fn<double(*)(Obj)>(0xdc4c0)(arrow));
    fn<void(*)(Obj,double)>(0x34a3c0)(curve,fn<double(*)(Obj)>(0xdc540)(arrow));
}
bool offsetAttachment(Obj arrow,int id) {
    Obj page=at<Obj>(arrow,0x60),object=id>=0&&page?fn<Obj(*)(Obj,int)>(0x712f80)(page,id):nullptr;
    if(!object||!at<uint8_t>(object,0x34)) return false;
    const uintptr_t table=at<uintptr_t>(object,0);
    Point boundary{};
    if(table==base+0x89a468&&arrowInsertionAttachment(arrow,id==at<int>(arrow,0x1f0),boundary))return false;
    return table==base+0x8b34f8||table==base+0x8babc0||table==base+0x89a468;
}
double endpointClearance(Obj arrow,Point tail,Point head) {
    Obj page=at<Obj>(arrow,0x60);if(!page) return 0;
    Obj setting=fn<Obj(*)(Obj,int)>(0x7128f0)(page,0x805);
    double bond=setting?fn<double(*)(Obj)>(0x37be40)(setting):0;
    const double chord=length(sub(head,tail));
    if(!std::isfinite(chord)||chord<=1e-6) return 0;
    // Document units keep the gap consistent with bond dimensions on export
    // and save. Cap it for short arrows so their endpoints cannot cross.
    return std::min(std::isfinite(bond)&&bond>0?bond*0.10:chord*0.015,chord*0.10);
}
Point unitDirection(Point direction,Point fallback) {
    double n=length(direction);if(!std::isfinite(n)||n<1e-8) {direction=fallback;n=length(direction);}
    return n>1e-8?Point{direction.x/n,direction.y/n,0}:Point{};
}
void curveAttachmentTargets(Obj arrow,const std::vector<Point>& p,Point& tail,Point& head) {
    if(p.size()<6) return;
    const double gap=endpointClearance(arrow,tail,head);Point chord=sub(head,tail);
    Obj page=at<Obj>(arrow,0x60);
    if(offsetAttachment(arrow,at<int>(arrow,0x1f0)))
        tail=arrowAttachmentPoint(fn<Obj(*)(Obj,int)>(0x712f80)(page,at<int>(arrow,0x1f0)),tail,
            unitDirection(sub(p[2],p[1]),chord),gap);
    if(offsetAttachment(arrow,at<int>(arrow,0x1f4)))
        head=arrowAttachmentPoint(fn<Obj(*)(Obj,int)>(0x712f80)(page,at<int>(arrow,0x1f4)),head,
            unitDirection(sub(p[p.size()-3],p[p.size()-2]),mul(chord,-1)),gap);
}
void applyCurveClearance(Obj arrow,std::vector<Point>& p,Point tail,Point head) {
    if(p.size()<6) return;
    curveAttachmentTargets(arrow,p,tail,head);
    Point dt=sub(tail,p[1]),dh=sub(head,p[p.size()-2]);
    for(size_t i=0;i<3;++i) p[i]=add(p[i],dt);
    for(size_t i=p.size()-3;i<p.size();++i) p[i]=add(p[i],dh);
    p[1]=tail;p[p.size()-2]=head;
}
std::vector<Point> initialPath(Obj arrow) {
    Point tail{},head{};if(!endpoints(arrow,tail,head)) return {};
    Shape s;
    if(readShape(arrow,s)&&s.custom) {
        auto p=worldPoints(s,tail,head);applyCurveClearance(arrow,p,tail,head);return p;
    }
    // Use ChemDraw's own circular-arc approximation rather than choosing a
    // handedness or resolution different from its arrow renderer.
    Point localTail{};fn<void(*)(Obj,Point*)>(0x2a4c90)(arrow,&localTail);
    Obj spline=fn<Obj(*)(Obj,const Point*)>(0x29f350)(arrow,&localTail);
    std::vector<Point> p;
    if(spline) {
        p=curvePoints(spline);
        Obj previous=transientSpline;transientSpline=spline;
        vf<void(*)(Obj,unsigned)>(spline,0)(spline,1);transientSpline=previous;
    }
    if(p.empty()) {p={tail,tail,mix(tail,head,1.0/3),mix(tail,head,2.0/3),head,head};}
    else {
        // The native shaft can stop at the arrowhead base. Extend the final
        // tangent by the same delta, keeping the actual tip on the attachment.
        Point dt=sub(tail,p[1]),dh=sub(head,p[p.size()-2]);
        for(size_t i=0;i<3;++i) p[i]=add(p[i],dt);
        for(size_t i=p.size()-3;i<p.size();++i) p[i]=add(p[i],dh);
    }
    applyCurveClearance(arrow,p,tail,head);return p;
}
Obj nativePath(Obj arrow) {
    if(auto p=findPair(arrow);p.curve) return p.curve;
    if(!isArrow(arrow)||at<uint8_t>(arrow,0x30)) return nullptr;
    auto points=initialPath(arrow);if(points.empty()) return nullptr;
    UndoRecord record(arrow);Obj page=at<Obj>(arrow,0x60),parent=at<Obj>(arrow,0x28),group{},curve{};
    {
        UndoOff off(arrow);
        group=fn<Obj(*)(Obj,int)>(0x7178b0)(page,-1);
        curve=fn<Obj(*)(Obj,int)>(0x717f30)(page,-1);
        if(!group||!curve) throw std::bad_alloc();
        copyArrowStyle(arrow,curve);oldSetPoints(curve,&points);
        setTag(group,pairTag,"2");setTag(curve,cornersTag,std::string(points.size()/3,'1'));
    }
    recordCreate(group);recordCreate(curve);
    // Group ownership makes clipboard/save remapping native. No metadata IDs
    // point between objects, and the visible curve is an ordinary CDX Curve.
    if(parent) fn<void(*)(Obj,Obj)>(0x32b0f0)(group,parent);
    fn<void(*)(Obj,Obj)>(0x32b0f0)(curve,group);
    fn<void(*)(Obj,Obj)>(0x32b0f0)(arrow,group);
    fn<void(*)(Obj,bool)>(0x32be20)(arrow,false);
    redraw(curve);return curve;
}
void selectCurve(Obj curve) {
    Obj page=at<Obj>(curve,0x60);
    fn<Unary>(0x14a530)(at<Obj>(page,8));
    fn<void(*)(int,short)>(0x4f70c0)(8,0);
    fn<void(*)(Obj,Obj)>(0x71a690)(page,curve);
    vf<void(*)(Obj,bool)>(curve,0x10)(curve,true);redraw(curve);
}
void selectArc(Obj object) {
    Obj arrow=object;Pair p=findPair(object);
    if(p.arrow) {
        arrow=p.arrow;UndoRecord record(arrow);Obj parent=at<Obj>(p.group,0x28);
        fn<Unary>(0x14a530)(at<Obj>(at<Obj>(arrow,0x60),8));
        fn<void(*)(Obj,Obj)>(0x32b0f0)(arrow,parent);
        fn<void(*)(Obj,bool)>(0x32be20)(arrow,true);
        bool was=deletingPair;deletingPair=true;
        // Native group deletion ungroups its members; explicitly remove the
        // visible curve while retaining the restored native arrow.
        oldDelete(p.group);oldDelete(p.curve);deletingPair=was;
        Point tail{},head{};
        if(endpoints(arrow,tail,head)) {
            Shape arc;arc.angle=at<double>(arrow,0x130);arc.tail=tail;arc.head=head;
            arc.points=arcPoints(arc.angle);arc.smooth.assign(arc.points.size()/3,1);
            setTag(arrow,tagName,encode(arc));
        }
    }
    if(!isArrow(arrow)) return;
    fn<void(*)(int,short)>(0x4f70c0)(1,0);
    vf<void(*)(Obj,bool)>(arrow,0x10)(arrow,true);redraw(arrow);
}
void syncCurve(Obj curve) {
    if(!onUI()||syncing||movingCurve==curve||at<uint8_t>(curve,0x30)) return;
    Pair pair=findPair(curve);if(pair.curve!=curve) return;
    auto p=curvePoints(curve);if(p.empty()) return;
    Point tail=p[1],head=p[p.size()-2];
    Point targetTail=anchor(pair.arrow,at<int>(pair.arrow,0x1f0),tail);
    Point targetHead=anchor(pair.arrow,at<int>(pair.arrow,0x1f4),head);
    const Point attachedTail=targetTail,attachedHead=targetHead;
    curveAttachmentTargets(pair.arrow,p,targetTail,targetHead);
    Point dt=sub(targetTail,tail),dh=sub(targetHead,head);
    const double a=length(dt),b=length(dh);if(a<1e-6&&b<1e-6) return;
    Point oldChord=sub(head,tail),newChord=sub(targetHead,targetTail);
    const double norm=oldChord.x*oldChord.x+oldChord.y*oldChord.y;
    const bool common=a>1e-6&&b>1e-6&&norm>1e-12;
    for(size_t i=0;i<p.size();++i) {
        if(common) {
            Point v=sub(p[i],tail);double x=(v.x*oldChord.x+v.y*oldChord.y)/norm,
                y=(v.y*oldChord.x-v.x*oldChord.y)/norm;
            p[i]=add(targetTail,{newChord.x*x-newChord.y*y,newChord.y*x+newChord.x*y,v.z});
        } else p[i]=add(p[i],mix(dt,dh,double(i/3)/double(p.size()/3-1)));
    }
    p[1]=targetTail;p[p.size()-2]=targetHead;
    // Rotation also rotates the endpoint tangents. Reapply the gap against
    // the actual attachments once, without accumulating endpoint offsets.
    applyCurveClearance(pair.arrow,p,attachedTail,attachedHead);
    syncing=true;{UndoOff off(curve);oldSetPoints(curve,&p);}syncing=false;
}
void curveValidate(Obj curve) {syncCurve(curve);oldCurveValidate(curve);}
void curveDraw(Obj curve,Obj port) {
    GpuObjectScope owner(curve,movingCurve==curve);oldCurveDraw(curve,port);
}
void pathDeleted(Obj object) {
    if(onUI()&&!deletingPair) if(auto p=findPair(object);p.group) {
        deletingPair=true;
        // DeleteInternal on CDGroup detaches members, so remove the complete
        // arrow/curve pair explicitly within the existing native transaction.
        oldDelete(p.group);oldDelete(p.curve);oldDelete(p.arrow);
        deletingPair=false;return;
    }
    oldDelete(object);
}
bool canUngroup(Obj group) {return pairedGroup(group)?false:oldCanUngroup(group);}

struct DetachedQuery {
    Obj arrow;int source,target;
    explicit DetachedQuery(Obj o):arrow(o),source(at<int>(o,0x1f0)),target(at<int>(o,0x1f4)) {
        at<int>(o,0x1f0)=-1;at<int>(o,0x1f4)=-1;
    }
    ~DetachedQuery() {at<int>(arrow,0x1f0)=source;at<int>(arrow,0x1f4)=target;}
};
bool linkedArc(Obj arrow) {
    return onUI()&&at<uint8_t>(arrow,0x34)&&!findPair(arrow).group&&
        (at<int>(arrow,0x1f0)>=0||at<int>(arrow,0x1f4)>=0);
}
void adjustments(Obj arrow,bool first,bool second,Obj result) {
    if(linkedArc(arrow)) {DetachedQuery q(arrow);oldAdjustment(arrow,first,second,result);}
    else oldAdjustment(arrow,first,second,result);
}
int subHit(Obj arrow,const Point* point) {
    if(linkedArc(arrow)) {DetachedQuery q(arrow);return oldSubHit(arrow,point);}
    return oldSubHit(arrow,point);
}
void startEnd(Obj arrow,const Point* head,const Point* end) {
    Point regularTail{},regularHead{};
    if(!redirectingEnds&&regularArrowEndpoints(arrow,regularTail,regularHead)) {
        redirectingEnds=true;
        insetArrowEndpoints(arrow,regularTail,regularHead,at<double>(arrow,0x130));
        fn<void(*)(Obj,const Point*,const Point*)>(0x2a8260)(arrow,&regularHead,&regularTail);
        redirectingEnds=false;
    } else if(arcDrag.running&&arcDrag.arrow==arrow&&!redirectingEnds) {
        redirectingEnds=true;
        fn<void(*)(Obj,double)>(0x2a6c00)(arrow,arcDrag.angle);
        Point visibleTail=arcDrag.tail,visibleHead=arcDrag.head;
        insetArrowEndpoints(arrow,visibleTail,visibleHead,arcDrag.angle);
        fn<void(*)(Obj,const Point*,const Point*)>(0x2a8260)(arrow,&visibleHead,&visibleTail);
        redirectingEnds=false;
    } else oldStartEnd(arrow,head,end);
}
void arcMove(Obj tracker) {
    Obj arrow=at<Obj>(tracker,0x138);
    if(!linkedArc(arrow)) {oldArcMove(tracker);return;}
    if(arcDrag.tracker!=tracker) {
        arcDrag={};arcDrag.tracker=tracker;arcDrag.arrow=arrow;
        arcDrag.window=drawingWindow(at<Obj>(arrow,0x60));
        fn<void(*)(Obj,Point*)>(0x2a4c90)(arrow,&arcDrag.localTail);
        fn<void(*)(Obj,Point*)>(0xdc4d0)(arrow,&arcDrag.localHead);
        endpoints(arrow,arcDrag.tail,arcDrag.head);
        arcDrag.initial=arcDrag.angle=at<double>(arrow,0x130);
        // A straight shaft has no midpoint displacement to calibrate against.
        // Native positive angles bend toward the positive chord normal; retain
        // the plane's reflected handedness when starting from zero curvature.
        arcDrag.sign=arrowPageArcAngle(arrow,1.0);
        vf<Bounds>(arrow,0x1c8)(arrow,&arcDrag.initialBounds);
        Point mid{};fn<void(*)(Obj,Point*)>(0x2a1a20)(arrow,&mid);
        Point chord=sub(arcDrag.localHead,arcDrag.localTail),offset=sub(mid,arcDrag.localTail);
        const double y=chord.x*offset.y-chord.y*offset.x;
        if(std::abs(y)>1e-6&&std::abs(arcDrag.initial)>1e-6) arcDrag.sign=y*arcDrag.initial>0?1:-1;
    }
    if(at<int>(tracker,0x140)==2) {
        Point chord=sub(arcDrag.head,arcDrag.tail),cursor=sub(at<Point>(tracker,0x50),arcDrag.tail);
        double norm=chord.x*chord.x+chord.y*chord.y;
        if(norm>1e-12) arcDrag.angle=std::clamp(arcDrag.sign*4*std::atan(2*(chord.x*cursor.y-chord.y*cursor.x)/norm)*180/pi,-330.0,330.0);
    }
    RectD previous{};vf<Bounds>(arrow,0x1c8)(arrow,&previous);
    arcDrag.running=true;oldArcMove(tracker);arcDrag.running=false;
    // SetStartAndEnd and SetArcAngle now publish the pinned geometry before
    // native Draw. Copy damage includes every area occupied by the old shaft.
    RectD current{};vf<Bounds>(arrow,0x1c8)(arrow,&current);
    fn<void(*)(Obj,const RectD*)>(0x1464e0)(tracker,&arcDrag.initialBounds);
    fn<void(*)(Obj,const RectD*)>(0x1464e0)(tracker,&previous);
    fn<void(*)(Obj,const RectD*)>(0x1464e0)(tracker,&current);
}
short arcFinish(Obj tracker) {
    if(arcDrag.tracker!=tracker) return oldArcFinish(tracker);
    Obj arrow=arcDrag.arrow;
    if(arcDrag.cancelled) {
        UndoOff off(arrow);arcDrag.running=true;arcDrag.angle=arcDrag.initial;
        fn<void(*)(Obj,double)>(0x2a6c00)(arrow,arcDrag.initial);
        fn<void(*)(Obj,const Point*,const Point*)>(0x2a8260)(arrow,&arcDrag.localHead,&arcDrag.localTail);
        arcDrag.running=false;
    } else {
        Shape s;s.angle=arcDrag.angle;s.tail=arcDrag.tail;s.head=arcDrag.head;
        s.points=arcPoints(s.angle);s.smooth.assign(s.points.size()/3,1);
        setTag(arrow,tagName,encode(s)); // Explicit zero remains straight on later moves.
    }
    arcDrag={};return oldArcFinish(tracker);
}
Obj arcDestructor(Obj tracker,unsigned flags) {if(arcDrag.tracker==tracker) arcDrag={};return oldArcDestructor(tracker,flags);}

Obj splineCtor(Obj tracker,const Point* point,Obj curve,int index,bool flag,const Point* extra) {
    Pair pair=findPair(curve);
    if(pair.curve==curve) {
        syncCurve(curve);curveDrag={tracker,curve,drawingWindow(at<Obj>(curve,0x60)),curvePoints(curve),index};
        pickedPoints[curve]=index;
        fn<Unary>(0x349920)(curve); // Same native input transaction as SplineTracker.
    }
    return oldSplineCtor(tracker,point,curve,index,flag,extra);
}
void setPoints(Obj curve,const std::vector<Point>* value) {
    if(movingCurve!=curve||curveDrag.curve!=curve||curveDrag.original.empty()) {oldSetPoints(curve,value);return;}
    auto p=*value;const auto& original=curveDrag.original;
    if(p.size()!=original.size()||p.size()<6) {oldSetPoints(curve,value);return;}
    const size_t end=p.size()-2;
    Point dt=sub(original[1],p[1]),dh=sub(original[end],p[end]);
    for(size_t i=0;i<3;++i) p[i]=add(p[i],dt);
    for(size_t i=p.size()-3;i<p.size();++i) p[i]=add(p[i],dh);
    p[1]=original[1];p[end]=original[end];
    if(curveDrag.index==1||curveDrag.index==int(end)) {
        size_t first=curveDrag.index==1?0:p.size()-3;for(size_t i=first;i<first+3;++i) p[i]=original[i];
    }
    const int index=curveDrag.index;
    if(index>=0&&size_t(index)<p.size()&&index%3!=1) {
        const auto corners=tagString(curve,cornersTag);const size_t node=size_t(index)/3;
        const size_t opposite=index%3==0?size_t(index)+2:size_t(index)-2;
        if(node<corners.size()&&corners[node]=='0'&&opposite<p.size()) p[opposite]=original[opposite];
    }
    if(auto pair=findPair(curve);pair.arrow) {
        Point tail=anchor(pair.arrow,at<int>(pair.arrow,0x1f0),p[1]);
        Point head=anchor(pair.arrow,at<int>(pair.arrow,0x1f4),p[p.size()-2]);
        applyCurveClearance(pair.arrow,p,tail,head);
    }
    // Keep ChemDraw's tracker copy consistent with the pinned native geometry.
    auto first=at<Point*>(curveDrag.tracker,0x140),last=at<Point*>(curveDrag.tracker,0x148);
    if(first&&last-first==ptrdiff_t(p.size())) std::copy(p.begin(),p.end(),first);
    oldSetPoints(curve,&p);
}
void splineMove(Obj tracker) {
    Obj curve=at<Obj>(tracker,0x138);
    if(curveDrag.tracker!=tracker) {oldSplineMove(tracker);return;}
    Obj previous=movingCurve;movingCurve=curve;
    int index=at<int>(tracker,0x158);std::string corners=tagString(curve,cornersTag);size_t node=size_t(std::max(index,0))/3;
    if(node<corners.size()&&index%3!=1) at<uint8_t>(tracker,0x17c)=corners[node]=='1'?0:1;
    oldSplineMove(tracker);movingCurve=previous;
}
short splineFinish(Obj tracker) {
    if(curveDrag.tracker==tracker) {
        if(curveDrag.cancelled) {UndoOff off(curveDrag.curve);oldSetPoints(curveDrag.curve,&curveDrag.original);}
        Obj previous=movingCurve;movingCurve=curveDrag.curve;short result=oldSplineFinish(tracker);movingCurve=previous;
        curveDrag={};return result;
    }
    return oldSplineFinish(tracker);
}
Obj splineDestructor(Obj tracker,unsigned flags) {if(curveDrag.tracker==tracker) curveDrag={};return oldSplineDestructor(tracker,flags);}

Obj document(HWND w) {
    for(HWND p=w;p;p=GetParent(p)) {Obj doc=fn<Obj(*)(LONG_PTR)>(0x3e75c0)(GetWindowLongPtrW(p,GWLP_USERDATA));if(doc&&mainPage(doc)) return doc;}
    return nullptr;
}
Obj selectedPath(Obj page) {
    Obj set=fn<Obj(*)(Obj)>(0xf8af0)(static_cast<std::byte*>(page)+0x2b0);if(!set) return nullptr;
    auto head=at<TreeNode*>(set,0);size_t count=at<size_t>(set,8);Obj selected{};
    if(!head) return nullptr;
    for(auto n=head->left;n!=head&&count--;n=nextNode(n)) {
        Obj o=n->object;
        if(isArrow(o)||findPair(o).group) {
            if(selected&&selected!=o) {
                Obj first=findPair(selected).group,second=findPair(o).group;
                if(!first||first!=second) return nullptr;
            }
            selected=o;
        }
    }
    return selected;
}
bool cursorPoint(Obj doc,HWND window,int x,int y,Obj& actual,Point& point) {
    Obj page=mainPage(doc),port=at<Obj>(doc,0x258);if(!page||!port) return false;
    POINT client{x,y};if(window!=portWindow(port)) {ClientToScreen(window,&client);ScreenToClient(portWindow(port),&client);}
    struct PointI {int x,y,z;} p{client.x+at<int>(port,0xa8),client.y+at<int>(port,0xac),0};
    actual=fn<Obj(*)(Obj,const PointI*,Point*,bool)>(0x3e3570)(page,&p,&point,false);return actual&&finite(point);
}
Obj pathUnderCursor(Obj doc,HWND window,Point& point,Obj& page,int x,int y) {
    if(!cursorPoint(doc,window,x,y,page,point)) return nullptr;
    Obj hit=fn<Obj(*)(Obj,const Point*,Obj)>(0x711500)(page,&point,nullptr);
    if(isArrow(hit)||findPair(hit).group) return hit;
    // Pen handles can extend past the curve's shaft hit bounds.
    if(fn<int(*)()>(0x4f4ba0)()!=8) return nullptr;
    Obj current=at<Obj>(page,0x15a8);return findPair(current).curve==current?current:nullptr;
}
bool closestShaft(const std::vector<Point>& p,Point point,double limit,size_t& segment,double& parameter) {
    if(p.size()<6) return false;
    double best=limit;bool found=false;
    for(size_t i=1;i+3<p.size();i+=3) {
        auto distance=[&](double t) {return length(sub(point,cubic(p[i],p[i+1],p[i+2],p[i+3],t)));};
        double nearest=std::numeric_limits<double>::infinity();int sample{};
        for(int j=0;j<=64;++j) {double d=distance(double(j)/64);if(d<nearest) {nearest=d;sample=j;}}
        double low=double(std::max(0,sample-1))/64,high=double(std::min(64,sample+1))/64;
        for(int j=0;j<24;++j) {
            double first=(2*low+high)/3,last=(low+2*high)/3;
            if(distance(first)<distance(last)) high=last;else low=first;
        }
        double t=(low+high)*0.5,d=distance(t);
        if(d<best) {best=d;parameter=t;segment=i/3;found=true;}
    }
    return found;
}
bool penShaftPress(Obj doc,HWND window,POINT screen) {
    if(fn<int(*)()>(0x4f4ba0)()!=8) return false;
    POINT client=screen;if(!ScreenToClient(window,&client)) return false;
    Point point{};Obj page{};if(!cursorPoint(doc,window,client.x,client.y,page,point)) return false;
    Obj curve=at<Obj>(page,0x15a8);if(!curve||findPair(curve).curve!=curve) return false;
    auto p=curvePoints(curve);double units=dipUnits(page);
    // Let native knots/handles start their usual tracker. A press on the shaft
    // itself must not start a different spline before the second click arrives.
    for(size_t i=1;i+1<p.size();++i) if(length(sub(point,p[i]))<6*units) return false;
    size_t segment{};double parameter{};return closestShaft(p,point,8*units,segment,parameter);
}
bool insertPoint(Obj curve,Point point) {
    auto p=curvePoints(curve);if(p.empty()||p.size()/3>=maxNodes) return false;
    const double units=dipUnits(at<Obj>(curve,0x60));
    for(size_t i=1;i<p.size();i+=3) if(length(sub(point,p[i]))<3*units) {
        pickedPoints[curve]=int(i);selectCurve(curve);return true;
    }
    double parameter{};size_t segment{};
    if(!closestShaft(p,point,8*units,segment,parameter)) return false;
    size_t i=segment*3+1;double t=parameter;
    Point a=mix(p[i],p[i+1],t),b=mix(p[i+1],p[i+2],t),c=mix(p[i+2],p[i+3],t);
    Point d=mix(a,b,t),e=mix(b,c,t),f=mix(d,e,t);
    p[i+1]=a;p[i+2]=c;p.insert(p.begin()+ptrdiff_t(i+2),{d,f,e});
    UndoRecord record(curve);fn<Unary>(0x349920)(curve);oldSetPoints(curve,&p);
    auto corners=tagString(curve,cornersTag);if(corners.size()!=p.size()/3-1) corners.assign(p.size()/3-1,'1');
    corners.insert(corners.begin()+ptrdiff_t(segment+1),'1');setTag(curve,cornersTag,corners);
    pickedPoints[curve]=int((segment+1)*3+1);selectCurve(curve);return true;
}
bool editPoint(Obj curve,bool erase) {
    auto picked=pickedPoints.find(curve);if(picked==pickedPoints.end()||picked->second<0) return false;
    auto p=curvePoints(curve);size_t node=size_t(picked->second)/3;
    if(p.empty()||node==0||node+1>=p.size()/3) return false;
    auto corners=tagString(curve,cornersTag);if(corners.size()!=p.size()/3) corners.assign(p.size()/3,'1');
    UndoRecord record(curve);fn<Unary>(0x349920)(curve);
    if(erase) {p.erase(p.begin()+ptrdiff_t(node*3),p.begin()+ptrdiff_t(node*3+3));corners.erase(corners.begin()+ptrdiff_t(node));pickedPoints.erase(picked);}
    else {
        corners[node]=corners[node]=='1'?'0':'1';
        if(corners[node]=='1') {
            size_t k=node*3+1;Point direction=sub(p[k+1],p[k-1]);double n=length(direction);
            if(n>1e-8) {direction=mul(direction,1/n);double in=length(sub(p[k],p[k-1])),out=length(sub(p[k+1],p[k]));
                p[k-1]=sub(p[k],mul(direction,in));p[k+1]=add(p[k],mul(direction,out));}
        }
    }
    oldSetPoints(curve,&p);setTag(curve,cornersTag,corners);redraw(curve);return true;
}
template<class F> void trackerHook(size_t rva,F replacement,F& original) {
    auto status=createHook(reinterpret_cast<void*>(resolveDetour(L"ChemDrawUI.dll",uint32_t(rva))),reinterpret_cast<void*>(replacement),reinterpret_cast<void**>(&original));
    if(status!=MH_OK) throw std::runtime_error("Could not install native arrow editing tracker");
}
}
void rememberEditableArrow(Obj arrow) {if(onUI()&&isArrow(arrow)) lastArrows.insert_or_assign(at<Obj>(arrow,0x60),identity(arrow));}
bool arrowWorldEndpoints(Obj arrow,Point& tail,Point& head) {
    if(!isArrow(arrow)) return false;
    fn<void(*)(Obj,Point*)>(0x2a4c90)(arrow,&tail);
    fn<void(*)(Obj,Point*)>(0xdc4d0)(arrow,&head);
    return finite(tail)&&finite(head);
}
bool arrowEndpointTangents(Obj arrow,Point& tail,Point& head,Point& tailOut,Point& headOut) {
    if(!onUI()||!isArrow(arrow))return false;
    UndoOff undo(arrow);
    const auto pair=findPair(arrow);
    Shape shape;
    if(!pair.curve&&std::abs(at<double>(arrow,0x130))<1e-9&&
        (!readShape(arrow,shape)||!shape.custom)&&arrowWorldEndpoints(arrow,tail,head)) {
        const Point chord=sub(head,tail);
        if(length(chord)<1e-6)return false;
        headOut=unitDirection(chord,{});tailOut=mul(headOut,-1);
        return true;
    }
    auto points=pair.curve?curvePoints(pair.curve):initialPath(arrow);
    if(points.size()<6)return false;
    tail=points[1];head=points[points.size()-2];
    const Point chord=sub(head,tail);
    if(length(chord)<1e-6)return false;
    tailOut=unitDirection(sub(tail,points[2]),mul(chord,-1));
    headOut=unitDirection(sub(head,points[points.size()-3]),chord);
    return finite(tail)&&finite(head)&&finite(tailOut)&&finite(headOut);
}
bool drawAdjustments(Obj object,Obj highlighter,bool first,bool second) {
    // Adjustment artwork is emitted separately from CDArrow::DrawGraphic.
    // Give it the same owner so transparent resize previews retain its handles
    // and discard the previous handle positions along with the old shaft.
    GpuObjectScope owner(onUI()&&isArrow(object)?object:nullptr);
    return oldDrawAdjustment(object,highlighter,first,second);
}
bool standardArrowAttachments(Obj arrow) {return installed&&isArrow(arrow)&&tagString(arrow,standardTag)=="1";}
bool mechanismArrowAttachments(Obj arrow) {
    return installed&&isArrow(arrow)&&tagString(arrow,"Zuonik/SmartArrow/Mechanism/v1")=="1";
}
void rememberMechanismArrow(Obj arrow) {
    if(installed&&isArrow(arrow)&&!mechanismArrowAttachments(arrow)) {
        UndoOff off(arrow);
        setTag(arrow,"Zuonik/SmartArrow/Mechanism/v1","1");
    }
}
void rememberStandardArrow(Obj arrow) {
    if(installed&&isArrow(arrow)&&!standardArrowAttachments(arrow)) setTag(arrow,standardTag,"1");
}
bool arrowPathTransient(Obj object) {return object==transientSpline;}
bool nativeArrowAngle(Obj arrow,double& angle) {if(arcDrag.tracker&&arcDrag.arrow==arrow) {angle=arcDrag.angle;return true;}return false;}
double arrowPageArcAngle(Obj arrow,double angle) {
    // CDPlanarObject::Flip mirrors its two plane axes. Reading these axes
    // directly avoids matrix calculation and its EnsureValid callback.
    const double determinant=at<double>(arrow,0xd8)*at<double>(arrow,0xf8)-
        at<double>(arrow,0xe0)*at<double>(arrow,0xf0);
    return std::isfinite(determinant)&&determinant<0?-angle:angle;
}
bool setMirroredArrowEndpoints(Obj arrow,const Point* head,const Point* tail) {
    const double localAngle=at<double>(arrow,0x130),pageAngle=arrowPageArcAngle(arrow,localAngle);
    if(!installed||!std::isfinite(localAngle)||localAngle==0||pageAngle==localAngle) return false;
    // SetStartAndTail computes its circle center using an unmirrored frame.
    // Publish the center in page space through its native SetStartAndEnd body,
    // while retaining the actual local arc angle and the reflected plane axes.
    const double half=pageAngle*pi/360,s=std::sin(half);
    if(std::abs(s)<1e-8) return false;
    const Point chord=sub(*head,*tail);
    const double cot=std::cos(half)/s;
    Point center=mix(*head,*tail,0.5);
    center.x+=chord.y*0.5*cot;center.y-=chord.x*0.5*cot;
    if(!finite(center)) return false;
    oldStartEnd(arrow,head,&center);return true;
}
void arrowPathFlipped(Obj arrow) {
    if(!installed||!onUI()||!isArrow(arrow)) return;
    Shape s;
    if(readShape(arrow,s)&&!s.custom&&s.angle!=at<double>(arrow,0x130)) {
        s.angle=at<double>(arrow,0x130);setTag(arrow,tagName,encode(s));
    }
    arrowPathInvalidated(arrow);
}
void insetArrowEndpoints(Obj arrow,Point& tail,Point& head,double angle) {
    arrowInsertionAttachment(arrow,true,tail);arrowInsertionAttachment(arrow,false,head);
    const double gap=endpointClearance(arrow,tail,head);
    Point chord=unitDirection(sub(head,tail),{});
    if(gap<=0||!std::isfinite(angle)) return;
    const double half=std::clamp(arrowPageArcAngle(arrow,angle),-330.0,330.0)*pi/360;
    const double c=std::cos(half),s=std::sin(half);
    // Native positive page-space angles bend below a left-to-right chord.
    // Use the current handedness for the outgoing tail and incoming tip.
    Obj page=at<Obj>(arrow,0x60);
    if(offsetAttachment(arrow,at<int>(arrow,0x1f0)))
        tail=arrowAttachmentPoint(fn<Obj(*)(Obj,int)>(0x712f80)(page,at<int>(arrow,0x1f0)),tail,
            {chord.x*c-chord.y*s,chord.y*c+chord.x*s,0},gap);
    if(offsetAttachment(arrow,at<int>(arrow,0x1f4)))
        head=arrowAttachmentPoint(fn<Obj(*)(Obj,int)>(0x712f80)(page,at<int>(arrow,0x1f4)),head,
            {-chord.x*c-chord.y*s,-chord.y*c+chord.x*s,0},gap);
}
bool storedArrowAngle(Obj arrow,double& angle) {
    if(!installed) return false;
    if(nativeArrowAngle(arrow,angle)) return true;
    Shape s;if(!readShape(arrow,s)) return false;angle=s.angle;return true;
}
void arrowPathInvalidated(Obj object) {
    if(!installed||syncing||movingCurve||!isArrow(object)) return;
    if(auto p=findPair(object);p.curve&&!at<uint8_t>(p.curve,0x30)) {
        syncing=true;fn<Unary>(0x328a90)(p.curve);syncing=false;
    }
}
bool cancelArrowPathTracking(HWND w) {
    bool cancelled=false;
    if(arcDrag.tracker&&arcDrag.window==w) {arcDrag.cancelled=true;cancelled=true;}
    if(curveDrag.tracker&&curveDrag.window==w) {curveDrag.cancelled=true;cancelled=true;}
    return cancelled;
}
bool arrowPathShortcut(HWND w,UINT message,WPARAM key,LPARAM flags) {
    if(!installed||trackingDepth||(message!=WM_KEYDOWN&&message!=WM_SYSKEYDOWN)) return false;
    Obj doc{},page{};
    const bool mode=key=='P'&&(GetKeyState(VK_CONTROL)&0x8000)&&(GetKeyState(VK_MENU)&0x8000);
    const bool pointKey=key==VK_DELETE||(key=='S'&&!(GetKeyState(VK_CONTROL)&0x8000)&&!(GetKeyState(VK_MENU)&0x8000));
    if(!mode&&!pointKey) return false;
    doc=document(w);page=doc?mainPage(doc):nullptr;if(!page) return false;
    if(flags&(1LL<<30)) return mode;
    try {
        if(pointKey) {
            Obj curve=at<Obj>(page,0x15a8);
            if(fn<int(*)()>(0x4f4ba0)()!=8||findPair(curve).curve!=curve) return false;
            return editPoint(curve,key==VK_DELETE);
        }
        Obj object=selectedPath(page);
        if(!object) {POINT p{};GetCursorPos(&p);ScreenToClient(w,&p);Point point{};Obj actual{};object=pathUnderCursor(doc,w,point,actual,p.x,p.y);}
        if(!object) if(auto last=lastArrows.find(page);last!=lastArrows.end()) object=resolve(last->second);
        if(object) {
            if(GetKeyState(VK_SHIFT)&0x8000) selectArc(object);
            else {Pair pair=findPair(object);Obj curve=pair.curve?pair.curve:nativePath(object);if(curve) selectCurve(curve);}
        }
    } catch(const std::exception&) {writeArrowStatus("Native path edit could not be completed.\r\n");}
    return true;
}
uint64_t arrowPathTrackingObject(Obj tracker) {
    if(!installed||!onUI()||!tracker) return 0;
    const auto move=at<uintptr_t*>(tracker,0)[0x38/8];
    if(move==nativeUI+0x25cd40) {
        Obj arrow=at<Obj>(tracker,0x138);return regularArrowTracker(tracker)&&arrow?at<uint64_t>(arrow,0xb0):0;
    }
    if(move==nativeUI+0x25b720) {
        Obj arrow=at<Obj>(tracker,0x138);return arrow?at<uint64_t>(arrow,0xb0):0;
    }
    if(curveDrag.tracker==tracker&&curveDrag.curve) return at<uint64_t>(curveDrag.curve,0xb0);
    return 0;
}
bool arrowPathTrackingPreview(Obj object) {
    return (arcDrag.running&&arcDrag.arrow==object)||movingCurve==object||regularArrowPreview(object);
}
void arrowPathDrawingStarted() {shaftClick={};}
bool arrowPathQueuedInput(MSG& msg) {
    if(!installed||!onUI()) return false;
    const UINT m=msg.message;
    UINT dpi=msg.hwnd?GetDpiForWindow(msg.hwnd):96;if(!dpi) dpi=96;
    const int dx=std::max(1,GetSystemMetricsForDpi(SM_CXDOUBLECLK,dpi)/2);
    const int dy=std::max(1,GetSystemMetricsForDpi(SM_CYDOUBLECLK,dpi)/2);
    if(m==WM_MOUSEMOVE||m==WM_LBUTTONUP) {
        if(shaftClick.valid&&(std::abs(msg.pt.x-shaftClick.point.x)>dx||std::abs(msg.pt.y-shaftClick.point.y)>dy)) shaftClick={};
        return false;
    }
    if(m==WM_KEYDOWN||m==WM_SYSKEYDOWN||m==WM_RBUTTONDOWN||m==WM_CANCELMODE) {shaftClick={};return false;}
    if(trackingDepth||!msg.hwnd||(m!=WM_LBUTTONDOWN&&m!=WM_LBUTTONDBLCLK)) return false;
    // ChemDraw may count ordinary presses itself without CS_DBLCLKS. Use
    // queued event times and screen points, before its modal tracker consumes
    // the second press, and also accept Windows' explicit double-click message.
    bool twice=m==WM_LBUTTONDBLCLK||(shaftClick.valid&&shaftClick.window==msg.hwnd&&
        DWORD(msg.time-shaftClick.time)<=GetDoubleClickTime()&&
        std::abs(msg.pt.x-shaftClick.point.x)<=dx&&std::abs(msg.pt.y-shaftClick.point.y)<=dy);
    shaftClick={msg.hwnd,msg.time,msg.pt,true};
    if(!twice) {
        try {Obj doc=document(msg.hwnd);return doc&&penShaftPress(doc,msg.hwnd,msg.pt);}
        catch(const std::exception&) {return false;}
    }
    shaftClick={};
    Obj doc=document(msg.hwnd);if(!doc) return false;
    const int tool=fn<int(*)()>(0x4f4ba0)();
    if(tool!=1&&tool!=6&&tool!=8&&tool!=0x27) return false;
    POINT client=msg.pt;if(!ScreenToClient(msg.hwnd,&client)) return false;
    Point point{};Obj page{};
    try {
        if(!cursorPoint(doc,msg.hwnd,client.x,client.y,page,point)) return false;
        if(nativeArrowTool(tool)&&arrowChemistryTarget(page,point)) {
            // A second draw from the same atom is a drawing press, including
            // Windows' double-click message after a rapid previous gesture.
            if(msg.message==WM_LBUTTONDBLCLK) msg.message=WM_LBUTTONDOWN;
            return false;
        }
        Obj object=pathUnderCursor(doc,msg.hwnd,point,page,client.x,client.y);if(!object) return false;
        Pair pair=findPair(object);
        auto points=pair.curve?curvePoints(pair.curve):initialPath(object);
        if(points.size()<6) return false;
        const double units=dipUnits(page);
        // Endpoint presses belong to drawing/endpoint editing. Only an actual
        // shaft hit can consume the press and enter the native Pen tool.
        if(length(sub(point,points[1]))<6*units||length(sub(point,points[points.size()-2]))<6*units) return false;
        size_t segment{};double parameter{};
        if(!closestShaft(points,point,8*units,segment,parameter)) return false;
        // A shaft double-click enters native Pen editing and adds a knot in
        // either arc or custom-path mode, in one native undo transaction.
        UndoRecord record(pair.curve?pair.curve:object);
        Obj curve=pair.curve?pair.curve:nativePath(object);if(!curve) return false;
        bool inserted=insertPoint(curve,point);selectCurve(curve);
        queueGpuDrawingCommit(doc);
        if(inserted) writeArrowStatus("Native shaft double-click: knot inserted.\r\n");
        return true;
    } catch(const std::exception&) {writeArrowStatus("Native shaft double-click could not be completed.\r\n");}
    return false;
}
void forgetArrowPaths(Obj page) {
    lastArrows.erase(page);
    if(arcDrag.arrow&&at<Obj>(arcDrag.arrow,0x60)==page) arcDrag={};
    if(curveDrag.curve&&at<Obj>(curveDrag.curve,0x60)==page) curveDrag={};
    for(auto i=pickedPoints.begin();i!=pickedPoints.end();) {if(at<Obj>(i->first,0x60)==page) i=pickedPoints.erase(i);else ++i;}
}
void forgetArrowPathObject(Obj object) {
    pickedPoints.erase(object);if(arcDrag.arrow==object) arcDrag={};if(curveDrag.curve==object) curveDrag={};
}
void clearNativeArrowEditing() {arcDrag={};curveDrag={};shaftClick={};pickedPoints.clear();installed=false;}
void installArrowPaths() {
    nativeUI=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ChemDrawUI.dll"));
    // Revision 57 tags still render while an older arrow awaits native conversion.
    hook(0x29cab0,draw,oldDraw);hook(0x2a2580,bounds,oldBounds);hook(0x2a5a90,pathNear,oldNear);hook(0x2a9190,validate,oldValidate);
    hook(0x346e40,curveDraw,oldCurveDraw);
    hook(0xa5ba0,setPoints,oldSetPoints);hook(0x34c4e0,curveValidate,oldCurveValidate);
    hook(0x325cd0,pathDeleted,oldDelete);hook(0xf2240,canUngroup,oldCanUngroup);
    hook(0x29ed20,adjustments,oldAdjustment);hook(0x326140,drawAdjustments,oldDrawAdjustment);
    hook(0x2a2c00,subHit,oldSubHit);hook(0x2a7bc0,startEnd,oldStartEnd);
    trackerHook(0x25b720,arcMove,oldArcMove);trackerHook(0x25b100,arcFinish,oldArcFinish);trackerHook(0x25aea0,arcDestructor,oldArcDestructor);
    trackerHook(0x2562e0,splineCtor,oldSplineCtor);trackerHook(0x256ed0,splineMove,oldSplineMove);
    trackerHook(0x256b50,splineFinish,oldSplineFinish);trackerHook(0x2567b0,splineDestructor,oldSplineDestructor);
    if (patchEnabled(16)) installArrowInsertion();
    installed=true;
}
}

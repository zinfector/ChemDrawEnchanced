// Shares the native arrow/spline helpers in arrow-path.cpp. Preview descriptions
// are immutable GPU scenes; only native release finalization edits the page.
namespace cd {
namespace {
const std::string insertionTailTag="Zuonik/ArrowInsertion/Tail/v1";
const std::string insertionHeadTag="Zuonik/ArrowInsertion/Head/v1";
struct InsertCandidate {
    Identity arrow;Identity visible;
    RectD bounds{};
    std::vector<Point> path;
    bool curved{},loaded{};
};
struct InsertPlan {
    size_t candidate{SIZE_MAX};
    Point offset{};
    RectD molecule{};
    std::vector<Point> first,second;
};
struct InsertionGesture {
    Obj tracker{},doc{},page{};
    Identity molecule;
    RectD initial{};
    Point down{};
    HWND window{};
    double units{};
    bool released{},finishing{},committing{};
    std::vector<InsertCandidate> candidates;
    InsertPlan plan;
    uint64_t filteredOwner{},filteredCurve{};
    using Filtered=std::pair<std::shared_ptr<const GpuScene>,std::shared_ptr<const GpuScene>>;
    std::unordered_map<const GpuScene*,Filtered> filtered;
};
thread_local InsertionGesture insertion;
struct InsertionTrace {
    bool active{},gpu{},armed{},released{},prepared{},nativeFinish{},inserted{};
    uintptr_t table{},callback{};
    unsigned copy{},moves{},paths{},splits{},previews{},invalidations{};
    size_t candidates{};
    double units{},distance{},firstLength{},secondLength{},minimum{};
    const char* stage="tracking entry";
    const char* rejection="none";
};
thread_local InsertionTrace insertionTrace;
using DragFinish=short(*)(Obj);
DragFinish oldInsertionFinish{};

Obj resolveInsertObject(const Identity& id) {
    Obj object=id.page&&id.id>=0?fn<Obj(*)(Obj,int)>(0x712f80)(id.page,id.id):nullptr;
    // Custom native paths have a hidden carrier arrow (Visible at 0x34).
    // Visibility does not indicate whether the page object is still alive.
    return object&&!at<uint8_t>(object,0x30)&&
        at<uint64_t>(object,0xb0)==id.instance?object:nullptr;
}
bool insertionArrow(Obj arrow) {
    if(!isArrow(arrow)||at<uint8_t>(arrow,0x30))return false;
    if(!fn<bool(*)(Obj)>(0xdc5d0)(arrow)||fn<bool(*)(Obj)>(0xdc690)(arrow))return false;
    // IsElectronPushArrow only tests attachment IDs. Ordinary palette arrows
    // deliberately have those IDs too; use the explicit palette role instead.
    if(!standardArrowAttachments(arrow)&&mechanismArrowAttachments(arrow))return false;
    for(size_t rva:{size_t(0xdc970),size_t(0xdc880),size_t(0xdcac0),
        size_t(0xdca80),size_t(0xdcbb0),size_t(0xdc930),size_t(0xdcb50),size_t(0xdcb60)})
        if(fn<bool(*)(Obj)>(rva)(arrow))return false;
    return true;
}
RectD translated(RectD r,Point shift) {return {r.t+shift.y,r.l+shift.x,r.b+shift.y,r.r+shift.x};}
bool inside(RectD r,Point p) {return p.x>=r.l&&p.x<=r.r&&p.y>=r.t&&p.y<=r.b;}
double pathSegments(const std::vector<Point>& p) {return double(p.size()/3-1);}
Point pathPoint(const std::vector<Point>& p,double u) {
    const size_t segments=p.size()/3-1;
    const size_t segment=std::min(size_t(std::max(0.0,std::floor(u))),segments-1);
    const double t=std::clamp(u-double(segment),0.0,1.0);
    return cubic(p[segment*3+1],p[segment*3+2],p[segment*3+3],p[segment*3+4],t);
}
struct CubicPart {Point a,b,c,d;};
std::pair<CubicPart,CubicPart> divide(CubicPart p,double t) {
    const Point ab=mix(p.a,p.b,t),bc=mix(p.b,p.c,t),cd=mix(p.c,p.d,t);
    const Point abc=mix(ab,bc,t),bcd=mix(bc,cd,t),middle=mix(abc,bcd,t);
    return {{p.a,ab,abc,middle},{middle,bcd,cd,p.d}};
}
std::vector<Point> trimInsertionPath(const std::vector<Point>& p,double from,double to) {
    std::vector<Point> result;
    if(to-from<1e-9)return result;
    const size_t segments=p.size()/3-1;
    for(size_t i=size_t(std::max(0.0,std::floor(from)));i<segments&&double(i)<to;++i) {
        const double lo=std::max(0.0,from-double(i)),hi=std::min(1.0,to-double(i));
        if(hi-lo<1e-9)continue;
        CubicPart part{p[i*3+1],p[i*3+2],p[i*3+3],p[i*3+4]};
        if(hi<1)part=divide(part,hi).first;
        if(lo>0)part=divide(part,lo/hi).second;
        if(result.empty())result={part.a,part.a,part.b};
        else result.back()=part.b;
        result.push_back(part.c);result.push_back(part.d);result.push_back(part.d);
    }
    return result;
}
double insertionLength(const std::vector<Point>& p) {
    if(p.size()<6)return 0;
    double total{};Point previous=p[1];
    for(size_t i=0;i<p.size()/3-1;++i)for(unsigned j=1;j<=24;++j) {
        const Point next=pathPoint(p,double(i)+double(j)/24);
        total+=length(sub(next,previous));previous=next;
    }
    return total;
}
double closestInsertionPoint(const std::vector<Point>& p,Point center,double& distance) {
    double best=std::numeric_limits<double>::infinity(),parameter{};
    const double segments=pathSegments(p);
    Point previous=p[1];
    for(size_t i=0;i<size_t(segments);++i)for(unsigned j=1;j<=32;++j) {
        const double u=double(i)+double(j)/32;
        const Point next=pathPoint(p,u),direction=sub(next,previous),relative=sub(center,previous);
        const double norm=direction.x*direction.x+direction.y*direction.y;
        const double fraction=norm>1e-12?std::clamp((relative.x*direction.x+relative.y*direction.y)/norm,0.0,1.0):0;
        const double d=length(sub(center,mix(previous,next,fraction)));
        if(d<best){best=d;parameter=u-(1-fraction)/32;}
        previous=next;
    }
    const double segment=std::min(std::floor(parameter),segments-1);
    double lo=std::max(segment,parameter-1.0/32),hi=std::min(segment+1,parameter+1.0/32);
    for(unsigned iteration=0;iteration<24;++iteration) {
        const double a=lo+(hi-lo)/3,b=hi-(hi-lo)/3;
        if(length(sub(pathPoint(p,a),center))<length(sub(pathPoint(p,b),center)))hi=b;else lo=a;
    }
    parameter=(lo+hi)*.5;distance=length(sub(pathPoint(p,parameter),center));return parameter;
}
bool splitInsertionPath(InsertCandidate& candidate,RectD bounds,double units,InsertPlan& plan) {
    const Point center{(bounds.l+bounds.r)*.5,(bounds.t+bounds.b)*.5,0};
    double distance{};const double middle=closestInsertionPoint(candidate.path,center,distance);
    const Point projected=pathPoint(candidate.path,middle);
    plan.offset=sub(projected,center);plan.molecule=translated(bounds,plan.offset);
    Obj arrow=resolveInsertObject(candidate.arrow);if(!arrow)return false;
    const double gap=std::max(3*units,endpointClearance(arrow,candidate.path[1],candidate.path[candidate.path.size()-2]));
    RectD expanded{plan.molecule.t-gap,plan.molecule.l-gap,plan.molecule.b+gap,plan.molecule.r+gap};
    const double end=pathSegments(candidate.path);
    auto boundary=[&](bool after,double& out) {
        double inner=middle,outer=middle;
        for(unsigned step=0;step<=unsigned(end*32)+1;++step) {
            outer=std::clamp(outer+(after?1.0/32:-1.0/32),0.0,end);
            if(!inside(expanded,pathPoint(candidate.path,outer))) {
                for(unsigned n=0;n<32;++n) {
                    const double midpoint=(inner+outer)*.5;
                    if(inside(expanded,pathPoint(candidate.path,midpoint)))inner=midpoint;else outer=midpoint;
                }
                out=outer;return true;
            }
            inner=outer;
            if(outer==0||outer==end)return false;
        }
        return false;
    };
    double entry{},exit{};if(!boundary(false,entry)||!boundary(true,exit)||exit<=entry) {
        insertionTrace.rejection="molecule gap reaches an outer endpoint";return false;
    }
    plan.first=trimInsertionPath(candidate.path,0,entry);plan.second=trimInsertionPath(candidate.path,exit,end);
    // Reject only unusably short shafts. Native arrowhead settings are style
    // parameters; the native renderer owns their geometry, not this hit gate.
    const double minimum=12*units;
    insertionTrace.firstLength=insertionLength(plan.first);insertionTrace.secondLength=insertionLength(plan.second);
    insertionTrace.minimum=minimum;
    if(insertionTrace.firstLength<minimum||insertionTrace.secondLength<minimum) {
        insertionTrace.rejection="remaining arrow leg too short";return false;
    }
    // Highly looping paths must not enter the intermediate a second time.
    for(const auto* leg:{&plan.first,&plan.second})for(size_t i=0;i<leg->size()/3-1;++i)
        for(unsigned j=1;j<24;++j)if(inside(plan.molecule,pathPoint(*leg,double(i)+double(j)/24)))return false;
    return true;
}

std::string boundaryTag(RectD bounds,Point point) {
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17)<<"1 "
        <<(point.x-bounds.l)/(bounds.r-bounds.l)<<' '<<(point.y-bounds.t)/(bounds.b-bounds.t);
    return out.str();
}
void copyInsertionStyle(Obj source,Obj duplicate) {
    fn<void(*)(Obj,int)>(0x296810)(duplicate,fn<int(*)(Obj)>(0x2a3bf0)(source));
    fn<void(*)(Obj,Obj)>(0x32aef0)(duplicate,static_cast<std::byte*>(source)+0x80);
    Obj color=vf<Obj(*)(Obj,int)>(source,0x48)(source,0x301);
    if(color)vf<void(*)(Obj,int,Obj)>(duplicate,0x50)(duplicate,0x301,color);
    fn<void(*)(Obj,int)>(0x2a6c80)(duplicate,fn<int(*)(Obj)>(0xdc440)(source));
    fn<void(*)(Obj,int)>(0x2a7590)(duplicate,fn<int(*)(Obj)>(0x8e5f0)(source));
    fn<void(*)(Obj,int)>(0x2a7660)(duplicate,fn<int(*)(Obj)>(0xdc520)(source));
    fn<void(*)(Obj,double)>(0x2a7510)(duplicate,fn<double(*)(Obj)>(0xdc4c0)(source));
    fn<void(*)(Obj,double)>(0x2a7740)(duplicate,fn<double(*)(Obj)>(0xdc530)(source));
    fn<void(*)(Obj,double)>(0x2a77c0)(duplicate,fn<double(*)(Obj)>(0xdc540)(source));
    fn<void(*)(Obj,bool)>(0x2a6d00)(duplicate,fn<bool(*)(Obj)>(0xdc910)(source));
    fn<void(*)(Obj,bool)>(0x2a6e10)(duplicate,fn<bool(*)(Obj)>(0xdc920)(source));
    fn<void(*)(Obj,bool)>(0x2a7060)(duplicate,fn<bool(*)(Obj)>(0xdc940)(source));
    fn<void(*)(Obj,double)>(0x2a7170)(duplicate,fn<double(*)(Obj)>(0xdc4a0)(source));
}
void placeInsertionLeg(Obj arrow,const std::vector<Point>& points,bool curved) {
    if(points.size()<6)throw std::runtime_error("Empty insertion leg");
    fn<void(*)(Obj,double)>(0x2a6c00)(arrow,0);
    // Native setters record UndoMoveArrow, including the attachment snapshot.
    fn<void(*)(Obj,const Point*,const Point*)>(0x2a8260)(arrow,&points[points.size()-2],&points[1]);
    if(curved) {
        Obj curve=nativePath(arrow);if(!curve)throw std::runtime_error("Could not create insertion curve");
        fn<Unary>(0x349920)(curve);oldSetPoints(curve,&points);
        setTag(curve,cornersTag,std::string(points.size()/3,'1'));
        redraw(curve);
    }else {rememberStandardArrow(arrow);redraw(arrow);}
}
void commitInsertion() {
    insertionTrace.stage="native insertion commit";
    if(insertion.plan.candidate>=insertion.candidates.size()) {
        insertionTrace.rejection="split plan retired before native finalization";return;
    }
    if(!at<Obj>(insertion.doc,0xf0)) {
        insertionTrace.rejection="native move Undo record already closed";return;
    }
    Obj molecule=resolveInsertObject(insertion.molecule);
    auto& candidate=insertion.candidates[insertion.plan.candidate];
    Obj arrow=resolveInsertObject(candidate.arrow);if(!molecule||!arrow||!insertionArrow(arrow)) {
        insertionTrace.rejection="molecule or arrow changed before native finalization";return;
    }
    RectD actual{};vf<RectD*(*)(Obj,RectD*)>(molecule,0x1d8)(molecule,&actual);
    const auto& expected=insertion.plan.molecule;
    if(!valid(actual)||std::abs(actual.l-expected.l)>insertion.units*.5||
        std::abs(actual.t-expected.t)>insertion.units*.5) {
        insertionTrace.rejection="native final molecule bounds differ from preview";return;
    }
    const auto first=insertion.plan.first,second=insertion.plan.second;
    const std::string newHead=boundaryTag(actual,first[first.size()-2]),newTail=boundaryTag(actual,second[1]);
    const auto oldHeadTag=tagString(arrow,insertionHeadTag);
    const int outerTarget=at<int>(arrow,0x1f4),middle=at<int>(molecule,0x24);
    const auto originalPair=findPair(arrow);
    Obj originalParent=originalPair.group?at<Obj>(originalPair.group,0x28):at<Obj>(arrow,0x28);
    const double originalAngle=at<double>(arrow,0x130);
    Point originalTail{},originalHead{};
    fn<void(*)(Obj,Point*)>(0x2a4c90)(arrow,&originalTail);
    fn<void(*)(Obj,Point*)>(0xdc4d0)(arrow,&originalHead);
    const auto originalPoints=originalPair.curve?curvePoints(originalPair.curve):std::vector<Point>{};
    // Creation and both changes stay inside the native move's pending record.
    Obj duplicate=fn<Obj(*)(Obj,int)>(0x716a10)(insertion.page,-1);
    if(!duplicate)throw std::bad_alloc();
    bool changedOriginal=false;
    try {
    copyInsertionStyle(arrow,duplicate);
    at<int>(duplicate,0x1f0)=middle;at<int>(duplicate,0x1f4)=outerTarget;
    setTag(duplicate,insertionTailTag,newTail);
    if(!oldHeadTag.empty())setTag(duplicate,insertionHeadTag,oldHeadTag);
    rememberStandardArrow(duplicate);
    placeInsertionLeg(duplicate,second,candidate.curved);
    if(originalParent) {
        const auto pair=findPair(duplicate);
        fn<void(*)(Obj,Obj)>(0x32b0f0)(pair.group?pair.group:duplicate,originalParent);
    }
    vf<void(*)(Obj)>(arrow,0x1b8)(arrow);
    changedOriginal=true;
    // Freeze custom curves before changing the carrier's endpoints.
    if(candidate.curved) {
        Obj originalCurve=nativePath(arrow);if(!originalCurve)throw std::runtime_error("Could not preserve original path");
        fn<Unary>(0x349920)(originalCurve);
    }
    at<int>(arrow,0x1f4)=middle;
    setTag(arrow,insertionHeadTag,newHead);rememberStandardArrow(arrow);
    placeInsertionLeg(arrow,first,candidate.curved);
    insertionTrace.inserted=true;
    bumpGeneration();InvalidateRect(insertion.window,nullptr,FALSE);
    }catch(...) {
        // A failed native allocation must not leave a half-inserted arrow.
        UndoOff off(arrow);
        if(changedOriginal) {
            at<int>(arrow,0x1f4)=outerTarget;
            setTag(arrow,insertionHeadTag,oldHeadTag);
            if(!originalPair.curve)if(auto pair=findPair(arrow);pair.group) {
                fn<void(*)(Obj,Obj)>(0x32b0f0)(arrow,originalParent);
                fn<void(*)(Obj,bool)>(0x32be20)(arrow,true);
                const bool previous=deletingPair;deletingPair=true;
                oldDelete(pair.curve);oldDelete(pair.group);deletingPair=previous;
            }
            fn<void(*)(Obj,double)>(0x2a6c00)(arrow,originalAngle);
            fn<void(*)(Obj,const Point*,const Point*)>(0x2a8260)(arrow,&originalHead,&originalTail);
            if(originalPair.curve)oldSetPoints(originalPair.curve,&originalPoints);
            redraw(arrow);
        }
        pathDeleted(duplicate);
        throw;
    }
}
short insertionNativeFinish(Obj tracker) {
    if(insertion.tracker==tracker)insertionTrace.nativeFinish=true;
    if(onUI()&&insertion.tracker==tracker&&insertion.finishing&&!insertion.committing&&
        insertion.released&&insertion.plan.candidate<insertion.candidates.size()) {
        insertion.finishing=false;insertion.committing=true;
        try {commitInsertion();}
        catch(...) {insertionTrace.rejection="exception during native insertion commit";writeArrowStatus("Arrow insertion could not finish; the molecule move is retained.\r\n");}
        insertion.committing=false;
    }
    return oldInsertionFinish(tracker);
}
}

void beginArrowInsertionTrace(Obj tracker,bool gpu) noexcept {
    insertionTrace={};if(!patchEnabled(19)||!onUI()||!tracker)return;
    const auto ui=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ChemDrawUI.dll"));
    const auto callback=reinterpret_cast<uintptr_t>(vf<Unary>(tracker,0x38));
    if(callback!=base+0x16dcd0&&(!ui||callback!=ui+0x379174))return;
    insertionTrace.active=true;insertionTrace.gpu=gpu;
    insertionTrace.table=at<uintptr_t>(tracker,0);
    insertionTrace.callback=callback;
}
void arrowInsertionStage(const char* stage) noexcept {if(insertionTrace.active)insertionTrace.stage=stage;}
void endArrowInsertionTrace() noexcept {
    if(!insertionTrace.active)return;
    const auto& t=insertionTrace;
    char message[1200]{};
    sprintf_s(message,"Arrow insertion drag: stage=%s; rejection=%s\r\n"
        "tracker_table=%llx callback=%llx gpu=%u copy=%u armed=%u candidates=%zu\r\n"
        "moves=%u paths=%u valid_splits=%u preview_frames=%u invalidations=%u\r\n"
        "units_per_dip=%.9g nearest_distance=%.9g leg_lengths=%.9g,%.9g minimum=%.9g\r\n"
        "released=%u prepared=%u native_finish=%u inserted=%u\r\n",
        t.stage,t.rejection,static_cast<unsigned long long>(t.table),static_cast<unsigned long long>(t.callback),
        unsigned(t.gpu),t.copy,unsigned(t.armed),t.candidates,t.moves,t.paths,t.splits,t.previews,t.invalidations,
        t.units,t.distance,t.firstLength,t.secondLength,t.minimum,unsigned(t.released),unsigned(t.prepared),
        unsigned(t.nativeFinish),unsigned(t.inserted));
    writeArrowInsertionStatus(message);insertionTrace.active=false;
}
bool moleculeArrowAnchor(Obj object,Point& point) noexcept {
    if(!onUI()||!object||at<uintptr_t>(object,0)!=base+0x89a468||!at<uint8_t>(object,0x34))return false;
    RectD r{};vf<RectD*(*)(Obj,RectD*)>(object,0x1d8)(object,&r);
    if(!valid(r))return false;
    point={(r.l+r.r)*.5,(r.t+r.b)*.5,0};return finite(point);
}
bool arrowInsertionAttachment(Obj arrow,bool tail,Point& point) noexcept {
    if(!onUI()||!isArrow(arrow))return false;
    try {
        const int id=at<int>(arrow,tail?0x1f0:0x1f4);
        Obj object=id>=0?fn<Obj(*)(Obj,int)>(0x712f80)(at<Obj>(arrow,0x60),id):nullptr;
        if(!object||at<uintptr_t>(object,0)!=base+0x89a468||!at<uint8_t>(object,0x34))return false;
        const auto value=tagString(arrow,tail?insertionTailTag:insertionHeadTag);if(value.empty())return false;
        std::istringstream in(value);in.imbue(std::locale::classic());unsigned version{};double x{},y{};
        if(!(in>>version>>x>>y)||version!=1||!std::isfinite(x+y)||std::abs(x)>4||std::abs(y)>4)return false;
        RectD r{};vf<RectD*(*)(Obj,RectD*)>(object,0x1d8)(object,&r);if(!valid(r))return false;
        point={r.l+(r.r-r.l)*x,r.t+(r.b-r.t)*y,0};return finite(point);
    }catch(...){return false;}
}
void installArrowInsertion() {hook(0x16a290,insertionNativeFinish,oldInsertionFinish);}
void beginArrowInsertion(Obj tracker,Obj doc,Obj molecule,const RectD& bounds,
    const std::vector<ArrowInsertionTarget>& targets) noexcept {
    insertion={};
    if (!patchEnabled(16)) return;
    arrowInsertionStage("arming molecule insertion");
    insertionTrace.copy=tracker?at<uint8_t>(tracker,0x238):0;
    if(!onUI()||!installed||!tracker||!doc||!molecule||!valid(bounds)) {
        insertionTrace.rejection="invalid molecule drag context";return;
    }
    if(at<uintptr_t>(molecule,0)!=base+0x89a468) {
        insertionTrace.rejection="moving root is a group rather than one fragment";return;
    }
    if(at<uint8_t>(tracker,0x238)){insertionTrace.rejection="copy drag";return;}
    try {
        insertion.tracker=tracker;insertion.doc=doc;insertion.page=at<Obj>(molecule,0x60);
        insertion.molecule=identity(molecule);insertion.initial=bounds;insertion.down=at<Point>(tracker,0x38);
        insertion.window=drawingWindow(insertion.page);
        std::unordered_set<Obj> seen;
        for(const auto& target:targets) {
            const auto pair=findPair(target.object);
            Obj arrow=pair.arrow?pair.arrow:target.object,visible=pair.curve?pair.curve:arrow;
            if(!insertionArrow(arrow)||at<Obj>(arrow,0x60)!=insertion.page||
                at<int>(arrow,0x1f0)==insertion.molecule.id||at<int>(arrow,0x1f4)==insertion.molecule.id)continue;
            if(!seen.insert(arrow).second)continue;
            RectD arrowBounds{};vf<RectD*(*)(Obj,RectD*)>(visible,0x1d8)(visible,&arrowBounds);
            if(!valid(arrowBounds))continue;
            insertion.candidates.push_back({identity(arrow),identity(visible),arrowBounds});
        }
        insertionTrace.candidates=insertion.candidates.size();insertionTrace.armed=!insertion.candidates.empty();
        if(insertion.candidates.empty()){insertionTrace.rejection="no supported stationary arrow candidates";insertion={};}
    }catch(...){insertionTrace.rejection="exception during insertion candidate preparation";insertion={};}
}
bool constrainArrowInsertion(Obj tracker,Point& point,double units) noexcept {
    if(!onUI()||insertion.tracker!=tracker)return false;
    ++insertionTrace.moves;arrowInsertionStage("native constrained drag point");
    if((GetAsyncKeyState(VK_MENU)|GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_CONTROL))&0x8000||
        !std::isfinite(units)||units<=0||!finite(point)||!resolveInsertObject(insertion.molecule)) {
        insertionTrace.rejection="modifier, invalid scale or missing molecule";insertion.plan={};return false;
    }
    try {
        insertion.units=units;
        insertionTrace.units=units;
        const RectD moved=translated(insertion.initial,sub(point,insertion.down));
        const Point center{(moved.l+moved.r)*.5,(moved.t+moved.b)*.5,0};
        double best=std::numeric_limits<double>::infinity();InsertPlan selected;
        for(size_t i=0;i<insertion.candidates.size();++i) {
            auto& candidate=insertion.candidates[i];
            const double radius=(insertion.plan.candidate==i?16:10)*units;
            const auto& r=candidate.bounds;
            if(moved.r<r.l-radius||moved.l>r.r+radius||moved.b<r.t-radius||moved.t>r.b+radius)continue;
            Obj arrow=resolveInsertObject(candidate.arrow);if(!arrow)continue;
            if(!candidate.loaded) {
                auto pair=findPair(arrow);candidate.path=pair.curve?curvePoints(pair.curve):initialPath(arrow);
                candidate.curved=bool(pair.curve)||std::abs(at<double>(arrow,0x130))>1e-6;
                candidate.loaded=true;
            }
            if(candidate.path.size()<6)continue;
            ++insertionTrace.paths;
            double distance{};const double nearest=closestInsertionPoint(candidate.path,center,distance);
            insertionTrace.distance=distance;
            const Point projected=pathPoint(candidate.path,nearest);
            const double dx=std::max(0.0,std::abs(projected.x-center.x)-(moved.r-moved.l)*.5);
            const double dy=std::max(0.0,std::abs(projected.y-center.y)-(moved.b-moved.t)*.5);
            if(std::hypot(dx,dy)>radius)continue;
            const double score=distance-(insertion.plan.candidate==i?3*units:0);
            if(score>=best)continue;
            InsertPlan plan;plan.candidate=i;
            if(!splitInsertionPath(candidate,moved,units,plan))continue;
            ++insertionTrace.splits;
            // Keep the corrected molecule inside native drag constraints.
            const RectD limits=at<RectD>(tracker,0x198),slack=at<RectD>(tracker,0x1f8);
            if(valid(limits)&&valid(slack)) {
                const Point final=add(point,plan.offset);
                if(final.x<limits.l-(slack.r-slack.l)*.8||final.x>limits.r+(slack.r-slack.l)*.8||
                    final.y<limits.t-(slack.b-slack.t)*.8||final.y>limits.b+(slack.b-slack.t)*.8)continue;
            }
            best=score;selected=std::move(plan);
        }
        insertion.plan=std::move(selected);
        if(insertion.plan.candidate==SIZE_MAX)return false;
        insertionTrace.rejection="none";point=add(point,insertion.plan.offset);return true;
    }catch(...){insertionTrace.rejection="exception while projecting native drag point";insertion.plan={};return false;}
}
void prepareArrowInsertionFinish(Obj tracker,bool samePage,bool commit) noexcept {
    if(insertion.tracker!=tracker)return;
    insertion.finishing=samePage&&commit&&insertion.released&&
        !((GetAsyncKeyState(VK_MENU)|GetAsyncKeyState(VK_SHIFT)|GetAsyncKeyState(VK_CONTROL))&0x8000);
    insertionTrace.prepared=insertion.finishing;
    if(!insertion.finishing)insertion.plan={};
}
void finishArrowInsertion(Obj tracker) noexcept {if(insertion.tracker==tracker)insertion={};}
void cancelArrowInsertion() noexcept {insertion={};}
void arrowInsertionReleased(HWND window) noexcept {if(insertion.window==window){insertion.released=true;insertionTrace.released=true;}}
void arrowInsertionObjectChanged(Obj object) noexcept {
    if(!insertion.tracker||insertion.committing||!object)return;
    for(auto& c:insertion.candidates)if(c.arrow.instance==at<uint64_t>(object,0xb0)||c.visible.instance==at<uint64_t>(object,0xb0)) {
        ++insertionTrace.invalidations;
        c.path.clear();c.loaded=false;insertion.plan={};insertion.filtered.clear();
    }
}
std::shared_ptr<const GpuScene> arrowInsertionPreview(const std::shared_ptr<const GpuScene>& root) noexcept {
    if(!root||!onUI()||!insertion.tracker||insertion.plan.candidate>=insertion.candidates.size())return root;
    bool capturing=false;
    try {
        const auto& candidate=insertion.candidates[insertion.plan.candidate];
        Obj arrow=resolveInsertObject(candidate.arrow),port=at<Obj>(insertion.doc,0x258);
        if(!arrow||!port||!beginGpuHover(insertion.window,port)) {insertionTrace.rejection="preview graphics capture unavailable";insertion.plan={};return root;}
        capturing=true;
        {
            auto* graphics=fn<Gdiplus::Graphics*(*)(Obj)>(0x623140)(port);
            if(!graphics){endGpuHover();capturing=false;insertion.plan={};return root;}
            struct ClipScope {
                Gdiplus::Graphics* graphics;Gdiplus::GraphicsState saved;
                explicit ClipScope(Gdiplus::Graphics* g):graphics(g),saved(g->Save()){g->ResetClip();}
                ~ClipScope(){graphics->Restore(saved);}
            } clip(graphics);
            ShapeScope shape;
            NativeSpline first(arrow,insertion.plan.first);oldCurveDraw(first.data,port);
            NativeSpline second(arrow,insertion.plan.second);oldCurveDraw(second.data,port);
        }
        RectI bounds{};auto artwork=endGpuHover(&bounds);capturing=false;
        if(!artwork||artwork->commands.empty()){insertionTrace.rejection="native preview emitted no vector artwork";insertion.plan={};return root;}
        ++insertionTrace.previews;
        if(insertion.filteredOwner!=candidate.arrow.instance||insertion.filteredCurve!=candidate.visible.instance||
            insertion.filtered.size()>256) {
            insertion.filtered.clear();insertion.filteredOwner=candidate.arrow.instance;insertion.filteredCurve=candidate.visible.instance;
        }
        // Copy only layers containing the old arrow; shared stationary page
        // artwork is retained across frames and never materialized as pixels.
        struct Visit {std::shared_ptr<const GpuScene> scene,previous;bool finish{};};
        std::vector<Visit> visits{{root}};
        while(!visits.empty()) {
            auto v=std::move(visits.back());visits.pop_back();
            if(insertion.filtered.contains(v.scene.get()))continue;
            if(!v.finish) {
                v.previous=v.scene->base.load(std::memory_order_acquire);v.finish=true;visits.push_back(v);
                if(v.previous)visits.push_back({v.previous});
                for(const auto& c:v.scene->commands)if(c.image)visits.push_back({c.image});
                continue;
            }
            const auto previous=v.previous?insertion.filtered.at(v.previous.get()).second:std::shared_ptr<const GpuScene>{};
            auto removed=[&](const SceneCommand& c) {
                return c.objectOwner&&(c.objectOwner==candidate.arrow.instance||c.objectOwner==candidate.visible.instance);
            };
            bool changed=previous!=v.previous;
            for(const auto& c:v.scene->commands)
                if(removed(c)||(c.image&&insertion.filtered.at(c.image.get()).second!=c.image))changed=true;
            std::shared_ptr<const GpuScene> result=v.scene;
            if(changed) {
                auto layer=makeGpuScene();layer->width=v.scene->width;layer->height=v.scene->height;
                layer->extent=v.scene->extent;layer->trackingFrame=v.scene->trackingFrame;
                layer->serial=nextGpuSceneSerial();layer->base=previous;layer->commands.reserve(v.scene->commands.size());
                for(auto c:v.scene->commands) {
                    if(removed(c))continue;
                    if(c.image)c.image=insertion.filtered.at(c.image.get()).second;
                    layer->commands.push_back(std::move(c));
                }
                result=std::move(layer);
            }
            insertion.filtered.emplace(v.scene.get(),InsertionGesture::Filtered{v.scene,std::move(result)});
        }
        auto frame=makeGpuScene();frame->width=root->width;frame->height=root->height;
        frame->extent=root->extent;frame->trackingFrame=true;frame->serial=nextGpuSceneSerial();
        frame->base=insertion.filtered.at(root.get()).second;
        SceneCommand image;image.kind=SceneCommand::Image;image.image=std::move(artwork);
        auto state=std::make_shared<SceneState>();state->clip={{0,0,float(root->width),float(root->height)}};
        image.state=std::move(state);image.source={0,0,float(bounds.r-bounds.l),float(bounds.b-bounds.t)};
        image.destination={float(bounds.l),float(bounds.t),image.source.w,image.source.h};
        frame->commands.push_back(std::move(image));return frame;
    }catch(...) {
        if(capturing)endGpuHover();insertionTrace.rejection="exception while constructing split preview";insertion.plan={};return root;
    }
}
}

#include "gpu-preview.hpp"
#include "curved-arrow.hpp"
#include <numbers>

namespace cd {
namespace {
bool finite(Point p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
Point position(Obj object) {
    Point p{};return *vf<Point*(*)(Obj,Point*)>(object,0x1f0)(object,&p);
}
bool bondEndpoints(Obj bond,Point& a,Point& b) {
    const Obj left=at<Obj>(bond,0xc0),right=at<Obj>(bond,0xc8);
    if(!left||!right) return false;
    // FuseRing uses atom character-attachment points, not atom hit circles.
    // The native const-bond overload also handles plain unlabeled vertices.
    fn<Point*(*)(Obj,Point*,Obj)>(0x2b50e0)(left,&a,bond);
    fn<Point*(*)(Obj,Point*,Obj)>(0x2b50e0)(right,&b,bond);
    return finite(a)&&finite(b)&&std::hypot(b.x-a.x,b.y-a.y)>0;
}
double setting(Obj page,int property) {
    const Obj value=fn<Obj(*)(Obj,int)>(0x7128f0)(page,property);
    return value?fn<double(*)(Obj)>(0x37be40)(value):0;
}
bool sproutRingPreference() {
    // Release-build MSVC string layout, borrowed only by the native const-ref
    // reader. These plain structs have no destructor or allocator ownership.
    struct NativeString { const char* data{};uint64_t padding{},length{},capacity{15}; };
    constexpr char name[]="sproutRingsInsteadOfSpiro";
    const NativeString key{name,0,sizeof(name)-1,sizeof(name)-1},section{};
    const Obj manager=fn<Obj(*)()>(0x0d61a0)();
    return manager&&fn<bool(*)(Obj,const NativeString*,const NativeString*)>(0x245a90)(manager,&key,&section);
}
bool bonds(Obj atom,Obj*& first,Obj*& last) {
    first=at<Obj*>(atom,0x120);last=at<Obj*>(atom,0x128);
    const auto a=reinterpret_cast<uintptr_t>(first),b=reinterpret_cast<uintptr_t>(last);
    return b>=a&&(b-a)%sizeof(Obj)==0&&(b-a)/sizeof(Obj)<=64&&(first||a==b);
}
Obj neighbor(Obj bond,Obj atom) {
    if(!bond) return nullptr;
    const Obj a=at<Obj>(bond,0xc0),b=at<Obj>(bond,0xc8);
    return a==atom?b:(b==atom?a:nullptr);
}
double spacing(const GhostTool& tool,double length) {
    const double minimum=*reinterpret_cast<const double*>(base+0x8973b8)*tool.width*0.5;
    return std::max(tool.spacing*length,minimum);
}
void stroke(GhostArtwork& art,Point a,Point b,double width) {
    if(finite(a)&&finite(b)&&std::isfinite(width)&&width>0&&std::hypot(b.x-a.x,b.y-a.y)>0)
        art.strokes.push_back({a,b,width});
}
struct ArrowStyle {
    double angle{},headSize{};
    int head{1},tail{1},headType{1},noGo{};
    bool bold{},dashed{},dipole{},wavy{},parallel{};
};
bool bits(int value,int first,int count,uint64_t mask) {
    return value>=first&&value-first<count&&((mask>>unsigned(value-first))&1)!=0;
}
ArrowStyle arrowStyle(const GhostTool& tool) {
    // Geometry-only transcription of CDArrow::ApplySubtype. Do not instantiate
    // temporary native arrows: their setters notify the page and allocate undo.
    const int s=tool.tool==0x27?(tool.subtype==1?0x1c:7):tool.subtype;
    ArrowStyle a;
    a.bold=bits(s,3,3,7)||s==0x3b||bits(s,0x71,12,0xf0f);
    a.dashed=bits(s,0x12,3,7)||s==0x3c||bits(s,0x69,20,0xf000f);
    a.dipole=s==0x80;a.wavy=s==0x90;
    if(bits(s,0,0x24,0x808000040)||bits(s,0x69,0x26,0x2040010101)) a.angle=-270;
    else if(s==12||s==31||s==0x8f) a.angle=270;
    else if(bits(s,0,0x25,0x1010000080)||bits(s,0x6a,0x1f,0x40010101)) a.angle=-180;
    else if(s==13||s==32) a.angle=180;
    else if(bits(s,0,0x26,0x2020000100)||bits(s,0x6b,0x1f,0x40010101)) a.angle=-120;
    else if(s==14||s==33) a.angle=120;
    else if(bits(s,0,0x27,0x4040000200)||bits(s,0x6c,0x1f,0x40010101)) a.angle=-90;
    else if(s==15||s==34) a.angle=90;
    if(bits(s,0x87,9,399)) a.headSize=8;
    else if(bits(s,0,0x1b,0x4900024)||bits(s,0x46,0x3e,0x2000000000000009)||s==0x86||s==0x8d) a.headSize=10;
    else if(bits(s,0,0x1a,0x2480012)||bits(s,0x45,0x3e,0x2000000000000009)||s==0x85||s==0x8c||a.wavy) a.headSize=15;
    else if(bits(s,0,0x19,0x1240009)||bits(s,0x44,0x3e,0x2000000000000009)||s==0x84||s==0x8b) a.headSize=22.5;
    else if(s==10||s==11) a.headSize=6;
    else if(s==16||s==17) a.headSize=12;
    else if(a.angle) a.headSize=8;
    if(s>=0x18&&s<=0x1a) a.headSize=-a.headSize;
    if(a.bold) a.headSize*=2;
    if((s>=0x15&&s<=0x17)||(s>=0x1b&&s<=0x1e)) a.head=3;
    else if((s>=0x18&&s<=0x1a)||(s>=0x1f&&s<=0x22)) a.head=4;
    else if((s>=0x44&&s<=0x46)||(s>=0x87&&s<=0x8a)) {a.head=2;a.tail=2;}
    else if((s>=0x47&&s<=0x49)||(s>=0x8b&&s<=0x8d)) {a.head=3;a.tail=3;a.parallel=true;}
    else if(s==10||s==11||s==16||s==17) {a.head=2;a.headType=(s==10||s==16)?3:2;a.parallel=true;}
    else if(a.headSize!=0) a.head=2;
    if(s>=0x81&&s<=0x83) a.noGo=2;
    else if(s>=0x84&&s<=0x86) a.noGo=3;
    if(tool.tool==0x27) a.headSize=10;
    return a;
}
void polygon(GhostArtwork& art,std::vector<Point> points,double width,bool filled) {
    if(points.size()<2) return;
    GhostPath path;path.points=std::move(points);path.width=width;path.filled=filled;
    path.types.assign(path.points.size(),Gdiplus::PathPointTypeLine);
    path.types.front()=Gdiplus::PathPointTypeStart;
    path.types.back()|=Gdiplus::PathPointTypeCloseSubpath;
    art.paths.push_back(std::move(path));
}
void arrowHead(GhostArtwork& art,Point tip,Point direction,int side,const ArrowStyle& style,double width,double length) {
    if(side==1||style.headSize==0) return;
    const double norm=std::hypot(direction.x,direction.y);if(norm<=0) return;
    const Point u{direction.x/norm,direction.y/norm,0},n{-u.y,u.x,0};
    const double size=std::clamp(std::abs(style.headSize)*width*.875,width*3,length*.35);
    const double half=std::max(width,size*.25);
    const Point back{tip.x-u.x*size,tip.y-u.y*size,tip.z};
    Point left{back.x+n.x*half,back.y+n.y*half,tip.z},right{back.x-n.x*half,back.y-n.y*half,tip.z};
    if(side==3) right=back;
    if(side==4) left=back;
    if(style.headType==3) {
        stroke(art,left,tip,width);stroke(art,tip,right,width);
    } else polygon(art,{left,tip,right},width,style.headType==1);
}
void arrowArtwork(Obj page,Obj atom,Obj bond,Point mouse,const GhostTool& tool,GhostBond& ghost,GhostArtwork& art) {
    const auto style=arrowStyle(tool);
    Point start=mouse;
    if(atom) start=position(atom);
    else if(bond) {Point a{},b{};if(bondEndpoints(bond,a,b)) start={(a.x+b.x)*.5,(a.y+b.y)*.5,(a.z+b.z)*.5};}
    const double length=tool.length*2;
    double width=tool.width;
    if(style.bold) {const double bold=setting(page,0x806);if(std::isfinite(bold)&&bold>0) width=bold;}
    // Native positive page-space arc angles bend below a left-to-right chord.
    // This circle parameterization uses the opposite sweep convention; reflect
    // the sweep before deriving the shaft, arrowhead tangent and bond clearance.
    const double theta=-style.angle*std::numbers::pi/180;
    const Point center{.5,theta? .5/std::tan(theta/2):0,0};
    auto point=[&](double t,double offset=0.0) {
        Point p{t,0,0},tangent{1,0,0};
        if(theta) {
            const Point v{-.5,-center.y,0};const double a=theta*t;
            Point r{v.x*std::cos(a)-v.y*std::sin(a),v.x*std::sin(a)+v.y*std::cos(a),0};
            p={center.x+r.x,center.y+r.y,0};
            const double norm=std::hypot(r.x,r.y);tangent={-r.y/norm*(theta<0?-1:1),r.x/norm*(theta<0?-1:1),0};
        }
        return Point{start.x+p.x*length-tangent.y*offset,start.y+p.y*length+tangent.x*offset,start.z};
    };
    if(atom||bond) {
        const auto next=point(.0001),first=point(0);const double dx=next.x-first.x,dy=next.y-first.y,norm=std::hypot(dx,dy);
        if(norm>0) start=arrowAttachmentPoint(atom?atom:bond,start,{dx/norm,dy/norm,0},tool.length*.1);
    }
    ghost.start=point(0);ghost.end=point(1);
    auto shaft=[&](double offset,bool reverse) {
        const int samples=theta?96:style.wavy?128:1;
        std::vector<Point> points;points.reserve(size_t(samples)+1);
        for(int i=0;i<=samples;++i) {
            const double t=double(i)/samples;auto p=point(t,offset);
            if(style.wavy) p.y+=std::sin(t*std::numbers::pi*16)*width*2;
            points.push_back(p);
        }
        if(style.dashed) {
            const double dash=std::max(tool.dashSpacing,width*2);double travel{};
            for(size_t i=1;i<points.size();++i) {
                const Point a=points[i-1],b=points[i];const double segment=std::hypot(b.x-a.x,b.y-a.y);
                if(segment<=0) continue;
                double distance{};
                while(distance<segment) {
                    const double cell=std::fmod(travel+distance,dash*2);
                    const double next=std::min(segment,distance+std::max(width*.001,(cell<dash?dash:dash*2)-cell));
                    if(cell<dash) stroke(art,{a.x+(b.x-a.x)*distance/segment,a.y+(b.y-a.y)*distance/segment,a.z},
                        {a.x+(b.x-a.x)*next/segment,a.y+(b.y-a.y)*next/segment,a.z},width);
                    distance=next;
                }
                travel+=segment;
            }
        } else {
            GhostPath path;path.points=points;path.width=width;
            path.types.assign(points.size(),Gdiplus::PathPointTypeLine);path.types.front()=Gdiplus::PathPointTypeStart;
            art.paths.push_back(std::move(path));
        }
        auto tip=reverse?points.front():points.back();auto next=reverse?points[1]:points[points.size()-2];
        arrowHead(art,tip,{tip.x-next.x,tip.y-next.y,0},style.head,style,width,length);
        if(!reverse&&!style.parallel&&style.tail!=1) {
            tip=points.front();next=points[1];
            arrowHead(art,tip,{tip.x-next.x,tip.y-next.y,0},style.tail,style,width,length);
        }
    };
    if(style.parallel) {
        const double offset=std::max(tool.width*2,length*.035);
        shaft(offset,false);shaft(-offset,style.headType!=3);
    } else shaft(0,false);
    if(style.dipole) {
        const auto p=point(.04);stroke(art,{p.x,p.y-width*3,p.z},{p.x,p.y+width*3,p.z},width);
    }
    if(style.noGo) {
        const auto p=point(.5);const double d=width*5;
        if(style.noGo==2) {
            stroke(art,{p.x-d,p.y-d,p.z},{p.x+d,p.y+d,p.z},width);
            stroke(art,{p.x-d,p.y+d,p.z},{p.x+d,p.y-d,p.z},width);
        } else for(double offset:{-width*2,width*2})
            stroke(art,{p.x+offset-d*.4,p.y+d,p.z},{p.x+offset+d*.4,p.y-d,p.z},width);
    }
}
void bondArtwork(GhostArtwork& art,Point a,Point b,const GhostTool& tool,Obj source) {
    const double length=std::hypot(b.x-a.x,b.y-a.y),gap=spacing(tool,length);
    if(length<=0||!std::isfinite(gap)||gap<=0) return;
    const Point normal{-(b.y-a.y)/length,(b.x-a.x)/length,0};
    auto parallel=[&](double offset,double endOffset,bool shorten=false) {
        const double trim=shorten?std::min(gap*0.5,length*0.15):0;
        const double ux=(b.x-a.x)/length,uy=(b.y-a.y)/length;
        stroke(art,{a.x+normal.x*offset+ux*trim,a.y+normal.y*offset+uy*trim,a.z},
            {b.x+normal.x*endOffset-ux*trim,b.y+normal.y*endOffset-uy*trim,b.z},tool.width);
    };
    if(tool.order==1) {
        if(tool.type==3) {
            // GenerateDashedLine fits an odd number of equal dash/gap cells
            // to the complete bond, retaining a dash at both endpoints.
            if(!std::isfinite(tool.dashSpacing)||tool.dashSpacing<=0) return;
            const double requested=std::floor((length+tool.dashSpacing)/(2*tool.dashSpacing)+0.5);
            const int count=int(std::clamp(requested,1.0,2048.0));
            const double cells=double(count)*2-1;
            for(int i=0;i<count;++i) {
                const double from=double(i)*2/cells,to=(double(i)*2+1)/cells;
                stroke(art,{a.x+(b.x-a.x)*from,a.y+(b.y-a.y)*from,a.z+(b.z-a.z)*from},
                    {a.x+(b.x-a.x)*to,a.y+(b.y-a.y)*to,a.z+(b.z-a.z)*to},tool.width);
            }
        } else parallel(0,0);
        return;
    }
    if(tool.order==3) { parallel(-gap,-gap);parallel(0,0);parallel(gap,gap);return; }
    int flags=tool.type&3;
    if(!(tool.type&8)) {
        flags=2;
        Obj* first{};Obj* last{};
        if(source&&bonds(source,first,last)) for(auto it=first;it!=last;++it) {
            const Obj other=neighbor(*it,source);if(!other) continue;
            const auto p=position(other);
            const double side=(b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);
            if(std::abs(side)>length*length*1.0e-8) { flags=side>0?0:1;break; }
        }
    }
    if(tool.type&0x20) { parallel(-gap*0.5,gap*0.5);parallel(gap*0.5,-gap*0.5); }
    else if(flags==2||flags==3) { parallel(-gap*0.5,-gap*0.5);parallel(gap*0.5,gap*0.5); }
    else { parallel(0,0);parallel(flags==0?gap:-gap,flags==0?gap:-gap,true); }
}
bool firstBond(Obj atom,Point mouse,const GhostTool& tool,Point& start,Point& end) {
    if(atom) {
        start=position(atom);char attachment=-1;
        previewBondPlacement(&end,&mouse,&attachment,atom);
        if(attachment>=0) return false;
    } else {
        start=mouse;
        const double angle=(90.0-tool.chainAngle*0.5)*std::numbers::pi/180.0;
        end={start.x+tool.length*std::cos(angle),start.y-tool.length*std::sin(angle),start.z};
    }
    return finite(start)&&finite(end)&&std::hypot(end.x-start.x,end.y-start.y)>0;
}
struct RingPoints {
    std::array<Point,64> points{};size_t count{};
    bool calculate(Point a,Point b,const GhostTool& tool) {
        points={};points[0]=a;points[1]=b;
        // Native geometry-only routines resize this borrowed vector inside its
        // preallocated capacity. They never own or destroy the supplied storage.
        auto first=reinterpret_cast<std::byte*>(points.data());
        Vec vector{first,first+2*sizeof(Point),first+sizeof(points)};
        if(tool.ringCode==9||tool.ringCode==10)
            fn<void(*)(Vec*,int,bool)>(0x4fdd60)(&vector,tool.ringCode==10?1:0,false);
        else fn<void(*)(int,Vec*)>(0x4fe640)(tool.ringCode,&vector);
        count=size_t(vector.last-vector.first)/sizeof(Point);
        return count>=3&&count<=points.size()&&
            std::all_of(points.begin(),points.begin()+count,finite);
    }
};
bool attachedRing(RingPoints& ring,Point a,Point b,const GhostTool& tool) {
    if(!ring.calculate(a,b,tool)) return false;
    const Point midpoint{(ring.points[1].x+ring.points[ring.count-1].x)*0.5,
        (ring.points[1].y+ring.points[ring.count-1].y)*0.5,a.z};
    const double dx=b.x-a.x,dy=b.y-a.y,d=std::hypot(dx,dy);
    if(d<=0) return false;
    const double projection=((midpoint.x-a.x)*dx+(midpoint.y-a.y)*dy)/(d*d);
    b={2*(a.x+projection*dx)-midpoint.x,2*(a.y+projection*dy)-midpoint.y,a.z};
    const double norm=std::hypot(b.x-a.x,b.y-a.y);if(norm<=0) return false;
    b={a.x+(b.x-a.x)*tool.length/norm,a.y+(b.y-a.y)*tool.length/norm,a.z};
    return ring.calculate(a,b,tool);
}
bool alreadyBonded(Obj a,Obj b) {
    Obj* first{};Obj* last{};
    if(!a||!b||!bonds(a,first,last)) return false;
    for(auto it=first;it!=last;++it) if(neighbor(*it,a)==b) return true;
    return false;
}
bool hasNewRingVertex(Obj page,const RingPoints& ring) {
    for(size_t i=0;i<ring.count;++i)
        if(!fn<Obj(*)(Obj,const Point*,Obj)>(0x7107c0)(page,&ring.points[i],nullptr)) return true;
    return false;
}
bool changedBondTool(Obj bond,const GhostTool& selected,GhostTool& result) {
    if(at<uint8_t>(bond,0xf8)) return false; // native ChangeBondType is a no-op
    result=selected;
    const int order=at<int>(bond,0xdc),type=at<uint8_t>(bond,0xd8);
    if(selected.order>=2) {
        if(selected.order==2&&order==2&&((type^selected.type)&0xf4)==0) {
            uint8_t next=uint8_t(type);
            // Native geometry helper writes only this output byte and lazily
            // reads double-bond side flags; it does not change bond properties.
            fn<void(*)(Obj,uint8_t*)>(0x4fb4f0)(bond,&next);result.type=next;
        }
    } else if(selected.subtype!=0xe&&selected.subtype!=0xf&&selected.subtype!=0x10&&selected.subtype!=0x11) {
        if(order==1) {
            const int reversed=fn<uint8_t(*)(uint8_t,int)>(0x449740)(uint8_t(selected.subtype),1);
            result.type=type==selected.subtype?reversed:
                type==reversed?selected.subtype:selected.type;
            if(result.type==type) { result.order=2;result.type=0; }
        } else if(order==2) {
            // Plain single-bond clicks cycle the existing double-bond side;
            // dashed clicks here are a native no-op, not a new sprouting bond.
            if(selected.subtype!=1) return false;
            result.order=2;
            if(type&0x54) result.type=type&0xab;
            else {
                uint8_t next=uint8_t(type);
                fn<void(*)(Obj,uint8_t*)>(0x4fb4f0)(bond,&next);result.type=next;
            }
        } else if(order!=3&&order!=4) return false;
    }
    return result.order!=order||result.type!=type;
}
struct RingBondPlan {
    std::array<Obj,64> atoms{},edges{};
    std::array<int,64> order{},degree{};
    std::array<bool,64> created{},processed{};
    bool calculate(Obj page,const RingPoints& ring,bool aromatic,Obj sproutParent) {
        const size_t count=ring.count;size_t newCount{};
        for(size_t i=0;i<count;++i) {
            atoms[i]=fn<Obj(*)(Obj,const Point*,Obj)>(0x7107c0)(page,&ring.points[i],nullptr);
            created[i]=!atoms[i];newCount+=created[i];order[i]=1;
            Obj* first{};Obj* last{};
            if(atoms[i]) {
                if(!bonds(atoms[i],first,last))return false;
                degree[i]=first==last?0:int(last-first);
            }
        }
        // DropRing creates the sprout before calling PlaceRing. Account for
        // that existing endpoint/connector without creating them on hover.
        if(sproutParent&&!alreadyBonded(sproutParent,atoms[0])) {
            ++degree[0];
            if(created[0]) {created[0]=false;--newCount;}
        }
        for(size_t i=0;i<count;++i) {
            Obj* first{};Obj* last{};
            if(atoms[i]&&atoms[(i+1)%count]&&bonds(atoms[i],first,last))
                for(auto it=first;it!=last;++it)if(neighbor(*it,atoms[i])==atoms[(i+1)%count]) {
                    edges[i]=*it;break;
                }
        }
        if(!aromatic)return true;
        // Mirror PlaceRing's read-only bond-order decision, including its
        // creation boundary, unsaturated attachments and sequential degrees.
        // No temporary native atoms, bonds, undo records or chemistry jobs.
        bool phase=(GetAsyncKeyState(VK_SHIFT)&0x8000)!=0;
        auto multiple=[&](size_t i) {
            Obj* first{};Obj* last{};
            if(!atoms[i]||!bonds(atoms[i],first,last))return false;
            for(auto it=first;it!=last;++it)if(at<unsigned>(*it,0xdc)>1)return true;
            return false;
        };
        size_t start{};bool anchored{};
        if(newCount+2<=count) {
            size_t boundary{};
            for(size_t i=0;i<count;++i)if(created[i]!=created[(i+1)%count]) {boundary=i;break;}
            // An absent seed edge permits an anchor with a single first edge.
            // An existing seed edge instead preserves parity at the anchor.
            for(bool singleFirst:{true,false}) {
                if(singleFirst&&edges[0])continue;
                for(size_t step=0;step<count&&!anchored;++step) {
                    const size_t i=(boundary+step)%count,next=(i+1)%count;
                    if(!created[i]&&!created[next]&&multiple(i)&&multiple(next)) {
                        start=i;anchored=true;
                        if(singleFirst)phase=false;else if(i&1)phase=!phase;
                    }
                }
                if(anchored)break;
            }
        }
        auto outsideDouble=[&](size_t i) {
            Obj* first{};Obj* last{};
            if(!atoms[i]||!bonds(atoms[i],first,last))return false;
            const Obj previous=atoms[(i+count-1)%count],next=atoms[(i+1)%count];
            for(auto it=first;it!=last;++it) {
                const Obj other=neighbor(*it,atoms[i]);
                if(other==previous||other==next)continue;
                int nativeOrder=at<int>(*it,0xdc);
                for(size_t edge=0;edge<count;++edge)
                    if(processed[edge]&&edges[edge]==*it)nativeOrder=order[edge];
                if(nativeOrder==2)return true;
            }
            return false;
        };
        for(size_t step=0;step<count;++step) {
            const size_t i=(start+step)%count,next=(i+1)%count;
            const bool left=outsideDouble(i),right=outsideDouble(next);
            if(left&&right) {order[i]=1;if(phase&&step==0)phase=false;}
            else if((left&&(degree[next]==0||degree[i]==2))||
                    (right&&(degree[i]==0||degree[next]==2)))order[i]=1;
            else {
                const bool doubled=phase&&step==count-1||
                    (size_t(phase)+1+step!=count&&(step&1)==size_t(phase));
                order[i]=doubled?2:1;
            }
            processed[i]=true;
            if(!edges[i]) {++degree[i];++degree[next];}
        }
        return true;
    }
};
Point ringInsetEndpoint(Point vertex,Point toward,Point adjacent,double gap) {
    const double length=std::hypot(toward.x-vertex.x,toward.y-vertex.y);
    const double parent=std::hypot(adjacent.x-vertex.x,adjacent.y-vertex.y);
    if(length<=0||parent<=0)return vertex;
    const Point u{(toward.x-vertex.x)/length,(toward.y-vertex.y)/length,0};
    const Point v{(adjacent.x-vertex.x)/parent,(adjacent.y-vertex.y)/parent,0};
    const double cross=std::abs(u.x*v.y-u.y*v.x);
    if(cross<=1e-8)return vertex;
    // Native double-bond endpoints follow the neighboring ray plus this ray,
    // scaled by perpendicular distance. For a regular ring the axial trim is
    // gap*tan(pi/count), rather than the old gap/tan(pi/count).
    return {vertex.x+(u.x+v.x)*gap/cross,vertex.y+(u.y+v.y)*gap/cross,vertex.z};
}
bool ringArtwork(Obj page,Obj atom,Obj bond,Point mouse,const GhostTool& tool,GhostArtwork& art) {
    RingPoints ring;Point a{},b{};Obj sproutParent{};
    if(bond) {
        if(!bondEndpoints(bond,a,b)) return false;
        // FuseRing chooses the less occupied side; this helper only counts
        // neighboring atoms and allocates its own geometry scratch vector.
        const int forward=fn<int(*)(Obj,const Point*,const Point*,bool)>(0x4ffe30)(page,&a,&b,false);
        const int backward=fn<int(*)(Obj,const Point*,const Point*,bool)>(0x4ffe30)(page,&b,&a,false);
        bool reverse{};
        if(backward!=forward) reverse=forward<backward;
        else {
            const bool odd=at<int>(bond,0xdc)==1&&tool.ringCode<8&&(tool.ringCode&1);
            reverse=std::abs(b.x-a.x)<=std::abs(b.y-a.y)?
                (odd?b.y<a.y:a.y<b.y):(odd?a.x<b.x:b.x<a.x);
        }
        if(!reverse) std::swap(a,b);
        if(!ring.calculate(a,b,tool)) return false;
        // FuseRing retries the opposite side if the preferred candidate adds
        // no ring vertices, then permits completing missing edges on the
        // preferred side if neither candidate adds atoms.
        if(!hasNewRingVertex(page,ring)) {
            std::swap(a,b);
            if(!ring.calculate(a,b,tool)) return false;
            if(!hasNewRingVertex(page,ring)) {
                std::swap(a,b);
                if(!ring.calculate(a,b,tool)) return false;
            }
        }
    } else if(!atom) {
        a=mouse;b={a.x,a.y+tool.length,a.z};
        if(tool.ringCode>=9) b={a.x+(tool.ringCode==9?0.5:-0.5)*tool.length,
            a.y+tool.length*std::sqrt(3.0)*0.5,a.z};
        if(!ring.calculate(a,b,tool)) return false;
        Point center{};for(size_t i=0;i<ring.count;++i) {
            center.x+=ring.points[i].x;center.y+=ring.points[i].y;
        }
        center.x/=double(ring.count);center.y/=double(ring.count);
        for(size_t i=0;i<ring.count;++i) {
            ring.points[i].x+=mouse.x-center.x;ring.points[i].y+=mouse.y-center.y;
        }
    } else {
        a=position(atom);Obj* first{};Obj* last{};
        if(!bonds(atom,first,last)) return false;
        size_t degree=first==last?0:size_t(last-first);
        bool sprouted{};
        if(degree>1&&tool.sproutRings) {
            Point connectorStart{},connectorEnd{};
            if(!firstBond(atom,a,tool,connectorStart,connectorEnd)) return false;
            const Obj existing=fn<Obj(*)(Obj,const Point*,Obj)>(0x7107c0)(page,&connectorEnd,nullptr);
            Obj* existingFirst{};Obj* existingLast{};
            // DropRing keeps the sprout only if its destination has exactly
            // one bond after placement. Otherwise it removes the connector
            // and continues with the original junction.
            if(!existing||(existing!=atom&&bonds(existing,existingFirst,existingLast)&&
                (existingFirst==existingLast||
                 (existingLast-existingFirst==1&&alreadyBonded(atom,existing))))) {
                if(existing) connectorEnd=position(existing);
                if(!existing||!alreadyBonded(atom,existing)) stroke(art,connectorStart,connectorEnd,tool.width);
                a=connectorEnd;
                b={2*a.x-connectorStart.x,2*a.y-connectorStart.y,a.z};
                if(!attachedRing(ring,a,b,tool)) return false;
                sproutParent=atom;
                sprouted=true;
            }
        }
        b={a.x,a.y+tool.length,a.z};
        if(!sprouted&&degree>2) {
            // DropRing tries each incident bond, in connectivity order, then
            // its reverse when a candidate ring contains no new vertices.
            bool found{};
            for(auto it=first;it!=last&&!found;++it) {
                const Obj other=neighbor(*it,atom);if(!other) continue;
                const auto p=position(other);
                for(bool reverse:{false,true}) {
                    if(ring.calculate(reverse?p:a,reverse?a:p,tool)&&hasNewRingVertex(page,ring)) {
                        found=true;break;
                    }
                }
            }
            if(!found) return false;
        } else if(!sprouted&&degree==2) {
            // Native spiro placement starts along the mean of the two outward
            // parent vectors, each normalized to the document bond length.
            Point away{};
            for(auto it=first;it!=last;++it) {
                Point start{},end{};
                fn<void(*)(Obj,Point*,Point*,Obj)>(0x3de900)(*it,&start,&end,atom);
                const double dx=start.x-end.x,dy=start.y-end.y,norm=std::hypot(dx,dy);
                if(!finite(start)||!finite(end)||norm<=0) return false;
                away.x+=dx*tool.length/norm;away.y+=dy*tool.length/norm;
            }
            b={a.x+away.x*0.5,a.y+away.y*0.5,a.z};
            if(!attachedRing(ring,a,b,tool)) return false;
        } else if(!sprouted&&degree==1) {
            const Obj other=neighbor(*first,atom);if(!other) return false;
            const auto p=position(other);b={2*a.x-p.x,2*a.y-p.y,a.z};
            if(!attachedRing(ring,a,b,tool)) return false;
        } else if(!sprouted&&degree==0&&!ring.calculate(a,b,tool)) return false;
    }
    const bool aromatic=tool.subtype==11||tool.subtype==12;
    RingBondPlan plan;if(!plan.calculate(page,ring,aromatic,sproutParent))return false;
    for(size_t i=0;i<ring.count;++i) {
        const bool occupied=plan.edges[i]||(bond&&i==0);
        const auto ringStart=ring.points[i],ringEnd=ring.points[(i+1)%ring.count];
        if(!occupied) stroke(art,ringStart,ringEnd,tool.width);
        if(aromatic&&plan.order[i]==2) {
            const double length=std::hypot(ringEnd.x-ringStart.x,ringEnd.y-ringStart.y);
            if(length<=0) continue;
            const double gap=spacing(tool,length);
            stroke(art,ringInsetEndpoint(ringStart,ringEnd,ring.points[(i+ring.count-1)%ring.count],gap),
                ringInsetEndpoint(ringEnd,ringStart,ring.points[(i+2)%ring.count],gap),tool.width);
        }
    }
    return !art.strokes.empty();
}
}
Obj findToolGhostTarget(Obj page,const Point& mouse) {
    if(!onUI()||!page||!finite(mouse)) return nullptr;
    if(nativeArrowTool(fn<int(*)()>(0x4f4ba0)())) return arrowSnapTarget(page,mouse);
    // Match the native atom-before-edge attachment priority. Resolve edges
    // from the live page hit index even if native hover feedback was cleared
    // or still refers to a preceding pointer position.
    if(const Obj atom=fn<Obj(*)(Obj,const Point*,Obj)>(0x7107c0)(page,&mouse,nullptr)) return atom;
    return fn<Obj(*)(Obj,const Point*,Obj)>(0x711500)(page,&mouse,nullptr);
}
bool readGhostTool(Obj page,GhostTool& tool) {
    if(!onUI()||!page) return false;
    tool.tool=fn<int(*)()>(0x4f4ba0)();
    tool.subtype=int(uint16_t(fn<short(*)()>(0x4f4bb0)()));
    if(tool.tool==3) {
        if(!ordinaryBondTool()) return false;
        tool.order=fn<int(*)()>(0x4fad40)();
        tool.type=uint8_t(fn<char(*)(bool)>(0x4fad80)(false));
    } else if(tool.tool==4) {
        const auto ui=reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ChemDrawUI.dll"));
        if(!ui) return false;
        tool.chainAtoms=std::clamp(*reinterpret_cast<const int*>(ui+0x747000),2,999);
    } else if(tool.tool==7) {
        if(tool.subtype<3||tool.subtype>12) return false;
        tool.ringCode=tool.subtype==11?5:tool.subtype==12?6:tool.subtype;
        tool.sproutRings=sproutRingPreference();
    } else if(nativeArrowTool(tool.tool)) {
        if(tool.subtype>0x90) return false;
    } else return false;
    tool.length=setting(page,0x805);tool.width=setting(page,0x807);
    // Both native color returns use MSVC's hidden result pointer. Retain only
    // the packed color; the rendering worker never accesses a native variant.
    alignas(8) std::array<std::byte,16> color{};
    const Obj doc=at<Obj>(page,8);if(!doc) return false;
    fn<Obj(*)(Obj,void*)>(0x14e380)(doc,color.data());
    fn<uint32_t*(*)(Obj,uint32_t*)>(0x30c7c0)(color.data(),&tool.ink);
    tool.spacing=setting(page,0x804);tool.chainAngle=setting(page,0x803);
    if((tool.tool==3&&tool.order==1&&tool.type==3)||nativeArrowTool(tool.tool))
        tool.dashSpacing=setting(page,0x809)*(*reinterpret_cast<const double*>(base+0x899ae0));
    tool.key=(uint64_t(tool.tool)<<48)|(uint64_t(tool.subtype)<<32)|uint32_t(tool.chainAtoms);
    return std::isfinite(tool.length)&&tool.length>0&&std::isfinite(tool.width)&&tool.width>0&&
        std::isfinite(tool.spacing)&&tool.spacing>=0&&std::isfinite(tool.chainAngle);
}
bool makeToolGhost(Obj page,Obj target,Point mouse,const GhostTool& tool,GhostBond& ghost) {
    RectD circle{};Obj atom=vertexHitCircle(target,circle)?target:nullptr;
    Obj bond=target&&at<uintptr_t>(target,0)==base+0x8babc0?target:nullptr;
    if(target&&!atom&&!bond) return false;
    if(bond&&!nativeArrowTool(tool.tool)&&!vf<bool(*)(Obj,int,short,const Point*,bool,bool)>(bond,0x2c0)(
        bond,tool.tool,short(tool.subtype),&mouse,false,false)) return false;
    auto art=std::make_shared<GhostArtwork>();
    ghost.width=tool.width;ghost.toolKey=tool.key;ghost.pressPoint=mouse;ghost.ink=tool.ink;
    ghost.identity=target?at<uint64_t>(target,0xb0)+1:0;
    if(nativeArrowTool(tool.tool)) {
        arrowArtwork(page,atom,bond,mouse,tool,ghost,*art);
    } else if(tool.tool==7) {
        if(!ringArtwork(page,atom,bond,mouse,tool,*art)) return false;
        ghost.start=art->strokes.front().start;ghost.end=art->strokes.front().end;
    } else if(tool.tool==3) {
        if(bond) {
            GhostTool changed{};if(!changedBondTool(bond,tool,changed)) return false;
            const Obj left=at<Obj>(bond,0xc0);
            if(!bondEndpoints(bond,ghost.start,ghost.end)) return false;
            bondArtwork(*art,ghost.start,ghost.end,changed,left);
        } else {
            if(!firstBond(atom,mouse,tool,ghost.start,ghost.end)) return false;
            bondArtwork(*art,ghost.start,ghost.end,tool,atom);
            ghost.placement=atom!=nullptr;
        }
    } else {
        if(bond||!firstBond(atom,mouse,tool,ghost.start,ghost.end)) return false;
        const int count=tool.chainAtoms-(atom?0:1);
        art->strokes.reserve(size_t(count));
        stroke(*art,ghost.start,ghost.end,tool.width);
        const double angle=(180.0-tool.chainAngle)*std::numbers::pi/360.0;
        double dx=tool.length*std::cos(angle),dy=tool.length*std::sin(angle);
        const double initialX=ghost.end.x-ghost.start.x,initialY=ghost.end.y-ghost.start.y;
        const bool vertical=std::abs(initialX)<std::abs(initialY);
        if(vertical) std::swap(dx,dy);
        if(initialX<0) dx=-dx;
        if(initialY<0) dy=-dy;
        Point previous=ghost.end;
        for(int i=1;i<count;++i) {
            if(vertical) dx=-dx;else dy=-dy;
            Point next{previous.x+dx,previous.y+dy,previous.z};
            const Obj existing=fn<Obj(*)(Obj,const Point*,Obj)>(0x7107c0)(page,&next,nullptr);
            if(existing&&vertexHitCircle(existing,circle)) next=position(existing);
            stroke(*art,previous,next,tool.width);previous=next;
        }
    }
    if(art->strokes.empty()&&art->paths.empty()) return false;
    ghost.artwork=std::move(art);ghost.visible=true;return true;
}
}

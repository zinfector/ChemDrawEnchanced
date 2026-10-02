#include "arrow-path.hpp"
#include "gpu-scene.hpp"
#include "curved-arrow.hpp"
#include "arrow-snap-math.hpp"
#include "arrow-insertion.hpp"
#include <string>
#include <sstream>
#include <iomanip>
#include <locale>
#include <unordered_set>
#include <windowsx.h>

namespace cd {
namespace {
// The native arrow owns attachments and their clipboard ID remapping. Only
// shape data goes in this persistent tag; it contains no object references.
const std::string tagName="Zuonik/SmartArrow/Path/v1";
constexpr double pi=3.14159265358979323846;
constexpr size_t maxNodes=256;
struct Shape {
    bool custom{};
    double angle{};
    Point tail{},head{};
    std::vector<Point> points; // Native CDSpline triplets: incoming, knot, outgoing.
    std::vector<uint8_t> smooth;
};
struct Identity { Obj page{};int id{-1};uint64_t instance{}; };
struct TreeNode { TreeNode *left,*parent,*right;uint8_t color,nil,pad[6];Obj object; };
std::unordered_map<Obj,Identity> lastArrows;
using Draw=void(*)(Obj,Obj);
using Bounds=RectD*(*)(Obj,RectD*);
using Near=bool(*)(Obj,const Point*);
using Unary=void(*)(Obj);
Draw oldDraw{};
Bounds oldBounds{};
Near oldNear{};
Unary oldValidate{};
thread_local bool shaping{};
thread_local Obj transientSpline{};
bool installed{};
bool isArrow(Obj o) { return o&&at<uintptr_t>(o,0)==base+0x8b2cf0; }
Identity identity(Obj o) { return {at<Obj>(o,0x60),at<int>(o,0x24),at<uint64_t>(o,0xb0)}; }
Obj resolve(const Identity& i) {
    if(!i.page||i.id<0) return nullptr;
    Obj o=fn<Obj(*)(Obj,int)>(0x712f80)(i.page,i.id);
    return isArrow(o)&&at<uint8_t>(o,0x34)&&at<uint64_t>(o,0xb0)==i.instance?o:nullptr;
}
Point add(Point a,Point b) { return {a.x+b.x,a.y+b.y,a.z+b.z}; }
Point sub(Point a,Point b) { return {a.x-b.x,a.y-b.y,a.z-b.z}; }
Point mul(Point a,double s) { return {a.x*s,a.y*s,a.z*s}; }
Point mix(Point a,Point b,double t) { return add(mul(a,1-t),mul(b,t)); }
double length(Point p) { return std::hypot(p.x,p.y); }
bool finite(Point p) { return std::isfinite(p.x)&&std::isfinite(p.y)&&std::isfinite(p.z); }
struct UndoOff {
    alignas(8) std::byte storage[16]{};
    explicit UndoOff(Obj o) { fn<void(*)(void*,Obj)>(0x501ff0)(storage,o); }
    ~UndoOff() { fn<void(*)(void*)>(0x5023f0)(storage); }
};
struct ShapeScope {
    bool previous=shaping;
    ShapeScope() { shaping=true; }
    ~ShapeScope() { shaping=previous; }
};
Point anchor(Obj arrow,int id,Point fallback) {
    Obj page=at<Obj>(arrow,0x60),o=id>=0?fn<Obj(*)(Obj,int)>(0x712f80)(page,id):nullptr;
    if(!o||!at<uint8_t>(o,0x34)) return fallback;
    Point attached{};if(arrowInsertionAttachment(arrow,id==at<int>(arrow,0x1f0),attached))return attached;
    Point molecule{};if(moleculeArrowAnchor(o,molecule))return molecule;
    if(at<uintptr_t>(o,0)==base+0x8b34f8) return at<Point>(o,0x1f8);
    if(at<uintptr_t>(o,0)==base+0x8babc0) {
        Obj a=at<Obj>(o,0xc0),b=at<Obj>(o,0xc8);
        if(a&&b) return mix(at<Point>(a,0x1f8),at<Point>(b,0x1f8),0.5);
    }
    return fallback;
}
bool endpoints(Obj arrow,Point& tail,Point& head) {
    Point t{},h{};
    fn<void(*)(Obj,Point*)>(0x2a4c90)(arrow,&t);
    fn<void(*)(Obj,Point*)>(0xdc4d0)(arrow,&h);
    // Native arrow getters already return page coordinates, including flips.
    tail=anchor(arrow,at<int>(arrow,0x1f0),t);
    head=anchor(arrow,at<int>(arrow,0x1f4),h);
    return finite(tail)&&finite(head)&&length(sub(head,tail))>1e-6;
}
bool readShape(Obj arrow,Shape& s) {
    Obj tag=fn<Obj(*)(Obj,const std::string*)>(0x328390)(arrow,&tagName);
    if(!tag||at<int>(tag,0x218)!=3) return false;
    const auto* value=fn<const std::string*(*)(Obj)>(0x32db70)(tag);
    if(!value||value->size()>200000) return false;
    std::istringstream in(*value);in.imbue(std::locale::classic());
    unsigned version{},custom{};size_t count{};
    if(!(in>>version>>custom>>s.angle>>s.tail.x>>s.tail.y>>s.tail.z>>s.head.x>>s.head.y>>s.head.z>>count)||
        version!=1||custom>1||!std::isfinite(s.angle)||std::abs(s.angle)>330||!finite(s.tail)||!finite(s.head)||
        count>maxNodes||count<2) return false;
    s.custom=custom!=0;s.points.resize(count*3);s.smooth.resize(count);
    for(size_t i=0;i<count;++i) {
        unsigned smooth{};if(!(in>>smooth)||smooth>1) return false;s.smooth[i]=uint8_t(smooth);
        for(size_t j=0;j<3;++j) {
            Point& p=s.points[3*i+j];
            if(!(in>>p.x>>p.y>>p.z)||!finite(p)||std::abs(p.x)>100||std::abs(p.y)>100) return false;
        }
    }
    s.points[1]={0,0,0};s.points[s.points.size()-2]={1,0,0};
    return true;
}
std::string encode(const Shape& s) {
    std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);
    out<<"1 "<<unsigned(s.custom)<<' '<<s.angle<<' '<<s.tail.x<<' '<<s.tail.y<<' '<<s.tail.z<<' '
       <<s.head.x<<' '<<s.head.y<<' '<<s.head.z<<' '<<s.smooth.size();
    for(size_t i=0;i<s.smooth.size();++i) {
        out<<' '<<unsigned(s.smooth[i]);
        for(size_t j=0;j<3;++j) { Point p=s.points[3*i+j];out<<' '<<p.x<<' '<<p.y<<' '<<p.z; }
    }
    return out.str();
}
std::vector<Point> arcPoints(double degrees) {
    double theta=std::clamp(degrees,-330.0,330.0)*pi/180;
    if(std::abs(theta)<1e-8) return {{0,0,0},{0,0,0},{1.0/3,0,0},{2.0/3,0,0},{1,0,0},{1,0,0}};
    // Negative angles put the arc above a left-to-right chord in screen space.
    const size_t segments=size_t(std::ceil(std::abs(theta)/(pi/3)));
    Point c{0.5,0.5/std::tan(theta/2),0},v=sub(Point{0,0,0},c);
    double step=theta/double(segments),k=4.0/3*std::tan(step/4);
    std::vector<Point> p((segments+1)*3);
    for(size_t i=0;i<=segments;++i) {
        double a=step*double(i);Point r{v.x*std::cos(a)-v.y*std::sin(a),v.x*std::sin(a)+v.y*std::cos(a),0};
        Point knot=add(c,r),tangent{-r.y*k,r.x*k,0};
        p[3*i]=sub(knot,tangent);p[3*i+1]=knot;p[3*i+2]=add(knot,tangent);
    }
    p[1]={0,0,0};p[p.size()-2]={1,0,0};return p;
}
std::vector<Point> shapePoints(const Shape& s) { return s.custom?s.points:arcPoints(s.angle); }
Point cubic(Point a,Point b,Point c,Point d,double t) {
    return mix(mix(mix(a,b,t),mix(b,c,t),t),mix(mix(b,c,t),mix(c,d,t),t),t);
}
std::vector<Point> worldPoints(const Shape& s,Point tail,Point head) {
    auto points=shapePoints(s);Point chord=sub(head,tail),reference=sub(s.head,s.tail);
    const double refLength=length(reference),newLength=length(chord);
    Point dt=sub(tail,s.tail),dh=sub(head,s.head);
    // If only one attachment moves, translate each endpoint's tangent with its
    // knot and blend displacement across interior knots. A common transform
    // uses the chord frame once for all points, including rotation and scaling.
    bool single=s.custom&&refLength>1e-6&&((length(dt)<1e-6)!=(length(dh)<1e-6));
    for(size_t i=0;i<points.size();++i) {
        Point p=points[i];
        if(single) {
            double weight=std::clamp(points[(i/3)*3+1].x,0.0,1.0);
            points[i]=add(add(s.tail,Point{reference.x*p.x-reference.y*p.y,
                reference.y*p.x+reference.x*p.y,reference.z*p.x+refLength*p.z}),mix(dt,dh,weight));
        } else points[i]=add(tail,Point{chord.x*p.x-chord.y*p.y,chord.y*p.x+chord.x*p.y,
            chord.z*p.x+newLength*p.z});
    }
    points[1]=tail;points[points.size()-2]=head;return points;
}
struct NativeSpline {
    UndoOff undo;
    alignas(16) std::byte data[400]{};
    Obj previous{};
    explicit NativeSpline(Obj arrow,const std::vector<Point>& points):undo(arrow) {
        previous=transientSpline;transientSpline=data;
        fn<void(*)(void*,Obj)>(0x343540)(data,at<Obj>(arrow,0x60));
        fn<void(*)(Obj,Obj)>(0x32aef0)(data,static_cast<std::byte*>(arrow)+0x80);
        fn<void(*)(Obj,const std::vector<Point>*)>(0xa5ba0)(data,&points);
        fn<void(*)(Obj,bool)>(0x349a80)(data,at<uint8_t>(arrow,0x160)!=0);
        fn<void(*)(Obj,bool)>(0x349be0)(data,at<uint8_t>(arrow,0x161)!=0);
        fn<void(*)(Obj,bool)>(0x349b20)(data,false);
        fn<void(*)(Obj,int)>(0x349a00)(data,fn<int(*)(Obj)>(0xdc440)(arrow));
        fn<void(*)(Obj,int)>(0x34a260)(data,fn<int(*)(Obj)>(0xdc520)(arrow));
        fn<void(*)(Obj,int)>(0x34a190)(data,fn<int(*)(Obj)>(0x8e5f0)(arrow));
        fn<void(*)(Obj,double)>(0x34a330)(data,fn<double(*)(Obj)>(0xdc530)(arrow));
        fn<void(*)(Obj,double)>(0x34a100)(data,fn<double(*)(Obj)>(0xdc4c0)(arrow));
        fn<void(*)(Obj,double)>(0x34a3c0)(data,fn<double(*)(Obj)>(0xdc540)(arrow));
    }
    ~NativeSpline() { fn<Unary>(0x9b540)(data);transientSpline=previous; }
};
void draw(Obj arrow,Obj port) {
    double activeAngle{};const bool resizing=nativeArrowAngle(arrow,activeAngle);
    GpuObjectScope owner(arrow,arrowPathTrackingPreview(arrow));
    if(shaping) { oldDraw(arrow,port);return; }
    Shape s;Point tail{},head{};
    ShapeScope scope;
    if(!resizing&&readShape(arrow,s)&&s.custom&&endpoints(arrow,tail,head)) {
        NativeSpline curve(arrow,worldPoints(s,tail,head));fn<Draw>(0x346e40)(curve.data,port);
    } else oldDraw(arrow,port);
}
RectD* bounds(Obj arrow,RectD* out) {
    if(shaping) return oldBounds(arrow,out);
    ShapeScope scope;oldBounds(arrow,out);Shape s;Point tail{},head{};
    if(!nativeArrowAngle(arrow,s.angle)&&readShape(arrow,s)&&s.custom&&endpoints(arrow,tail,head)) {
        NativeSpline curve(arrow,worldPoints(s,tail,head));fn<Bounds>(0x348610)(curve.data,out);
    }
    return out;
}
bool pathNear(Obj arrow,const Point* point) {
    if(shaping) return oldNear(arrow,point);
    ShapeScope scope;Shape s;Point tail{},head{};
    if(!nativeArrowAngle(arrow,s.angle)&&readShape(arrow,s)&&s.custom&&endpoints(arrow,tail,head)) {
        NativeSpline curve(arrow,worldPoints(s,tail,head));return fn<Near>(0x348da0)(curve.data,point);
    }
    return oldNear(arrow,point);
}
void validate(Obj arrow) {
    oldValidate(arrow);
    if(shaping) return;
    ShapeScope scope;
    // CalculateMatrixes (used by endpoint getters) calls EnsureValid. Native
    // validation must mark the arrow valid before attachment geometry is read.
    maintainArrowAttachments(arrow);
    if(!at<uint8_t>(arrow,0x31)) oldValidate(arrow);
    Shape s;Point tail{},head{};
    if(!nativeArrowAngle(arrow,s.angle)&&readShape(arrow,s)&&s.custom&&endpoints(arrow,tail,head)) {
        NativeSpline curve(arrow,worldPoints(s,tail,head));RectD r{};
        fn<Bounds>(0x348610)(curve.data,&r);at<RectD>(arrow,0x1a0)=r;
    }
}
#include "native-arrow-editing.inc"
#include "arrow-insertion.inc"

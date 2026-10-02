#pragma once
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace cd::arrowMath {
struct XY { double x{},y{}; };
inline bool finite(XY p) { return std::isfinite(p.x)&&std::isfinite(p.y); }
inline double segmentDistance(XY p,XY a,XY b) {
    if(!finite(p)||!finite(a)||!finite(b)) return std::numeric_limits<double>::infinity();
    const double dx=b.x-a.x,dy=b.y-a.y,length=dx*dx+dy*dy;
    const double t=length>0?std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/length,0.0,1.0):0;
    return std::hypot(p.x-a.x-t*dx,p.y-a.y-t*dy);
}
struct Candidate { int id{-1}; double distance{},penalty{}; };
// Distances, penalties and thresholds are in screen DIPs. Input is already in
// reverse native drawing order, which breaks equal-score ties deterministically.
inline int choose(std::span<const Candidate> candidates,int incumbent,
    double acquire=10,double retain=14,double switchMargin=2) {
    int best=-1;double score=std::numeric_limits<double>::infinity();
    double current=std::numeric_limits<double>::infinity();
    for(const auto& c:candidates) {
        if(c.id<0||!std::isfinite(c.distance)||c.distance<0||
            !std::isfinite(c.penalty)||c.penalty<0) continue;
        const double weighted=c.distance+c.penalty;
        if(c.id==incumbent&&c.distance<=retain) current=weighted;
        if(c.distance<=acquire&&weighted<score) { best=c.id;score=weighted; }
    }
    if(std::isfinite(current)&&(best<0||score+switchMargin>=current)) return incumbent;
    return best;
}
}

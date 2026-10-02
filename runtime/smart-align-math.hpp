#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace cd::alignMath {
struct Box {
    double l{},t{},r{},b{};
    bool valid() const { return std::isfinite(l+t+r+b)&&r>=l&&b>=t; }
    Box moved(double x,double y) const { return {l+x,t+y,r+x,b+y}; }
};
enum class Kind { None,Align,Between,After,Before };
struct Relation {
    Kind kind{Kind::None};int a{-1},b{-1},anchor{};double delta{};
    bool same(const Relation& o) const {return kind==o.kind&&a==o.a&&b==o.b&&anchor==o.anchor;}
};
inline double low(Box r,bool y) {return y?r.t:r.l;}
inline double high(Box r,bool y) {return y?r.b:r.r;}
inline double anchor(Box r,bool y,int n) {return n==0?low(r,y):n==2?high(r,y):(low(r,y)+high(r,y))*.5;}
inline bool lane(Box a,Box b,bool y,double unit) {
    const double overlap=std::min(high(a,!y),high(b,!y))-std::max(low(a,!y),low(b,!y));
    return overlap>std::min(high(a,!y)-low(a,!y),high(b,!y)-low(b,!y))*.2||
        std::abs(anchor(a,!y,1)-anchor(b,!y,1))<=8*unit;
}
inline bool fitsLane(Relation c,Box m,const std::vector<Box>& fixed,bool y,double unit) {
    return c.kind==Kind::None||c.kind==Kind::Align||
        (lane(m,fixed[c.a],y,unit)&&lane(m,fixed[c.b],y,unit));
}
inline Relation axis(Box m,const std::vector<Box>& fixed,const std::vector<int>& order,
    bool y,double unit,Relation incumbent,double limitLow,double limitHigh,double point,
    double lowWeight=1,double highWeight=1) {
    Relation best,current;double bestError=1e100,currentError=1e100;
    auto offer=[&](Relation c) {
        const double weight=c.kind==Kind::Between?(lowWeight+highWeight)*.5:
            c.kind==Kind::After?lowWeight:c.kind==Kind::Before?highWeight:
            c.anchor==0?lowWeight:c.anchor==2?highWeight:(lowWeight+highWeight)*.5;
        if(weight<=0)return;
        c.delta/=weight;
        const double error=std::abs(c.delta)/unit;
        if(!std::isfinite(error)||point+c.delta<limitLow||point+c.delta>limitHigh)return;
        if(c.same(incumbent)&&error<=10) {current=c;currentError=error;}
        if(error<=6&&(error<bestError-1e-8||(std::abs(error-bestError)<1e-8&&
            (int(c.kind)>int(best.kind)||(c.kind==best.kind&&c.a<best.a))))) {best=c;bestError=error;}
    };
    for(size_t i=0;i<fixed.size();++i) for(int n=0;n<3;++n)
        offer({Kind::Align,int(i),-1,n,anchor(fixed[i],y,n)-anchor(m,y,n)});
    // Each lane uses the first intersecting neighbor in sorted coordinate order.
    // This cannot skip a nearer same-lane object to manufacture a larger gap.
    for(size_t p=0;p<order.size();++p) {
        const int ai=order[p];const Box a=fixed[ai];
        if(!lane(m,a,y,unit))continue;
        for(size_t q=p+1;q<order.size();++q) {
            const int bi=order[q];const Box b=fixed[bi];
            if(!lane(a,b,y,unit)||!lane(m,b,y,unit))continue;
            const double gap=low(b,y)-high(a,y),width=high(m,y)-low(m,y);
            if(gap>width)offer({Kind::Between,ai,bi,0,high(a,y)+(gap-width)*.5-low(m,y)});
            if(gap>0) {
                // Only use a continuation if no same-lane object blocks the new gap.
                auto blocked=[&](double lo,double hi) {
                    for(size_t k=0;k<fixed.size();++k) if(int(k)!=ai&&int(k)!=bi&&
                        lane(m,fixed[k],y,unit)&&low(fixed[k],y)<hi&&high(fixed[k],y)>lo)return true;
                    return false;
                };
                if(std::abs(high(b,y)+gap-low(m,y))<=10*unit&&!blocked(high(b,y),high(b,y)+gap+width))
                    offer({Kind::After,ai,bi,0,high(b,y)+gap-low(m,y)});
                if(std::abs(low(a,y)-gap-width-low(m,y))<=10*unit&&!blocked(low(a,y)-gap-width,low(a,y)))
                    offer({Kind::Before,ai,bi,0,low(a,y)-gap-width-low(m,y)});
            }
            break;
        }
    }
    if(current.kind!=Kind::None&&bestError+2>=currentError)return current;
    return best;
}
}

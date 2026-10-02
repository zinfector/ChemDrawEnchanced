#include "runtime.hpp"
#include "placement-runtime.hpp"
#include "curved-arrow.hpp"
#include "arrow-path.hpp"
#include "smart-align.hpp"
#include "reaction-suggestion.hpp"

namespace cd {
struct Node { Node* left; Node* parent; Node* right; uint8_t color,nil; uint8_t pad[6]; Obj object; };
static_assert(offsetof(Node,object)==0x20);
struct Item { Obj object{}; Obj atom{}; bool dirty{},unbounded{}; std::vector<uint64_t> cells; };
struct Index {
    uint64_t epoch{}; size_t count{}; Node* head{}; double scale{},cell{},displayMetric{};
    std::vector<Item> ordered;
    std::unordered_map<uint64_t,std::vector<uint32_t>> cells;
    std::vector<uint32_t> unbounded, candidates;
    std::unordered_map<Obj,uint32_t> slots;
    std::vector<uint32_t> dirty,freeSlots;
};
static std::unordered_map<Obj,Index> indices;
// Hover/cursor generation changes do not imply new spatial bounds.
static std::atomic<uint64_t> geometryEpoch{1};
static thread_local bool building{};
static thread_local bool updatingBounds{};
using Find = Obj(*)(Obj,const Point*,Obj);
static Find oldFind{};
static Find oldFindAtom{};
using Unary = void(*)(Obj);
static Unary oldInvalidate{},oldLastFound{},oldPageDestroy{},oldObjectDestroy{},oldNeedValidate{};
static void (*oldAddObject)(Obj,Obj,int){};
static void (*oldRemoveObject)(Obj,Obj){};
using Cast = Obj(*)(Obj,long,Obj,Obj,int);
static Cast dynamicCast{};

using Near = bool(*)(Obj,const Point*);
using Bounds = RectD*(*)(Obj,RectD*);
static Near oldAtomNear{};
static Near oldBondNear{};
using Tolerance = double(*)(Obj);
static Tolerance oldTolerance{};
static thread_local Obj hitBond{};
// Fractions of the display's native small-icon extent, not fixed pixel sizes.
constexpr double bondDefaultRadiusRatio=0.3125, bondMaxRadiusRatio=0.625;
static Bounds oldHotKeyBounds{};
static Unary oldAtomHighlightInvalidation{};
using Highlight=void(*)(Obj,Obj);
static Highlight oldAtomHighlight{};
static Highlight oldBondHighlight{};
using Hilite=void(*)(Obj,Obj,int);
static Hilite oldAtomHilite{};
static Highlight oldAtomDrawHighlight{};

static double displayMetricForPage(Obj page) {
    Obj doc=page?at<Obj>(page,8):nullptr;
    Obj port=doc?at<Obj>(doc,0x258):nullptr;
    HWND window=port?portWindow(port):nullptr;
    UINT dpi=window?GetDpiForWindow(window):0;
    if(!dpi) dpi=GetDpiForSystem();
    static thread_local UINT cachedDpi{};
    static thread_local double cachedMetric{};
    if(cachedDpi!=dpi||cachedMetric<=0) {
        int width=GetSystemMetricsForDpi(SM_CXSMICON,dpi);
        int height=GetSystemMetricsForDpi(SM_CYSMICON,dpi);
        if(width<=0||height<=0) {
            width=GetSystemMetrics(SM_CXSMICON);height=GetSystemMetrics(SM_CYSMICON);
        }
        cachedMetric=double(std::max(1,std::min(width,height)));
        cachedDpi=dpi;
    }
    return cachedMetric;
}
static double zoomHitRadius(Obj o,double units,double defaultRatio,double maxRatio) {
    const double metric=displayMetricForPage(o?at<Obj>(o,0x60):nullptr);
    const double defaultPixels=metric*defaultRatio, maxPixels=metric*maxRatio;
    // kScreenScale is ChemDraw's document-units-per-pixel at 100% zoom.
    // Grow a fixed document-space radius with zoom, capped at the enlarged
    // screen-space size. Read the live scale so every document/zoom agrees.
    const double normalUnits=*reinterpret_cast<double*>(base+0xb3ea10);
    if(!std::isfinite(normalUnits)||normalUnits<=0) return defaultPixels*units;
    return std::min(defaultPixels*normalUnits,maxPixels*units);
}
static double bondTolerance(Obj o,double native) {
    Obj scale=o?at<Obj>(o,0x68):nullptr;
    const double units=scale?at<double>(scale,8):0;
    if(!std::isfinite(native)||native<0||!std::isfinite(units)||units<=0) return native;
    return std::max(native,zoomHitRadius(o,units,bondDefaultRadiusRatio,bondMaxRadiusRatio));
}
static double hitTolerance(Obj o) {
    const double native=oldTolerance(o);
    // Scope the wider tolerance to the bond's native hit predicate. Endpoint
    // atom tests and geometry calculations keep their own tolerances.
    return onUI()&&o==hitBond?bondTolerance(o,native):native;
}
static bool bondNear(Obj o,const Point* point) {
    if(!onUI()) return oldBondNear(o,point);
    const Obj previous=hitBond;hitBond=o;
    bool result{};
    try { result=oldBondNear(o,point); }
    catch(...) { hitBond=previous;throw; }
    hitBond=previous;
    return result;
}

// Restrict the enlargement to plain, unlabeled point vertices. Labels, monomers,
// rank indicators and other atoms with meaningful geometry retain native rules.
static double parentBondLength(Obj atom) {
    // Chemical connectivity preserves an attached-bond order. The first
    // nondegenerate bond is the parent used consistently for junction vertices.
    auto first=at<Obj*>(atom,0x120),last=at<Obj*>(atom,0x128);
    for(auto bond=first;bond!=last;++bond) {
        if(!*bond) continue;
        Obj a=at<Obj>(*bond,0xc0),b=at<Obj>(*bond,0xc8);
        if(!a||!b||(a!=atom&&b!=atom)) continue;
        Point start{},end{};
        vf<Point*(*)(Obj,Point*)>(a,0x1f0)(a,&start);
        vf<Point*(*)(Obj,Point*)>(b,0x1f0)(b,&end);
        const double length=std::hypot(end.x-start.x,end.y-start.y);
        if(std::isfinite(length)&&length>0) return length;
    }
    Obj page=at<Obj>(atom,0x60);
    if(!page) return 0;
    Obj setting=fn<Obj(*)(Obj,int)>(0x7128f0)(page,0x805);
    return setting?fn<double(*)(Obj)>(0x37be40)(setting):0;
}
static bool vertexBounds(Obj o,RectD& result) {
    if(!onUI()||at<uintptr_t>(o,0)!=base+0x8b34f8||at<Obj>(o,0xc8)) return false;
    if(fn<bool(*)(Obj)>(0x2c6db0)(o)) return false;
    Obj drawer=at<Obj>(o,0x210);
    if(!drawer||vf<bool(*)(Obj)>(drawer,8)(drawer)) return false;
    const int type=at<int>(o,0xc0);
    if(type==10||type==11||type==0x10||type==0x11) return false;
    RectD own{}; vf<Bounds>(o,0x1d0)(o,&own);
    if(!valid(own)||own.r-own.l>1.000001||own.b-own.t>1.000001) return false;
    Obj scale=at<Obj>(o,0x68);
    const double units=scale?at<double>(scale,8):0;
    if(!std::isfinite(units)||units<=0) return false;
    const double radius=parentBondLength(o)/3.0;
    if(!std::isfinite(radius)||radius<=0) return false;
    const Point center=at<Point>(o,0x1f8);
    result={center.y-radius,center.x-radius,center.y+radius,center.x+radius};
    return valid(result);
}
static bool atomNear(Obj o,const Point* point) {
    RectD r{};
    if(!point||!vertexBounds(o,r)) return oldAtomNear(o,point);
    const double radius=(r.r-r.l)*0.5;
    return std::hypot(point->x-(r.l+radius),point->y-(r.t+radius))<=radius;
}
bool vertexHitCircle(Obj o,RectD& result) { return o&&vertexBounds(o,result); }
static void highlightAtom(Obj o,Obj highlighter) {
    if(gpuOwnsPlacementHighlight(o)||gpuOwnsHoverHighlight(o,highlighter)) return;
    RectD r{};
    if(highlighter&&vertexBounds(o,r)) {
        // Fill the native oval so the entire active radial hit area is visible.
        fn<void(*)(Obj,const RectD*)>(0x447760)(highlighter,&r);return;
    }
    oldAtomHighlight(o,highlighter);
}
static void highlightBond(Obj bond,Obj highlighter) {
    if(gpuOwnsHoverHighlight(bond,highlighter)) return;
    oldBondHighlight(bond,highlighter);
}
static void hiliteAtom(Obj atom,Obj highlighter,int character) {
    if(gpuOwnsPlacementHighlight(atom)||gpuOwnsHoverHighlight(atom,highlighter)) return;
    oldAtomHilite(atom,highlighter,character);
}
static void drawAtomHighlight(Obj atom,Obj port) {
    if(gpuOwnsPlacementHighlight(atom)) return;
    oldAtomDrawHighlight(atom,port);
}
static RectD* hotKeyBounds(Obj o,RectD* result) {
    oldHotKeyBounds(o,result);
    RectD minimum{};
    if(vertexBounds(o,minimum)) {
        // Feedback and spatial indexing share the circle's bounding rectangle.
        *result=minimum;
    }
    return result;
}
static void invalidateAtomHighlight(Obj o) {
    oldAtomHighlightInvalidation(o);
    RectD rect{};
    Obj page=at<Obj>(o,0x60);
    if(page&&vertexBounds(o,rect)) {
        const double pad=2.0*at<double>(at<Obj>(o,0x68),8);
        rect.t-=pad;rect.l-=pad;rect.b+=pad;rect.r+=pad;
        fn<void(*)(Obj,const RectD*)>(0x715a50)(page,&rect);
    }
}

static uint64_t cellKey(int x,int y) { return (uint64_t(uint32_t(x))<<32)|uint32_t(y); }
static bool cellCoord(double value,double cell,int& result) {
    const double v=std::floor(value/cell);
    if (!std::isfinite(v)||v < INT_MIN+1.0||v > INT_MAX-1.0) return false;
    result=int(v); return true;
}
static Node* previous(Node* n) {
    if (n->nil) return n->right;
    if (!n->left->nil) {
        n=n->left;
        while (!n->right->nil) n=n->right;
        return n;
    }
    Node* p=n->parent;
    while (!p->nil && n==p->left) { n=p; p=p->parent; }
    return p;
}
// Only reject candidates with bounds proven by their native Near() routines.
// Complex atoms and other object types retain their exact native predicates.
static bool boundsFor(Obj o,RectD& r) {
    if(vertexBounds(o,r)) return true;
    const uintptr_t table=at<uintptr_t>(o,0);
    size_t slot{};
    if (table==base+0x8babc0) slot=0x1c8; // CDBond::Near starts with these bounds.
    else if (table==base+0x8b34f8 && !at<Obj>(o,0xc8) && at<int>(o,0xc0)!=0x10) {
        Obj drawer=at<Obj>(o,0x210);
        if (!drawer || vf<bool(*)(Obj)>(drawer,8)(drawer)) return false;
        slot=0x1d0; // Ordinary atom Near uses its own bounds, not selection bounds.
    } else return false;
    vf<RectD*(*)(Obj,RectD*)>(o,slot)(o,&r);
    double tolerance=fn<double(*)(Obj)>(0x327890)(o);
    if(table==base+0x8babc0) tolerance=bondTolerance(o,tolerance);
    const double epsilon=*reinterpret_cast<double*>(base+0x896110);
    if (!valid(r)||!std::isfinite(tolerance)||tolerance<0) return false;
    const double pad=tolerance+std::abs(epsilon)*2;
    r.t-=pad; r.l-=pad; r.b+=pad; r.r+=pad;
    return valid(r);
}
static void removeBounds(Index& index,uint32_t id) {
    auto& item=index.ordered[id];
    for(auto key:item.cells) if(auto cell=index.cells.find(key);cell!=index.cells.end()) {
        auto& entries=cell->second;entries.erase(std::remove(entries.begin(),entries.end(),id),entries.end());
        if(entries.empty()) index.cells.erase(cell);
    }
    item.cells.clear();
    if(item.unbounded) {
        auto& entries=index.unbounded;entries.erase(std::remove(entries.begin(),entries.end(),id),entries.end());
        item.unbounded=false;
    }
}
static void insertBounds(Index& index,uint32_t id) {
    auto& item=index.ordered[id];RectD rect{};int x0{},y0{},x1{},y1{};
    if(!boundsFor(item.object,rect)||!cellCoord(rect.l,index.cell,x0)||
        !cellCoord(rect.r,index.cell,x1)||!cellCoord(rect.t,index.cell,y0)||
        !cellCoord(rect.b,index.cell,y1)||int64_t(x1)-x0>64||int64_t(y1)-y0>64||
        (int64_t(x1)-x0+1)*(int64_t(y1)-y0+1)>256) {
        index.unbounded.push_back(id);item.unbounded=true;return;
    }
    for(int64_t x=x0;x<=x1;++x) for(int64_t y=y0;y<=y1;++y) {
        const auto key=cellKey(int(x),int(y));
        index.cells[key].push_back(id);item.cells.push_back(key);
    }
}
static uint32_t insertItem(Index& index,Obj object,bool dirty) {
    uint32_t id{};
    if(index.freeSlots.empty()) { id=uint32_t(index.ordered.size());index.ordered.emplace_back(); }
    else { id=index.freeSlots.back();index.freeSlots.pop_back(); }
    auto& item=index.ordered[id];item.object=object;
    item.atom=dynamicCast(object,0,reinterpret_cast<Obj>(base+0xb46358),
        reinterpret_cast<Obj>(base+0xb46378),0);
    index.slots.emplace(object,id);item.dirty=dirty;
    if(dirty) index.dirty.push_back(id);else insertBounds(index,id);
    return id;
}
static void rebuild(Obj page,Index& index,double scale,double displayMetric) {
    const auto epoch=geometryEpoch.load(std::memory_order_relaxed);
    index.epoch=0;
    index.ordered.clear();index.cells.clear();index.unbounded.clear();
    index.slots.clear();index.dirty.clear();index.freeSlots.clear();
    Node* head=at<Node*>(page,0xc8);
    index.head=head; index.count=at<size_t>(page,0xd0);
    index.scale=scale; index.cell=std::max(std::abs(scale)*64.0,1e-9);
    index.displayMetric=displayMetric;
    index.ordered.reserve(index.count);
    for (Node* n=head->right;n!=head;n=previous(n)) {
        insertItem(index,n->object,false);
    }
    index.epoch=epoch;
}
static void markDirty(Obj object) {
    if(!onUI()||updatingBounds) { geometryEpoch.fetch_add(1,std::memory_order_relaxed);return; }
    Obj page=at<Obj>(object,0x60);auto found=indices.find(page);if(found==indices.end()) return;
    auto& index=found->second;auto slot=index.slots.find(object);if(slot==index.slots.end()) return;
    auto& item=index.ordered[slot->second];
    if(!item.dirty) {
        try { index.dirty.push_back(slot->second);item.dirty=true; }
        catch(const std::bad_alloc&) { index.epoch=0; }
    }
}
static void refreshDirty(Index& index) {
    // During a native gesture edited objects may have unfinished bounds. Keep
    // them as exact-test candidates; unchanged page objects remain indexed.
    if(trackingDepth) return;
    size_t remaining{};
    for(uint32_t id:index.dirty) {
        auto& item=index.ordered[id];if(!item.object||!item.dirty) continue;
        if(!at<uint8_t>(item.object,0x31)) { index.dirty[remaining++]=id;continue; }
        building=updatingBounds=true;
        try { removeBounds(index,id);insertBounds(index,id); }
        catch(...) { building=updatingBounds=false;index.epoch=0;throw; }
        building=updatingBounds=false;item.dirty=false;
    }
    index.dirty.resize(remaining);
}
static Index* candidatesFor(Obj page,const Point* point) {
    Obj scalePtr=at<Obj>(page,0x2a8);
    double scale=at<double>(scalePtr,8);
    if (!std::isfinite(scale)||scale<=0) return nullptr;
    const double displayMetric=displayMetricForPage(page);
    Index* cached{};
    try {
        cached=&indices[page];
        auto& index=*cached;
        if (index.epoch!=geometryEpoch.load(std::memory_order_relaxed) || index.scale!=scale ||
            index.displayMetric!=displayMetric ||
            index.head!=at<Node*>(page,0xc8)||index.count!=at<size_t>(page,0xd0)) {
            building=updatingBounds=true;
            try { rebuild(page,index,scale,displayMetric); } catch (...) { building=updatingBounds=false; throw; }
            building=updatingBounds=false;
        }
        refreshDirty(index);
        int x{},y{};
        if (!cellCoord(point->x,index.cell,x)||!cellCoord(point->y,index.cell,y))
            return nullptr;
        index.candidates=index.unbounded;
        for(uint32_t id:index.dirty) if(index.ordered[id].object&&index.ordered[id].dirty)
            index.candidates.push_back(id);
        auto it=index.cells.find(cellKey(x,y));
        if (it!=index.cells.end()) index.candidates.insert(index.candidates.end(),it->second.begin(),it->second.end());
        auto& entries=index.candidates;
        entries.erase(std::remove_if(entries.begin(),entries.end(),[&](uint32_t id) {
            return !index.ordered[id].object;
        }),entries.end());
        // Page's native ordered tree compares CDObject::mIndex (+0x20).
        // Read live indices so bring-to-front and incremental insertions agree.
        std::sort(entries.begin(),entries.end(),[&](uint32_t a,uint32_t b) {
            const int first=at<int>(index.ordered[a].object,0x20),second=at<int>(index.ordered[b].object,0x20);
            return first!=second?first>second:a<b;
        });
        entries.erase(std::unique(entries.begin(),entries.end()),entries.end());
    } catch (const std::bad_alloc&) { if(cached) cached->epoch=0;return nullptr; }
    return cached;
}
bool spatialCandidatesInRadius(Obj page,const Point& point,double radius,std::vector<Obj>& objects) {
    if(!onUI()||!page||building||!std::isfinite(radius)||radius<0) return false;
    Index* cached=candidatesFor(page,&point);if(!cached) return false;
    auto& index=*cached;int x0{},x1{},y0{},y1{};
    if(!cellCoord(point.x-radius,index.cell,x0)||!cellCoord(point.x+radius,index.cell,x1)||
        !cellCoord(point.y-radius,index.cell,y0)||!cellCoord(point.y+radius,index.cell,y1)||
        int64_t(x1)-x0>16||int64_t(y1)-y0>16) return false;
    auto entries=index.candidates;
    for(int64_t x=x0;x<=x1;++x) for(int64_t y=y0;y<=y1;++y) {
        const auto found=index.cells.find(cellKey(int(x),int(y)));
        if(found!=index.cells.end()) entries.insert(entries.end(),found->second.begin(),found->second.end());
    }
    std::sort(entries.begin(),entries.end());entries.erase(std::unique(entries.begin(),entries.end()),entries.end());
    objects.reserve(entries.size());
    for(uint32_t id:entries) if(index.ordered[id].object) objects.push_back(index.ordered[id].object);
    std::stable_sort(objects.begin(),objects.end(),[](Obj a,Obj b){return at<int>(a,0x20)>at<int>(b,0x20);});
    // Copy membership before any caller invokes native bounds/validation callbacks.
    return true;
}
static Obj findIndexed(Obj page,const Point* point,Obj exclude) {
    Obj snapped{};
    if(!building&&smartArrowHit(page,point,exclude,snapped)) return snapped;
    if (!onUI()||!page||!point||exclude||building) return oldFind(page,point,exclude);
    if (point->x==at<double>(page,0x15f0) && point->y==at<double>(page,0x15f8) &&
        point->z==at<double>(page,0x1600)) return oldFind(page,point,exclude);
    if (at<size_t>(page,0xd0)<64) return oldFind(page,point,exclude);
    Index* cached=candidatesFor(page,point);if(!cached) return oldFind(page,point,exclude);
    Obj found{};
    // Preserve reverse drawing order and native exact hit tests, including atom
    // visibility. The tool-specific repeat/skip path remains fully native.
    building=true;
    try {
        for (uint32_t id:cached->candidates) {
            const Item& item=cached->ordered[id];
            Obj o=item.object;
            if ((!item.atom || vf<bool(*)(Obj)>(o,0xb8)(o)) && at<uint8_t>(o,0x34) &&
                vf<bool(*)(Obj,const Point*)>(o,0x278)(o,point)) { found=o; break; }
        }
    } catch (...) { building=false; throw; }
    building=false;
    at<Point>(page,0x15f0)=*point; at<Obj>(page,0x15e8)=found;
    return found;
}
static Obj findAtomIndexed(Obj page,const Point* point,Obj exclude) {
    if(!onUI()||!page||!point||building||at<size_t>(page,0xd0)<64)
        return oldFindAtom(page,point,exclude);
    Index* cached=candidatesFor(page,point);if(!cached) return oldFindAtom(page,point,exclude);
    Obj found{};building=true;
    try {
        // FindCDAtom has two reverse-drawing-order passes: labeled atoms first,
        // then unlabeled atoms. Retain its visibility, exclusion and rank rules.
        for(bool labeled:{true,false}) {
            for(uint32_t id:cached->candidates) {
                Obj atom=cached->ordered[id].atom;if(!atom||atom==exclude||!at<uint8_t>(atom,0x34)) continue;
                if(fn<bool(*)(Obj)>(0x2c6db0)(atom)!=labeled) continue;
                if(at<int>(atom,0xc0)==0x10) {
                    const int rank=at<int>(atom,0x110);
                    if(!rank||((uint32_t(rank)-1U&0xfffffffcU)==0&&rank!=3)) continue;
                }
                if(vf<bool(*)(Obj)>(atom,0xb8)(atom)&&vf<bool(*)(Obj,const Point*)>(atom,0x278)(atom,point)) {
                    found=atom;break;
                }
            }
            if(found) break;
        }
    } catch(...) { building=false;throw; }
    building=false;return found;
}
void forgetPage(Obj page) { forgetArrowPage(page);indices.erase(page);geometryEpoch.fetch_add(1,std::memory_order_relaxed);bumpGeneration(); }
  static void invalidateObject(Obj o) {
    // Native drag drawing invalidates bounds caches, including stationary ink.
    // The modal drag owns geometry; cache housekeeping cannot retire its guides.
    if(arrowPathTransient(o)) {oldInvalidate(o);return;}
    if(onUI()&&at<uintptr_t>(o,0)==base+0x8b2cf0)reactionObjectChanged(o);
    smartAlignmentGeometryInvalidated(o);
    markDirty(o);bumpGeneration();oldInvalidate(o);arrowPathInvalidated(o);markDirty(o);
}
static void invalidateLast(Obj p) { bumpGeneration(); oldLastFound(p); }
  static void destroyPage(Obj p) {
    if(onUI())forgetReactionPage(p);
    smartAlignmentPageGone(p);
    if(onUI()) { forgetPlacementPage(p);clearBondPlacement(nullptr);forgetPage(p); }
    else { geometryEpoch.fetch_add(1,std::memory_order_relaxed);bumpGeneration(); }
    oldPageDestroy(p);
}
static void eraseItem(Index& index,Obj object) {
    auto slot=index.slots.find(object);if(slot==index.slots.end()) return;
    const auto id=slot->second;removeBounds(index,id);index.slots.erase(slot);
    auto& pending=index.dirty;pending.erase(std::remove(pending.begin(),pending.end(),id),pending.end());
    index.ordered[id]=Item{};index.freeSlots.push_back(id);
}
  static void destroyObject(Obj o) {
    smartAlignmentObjectChanged(o);
    if(arrowPathTransient(o)) {oldObjectDestroy(o);return;}
    if(onUI())reactionPageChanged(at<Obj>(o,0x60));
    if(onUI()) forgetArrowPathObject(o);
    clearBondPlacement(nullptr);bumpGeneration();
    if(onUI()&&!building) {
        auto found=indices.find(at<Obj>(o,0x60));
        if(found!=indices.end()) {
            // Destruction without Page::RemoveObject must force membership
            // reconstruction, while discarding the pointer before it is freed.
            found->second.epoch=0;
            try { eraseItem(found->second,o); } catch(const std::bad_alloc&) { indices.erase(found); }
        }
    } else geometryEpoch.fetch_add(1,std::memory_order_relaxed);
    oldObjectDestroy(o);
}
static void addObject(Obj page,Obj object,int id) {
    const auto before=at<size_t>(page,0xd0);oldAddObject(page,object,id);
    smartAlignmentGeometryInvalidated(object);
    reactionPageChanged(page);
    if(!onUI()||building) { geometryEpoch.fetch_add(1,std::memory_order_relaxed);return; }
    auto found=indices.find(page);if(found==indices.end()) return;
    auto& index=found->second;const auto after=at<size_t>(page,0xd0);
    if(index.head!=at<Node*>(page,0xc8)||index.count!=before||after!=before+1) { index.epoch=0;return; }
    try { if(!index.slots.contains(object)) insertItem(index,object,true);index.count=after; }
    catch(const std::bad_alloc&) { indices.erase(found); }
}
  static void removeObject(Obj page,Obj object) {
    reactionPageChanged(page);
    smartAlignmentObjectChanged(object);
    // Retain the native object's lifetime and callbacks. Membership is updated
    // only after the native removal has completed; reentrant queries can rebuild.
    const auto before=at<size_t>(page,0xd0);oldRemoveObject(page,object);
    if(!onUI()||building) { geometryEpoch.fetch_add(1,std::memory_order_relaxed);return; }
    auto found=indices.find(page);if(found==indices.end()) return;
    auto& index=found->second;const auto after=at<size_t>(page,0xd0);
    try {
        eraseItem(index,object);
        if(index.head!=at<Node*>(page,0xc8)||index.count!=before||!before||after!=before-1) index.epoch=0;
        index.count=after;
    } catch(const std::bad_alloc&) { indices.erase(found); }
}
static void needValidate(Obj d) {
    // This only schedules native validation (document +0x40c). Object
    // invalidation, membership changes and scale changes own spatial updates.
    bumpGeneration();oldNeedValidate(d);
}
void installSpatial() {
    dynamicCast=reinterpret_cast<Cast>(GetProcAddress(GetModuleHandleW(L"vcruntime140.dll"),"__RTDynamicCast"));
    if (!dynamicCast) throw std::runtime_error("C++ runtime unavailable");
    hook(0x327890,hitTolerance,oldTolerance);
    hook(0x2efce0,bondNear,oldBondNear);
    hook(0x2c9550,atomNear,oldAtomNear);
    hook(0x2c39d0,highlightAtom,oldAtomHighlight);
    hook(0x2eef50,highlightBond,oldBondHighlight);
    hook(0x2c3b60,hiliteAtom,oldAtomHilite);
    hook(0x2b91d0,drawAtomHighlight,oldAtomDrawHighlight);
    hook(0x2bfc30,hotKeyBounds,oldHotKeyBounds);
    hook(0x2c4860,invalidateAtomHighlight,oldAtomHighlightInvalidation);
    if (patchEnabled(2)) hook(0x711500,findIndexed,oldFind);
    if (patchEnabled(2)) hook(0x7107c0,findAtomIndexed,oldFindAtom);
    hook(0x328a90,invalidateObject,oldInvalidate);
    hook(0x715aa0,invalidateLast,oldLastFound);
    hook(0x70adc0,destroyPage,oldPageDestroy);
    hook(0x324920,destroyObject,oldObjectDestroy);
    hook(0x1523e0,needValidate,oldNeedValidate);
    hook(0x70b790,addObject,oldAddObject);
    hook(0x719db0,removeObject,oldRemoveObject);
}
}

#include "runtime.hpp"
#include "reaction-suggestion.hpp"

namespace cd {
namespace {
using Undo=short(*)(Obj);
using ZoomWithoutCenter=void(*)(Obj,double,bool);
using PerformRecord=void(*)(Obj,Obj);
using InvalWindow=void(*)(Obj,const RectD*);
using SaveStack=void(*)(Obj);
using Enabled=bool(*)(Obj);
Undo oldUndo{},oldRedo{};
SaveStack oldPushUndo{},oldPushRedo{};
Enabled oldUndoEnabled{},oldRedoEnabled{};
ZoomWithoutCenter oldZoomWithoutCenter{};
PerformRecord oldPerformRecord{};
InvalWindow oldInvalWindow{};

struct Bounds {
    RectD rect{};bool present{};
    void add(const RectD& r) {
        if(!valid(r)) return;
        rect=present?RectD{std::min(rect.t,r.t),std::min(rect.l,r.l),
            std::max(rect.b,r.b),std::max(rect.r,r.r)}:r;
        present=true;
    }
};
struct UndoArea { Obj doc{},page{};Bounds before,after,damage; };
thread_local UndoArea* active{};
thread_local unsigned boundsDepth{};
struct AreaScope {
    UndoArea* previous;
    explicit AreaScope(UndoArea& area):previous(active) { active=&area; }
    ~AreaScope() { active=previous; }
};
// Native DoZoomWithoutCenter allocates UndoMagnify even during a camera
// commit. Its EndRecordUndo also flushes an unfinished edit unless recording
// is disabled. Use the native nested disabler for this entire camera routine;
// never disable the real undo operation or its inverse commands for redo.
struct CameraUndoOff {
    alignas(8) std::byte storage[16]{};
    explicit CameraUndoOff(Obj doc) { fn<void(*)(void*,Obj)>(0x501fd0)(storage,doc); }
    ~CameraUndoOff() { fn<void(*)(void*)>(0x5023f0)(storage); }
};
struct BoundsReadScope {
    CameraUndoOff off;
    explicit BoundsReadScope(Obj doc):off(doc) { ++boundsDepth; }
    ~BoundsReadScope() { --boundsDepth; }
};
void zoomWithoutCenter(Obj doc,double scale,bool refresh) {
    ReactionCameraScope reactionCamera;
    CameraUndoOff off(doc);
    oldZoomWithoutCenter(doc,scale,refresh);
}
bool emptyRecord(Obj record) {
    // Selection/hot-key snapshots accompany every record. They do not make a
    // transaction into a document edit: its command vector must contain work.
    return !record||at<Obj*>(record,8)==at<Obj*>(record,0x10);
}
Obj stackRecord(Obj doc,bool redo,size_t distance=0) {
    const size_t count=at<size_t>(doc,redo?0xe8:0xc0);
    if(distance>=count) return nullptr;
    const size_t index=at<size_t>(doc,redo?0xe0:0xb8)+count-1-distance;
    // Native MSVC deque layout, also used by PopUndoStack/PopRedoStack.
    const auto map=at<Obj**>(doc,redo?0xd0:0xa8);
    const size_t blocks=at<size_t>(doc,redo?0xd8:0xb0);
    return map[(index>>1)&(blocks-1)][index&1];
}
bool hasStackEdit(Obj doc,bool redo) {
    const size_t count=at<size_t>(doc,redo?0xe8:0xc0);
    for(size_t distance=0;distance<count;++distance)
        if(!emptyRecord(stackRecord(doc,redo,distance))) return true;
    return false;
}
bool undoEnabled(Obj doc) {
    if(!onUI()||!doc) return oldUndoEnabled(doc);
    return !emptyRecord(at<Obj>(doc,0xf0))||hasStackEdit(doc,false);
}
bool redoEnabled(Obj doc) {
    return onUI()&&doc?hasStackEdit(doc,true):oldRedoEnabled(doc);
}
void discardEmptyPending(Obj doc) {
    Obj record=at<Obj>(doc,0xf0);
    if(!record||!emptyRecord(record)) return;
    // Use native ownership/destruction, then clear the damage accumulator just
    // as NewSavedUndoHandle would when transferring a record to a stack.
    fn<void(*)(Obj)>(0x502860)(doc);
    at<RectI>(doc,0x90)={};
}
void pruneEmptyStack(Obj doc,bool redo) {
    while(at<size_t>(doc,redo?0xe8:0xc0)&&emptyRecord(stackRecord(doc,redo))) {
        Obj record=fn<Obj(*)(Obj)>(redo?0x503040:0x5030a0)(doc);
        if(record) fn<Obj(*)(Obj,unsigned)>(0x502660)(record,1);
    }
}
void pushUndo(Obj doc) {
    if(onUI()&&boundsDepth) return; // Bounds observation cannot close the inverse transaction.
    if(onUI()&&doc&&at<unsigned char>(doc,0x260)) { oldPushUndo(doc);return; }
    if(onUI()&&doc&&emptyRecord(at<Obj>(doc,0xf0))) { discardEmptyPending(doc);return; }
    traceHistoryPublication(doc,false,true);
    oldPushUndo(doc);
    traceHistoryPublication(doc,false,false);
}
void pushRedo(Obj doc) {
    if(onUI()&&boundsDepth) return;
    if(onUI()&&doc&&at<unsigned char>(doc,0x260)) { oldPushRedo(doc);return; }
    if(onUI()&&doc&&emptyRecord(at<Obj>(doc,0xf0))) { discardEmptyPending(doc);return; }
    traceHistoryPublication(doc,true,true);
    oldPushRedo(doc);
    traceHistoryPublication(doc,true,false);
}
void commandBounds(Obj record,Bounds& bounds) {
    if(!record||!active) return;
    // A lazy bounds computation can touch native caches. Observe it without
    // appending housekeeping commands to the fresh inverse undo/redo record.
    BoundsReadScope reading(active->doc);
    const auto first=at<Obj*>(record,8),last=at<Obj*>(record,0x10);
    // SavedUndoRecord owns a native vector of commands. GetObject is the same
    // virtual method its native executor uses; some settings commands return
    // null. Copy bounds now, never retain pointers to deleted objects.
    for(auto entry=first;entry!=last;++entry) {
        if(!*entry) continue;
        Obj object=vf<Obj(*)(Obj)>(*entry,0x10)(*entry);
        if(!object||at<unsigned char>(object,0x30)) continue;
        Obj page=at<Obj>(object,0x60);
        if(page!=active->page) continue;
        RectD rect{};
        vf<RectD*(*)(Obj,RectD*)>(object,0x1c8)(object,&rect);
        bounds.add(rect);
    }
}
void performRecord(Obj record,Obj doc) {
    if(onUI()&&active&&active->doc==doc) commandBounds(record,active->before);
    oldPerformRecord(record,doc);
    // The native executor has consumed/deleted the old commands. Its new
    // inverse record refers to surviving/restored objects at their new bounds.
    if(onUI()&&active&&active->doc==doc) commandBounds(at<Obj>(doc,0xf0),active->after);
}
void invalWindow(Obj page,const RectD* rect) {
    if(onUI()&&active&&page==active->page&&rect) active->damage.add(*rect);
    oldInvalWindow(page,rect);
}
short undoEdit(Obj doc,Undo original,bool redo) {
    if(!onUI()||!doc||active) return original(doc);
    discardEmptyPending(doc);
    pruneEmptyStack(doc,false);pruneEmptyStack(doc,true);
    if(!(redo?redoEnabled(doc):undoEnabled(doc))) return 0;
    prepareUndoCamera(doc);
    UndoArea area{doc,mainPage(doc)};
    short result{};
    { AreaScope scope(area);result=original(doc); }
    // Reveal the restored object at its resulting position. Do not fit the
    // whole travel span of a move. Deleted artwork uses its former location.
    const auto& bounds=area.after.present?area.after:area.before;
    // Repaint damage can include unchanged neighbors or the whole page after
    // chemistry/cache invalidation. It is not an object to reveal. Edits with
    // no object target still get their fresh content publication below.
    // Content publication is required even when the changed area is already
    // visible, has no object bounds, or native history switched pages.
    queueUndoCamera(doc,mainPage(doc),bounds.rect,bounds.present&&area.page==mainPage(doc));
    return result;
}
short undo(Obj doc) {
    traceHistoryOperation(doc,false,true);
    const auto result=undoEdit(doc,oldUndo,false);
    traceHistoryOperation(doc,false,false);return result;
}
short redo(Obj doc) {
    traceHistoryOperation(doc,true,true);
    const auto result=undoEdit(doc,oldRedo,true);
    traceHistoryOperation(doc,true,false);return result;
}
}
void installNavigationUndo() {hook(0x14a710,zoomWithoutCenter,oldZoomWithoutCenter);}
void installUndoCamera() {
    hook(0x152c50,undo,oldUndo);hook(0x152ad0,redo,oldRedo);
    hook(0x461460,performRecord,oldPerformRecord);
    hook(0x715a50,invalWindow,oldInvalWindow);
    hook(0x503140,pushUndo,oldPushUndo);hook(0x503100,pushRedo,oldPushRedo);
    hook(0x5035c0,undoEnabled,oldUndoEnabled);hook(0x503180,redoEnabled,oldRedoEnabled);
}
}

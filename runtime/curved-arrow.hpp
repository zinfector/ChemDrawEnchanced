#pragma once
#include "runtime.hpp"
namespace cd {
void installCurvedArrows();
void removeArrowShortcuts();
// Extends the existing FindObject detour; never install a second hook on it.
bool smartArrowHit(Obj,const Point*,Obj,Obj&);
bool spatialCandidatesInRadius(Obj,const Point&,double,std::vector<Obj>&);
void forgetArrowPage(Obj);
void arrowMouseReleased(HWND,const MSG&);
bool cancelArrowTracking(HWND);
bool regularArrowEndpoints(Obj,Point& tail,Point& head);
bool regularArrowPreview(Obj);
bool regularArrowTracker(Obj);
bool arrowChemistryTarget(Obj,const Point&);
Obj arrowSnapTarget(Obj,const Point&);
Point arrowAttachmentPoint(Obj target,Point anchor,Point outward,double clearance);
inline bool nativeArrowTool(int tool) {return tool==1||tool==0x27;}
void maintainArrowAttachments(Obj);
bool arrowShortcut(HWND,UINT,WPARAM,LPARAM);
void writeArrowStatus(const char*);
}

#pragma once
#include "runtime.hpp"
namespace cd {
void installArrowPaths();
bool arrowPathShortcut(HWND,UINT,WPARAM,LPARAM);
bool arrowPathQueuedInput(MSG&);
void arrowPathDrawingStarted();
uint64_t arrowPathTrackingObject(Obj);
bool arrowPathTrackingPreview(Obj);
void rememberEditableArrow(Obj);
void forgetArrowPaths(Obj page);
void forgetArrowPathObject(Obj);
void clearNativeArrowEditing();
bool nativeArrowAngle(Obj,double&);
void insetArrowEndpoints(Obj,Point& tail,Point& head,double angle);
void arrowPathInvalidated(Obj);
bool cancelArrowPathTracking(HWND);
bool storedArrowAngle(Obj,double&);
bool arrowPathTransient(Obj);
bool arrowWorldEndpoints(Obj,Point& tail,Point& head);
bool standardArrowAttachments(Obj);
void rememberStandardArrow(Obj);
double arrowPageArcAngle(Obj,double);
bool setMirroredArrowEndpoints(Obj,const Point*,const Point*);
void arrowPathFlipped(Obj);
}

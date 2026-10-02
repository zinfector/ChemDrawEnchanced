#include "runtime.hpp"
#include <commctrl.h>
#include <gdiplusflat.h>
#include <intrin.h>
#include <string>

namespace cd {
namespace {
constexpr wchar_t bufferedProperty[]=L"ChemDrawLatency.ToolbarBuffer";
constexpr UINT_PTR toolbarSubclass=0x43445442;
static void (*oldRefreshToolbars)(){};
static bool (*oldToggleShowing)(Obj){};
static int (*oldToolbarProc)(Obj,UINT,WPARAM,LPARAM){};
static void (*oldPrepareToolbar)(Obj,int){};
static void (*oldGridRefresh)(Obj,bool){};
static WNDPROC oldXmlToolbarProc{},oldXmlButtonProc{};
static decltype(&Gdiplus::DllExports::GdipCreateFromHWND) oldCreateFromWindow{};
static decltype(&EndPaint) oldEndPaint{};
static decltype(&PostMessageW) oldPostMessage{};
struct RefreshScope {
    std::vector<Obj> updated;
    RefreshScope* previous{};
};
struct PaintScope {
    Obj toolbar{};HWND window{};bool completed{},finishing{};
    PaintScope* previous{};
};
static thread_local RefreshScope* refreshScope{};
static thread_local PaintScope* paintScope{};

// XMLToolbarClass is the renderer used by the application's main side/top
// bars. Its owner-draw callback opens separate HWND Graphics for background,
// icon and dropdown marker. Keep all of those writes offscreen until complete.
struct XmlButtonFrame {
    HDC dc{};HBITMAP bitmap{};HGDIOBJ previous{};int width{},height{};
    ~XmlButtonFrame() {reset();}
    void reset() {
        if(dc&&previous)SelectObject(dc,previous);
        if(bitmap)DeleteObject(bitmap);
        if(dc)DeleteDC(dc);
        dc=nullptr;bitmap=nullptr;previous=nullptr;width=height=0;
    }
    bool prepare(HDC reference,int w,int h) {
        if(dc&&width==w&&height==h)return true;
        reset();if(w<=0||h<=0||uint64_t(w)*h>1000000)return false;
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void* bits{};dc=CreateCompatibleDC(reference);
        bitmap=CreateDIBSection(reference,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!dc||!bitmap) {reset();return false;}
        previous=SelectObject(dc,bitmap);
        if(!previous||previous==HGDI_ERROR) {previous=nullptr;reset();return false;}
        width=w;height=h;
        RECT all{0,0,w,h};FillRect(dc,&all,GetSysColorBrush(COLOR_BTNFACE));
        return true;
    }
};
static std::unordered_map<HWND,std::shared_ptr<XmlButtonFrame>> xmlButtons;
struct XmlDrawScope {
    HWND button;HDC dc;XmlDrawScope* previous;
    static thread_local XmlDrawScope* current;
    XmlDrawScope(HWND w,HDC destination):button(w),dc(destination),previous(current) {current=this;}
    ~XmlDrawScope() {current=previous;}
};
thread_local XmlDrawScope* XmlDrawScope::current{};
static bool xmlButton(HWND w,HWND parent=nullptr) {
    if(!onUI()||!w||!GetWindowLongPtrW(w,GWLP_USERDATA))return false;
    if((GetWindowLongPtrW(w,GWL_STYLE)&BS_TYPEMASK)!=BS_OWNERDRAW)return false;
    const auto owner=GetParent(w);if(!owner||(parent&&owner!=parent))return false;
    wchar_t name[64]{};GetClassNameW(owner,name,int(std::size(name)));
    if(wcscmp(name,L"XMLToolbarClass"))return false;
    GetClassNameW(w,name,int(std::size(name)));return !_wcsicmp(name,L"Button");
}
static Gdiplus::GpStatus WINGDIPAPI createFromWindow(HWND w,Gdiplus::GpGraphics** graphics) {
    const auto scope=XmlDrawScope::current;
    if(onUI()&&scope&&scope->button==w)
        return Gdiplus::DllExports::GdipCreateFromHDC(scope->dc,graphics);
    return oldCreateFromWindow(w,graphics);
}
static bool historyButton(HWND w) {
    if(!xmlButton(w)||!IsWindowEnabled(w))return false;
    const auto wrapper=reinterpret_cast<Obj>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(at<uintptr_t>(wrapper,0x58)!=base+0x8e2ea0)return false;
    const auto listener=static_cast<std::byte*>(wrapper)+0x58;
    // Resolve the native command tag. XML items often have a zero cached ID;
    // the wrapper's native getter resolves it through the current command set.
    const auto command=vf<unsigned short(*)(Obj)>(listener,8)(listener);
    return command==0x30||command==0x31; // Native EditMenu Undo / Redo.
}
static LRESULT CALLBACK xmlButtonProc(HWND w,UINT m,WPARAM a,LPARAM b) {
    // The native XML owner-draw callback supplies the complete background.
    // Prevent a separate class erase from exposing an empty button first.
    if(m==WM_ERASEBKGND&&xmlButton(w))return 1;
    if(onUI()&&(m==WM_DESTROY||m==WM_NCDESTROY))xmlButtons.erase(w);
    traceHistoryButton(w,m,a,b,true);
    // Windows substitutes DBLCLK for the second rapid DOWN. The native XML
    // button clears its pressed flag on DBLCLK, so the following UP silently
    // drops that click. History buttons use the normal native press/release
    // path for every click, with one command fired on each in-bounds release.
    const UINT nativeMessage=patchEnabled(13)&&m==WM_LBUTTONDBLCLK&&historyButton(w)?WM_LBUTTONDOWN:m;
    const auto result=oldXmlButtonProc(w,nativeMessage,a,b);
    traceHistoryButton(w,m,a,b,false);
    return result;
}
static LRESULT CALLBACK xmlToolbarProc(HWND w,UINT m,WPARAM a,LPARAM b) {
    if(m!=WM_DRAWITEM||!onUI()||!b) {
        if(m==WM_COMMAND)traceHistoryToolbarCommand(w,a,b,true);
        const auto result=oldXmlToolbarProc(w,m,a,b);
        if(m==WM_COMMAND)traceHistoryToolbarCommand(w,a,b,false);
        return result;
    }
    const auto item=reinterpret_cast<DRAWITEMSTRUCT*>(b);
    if(item->CtlType!=ODT_BUTTON||!item->hDC||!xmlButton(item->hwndItem,w))
        return oldXmlToolbarProc(w,m,a,b);
    const auto rect=clientRect(item->hwndItem);std::shared_ptr<XmlButtonFrame> frame;
    try {
        auto& cached=xmlButtons[item->hwndItem];if(!cached)cached=std::make_shared<XmlButtonFrame>();
        frame=cached;
        if(!frame->prepare(item->hDC,rect.r,rect.b))return oldXmlToolbarProc(w,m,a,b);
    } catch(const std::bad_alloc&) {return oldXmlToolbarProc(w,m,a,b);}
    if(auto font=GetCurrentObject(item->hDC,OBJ_FONT))SelectObject(frame->dc,font);
    SetTextColor(frame->dc,GetTextColor(item->hDC));SetBkColor(frame->dc,GetBkColor(item->hDC));
    SetBkMode(frame->dc,GetBkMode(item->hDC));
    DRAWITEMSTRUCT buffered=*item;buffered.hDC=frame->dc;LRESULT result{};
    {
        XmlDrawScope scope(item->hwndItem,frame->dc);
        result=oldXmlToolbarProc(w,m,a,reinterpret_cast<LPARAM>(&buffered));
    }
    GdiFlush();
    if(!BitBlt(item->hDC,0,0,rect.r,rect.b,frame->dc,0,0,SRCCOPY))
        return oldXmlToolbarProc(w,m,a,b);
    static bool reported{};
    if(!reported) {
        reported=true;writeToolbarStatus("Revision 94 active: actual XML toolbar buttons suppress separate erase; native background, icon and dropdown drawing completes offscreen before one publication.\r\n");
    }
    return result;
}

static HWND toolbarWindow(Obj toolbar) {
    if(!onUI()||!toolbar)return nullptr;
    const auto w=vf<HWND(*)(Obj)>(toolbar,0x78)(toolbar);
    return w&&IsWindow(w)&&GetWindowThreadProcessId(w,nullptr)==uiThread?w:nullptr;
}
static void releaseWindowGraphics(Obj toolbar) {
    if(auto port=at<Obj>(toolbar,0xe0))fn<void(*)(Obj)>(0x625300)(port);
}
static LRESULT CALLBACK bufferProc(HWND w,UINT m,WPARAM a,LPARAM b,UINT_PTR,DWORD_PTR) {
    if(m==WM_NCDESTROY) {
        RemovePropW(w,bufferedProperty);RemoveWindowSubclass(w,bufferProc,toolbarSubclass);
    }
    return DefSubclassProc(w,m,a,b);
}
static HWND bufferToolbar(Obj toolbar) {
    const auto w=toolbarWindow(toolbar);if(!w||GetMenu(w))return nullptr;
    const auto port=at<Obj>(toolbar,0xe0);
    // NativeWindowModel at +2b8 is also used by ordinary XML toolbars. The
    // actual native window/port pair, rather than that model pointer, scopes us.
    if(!port||portWindow(port)!=w)return nullptr;
    // DrawItem clears the parent port before invalidating the icon's child
    // HWND. Exclude those child interiors from every parent background DC,
    // including native WM_ERASEBKGND and direct item/frame updates. Each child
    // still owns its normal icon, enabled/pressed-state and exposure paints.
    const auto clientStyle=GetWindowLongPtrW(w,GWL_STYLE);
    if(!(clientStyle&WS_CLIPCHILDREN)) {
        SetLastError(0);
        const auto previous=SetWindowLongPtrW(w,GWL_STYLE,clientStyle|WS_CLIPCHILDREN);
        if(previous||!GetLastError()) {
            // A retained Graphics acquired before the style change can still
            // have the old DC clip. Release it before the next native draw.
            releaseWindowGraphics(toolbar);
            static bool reported{};
            if(!reported) {
                reported=true;
                writeToolbarStatus("Revision 94 active: native toolbar parent background painting excludes child button interiors; child icon and state repainting remains native.\r\n");
            }
        }
    }
    if(GetPropW(w,bufferedProperty)==toolbar)return w;
    if(GetClassLongPtrW(w,GCL_STYLE)&(CS_OWNDC|CS_CLASSDC|CS_PARENTDC))return nullptr;
    const auto style=GetWindowLongPtrW(w,GWL_EXSTYLE);
    if(style&(WS_EX_NOREDIRECTIONBITMAP|WS_EX_LAYERED))return nullptr;
    wchar_t name[128]{};GetClassNameW(w,name,int(std::size(name)));
    if(wcsstr(name,L"HwndWrapper")||wcsstr(name,L"Chrome_")||wcsstr(name,L"Xaml"))return nullptr;
    if(!SetWindowSubclass(w,bufferProc,toolbarSubclass,0))return nullptr;
    if(!SetPropW(w,bufferedProperty,toolbar)) {
        RemoveWindowSubclass(w,bufferProc,toolbarSubclass);return nullptr;
    }
    SetLastError(0);
    if(!SetWindowLongPtrW(w,GWL_EXSTYLE,style|WS_EX_COMPOSITED)&&GetLastError()) {
        RemovePropW(w,bufferedProperty);RemoveWindowSubclass(w,bufferProc,toolbarSubclass);return nullptr;
    }
    return w;
}
static void refreshToolbars() {
    if(!onUI()) {oldRefreshToolbars();return;}
    RefreshScope scope{{},refreshScope};refreshScope=&scope;
    try {oldRefreshToolbars();refreshScope=scope.previous;}
    catch(...) {refreshScope=scope.previous;throw;}
}
static bool toggleShowing(Obj delegate) {
    const auto caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    // These two exact native RefreshToolbars call sites hide and show each
    // visible toolbar. Ordinary View/toolbar visibility commands still toggle.
    if(!onUI()||!refreshScope||(caller!=base+0x542fff&&caller!=base+0x543010))
        return oldToggleShowing(delegate);
    const auto toolbar=static_cast<std::byte*>(delegate)-0x1e0;
    const auto w=toolbarWindow(toolbar);if(!w)return oldToggleShowing(delegate);
    if(std::find(refreshScope->updated.begin(),refreshScope->updated.end(),delegate)!=refreshScope->updated.end())
        return true;
    refreshScope->updated.push_back(delegate);
    bufferToolbar(toolbar);
    // DockingDelegate +50 is ValidateContent; it runs the native enable visitor
    // without hiding the window or recalculating its docking layout.
    vf<void(*)(Obj)>(delegate,0x50)(delegate);
    InvalidateRect(w,nullptr,FALSE);
    static bool reported{};
    if(!reported) {
        reported=true;writeToolbarStatus("Revision 94 active: native toolbar refresh hide/show pair replaced by in-place content update; toolbar background excludes child button interiors and item repaint requests precede EndPaint; grid window Graphics released after refresh.\r\n");
    }
    return true;
}
static void prepareToolbar(Obj toolbar,int kind) {
    oldPrepareToolbar(toolbar,kind);bufferToolbar(toolbar);
}
static int toolbarProc(Obj toolbar,UINT m,WPARAM a,LPARAM b) {
    const auto w=bufferToolbar(toolbar);
    if(!w)return oldToolbarProc(toolbar,m,a,b);
    if(m==WM_PAINT) {
        PaintScope scope{toolbar,w,false,false,paintScope};paintScope=&scope;
        try {
            const int result=oldToolbarProc(toolbar,m,a,b);
            paintScope=scope.previous;return result;
        } catch(...) {paintScope=scope.previous;throw;}
    }
    const int result=oldToolbarProc(toolbar,m,a,b);
    if(m==WM_USER)releaseWindowGraphics(toolbar);
    return result;
}
static BOOL WINAPI endPaint(HWND w,const PAINTSTRUCT* paint) {
    auto scope=paintScope;
    if(scope&&scope->window==w&&!scope->finishing&&!scope->completed) {
        // CwWindow paints the background and posts WM_USER only AFTER EndPaint.
        // Run that item phase before ending this known toolbar's buffered paint.
        scope->finishing=true;
        try {
            oldToolbarProc(scope->toolbar,WM_USER,0,0);
            releaseWindowGraphics(scope->toolbar);scope->completed=true;
        } catch(...) {scope->finishing=false;oldEndPaint(w,paint);throw;}
        scope->finishing=false;
    }
    return oldEndPaint(w,paint);
}
static BOOL WINAPI postMessage(HWND w,UINT m,WPARAM a,LPARAM b) {
    const auto scope=paintScope;
    if(scope&&scope->window==w&&scope->completed&&m==WM_USER&&!a&&!b)return TRUE;
    return oldPostMessage(w,m,a,b);
}
static void refreshGrid(Obj toolbar,bool force) {
    const auto w=bufferToolbar(toolbar);
    oldGridRefresh(toolbar,force);
    if(w&&!paintScope)releaseWindowGraphics(toolbar);
}
template<class F> void intercept(const char* name,F replacement,F& original,const wchar_t* library=L"user32.dll") {
    const auto address=GetProcAddress(GetModuleHandleW(library),name);
    if(!address||MH_CreateHook(address,reinterpret_cast<void*>(replacement),
        reinterpret_cast<void**>(&original))!=MH_OK)
        throw std::runtime_error(std::string("Toolbar paint hook: ")+name);
}
}
void installToolbarPaint() {
    hook(0x542fb0,refreshToolbars,oldRefreshToolbars);
    hook(0x545970,toggleShowing,oldToggleShowing);
    hook(0x5adc10,toolbarProc,oldToolbarProc);
    hook(0x5b1f10,prepareToolbar,oldPrepareToolbar);
    hook(0x5a3de0,refreshGrid,oldGridRefresh);
    hook(0x5f1b90,xmlToolbarProc,oldXmlToolbarProc);
    hook(0x5efcc0,xmlButtonProc,oldXmlButtonProc);
    intercept("GdipCreateFromHWND",createFromWindow,oldCreateFromWindow,L"gdiplus.dll");
    intercept("EndPaint",endPaint,oldEndPaint);
    intercept("PostMessageW",postMessage,oldPostMessage);
    writeToolbarStatus("Revision 94 installed: actual XML toolbar owner-drawn button hook at +5f1b90; separate child erase suppressed; native background/icon/marker draws share one scoped offscreen frame; legacy toolbar handling retained.\r\n");
}
}

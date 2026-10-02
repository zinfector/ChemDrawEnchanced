#include "gpu-ui.hpp"
#include "gpu-scene-renderer.hpp"
#include <commctrl.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dcomp.h>
#include <wrl/client.h>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <string>
#include <deque>

namespace cd {
using Microsoft::WRL::ComPtr;
namespace {
constexpr UINT_PTR subclassId=0x43445549;
constexpr wchar_t documentProperty[]=L"ChemDrawLatency.DocumentGPU";
constexpr wchar_t overlayClass[]=L"ChemDrawLatency.MenuGPU";
static UINT fallbackMessage{},readyMessage{},menuRefreshMessage{};
static thread_local unsigned bypass{};
struct Bypass { Bypass(){++bypass;} ~Bypass(){--bypass;} };
struct UiState {
    HWND window{},target{};bool menu{};
    std::atomic<bool> disabled{},alive{true};
    std::atomic<bool> presented{};
    int width{},height{};bool seeded{};
};
static std::unordered_map<HWND,std::shared_ptr<UiState>> clients,menus;
struct MenuStamp {uint64_t layout{},paint{};};
static std::unordered_map<HWND,MenuStamp> menuStamps;
static std::unordered_map<HWND,bool> menuPending;
struct Capture {
    std::shared_ptr<UiState> state;
    HDC real{},dc{};int width{},height{},offsetX{},offsetY{};
    RECT dirty{};
};
struct Frame {
    std::shared_ptr<UiState> state;
    std::shared_ptr<const SceneMetafile> metafile;
    RECT dirty{};int width{},height{};bool reset{},remove{};
};
static decltype(&DrawMenuBar) oldDrawMenuBar{};
static bool (*oldMenuItem)(DRAWITEMSTRUCT*){};
static LRESULT CALLBACK uiProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
static void check(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("GPU UI operation"); }
static bool document(HWND w) {
    if(GetPropW(w,documentProperty)) return true;
    if(auto port=fn<Obj(*)(HWND)>(0x625270)(w))
        if(fn<Obj(*)(Obj)>(0x3e75c0)(port)) { SetPropW(w,documentProperty,HANDLE(1));return true; }
    return false;
}
static bool eligible(HWND w) {
    if(!w||!onUI()||bypass||!IsWindow(w)||GetWindowThreadProcessId(w,nullptr)!=uiThread) return false;
    wchar_t name[128]{};GetClassNameW(w,name,int(std::size(name)));
    // Native edit controls own their caret/IME rendering. Browser and external
    // hardware surfaces also retain their own compositor instead of stacking it.
    if(!_wcsicmp(name,overlayClass)||!_wcsicmp(name,L"Edit")||
        !_wcsnicmp(name,L"RichEdit",8)||wcsstr(name,L"Chrome_")||
        wcsstr(name,L"WebView")||wcsstr(name,L"DirectUI")||wcsstr(name,L"Xaml")) return false;
    return !document(w);
}
static std::shared_ptr<UiState> client(HWND w) {
    if(!eligible(w)) return {};
    auto& state=clients[w];
    if(!state) {
        state=std::make_shared<UiState>();state->window=state->target=w;
        SetWindowSubclass(w,uiProc,subclassId,0);
    }
    return state->disabled.load()?std::shared_ptr<UiState>{}:state;
}
struct Surface {
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDXGISwapChain1> swap;
    ComPtr<ID2D1Bitmap1> output,image;
    std::shared_ptr<const GpuScene> scene;
    int width{},height{};bool attached{};
};
class Backend {
    std::mutex mutex;std::condition_variable changed;
    std::unordered_map<std::shared_ptr<UiState>,std::deque<Frame>> pending;
    ComPtr<ID3D11Device> device;ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIFactory2> factory;ComPtr<ID2D1Factory1> d2d;
    ComPtr<ID2D1Device> d2dDevice;ComPtr<ID2D1DeviceContext> context;
    ComPtr<IDCompositionDevice> composition;
    SceneRenderer scenes;uint64_t serial{};bool ready{},failed{};
    std::unordered_map<UiState*,Surface> surfaces;
    void initialize() {
        check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            nullptr,0,D3D11_SDK_VERSION,device.GetAddressOf(),nullptr,nullptr));
        check(device.As(&dxgi));ComPtr<IDXGIAdapter> adapter;check(dxgi->GetAdapter(adapter.GetAddressOf()));
        check(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())));
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,__uuidof(ID2D1Factory1),nullptr,
            reinterpret_cast<void**>(d2d.GetAddressOf())));
        check(d2d->CreateDevice(dxgi.Get(),d2dDevice.GetAddressOf()));
        check(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,context.GetAddressOf()));
        context->SetDpi(96,96);scenes.initialize(context.Get(),d2d.Get());
        check(DCompositionCreateDevice(dxgi.Get(),__uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(composition.GetAddressOf())));
        ready=true;writeGpuUiStatus("GPU owner-drawn menu replay active; native startup and control painting remain authoritative. Theme/unsupported EMF operations use raster compatibility.\r\n");
    }
    void prepare(Surface& s,const Frame& f) {
        if(!s.target) {
            // Child HWNDs retain their own input and composition ownership.
            check(composition->CreateTargetForHwnd(f.state->target,FALSE,s.target.GetAddressOf()));
            check(composition->CreateVisual(s.visual.GetAddressOf()));
        }
        if(!s.swap||s.width!=f.width||s.height!=f.height) {
            context->SetTarget(nullptr);s.output.Reset();s.swap.Reset();
            DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=UINT(f.width);desc.Height=UINT(f.height);
            desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;
            desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;
            desc.Scaling=DXGI_SCALING_STRETCH;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            desc.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
            check(factory->CreateSwapChainForComposition(device.Get(),&desc,nullptr,s.swap.GetAddressOf()));
            ComPtr<IDXGISurface> buffer;check(s.swap->GetBuffer(0,IID_PPV_ARGS(buffer.GetAddressOf())));
            auto properties=D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET|D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(desc.Format,D2D1_ALPHA_MODE_PREMULTIPLIED),96,96);
            check(context->CreateBitmapFromDxgiSurface(buffer.Get(),&properties,s.output.GetAddressOf()));
            s.width=f.width;s.height=f.height;s.image.Reset();s.scene.reset();
        }
    }
    std::shared_ptr<const ScenePixels> raster(const Frame& f) {
        // Compatibility stays off the input thread and is limited to this UI
        // surface; it never reads back or redraws the chemical page.
        auto pixels=std::make_shared<ScenePixels>();pixels->width=f.width;pixels->height=f.height;
        pixels->bytes.resize(size_t(f.width)*size_t(f.height)*4);
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=f.width;info.bmiHeader.biHeight=-f.height;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        HDC dc=CreateCompatibleDC(nullptr);void* bits{};
        HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        if(!dc||!bitmap) { if(bitmap)DeleteObject(bitmap);if(dc)DeleteDC(dc);throw std::bad_alloc(); }
        auto previous=SelectObject(dc,bitmap);memset(bits,0,pixels->bytes.size());
        auto file=SetEnhMetaFileBits(UINT(f.metafile->bytes.size()),f.metafile->bytes.data());
        const auto b=f.metafile->bounds;RECT bounds{LONG(b.x),LONG(b.y),LONG(b.x+b.w),LONG(b.y+b.h)};
        const BOOL copied=file&&PlayEnhMetaFile(dc,file,&bounds);GdiFlush();
        if(copied) {
            memcpy(pixels->bytes.data(),bits,pixels->bytes.size());
            for(int y=std::max(0L,f.dirty.top);y<std::min(LONG(f.height),f.dirty.bottom);++y)
                for(int x=std::max(0L,f.dirty.left);x<std::min(LONG(f.width),f.dirty.right);++x)
                    pixels->bytes[(size_t(y)*f.width+x)*4+3]=255;
        }
        if(file)DeleteEnhMetaFile(file);SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);
        if(!copied) throw std::runtime_error("UI metafile compatibility");return pixels;
    }
    void draw(const Frame& f,bool publish) {
        auto& s=surfaces[f.state.get()];prepare(s,f);
        auto scene=makeGpuScene();scene->width=f.width;scene->height=f.height;scene->serial=++serial;
        if(!f.reset&&s.scene) { scenes.remember(s.scene,s.image);scene->base=s.scene; }
        auto state=std::make_shared<SceneState>();
        state->clip={{float(f.dirty.left),float(f.dirty.top),float(f.dirty.right-f.dirty.left),float(f.dirty.bottom-f.dirty.top)}};
        SceneCommand command;command.kind=SceneCommand::Metafile;command.state=state;
        command.metafile=f.metafile;command.source={0,0,1,1};command.destination=f.metafile->bounds;
        try { scene->commands=translateGpuMetafile(command,f.width,f.height,serial);s.image=scenes.render(scene); }
        catch(...) {
            auto compatible=makeGpuScene();compatible->width=f.width;compatible->height=f.height;compatible->serial=++serial;
            compatible->base=scene->base.load();command={};command.kind=SceneCommand::Image;
            auto copy=std::make_shared<SceneState>(*state);copy->copy=true;command.state=copy;
            command.pixels=raster(f);command.source=command.destination={0,0,float(f.width),float(f.height)};
            compatible->commands.push_back(std::move(command));scene=std::move(compatible);s.image=scenes.render(scene);
        }
        if(publish&&f.state->alive.load()) {
            context->SetTarget(s.output.Get());context->SetTransform(D2D1::Matrix3x2F::Identity());
            context->BeginDraw();context->Clear(D2D1::ColorF(0,0.0f));
            context->DrawBitmap(s.image.Get(),nullptr,1,D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
            check(context->EndDraw());check(s.swap->Present(0,0));check(s.visual->SetContent(s.swap.Get()));
            if(!s.attached) { check(s.target->SetRoot(s.visual.Get()));s.attached=true; }
            check(composition->Commit());
            if(!f.state->presented.exchange(true)&&f.state->menu)PostMessageW(f.state->window,readyMessage,0,0);
        }
        s.scene=scene;
        // UI scenes have no native readback consumer. Keep a GPU checkpoint,
        // rather than an unbounded chain of menu and hover repaint histories.
        scene->base.store(nullptr);scenes.remember(scene,s.image);scenes.prune();
    }
    void detach(const std::shared_ptr<UiState>& state) {
        if(auto i=surfaces.find(state.get());i!=surfaces.end()) {
            if(i->second.target) i->second.target->SetRoot(nullptr);
            surfaces.erase(i);if(composition) composition->Commit();
        }
    }
    void run() noexcept {
        const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        for(;;) {
            std::unordered_map<std::shared_ptr<UiState>,std::deque<Frame>> work;
            { std::unique_lock lock(mutex);changed.wait(lock,[&]{return !pending.empty();});
                // Fold a native menu's item paints into one visible publication.
                changed.wait_for(lock,std::chrono::milliseconds(4),[]{return false;});work.swap(pending); }
            for(auto& [state,frames]:work) {
                if(frames.back().remove||!state->alive.load()) { detach(state);continue; }
                if(state->disabled.load()) continue;
                try {
                    if(failed||FAILED(com)) throw std::runtime_error("UI device unavailable");
                    if(!ready) { try { initialize(); } catch(...) { failed=true;throw; } }
                    for(size_t n=0;n<frames.size();++n)draw(frames[n],n+1==frames.size());
                } catch(...) {
                    state->disabled.store(true);detach(state);
                    PostMessageW(state->window,fallbackMessage,0,0);
                    writeGpuUiStatus("A GPU UI surface reverted to native painting after a device or compatibility failure.\r\n");
                }
            }
        }
    }
public:
    Backend() { std::thread([this]{run();}).detach(); }
    void submit(Frame f) {
        std::lock_guard lock(mutex);
        auto& queue=pending[f.state];
        // Keep all partial updates in order. A complete replacement supersedes
        // earlier paints only when it really covers this entire UI surface.
        if(f.remove||(f.reset&&f.dirty.left==0&&f.dirty.top==0&&f.dirty.right==f.width&&f.dirty.bottom==f.height))queue.clear();
        queue.push_back(std::move(f));
        changed.notify_one();
    }
};
// Intentionally process-lifetime: no driver teardown or joins from DllMain.
static Backend* backend{};
static void finish(const std::shared_ptr<Capture>& c) {
    if(!c||!c->dc)return;
    auto file=CloseEnhMetaFile(c->dc);c->dc=nullptr;
    if(!file)return;
    try {
        ENHMETAHEADER header{};GetEnhMetaFileHeader(file,sizeof(header),&header);
        if(header.rclBounds.right>=header.rclBounds.left&&header.rclBounds.bottom>=header.rclBounds.top) {
            auto meta=std::make_shared<SceneMetafile>();const UINT count=GetEnhMetaFileBits(file,0,nullptr);
            meta->bytes.resize(count);
            if(!count||GetEnhMetaFileBits(file,count,meta->bytes.data())!=count)throw std::runtime_error("UI capture");
            meta->bounds={float(header.rclBounds.left),float(header.rclBounds.top),
                float(header.rclBounds.right-header.rclBounds.left+1),float(header.rclBounds.bottom-header.rclBounds.top+1)};
            const bool reset=!c->state->seeded||c->state->width!=c->width||c->state->height!=c->height;
            c->state->width=c->width;c->state->height=c->height;c->state->seeded=true;
            backend->submit({c->state,meta,c->dirty,c->width,c->height,reset,false});
        }
    } catch(...) { c->state->disabled.store(true);PostMessageW(c->state->window,fallbackMessage,0,0); }
    DeleteEnhMetaFile(file);
}
static std::shared_ptr<Capture> start(const std::shared_ptr<UiState>& state,HDC reference,int width,int height,
    int offsetX=0,int offsetY=0,const RECT* dirty=nullptr) {
    if(!state||!reference||width<=0||height<=0||uint64_t(width)*height>16000000) return {};
    auto c=std::make_shared<Capture>();c->state=state;c->real=reference;c->width=width;c->height=height;
    c->offsetX=offsetX;c->offsetY=offsetY;c->dirty=dirty?*dirty:RECT{0,0,width,height};
    c->dc=CreateEnhMetaFileW(reference,nullptr,nullptr,nullptr);if(!c->dc) return {};
    SetViewportOrgEx(c->dc,-offsetX,-offsetY,nullptr);
    for(auto type:{OBJ_FONT,OBJ_PEN,OBJ_BRUSH}) if(auto selected=GetCurrentObject(reference,type)) SelectObject(c->dc,selected);
    SetBkMode(c->dc,GetBkMode(reference));SetTextColor(c->dc,GetTextColor(reference));SetBkColor(c->dc,GetBkColor(reference));
    const RECT clip=dirty?*dirty:c->dirty;
    IntersectClipRect(c->dc,clip.left+offsetX,clip.top+offsetY,clip.right+offsetX,clip.bottom+offsetY);
    return c;
}
static LRESULT CALLBACK overlayProc(HWND w,UINT m,WPARAM a,LPARAM b) {
    if(m==WM_NCHITTEST) return HTTRANSPARENT;
    if(m==WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if(m==WM_ERASEBKGND) return 1;
    return DefWindowProcW(w,m,a,b);
}
static std::shared_ptr<UiState> menu(HWND w,int& x,int& y,int& width,int& height) {
    MENUBARINFO info{sizeof(info)};RECT outer{};
    if(!GetMenu(w)||!GetMenuBarInfo(w,OBJID_MENU,0,&info)||!GetWindowRect(w,&outer)||IsIconic(w)) return {};
    const auto rect=info.rcBar;x=rect.left-outer.left;y=rect.top-outer.top;
    width=rect.right-rect.left;height=rect.bottom-rect.top;if(width<=0||height<=0) return {};
    auto& state=menus[w];
    if(!state) {
        Bypass suspended;state=std::make_shared<UiState>();state->window=w;state->menu=true;
        state->target=CreateWindowExW(WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT|WS_EX_NOREDIRECTIONBITMAP,
            overlayClass,L"",WS_POPUP|WS_DISABLED,rect.left,rect.top,width,height,w,nullptr,module,nullptr);
        if(!state->target) { state.reset();return {}; }
        SetWindowSubclass(w,uiProc,subclassId,0);
    }
    if(state->disabled.load()) return {};
    RECT current{};GetWindowRect(state->target,&current);
    const bool show=state->presented.load()&&IsWindowVisible(w);
    const bool shown=(GetWindowLongPtrW(state->target,GWL_STYLE)&WS_VISIBLE)!=0;
    if(!EqualRect(&rect,&current)||show!=shown) {
        Bypass suspended;SetWindowPos(state->target,nullptr,rect.left,rect.top,width,height,
            SWP_NOACTIVATE|SWP_NOZORDER|(show!=shown?(show?SWP_SHOWWINDOW:SWP_HIDEWINDOW):0));
    }
    return state;
}
static bool drawMenuItem(DRAWITEMSTRUCT* item) {
    // Native painting always happens first, on the original DC. GPU replay is
    // additive and scoped to this known owner-drawn item, never a CLR container.
    const bool result=oldMenuItem(item);
    if(!result||!item||!onUI()||bypass)return result;
    const HWND w=WindowFromDC(item->hDC);if(!eligible(w))return result;
    std::shared_ptr<Capture> capture;
    try {
        int x{},y{},width{},height{};auto state=menu(w,x,y,width,height);
        POINT surfaceOrigin{};
        if(state) { RECT outer{};GetWindowRect(w,&outer);surfaceOrigin={outer.left+x,outer.top+y}; }
        else {
            wchar_t name[128]{};GetClassNameW(w,name,int(std::size(name)));
            if(wcscmp(name,L"#32768"))return result;
            state=client(w);if(!state)return result;
            const auto rect=clientRect(w);width=rect.r;height=rect.b;ClientToScreen(w,&surfaceOrigin);
        }
        POINT origin{};GetDCOrgEx(item->hDC,&origin);
        const int ox=surfaceOrigin.x-origin.x,oy=surfaceOrigin.y-origin.y;
        RECT dirty{item->rcItem.left-ox,item->rcItem.top-oy,item->rcItem.right-ox,item->rcItem.bottom-oy};
        capture=start(state,item->hDC,width,height,ox,oy,&dirty);if(!capture)return result;
        auto mirror=*item;mirror.hDC=capture->dc;
        { Bypass scoped;oldMenuItem(&mirror); }
        finish(capture);
    } catch(...) { if(capture&&capture->dc)finish(capture); }
    return result;
}
static bool menuStamp(HWND w,MenuStamp& result) {
    uint64_t stamp=14695981039346656037ULL;
    auto mix=[&](uint64_t n){stamp=(stamp^n)*1099511628211ULL;};
    HMENU handle=GetMenu(w);mix(uintptr_t(handle));RECT rect{};GetWindowRect(w,&rect);mix(rect.right-rect.left);mix(GetDpiForWindow(w));
    const int count=GetMenuItemCount(handle);if(count<0)return false;
    mix(count);uint64_t states=14695981039346656037ULL;
    for(int n=0;n<count;++n) {
        MENUITEMINFOW info{sizeof(info)};
        info.fMask=MIIM_STATE|MIIM_FTYPE|MIIM_STRING|MIIM_ID|MIIM_SUBMENU|MIIM_DATA|MIIM_BITMAP|MIIM_CHECKMARKS;
        if(!GetMenuItemInfoW(handle,n,TRUE,&info))return false;
        std::vector<wchar_t> text(size_t(info.cch)+1);info.dwTypeData=text.data();info.cch=UINT(text.size());
        if(!GetMenuItemInfoW(handle,n,TRUE,&info))return false;
        states=(states^info.fState)*1099511628211ULL;
        mix(info.fType);mix(info.wID);mix(uintptr_t(info.hSubMenu));mix(info.dwItemData);
        mix(uintptr_t(info.hbmpItem));mix(uintptr_t(info.hbmpChecked));mix(uintptr_t(info.hbmpUnchecked));
        for(auto p=text.data();*p;++p)mix(*p);
    }
    result={stamp,(stamp^states)*1099511628211ULL};return true;
}
static BOOL refreshMenu(HWND w) {
    MenuStamp stamp{};
    if(!menuStamp(w,stamp)) {menuStamps.erase(w);return oldDrawMenuBar(w);}
    if(auto i=menuStamps.find(w);i!=menuStamps.end()&&i->second.paint==stamp.paint)return TRUE;
    const auto result=oldDrawMenuBar(w);if(result)menuStamps[w]=stamp;return result;
}
static BOOL WINAPI drawMenuBar(HWND w) {
    // The parent port can resolve to a document. Do not use GPU surface
    // eligibility here: the File/Edit bar still needs redraw filtering.
    if(!w||!onUI()||bypass||GetWindowThreadProcessId(w,nullptr)!=uiThread||!GetMenu(w))
        return oldDrawMenuBar(w);
    if(!SetWindowSubclass(w,uiProc,subclassId,0))return oldDrawMenuBar(w);
    MenuStamp stamp{};if(!menuStamp(w,stamp))return oldDrawMenuBar(w);
    const auto i=menuStamps.find(w);
    if(i!=menuStamps.end()&&i->second.paint==stamp.paint)return TRUE;
    // Startup and menu layout changes remain synchronous. Enable/check updates
    // are published once after the current input/visitor sequence has finished.
    if(i==menuStamps.end()||i->second.layout!=stamp.layout)return refreshMenu(w);
    if(menuPending.contains(w))return TRUE;
    menuPending[w]=true;
    if(PostMessageW(w,menuRefreshMessage,0,0))return TRUE;
    menuPending.erase(w);return refreshMenu(w);
}
static void retire(std::unordered_map<HWND,std::shared_ptr<UiState>>& map,HWND w) {
    auto i=map.find(w);if(i==map.end())return;auto state=i->second;map.erase(i);
    state->alive.store(false);Frame f;f.state=state;f.remove=true;backend->submit(std::move(f));
    if(state->menu&&state->target&&IsWindow(state->target)) { Bypass suspended;DestroyWindow(state->target); }
}
static LRESULT CALLBACK uiProc(HWND w,UINT m,WPARAM a,LPARAM b,UINT_PTR,DWORD_PTR) {
    if(m==WM_NCDESTROY) {
        retire(clients,w);retire(menus,w);menuStamps.erase(w);menuPending.erase(w);RemoveWindowSubclass(w,uiProc,subclassId);
        return DefSubclassProc(w,m,a,b);
    }
    if(m==fallbackMessage) {
        if(auto i=menus.find(w);i!=menus.end()&&i->second->disabled.load())ShowWindow(i->second->target,SW_HIDE);
        if(clients.contains(w))InvalidateRect(w,nullptr,TRUE);
        { Bypass suspended;oldDrawMenuBar(w); }return 0;
    }
    if(m==menuRefreshMessage) {
        if(menuPending.erase(w)&&GetMenu(w))refreshMenu(w);
        return 0;
    }
    if(m==WM_ENTERMENULOOP||m==WM_INITMENU||m==WM_INITMENUPOPUP||
        (m==WM_NCLBUTTONDOWN&&a==HTMENU)||m==WM_SYSKEYDOWN) {
        if(menuPending.erase(w))refreshMenu(w);
    }
    if(m==readyMessage) {
        if(menus.contains(w)) { int x{},y{},width{},height{};menu(w,x,y,width,height); }
        return 0;
    }
    if(m==WM_THEMECHANGED||m==WM_SYSCOLORCHANGE||m==WM_DPICHANGED||m==WM_DISPLAYCHANGE||
        m==WM_SIZE||m==WM_INITMENU||m==WM_EXITMENULOOP||m==WM_MDISETMENU) {
        menuStamps.erase(w);if(auto i=clients.find(w);i!=clients.end())i->second->seeded=false;
        if(auto i=menus.find(w);i!=menus.end())i->second->seeded=false;
    }
    if(m==WM_WINDOWPOSCHANGED||m==WM_SHOWWINDOW||m==WM_ACTIVATE) {
        if(menus.contains(w)) { int x{},y{},width{},height{};menu(w,x,y,width,height); }
    }
    return DefSubclassProc(w,m,a,b);
}
template<class F> void intercept(const char* name,F replacement,F& original,const wchar_t* library=L"user32.dll") {
    auto address=GetProcAddress(GetModuleHandleW(library),name);
    const auto result=MH_CreateHook(address,reinterpret_cast<void*>(replacement),reinterpret_cast<void**>(&original));
    if(result!=MH_OK)throw std::runtime_error(std::string("GPU UI hook: ")+name);
}
}
void markGpuDocument(HWND w) {
    if(!w||!onUI())return;SetPropW(w,documentProperty,HANDLE(1));
    retire(clients,w);if(!GetMenu(w))RemoveWindowSubclass(w,uiProc,subclassId);
}
static RECT canvasDamage(HWND w) {
    const auto client=clientRect(w);RectI pane=client;
    if(onUI()) if(auto port=fn<Obj(*)(HWND)>(0x625270)(w))
        if(auto doc=fn<Obj(*)(Obj)>(0x3e75c0)(port)) {
            vf<RectI*(*)(Obj,RectI*)>(doc,0x100)(doc,&pane);
            pane.l-=at<int>(port,0xa8);pane.r-=at<int>(port,0xa8);
            pane.t-=at<int>(port,0xac);pane.b-=at<int>(port,0xac);
            pane=intersect(pane,client);
        }
    return winRect(pane);
}
void invalidateCanvas(HWND w) {
    if(!w)return;const auto rect=canvasDamage(w);
    if(rect.right>rect.left&&rect.bottom>rect.top)InvalidateRect(w,&rect,FALSE);
}
void redrawCanvas(HWND w) {
    if(!w)return;const auto rect=canvasDamage(w);
    if(rect.right>rect.left&&rect.bottom>rect.top)RedrawWindow(w,&rect,nullptr,RDW_INVALIDATE|RDW_UPDATENOW|RDW_NOERASE|RDW_NOCHILDREN);
}
void installGpuUi() {
    fallbackMessage=RegisterWindowMessageW(L"ChemDrawLatency.UiFallback.94");
    readyMessage=RegisterWindowMessageW(L"ChemDrawLatency.UiPresented.94");
    menuRefreshMessage=RegisterWindowMessageW(L"ChemDrawLatency.MenuRefresh.94");
    WNDCLASSW cls{};cls.lpfnWndProc=overlayProc;cls.hInstance=module;cls.lpszClassName=overlayClass;
    if(!RegisterClassW(&cls)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("GPU menu class");
    HMODULE pinned{};GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&installGpuUi),&pinned);
    backend=new Backend();
    intercept("DrawMenuBar",drawMenuBar,oldDrawMenuBar);
    hook(0x6df570,drawMenuItem,oldMenuItem);
    writeGpuUiStatus("Revision 94: actual XML toolbar button erase suppressed and native button frames published complete; native toolbar background clears exclude child button interiors; persistent document UI independent of page-cache publications and camera handoff; canvas redraw excludes child HWNDs; native toolbar repaint fix installed separately; File/Edit menu redraw filtering includes document-associated parent windows; enable/check redraws coalesced after input; startup/layout/menu activation remain synchronous; stationary GPU menu overlays retain position and visibility; native startup/control painting preserved.\r\n");
}
}

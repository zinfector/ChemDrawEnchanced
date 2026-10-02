#include "runtime.hpp"
#include "placement-runtime.hpp"
#include "curved-arrow.hpp"
#include "gpu-ui.hpp"
#include "smart-align.hpp"
#include <string>

namespace cd {
uintptr_t base{};
DWORD uiThread{};
HMODULE module{};
std::atomic<uint64_t> generation{1};
thread_local unsigned drawingDepth{};
thread_local Obj hoverDocument{};
thread_local unsigned trackingDepth{};
thread_local unsigned zoomDispatch{};
void bumpGeneration() { generation.fetch_add(1,std::memory_order_relaxed); }

static void writeStatusFile(const wchar_t* name,const char* message) {
    OutputDebugStringA(message);
    wchar_t folder[32768]{};
    const DWORD length=GetEnvironmentVariableW(L"LOCALAPPDATA",folder,DWORD(std::size(folder)));
    if(!length||length>=std::size(folder)) return;
    auto path=std::wstring(folder)+L"\\ChemDrawLatency";
    CreateDirectoryW(path.c_str(),nullptr);
    path+=L"\\";path+=name;
    HANDLE f = CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,
        CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD written{}; WriteFile(f,message,DWORD(strlen(message)),&written,nullptr); CloseHandle(f);
    }
}
void writeStatus(const char* message) { writeStatusFile(L"status.txt",message); }
void writeGpuStatus(const char* message) { writeStatusFile(L"gpu-status.txt",message); }
void writeGpuUiStatus(const char* message) { writeStatusFile(L"gpu-ui-status.txt",message); }
void writeToolbarStatus(const char* message) { writeStatusFile(L"toolbar-status.txt",message); }
void writeHistoryStatus(const char* message) { writeStatusFile(L"history-toolbar.txt",message); }
void writeArrowStatus(const char* message) { writeStatusFile(L"curved-arrow-status.txt",message); }

using WaitFn = int(*)(HWND,int);
static WaitFn oldWait{};
// These are the application's existing capture transitions, preserving its state.
static void releaseMouse(HWND w) { releaseTrackingGhost();holdTrackingPresentation();fn<void(*)(HWND)>(0x5bdf80)(w); }
static int boundedWait(HWND w, int ticks) {
    if (!onUI()) return oldWait(w,ticks);
    const auto deadline = GetTickCount64() + (uint64_t(std::max(ticks,0))*1000+59)/60;
    auto captured = [] { return *reinterpret_cast<HWND*>(base+0xb72540); };
    if (w != captured()) { cancelSmartAlignment(w);cancelArrowTracking(w);holdTrackingPresentation();flushTrackingPresentation(true);return 2; }
    for (;;) {
        flushTrackingPresentation();
        MSG msg{}, up{};
        // Match the original loop's release priority. It intentionally leaves this
        // message for the normal dispatcher after ending modal tracking.
        if (!captured()) { cancelSmartAlignment(w);cancelArrowTracking(w);holdTrackingPresentation();flushTrackingPresentation(true);return 2; }
        if (PeekMessageW(&up,nullptr,WM_LBUTTONUP,WM_LBUTTONUP,PM_NOREMOVE)) {
            smartAlignmentReleased(w,up);
            arrowMouseReleased(w,up);
            releaseTrackingGhost();flushTrackingPresentation(true);releaseMouse(w); return 2;
        }
        BOOL got = PeekMessageW(&msg,nullptr,WM_MOUSEMOVE,WM_LBUTTONUP,PM_REMOVE);
        if (!got) got = PeekMessageW(&msg,nullptr,WM_KEYDOWN,WM_KEYUP,PM_REMOVE);
        if (!got) got = PeekMessageW(&msg,nullptr,WM_SYSKEYDOWN,WM_SYSKEYUP,PM_REMOVE);
        if (got) {
            if (msg.message == WM_QUIT) { cancelSmartAlignment(w);cancelArrowTracking(w);PostQuitMessage(int(msg.wParam));flushTrackingPresentation(true); releaseMouse(w); return 2; }
            switch (msg.message) {
            case WM_MOUSEMOVE: return 5;
            case WM_LBUTTONDOWN: fn<void(*)(HWND)>(0x5be010)(w); return 1;
            case WM_LBUTTONUP: smartAlignmentReleased(w,msg);arrowMouseReleased(w,msg);flushTrackingPresentation(true);releaseMouse(w); return 2;
            case WM_KEYDOWN: case WM_SYSKEYDOWN:
                if(msg.wParam==VK_ESCAPE) {
                    const bool aligned=cancelSmartAlignment(w),arrow=cancelArrowTracking(w);
                    if(aligned||arrow) {flushTrackingPresentation(true);releaseMouse(w);return 2;}
                }
                if (!(msg.lParam & (1LL<<30)) && (msg.wParam==VK_SHIFT ||
                    msg.wParam==VK_CONTROL || msg.wParam==VK_MENU)) return 6;
                break;
            case WM_KEYUP: case WM_SYSKEYUP:
                if ((msg.lParam & (1LL<<30)) && (msg.wParam==VK_SHIFT ||
                    msg.wParam==VK_CONTROL || msg.wParam==VK_MENU)) return 6;
                break;
            }
        }
        // Do not dispatch arbitrary commands/timers into a half-built molecule.
        // Return to the tracking owner at a deadline even with no fresh messages.
        const auto now = GetTickCount64();
        if (now >= deadline) { flushTrackingPresentation(true);return 8; }
        const DWORD remaining = std::min(DWORD(std::min<uint64_t>(deadline-now,MAXDWORD-1)),trackingPresentationDelay());
        const auto result = MsgWaitForMultipleObjectsEx(0,nullptr,remaining,
            QS_MOUSEMOVE|QS_MOUSEBUTTON|QS_KEY,MWMO_INPUTAVAILABLE);
        if (result == WAIT_TIMEOUT) { flushTrackingPresentation();continue; }
        if (result == WAIT_FAILED) return 8;
        // Right-button/raw keyboard messages outside the original filter must not
        // make MWMO_INPUTAVAILABLE spin continuously. Let the modal owner run.
        if (!got && result == WAIT_OBJECT_0 &&
            !PeekMessageW(&msg,nullptr,WM_MOUSEMOVE,WM_LBUTTONUP,PM_NOREMOVE) &&
            !PeekMessageW(&msg,nullptr,WM_KEYDOWN,WM_KEYUP,PM_NOREMOVE) &&
            !PeekMessageW(&msg,nullptr,WM_SYSKEYDOWN,WM_SYSKEYUP,PM_NOREMOVE)) {
            const DWORD next=MsgWaitForMultipleObjectsEx(0,nullptr,remaining,
                QS_MOUSEMOVE|QS_MOUSEBUTTON|QS_KEY,0);
            if(next==WAIT_TIMEOUT) { flushTrackingPresentation();continue; }
            if(next==WAIT_FAILED) return 8;
        }
    }
}
}

extern "C" __declspec(dllexport) void Initialize() noexcept {
    using namespace cd;
    static bool entered{};
    if (entered) return;
    entered = true;
    base = reinterpret_cast<uintptr_t>(GetModuleHandleW(L"ChemDrawBase.dll"));
    if (!base) {
        writeStatus("Inactive: ChemDrawBase.dll not loaded.\r\n");
        writeGpuStatus("Inactive: startup did not install hooks; see status.txt.\r\n");return;
    }
    // Offsets describe one binary build. Never apply them to an updated DLL.
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->FileHeader.TimeDateStamp != 0x69f1faa8 || nt->OptionalHeader.SizeOfImage != 0xf70000) {
        writeStatus("Inactive: unsupported ChemDrawBase build.\r\n"); return;
    }
    uiThread = GetCurrentThreadId();
    try {
        if (MH_Initialize()!=MH_OK) throw std::runtime_error("MinHook initialization failed");
        loadPatchOptions(module);
        preflightDetours();
        if (!patchEnabled(0)) { writeStatus("Inactive: canvas foundation disabled.\r\n"); MH_Uninitialize(); return; }
        if (patchEnabled(1)) hook(0x5be6b0,boundedWait,oldWait);
        installSpatial(); installBuffers(); installPresentation();
        if (patchEnabled(5)) installPlacementRuntime();
        if (patchEnabled(7)) installCurvedArrows();
        if (patchEnabled(8)) installSmartAlignment();
        if (patchEnabled(9)) installUndoCamera();
        if (patchEnabled(14)) installNavigationUndo();
        if (patchEnabled(10)) installGpuUi();
        if (patchEnabled(11)) installToolbarPaint();
        if (patchEnabled(12)) installHistoryTrace();
        if (MH_EnableHook(MH_ALL_HOOKS)!=MH_OK) throw std::runtime_error("Could not enable hooks");
        char status[160]{};
        sprintf_s(status,"Active: native patch manager r95, feature mask 0x%08x. See ChemDrawLatency.ini for selected patches.\r\n",patchMask());
        writeStatus(status);writeGpuStatus(status);
    } catch (const std::exception& e) {
        removeSmartAlignment();
        removeArrowShortcuts();
        MH_DisableHook(MH_ALL_HOOKS); MH_Uninitialize();
        writeStatus((std::string("Inactive: ")+e.what()+"\r\n").c_str());
        writeGpuStatus("Inactive: startup rolled back all hooks; see status.txt for the cause.\r\n");
        writeArrowStatus("Inactive: startup rolled back smart-arrow hooks; see status.txt for the cause.\r\n");
    }
}
BOOL WINAPI DllMain(HINSTANCE h,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) cd::module=h;
    return TRUE;
}

#include "startup.hpp"
#include "curved-arrow.hpp"
#include "smart-align.hpp"
#include <bcrypt.h>
#include <thread>
#include <string>

namespace cd {
namespace {
struct ModuleHash {
    HMODULE handle{};
    char hex[65]{};
    bool valid{};
    std::atomic<bool> ready{};
};
ModuleHash hashes[2];
UINT_PTR preparationTimer{};
bool coreReady{},installing{};
bool featuresPrepared{},bannerInstalled{};
ULONGLONG preparationStarted{};
thread_local std::vector<void*>* pendingHooks{};

using SplashShow=void(*)(Obj);
SplashShow oldSplashShow{};
bool mainWindowReady() noexcept {
    if(!*reinterpret_cast<const bool*>(base+0xb73030))return false;
    const HWND parent=fn<HWND(*)()>(0x5e8790)();
    return parent&&IsWindowVisible(parent);
}
bool dismissStartupBanner() noexcept {
    // CSplashWnd is a process-lifetime native singleton. This exact vtable
    // identifies the startup bitmap, independently of About and other dialogs.
    Obj splash=reinterpret_cast<Obj>(base+0xb74900);
    if(at<uintptr_t>(splash,0)!=base+0x932be0||!mainWindowReady())return false;
    const HWND window=at<HWND>(splash,8);
    if(window&&IsWindow(window)&&GetWindowThreadProcessId(window,nullptr)==uiThread&&IsWindowVisible(window))
        // Reuse the native timer handler: it cancels 1001, hides the splash and
        // invalidates the exposed windows without destroying its ATL owner.
        SendMessageW(window,WM_TIMER,0x3e9,0);
    return true;
}
void showStartupBanner(Obj splash) {
    oldSplashShow(splash);
    if(!onUI()||at<uintptr_t>(splash,0)!=base+0x932be0)return;
    const HWND window=at<HWND>(splash,8);
    if(!window||!IsWindow(window)||!IsWindowVisible(window))return;
    if(mainWindowReady())dismissStartupBanner();
    else SetTimer(window,0x3e9,2000,nullptr);
}
void installStartupBanner() {
    if(!supportedStartupHash(reinterpret_cast<HMODULE>(base),
        "0bf203d7ddf0c700bf9d44fd156425d8df274277fdaea58fa511cfbddc51861a"))return;
    // Start the timeout after bitmap loading and layered-window publication,
    // rather than letting a WM_CREATE timer expire during initialization.
    hook(0x824db0,showStartupBanner,oldSplashShow);
    bannerInstalled=true;
}

void hashModule(ModuleHash& result) noexcept {
    SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_BELOW_NORMAL);
    wchar_t path[32768]{};
    HANDLE file=INVALID_HANDLE_VALUE;
    if(result.handle&&GetModuleFileNameW(result.handle,path,DWORD(std::size(path))))
        file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,
            OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    bool ok=file!=INVALID_HANDLE_VALUE&&BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok)ok=BCryptCreateHash(algorithm,&hash,nullptr,0,nullptr,0,0)>=0;
    std::array<BYTE,65536> bytes{};DWORD count{};
    while(ok) {
        if(!ReadFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr)){ok=false;break;}
        if(!count)break;
        ok=BCryptHashData(hash,bytes.data(),count,0)>=0;
    }
    std::array<BYTE,32> digest{};
    if(ok)ok=BCryptFinishHash(hash,digest.data(),ULONG(digest.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);
    if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    if(ok)for(size_t i=0;i<digest.size();++i)sprintf_s(result.hex+i*2,3,"%02x",unsigned(digest[i]));
    result.valid=ok;
    result.ready.store(true,std::memory_order_release);
    PostThreadMessageW(uiThread,WM_NULL,0,0);
}

void CALLBACK prepareFeatures(HWND,UINT,UINT_PTR,DWORD) noexcept {
    if(!coreReady||installing||drawingDepth||trackingDepth||GetCapture()||
        !hashes[0].ready.load(std::memory_order_acquire)||!hashes[1].ready.load(std::memory_order_acquire))return;
    if(featuresPrepared) {
        if(!bannerInstalled||dismissStartupBanner()||GetTickCount64()-preparationStarted>=30000)
            cancelStartupPreparation();
        return;
    }
    installing=true;
    std::vector<void*> targets;
    try {
        targets.reserve(128);pendingHooks=&targets;
        if (patchEnabled(7)) installCurvedArrows();
        if (patchEnabled(8)) installSmartAlignment();
        if (patchEnabled(15)) installStartupBanner();
        pendingHooks=nullptr;
        for(auto target:targets)if(MH_QueueEnableHook(target)!=MH_OK)
            throw std::runtime_error("Could not queue drawing hooks");
        if(!targets.empty()&&MH_ApplyQueued()!=MH_OK)
            throw std::runtime_error("Could not enable drawing hooks");
    } catch(const std::exception& e) {
        pendingHooks=nullptr;
        bannerInstalled=false;
        removeSmartAlignment();removeArrowShortcuts();
        // Retire only this batch. Previously enabled canvas/chemistry hooks stay active.
        for(auto target:targets)MH_QueueDisableHook(target);
        if(!targets.empty())MH_ApplyQueued();
        for(auto target:targets){MH_DisableHook(target);MH_RemoveHook(target);}
        writeArrowStatus((std::string("Inactive: deferred drawing hooks: ")+e.what()+"\r\n").c_str());
    }
    featuresPrepared=true;installing=false;
    if(!bannerInstalled||dismissStartupBanner()||GetTickCount64()-preparationStarted>=30000)
        cancelStartupPreparation();
}
}

MH_STATUS createHook(void* target,void* replacement,void** original) {
    if(pendingHooks)pendingHooks->push_back(target);
    const auto status=MH_CreateHook(target,replacement,original);
    if(status!=MH_OK&&pendingHooks)pendingHooks->pop_back();
    return status;
}
bool supportedStartupHash(HMODULE handle,const char* expected) noexcept {
    if(!handle)return false;
    for(auto& result:hashes)if(result.handle==handle)
        return result.ready.load(std::memory_order_acquire)&&result.valid&&strcmp(result.hex,expected)==0;
    return false;
}
void beginStartupPreparation() {
    // Workers never access a document or install native hooks.
    HMODULE pinned{};
    if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
        reinterpret_cast<LPCWSTR>(&beginStartupPreparation),&pinned))
        throw std::runtime_error("Could not retain startup worker module");
    preparationTimer=SetTimer(nullptr,0,50,prepareFeatures);
    if(!preparationTimer)throw std::runtime_error("Could not schedule drawing initialization");
    preparationStarted=GetTickCount64();
    hashes[0].handle=reinterpret_cast<HMODULE>(base);
    hashes[1].handle=GetModuleHandleW(L"ChemDrawUI.dll");
    for(auto& result:hashes) {
        try {std::thread(hashModule,std::ref(result)).detach();}
        catch(const std::exception&) {result.ready.store(true,std::memory_order_release);}
    }
}
void finishStartupPreparation() noexcept {coreReady=true;}
void cancelStartupPreparation() noexcept {
    coreReady=false;
    if(preparationTimer){KillTimer(nullptr,preparationTimer);preparationTimer=0;}
}
}

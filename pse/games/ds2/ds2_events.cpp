// See ds2_events.h. The DS1 layer's ds1_events.cpp on DS2's sites: same messages / events, same priorities, so a plugin
// written against the core events runs on both games. Differences from DS1, all from the game:
//   - the front end and missions load through two different calls (FrontEnd_Init / Level_Init) - one event pair each;
//   - D3D9 instead of D3D8 (other vtable slots: CreateDevice 16, Reset 16, Present 17, EndScene 42);
//   - saves take UTF-16 paths (ds2_saves.h);
//   - the window's WM_DESTROY sends PSE_MSG_EXIT here (DS1: DesertStormFix's window procedure does it).
#include "ds2_events.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "ds2_saves.h"
#include "host_api.h"
#include "loader.h"
#include "pse/pse_game_ds2.h"

namespace ds2_events {
namespace {   // (inside ds2_events: pse_tests compiles this file into one unit with ds2_saves.cpp)
constexpr int32_t kInner = PSE_DS2_LAYER_PRIORITY_INNER, kOuter = PSE_DS2_LAYER_PRIORITY_OUTER;
constexpr uint32_t kDispatchMode = 0x627158;   // u8: 0 front end, 1 mission, 2 level load, 3 film, 4 other

PSEPluginHandle g_self = 0;
bool g_installed = false;
int g_failed = 0;

void Log(PSELogLevel level, const char* fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    pse::LogLine(g_self, level, line);
}

const PSEHooksInterface* H() { return pse::HooksInterface(); }

uint32_t Peek32(uint32_t va) {
    __try {
        return *reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(va));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFF;
    }
}
int PeekMode() {
    __try {
        return *reinterpret_cast<const volatile uint8_t*>(static_cast<uintptr_t>(kDispatchMode));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// ---- frame ---------------------------------------------------------------------------------------------------------
void* g_frameNext = nullptr;
std::string g_level;              // the level loaded last ("" = none / unloaded)
bool g_frontEndPending = false;   // the front end finished loading: FRONT_END_READY on its first front-end frame

void __cdecl FrameHook() {
    if (g_frontEndPending && PeekMode() == 0) {
        g_frontEndPending = false;
        pse::Broadcast(PSE_MSG_FRONT_END_READY, g_level.c_str(), static_cast<uint32_t>(g_level.size() + 1));
    }
    pse::Frame();   // pse.tasks, then PSE_EVENT_FRAME
    reinterpret_cast<void(__cdecl*)()>(g_frameNext)();
}

// ---- render --------------------------------------------------------------------------------------------------------
void* g_renderNext = nullptr;

int __cdecl RenderHook(int a) {
    pse::RaiseEvent(PSE_EVENT_PRE_RENDER, nullptr);
    const int r = reinterpret_cast<int(__cdecl*)(int)>(g_renderNext)(a);
    pse::RaiseEvent(PSE_EVENT_POST_RENDER, nullptr);
    return r;
}

// ---- levels --------------------------------------------------------------------------------------------------------
void* g_frontEndNext = nullptr;
void* g_levelInitNext = nullptr;
void* g_unloadNext = nullptr;
void* g_destroyNext = nullptr;

void Begin(const char* name) {
    ds2_saves::LevelStart(name);   // NEW_GAME (+ revert callbacks) when a game starts from the front end
    g_level = name ? name : "";
    g_frontEndPending = false;
    pse::RaiseEvent(PSE_EVENT_LEVEL_LOAD_BEGIN, const_cast<char*>(g_level.c_str()));
}
void End() {
    pse::RaiseEvent(PSE_EVENT_LEVEL_LOAD_END, const_cast<char*>(g_level.c_str()));
    g_frontEndPending = _stricmp(g_level.c_str(), kFrontEndName) == 0;
}

// FrontEnd_Init is a thiscall (ecx = Level_LoadDispatch's object) - passed on as it came.
void __fastcall FrontEndHook(void* self, void*) {
    Begin(kFrontEndName);
    reinterpret_cast<void(__thiscall*)(void*)>(g_frontEndNext)(self);
    End();
}

int __cdecl LevelInitHook(const char* name) {
    Begin(name);
    const int r = reinterpret_cast<int(__cdecl*)(const char*)>(g_levelInitNext)(name);
    End();
    return r;
}

void RaiseUnload() {
    if (g_level.empty()) return;
    const std::string level = g_level;   // (a subscriber may start something that changes g_level)
    pse::RaiseEvent(PSE_EVENT_LEVEL_UNLOAD, const_cast<char*>(level.c_str()));
}

// Level_Unload ignores ecx, but it is called with one (mov ecx, edi): kept.
void __fastcall UnloadHook(void* self, void*) {
    RaiseUnload();
    reinterpret_cast<void(__thiscall*)(void*)>(g_unloadNext)(self);
    g_level.clear();
    g_frontEndPending = false;
}

// WM_DESTROY: the level goes, then the plugins get PSE_MSG_EXIT and are unloaded (pse.host Shutdown - once, also if
// a host plugin calls it too), then the game's own clean-up.
void __cdecl DestroyHook() {
    RaiseUnload();
    g_level.clear();
    if (const PSEHostInterface* host = pse::HostInterface()) host->Shutdown();
    reinterpret_cast<void(__cdecl*)()>(g_destroyNext)();
}

// ---- save / load ---------------------------------------------------------------------------------------------------
void* g_saveNext = nullptr;
void* g_loadNext = nullptr;

bool FileExists(const std::wstring& w) { return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES; }

int __fastcall SaveHook(void* self, void*, const wchar_t* path, int flag) {
    const bool file = path && !flag;   // flag != 0 / no path: nothing is written
    bool existed = false;
    std::string sav;
    if (file) {
        sav = ds2_saves::SaveFileFor(Utf8(path));
        existed = FileExists(std::wstring(path) + L".sav");
        ds2_saves::SaveBegin(sav);
    }
    const int r = reinterpret_cast<int(__thiscall*)(void*, const wchar_t*, int)>(g_saveNext)(self, path, flag);
    if (file) ds2_saves::SaveEnd(sav, r != 0, existed);   // every plugin's save callback -> "<name>.pse", SAVE_END
    return r;
}

int __fastcall LoadHook(void* self, void*, const wchar_t* path) {
    std::string sav;
    if (path) {
        sav = ds2_saves::LoadFileFor(Utf8(path));
        ds2_saves::LoadBegin(sav);
    }
    const int r = reinterpret_cast<int(__thiscall*)(void*, const wchar_t*)>(g_loadNext)(self, path);
    if (path) ds2_saves::LoadEnd(sav, r != 0);   // the co-save to the plugins, LOAD_END
    return r;
}

// ---- D3D9 ----------------------------------------------------------------------------------------------------------
// COM vtables are per class: one broker pointer hook on &vtable[i] catches every object of the class; a class we don't
// know yet gets its own record with its own `next` per slot (as ds1_events.cpp).
enum : int { kCreateDevice = 16, kTestCooperativeLevel = 3, kReset = 16, kPresent = 17, kEndScene = 42, kSlots = 48 };
struct ComClass {
    void** vtable = nullptr;
    void* next[kSlots] = {};
    bool hooked[kSlots] = {};
};
ComClass g_d3dClasses[4], g_devClasses[4];

void** Vtable(void* obj) { return *static_cast<void***>(obj); }

ComClass* ClassOf(ComClass (&table)[4], void* obj, bool add) {
    void** vt = Vtable(obj);
    for (ComClass& c : table)
        if (c.vtable == vt) return &c;
    if (!add) return nullptr;
    for (ComClass& c : table)
        if (!c.vtable) {
            c.vtable = vt;
            return &c;
        }
    return nullptr;
}
void* NextOf(ComClass (&table)[4], void* obj, int index) {
    const ComClass* c = ClassOf(table, obj, false);
    return c && c->next[index] ? c->next[index] : Vtable(obj)[index];
}
bool HookCom(ComClass (&table)[4], void* obj, int index, const void* fn, int32_t priority, const char* what) {
    ComClass* c = ClassOf(table, obj, true);
    if (!c) {
        Log(PSE_LOG_ERROR, "[fail] Game layer: too many D3D9 classes - %s not hooked", what);
        return false;
    }
    if (c->hooked[index]) return true;
    PSEHookId id = 0;
    const PSEResult r = H()->HookPointer(g_self, reinterpret_cast<uint32_t>(&c->vtable[index]), const_cast<void*>(fn),
                                         priority, &c->next[index], &id);
    c->hooked[index] = r == PSE_OK && c->next[index];
    if (!c->hooked[index]) Log(PSE_LOG_ERROR, "[fail] Game layer: %s hook refused (%d)", what, static_cast<int>(r));
    return c->hooked[index];
}

bool g_deviceLost = false;
constexpr HRESULT kDeviceLost = static_cast<HRESULT>(0x88760868), kDeviceNotReset = static_cast<HRESULT>(0x88760869);

HRESULT __stdcall TestCooperativeLevel(void* dev) {
    const HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*)>(NextOf(g_devClasses, dev, kTestCooperativeLevel))(dev);
    if ((hr == kDeviceLost || hr == kDeviceNotReset) && !g_deviceLost) {
        g_deviceLost = true;
        pse::Broadcast(PSE_MSG_DEVICE_LOST, dev, 0);
    } else if (hr == 0) {
        g_deviceLost = false;
    }
    return hr;
}

HRESULT __stdcall Reset(void* dev, void* pp) {
    const HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(NextOf(g_devClasses, dev, kReset))(dev, pp);
    if (SUCCEEDED(hr)) {
        g_deviceLost = false;
        pse::Broadcast(PSE_MSG_DEVICE_RESET, dev, 0);
    }
    return hr;
}

HRESULT __stdcall EndScene(void* dev) {
    pse::RaiseEvent(PSE_EVENT_END_SCENE, dev);
    return reinterpret_cast<HRESULT(__stdcall*)(void*)>(NextOf(g_devClasses, dev, kEndScene))(dev);
}

HRESULT __stdcall Present(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    pse::RaiseEvent(PSE_EVENT_PRESENT, dev);
    using Fn = HRESULT(__stdcall*)(void*, const RECT*, const RECT*, HWND, const void*);
    return reinterpret_cast<Fn>(NextOf(g_devClasses, dev, kPresent))(dev, src, dst, wnd, dirty);
}

HRESULT __stdcall CreateDevice(void* d3d, UINT adapter, uint32_t type, HWND wnd, DWORD flags, void* pp, void** out) {
    using Fn = HRESULT(__stdcall*)(void*, UINT, uint32_t, HWND, DWORD, void*, void**);
    const HRESULT hr = reinterpret_cast<Fn>(NextOf(g_d3dClasses, d3d, kCreateDevice))(d3d, adapter, type, wnd, flags, pp, out);
    if (FAILED(hr) || !out || !*out) return hr;
    void* dev = *out;
    bool ok = HookCom(g_devClasses, dev, kTestCooperativeLevel, &TestCooperativeLevel, kInner, "TestCooperativeLevel");
    ok = HookCom(g_devClasses, dev, kReset, &Reset, kOuter, "Reset") && ok;
    ok = HookCom(g_devClasses, dev, kEndScene, &EndScene, kInner, "EndScene") && ok;
    ok = HookCom(g_devClasses, dev, kPresent, &Present, kInner, "Present") && ok;
    if (!ok) Log(PSE_LOG_ERROR, "[fail] Game layer: some D3D device events are missing");
    g_deviceLost = false;
    pse::Broadcast(PSE_MSG_DEVICE_CREATED, dev, 0);
    return hr;
}

void* g_create9Next = nullptr;

void* WINAPI Direct3DCreate9Hook(UINT sdk) {
    void* d3d = reinterpret_cast<void*(WINAPI*)(UINT)>(g_create9Next)(sdk);
    if (d3d) HookCom(g_d3dClasses, d3d, kCreateDevice, &CreateDevice, kOuter, "IDirect3D9::CreateDevice");
    return d3d;
}

// ---- installation --------------------------------------------------------------------------------------------------
bool Check(PSEResult r, const char* what, uint32_t va) {
    if (r == PSE_OK) return true;
    Log(PSE_LOG_ERROR, "[fail] Game layer: %s hook at 0x%08X refused (%d) - that event is missing", what, va,
        static_cast<int>(r));
    ++g_failed;
    return false;
}
}  // namespace

std::string Utf8(const wchar_t* path) {
    if (!path || !*path) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n - 1));
    return s;
}

int Install(PSEPluginHandle self) {
    if (g_installed) return g_failed;
    g_installed = true;
    g_self = self;
    g_failed = 0;
    ds2_saves::SetOwner(self);
    const PSEHooksInterface* h = H();
    PSEHookId id = 0;
    Check(h->HookCallSite(self, kFrameCall, kFrameDispatch, reinterpret_cast<void*>(&FrameHook), kInner, &g_frameNext,
                          &id), "frame", kFrameCall);
    Check(h->HookCallSite(self, kRenderCall, kRenderFrame, reinterpret_cast<void*>(&RenderHook), kInner, &g_renderNext,
                          &id), "render", kRenderCall);
    Check(h->HookCallSite(self, kFrontEndInitCall, kFrontEndInit, reinterpret_cast<void*>(&FrontEndHook), kOuter,
                          &g_frontEndNext, &id), "front-end init", kFrontEndInitCall);
    Check(h->HookCallSite(self, kLevelInitCall, kLevelInit, reinterpret_cast<void*>(&LevelInitHook), kOuter,
                          &g_levelInitNext, &id), "level init", kLevelInitCall);
    Check(h->HookCallSite(self, kUnloadCall, kLevelUnload, reinterpret_cast<void*>(&UnloadHook), kOuter, &g_unloadNext,
                          &id), "level unload", kUnloadCall);
    Check(h->HookCallSite(self, kDestroyCall, kDestroyTarget, reinterpret_cast<void*>(&DestroyHook), kOuter,
                          &g_destroyNext, &id), "window destroyed", kDestroyCall);
    Check(h->HookFunction(self, kSaveGame, sizeof kSavePrologue, kSavePrologue, reinterpret_cast<void*>(&SaveHook),
                          kOuter, &g_saveNext, &id), "save", kSaveGame);
    Check(h->HookFunction(self, kLoadGame, sizeof kLoadPrologue, kLoadPrologue, reinterpret_cast<void*>(&LoadHook),
                          kOuter, &g_loadNext, &id), "load", kLoadGame);
    // Only when the slot holds d3d9.dll's Direct3DCreate9: another file of the 1.0 build (a no-CD exe) may lay its
    // imports out differently, and a pointer hook checks nothing else.
    HMODULE d3d9 = GetModuleHandleA("d3d9.dll");
    const uint32_t create9 = d3d9 ? reinterpret_cast<uint32_t>(GetProcAddress(d3d9, "Direct3DCreate9")) : 0;
    if (Peek32(kIatDirect3DCreate9) == 0 || Peek32(kIatDirect3DCreate9) == 0xFFFFFFFF ||
        (create9 && Peek32(kIatDirect3DCreate9) != create9)) {
        Log(PSE_LOG_ERROR, "[fail] Game layer: import slot 0x%08X isn't Direct3DCreate9 (holds 0x%08X) - no D3D events",
            kIatDirect3DCreate9, Peek32(kIatDirect3DCreate9));
        ++g_failed;
    } else {
        Check(h->HookPointer(self, kIatDirect3DCreate9, reinterpret_cast<void*>(&Direct3DCreate9Hook), kOuter,
                             &g_create9Next, &id), "Direct3DCreate9", kIatDirect3DCreate9);
    }
    Log(g_failed ? PSE_LOG_ERROR : PSE_LOG_INFO, "Game layer: %s (frame, render, level, save / load, D3D9, exit)",
        g_failed ? "some event hooks failed" : "event hooks installed");
    return g_failed;
}

void ResetForTests() {
    g_installed = false;
    g_failed = 0;
    g_level.clear();
    g_frontEndPending = false;
    g_deviceLost = false;
    for (ComClass& c : g_d3dClasses) c = ComClass{};
    for (ComClass& c : g_devClasses) c = ComClass{};
}
}  // namespace ds2_events

// See tge_events.h. The DS2 layer's ds2_events.cpp on The Great Escape's sites, with DS1's D3D8 vtable slots: same
// messages / events, same priorities, so a plugin written against the core events runs on all three games. Differences,
// all from the game:
//   - menus, films, logos and missions are all entries of one level table; non-mission entries load through
//     Screen_Load, missions through Mission_Load -> Level_Init - one event pair each;
//   - Screen_Load unloads the previous level itself, after its entry (where LEVEL_LOAD_BEGIN goes out): the previous
//     level's LEVEL_UNLOAD is raised in front of it, and the unload hook stays quiet for that load;
//   - the window's WM_DESTROY sends PSE_MSG_EXIT here (as DS2).
#include "tge_events.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "host_api.h"
#include "loader.h"
#include "pse/pse_game_tge.h"
#include "pse_game_tge.h"

namespace tge_events {
namespace {
constexpr int32_t kInner = PSE_TGE_LAYER_PRIORITY_INNER, kOuter = PSE_TGE_LAYER_PRIORITY_OUTER;
constexpr uint32_t kDispatchMode = 0x66A69C;     // u8: 0 level running, 1 load pending, 2..7 screens
constexpr uint32_t kLevelTableIndex = 0x7DDAEC;  // i32, -1 = Level_NameBuffer
constexpr uint32_t kLevelTable = 0x666B00;       // 0x28-byte entries: +4 name char*, +8 type
constexpr uint32_t kNameBuffer = 0x7DDAFC, kTypeFiveBuffer = 0x7DDC08;

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

bool Peek(uint32_t va, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(va)), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint32_t Peek32(uint32_t va) {
    uint32_t v = 0xFFFFFFFF;
    Peek(va, &v, 4);
    return v;
}
int PeekMode() {
    uint8_t m = 0;
    return Peek(kDispatchMode, &m, 1) ? m : -1;
}
std::string PeekString(uint32_t va) {
    char text[129] = {};
    uint32_t n = 0;
    while (va && n < 128 && Peek(va + n, &text[n], 1) && text[n]) ++n;
    text[n] = 0;
    return text;
}

// ---- frame ---------------------------------------------------------------------------------------------------------
void* g_frameNext = nullptr;
std::string g_level;              // the level loaded last ("" = none / unloaded)
bool g_frontEndPending = false;   // the front end finished loading: FRONT_END_READY on its first running frame
bool g_inScreenLoad = false;      // Screen_Load runs: its own Level_Unload call was announced already

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
void* g_screenNext = nullptr;
void* g_levelInitNext = nullptr;
void* g_unloadNext = nullptr;
void* g_shutdownNext = nullptr;

void RaiseUnload() {
    if (g_level.empty()) return;
    const std::string level = g_level;   // (a subscriber may start something that changes g_level)
    g_level.clear();
    pse_game_tge::SetLevel("");
    g_frontEndPending = false;
    pse::RaiseEvent(PSE_EVENT_LEVEL_UNLOAD, const_cast<char*>(level.c_str()));
}

void Begin(const std::string& name) {
    g_level = name;
    pse_game_tge::SetLevel(g_level.c_str());
    g_frontEndPending = false;
    pse::RaiseEvent(PSE_EVENT_LEVEL_LOAD_BEGIN, const_cast<char*>(g_level.c_str()));
}
void End() {
    pse::RaiseEvent(PSE_EVENT_LEVEL_LOAD_END, const_cast<char*>(g_level.c_str()));
    g_frontEndPending = _stricmp(g_level.c_str(), kFrontEndName) == 0;
}

// Screen_Load: cdecl, no arguments, no result (reads Level_TableIndex itself).
void __cdecl ScreenLoadHook() {
    const std::string name = PendingLevelName();
    RaiseUnload();
    Begin(name);
    g_inScreenLoad = true;
    reinterpret_cast<void(__cdecl*)()>(g_screenNext)();
    g_inScreenLoad = false;
    End();
}

int __cdecl LevelInitHook(const char* name) {
    Begin(name ? name : "");
    const int r = reinterpret_cast<int(__cdecl*)(const char*)>(g_levelInitNext)(name);
    End();
    return r;
}

// Level_Unload: called with the previous level still loaded (Screen_Load / Mission_Load, WM_DESTROY).
void __cdecl UnloadHook() {
    if (!g_inScreenLoad) RaiseUnload();
    reinterpret_cast<void(__cdecl*)()>(g_unloadNext)();
}

// WM_DESTROY: the level goes (Level_Unload ran in front unless the game skipped it), then the plugins get PSE_MSG_EXIT
// and are unloaded (pse.host Shutdown - once, also if a host plugin calls it too), then the game's own clean-up.
void __cdecl ShutdownHook() {
    RaiseUnload();
    if (const PSEHostInterface* host = pse::HostInterface()) host->Shutdown();
    reinterpret_cast<void(__cdecl*)()>(g_shutdownNext)();
}

// ---- D3D8 ----------------------------------------------------------------------------------------------------------
// COM vtables are per class: one broker pointer hook on &vtable[i] catches every object of the class; a class we don't
// know yet gets its own record with its own `next` per slot (as ds1_events.cpp).
enum : int { kCreateDevice = 15, kTestCooperativeLevel = 3, kReset = 14, kPresent = 15, kEndScene = 35, kSlots = 40 };
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
        Log(PSE_LOG_ERROR, "[fail] Game layer: too many D3D8 classes - %s not hooked", what);
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

void* g_create8Next = nullptr;

void* WINAPI Direct3DCreate8Hook(UINT sdk) {
    void* d3d = reinterpret_cast<void*(WINAPI*)(UINT)>(g_create8Next)(sdk);
    if (d3d) HookCom(g_d3dClasses, d3d, kCreateDevice, &CreateDevice, kOuter, "IDirect3D8::CreateDevice");
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

const char* PendingLevelName() {
    static std::string name;
    const int32_t index = static_cast<int32_t>(Peek32(kLevelTableIndex));
    if (index == -1) {
        name = PeekString(kNameBuffer);
    } else if (index >= 0 && index < 256) {
        const uint32_t entry = kLevelTable + static_cast<uint32_t>(index) * 0x28;
        name = Peek32(entry + 8) == 5 ? PeekString(kTypeFiveBuffer) : PeekString(Peek32(entry + 4));
    } else {
        name.clear();
    }
    return name.c_str();
}

int Install(PSEPluginHandle self) {
    if (g_installed) return g_failed;
    g_installed = true;
    g_self = self;
    g_failed = 0;
    const PSEHooksInterface* h = H();
    PSEHookId id = 0;
    Check(h->HookCallSite(self, kFrameCall, kFrameDispatch, reinterpret_cast<void*>(&FrameHook), kInner, &g_frameNext,
                          &id), "frame", kFrameCall);
    Check(h->HookCallSite(self, kRenderCall, kRenderFrame, reinterpret_cast<void*>(&RenderHook), kInner, &g_renderNext,
                          &id), "render", kRenderCall);
    Check(h->HookFunction(self, kScreenLoad, sizeof kScreenLoadPrologue, kScreenLoadPrologue,
                          reinterpret_cast<void*>(&ScreenLoadHook), kOuter, &g_screenNext, &id), "screen load", kScreenLoad);
    Check(h->HookFunction(self, kLevelInit, sizeof kLevelInitPrologue, kLevelInitPrologue,
                          reinterpret_cast<void*>(&LevelInitHook), kOuter, &g_levelInitNext, &id), "level init", kLevelInit);
    Check(h->HookFunction(self, kLevelUnload, sizeof kUnloadPrologue, kUnloadPrologue,
                          reinterpret_cast<void*>(&UnloadHook), kOuter, &g_unloadNext, &id), "level unload", kLevelUnload);
    Check(h->HookCallSite(self, kShutdownCall, kShutdown, reinterpret_cast<void*>(&ShutdownHook), kOuter,
                          &g_shutdownNext, &id), "window destroyed", kShutdownCall);
    // Only when the slot holds d3d8.dll's Direct3DCreate8 (a wrapper d3d8.dll in the game folder is fine: it is the one
    // the game loaded) - a pointer hook checks nothing else.
    HMODULE d3d8 = GetModuleHandleA("d3d8.dll");
    const uint32_t create8 = d3d8 ? reinterpret_cast<uint32_t>(GetProcAddress(d3d8, "Direct3DCreate8")) : 0;
    const uint32_t slot = Peek32(kIatDirect3DCreate8);
    if (slot == 0 || slot == 0xFFFFFFFF || (create8 && slot != create8)) {
        Log(PSE_LOG_ERROR, "[fail] Game layer: import slot 0x%08X isn't Direct3DCreate8 (holds 0x%08X) - no D3D events",
            kIatDirect3DCreate8, slot);
        ++g_failed;
    } else {
        Check(h->HookPointer(self, kIatDirect3DCreate8, reinterpret_cast<void*>(&Direct3DCreate8Hook), kOuter,
                             &g_create8Next, &id), "Direct3DCreate8", kIatDirect3DCreate8);
    }
    Log(g_failed ? PSE_LOG_ERROR : PSE_LOG_INFO, "Game layer: %s (frame, render, level, D3D8, exit)",
        g_failed ? "some event hooks failed" : "event hooks installed");
    return g_failed;
}
}  // namespace tge_events

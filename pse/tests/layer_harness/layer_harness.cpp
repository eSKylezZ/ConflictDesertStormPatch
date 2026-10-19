// test.layer_harness - TEST ONLY (never shipped, never deployed by the live runners' normal step): lets
// tests/live/test_layer_without_dsfix.py run the game with the DS1 game layer but WITHOUT DesertStormFix, off-screen,
// unfocused and muted, booting straight into a level, then saving, loading and reloading it - so the game layer's own
// events, messages and co-save can be checked without dsfix's dev server. SDK header + raw addresses only (the same
// patches DesertStormFix's dev background mode makes, a minimal set).
//
// Active only when HKCU\Software\DesertStormFix\Dev has PseLayerHarness = <level> (REG_SZ) AND RunInBackground = 1
// (what tools/bg.py sets): otherwise it loads and does nothing. PseLayerHarnessSave (REG_SZ) = save file path for the
// save / load step (skipped when missing). Its own log: test.layer_harness.log next to the DLL.
//
//   0x5E7050          start-up option pointer -> "-d <level>"           (dev.cpp StartLevel)
//   0x448750          launcher OK's fullscreen flag imm 1 -> 0          (windowed device)
//   0x44793D          launcher dialog proc push imm: off-screen, presses Play
//   0x411569 / 0x40B897  CreateWindowExA / SetWindowPos: off-screen, never activated; active flag 0x60EE2C = 1
//   0x411390          WM_ACTIVATE inactive branch jne -> jmp            (keeps running without focus)
//   0x4112B0          ClipCursor function -> nothing                    (never clip the PC user's mouse)
//   0x44B4E1 / 0x538363 / 0x55E08A  SetCursorPos / SetCapture -> nothing
//   0x40C055          "PLEASE PRESS ANY KEY" wait -> returns at once
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <pse/pse_api.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

PSEPluginHandle g_self = 0;
const PSEHooksInterface* g_hooks = nullptr;
const PSEMessagingInterface* g_messaging = nullptr;
char g_file[MAX_PATH] = {};
char g_level[64] = {}, g_save[MAX_PATH] = {}, g_options[96] = {};
constexpr int kOff = -8000;

void Line(const char* fmt, ...) {
    if (!g_file[0]) return;
    char text[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(text, sizeof text, fmt, a);
    va_end(a);
    FILE* f = nullptr;
    if (fopen_s(&f, g_file, "a") || !f) return;
    std::fprintf(f, "[%10lu] %s\n", GetTickCount(), text);
    std::fclose(f);
}

bool DevString(const char* name, char* out, DWORD size) {
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_SZ, nullptr, out, &size) ==
               ERROR_SUCCESS && out[0];
}
DWORD DevDword(const char* name) {
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

template <class T>
T& At(uint32_t va) { return *reinterpret_cast<T*>(static_cast<uintptr_t>(va)); }

// ---- window / focus ----
using CreateWindowFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
void* g_createWindowNext = nullptr;
HWND WINAPI CreateGameWindow(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int, int, int w, int h, HWND parent,
                             HMENU menu, HINSTANCE inst, LPVOID param) {
    if (auto noGhosts = reinterpret_cast<void(WINAPI*)()>(
            GetProcAddress(GetModuleHandleA("user32.dll"), "DisableProcessWindowsGhosting")))
        noGhosts();
    HWND wnd = reinterpret_cast<CreateWindowFn>(g_createWindowNext)(ex | WS_EX_NOACTIVATE, cls, title, style, kOff, kOff, w,
                                                                    h, parent, menu, inst, param);
    At<volatile uint32_t>(0x60EE2C) = 1;   // active: it never gets WM_ACTIVATE
    Line("window %p created off-screen", wnd);
    return wnd;
}
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
void* g_placeNext = nullptr;
BOOL WINAPI PlaceWindow(HWND wnd, HWND, int, int, int cx, int cy, UINT flags) {
    return reinterpret_cast<SetWindowPosFn>(g_placeNext)(wnd, HWND_BOTTOM, kOff, kOff, cx, cy, flags | SWP_NOACTIVATE);
}
void __cdecl NoClip() {}
BOOL WINAPI NoSetCursorPos(int, int) { return TRUE; }
HWND WINAPI NoSetCapture(HWND) { return nullptr; }
int __cdecl NoKeyWait(int) { return 0; }

void* g_dialogNext = nullptr;
INT_PTR CALLBACK LauncherProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_WINDOWPOSCHANGING) {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        pos->x = pos->y = kOff;
        pos->flags |= SWP_NOACTIVATE;
    }
    const INT_PTR r = reinterpret_cast<DLGPROC>(g_dialogNext)(dlg, msg, wp, lp);
    if (msg == WM_INITDIALOG) {
        Line("launcher: Play");
        PostMessageA(dlg, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), reinterpret_cast<LPARAM>(GetDlgItem(dlg, IDOK)));
    }
    return r;
}

DWORD WINAPI MuteThread(void*) {   // the game's own audio session muted, every second (as dev.cpp's background mode)
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        IMMDeviceEnumerator* en = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&en)))) {
            IMMDevice* dev = nullptr;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
                IAudioSessionManager* mgr = nullptr;
                if (SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager), CLSCTX_ALL, nullptr,
                                            reinterpret_cast<void**>(&mgr)))) {
                    ISimpleAudioVolume* vol = nullptr;
                    if (SUCCEEDED(mgr->GetSimpleAudioVolume(nullptr, FALSE, &vol))) {
                        vol->SetMute(TRUE, nullptr);
                        vol->Release();
                    }
                    mgr->Release();
                }
                dev->Release();
            }
            en->Release();
        }
        Sleep(1000);
    }
}

// ---- the scripted run: level up -> save -> load -> the save's level again ----
enum class Step { WaitLevel, Play, Reload, Done };
Step g_step = Step::WaitLevel;
uint32_t g_playFrames = 0, g_levelEnds = 0;
DWORD g_levelAt = 0;   // GetTickCount at the last level load end (no frame cap without DesertStormFix: count time)
char* g_loadPath = nullptr;   // (kept: the game may hold the path)

int PSE_CALL OnLevelEnd(PSEEventId, void* data, void*) {
    ++g_levelEnds;
    Line("level loaded: %s (%u)", data ? static_cast<const char*>(data) : "-", g_levelEnds);
    if (g_step == Step::WaitLevel || g_step == Step::Reload) g_playFrames = 0;
    g_levelAt = GetTickCount();
    if (g_step == Step::WaitLevel) g_step = Step::Play;
    return 0;
}

int SaveAndLoad() {
    void* saver = At<void*>(0x60EDC4);
    const uint8_t direct = At<uint8_t>(0x60EE14);   // direct level starts make the saver refuse in some states
    At<uint8_t>(0x60EE14) = 0;
    const int saved = reinterpret_cast<int(__thiscall*)(void*, const char*, int)>(0x415650)(saver, g_save, 0);
    At<uint8_t>(0x60EE14) = direct;
    Line("save returned %d", saved);
    if (!saved) return 0;
    g_loadPath = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, MAX_PATH));
    std::strcpy(g_loadPath, g_save);
    const int loaded = reinterpret_cast<int(__thiscall*)(void*, const char*)>(0x415B60)(saver, g_loadPath);
    Line("load returned %d", loaded);
    if (!loaded) return 0;
    // What the load menu does next (0x449B7C..): the campaign's level into the next-level slot, 0x409F20, a level load.
    const uint8_t* camp = At<uint8_t*>(0x60EDC0);
    const char* name = camp ? reinterpret_cast<const char*>(camp + (*reinterpret_cast<const uint32_t*>(camp + 4) == 2 ? 0x37 : 0x14)) : "";
    if (!name[0]) name = g_level;
    std::strcpy(At<char*>(0x5E7028), name);
    reinterpret_cast<void(__cdecl*)()>(0x409F20)();
    At<volatile uint32_t>(0x60EC8C) = 1;
    Line("reloading %s", name);
    return 1;
}

int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    if (At<volatile uint32_t>(0x60EC8C) != 0) return 0;
    ++g_playFrames;
    const DWORD since = GetTickCount() - g_levelAt;
    if (g_step == Step::Play && since >= 8000 && g_playFrames >= 60) {
        if (!g_save[0]) {
            g_step = Step::Done;
            Line("harness: done (no save step)");
        } else if (SaveAndLoad()) {
            g_step = Step::Reload;
        } else {
            g_step = Step::Done;
            Line("harness: done (save / load failed)");
        }
    } else if (g_step == Step::Reload && g_levelEnds >= 2 && since >= 4000 && g_playFrames >= 60) {
        g_step = Step::Done;
        Line("harness: done (%u frames since the reload)", g_playFrames);
    }
    return 0;
}

// ---- patches ----
bool Bytes(const char* name, uint32_t va, const void* original, const void* patched, uint32_t len) {
    PSEBytePatch p{va, static_cast<const uint8_t*>(original), static_cast<const uint8_t*>(patched), len, 0, name};
    const PSEResult r = g_hooks->PatchGroup(g_self, name, &p, 1);
    Line("%s: %d", name, static_cast<int>(r));
    return r == PSE_OK;
}
bool Indirect(uint32_t site, uint32_t iat, const void* fn, void** next) {
    PSEHookId id = 0;
    const PSEResult r = g_hooks->HookIndirectCall(g_self, site, iat, const_cast<void*>(fn), 0, next, &id);
    Line("indirect call 0x%08X: %d", site, static_cast<int>(r));
    return r == PSE_OK;
}

const char* const kGames[] = {"ds1", nullptr};

}  // namespace

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, "test.layer_harness", "Game layer test harness (TEST ONLY)", "PSE",
    PSE_MAKE_VERSION(1, 0, 0), 0, kGames, PSE_PLUGIN_CLIENT_ONLY | PSE_PLUGIN_USES_RAW_ADDRESSES,
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_hooks = static_cast<const PSEHooksInterface*>(pse->Query(g_self, PSE_HOOKS_INTERFACE, 1));
    g_messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    if (!g_hooks || !g_messaging) return 0;
    HMODULE me = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Line), &me);
    if (GetModuleFileNameA(me, g_file, MAX_PATH) && std::strlen(g_file) > 4) {
        std::strcpy(g_file + std::strlen(g_file) - 4, ".log");
        DeleteFileA(g_file);
    } else {
        g_file[0] = 0;
    }
    if (!DevString("PseLayerHarness", g_level, sizeof g_level) || DevDword("RunInBackground") != 1) {
        Line("not configured - doing nothing");
        return 1;
    }
    DevString("PseLayerHarnessSave", g_save, sizeof g_save);

    std::snprintf(g_options, sizeof g_options, "-d %s", g_level);
    const uint32_t oldOptions = At<uint32_t>(0x5E7050), newOptions = reinterpret_cast<uint32_t>(g_options);
    bool ok = Bytes("harness: start-up options", 0x5E7050, &oldOptions, &newOptions, 4);
    const uint32_t full = 1, windowed = 0;
    ok = Bytes("harness: windowed device", 0x448750, &full, &windowed, 4) && ok;
    const uint8_t jne = 0x75, jmp = 0xEB;
    ok = Bytes("harness: run without focus", 0x411390, &jne, &jmp, 1) && ok;
    PSEHookId id = 0;
    ok = g_hooks->HookPointer(g_self, 0x44793D, reinterpret_cast<void*>(&LauncherProc), 0, &g_dialogNext, &id) == PSE_OK &&
         ok;
    ok = Indirect(0x411569, 0x5D8248, reinterpret_cast<const void*>(&CreateGameWindow), &g_createWindowNext) && ok;
    ok = Indirect(0x40B897, 0x5D82CC, reinterpret_cast<const void*>(&PlaceWindow), &g_placeNext) && ok;
    static void* unused[4] = {};
    ok = Indirect(0x44B4E1, 0x5D8268, reinterpret_cast<const void*>(&NoSetCursorPos), &unused[0]) && ok;
    ok = Indirect(0x538363, 0x5D8214, reinterpret_cast<const void*>(&NoSetCapture), &unused[1]) && ok;
    ok = Indirect(0x55E08A, 0x5D8214, reinterpret_cast<const void*>(&NoSetCapture), &unused[2]) && ok;
    static const uint8_t kClip[] = {0xA1, 0x60, 0x6A, 0x60, 0x00};
    ok = g_hooks->HookFunction(g_self, 0x4112B0, 5, kClip, reinterpret_cast<void*>(&NoClip), 0, &unused[3], &id) ==
             PSE_OK && ok;
    static void* keyNext = nullptr;
    ok = g_hooks->HookCallSite(g_self, 0x40C055, 0x539A20, reinterpret_cast<void*>(&NoKeyWait), 0, &keyNext, &id) ==
             PSE_OK && ok;
    CloseHandle(CreateThread(nullptr, 0, &MuteThread, nullptr, 0, nullptr));
    g_messaging->Subscribe(g_self, PSE_EVENT_LEVEL_LOAD_END, &OnLevelEnd, nullptr, 0);
    g_messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
    Line("active: level %s, save %s, patches %s", g_level, g_save[0] ? g_save : "(none)", ok ? "ok" : "FAILED");
    return 1;
}

}  // extern "C"

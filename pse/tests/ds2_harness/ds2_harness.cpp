// test.ds2_harness - TEST ONLY (never shipped): lets a scripted run start Conflict: Desert Storm II with PSE but without
// DesertStorm2Fix (its dinput8.dll is replaced by the PSE shim for the run) - off-screen, unfocused, muted, windowed,
// booting straight into a level, then saving, loading and closing the window - so the DS2 game layer's events,
// messages and co-save can be checked from the plugins' log files. SDK header + raw addresses only: the same patches
// DesertStorm2Fix's dev background mode makes (games/ds2/src/features/devbg.cpp), through the broker.
//
// Active only when HKCU\Software\DesertStorm2Fix\Dev has PseHarness = <level> (REG_SZ, e.g. "Mission01.dll") AND
// Background = 1; otherwise it loads and does nothing. PseHarnessSave (REG_SZ) = save path WITHOUT extension for the
// save / load step (skipped when missing). PseHarnessPlayMs (DWORD) = play time before the save (default 8000).
// Its own log: test.ds2_harness.log next to the DLL.
//
//   0x60981C            start-up options pointer ("-fl -d") -> "-d <level>"
//   0x414BA5 / 0x414BCA message loop: run while inactive (je -> nop), no per-frame restyle (je -> jmp)
//   0x40EA99 / 0x40EAB8 CreateWindowExA / ShowWindow: off-screen, never activated
//   0x411E14 / 0x411FAD SetWindowPos: off-screen
//   0x421AA7            DialogBoxParamA (launcher): off-screen, presses Play
//   0x40B315 0x40E850 0x40E8EC 0x4126FF  ClipCursor -> nothing;  0x4CB15C SetCursorPos -> nothing
//   0x40FB50 / 0x4440ED "press any key" waits -> return at once
//   0x5DA1D4            MessageBoxA import slot -> logged + answered
//   0x5DA2C4            Direct3DCreate9 -> IDirect3D9::CreateDevice (slot 16): windowed device
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <pse/pse_api.h>
#include <pse/pse_game_ds2.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

PSEPluginHandle g_self = 0;
const PSEHooksInterface* g_hooks = nullptr;
const PSEMessagingInterface* g_messaging = nullptr;
char g_file[MAX_PATH] = {};
char g_level[64] = {}, g_options[96] = {};
wchar_t g_save[MAX_PATH] = {};
DWORD g_playMs = 8000;
constexpr int kOff = -30000;
constexpr char kDevKey[] = "Software\\DesertStorm2Fix\\Dev";

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
    return RegGetValueA(HKEY_CURRENT_USER, kDevKey, name, RRF_RT_REG_SZ, nullptr, out, &size) == ERROR_SUCCESS && out[0];
}
DWORD DevDword(const char* name, DWORD def) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, kDevKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : def;
}

template <class T>
T& At(uint32_t va) { return *reinterpret_cast<T*>(static_cast<uintptr_t>(va)); }

// ---- window / focus / launcher ----
void* g_createNext = nullptr;
void* g_showNext = nullptr;
void* g_placeNext[2] = {};
void* g_dialogNext = nullptr;
DLGPROC g_launcherProc = nullptr;
using CreateWindowFn = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
HWND WINAPI CreateGameWindow(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int, int, int w, int h, HWND parent,
                             HMENU menu, HINSTANCE inst, LPVOID param) {
    if (auto noGhosts = reinterpret_cast<void(WINAPI*)()>(
            GetProcAddress(GetModuleHandleA("user32.dll"), "DisableProcessWindowsGhosting")))
        noGhosts();
    HWND wnd = reinterpret_cast<CreateWindowFn>(g_createNext)(ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, title, style,
                                                              kOff, kOff, w, h, parent, menu, inst, param);
    Line("window %p created off-screen (%dx%d)", wnd, w, h);
    return wnd;
}
BOOL WINAPI ShowNoActivate(HWND wnd, int) { return reinterpret_cast<BOOL(WINAPI*)(HWND, int)>(g_showNext)(wnd, SW_SHOWNOACTIVATE); }
using SetWindowPosFn = BOOL(WINAPI*)(HWND, HWND, int, int, int, int, UINT);
BOOL Place(void* next, HWND wnd, int cx, int cy, UINT flags) {
    return reinterpret_cast<SetWindowPosFn>(next)(wnd, nullptr, kOff, kOff, cx, cy,
                                                  (flags | SWP_NOACTIVATE | SWP_NOZORDER) & ~SWP_SHOWWINDOW);
}
BOOL WINAPI Place0(HWND wnd, HWND, int, int, int cx, int cy, UINT flags) { return Place(g_placeNext[0], wnd, cx, cy, flags); }
BOOL WINAPI Place1(HWND wnd, HWND, int, int, int cx, int cy, UINT flags) { return Place(g_placeNext[1], wnd, cx, cy, flags); }
BOOL WINAPI NoClip(const RECT*) { return TRUE; }
BOOL WINAPI NoSetCursorPos(int, int) { return TRUE; }
void __cdecl NoKeyWait(unsigned) {}

INT_PTR CALLBACK LauncherProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_WINDOWPOSCHANGING) {
        auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
        pos->x = pos->y = kOff;
        pos->flags |= SWP_NOACTIVATE;
    }
    const INT_PTR r = g_launcherProc(dlg, msg, wp, lp);
    if (msg == WM_INITDIALOG) {
        SetWindowPos(dlg, nullptr, kOff, kOff, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        Line("launcher: Play");
        PostMessageA(dlg, WM_COMMAND, IDOK, 0);
    }
    return r;
}
using DialogBoxParamFn = INT_PTR(WINAPI*)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
INT_PTR WINAPI LauncherDialog(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
    g_launcherProc = proc;
    return reinterpret_cast<DialogBoxParamFn>(g_dialogNext)(inst, tmpl, parent, &LauncherProc, lp);
}

void* g_boxNext = nullptr;
int WINAPI AnswerBox(HWND, LPCSTR text, LPCSTR caption, UINT type) {
    Line("message box \"%s\": %s", caption ? caption : "", text ? text : "");
    switch (type & MB_TYPEMASK) {
        case MB_ABORTRETRYIGNORE: return IDIGNORE;
        case MB_YESNO:
        case MB_YESNOCANCEL: return IDYES;
        case MB_RETRYCANCEL: return IDCANCEL;
        default: return IDOK;
    }
}

// ---- D3D9: a windowed device (any size, never fullscreen) ----
void* g_create9Next = nullptr;
void** g_d3dVtable = nullptr;
void* g_createDeviceNext = nullptr;
HRESULT __stdcall CreateDevice(void* d3d, UINT adapter, uint32_t type, HWND wnd, DWORD flags, uint8_t* pp, void** out) {
    if (pp) {   // D3DPRESENT_PARAMETERS (D3D9)
        *reinterpret_cast<uint32_t*>(pp + 8) = 22;            // BackBufferFormat = D3DFMT_X8R8G8B8
        *reinterpret_cast<uint32_t*>(pp + 24) = 1;            // SwapEffect = DISCARD
        *reinterpret_cast<uint32_t*>(pp + 32) = 1;            // Windowed
        *reinterpret_cast<uint32_t*>(pp + 48) = 0;            // FullScreen_RefreshRateInHz
        *reinterpret_cast<uint32_t*>(pp + 52) = 0x80000000u;  // PresentationInterval = IMMEDIATE
    }
    using Fn = HRESULT(__stdcall*)(void*, UINT, uint32_t, HWND, DWORD, uint8_t*, void**);
    const HRESULT hr = reinterpret_cast<Fn>(g_createDeviceNext)(d3d, adapter, type, wnd, flags, pp, out);
    Line("CreateDevice (windowed): 0x%08lX", static_cast<unsigned long>(hr));
    return hr;
}
void* WINAPI Create9(UINT sdk) {
    void* d3d = reinterpret_cast<void*(WINAPI*)(UINT)>(g_create9Next)(sdk);
    if (d3d && !g_d3dVtable) {
        g_d3dVtable = *static_cast<void***>(d3d);
        PSEHookId id = 0;
        const PSEResult r = g_hooks->HookPointer(g_self, reinterpret_cast<uint32_t>(&g_d3dVtable[16]),
                                                 reinterpret_cast<void*>(&CreateDevice), 0, &g_createDeviceNext, &id);
        Line("IDirect3D9::CreateDevice hook: %d", static_cast<int>(r));
    }
    return d3d;
}

// ---- sound: this process's sessions muted (re-checked while the game opens them); unmuted again at EXIT, because
// Windows remembers a per-app mute (the user's own next game would start muted) ----
volatile bool g_stopMute = false;
void SetSessionsMuted(BOOL mute) {
    {
        IMMDeviceEnumerator* en = nullptr;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                       reinterpret_cast<void**>(&en)))) {
            IMMDeviceCollection* devs = nullptr;
            UINT ndev = 0;
            if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs)) && SUCCEEDED(devs->GetCount(&ndev))) {
                for (UINT d = 0; d < ndev; ++d) {
                    IMMDevice* dev = nullptr;
                    IAudioSessionManager2* mgr = nullptr;
                    IAudioSessionEnumerator* sessions = nullptr;
                    int n = 0;
                    if (SUCCEEDED(devs->Item(d, &dev)) &&
                        SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                                reinterpret_cast<void**>(&mgr))) &&
                        SUCCEEDED(mgr->GetSessionEnumerator(&sessions)) && SUCCEEDED(sessions->GetCount(&n))) {
                        for (int i = 0; i < n; ++i) {
                            IAudioSessionControl* ctl = nullptr;
                            IAudioSessionControl2* ctl2 = nullptr;
                            ISimpleAudioVolume* vol = nullptr;
                            DWORD pid = 0;
                            if (SUCCEEDED(sessions->GetSession(i, &ctl)) &&
                                SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&ctl2))) &&
                                SUCCEEDED(ctl2->GetProcessId(&pid)) && pid == GetCurrentProcessId() &&
                                SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&vol))))
                                vol->SetMute(mute, nullptr);
                            if (vol) vol->Release();
                            if (ctl2) ctl2->Release();
                            if (ctl) ctl->Release();
                        }
                    }
                    if (sessions) sessions->Release();
                    if (mgr) mgr->Release();
                    if (dev) dev->Release();
                }
            }
            if (devs) devs->Release();
            en->Release();
        }
    }
}
DWORD WINAPI MuteThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    while (!g_stopMute) {
        SetSessionsMuted(TRUE);
        Sleep(500);
    }
    return 0;
}

// ---- the scripted run: level up -> play -> save -> load -> close the window ----
enum class Step { WaitLevel, Play, Close, Done };
Step g_step = Step::WaitLevel;
DWORD g_levelAt = 0, g_closeAt = 0;
uint32_t g_playFrames = 0;

const PSEGameDs2V1* g_game = nullptr;   // pse.game.ds2: its values logged at each step (checked by the live test)
bool g_frontEndRun = false;              // PseHarness = "FrontEnd": normal start (logo films, menus), close at FRONT_END_READY
void LogGame(const char* when) {
    if (!g_game) return;
    char level[64] = {};
    g_game->LevelName(level, sizeof level);
    Line("game %s: level \"%s\" kind %u front-end state 0x%X dispatch %u cutscene %d views %u window %s exe known %d",
         when, level, g_game->LevelKind(), g_game->FrontEndState(), g_game->DispatchMode(), g_game->InCutscene(),
         g_game->SplitScreenPlayers(), g_game->GameWindow() ? "yes" : "no", g_game->ExeKnown());
}

int PSE_CALL OnLevelEnd(PSEEventId, void* data, void*) {
    const char* name = data ? static_cast<const char*>(data) : "-";
    Line("level loaded: %s", name);
    LogGame("at level load end");
    if (g_step == Step::WaitLevel && !g_frontEndRun && _stricmp(name, "FrontEnd") != 0) {
        g_step = Step::Play;
        g_levelAt = GetTickCount();
        g_playFrames = 0;
    }
    return 0;
}

int SaveAndLoad() {
    void* saver = At<void*>(0x62F458);
    if (!saver) return 0;
    const int saved = reinterpret_cast<int(__thiscall*)(void*, const wchar_t*, int)>(0x41C600)(saver, g_save, 0);
    Line("save returned %d", saved);
    if (!saved) return 0;
    const int loaded = reinterpret_cast<int(__thiscall*)(void*, const wchar_t*)>(0x41C7C0)(saver, g_save);
    Line("load returned %d", loaded);
    return loaded;
}

int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    ++g_playFrames;
    const DWORD now = GetTickCount();
    if (g_step == Step::Play && now - g_levelAt >= g_playMs && g_playFrames >= 60 &&
        At<volatile uint8_t>(0x627158) == (g_frontEndRun ? 0 : 1)) {   // dispatch mode: menus / mission running
        LogGame("playing");
        if (g_save[0] && !g_frontEndRun) SaveAndLoad();
        g_step = Step::Close;
        g_closeAt = now;
        Line("harness: closing the window");
        PostMessageA(At<HWND>(0x62F41C), WM_CLOSE, 0, 0);
    } else if (g_step == Step::Close && now - g_closeAt > 15000) {
        g_step = Step::Done;
        Line("harness: the window didn't close in 15 s");
    }
    return 0;
}

void PSE_CALL OnMessage(const PSEMessage* m, void*) {
    if (m->type == PSE_MSG_FRONT_END_READY) {
        Line("front end ready");
        LogGame("front end ready");
        if (g_frontEndRun && g_step == Step::WaitLevel) {
            g_step = Step::Play;
            g_levelAt = GetTickCount();
            g_playFrames = 0;
        }
    }
    if (m->type != PSE_MSG_EXIT) return;
    g_stopMute = true;
    Sleep(600);   // the mute thread's last round
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // (already initialised by the game: harmless)
    SetSessionsMuted(FALSE);
    Line("harness: done (EXIT)");
}

// ---- patches ----
bool Bytes(const char* name, uint32_t va, const void* original, const void* patched, uint32_t len) {
    PSEBytePatch p{va, static_cast<const uint8_t*>(original), static_cast<const uint8_t*>(patched), len, 0, name};
    const PSEResult r = g_hooks->PatchGroup(g_self, name, &p, 1);
    if (r != PSE_OK) Line("%s: %d", name, static_cast<int>(r));
    return r == PSE_OK;
}
bool Indirect(uint32_t site, uint32_t iat, const void* fn, void** next) {
    PSEHookId id = 0;
    const PSEResult r = g_hooks->HookIndirectCall(g_self, site, iat, const_cast<void*>(fn), 0, next, &id);
    if (r != PSE_OK) Line("indirect call 0x%08X: %d", site, static_cast<int>(r));
    return r == PSE_OK;
}

const char* const kGames[] = {"ds2", nullptr};

}  // namespace

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, "test.ds2_harness", "DS2 game layer test harness (TEST ONLY)", "PSE",
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
    if (!DevString("PseHarness", g_level, sizeof g_level) || DevDword("Background", 0) != 1) {
        Line("not configured - doing nothing");
        return 1;
    }
    char save[MAX_PATH] = {};
    if (DevString("PseHarnessSave", save, sizeof save)) MultiByteToWideChar(CP_ACP, 0, save, -1, g_save, MAX_PATH);
    g_playMs = DevDword("PseHarnessPlayMs", 8000);

    g_game = static_cast<const PSEGameDs2V1*>(pse->Query(g_self, PSE_GAME_DS2_INTERFACE, 1));
    g_frontEndRun = _stricmp(g_level, "FrontEnd") == 0;
    bool ok = true;
    if (!g_frontEndRun) {
        std::snprintf(g_options, sizeof g_options, "-d %s", g_level);
        const uint32_t oldOptions = At<uint32_t>(0x60981C), newOptions = reinterpret_cast<uint32_t>(g_options);
        const uint32_t expectOptions = 0x005DA84C;
        ok = oldOptions == expectOptions && Bytes("harness: start-up options", 0x60981C, &oldOptions, &newOptions, 4);
    }
    const uint8_t je70[] = {0x74, 0x70}, nop2[] = {0x90, 0x90}, je30[] = {0x74, 0x30}, jmp30[] = {0xEB, 0x30};
    ok = Bytes("harness: run while inactive", 0x414BA5, je70, nop2, 2) && ok;
    ok = Bytes("harness: no per-frame restyle", 0x414BCA, je30, jmp30, 2) && ok;
    ok = Indirect(0x40EA99, 0x5DA24C, reinterpret_cast<const void*>(&CreateGameWindow), &g_createNext) && ok;
    ok = Indirect(0x40EAB8, 0x5DA250, reinterpret_cast<const void*>(&ShowNoActivate), &g_showNext) && ok;
    ok = Indirect(0x411E14, 0x5DA224, reinterpret_cast<const void*>(&Place0), &g_placeNext[0]) && ok;
    ok = Indirect(0x411FAD, 0x5DA224, reinterpret_cast<const void*>(&Place1), &g_placeNext[1]) && ok;
    ok = Indirect(0x421AA7, 0x5DA1E8, reinterpret_cast<const void*>(&LauncherDialog), &g_dialogNext) && ok;
    static void* unused[8] = {};
    static constexpr uint32_t kClipSites[] = {0x40B315, 0x40E850, 0x40E8EC, 0x4126FF};
    for (int i = 0; i < 4; ++i)
        ok = Indirect(kClipSites[i], 0x5DA1C4, reinterpret_cast<const void*>(&NoClip), &unused[i]) && ok;
    ok = Indirect(0x4CB15C, 0x5DA264, reinterpret_cast<const void*>(&NoSetCursorPos), &unused[4]) && ok;
    PSEHookId id = 0;
    ok = g_hooks->HookCallSite(g_self, 0x40FB50, 0x4CCBD0, reinterpret_cast<void*>(&NoKeyWait), 0, &unused[5], &id) ==
             PSE_OK && ok;
    ok = g_hooks->HookCallSite(g_self, 0x4440ED, 0x4CCBD0, reinterpret_cast<void*>(&NoKeyWait), 0, &unused[6], &id) ==
             PSE_OK && ok;
    ok = g_hooks->HookPointer(g_self, 0x5DA1D4, reinterpret_cast<void*>(&AnswerBox), 0, &g_boxNext, &id) == PSE_OK && ok;
    ok = g_hooks->HookPointer(g_self, 0x5DA2C4, reinterpret_cast<void*>(&Create9), 0, &g_create9Next, &id) == PSE_OK && ok;
    CloseHandle(CreateThread(nullptr, 0, &MuteThread, nullptr, 0, nullptr));
    g_messaging->Subscribe(g_self, PSE_EVENT_LEVEL_LOAD_END, &OnLevelEnd, nullptr, 0);
    g_messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
    g_messaging->RegisterListener(g_self, "pse", &OnMessage, nullptr);
    Line("active: level %s, save %s, patches %s", g_level, g_save[0] ? "yes" : "(none)", ok ? "ok" : "FAILED");
    return 1;
}

}  // extern "C"

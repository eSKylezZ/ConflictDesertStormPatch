// Dev builds only: run the game in the background for scripted tests, without touching the user's screen, mouse or
// keyboard. HKCU\Software\DesertStorm2Fix\Dev:
//   Background   (DWORD 1)   window off-screen, never activated, no taskbar button, sound muted; the launcher is OK'd unseen;
//                            the game keeps running while inactive; Direct3D runs windowed; cursor clip / cursor
//                            moves are dropped (the per-frame ClipCursor would trap the user's mouse off-screen).
//   ShotDir      (REG_SZ)    folder for back-buffer screenshots (BMP), taken at Present - no need to be visible.
//   ShotEveryMs  (DWORD)     interval between screenshots (default 2000).
//   StartLevel   (REG_SZ)    boot straight into a level ("Mission01.dll"): the built-in start-up options pointer
//                            0x60981C ("-fl -d", read at 0x40B0BF) is aimed at "-d <level>" (as in DS1).
//
// Message loop FUN_00414ac0: active flag [0x62F4BC] (0 -> WaitMessage, nothing runs) - the jz at 0x414BA5 is NOPed;
// while active it forces the style 0x15800000 every frame (would maximise the window back onto the screen) - the jz at
// 0x414BCA becomes jmp. Window FUN_0040e970: CreateWindowExA 0x40EA99, ShowWindow 0x40EAB8; placement SetWindowPos
// 0x411E14 / 0x411FAD. Launcher = dialog 0x6E (proc 0x421A20) via DialogBoxParamA at 0x421AA7.
#ifndef DS_DIST
#include <windows.h>
#include <audiopolicy.h>
#include <d3d9.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/graphics.h"

namespace {
constexpr char kDevKey[] = "Software\\DesertStorm2Fix\\Dev";
constexpr int kOffX = -30000, kOffY = -30000;   // on no monitor; not -32000 (Windows' minimised position)

DWORD g_shotEvery = 2000;
char g_shotDir[MAX_PATH] = {};
char g_startOptions[300] = {};
int g_shotCount = 0;
DWORD g_lastShot = 0;

DWORD DevDword(const char* name, DWORD def) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, kDevKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v
                                                                                                                 : def;
}

bool DevString(const char* name, char* out, DWORD size) {
    return RegGetValueA(HKEY_CURRENT_USER, kDevKey, name, RRF_RT_REG_SZ, nullptr, out, &size) == ERROR_SUCCESS &&
           out[0];
}

// ---- sound: muted (this process's audio sessions on every playback device, re-checked as the game / Bink open them) --
// Windows remembers a per-app mute (Volume Mixer), so a test's mute would carry over into the user's own play: muting
// sets Dev\MutedByTest, and the next normal (non-background) start unmutes its sessions once and clears it.
void SetOwnSessionsMuted(bool mute) {
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&en))))
        return;
    IMMDeviceCollection* devs = nullptr;
    UINT ndev = 0;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs)) && SUCCEEDED(devs->GetCount(&ndev))) {
        for (UINT d = 0; d < ndev; ++d) {
            IMMDevice* dev = nullptr;
            IAudioSessionManager2* mgr = nullptr;
            IAudioSessionEnumerator* sessions = nullptr;
            int n = 0;
            if (SUCCEEDED(devs->Item(d, &dev)) &&
                SUCCEEDED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&mgr))) &&
                SUCCEEDED(mgr->GetSessionEnumerator(&sessions)) && SUCCEEDED(sessions->GetCount(&n))) {
                for (int i = 0; i < n; ++i) {
                    IAudioSessionControl* ctl = nullptr;
                    IAudioSessionControl2* ctl2 = nullptr;
                    ISimpleAudioVolume* vol = nullptr;
                    DWORD pid = 0;
                    BOOL muted = FALSE;
                    if (SUCCEEDED(sessions->GetSession(i, &ctl)) &&
                        SUCCEEDED(ctl->QueryInterface(__uuidof(IAudioSessionControl2), reinterpret_cast<void**>(&ctl2))) &&
                        SUCCEEDED(ctl2->GetProcessId(&pid)) && pid == GetCurrentProcessId() &&
                        SUCCEEDED(ctl->QueryInterface(__uuidof(ISimpleAudioVolume), reinterpret_cast<void**>(&vol))) &&
                        SUCCEEDED(vol->GetMute(&muted)) && muted != (mute ? TRUE : FALSE)) {
                        vol->SetMute(mute ? TRUE : FALSE, nullptr);
                        if (mute) {
                            DWORD one = 1;
                            RegSetKeyValueA(HKEY_CURRENT_USER, kDevKey, "MutedByTest", REG_DWORD, &one, sizeof one);
                        }
                        dslog::Write("[dev] %s (session on device %u)", mute ? "background: sound muted" : "sound unmuted after a muted test run", d);
                    }
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

DWORD WINAPI MuteThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        SetOwnSessionsMuted(true);
        Sleep(500);
    }
}

// normal start after a muted test run: unmute the game's sessions as they appear (first 30 s), then clear the marker
DWORD WINAPI UnmuteThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (int i = 0; i < 60; ++i) {
        SetOwnSessionsMuted(false);
        Sleep(500);
    }
    RegDeleteKeyValueA(HKEY_CURRENT_USER, kDevKey, "MutedByTest");
    return 0;
}

// ---- window -------------------------------------------------------------------------------------------------------
HWND WINAPI BgCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int, int, int w, int h, HWND parent,
                              HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND wnd = CreateWindowExA(ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, title, style, kOffX, kOffY, w, h, parent,
                               menu, inst, param);
    dslog::Write("[dev] background: game window %p created off-screen (%dx%d)", wnd, w, h);
    return wnd;
}

BOOL WINAPI BgShowWindow(HWND wnd, int) { return ShowWindow(wnd, SW_SHOWNOACTIVATE); }

BOOL WINAPI BgSetWindowPos(HWND wnd, HWND, int, int, int cx, int cy, UINT flags) {
    return SetWindowPos(wnd, nullptr, kOffX, kOffY, cx, cy, (flags | SWP_NOACTIVATE | SWP_NOZORDER) & ~SWP_SHOWWINDOW);
}

BOOL WINAPI BgClipCursor(const RECT*) { return TRUE; }

// "Press any key" wait FUN_004ccbd0(timeout ms, 0 = forever; cdecl): its own message loop for key-up / pad buttons
// - nothing arrives in a background run. Called after a level load (0x40FB50) and at 0x4440ED.
void __cdecl BgAnyKeyWait(unsigned) {}

// Every game message box (22 call sites + the error helpers) would pop up on the user's screen and stop the game:
// logged and answered instead (Ignore for Abort/Retry/Ignore, else OK / Yes).
int WINAPI BgMessageBoxA(HWND, LPCSTR text, LPCSTR caption, UINT type) {
    dslog::Write("[dev] background: message box \"%s\": %s", caption ? caption : "", text ? text : "");
    switch (type & MB_TYPEMASK) {
        case MB_ABORTRETRYIGNORE: return IDIGNORE;
        case MB_YESNO:
        case MB_YESNOCANCEL: return IDYES;
        case MB_RETRYCANCEL: return IDCANCEL;
        default: return IDOK;
    }
}
BOOL WINAPI BgSetCursorPos(int, int) { return TRUE; }

// ---- launcher: OK'd without being seen --------------------------------------------------------------------------------
// Two dialogs: the launcher 0x6E (Play = IDOK, Settings = button 0x405, opened at 0x421AA7) and Settings 0x6F (opened
// at 0x421963 by the 0x405 handler; resolution combo 1020). Play (FUN_00420ac0) only re-reads ResolutionIndex from
// the registry, so a resolution is picked in Settings, whose OK saves it (into the user's VirtualStore copy, which
// tools/bg_run.py saves and restores) - then Play is pressed.
DLGPROC g_launcherProc = nullptr, g_settingsProc = nullptr;
HWND g_launcher = nullptr;
char g_resolution[32] = {};   // Dev\Resolution "WxH"
int g_forceW = 0, g_forceH = 0;   // Dev\ForceSize "WxH"

void OffScreen(HWND dlg) { SetWindowPos(dlg, nullptr, kOffX, kOffY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); }

INT_PTR CALLBACK BgLauncherProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR r = g_launcherProc(dlg, msg, wp, lp);
    if (msg == WM_INITDIALOG) {
        OffScreen(dlg);
        g_launcher = dlg;
        PostMessageA(dlg, WM_COMMAND, g_resolution[0] ? 0x405 : IDOK, 0);   // Settings first when a resolution is wanted
    }
    return r;
}

INT_PTR CALLBACK BgSettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    INT_PTR r = g_settingsProc(dlg, msg, wp, lp);
    if (msg == WM_INITDIALOG) {
        OffScreen(dlg);
        HWND combo = GetDlgItem(dlg, 1020);
        LRESULT i = SendMessageA(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(g_resolution));
        if (i != CB_ERR) {
            SendMessageA(combo, CB_SETCURSEL, i, 0);
            SendMessageA(dlg, WM_COMMAND, MAKEWPARAM(1020, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
            // Settings works on the launcher's object [0x62FDFC] (its dialog parameter; Settings' WM_INITDIALOG builds the
            // lists with it, FUN_00420cb0): +4 device table, +8 adapter, +0xC device, +0x10 selected mode = item data
            const DWORD mode = static_cast<DWORD>(SendMessageA(combo, CB_GETITEMDATA, i, 0));
            auto obj = *reinterpret_cast<uint8_t**>(0x62FDFC);
            if (obj) *reinterpret_cast<DWORD*>(obj + 0x10) = mode;
            if (obj && g_forceW && g_forceH) {
                // Dev\ForceSize: the picked entry of the game's mode table becomes WxH (a windowed device can be any
                // size - 4K / 21:9 without such a monitor). Entry: [obj+4] + adapter*0x68BC + device*0x1410
                // + 0x558 + 0x144 + mode*0x20 = {width, height, ...}; the device is created from it (FUN_00420ac0).
                auto base = *reinterpret_cast<uint8_t**>(obj + 4);
                const int adapter = *reinterpret_cast<int*>(obj + 8), device = *reinterpret_cast<int*>(obj + 0xC);
                auto e = reinterpret_cast<int*>(base + adapter * 0x68BC + device * 0x1410 + 0x558 + 0x144 + mode * 0x20);
                dslog::Write("[dev] background: mode %lu %dx%d forced to %dx%d", static_cast<unsigned long>(mode), e[0],
                             e[1], g_forceW, g_forceH);
                e[0] = g_forceW;
                e[1] = g_forceH;
            }
        }
        dslog::Write("[dev] background: Settings resolution %s %s (%ld items)", g_resolution,
                     i != CB_ERR ? "picked" : "NOT LISTED", static_cast<long>(SendMessageA(combo, CB_GETCOUNT, 0, 0)));
        PostMessageA(dlg, WM_COMMAND, IDOK, 0);
    }
    return r;
}

INT_PTR WINAPI BgDialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
    g_launcherProc = proc;
    return DialogBoxParamA(inst, tmpl, parent, BgLauncherProc, lp);
}

INT_PTR WINAPI BgSettingsDialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
    g_settingsProc = proc;
    INT_PTR r = DialogBoxParamA(inst, tmpl, parent, BgSettingsProc, lp);
    if (g_launcher) PostMessageA(g_launcher, WM_COMMAND, IDOK, 0);   // then Play
    return r;
}

// ---- Direct3D (graphics.cpp callbacks): windowed device, screenshots at Present ------------------------------------
void Windowed(D3DPRESENT_PARAMETERS* pp) {
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
}

void SaveShot(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb))) return;
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(bb, sys))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            char path[MAX_PATH + 32];
            std::snprintf(path, sizeof path, "%s\\shot_%04d.bmp", g_shotDir, g_shotCount++);
            if (FILE* f = std::fopen(path, "wb")) {
                const DWORD row = d.Width * 3, pad = (4 - row % 4) % 4, size = (row + pad) * d.Height;
                BITMAPFILEHEADER fh = {0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + size),
                                       0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
                BITMAPINFOHEADER ih = {sizeof ih, static_cast<LONG>(d.Width), static_cast<LONG>(d.Height), 1, 24};
                std::fwrite(&fh, sizeof fh, 1, f);
                std::fwrite(&ih, sizeof ih, 1, f);
                static unsigned char line[4 * 8192];
                for (int y = static_cast<int>(d.Height) - 1; y >= 0; --y) {   // BMP rows bottom-up
                    const auto* src = static_cast<const unsigned char*>(lr.pBits) + y * lr.Pitch;
                    for (DWORD x = 0; x < d.Width && x < 8192; ++x) std::memcpy(line + x * 3, src + x * 4, 3);
                    std::memset(line + row, 0, pad);
                    std::fwrite(line, 1, row + pad, f);
                }
                std::fclose(f);
            }
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    bb->Release();
}

void OnPresent(IDirect3DDevice9* dev) {
    if (!g_shotDir[0]) return;
    const DWORD now = GetTickCount();
    if (now - g_lastShot >= g_shotEvery) {
        g_lastShot = now;
        SaveShot(dev);
    }
}
constexpr uint8_t kActiveOrig[] = {0x74, 0x70}, kActiveNew[] = {0x90, 0x90};   // 0x414BA5 jz (inactive) -> nops
constexpr uint8_t kStyleOrig[] = {0x74, 0x30}, kStyleNew[] = {0xEB, 0x30};     // 0x414BCA jz -> jmp (no restyle)
constexpr BytePatch kBackground[] = {
    {DS_PATCH_NAME("Background: keep running while inactive"), 0x414BA5, kActiveOrig, kActiveNew, 2, false},
    {DS_PATCH_NAME("Background: no per-frame window restyle"), 0x414BCA, kStyleOrig, kStyleNew, 2, false},
};
}  // namespace

// dev: the first access violation (or other fatal exception) is logged with its address and the top of the stack
LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* e) {
    static bool logged = false;
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (logged || (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
                   code != EXCEPTION_STACK_OVERFLOW))
        return EXCEPTION_CONTINUE_SEARCH;
    logged = true;
    const CONTEXT* c = e->ContextRecord;
    const auto* sp = reinterpret_cast<const DWORD*>(c->Esp);
    dslog::Write("[dev] EXCEPTION 0x%08lX at %p (access %p) eax=%08lX ecx=%08lX edx=%08lX esi=%08lX edi=%08lX "
                 "stack %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                 code, e->ExceptionRecord->ExceptionAddress,
                 reinterpret_cast<void*>(e->ExceptionRecord->NumberParameters > 1
                                             ? e->ExceptionRecord->ExceptionInformation[1] : 0),
                 c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi, sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]);
    return EXCEPTION_CONTINUE_SEARCH;
}

void features::ApplyDevBackground() {
    AddVectoredExceptionHandler(0, &CrashLogger);
    if (DevString("StartLevel", g_startOptions + 3, sizeof g_startOptions - 3)) {
        std::memcpy(g_startOptions, "-d ", 3);
        const char* opts = g_startOptions;
        if (patch::Matches(0x60981C, "\x4C\xA8\x5D\x00", 4) && patch::WriteValue(0x60981C, opts))
            dslog::Write("[dev] StartLevel: start-up options \"%s\"", g_startOptions);
    }
    if (!DevDword("Background", 0)) {
        if (DevDword("MutedByTest", 0))
            if (HANDLE t = CreateThread(nullptr, 0, UnmuteThread, nullptr, 0, nullptr)) CloseHandle(t);
        return;
    }
    g_shotEvery = DevDword("ShotEveryMs", 2000);
    DevString("Resolution", g_resolution, sizeof g_resolution);
    char force[32] = {};
    if (DevString("ForceSize", force, sizeof force) && std::sscanf(force, "%dx%d", &g_forceW, &g_forceH) == 2 &&
        !g_resolution[0])
        std::strcpy(g_resolution, "800x600");   // any listed mode to overwrite
    if (DevString("ShotDir", g_shotDir, sizeof g_shotDir)) CreateDirectoryA(g_shotDir, nullptr);
    if (!patch::ApplyGroup("Background", kBackground)) return;
    if (HANDLE t = CreateThread(nullptr, 0, MuteThread, nullptr, 0, nullptr)) CloseHandle(t);
    patch::HookIndirectCall(0x40EA99, reinterpret_cast<void*>(&BgCreateWindowExA), 0x5DA24C);
    patch::HookIndirectCall(0x40EAB8, reinterpret_cast<void*>(&BgShowWindow), 0x5DA250);
    patch::HookIndirectCall(0x411E14, reinterpret_cast<void*>(&BgSetWindowPos), 0x5DA224);
    patch::HookIndirectCall(0x411FAD, reinterpret_cast<void*>(&BgSetWindowPos), 0x5DA224);
    patch::HookIndirectCall(0x421AA7, reinterpret_cast<void*>(&BgDialogBoxParamA), 0x5DA1E8);
    patch::HookIndirectCall(0x421963, reinterpret_cast<void*>(&BgSettingsDialogBoxParamA), 0x5DA1E8);
    static constexpr uint32_t kClipSites[] = {0x40B315, 0x40E850, 0x40E8EC, 0x4126FF};
    for (uint32_t site : kClipSites) patch::HookIndirectCall(site, reinterpret_cast<void*>(&BgClipCursor), 0x5DA1C4);
    patch::HookIndirectCall(0x4CB15C, reinterpret_cast<void*>(&BgSetCursorPos), 0x5DA264);
    patch::HookCall(0x40FB50, reinterpret_cast<void*>(&BgAnyKeyWait), 0x4CCBD0);
    patch::HookCall(0x4440ED, reinterpret_cast<void*>(&BgAnyKeyWait), 0x4CCBD0);
#ifdef DS2_PSE_HOST
    patch::HookSlot(0x5DA1D4, reinterpret_cast<const void*>(&BgMessageBoxA));   // MessageBoxA import slot (broker)
#else
    auto box = &BgMessageBoxA;
    patch::WriteValue(0x5DA1D4, box);   // MessageBoxA import slot
#endif
    graphics::g_adjustPresentParams = &Windowed;   // the device is created windowed (any size, never fullscreen)
    graphics::g_onPresent = &OnPresent;
    dslog::Write("[dev] background mode on (shots: %s every %lu ms)", g_shotDir[0] ? g_shotDir : "off",
                 static_cast<unsigned long>(g_shotEvery));
}
#else
#include "features/features.h"
void features::ApplyDevBackground() {}
#endif

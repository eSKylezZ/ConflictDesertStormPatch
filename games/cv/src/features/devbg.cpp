// Dev builds only: run the game in the background for scripted tests, without touching the user's screen, mouse or
// keyboard. HKCU\Software\VietnamFix\Dev:
//   Background   (DWORD 1)   window off-screen, never activated, no taskbar button, sound muted; the launcher is OK'd unseen;
//                            the game keeps running while inactive; Direct3D runs windowed; cursor clip / cursor
//                            moves are dropped (the per-frame ClipCursor would trap the user's mouse off-screen).
//   ShotDir      (REG_SZ)    folder for back-buffer screenshots (BMP), taken at Present - no need to be visible.
//   ShotEveryMs  (DWORD)     interval between screenshots (default 2000).
//   QuitAfterMs  (DWORD)     background runs end themselves after this long (user's ResolutionIndex put back first).
//   StartLevel   (REG_SZ)    boot straight into a level ("Mission01.dll"): selected by name (FUN_00436fd8 on the mission
//                            manager 0x70DB68, table [0x70DB58] of MissionDetails.txt entries) once the front end loaded.
//
// Message loop FUN_00431365: active flag [0x70D718] (0 -> WaitMessage, nothing runs) - the jz at 0x431425 is NOPed;
// while the device is fine it forces the style 0x15800000 every frame (would maximise the window back onto the screen) -
// the jz at 0x43144A becomes jmp. Window FUN_0042ee17: CreateWindowExA 0x42EF08, ShowWindow 0x42EF26; placement
// SetWindowPos 0x42FA27 / 0x42FB8A, SetWindowPlacement 0x42FB50, SetActiveWindow 0x42FB93; per-frame ClipCursor
// FUN_00429aad (0x429ADC, release 0x429AE9). Launcher = dialog 0x6E (proc 0x41BB54) via DialogBoxParamA at 0x41BB9D;
// Settings = 0x6F (proc 0x41BA94) at 0x41BACF; launcher object [0x703664].
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
constexpr char kDevKey[] = "Software\\VietnamFix\\Dev";
constexpr int kOffX = -30000, kOffY = -30000;   // on no monitor; not -32000 (Windows' minimised position)

DWORD g_shotEvery = 2000;
char g_shotDir[MAX_PATH] = {};
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
BOOL WINAPI BgSetWindowPlacement(HWND, const WINDOWPLACEMENT*) { return TRUE; }
HWND WINAPI BgSetActiveWindow(HWND) { return nullptr; }

// ---- launcher: OK'd without being seen --------------------------------------------------------------------------------
// Two dialogs: the launcher 0x6E (Play = IDOK, Settings = button 0x405, opened at 0x41BB9D) and Settings 0x6F (opened
// at 0x41BACF by the 0x405 handler; resolution combo 1020). A resolution is picked in Settings, whose OK saves it (into
// the user's VirtualStore copy, which tools/bg_run.py saves and restores) - then Play is pressed.
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
            // Settings works on the launcher's object [0x703664] (the launcher's dialog parameter; Settings' list builder
            // FUN_00419ff8): +4 device table, +8 adapter, +0xC device, +0x10 selected mode = item data
            const DWORD mode = static_cast<DWORD>(SendMessageA(combo, CB_GETITEMDATA, i, 0));
            auto obj = *reinterpret_cast<uint8_t**>(0x703664);
            if (obj) *reinterpret_cast<DWORD*>(obj + 0x10) = mode;
            if (obj && g_forceW && g_forceH) {
                // Dev\ForceSize: the picked entry of the game's mode table becomes WxH (a windowed device can be any
                // size - 4K / 21:9 without such a monitor). Entry: [obj+4] + adapter*0x68BC + device*0x1410
                // + 0x554 + 0x144 + mode*0x20 = {width, height, ...} (FUN_00419ff8).
                auto base = *reinterpret_cast<uint8_t**>(obj + 4);
                const int adapter = *reinterpret_cast<int*>(obj + 8), device = *reinterpret_cast<int*>(obj + 0xC);
                auto e = reinterpret_cast<int*>(base + adapter * 0x68BC + device * 0x1410 + 0x554 + 0x144 + mode * 0x20);
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
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr)) {
        if (g_shotCount == 0) dslog::Write("[dev] shot: GetBackBuffer 0x%08lX", static_cast<unsigned long>(hr));
        return;
    }
    D3DSURFACE_DESC d;
    bb->GetDesc(&d);
    IDirect3DSurface9* sys = nullptr;
    // a multisampled back buffer can't be read back directly: resolve it into a plain render target first
    IDirect3DSurface9* readFrom = bb;
    IDirect3DSurface9* resolved = nullptr;
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE &&
        SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
        SUCCEEDED(dev->StretchRect(bb, nullptr, resolved, nullptr, D3DTEXF_NONE)))
        readFrom = resolved;
    hr = dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = dev->GetRenderTargetData(readFrom, sys);
    if (FAILED(hr) && g_shotCount == 0)
        dslog::Write("[dev] shot: read-back of %ux%u format %d (msaa %d) failed 0x%08lX", d.Width, d.Height, d.Format,
                     d.MultiSampleType, static_cast<unsigned long>(hr));
    if (SUCCEEDED(hr)) {
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
    if (resolved) resolved->Release();
    bb->Release();
}

void OnPresent(IDirect3DDevice9* dev) {
    static bool first = true;
    if (first) {
        first = false;
        dslog::Write("[dev] first Present (shots %s)", g_shotDir[0] ? "on" : "off");
    }
    if (!g_shotDir[0]) return;
    const DWORD now = GetTickCount();
    if (now - g_lastShot >= g_shotEvery) {
        g_lastShot = now;
        SaveShot(dev);
    }
}
// Every dev run: the first access violations are logged (address, registers, code addresses on the stack) - background
// runs have no Windows error report to read. Vectored (first chance): the game's CRT replaces unhandled-exception filters.
int g_crashLogs = 0;
LONG WINAPI LogCrash(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_crashLogs >= 4) return EXCEPTION_CONTINUE_SEARCH;
    ++g_crashLogs;
    const CONTEXT* c = e->ContextRecord;
    dslog::Write("[dev] CRASH 0x%08lX at 0x%08lX (access %lu 0x%08lX) eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX "
                 "edi %08lX ebp %08lX esp %08lX", e->ExceptionRecord->ExceptionCode, c->Eip,
                 e->ExceptionRecord->NumberParameters > 1 ? static_cast<unsigned long>(e->ExceptionRecord->ExceptionInformation[0]) : 0,
                 e->ExceptionRecord->NumberParameters > 1 ? static_cast<unsigned long>(e->ExceptionRecord->ExceptionInformation[1]) : 0,
                 c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
    const auto* sp = reinterpret_cast<const DWORD*>(c->Esp);
    char line[512] = "[dev] stack code addresses:";
    size_t n = std::strlen(line);
    for (int i = 0; i < 256 && n < sizeof line - 12; ++i) {
        DWORD v = 0;
        __try { v = sp[i]; } __except (EXCEPTION_EXECUTE_HANDLER) { break; }
        if (v >= 0x401000 && v < 0x69F000) n += std::snprintf(line + n, sizeof line - n, " %08lX", v);
    }
    dslog::Write("%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Watchdog: the main thread's position (eip + return addresses into the exe on its stack) at 8 s, then every 15 s -
// where a background run stalls (no window to look at).
HANDLE g_mainThread = nullptr;
bool IsReturnAddress(DWORD v) {
    if (v < 0x401005 || v >= 0x69F000) return false;
    const auto* b = reinterpret_cast<const uint8_t*>(v);
    return b[-5] == 0xE8 || (b[-6] == 0xFF && (b[-5] == 0x15 || (b[-5] >= 0x90 && b[-5] <= 0x97))) ||
           (b[-2] == 0xFF && b[-1] >= 0xD0 && b[-1] <= 0xD7) || (b[-3] == 0xFF && b[-2] >= 0x50 && b[-2] <= 0x57);
}
void LogMainThread() {
    if (SuspendThread(g_mainThread) == static_cast<DWORD>(-1)) return;
    CONTEXT c = {};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(g_mainThread, &c)) {
        char line[600];
        int n = std::snprintf(line, sizeof line, "[dev] watchdog: main thread eip %08lX, calls:", c.Eip);
        const auto* sp = reinterpret_cast<const DWORD*>(c.Esp);
        MEMORY_BASIC_INFORMATION mbi = {};
        VirtualQuery(sp, &mbi, sizeof mbi);   // the stack's committed region: scan only up to its end
        const auto* end = reinterpret_cast<const DWORD*>(static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize);
        for (; sp < end && n < static_cast<int>(sizeof line) - 12; ++sp)
            if (IsReturnAddress(*sp)) n += std::snprintf(line + n, sizeof line - n, " %08lX", *sp);
        auto r = *reinterpret_cast<uint8_t* const*>(0x8842D0);
        if (r) {
            auto dev = *reinterpret_cast<void** const*>(r + 0x41878);
            if (dev) {
                void** vt = *reinterpret_cast<void***>(dev);
                n += std::snprintf(line + n, sizeof line - n, " | renderer device %p vt %p [3] %p [17] %p [43] %p", dev, vt,
                                   vt[3], vt[17], vt[43]);
            }
        }
        ResumeThread(g_mainThread);
        dslog::Write("%s", line);
        return;
    }
    ResumeThread(g_mainThread);
}
// QuitAfterMs: a background run ends itself (even when the script that started it died) - the user's ResolutionIndex,
// which the unseen Settings dialog overwrote, is put back first. Read through HKLM: the exe is virtualised, so this
// is the user's VirtualStore copy, as the launcher sees it.
constexpr char kDeviceSettings[] = "SOFTWARE\\Global Star Software\\Conflict Vietnam\\Device Settings";
DWORD g_quitAfter = 0, g_savedResolution = 0;
bool g_haveResolution = false;
DWORD WINAPI Watchdog(LPVOID) {
    const DWORD start = GetTickCount();
    for (int i = 0;; ++i) {
        Sleep(1000);
        if (i == 7 || (i >= 15 && i % 15 == 0)) LogMainThread();   // 8 s, then every 15 s
        if (g_quitAfter && GetTickCount() - start >= g_quitAfter) {
            if (g_haveResolution)
                RegSetKeyValueA(HKEY_LOCAL_MACHINE, kDeviceSettings, "ResolutionIndex", REG_DWORD, &g_savedResolution,
                                sizeof g_savedResolution);
            dslog::Write("[dev] background: QuitAfterMs reached - ResolutionIndex %s, exiting",
                         g_haveResolution ? "restored" : "not saved");
            TerminateProcess(GetCurrentProcess(), 0);
        }
    }
}

// "PLEASE PRESS ANY KEY TO BEGIN" after a level load: the loop runs FUN_0042c74d while [0x70D5C0] is set, which asks
// FUN_0053207f (any key) at 0x42C83E - nothing arrives in a background run, so it answers yes after 2 s on that screen.
constexpr uint32_t kAnyKey = 0x53207F;
DWORD g_anyKeySince = 0;
int __cdecl BgAnyKey() {
    const int real = reinterpret_cast<int(__cdecl*)()>(kAnyKey)();
    if (real == 1) return 1;
    const DWORD now = GetTickCount();
    if (!g_anyKeySince) g_anyKeySince = now;
    if (now - g_anyKeySince < 2000) return real;
    g_anyKeySince = 0;
    dslog::Write("[dev] background: \"press any key\" answered");
    return 1;
}

// StartLevel: the front end loads first (its archive must be mounted - level init unmounts the current one,
// 0x530340, and crashes on none); right after LoadFrontEnd (FUN_00428699, called at 0x428C69) the wanted level is
// selected by name and the loop mode [0x7052D8] goes back to 2, so the next frame's FUN_00428ba3 loads it.
constexpr uint32_t kMissionMgr = 0x70DB68, kSelectMission = 0x436FD8, kLoopMode = 0x7052D8;
char g_startLevel[260] = {};
void StartLevelAfterFrontEnd() {
    if (!g_startLevel[0]) return;
    const int ok = reinterpret_cast<int(__thiscall*)(uint32_t, const char*, int)>(kSelectMission)(kMissionMgr, g_startLevel, 0);
    dslog::Write("[dev] StartLevel: \"%s\" %s", g_startLevel, ok ? "selected" : "NOT in the mission table");
    if (ok) {
        // what the front end does for its "start mission" result 2 (FUN_0042c679): input reset, loop mode 2, menu
        // state 0x16 (in game) unless it is 0x18 - without the last one the main menu stayed drawn over the mission
        reinterpret_cast<void(__cdecl*)(int)>(0x531D80)(0);
        *reinterpret_cast<uint8_t*>(kLoopMode) = 2;
        if (*reinterpret_cast<const int*>(0x7126A8) != 0x18) reinterpret_cast<void(__cdecl*)(int)>(0x443012)(0x16);
    }
    g_startLevel[0] = 0;
}

constexpr uint8_t kActiveOrig[] = {0x0F, 0x84, 0x82, 0x00, 0x00, 0x00}, kActiveNew[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr uint8_t kStyleOrig[] = {0x74, 0x24}, kStyleNew[] = {0xEB, 0x24};   // 0x43144A jz -> jmp (no restyle)
constexpr BytePatch kBackground[] = {
    {DS_PATCH_NAME("Background: keep running while inactive"), 0x431425, kActiveOrig, kActiveNew, 6, false},
    {DS_PATCH_NAME("Background: no per-frame window restyle"), 0x43144A, kStyleOrig, kStyleNew, 2, false},
};
}  // namespace

void features::ApplyDevBackground() {
    AddVectoredExceptionHandler(1, &LogCrash);
    if (DevString("StartLevel", g_startLevel, sizeof g_startLevel)) {
        features::g_afterFrontEndLoad = &StartLevelAfterFrontEnd;   // called by frontend.cpp's LoadFrontEnd hook
        dslog::Write("[dev] StartLevel: \"%s\"", g_startLevel);
    }
    if (!DevDword("Background", 0)) {
        if (DevDword("MutedByTest", 0))
            if (HANDLE t = CreateThread(nullptr, 0, UnmuteThread, nullptr, 0, nullptr)) CloseHandle(t);
        return;
    }
    g_shotEvery = DevDword("ShotEveryMs", 2000);
    g_quitAfter = DevDword("QuitAfterMs", 0);
    DWORD size = sizeof g_savedResolution;
    g_haveResolution = RegGetValueA(HKEY_LOCAL_MACHINE, kDeviceSettings, "ResolutionIndex", RRF_RT_REG_DWORD, nullptr,
                                    &g_savedResolution, &size) == ERROR_SUCCESS;
    DevString("Resolution", g_resolution, sizeof g_resolution);
    char force[32] = {};
    if (DevString("ForceSize", force, sizeof force) && std::sscanf(force, "%dx%d", &g_forceW, &g_forceH) == 2 &&
        !g_resolution[0])
        std::strcpy(g_resolution, "800x600");   // any listed mode to overwrite
    if (DevString("ShotDir", g_shotDir, sizeof g_shotDir)) CreateDirectoryA(g_shotDir, nullptr);
    if (!patch::ApplyGroup("Background", kBackground)) {
        // a test run must never come up on the user's screen (full screen, focus): quit instead
        dslog::Write("[dev] background: patches failed - exiting instead of running visibly");
        ExitProcess(3);
    }
    if (HANDLE t = CreateThread(nullptr, 0, MuteThread, nullptr, 0, nullptr)) CloseHandle(t);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_mainThread, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    if (HANDLE t = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr)) CloseHandle(t);
    bool hooked = true;
    hooked &= patch::HookIndirectCall(0x42EF08, reinterpret_cast<void*>(&BgCreateWindowExA), 0x69F298);
    hooked &= patch::HookIndirectCall(0x42EF26, reinterpret_cast<void*>(&BgShowWindow), 0x69F244);
    hooked &= patch::HookIndirectCall(0x42FA27, reinterpret_cast<void*>(&BgSetWindowPos), 0x69F274);
    hooked &= patch::HookIndirectCall(0x42FB8A, reinterpret_cast<void*>(&BgSetWindowPos), 0x69F274);
    hooked &= patch::HookIndirectCall(0x42FB50, reinterpret_cast<void*>(&BgSetWindowPlacement), 0x69F278);
    hooked &= patch::HookIndirectCall(0x42FB93, reinterpret_cast<void*>(&BgSetActiveWindow), 0x69F280);
    hooked &= patch::HookIndirectCall(0x41BB9D, reinterpret_cast<void*>(&BgDialogBoxParamA), 0x69F230);
    hooked &= patch::HookIndirectCall(0x41BACF, reinterpret_cast<void*>(&BgSettingsDialogBoxParamA), 0x69F230);
    hooked &= patch::HookIndirectCall(0x429ADC, reinterpret_cast<void*>(&BgClipCursor), 0x69F2B8);
    hooked &= patch::HookIndirectCall(0x429AE9, reinterpret_cast<void*>(&BgClipCursor), 0x69F2B8);
    hooked &= patch::HookCall(0x42C83E, reinterpret_cast<void*>(&BgAnyKey), kAnyKey);
    if (!hooked) {
        dslog::Write("[dev] background: a window / launcher hook failed - exiting instead of running visibly");
        ExitProcess(3);
    }
    auto box = &BgMessageBoxA;
    patch::WriteValue(0x69F248, box);   // MessageBoxA import slot
    graphics::g_adjustPresentParams = &Windowed;   // the device is created windowed (any size, never fullscreen)
    graphics::g_onPresent = &OnPresent;
    graphics::g_retryLostCreate = true;
    dslog::Write("[dev] background mode on (shots: %s every %lu ms)", g_shotDir[0] ? g_shotDir : "off",
                 static_cast<unsigned long>(g_shotEvery));
}
#else
#include "features/features.h"
void features::ApplyDevBackground() {}
#endif

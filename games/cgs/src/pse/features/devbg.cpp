// Dev builds only: run the game in the background for scripted tests, without touching the user's screen, mouse or
// keyboard. HKCU\Software\GlobalStormFix\Dev:
//   Background   (DWORD 1)   window off-screen, never activated, no taskbar button, sound muted; the launcher is OK'd unseen;
//                            the game keeps running while inactive; Direct3D runs windowed; cursor clip / cursor
//                            moves are dropped (the per-frame ClipCursor would trap the user's mouse off-screen).
//   Resolution   (REG_SZ)    "WxH" picked in the launcher's Settings dialog (saved into the user's VirtualStore copy,
//                            which tools/bg_run.py saves and restores).
//   ShotDir      (REG_SZ)    folder for back-buffer screenshots (BMP), taken at Present - no need to be visible.
//   ShotEveryMs  (DWORD)     interval between screenshots (default 2000).
//
// Global Storm creates its window and runs the launcher dialogs from SecuROM-VM code (no static call sites), so the
// window / dialog / cursor / message-box functions are replaced in the exe's import slots (.rdata 0x6C0xxx) - the VM
// calls through them too. Message loop (WinMain FUN_00437A20): active flag [0x727CF8] (0 -> WaitMessage) - the je at
// 0x437B55 is NOPed; while active it forces style 0x15800000 every frame (0x437B85 cmp / je) - the je becomes jmp.
// Launcher = dialog 110 (Play = IDOK, Settings = button 1029), Settings = dialog 111 (resolution combo 1020).
#ifndef DS_DIST
#include <windows.h>
#include <audiopolicy.h>
#include <d3d9.h>
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/graphics.h"

namespace {
constexpr char kDevKey[] = "Software\\GlobalStormFix\\Dev";
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
HWND g_gameWindow = nullptr;
HWND WINAPI BgCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int, int, int w, int h, HWND parent,
                              HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND wnd = CreateWindowExA(ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, title, style, kOffX, kOffY, w, h, parent,
                               menu, inst, param);
    if (!parent) g_gameWindow = wnd;
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
        }
        dslog::Write("[dev] background: Settings resolution %s %s (%ld items)", g_resolution,
                     i != CB_ERR ? "picked" : "NOT LISTED", static_cast<long>(SendMessageA(combo, CB_GETCOUNT, 0, 0)));
        if (i == CB_ERR) {
            char list[1024] = {}, item[32];
            for (LRESULT k = 0, n = SendMessageA(combo, CB_GETCOUNT, 0, 0); k < n && k < 40; ++k) {
                SendMessageA(combo, CB_GETLBTEXT, k, reinterpret_cast<LPARAM>(item));
                strcat_s(list, item);
                strcat_s(list, " ");
            }
            dslog::Write("[dev] background: modes %s", list);
        }
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
// Dev\ForceSize: every entry of the picked size in the renderer's mode table ([0x739574] + 0x55C, per format 0x1428:
// count +0x140, modes +0x144 of 0x20 = {width, height, ...}; built just before the device) becomes WxH, so whatever reads
// the mode later sees it too. A windowed device can be any size (21:9 / 4K without such a monitor).
void ForceModeTable() {
    static bool done = false;
    auto table = *reinterpret_cast<uint8_t**>(0x739574);
    if (done || !table) return;
    done = true;
    int pw = 0, ph = 0, n = 0;
    std::sscanf(g_resolution, "%dx%d", &pw, &ph);
    for (int f = 0; f < 8; ++f) {
        uint8_t* fmt = table + 0x55C + f * 0x1428;
        const uint32_t count = *reinterpret_cast<uint32_t*>(fmt + 0x140);
        if (count > 0x9F) break;
        for (uint32_t m = 0; m < count; ++m) {
            auto e = reinterpret_cast<int*>(fmt + 0x144 + m * 0x20);
            if (e[0] == pw && e[1] == ph) { e[0] = g_forceW; e[1] = g_forceH; ++n; }
        }
    }
    dslog::Write("[dev] background: %d mode table entries %dx%d forced to %dx%d", n, pw, ph, g_forceW, g_forceH);
}

void Windowed(D3DPRESENT_PARAMETERS* pp) {
    pp->Windowed = TRUE;
    pp->FullScreen_RefreshRateInHz = 0;
    pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    pp->SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp->BackBufferFormat = D3DFMT_X8R8G8B8;
    if (g_forceW && g_forceH) {   // Dev\ForceSize: the device and the renderer's size ([0x8DE724] +0x41F20 / +0x41F24)
        pp->BackBufferWidth = static_cast<UINT>(g_forceW);
        pp->BackBufferHeight = static_cast<UINT>(g_forceH);
        ForceModeTable();
        if (auto r = *reinterpret_cast<uint8_t**>(0x8DE724)) {
            *reinterpret_cast<int*>(r + 0x41F20) = g_forceW;
            *reinterpret_cast<int*>(r + 0x41F24) = g_forceH;
        }
    }
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

int g_presents = 0;
void OnPresent(IDirect3DDevice9* dev) {
    if (++g_presents == 1 || g_presents % 1000 == 0) dslog::Write("[dev] background: Present #%d", g_presents);
    if (g_forceW && g_forceH) ForceModeTable();
    // The window proc (0x434490) turns input off on WM_ACTIVATE(inactive) and only on again when activated, which a
    // background window never is: it is told so directly (its own procedure only - real focus stays where it is).
    if (g_gameWindow && !*reinterpret_cast<volatile int*>(0x727CF8)) {
        SendMessageA(g_gameWindow, WM_ACTIVATE, WA_ACTIVE, 0);
        dslog::Write("[dev] background: window told it is active");
    }
    if (!g_shotDir[0]) return;
    const DWORD now = GetTickCount();
    if (now - g_lastShot >= g_shotEvery) {
        g_lastShot = now;
        SaveShot(dev);
    }
}
constexpr uint8_t kActiveOrig[] = {0x0F, 0x84, 0xA7, 0x00, 0x00, 0x00}, kActiveNew[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr uint8_t kStyleOrig[] = {0x74, 0x31}, kStyleNew[] = {0xEB, 0x31};     // 0x437B8A je -> jmp (no restyle)
constexpr BytePatch kBackground[] = {
    {DS_PATCH_NAME("Background: keep running while inactive"), 0x437B55, kActiveOrig, kActiveNew, 6, false},
    {DS_PATCH_NAME("Background: no per-frame window restyle"), 0x437B8A, kStyleOrig, kStyleNew, 2, false},
};

LONG WINAPI BgSetWindowLongA(HWND wnd, int index, LONG value) {
    if (index == GWL_STYLE) value &= ~WS_MAXIMIZE;
    if (index == GWL_EXSTYLE) value |= WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    return SetWindowLongA(wnd, index, value);
}
BOOL WINAPI BgSetWindowPlacement(HWND, const WINDOWPLACEMENT*) { return TRUE; }
HWND WINAPI BgSetActiveWindow(HWND) { return nullptr; }

// one DialogBoxParamA import serves both dialogs: told apart by the template id
INT_PTR WINAPI BgAnyDialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
    const uintptr_t id = reinterpret_cast<uintptr_t>(tmpl);
    if (id == 110) return BgDialogBoxParamA(inst, tmpl, parent, proc, lp);
    if (id == 111) return BgSettingsDialogBoxParamA(inst, tmpl, parent, proc, lp);
    dslog::Write("[dev] background: dialog %lu shown as it is", static_cast<unsigned long>(id));
    return DialogBoxParamA(inst, tmpl, parent, proc, lp);
}

// Bink films (logos, briefings) are paced by BinkWait (import 0x6C0384, polled per frame by the player FUN_006A7240),
// which follows the film's sound track - DirectSound doesn't play for an unfocused window, so the film never advanced.
// Background runs show a frame every 33 ms by the clock instead.
using BinkWaitFn = int(__stdcall*)(void*);
BinkWaitFn g_realBinkWait = nullptr;
DWORD g_lastBinkFrame = 0;
// Films are also cut short: the player (0x6A773E) ends a non-looping film once Bink's frame number (+0xC) is the last
// frame (frames +8 minus 1), so every film except the front-end's looping background (FeBg*.bik) jumps there.
void* g_skipBink = nullptr;
using BinkOpenFn = void*(__stdcall*)(const char*, unsigned);
BinkOpenFn g_realBinkOpen = nullptr;
void* __stdcall BgBinkOpen(const char* name, unsigned flags) {
    void* bink = g_realBinkOpen(name, flags);
    const char* base = name ? std::strrchr(name, '\\') : nullptr;
    base = base ? base + 1 : name;
    const bool keep = base && _strnicmp(base, "FeBg", 4) == 0;
    dslog::Write("[dev] background: film %s%s", name ? name : "?", keep ? "" : " - skipped");
    g_skipBink = keep ? nullptr : bink;
    return bink;
}

int __stdcall BgBinkWait(void* bink) {
    const DWORD now = GetTickCount();
    if (bink && bink == g_skipBink) {
        auto b = static_cast<uint32_t*>(bink);
        if (b[2] > 1 && b[3] < b[2] - 1) b[3] = b[2] - 1;
        return 0;
    }
    if (g_realBinkWait && !g_realBinkWait(bink)) { g_lastBinkFrame = now; return 0; }
    if (now - g_lastBinkFrame < 33) return 1;
    g_lastBinkFrame = now;
    return 0;
}

// ---- keyboard injection: Dev\KeyInject (DWORD) = DIK code held down (0 = none), written by the test script --------
// The game's keyboard (IDirectInputDevice8A from CreateDevice(GUID_SysKeyboard)) reads through GetDeviceState /
// GetDeviceData; the DirectInput vtable is shared by all devices, so the hooks check for that device. In a background
// run the device is never acquired (foreground cooperative level) - the hooks answer DI_OK with the injected state.
IDirectInputDevice8A* g_keyboard = nullptr;
using CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
using GetStateFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPVOID);
using GetDataFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
using AcquireFn = HRESULT(STDMETHODCALLTYPE*)(IDirectInputDevice8A*);
CreateDeviceFn g_realDiCreateDevice = nullptr;
GetStateFn g_realGetState = nullptr;
GetDataFn g_realGetData = nullptr;
AcquireFn g_realAcquire = nullptr;
DWORD g_injected = 0, g_reportedKey = 0, g_lastPoll = 0, g_sequence = 0;

DWORD InjectedKey() {
    const DWORD now = GetTickCount();
    if (now - g_lastPoll >= 10) {
        g_lastPoll = now;
        const DWORD k = DevDword("KeyInject", 0);
        if (k != g_injected) dslog::Write("[dev] background: key 0x%02lX %s", static_cast<unsigned long>(k ? k : g_injected), k ? "down" : "up");
        g_injected = k;
    }
    return g_injected & 0xFF;
}

HRESULT STDMETHODCALLTYPE BgGetState(IDirectInputDevice8A* dev, DWORD size, LPVOID data) {
    if (dev != g_keyboard) return g_realGetState(dev, size, data);
    HRESULT hr = g_realGetState(dev, size, data);
    if (FAILED(hr)) { std::memset(data, 0, size); hr = DI_OK; }
    if (const DWORD k = InjectedKey(); k && k < size) static_cast<uint8_t*>(data)[k] = 0x80;
    return hr;
}

// buffered reads: a press / release event for each change of the injected key
HRESULT STDMETHODCALLTYPE BgGetData(IDirectInputDevice8A* dev, DWORD objSize, LPDIDEVICEOBJECTDATA out, LPDWORD inOut,
                                    DWORD flags) {
    if (dev != g_keyboard) return g_realGetData(dev, objSize, out, inOut, flags);
    const DWORD room = inOut ? *inOut : 0;
    HRESULT hr = g_realGetData(dev, objSize, out, inOut, flags);
    if (FAILED(hr)) { if (inOut) *inOut = 0; hr = DI_OK; }
    const DWORD k = InjectedKey();
    if (k != g_reportedKey && inOut && out && *inOut + 2 <= room && !(flags & DIGDD_PEEK)) {
        auto put = [&](DWORD key, bool down) {
            auto e = reinterpret_cast<DIDEVICEOBJECTDATA*>(reinterpret_cast<uint8_t*>(out) + *inOut * objSize);
            std::memset(e, 0, objSize);
            e->dwOfs = key;
            e->dwData = down ? 0x80 : 0;
            e->dwTimeStamp = GetTickCount();
            e->dwSequence = ++g_sequence;
            ++*inOut;
        };
        if (g_reportedKey) put(g_reportedKey, false);
        if (k) put(k, true);
        g_reportedKey = k;
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE BgAcquire(IDirectInputDevice8A* dev) {
    const HRESULT hr = g_realAcquire(dev);
    return dev == g_keyboard && FAILED(hr) ? DI_OK : hr;
}

void PatchSlot(void** vt, int i, void* fn, void** orig) {
    if (vt[i] == fn) return;
    *orig = vt[i];
    DWORD old;
    VirtualProtect(&vt[i], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    vt[i] = fn;
    VirtualProtect(&vt[i], sizeof(void*), old, &old);
}

HRESULT STDMETHODCALLTYPE BgDiCreateDevice(IDirectInput8A* di, REFGUID guid, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN outer) {
    const HRESULT hr = g_realDiCreateDevice(di, guid, out, outer);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(guid, GUID_SysKeyboard)) {
        g_keyboard = *out;
        void** vt = *reinterpret_cast<void***>(g_keyboard);
        PatchSlot(vt, 7, reinterpret_cast<void*>(&BgAcquire), reinterpret_cast<void**>(&g_realAcquire));
        PatchSlot(vt, 9, reinterpret_cast<void*>(&BgGetState), reinterpret_cast<void**>(&g_realGetState));
        PatchSlot(vt, 10, reinterpret_cast<void*>(&BgGetData), reinterpret_cast<void**>(&g_realGetData));
        dslog::Write("[dev] background: keyboard device %p wrapped (Dev\\KeyInject)", g_keyboard);
    }
    return hr;
}

bool g_background = false;

// The game only reads its input while it is the foreground window: background runs say it is.
HWND WINAPI BgGetForegroundWindow() { return g_gameWindow ? g_gameWindow : GetForegroundWindow(); }
// KeyInject bits 8-15 = the virtual-key code of the same key (GetKeyboardState users)
BOOL WINAPI BgGetKeyboardState(PBYTE keys) {
    const BOOL ok = GetKeyboardState(keys);
    InjectedKey();
    if (const DWORD vk = (g_injected >> 8) & 0xFF) keys[vk] = 0x80;
    return ok;
}

template <typename F>
void Slot(uint32_t slot, F fn) { patch::WriteValue(slot, fn); }
}  // namespace

void features::ApplyDevBackground() {
    if (!DevDword("Background", 0)) {
        if (DevDword("MutedByTest", 0))
            if (HANDLE t = CreateThread(nullptr, 0, UnmuteThread, nullptr, 0, nullptr)) CloseHandle(t);
        return;
    }
    g_background = true;
    g_shotEvery = DevDword("ShotEveryMs", 2000);
    DevString("Resolution", g_resolution, sizeof g_resolution);
    char force[32] = {};
    if (DevString("ForceSize", force, sizeof force) && std::sscanf(force, "%dx%d", &g_forceW, &g_forceH) == 2 &&
        !g_resolution[0])
        std::strcpy(g_resolution, "800x600");   // any listed mode to overwrite
    if (DevString("ShotDir", g_shotDir, sizeof g_shotDir)) CreateDirectoryA(g_shotDir, nullptr);
    if (!patch::ApplyGroup("Background", kBackground)) return;
    if (HANDLE t = CreateThread(nullptr, 0, MuteThread, nullptr, 0, nullptr)) CloseHandle(t);
    Slot(0x6C025C, &BgCreateWindowExA);
    Slot(0x6C0218, &BgShowWindow);
    Slot(0x6C02C0, &BgSetWindowPos);
    Slot(0x6C02C8, &BgSetWindowLongA);
    Slot(0x6C02C4, &BgSetWindowPlacement);
    Slot(0x6C02CC, &BgSetActiveWindow);
    Slot(0x6C0208, &BgAnyDialogBoxParamA);
    Slot(0x6C023C, &BgClipCursor);
    Slot(0x6C026C, &BgSetCursorPos);
    Slot(0x6C02A0, &BgMessageBoxA);
    Slot(0x6C0280, &BgGetForegroundWindow);
    Slot(0x6C0294, &BgGetKeyboardState);
    g_realBinkWait = *reinterpret_cast<BinkWaitFn*>(0x6C0384);
    Slot(0x6C0384, &BgBinkWait);
    g_realBinkOpen = *reinterpret_cast<BinkOpenFn*>(0x6C03AC);
    Slot(0x6C03AC, &BgBinkOpen);
    graphics::Install();
    graphics::g_adjustPresentParams = &Windowed;   // the device is created windowed (any size, never fullscreen)
    graphics::g_onPresent = &OnPresent;
    dslog::Write("[dev] background mode on (shots: %s every %lu ms)", g_shotDir[0] ? g_shotDir : "off",
                 static_cast<unsigned long>(g_shotEvery));
}
void features::OnDirectInputCreated(void* di) {
    if (!g_background || !di) return;
    PatchSlot(*static_cast<void***>(di), 3, reinterpret_cast<void*>(&BgDiCreateDevice),
              reinterpret_cast<void**>(&g_realDiCreateDevice));
}
#else
#include "features/features.h"
void features::ApplyDevBackground() {}
void features::OnDirectInputCreated(void*) {}
#endif

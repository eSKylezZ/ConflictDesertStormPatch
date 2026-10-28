// Dev builds only (tgefix.devbg): run the game in the background for scripted tests, without touching the user's screen,
// mouse or keyboard - DesertStorm2Fix's devbg.cpp for The Great Escape. HKCU\Software\GreatEscapeFix\Dev:
//   Background  (DWORD 1)  window off-screen, never activated, no taskbar button, sound muted; the launcher and its
//                          Settings... dialog are never shown (OK'd unseen); the game keeps running while inactive; the
//                          device is windowed; cursor clip / cursor moves / mouse capture are dropped; message boxes are
//                          logged and answered; the log is GreatEscapeFix-test.log (log.cpp).
//   Resolution  (REG_SZ)   "WxH": picked in Settings... (32-bit colour) before Play. Settings' OK saves it into the
//                          game's Device Settings - the test driver saves and restores the user's ResolutionIndex.
//   Shot        (REG_SZ)   a .bmp path: the next presented frame's back buffer is written there and the value deleted
//                          (the driver waits for that). Needs no visible window.
//   MouseX/Y    (DWORD)    the game's pointer in back-buffer pixels (GetCursorPos answers it); missing = far outside the
//                          window, so the user's real pointer never hovers the menus. Clicks can't be posted (the
//                          game's mouse buttons are DirectInput); keys can (WM_KEYDOWN / WM_KEYUP with the scan code).
//
// Message loop 0x5D58C4: active flag [0x7DC2FC] = 0 -> WaitMessage (je 0x5D5933 -> nops). Window: CreateWindowExA at
// 0x5D0DA0 (class 0x61BF10); placement = display.cpp (PlaceWindow off-screen while Background()). Launcher = template
// 0x6A, proc 0x5BF9C0 pushed at 0x5BFA12 (Settings... = button 0x405 -> template 0x6C, display.cpp's
// SettingsProc calls DevBgSettingsDialog). ClipCursor 0x5CCA12 / 0x5CFFC6 / 0x5D0095 / 0x5D5999, SetCursorPos
// 0x4B81DC, SetCapture 0x4BA04C, MessageBoxA import 0x5F51B4, mouse poll's GetCursorPos load 0x4B91FF.
#include "features/features.h"

#ifndef DS_DIST
#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "loader.h"
#include "core/pse_host.h"

namespace {
constexpr char kDevKey[] = "Software\\GreatEscapeFix\\Dev";
constexpr int kOffX = -30000, kOffY = -30000;   // on no monitor; not -32000 (Windows' minimised position)

bool g_background = false;
char g_resolution[32] = {};
HWND g_launcher = nullptr;

DWORD DevDword(const char* name, DWORD def) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, kDevKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : def;
}
bool DevString(const char* name, char* out, DWORD size) {
    return RegGetValueA(HKEY_CURRENT_USER, kDevKey, name, RRF_RT_REG_SZ, nullptr, out, &size) == ERROR_SUCCESS && out[0];
}

// ---- sound: this process's audio sessions muted (as DS2: Dev\MutedByTest, the next normal start unmutes) ----------
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

DWORD WINAPI UnmuteThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (int i = 0; i < 60; ++i) {
        SetOwnSessionsMuted(false);
        Sleep(500);
    }
    RegDeleteKeyValueA(HKEY_CURRENT_USER, kDevKey, "MutedByTest");
    return 0;
}

// ---- window / cursor ------------------------------------------------------------------------------------------------
HWND WINAPI BgCreateWindowExA(DWORD ex, LPCSTR cls, LPCSTR title, DWORD style, int, int, int w, int h, HWND parent,
                              HMENU menu, HINSTANCE inst, LPVOID param) {
    HWND wnd = CreateWindowExA(ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, cls, title, style, kOffX, kOffY, w, h, parent,
                               menu, inst, param);
    dslog::Write("[dev] background: game window %p created off-screen (%dx%d)", wnd, w, h);
    return wnd;
}
BOOL WINAPI BgClipCursor(const RECT*) { return TRUE; }
BOOL WINAPI BgSetCursorPos(int, int) { return TRUE; }
HWND WINAPI BgSetCapture(HWND) { return nullptr; }

// The mouse poll's GetCursorPos: the test's pointer (back-buffer pixels -> screen), else far outside the window.
BOOL WINAPI BgGetCursorPos(POINT* pt) {
    if (!pt) return FALSE;
    HWND wnd = *reinterpret_cast<HWND*>(0x7DC238);
    const DWORD x = DevDword("MouseX", 0xFFFFFFFF), y = DevDword("MouseY", 0xFFFFFFFF);
    POINT p = {x == 0xFFFFFFFF ? -20000 : static_cast<LONG>(x), y == 0xFFFFFFFF ? -20000 : static_cast<LONG>(y)};
    if (wnd) ClientToScreen(wnd, &p);
    *pt = p;
    return TRUE;
}

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

// ---- launcher: never shown ------------------------------------------------------------------------------------------
// Taking WS_VISIBLE off in WM_INITDIALOG keeps the dialog manager from showing (and activating) it; posted commands
// still run its modal loop.
void Unseen(HWND dlg) {
    SetWindowLongA(dlg, GWL_STYLE, GetWindowLongA(dlg, GWL_STYLE) & ~WS_VISIBLE);
    SetWindowPos(dlg, nullptr, kOffX, kOffY, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

// Every DialogBoxParamA (import slot 0x5F51F0; the launcher is template 0x6A, its Settings... 0x6C) goes through
// BgDialogProc: the game's procedure is kept as a window property from the first message on.
DLGPROC g_pendingProc = nullptr;
INT_PTR CALLBACK BgDialogProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    auto proc = reinterpret_cast<DLGPROC>(GetPropA(dlg, "tgefix.proc"));
    if (!proc && g_pendingProc) {
        proc = g_pendingProc;
        g_pendingProc = nullptr;
        SetPropA(dlg, "tgefix.proc", reinterpret_cast<HANDLE>(proc));
    }
    if (!proc) return FALSE;
    if (msg == WM_NCDESTROY) RemovePropA(dlg, "tgefix.proc");
    const INT_PTR r = proc(dlg, msg, wp, lp);
    if (msg == WM_INITDIALOG) {
        Unseen(dlg);
        char title[96] = "";
        GetWindowTextA(dlg, title, sizeof title);
        const bool launcher = GetDlgItem(dlg, 0x405) && GetDlgItem(dlg, IDOK) && strcmp(title, "Settings...") != 0;
        dslog::Write("[dev] background: dialog \"%s\" (proc %p)%s", title, reinterpret_cast<void*>(proc),
                     launcher ? " = launcher, OK'd unseen" : strcmp(title, "Settings...") == 0 ? "" : " - OK'd");
        if (launcher) {
            g_launcher = dlg;
            PostMessageA(dlg, WM_COMMAND, g_resolution[0] ? 0x405 : IDOK, 0);   // Settings first for a resolution
        } else if (strcmp(title, "Settings...") != 0) {
            PostMessageA(dlg, WM_COMMAND, IDOK, 0);
        }
    }
    return r;
}

INT_PTR WINAPI BgDialogBoxParamA(HINSTANCE inst, LPCSTR tmpl, HWND parent, DLGPROC proc, LPARAM lp) {
    g_pendingProc = proc;
    return DialogBoxParamA(inst, tmpl, parent, &BgDialogProc, lp);
}

void SelectByText(HWND dlg, int id, const char* text) {
    HWND combo = GetDlgItem(dlg, id);
    const LRESULT i = SendMessageA(combo, CB_FINDSTRINGEXACT, static_cast<WPARAM>(-1), reinterpret_cast<LPARAM>(text));
    if (i != CB_ERR) {
        SendMessageA(combo, CB_SETCURSEL, i, 0);
        SendMessageA(dlg, WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    }
    dslog::Write("[dev] background: Settings %s \"%s\" %s", id == 0x3FC ? "resolution" : "colour", text,
                 i != CB_ERR ? "picked" : "NOT LISTED");
}

// ---- screenshots: the back buffer at Present (D3D8 vtable: 16 GetBackBuffer, 27 CreateImageSurface, 28 CopyRects;
// surface: 8 GetDesc, 9 LockRect, 10 UnlockRect) ------------------------------------------------------------------------
struct SurfDesc { uint32_t format, type, usage, pool, size, msType, width, height; };
struct LockedRect { int pitch; void* bits; };
template <class T> T VCall(void* obj, int slot) { return reinterpret_cast<T>((*reinterpret_cast<void***>(obj))[slot]); }
void Release(void* obj) { if (obj) VCall<ULONG(__stdcall*)(void*)>(obj, 2)(obj); }

bool SaveBackBuffer(void* dev, const char* path) {
    void* bb = nullptr;
    if (VCall<HRESULT(__stdcall*)(void*, UINT, int, void**)>(dev, 16)(dev, 0, 0, &bb) < 0 || !bb) return false;
    SurfDesc d = {};
    VCall<HRESULT(__stdcall*)(void*, SurfDesc*)>(bb, 8)(bb, &d);
    void* img = nullptr;
    bool ok = false;
    const bool x8 = d.format == 22 || d.format == 21, r565 = d.format == 23;   // X8R8G8B8 / A8R8G8B8, R5G6B5
    HRESULT hrImage = -1, hrCopy = -1;
    if ((x8 || r565) &&
        (hrImage = VCall<HRESULT(__stdcall*)(void*, UINT, UINT, uint32_t, void**)>(dev, 27)(dev, d.width, d.height, d.format, &img)) >= 0 &&
        (hrCopy = VCall<HRESULT(__stdcall*)(void*, void*, const RECT*, UINT, void*, const POINT*)>(dev, 28)(dev, bb, nullptr, 0, img, nullptr)) >= 0) {
        LockedRect lr = {};
        if (VCall<HRESULT(__stdcall*)(void*, LockedRect*, const RECT*, DWORD)>(img, 9)(img, &lr, nullptr, 0x10) >= 0) {
            if (FILE* f = std::fopen(path, "wb")) {
                const DWORD row = d.width * 3, pad = (4 - row % 4) % 4, size = (row + pad) * d.height;
                BITMAPFILEHEADER fh = {0x4D42, static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + size),
                                       0, 0, sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
                BITMAPINFOHEADER ih = {sizeof ih, static_cast<LONG>(d.width), static_cast<LONG>(d.height), 1, 24};
                std::fwrite(&fh, sizeof fh, 1, f);
                std::fwrite(&ih, sizeof ih, 1, f);
                static unsigned char line[3 * 8192 + 4];
                for (int y = static_cast<int>(d.height) - 1; y >= 0; --y) {   // BMP rows bottom-up
                    const auto* src = static_cast<const unsigned char*>(lr.bits) + y * lr.pitch;
                    for (DWORD x = 0; x < d.width && x < 8192; ++x) {
                        if (x8) {
                            std::memcpy(line + x * 3, src + x * 4, 3);
                        } else {
                            const uint16_t v = reinterpret_cast<const uint16_t*>(src)[x];
                            line[x * 3 + 0] = static_cast<unsigned char>((v & 0x1F) * 255 / 31);
                            line[x * 3 + 1] = static_cast<unsigned char>(((v >> 5) & 0x3F) * 255 / 63);
                            line[x * 3 + 2] = static_cast<unsigned char>((v >> 11) * 255 / 31);
                        }
                    }
                    std::memset(line + row, 0, pad);
                    std::fwrite(line, 1, row + pad, f);
                }
                std::fclose(f);
                ok = true;
            }
            VCall<HRESULT(__stdcall*)(void*)>(img, 10)(img);
        }
    }
    if (!ok)
        dslog::Write("[dev] background: back buffer %ux%u format %u ms %u: image surface %08lX, copy %08lX", d.width,
                     d.height, d.format, d.msType, static_cast<unsigned long>(hrImage), static_cast<unsigned long>(hrCopy));
    Release(img);
    Release(bb);
    return ok;
}

int PSE_CALL OnPresent(PSEEventId, void* dev, void*) {
    // ShotBurst (REG_SZ "<prefix>|<n>"): the next n presented frames -> <prefix>_000.bmp ... (transitions frame by frame)
    static int burstLeft = 0, burstIndex = 0;
    static char burstPrefix[MAX_PATH] = {};
    char burst[MAX_PATH] = {};
    if (dev && !burstLeft && DevString("ShotBurst", burst, sizeof burst)) {
        RegDeleteKeyValueA(HKEY_CURRENT_USER, kDevKey, "ShotBurst");
        if (char* bar = std::strrchr(burst, '|')) {
            *bar = 0;
            burstLeft = std::atoi(bar + 1);
            burstIndex = 0;
            strcpy_s(burstPrefix, burst);
        }
    }
    if (dev && burstLeft > 0) {
        char path[MAX_PATH + 16];
        sprintf_s(path, "%s_%03d.bmp", burstPrefix, burstIndex++);
        SaveBackBuffer(dev, path);
        --burstLeft;
        return 0;
    }
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (!dev || now - last < 100) return 0;
    last = now;
    char path[MAX_PATH] = {};
    if (!DevString("Shot", path, sizeof path)) return 0;
    const bool ok = SaveBackBuffer(dev, path);
    RegDeleteKeyValueA(HKEY_CURRENT_USER, kDevKey, "Shot");
    if (!ok) dslog::Write("[dev] background: shot %s failed", path);
    return 0;
}

constexpr uint8_t kActiveOrig[] = {0x74, 0x73}, kActiveNew[] = {0x90, 0x90};   // 0x5D5933 je WaitMessage -> nops
constexpr BytePatch kBackground[] = {
    {DS_PATCH_NAME("Background: keep running while inactive"), 0x5D5933, kActiveOrig, kActiveNew, 2, false},
};
}  // namespace

bool features::Background() { return g_background; }

void features::DevBgSettingsDialog(void* dlgParam, unsigned msg) {
    if (!g_background) return;
    HWND dlg = static_cast<HWND>(dlgParam);
    if (msg == WM_INITDIALOG) {
        SelectByText(dlg, 0x3FC, g_resolution);
        SelectByText(dlg, 0x405, "True Color (32 bit)");
        PostMessageA(dlg, WM_COMMAND, IDOK, 0);
    } else if (msg == WM_DESTROY && g_launcher) {   // then Play (posted earlier, the Settings loop would run it)
        PostMessageA(g_launcher, WM_COMMAND, IDOK, 0);
    }
}

void features::ApplyDevBackground() {
    if (!DevDword("Background", 0)) {
        if (DevDword("MutedByTest", 0))
            if (HANDLE t = CreateThread(nullptr, 0, UnmuteThread, nullptr, 0, nullptr)) CloseHandle(t);
        return;
    }
    if (!patch::ApplyGroup("Background", kBackground)) return;
    g_background = true;
    DevString("Resolution", g_resolution, sizeof g_resolution);
    if (HANDLE t = CreateThread(nullptr, 0, MuteThread, nullptr, 0, nullptr)) CloseHandle(t);
    patch::HookIndirectCall(0x5D0DA0, reinterpret_cast<const void*>(&BgCreateWindowExA), 0x5F5214);
    static constexpr uint32_t kClipSites[] = {0x5CCA12, 0x5CFFC6, 0x5D0095, 0x5D5999};
    for (uint32_t site : kClipSites) patch::HookIndirectCall(site, reinterpret_cast<const void*>(&BgClipCursor), 0x5F5258);
    patch::HookIndirectCall(0x4B81DC, reinterpret_cast<const void*>(&BgSetCursorPos), 0x5F5244);
    patch::HookIndirectCall(0x4BA04C, reinterpret_cast<const void*>(&BgSetCapture), 0x5F5254);
    patch::HookSlot(0x5F51B4, reinterpret_cast<const void*>(&BgMessageBoxA));   // MessageBoxA import slot
    patch::HookSlot(0x5F51F0, reinterpret_cast<const void*>(&BgDialogBoxParamA));   // DialogBoxParamA import slot
    // mov ebp, [GetCursorPos] -> mov ebp, BgGetCursorPos; nop
    static uint8_t load[6] = {0xBD, 0, 0, 0, 0, 0x90};
    const uint32_t fn = reinterpret_cast<uint32_t>(&BgGetCursorPos);
    memcpy(load + 1, &fn, 4);
    static const uint8_t orig[] = {0x8B, 0x2D, 0x50, 0x52, 0x5F, 0x00};
    static const BytePatch cursor[] = {{DS_PATCH_NAME("Background: test pointer"), 0x4B91FF, orig, load, sizeof load, false}};
    patch::ApplyGroup("Background: mouse", cursor);
    if (const PSEMessagingInterface* m = pse::MessagingInterface())
        m->Subscribe(pse_host::Find("tgefix.devbg"), PSE_EVENT_PRESENT, &OnPresent, nullptr, 0);
    dslog::Write("[dev] background mode on (resolution %s)", g_resolution[0] ? g_resolution : "as set");
}
#else
bool features::Background() { return false; }
void features::DevBgSettingsDialog(void*, unsigned) {}
void features::ApplyDevBackground() {}
#endif

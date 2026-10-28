// Windowed mode (tgefix.display): the launcher's Settings... dialog (template 108) has a "Windowed" checkbox the game
// hides at start-up (ShowWindow(GetDlgItem(0x401), SW_HIDE) at 0x5BEF34) and never reads; the launcher's OK handler
// writes the adapter's fullscreen flag = 1 unconditionally (0x5BF47E, adapter +0x66C8 = renderer +0x67AC for adapter
// 0; device creation 0x44501E makes a windowed device when it is 0). The engine's windowed path still exists: after the
// launcher, 0x5D0A58 sizes the window to AdjustWindowRect(mode, 0xCF0000) with SetWindowPos when the flag is 0. Same
// state as DS1 (games/ds1/src/features/display.cpp).
//
// What this feature does:
//  - the Settings dialog's procedure 0x5BF8B0 (pushed at 0x5BF904) is wrapped: the box is shown and ticked from the
//    setting `Windowed` on WM_INITDIALOG, read back on OK and applied at once (the device is made after the launcher);
//  - windowed: the OK handler's immediate (0x5BF484) is 0, so the device is windowed (back buffer = the chosen mode);
//  - the message loop forced the fullscreen window style 0x15800000 on every frame the device was fine
//    (0x5D5945..0x5D5986: GetWindowLong != 0x15800000 -> SetWindowLong + hide the cursor) - windowed it is skipped
//    (the cursor is hidden once, over the game's window only);
//  - the window placement (SetWindowPos at 0x5D0A80): a window with a caption, client = the mode's size, centred on
//    its monitor's work area (shrunk keeping the aspect when it doesn't fit), not topmost; a mode as big as the monitor
//    = a borderless window over the whole monitor;
//  - the window to the front after either placement (fullscreen: SetWindowPos at 0x5D0C18): the game never activates
//    it, so with another program in front a fullscreen device got no screen (black 800 x 600 box, game paused);
//  - the mouse: the poll 0x4B91E0 turns GetCursorPos into client coordinates with ScreenToClient (loaded at 0x4B9205)
//    -> ScreenToGame also scales them to the back buffer when the client area has another size.
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"

namespace {
constexpr uint32_t kSettingsProcPush = 0x5BF905;   // imm32 of `push 0x5BF8B0` (DialogBoxParamA, template 108)
constexpr uint32_t kSettingsProc = 0x5BF8B0;
constexpr int kWindowedBox = 0x401;
constexpr uint32_t kStyleCheck = 0x5D5945;   // continues at 0x5D594B, skips to the frame call 0x5D5988
constexpr uint8_t kStyleCheckOrig[] = {0x8B, 0x0D, 0x20, 0xED, 0x79, 0x00};   // mov ecx, [0x79ED20]
constexpr uint32_t kPlaceCall = 0x5D0A80, kIatSetWindowPos = 0x5F5190;
constexpr uint32_t kFullscreenPlaceCall = 0x5D0C18;   // the fullscreen branch's SetWindowPos (flags 0x248, no activate)
constexpr uint32_t kCursorLoad = 0x4B9205;
constexpr uint8_t kCursorLoadOrig[] = {0x8B, 0x1D, 0x40, 0x52, 0x5F, 0x00};   // mov ebx, [ScreenToClient]
constexpr uint32_t kRenderer = 0x79ED20;   // renderer*: +0x84 present params (back buffer W, H)

constexpr uint8_t kFullOrig[] = {0x01, 0x00, 0x00, 0x00}, kFullOff[] = {0x00, 0x00, 0x00, 0x00};
constexpr BytePatch kWindowedDevice[] = {
    {DS_PATCH_NAME("Launcher OK: windowed device"), 0x5BF484, kFullOrig, kFullOff, sizeof kFullOrig, false},
};

DLGPROC g_settingsProc = reinterpret_cast<DLGPROC>(kSettingsProc);
bool g_windowed = false;   // this run (read at start-up; the dialog's change applies at the next start)

// The device immediate follows the mode; every other hook checks g_windowed itself.
bool g_deviceApplied = false;
void SetWindowed(bool on) {
    g_windowed = on;
    bool ok = true;
    if (on && !g_deviceApplied) ok = g_deviceApplied = patch::ApplyGroup("Display: windowed", kWindowedDevice);
    else if (!on && g_deviceApplied) g_deviceApplied = !(ok = patch::RevertGroup("Display: windowed", kWindowedDevice));
    dslog::Write("%s Display: %s", ok ? "[ok]  " : "[fail]", on ? "windowed" : "fullscreen (launcher Settings... > Windowed)");
}

INT_PTR CALLBACK SettingsProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK && !features::Background()) {
        const bool on = IsDlgButtonChecked(dlg, kWindowedBox) == BST_CHECKED;
        if (on != g_windowed) {
            features::SaveWindowed(on);
            patch::OwnerScope owner(pse_host::Find("tgefix.display"));   // (a dialog message: outside the feature's scope)
            SetWindowed(on);   // in time: the device is made after the launcher's OK
        }
    } else if (msg == WM_COMMAND && LOWORD(wp) == kWindowedBox && HIWORD(wp) == BN_CLICKED) {
        HWND box = GetDlgItem(dlg, kWindowedBox);
        if (box && (GetWindowLongA(box, GWL_STYLE) & 0xF) != BS_AUTOCHECKBOX)   // a plain checkbox: toggle it ourselves
            CheckDlgButton(dlg, kWindowedBox, IsDlgButtonChecked(dlg, kWindowedBox) == BST_CHECKED ? BST_UNCHECKED : BST_CHECKED);
        return TRUE;
    }
    const INT_PTR r = g_settingsProc(dlg, msg, wp, lp);
    features::DevBgSettingsDialog(dlg, msg);
    if (msg == WM_INITDIALOG) {   // after the game's set-up, which hides the box
        if (HWND box = GetDlgItem(dlg, kWindowedBox)) {
            ShowWindow(box, SW_SHOW);
            EnableWindow(box, TRUE);
            CheckDlgButton(dlg, kWindowedBox, features::Get().windowed ? BST_CHECKED : BST_UNCHECKED);
        }
    }
    return r;
}

// The message loop's style check: windowed -> straight to the frame (once: the cursor hidden over the game's window).
void __cdecl HideCursorOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    while (ShowCursor(FALSE) >= 0) {}
}
__declspec(naked) void StyleCheckStub() {
    __asm {
        cmp byte ptr [g_windowed], 0
        jne windowed
        mov ecx, dword ptr ds:[0x79ED20]
        push 0x5D594B          // kStyleCheckNext (literals: a constexpr in __asm is a memory operand)
        ret
    windowed:
        call HideCursorOnce
        push 0x5D5988          // kStyleCheckSkip
        ret
    }
}

uint32_t Read32(uint32_t va) {
    __try {
        return *reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(va));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// The back buffer's size (the present parameters at renderer +0x84); 0 before the device exists.
bool BackBuffer(int& w, int& h) {
    const uint32_t r = Read32(kRenderer);
    w = r ? static_cast<int>(Read32(r + 0x84)) : 0;
    h = r ? static_cast<int>(Read32(r + 0x88)) : 0;
    return w > 0 && h > 0;
}

// The game window to the front: the game never activates it (both placement calls leave activation alone), so when
// another program is in front as the launcher closes - how the game was started decides - a fullscreen device gets no
// screen (an 800 x 600 black window over the desktop) and the game pauses as inactive. As DS1 (display.cpp).
void BringToFront(HWND wnd, const char* when, bool altTap = true) {
    if (!wnd || features::Background()) return;   // dev test runs: never take the user's foreground
    if (HWND fg0 = GetForegroundWindow(); fg0 && fg0 != wnd) {
        char c0[64] = "";
        DWORD p0 = 0;
        GetClassNameA(fg0, c0, sizeof c0);
        GetWindowThreadProcessId(fg0, &p0);
        dslog::Write("Display: in front before (%s): %s, pid %lu", when, c0, p0);
    }
    if (GetForegroundWindow() == wnd) {
        dslog::Write("Display: game window in front (%s)", when);
        return;
    }
    HWND fg = GetForegroundWindow();
    const DWORD me = GetCurrentThreadId(), other = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    const bool attached = other && other != me && AttachThreadInput(other, me, TRUE);
    ShowWindow(wnd, SW_SHOW);
    BringWindowToTop(wnd);
    SetForegroundWindow(wnd);
    if (attached) AttachThreadInput(other, me, FALSE);
    if (altTap && GetForegroundWindow() != wnd) {   // an Alt tap lifts the foreground lock for this process
        INPUT in[2] = {};
        in[0].type = in[1].type = INPUT_KEYBOARD;
        in[0].ki.wVk = in[1].ki.wVk = VK_MENU;
        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
        SetForegroundWindow(wnd);
    }
    char cls[64] = "", title[96] = "";
    DWORD pid = 0;
    if (HWND now = GetForegroundWindow()) {
        GetClassNameA(now, cls, sizeof cls);
        GetWindowTextA(now, title, sizeof title);
        GetWindowThreadProcessId(now, &pid);
    }
    dslog::Write("Display: game window %s (%s; was behind %s \"%s\", pid %lu%s)",
                 GetForegroundWindow() == wnd ? "brought to the front" : "still behind", when, cls, title, pid,
                 pid == GetCurrentProcessId() ? " = the game's own" : "");
}

// SetWindowPos at 0x5D0C18 (fullscreen path): as the game, then to the front.
BOOL WINAPI PlaceFullscreen(HWND wnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    const BOOL ok = SetWindowPos(wnd, after, x, y, cx, cy, flags);
    BringToFront(wnd, "fullscreen placement");
    return ok;
}

// SetWindowPos at 0x5D0A80 (windowed path only): cx / cy = AdjustWindowRect(0, 0, mode, 0xCF0000).
BOOL WINAPI PlaceWindow(HWND wnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    if (!g_windowed) return SetWindowPos(wnd, after, x, y, cx, cy, flags);
    if (features::Background())   // dev test runs: off-screen, shown without activation, the mode's own size
        return SetWindowPos(wnd, nullptr, -30000, -30000, cx, cy, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    RECT frame = {0, 0, 0, 0};
    AdjustWindowRect(&frame, 0xCF0000, FALSE);
    const int w = cx - (frame.right - frame.left), h = cy - (frame.bottom - frame.top);
    MONITORINFO mi = {sizeof mi};
    GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    const RECT& mon = mi.rcMonitor;
    const RECT& work = mi.rcWork;
    if (w >= mon.right - mon.left && h >= mon.bottom - mon.top) {
        SetWindowLongA(wnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        dslog::Write("Display: %dx%d borderless over the monitor (%ldx%ld)", w, h, mon.right - mon.left,
                     mon.bottom - mon.top);
        const BOOL ok = SetWindowPos(wnd, HWND_NOTOPMOST, mon.left, mon.top, mon.right - mon.left, mon.bottom - mon.top,
                                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        BringToFront(wnd, "borderless placement");
        return ok;
    }
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE;
    SetWindowLongA(wnd, GWL_STYLE, style);
    int cw = w, ch = h;
    RECT r = {0, 0, cw, ch};
    AdjustWindowRect(&r, style, FALSE);
    const int availW = work.right - work.left, availH = work.bottom - work.top;
    if (r.right - r.left > availW || r.bottom - r.top > availH) {   // keep the aspect, fit the work area
        const int fw = (r.right - r.left) - cw, fh = (r.bottom - r.top) - ch;
        const double s = std::min(double(availW - fw) / cw, double(availH - fh) / ch);
        cw = static_cast<int>(cw * s);
        ch = static_cast<int>(ch * s);
        r = {0, 0, cw, ch};
        AdjustWindowRect(&r, style, FALSE);
    }
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    dslog::Write("Display: %dx%d windowed, client %dx%d", w, h, cw, ch);
    const BOOL ok = SetWindowPos(wnd, HWND_NOTOPMOST, work.left + (availW - ww) / 2, work.top + (availH - wh) / 2, ww,
                                 wh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    BringToFront(wnd, "windowed placement");
    return ok;
}

// The mouse poll's ScreenToClient: client coordinates scaled to the back buffer (a shrunk window / borderless window
// at a smaller mode), then into the GUI's virtual screen (menu / HUD scaling). Same as ScreenToClient when neither
// applies. Installed in every display mode.
BOOL WINAPI ScreenToGame(HWND wnd, POINT* pt) {
    const BOOL ok = ScreenToClient(wnd, pt);
    int bw = 0, bh = 0;
    RECT c;
    if (ok && pt && BackBuffer(bw, bh) && GetClientRect(wnd, &c) && c.right > 0 && c.bottom > 0 &&
        (c.right != bw || c.bottom != bh)) {
        pt->x = MulDiv(pt->x, bw, c.right);
        pt->y = MulDiv(pt->y, bh, c.bottom);
    }
    if (ok && pt) features::GuiToVirtual(pt->x, pt->y);   // the GUI's own coordinates (guiscale.cpp)
    return ok;
}
}  // namespace

// The device exists (layer's PSE_MSG_DEVICE_CREATED, tgefix.cpp): a fullscreen device only gets the screen while the
// window is in front - check again (something may have taken the front while the device was made).
// What decides is the game's own active flag [0x7DC2FC] (WM_ACTIVATE): 0 = the loop draws nothing and a fullscreen device
// has no screen (a black window over the desktop - seen when VS Code took the front while the device was made). An
// overlay may hold the "foreground" while the game is active and fullscreen (AMD's amdow.exe "Default IME" window) -
// harmless, left alone. So for 5 s, while the game is NOT active and the player hasn't used the keyboard / mouse, a
// thread puts it in front (one Alt tap at most).
constexpr uint32_t kActiveFlag = 0x7DC2FC;
DWORD WINAPI KeepInFront(void* wndParam) {
    HWND wnd = static_cast<HWND>(wndParam);
    LASTINPUTINFO start = {sizeof start};
    GetLastInputInfo(&start);
    bool tapped = false;
    for (int i = 0; i < 25; ++i) {
        Sleep(200);
        if (!IsWindow(wnd) || Read32(kActiveFlag) != 0) return 0;
        LASTINPUTINFO now = {sizeof now};
        if (GetLastInputInfo(&now) && now.dwTime != start.dwTime) return 0;   // the player's own doing
        BringToFront(wnd, "game inactive after the device", !tapped);
        tapped = true;
        GetLastInputInfo(&start);   // (the Alt tap counts as input)
    }
    return 0;
}

void features::DisplayDeviceCreated() {
    HWND wnd = *reinterpret_cast<HWND*>(0x7DC238);
    // Fullscreen: the start-up placement sized the window from the mode the device had before the launcher's choice was
    // applied (800 x 600 -> 816 x 639), and D3D8 on current Windows presents into the window - a bigger mode showed only
    // its top-left 800 x 600 over the desktop. The window now covers its monitor at the back buffer's size.
    int bw = 0, bh = 0;
    const uint32_t r = Read32(kRenderer);
    const bool windowedDevice = r && Read32(r + 0x84 + 0x1C) != 0;   // D3DPRESENT_PARAMETERS.Windowed
    if (wnd && !windowedDevice && BackBuffer(bw, bh)) {
        MONITORINFO mi = {sizeof mi};
        GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
        RECT now;
        GetWindowRect(wnd, &now);
        if (now.left != mi.rcMonitor.left || now.top != mi.rcMonitor.top || now.right - now.left != bw ||
            now.bottom - now.top != bh) {
            SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, bw, bh, SWP_SHOWWINDOW | SWP_NOACTIVATE);
            dslog::Write("Display: fullscreen window %ldx%ld -> %dx%d at %ld,%ld", now.right - now.left,
                         now.bottom - now.top, bw, bh, mi.rcMonitor.left, mi.rcMonitor.top);
        }
    }
    BringToFront(wnd, "device created");
    static bool started = false;
    if (!started && wnd && !features::Background()) {
        started = true;
        if (HANDLE t = CreateThread(nullptr, 0, &KeepInFront, wnd, 0, nullptr)) CloseHandle(t);
    }
}

void features::ApplyDisplay() {
    // the dialog wrapper is how windowed mode is switched on; the other hooks go in in every mode and act only while
    // windowed (the launcher's OK may switch it on after start-up)
    if (!patch::HookSlot(kSettingsProcPush, reinterpret_cast<const void*>(&SettingsProc),
                         reinterpret_cast<void**>(&g_settingsProc), kSettingsProc))
        dslog::Write("[fail] Display: Settings dialog not recognised - no Windowed box");
    const bool style = patch::WriteJump(kStyleCheck, reinterpret_cast<const void*>(&StyleCheckStub),
                                        sizeof kStyleCheckOrig, kStyleCheckOrig);
    const bool place = patch::HookIndirectCall(kPlaceCall, reinterpret_cast<const void*>(&PlaceWindow), kIatSetWindowPos) &&
                       patch::HookIndirectCall(kFullscreenPlaceCall, reinterpret_cast<const void*>(&PlaceFullscreen),
                                               kIatSetWindowPos);
    // mov ebx, [ScreenToClient] -> mov ebx, ScreenToGame; nop (same as ScreenToClient while the sizes agree)
    static uint8_t load[6] = {0xBB, 0, 0, 0, 0, 0x90};
    const uint32_t fn = reinterpret_cast<uint32_t>(&ScreenToGame);
    memcpy(load + 1, &fn, 4);
    static const BytePatch cursor[] = {{DS_PATCH_NAME("Mouse poll: client -> back buffer"), kCursorLoad, kCursorLoadOrig,
                                        load, sizeof load, false}};
    const bool mouse = patch::ApplyGroup("Display: mouse", cursor);
    if (!style || !place || !mouse)
        dslog::Write("[fail] Display: style check %d, placement %d, mouse %d - windowed mode may misbehave", style, place,
                     mouse);
    SetWindowed(Get().windowed || Background());   // dev test runs: always a windowed device (the setting unchanged)
}

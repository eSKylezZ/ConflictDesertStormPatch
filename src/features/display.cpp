// Display mode: fullscreen (the game's only mode), windowed, or borderless window.
//
// The engine is built on the DirectX sample framework, which still creates a windowed device when the adapter's
// fullscreen flag (+0x66c8) is 0 (FUN_00542ac0: back buffer in the desktop format, W x H of the chosen mode). The
// launcher's OK handler FUN_00448570 always writes 1 there (0x44874A) and dialog 108 hides its own "Windowed"
// checkbox (id 1025), so the mode was fullscreen-only. What we change:
//   0x448750  immediate of `mov [ebp+0x66c8], 1` -> 0 for the windowed modes
//   0x411569  call [CreateWindowExA] (FUN_00411470) -> CreateGameWindow: the window is created per-monitor DPI
//             aware, so a 1920x1080 window is 1920x1080 pixels on a 150% desktop (not bitmap-stretched)
//   0x40B897  call [SetWindowPos] after the launcher (window sized to the mode, style 0x14800000) -> PlaceWindow:
//             windowed = caption + minimize box, client W x H centred on the adapter's monitor (scaled down to fit
//             its work area); borderless = popup covering that monitor (D3D stretches the back buffer to it)
//   0x4116AD  message loop: every frame forced the style back to 0x14800000 -> StyleCheck (only in fullscreen)
//   0x4112B0  ClipCursor to the window rect inset by 4 px while active -> ClipToWindow (client area if windowed)
//   0x5386EA  menu cursor = GetCursorPos + ScreenToClient in back-buffer pixels -> ScreenToGame scales the client
//             position when the client area is not the back-buffer size (borderless / shrunk window)
// The mode is taken when the window is placed (after the launcher) and fixed for the session.
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kFullscreenImm = 0x448750;    // mov dword ptr [ebp+0x66c8], <imm32> at 0x44874A
constexpr uint32_t kCreateWindowCall = 0x411569;
constexpr uint32_t kPlaceWindowCall = 0x40B897;
constexpr uint32_t kStyleCheck = 0x4116AD, kStyleCheckEnd = 0x4116EC;
constexpr uint32_t kClipCursorFn = 0x4112B0;
constexpr uint32_t kScreenToClientLoad = 0x5386EA;  // mov ebp, [ScreenToClient]
constexpr uint32_t kIatCreateWindowExA = 0x5D8248, kIatSetWindowPos = 0x5D82CC, kIatScreenToClient = 0x5D8220;
constexpr uint32_t kRenderer = 0x63C924;  // W +0x40688, H +0x4068c, HWND +0x14, adapter +0xe0, IDirect3D8* +0x4052c
constexpr uint32_t kGameWindow = 0x606A60;
constexpr uint32_t kClipRect = 0x606944;     // RECT the game clips the cursor to
constexpr uint32_t kShowCursorFn = 0x411240; // (int show): hides / shows the Windows cursor
constexpr LONG kFullscreenStyle = 0x14800000;
constexpr LONG kWindowedStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE | WS_CLIPSIBLINGS;
constexpr LONG kBorderlessStyle = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS;

using ShowCursorFn = void(__cdecl*)(int);
const DPI_AWARENESS_CONTEXT kPerMonitorV2 = reinterpret_cast<DPI_AWARENESS_CONTEXT>(-4);

features::DisplayMode g_mode = features::DisplayMode::Fullscreen;  // fixed once the window is placed
uint8_t* g_renderer = nullptr;  // esi at the SetWindowPos call (the renderer, before 0x63C924 may be set)
bool g_hooked = false;

template <class T>
T& At(uint8_t* base, uint32_t offset) {
    return *reinterpret_cast<T*>(base + offset);
}

uint8_t* Renderer() {
    auto r = *reinterpret_cast<uint8_t**>(kRenderer);
    return r ? r : g_renderer;
}

// Per-monitor DPI awareness for the calling thread (Windows 10 1607+; no-op before).
DPI_AWARENESS_CONTEXT SetThreadDpi(DPI_AWARENESS_CONTEXT ctx) {
    using Fn = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    static auto fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA("user32.dll"), "SetThreadDpiAwarenessContext"));
    return fn ? fn(ctx) : nullptr;
}

void AdjustForDpi(RECT* r, LONG style, HWND wnd) {
    using DpiFn = UINT(WINAPI*)(HWND);
    using AdjustFn = BOOL(WINAPI*)(RECT*, DWORD, BOOL, DWORD, UINT);
    HMODULE user32 = GetModuleHandleA("user32.dll");
    static auto dpiFn = reinterpret_cast<DpiFn>(GetProcAddress(user32, "GetDpiForWindow"));
    static auto adjustFn = reinterpret_cast<AdjustFn>(GetProcAddress(user32, "AdjustWindowRectExForDpi"));
    if (dpiFn && adjustFn)
        adjustFn(r, style, FALSE, 0, dpiFn(wnd));
    else
        AdjustWindowRect(r, style, FALSE);
}

// Monitor of the adapter the game renders on (IDirect3D8::GetAdapterMonitor, vtable +0x38).
HMONITOR GameMonitor(HWND wnd) {
    if (uint8_t* r = Renderer()) {
        auto d3d = At<void**>(r, 0x4052C);
        if (d3d) {
            using Fn = HMONITOR(__stdcall*)(void*, UINT);
            HMONITOR m = reinterpret_cast<Fn>((*reinterpret_cast<void***>(d3d))[14])(d3d, At<UINT>(r, 0xE0));
            if (m) return m;
        }
    }
    return MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY);
}

HWND WINAPI CreateGameWindow(DWORD exStyle, LPCSTR cls, LPCSTR title, DWORD style, int x, int y, int w, int h,
                             HWND parent, HMENU menu, HINSTANCE inst, LPVOID param) {
    DPI_AWARENESS_CONTEXT old = SetThreadDpi(kPerMonitorV2);
    HWND wnd = CreateWindowExA(exStyle, cls, title, style, x, y, w, h, parent, menu, inst, param);
    if (old) SetThreadDpi(old);  // the launcher dialogs stay as they were (system-scaled)
    return wnd;
}

BOOL WINAPI PlaceWindow(HWND wnd, HWND after, int x, int y, int cx, int cy, UINT flags) {
    g_mode = static_cast<features::DisplayMode>(settings::Get().displayMode);
    // cx/cy = the mode size run through AdjustWindowRect(0xcf0000) by the game; undo that in its DPI context.
    RECT frame{};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    const int w = cx - (frame.right - frame.left), h = cy - (frame.bottom - frame.top);
    SetThreadDpi(kPerMonitorV2);  // the game thread from here on: real pixels for the window, cursor, clipping

    if (g_mode == features::DisplayMode::Fullscreen || w <= 0 || h <= 0) {
        g_mode = features::DisplayMode::Fullscreen;
        return SetWindowPos(wnd, after, x, y, cx, cy, flags);
    }

    MONITORINFO mi{sizeof mi};
    GetMonitorInfoA(GameMonitor(wnd), &mi);
    SetWindowTextA(wnd, "Conflict: Desert Storm");
    RECT r;
    if (g_mode == features::DisplayMode::Borderless) {
        SetWindowLongA(wnd, GWL_STYLE, kBorderlessStyle);
        r = mi.rcMonitor;
    } else {
        SetWindowLongA(wnd, GWL_STYLE, kWindowedStyle);
        const RECT& work = mi.rcWork;
        int cw = w, ch = h;
        // Shrink (keeping the aspect ratio) until the framed window fits the work area.
        for (int i = 0; i < 2; ++i) {
            RECT f{0, 0, cw, ch};
            AdjustForDpi(&f, kWindowedStyle, wnd);
            const int fw = f.right - f.left, fh = f.bottom - f.top;
            const int aw = work.right - work.left, ah = work.bottom - work.top;
            if (fw <= aw && fh <= ah) break;
            const double s = std::min(static_cast<double>(aw - (fw - cw)) / cw, static_cast<double>(ah - (fh - ch)) / ch);
            cw = static_cast<int>(cw * s);
            ch = static_cast<int>(ch * s);
        }
        r = {0, 0, cw, ch};
        AdjustForDpi(&r, kWindowedStyle, wnd);
        OffsetRect(&r, work.left + ((work.right - work.left) - (r.right - r.left)) / 2 - r.left,
                   work.top + ((work.bottom - work.top) - (r.bottom - r.top)) / 2 - r.top);
        if (r.top < work.top) OffsetRect(&r, 0, work.top - r.top);
    }
    dslog::Write("Display: %s %dx%d, window %ld,%ld %ldx%ld", g_mode == features::DisplayMode::Borderless ? "borderless" : "windowed",
                 w, h, r.left, r.top, r.right - r.left, r.bottom - r.top);
    BOOL ok = SetWindowPos(wnd, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top,
                           SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOCOPYBITS);
    SetForegroundWindow(wnd);
    return ok;
}

// Called at 0x40B897 with the renderer in esi; keeps it for GameMonitor (0x63C924 may not be set yet).
__declspec(naked) void PlaceWindowStub() {
    __asm {
        mov g_renderer, esi
        jmp PlaceWindow
    }
}

// Replaces 0x4116AD..0x4116EC in the message loop (runs each active frame before the frame dispatch).
void __cdecl StyleCheck() {
    if (g_mode != features::DisplayMode::Fullscreen) return;
    HWND wnd = At<HWND>(Renderer(), 0x14);
    if (GetWindowLongA(wnd, GWL_STYLE) != kFullscreenStyle) {
        SetWindowLongA(wnd, GWL_STYLE, kFullscreenStyle);
        reinterpret_cast<ShowCursorFn>(kShowCursorFn)(0);
    }
}

// Replaces FUN_004112B0 (called each active frame and on move / size / focus).
void __cdecl ClipToWindow() {
    HWND wnd = *reinterpret_cast<HWND*>(kGameWindow);
    RECT& r = *reinterpret_cast<RECT*>(kClipRect);
    if (g_mode == features::DisplayMode::Fullscreen) {
        GetWindowRect(wnd, &r);
        r.left += 4;
        r.right -= 4;
        r.bottom -= 4;
    } else {
        if (IsIconic(wnd) || !GetClientRect(wnd, &r)) return;
        MapWindowPoints(wnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    }
    ClipCursor(&r);
}

BOOL WINAPI ScreenToGame(HWND wnd, POINT* pt) {
    BOOL ok = ScreenToClient(wnd, pt);
    uint8_t* r = Renderer();
    if (!ok || g_mode == features::DisplayMode::Fullscreen || !r) return ok;
    RECT c;
    const int bw = At<int>(r, 0x40688), bh = At<int>(r, 0x4068C);
    if (GetClientRect(wnd, &c) && c.right > 0 && c.bottom > 0 && (c.right != bw || c.bottom != bh)) {
        pt->x = MulDiv(pt->x, bw, c.right);
        pt->y = MulDiv(pt->y, bh, c.bottom);
    }
    return ok;
}

bool WriteCallJump(uint32_t site, const void* fn, uint32_t jumpTo) {
    uint8_t code[10] = {0xE8, 0, 0, 0, 0, 0xE9};
    const int32_t call = static_cast<int32_t>(reinterpret_cast<uint32_t>(fn) - (site + 5));
    const int32_t jump = static_cast<int32_t>(jumpTo - (site + 10));
    std::memcpy(code + 1, &call, 4);
    std::memcpy(code + 6, &jump, 4);
    return patch::Write(site, code, sizeof code);
}

bool InstallHooks() {
    static const uint8_t kFlagMov[] = {0xC7, 0x85, 0xC8, 0x66, 0x00, 0x00};           // mov [ebp+0x66c8], imm32
    static const uint8_t kStyleOrig[] = {0x8B, 0x15, 0x24, 0xC9, 0x63, 0x00, 0x6A, 0xF0, 0x8B, 0x42};
    static const uint8_t kClipOrig[] = {0xA1, 0x60, 0x6A, 0x60, 0x00};                 // mov eax, [0x606a60]
    static const uint8_t kLoadOrig[] = {0x8B, 0x2D, 0x20, 0x82, 0x5D, 0x00};           // mov ebp, [ScreenToClient]
    static const uint8_t kCreateOrig[] = {0xFF, 0x15, 0x48, 0x82, 0x5D, 0x00};         // call [CreateWindowExA]
    static const uint8_t kPlaceOrig[] = {0xFF, 0x15, 0xCC, 0x82, 0x5D, 0x00};          // call [SetWindowPos]
    uint8_t load[6] = {0xBD, 0, 0, 0, 0, 0x90};                                        // mov ebp, imm32; nop
    const uint32_t screenToGame = reinterpret_cast<uint32_t>(&ScreenToGame);
    std::memcpy(load + 1, &screenToGame, 4);
    if (!patch::Matches(kFullscreenImm - 6, kFlagMov, sizeof kFlagMov) ||
        !patch::Matches(kStyleCheck, kStyleOrig, sizeof kStyleOrig) ||
        !patch::Matches(kClipCursorFn, kClipOrig, sizeof kClipOrig) ||
        !patch::Matches(kScreenToClientLoad, kLoadOrig, sizeof kLoadOrig) ||
        !patch::Matches(kCreateWindowCall, kCreateOrig, sizeof kCreateOrig) ||
        !patch::Matches(kPlaceWindowCall, kPlaceOrig, sizeof kPlaceOrig)) {
        dslog::Write("[fail] Display mode: unexpected bytes - different exe version? Not applied.");
        return false;
    }
    return patch::HookIndirectCall(kCreateWindowCall, &CreateGameWindow, kIatCreateWindowExA) &&
           patch::HookIndirectCall(kPlaceWindowCall, &PlaceWindowStub, kIatSetWindowPos) &&
           WriteCallJump(kStyleCheck, &StyleCheck, kStyleCheckEnd) && patch::WriteJump(kClipCursorFn, &ClipToWindow) &&
           patch::Write(kScreenToClientLoad, load, sizeof load);
}
}  // namespace

void features::ApplyDisplay() {
    if (!g_hooked) {
        g_hooked = InstallHooks();
        if (!g_hooked) return;
        dslog::Write("[ok]   Display mode: window hooks");
    }
    // Read by the launcher's OK handler, after the settings dialog may have changed the mode.
    const uint32_t fullscreen = settings::Get().displayMode == static_cast<uint32_t>(DisplayMode::Fullscreen);
    patch::WriteValue(kFullscreenImm, fullscreen);
}

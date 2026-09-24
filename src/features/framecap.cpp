// Frame limiter. The message loop FUN_004115b0 calls the per-frame dispatcher FUN_0040de00 (update + render +
// present) at 0x4116EC; that call goes through Frame(), which waits until the next frame is due.
// Target = min(FpsCap, monitor refresh rate): unthrottled, the game runs at 1000+ fps, where even the fixed
// millisecond clock rounds some frame times to 0 and the game speeds up again.
#include <windows.h>
#include <timeapi.h>

#include <algorithm>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {
constexpr uint32_t kFrameCallSite = 0x4116EC;
constexpr uint32_t kFrameDispatch = 0x40DE00;
constexpr uint32_t kGameWindow = 0x606A60;  // HWND global (created in FUN_00411470)

using FrameFn = void(__cdecl*)();
const FrameFn g_dispatch = reinterpret_cast<FrameFn>(kFrameDispatch);

LONGLONG g_freq = 0;
LONGLONG g_next = 0;
LONGLONG g_period = 0;       // 0 = unlimited
LONGLONG g_lastRefreshCheck = 0;
uint32_t g_targetFps = 0;
HANDLE g_timer = nullptr;
bool g_started = false;

LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

uint32_t MonitorRefresh() {
    HWND wnd = *reinterpret_cast<HWND*>(kGameWindow);
    MONITORINFOEXA mi{};
    mi.cbSize = sizeof mi;
    const char* device = nullptr;
    if (wnd && GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) device = mi.szDevice;
    DEVMODEA dm{};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsA(device, ENUM_CURRENT_SETTINGS, &dm)) return 0;
    return dm.dmDisplayFrequency > 1 ? dm.dmDisplayFrequency : 0;  // 0/1 = "hardware default"
}

void UpdateTarget() {
    const auto& s = settings::Get();
    uint32_t fps = s.fpsCap;
    if (s.fpsCapToRefresh) {
        uint32_t hz = MonitorRefresh();
        if (hz >= 30) fps = fps ? std::min(fps, hz) : hz;
    }
    if (fps != g_targetFps) {
        g_targetFps = fps;
        g_period = fps ? g_freq / fps : 0;
        g_next = 0;
        dslog::Write("Frame cap: %u fps%s", fps, fps ? "" : " (unlimited)");
    }
}

void Start() {
    g_started = true;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_freq = f.QuadPart;
    timeBeginPeriod(1);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    dslog::Write("Frame cap: %s timer", g_timer ? "high-resolution" : "Sleep(1)");
}

// Sleeps for most of the remaining time, then spins for the last ~1 ms for accuracy.
void WaitUntil(LONGLONG deadline) {
    const LONGLONG spin = g_freq / 1000;
    for (LONGLONG now = Now(); now < deadline; now = Now()) {
        LONGLONG left = deadline - now;
        if (left > spin * 2) {
            if (g_timer) {
                LARGE_INTEGER due;
                due.QuadPart = -((left - spin) * 10'000'000 / g_freq);  // relative, 100 ns units
                SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(g_timer, INFINITE);
            } else {
                Sleep(1);
            }
        } else {
            YieldProcessor();
        }
    }
}

void __cdecl Frame() {
    if (!g_started) Start();
    LONGLONG now = Now();
    if (g_lastRefreshCheck == 0 || now - g_lastRefreshCheck > g_freq * 2) {  // monitor / mode can change (alt-tab, window moved)
        g_lastRefreshCheck = now;
        UpdateTarget();
    }
    if (g_period) {
        if (g_next == 0 || now >= g_next) {
            g_next = now + g_period;  // slow frame (or first one): no wait, no catch-up burst
        } else {
            WaitUntil(g_next);
            g_next += g_period;
        }
    }
    features::OnFrame();
    g_dispatch();
}
}  // namespace

// The hook stays installed even with no cap (Frame() then just calls through), so the cap can change live.
void features::ApplyFrameCap() {
    static bool hooked = false;
    if (!hooked && patch::HookCall(kFrameCallSite, reinterpret_cast<const void*>(&Frame), kFrameDispatch)) {
        hooked = true;
        dslog::Write("[ok]   Frame cap: hooked per-frame call at 0x%08X", kFrameCallSite);
    }
    g_lastRefreshCheck = 0;  // re-evaluate the target on the next frame
}

// Frame cap (tgefix.framecap): with nothing slowing it down the game runs uncapped - ~1400 fps in the first mission on
// the dev PC (measured 2026-10-24 with Frida on Present; a d3d8.dll call logger in the game folder had kept it at
// ~19 fps). The game's timer is 64-bit integer QPC maths (0x4D1940 / 0x4CF990 + 64-bit divide), so DS1's float-clock
// turbo bug isn't there, but frame rates in the thousands mean whole-ms frame steps of 0-1 ms. Same as DS1's framecap:
// on every PSE_EVENT_FRAME (the layer's frame hook, before the game's update + render) wait until the next frame is due.
// Target = FpsCap (30..240), or 0 = the refresh rate of the monitor the game window is on (re-checked every 2 s), never
// above 240. A high-resolution waitable timer sleeps to 1 ms before the deadline, the rest is spun; a slow frame
// doesn't cause a catch-up burst.
#include <windows.h>

#include <algorithm>

#include "core/log.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "loader.h"

namespace {
constexpr uint32_t kGameWindow = 0x7DC238;
constexpr int kMaxFps = 240;

LARGE_INTEGER g_freq = {};
LONGLONG g_next = 0;
int g_target = 0;
DWORD g_lastCheck = 0;
HANDLE g_timer = nullptr;

int MonitorRefresh() {
    HWND wnd = *reinterpret_cast<HWND*>(kGameWindow);
    MONITORINFOEXA mi = {};
    mi.cbSize = sizeof mi;
    if (!GetMonitorInfoA(MonitorFromWindow(wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return 60;
    DEVMODEA dm = {};
    dm.dmSize = sizeof dm;
    if (!EnumDisplaySettingsA(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) || dm.dmDisplayFrequency <= 1) return 60;
    return static_cast<int>(dm.dmDisplayFrequency);
}

int Target() {
    const DWORD now = GetTickCount();
    if (!g_target || now - g_lastCheck > 2000) {
        g_lastCheck = now;
        const int set = static_cast<int>(features::Get().fpsCap);
        const int t = std::clamp(set > 0 ? set : MonitorRefresh(), 30, kMaxFps);
        if (t != g_target) dslog::Write("Frame cap: %d fps%s", t, set > 0 ? "" : " (monitor refresh)");
        g_target = t;
    }
    return g_target;
}

int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const LONGLONG period = g_freq.QuadPart / Target();
    if (g_next == 0 || now.QuadPart > g_next + period) {   // first frame, or late: no catch-up
        g_next = now.QuadPart + period;
        return 0;
    }
    const LONGLONG wait = g_next - now.QuadPart;
    const LONGLONG ms = wait * 1000 / g_freq.QuadPart;
    if (ms > 1 && g_timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>((ms - 1) * 10000);   // 100 ns units, relative
        if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(g_timer, 50);
    }
    do QueryPerformanceCounter(&now);
    while (now.QuadPart < g_next);
    g_next += period;
    return 0;
}
}  // namespace

void features::ApplyFrameCap() {
    if (!Get().frameCap) {
        dslog::Write("[off]  Frame cap: off (FrameCap)");
        return;
    }
    QueryPerformanceFrequency(&g_freq);
    g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!g_timer) g_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);   // (Windows before 10 1803)
    const PSEMessagingInterface* m = pse::MessagingInterface();
    if (!m || !m->Subscribe(pse_host::Find("tgefix.framecap"), PSE_EVENT_FRAME, &OnFrame, nullptr, 0)) {
        dslog::Write("[fail] Frame cap: no frame event");
        return;
    }
    dslog::Write("[ok]   Frame cap: on (%s)", Get().fpsCap ? "FpsCap" : "monitor refresh, max 240");
}

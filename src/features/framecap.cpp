// Frame limiter. The message loop FUN_004115b0 calls the per-frame dispatcher FUN_0040de00 (update + render +
// present) at 0x4116EC; that call goes through Frame(), which waits until the next frame is due.
// Target = min(FpsCap, monitor refresh rate): unthrottled, the game runs at 1000+ fps, where even the fixed
// millisecond clock rounds some frame times to 0 and the game speeds up again.
#include <windows.h>

#include <cstdio>
#include <timeapi.h>

#include <algorithm>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/framerec.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace {
constexpr uint32_t kFrameCallSite = 0x4116EC;
constexpr uint32_t kFrameDispatch = 0x40DE00;
constexpr uint32_t kGameWindow = 0x606A60;  // HWND global (created in FUN_00411470)
constexpr uint32_t kMaxFps = 240;           // the most the game runs at (whole-ms game clock; high-refresh monitors)

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

bool g_byVsync = false;  // cap left to V-Sync
uint32_t g_monitorHz = 0;
uint32_t g_vsyncSeen = 0xFFFFFFFF;  // (VsyncFps, VsyncProbeFps) the target was last computed for

void UpdateTarget() {
    const auto& s = settings::Get();
    uint32_t fps = s.fpsCap;
    if (s.fpsCapToRefresh) {
        const uint32_t hz = g_monitorHz;
        if (hz >= 30) fps = fps ? std::min(fps, hz) : hz;
    }
    // While V-Sync is being checked: twice the synced rate - clearly too fast if the driver ignores the interval,
    // but never uncapped.
    if (const uint32_t probe = features::VsyncProbeFps()) fps = probe;
    // "No limit" still stops at 500 fps: the game clock counts whole milliseconds, so near 1000 fps frames get
    // dt = 0 and the frame timer reuses the previous frame's dt (early return at 0x4BA5C4) - the game speeds up.
    const bool unlimited = fps == 0;
    fps = unlimited ? kMaxFps : std::min(fps, features::VsyncProbeFps() ? 500u : kMaxFps);
    // V-Sync paces frames itself (every n-th refresh, never above 240 or the limit): a software cap would only fight
    // it with uneven waits.
    const uint32_t synced = features::VsyncFps();
    const bool byVsync = synced >= 20;
    if (fps != g_targetFps || byVsync != g_byVsync) {
        g_targetFps = fps;
        g_byVsync = byVsync;
        g_period = byVsync ? 0 : g_freq / fps;
        g_next = 0;
        if (byVsync) dslog::Write("Frame cap: off - V-Sync paces frames at %u fps", synced);
        else dslog::Write("Frame cap: %u fps%s", fps, features::VsyncProbeFps() ? " (while checking V-Sync)" : "");
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

LONGLONG g_frameStart = 0;  // QPC when this frame's update + render began (after the cap wait)

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
    const uint32_t vsyncState = features::VsyncFps() << 16 | features::VsyncProbeFps();
    if (g_lastRefreshCheck == 0 || now - g_lastRefreshCheck > g_freq * 2) {  // monitor / mode can change (alt-tab, window moved)
        g_lastRefreshCheck = now;
        const double t0 = framerec::Now();
        g_monitorHz = MonitorRefresh();
        UpdateTarget();
        if (const double ms = framerec::Now() - t0; ms > 2.0) {
            char note[64];
            snprintf(note, sizeof note, "framecap refresh check %.1f ms", ms);
            framerec::Note(note);
        }
    } else if (vsyncState != g_vsyncSeen) {
        UpdateTarget();
    }
    g_vsyncSeen = vsyncState;
    if (g_period) {
        if (g_next == 0 || now >= g_next) {
            g_next = now + g_period;  // slow frame (or first one): no wait, no catch-up burst
        } else {
            WaitUntil(g_next);
            g_next += g_period;
        }
    }
    g_frameStart = Now();
    framerec::BeginFrame(double(g_frameStart - now) * 1000.0 / double(g_freq));
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

long long features::FrameStartQpc() { return g_frameStart; }

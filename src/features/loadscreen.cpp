// Faster level loads with V-Sync: the loading bar is drawn at most 20 times a second.
//
// The loading-bar callback FUN_0040ab50 (cdecl: name, total, noDraw) is passed to the environment loader FUN_004f8d00
// (push at 0x40B63D, in LoadEnvironment) and to the texture batch FUN_0054a3e0 (push at 0x40C88D, in the level init
// FUN_0040c090); they call it as they load the level's objects and for every second texture - 430 to 1,370 times a
// level, ~840 for the front end. Each call counts itself in 0x606A58 (the bar shows count / total, the total being the
// level's .FAS "Loading Bar" value; at the end of the load FUN_0040be70 hands the count to the .FAS writer
// FUN_004fb750) and, unless noDraw is set, draws the bar and presents the frame (FUN_00546be0). With V-Sync every
// present waits for the next refresh - our own wait when the driver ignores the D3D8 interval (WaitForVblanks,
// graphics.cpp), the driver's otherwise - so a load can't end sooner than calls / refresh rate: ~19 s for Mission 3
// at 60 Hz, ~8 s at 144 Hz, while the loading itself takes ~1.5 s on a current PC.
//
// The wrapper passes a call on as it is when 50 ms have passed since the last draw; otherwise it sets noDraw - the
// callback's own path that still counts the call, sets and restores the FPU control word and updates the sound
// (FUN_004124b0), and only leaves out the draw and the present. Always passed on as they are: the first call of a load
// (count 0: it sets up the loading picture) and the start-up logo slideshow ([0x60ED34] != 0: it advances the
// slideshow, which is waited for at the end of the load anyway).
#include <windows.h>

#include <cstdint>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kLoadBar = 0x40AB50;
constexpr uint32_t kPushSites[] = {0x40B63D, 0x40C88D};  // push imm32 0x0040AB50
constexpr uint32_t kSlideshow = 0x60ED34;               // int: first-boot logo slideshow runs in this load
constexpr uint32_t kCalls = 0x606A58;                   // int: bar callback calls in this load (0 = first)
constexpr double kMinIntervalMs = 50.0;

using LoadBarFn = void(__cdecl*)(const char*, int, int);

LARGE_INTEGER g_last{};
LONGLONG g_minInterval = 0;  // in performance-counter ticks
#ifndef DS_DIST
uint32_t g_drawn = 0, g_skipped = 0;
#endif

void __cdecl LoadBar(const char* name, int total, int noDraw) {
    const int calls = *reinterpret_cast<volatile int*>(kCalls);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
#ifndef DS_DIST
    if (calls == 0 && g_drawn + g_skipped) {
        dslog::Write("Loading bar: last load %u calls, %u drawn", g_drawn + g_skipped, g_drawn);
        g_drawn = g_skipped = 0;
    }
#endif
    const bool skip = !noDraw && calls != 0 && *reinterpret_cast<volatile int*>(kSlideshow) == 0 &&
                      now.QuadPart - g_last.QuadPart < g_minInterval;
    reinterpret_cast<LoadBarFn>(kLoadBar)(name, total, skip ? 1 : noDraw);
    if (!skip) QueryPerformanceCounter(&g_last);
#ifndef DS_DIST
    ++(skip ? g_skipped : g_drawn);
#endif
}
}  // namespace

void features::ApplyLoadScreen() {
    static bool applied = false;
    if (applied) return;
    applied = true;
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    g_minInterval = static_cast<LONGLONG>(kMinIntervalMs * freq.QuadPart / 1000.0);
    const uint8_t original[] = {0x68, 0x50, 0xAB, 0x40, 0x00};
    for (uint32_t site : kPushSites)
        if (!patch::Matches(site, original, sizeof original)) {
            dslog::Write("[fail] Loading bar: unexpected bytes at 0x%06X - not applied", site);
            return;
        }
    const uint32_t wrapper = reinterpret_cast<uint32_t>(&LoadBar);
    for (uint32_t site : kPushSites) patch::WriteValue(site + 1, wrapper);
    dslog::Write("[ok]   Loading bar: drawn at most every %.0f ms (fast loads with V-Sync)", kMinIntervalMs);
}

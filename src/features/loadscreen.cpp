// Faster level loads with V-Sync: the loading bar is redrawn at most 20 times a second.
//
// Level loads (FUN_0040c090) pass the loading-bar callback FUN_0040ab50 to the environment loader (push at 0x40B63D)
// and to the texture batch FUN_0054a3e0 (push at 0x40C88D), which calls it for every second texture - ~800 calls for
// a mission. Each call draws the bar and presents a frame (FUN_004124b0). With V-Sync on (fullscreen) every present
// waits for the next refresh, so a load that takes ~1.5 s of work showed the bar for 10-15 s (800 x 16.7 ms at
// 60 Hz) - the same going back to the main menu. On the old PCs the work per texture was slower than a refresh, so
// the wait hardly showed.
// The wrapper calls the original only when 50 ms have passed since its last draw. Always passed on: the first call
// of a load (counter 0x606A58 = 0: sets up the loading picture), calls with the third argument set, and the start-up
// logo slideshow ([0x60ED34] != 0: it advances the slideshow, which is waited for at the end of the load anyway).
// A skipped call only counts itself (0x606A58), as the original's slideshow branch does.
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

using LoadBarFn = void(__cdecl*)(int, int, int);

LARGE_INTEGER g_freq{}, g_last{};
uint32_t g_drawn = 0, g_skipped = 0;

void __cdecl LoadBar(int a, int b, int c) {
    auto& calls = *reinterpret_cast<volatile int*>(kCalls);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (calls == 0) {
#ifndef DS_DIST
        if (g_drawn + g_skipped)
            dslog::Write("Loading bar: last load %u draws, %u skipped", g_drawn, g_skipped);
#endif
        g_drawn = g_skipped = 0;
    }
    const double sinceMs = 1000.0 * static_cast<double>(now.QuadPart - g_last.QuadPart) / g_freq.QuadPart;
    if (calls != 0 && c == 0 && *reinterpret_cast<volatile int*>(kSlideshow) == 0 && sinceMs < kMinIntervalMs) {
        ++calls;
        ++g_skipped;
        return;
    }
    reinterpret_cast<LoadBarFn>(kLoadBar)(a, b, c);
    ++g_drawn;
    QueryPerformanceCounter(&g_last);
}
}  // namespace

void features::ApplyLoadScreen() {
    static bool applied = false;
    if (applied) return;
    applied = true;
    QueryPerformanceFrequency(&g_freq);
    const uint8_t original[] = {0x68, 0x50, 0xAB, 0x40, 0x00};
    for (uint32_t site : kPushSites)
        if (!patch::Matches(site, original, sizeof original)) {
            dslog::Write("[fail] Loading bar: unexpected bytes at 0x%06X - not applied", site);
            return;
        }
    const uint32_t wrapper = reinterpret_cast<uint32_t>(&LoadBar);
    for (uint32_t site : kPushSites) patch::WriteValue(site + 1, wrapper);
    dslog::Write("[ok]   Loading bar: redrawn at most every %.0f ms (fast loads with V-Sync)", kMinIntervalMs);
}

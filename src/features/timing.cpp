// Game clock FUN_004ba370 computed ms as QPC*1000/QPF on an x87 FPU that D3D8 leaves at 24-bit precision,
// so the clock moved in 8-64 ms steps on long uptimes and the frame timer reused the previous dt when it had
// not moved (turbo at high FPS). Replaced with exact integer math
// (QPC / f * 1000 + QPC % f * 1000 / f, f = QPF; the bytes are in scripts/patch_defs.py TIMING).
//
// Characters who never start falling above ~140 fps (e.g. the soldiers who stay in the helicopter in the
// opening film): the character mover FUN_0050DC20 decides a falling character has landed when this frame's
// vertical movement is at most 0.05 units (fcomp [0x5D857C] at 0x50E6EC). The first frame of a fall moves
// g*dt^2 = 981*dt^2, which is below 0.05 once frames are shorter than ~7.1 ms - so the character "lands" on the
// frame after stepping off, every time. That compare now reads g_landThreshold = 0.05 * min(1, (dt*120)^2):
// unchanged up to 120 fps, and above it the first falling frame keeps the same margin over the threshold as at
// 120 fps. It is refreshed from the current frame time by the timescale call at 0x50E177 (ecx = frame timer),
// which runs earlier in every mover call and still returns the timescale unchanged. The 0.05 constant itself is
// shared by 12 other sites, so only this operand is repointed.
//
// Micro-stutter: the frame timer FUN_004ba5b0 (fastcall, timer object; gameplay timer [0x63C958]) derives the frame
// time the game moves things by - float seconds at +0xA8, read by 99 callers through FUN_004ba480 - from the
// whole-millisecond clock: +0xC4 = raw ms since its last update, +0x14 = ftol(raw * timescale +0x28) clamped to 500,
// +0xA8 = +0x14 / units (+0x04, 1000) * global multiplier [0x5F7E64]. At 240 fps (4.17 ms frames) that is 4, 4, 4,
// 5 ... and often 6 / 2 or 3 ms (measured, Dev\FrameLog) - everything moves up to +-50 % unevenly from frame to frame.
// The function is wrapped: afterwards +0xA8 is recomputed from the exact QPC time since this timer's last update, the
// same way. Only when that agrees with the game's whole-ms value to within 2 ms - pauses (the game subtracts paused
// time) and hitches keep the game's value. The 30-frame ring (+0x2C + i*4, slot +0xA4) and its average (+0xAC) get the
// same value. The integer +0x14 / total +0x10 stay whole ms (they still add up to the same time).
#include <windows.h>

#include <cmath>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "generated/patch_tables.h"

namespace {
constexpr uint32_t kTimescaleCall = 0x50E177;    // call 0x4BA530 in FUN_0050DC20
constexpr uint32_t kTimescaleGetter = 0x4BA530;  // thiscall: float timer+0x28
constexpr uint32_t kLandCompare = 0x50E6EC;      // fcomp dword ptr [0x5D857C]
constexpr uint32_t kLandThreshold = 0x5D857C;    // 0.05
constexpr float kReferenceFps = 120.0f;

float g_landThreshold = 0.05f;

float __fastcall TimescaleAndLandThreshold(const uint8_t* timer) {
    const float dt = *reinterpret_cast<const float*>(timer + 0xA8);  // frame time, seconds
    const float scale = dt * kReferenceFps;
    g_landThreshold = *reinterpret_cast<const float*>(kLandThreshold) * (scale < 1.0f ? scale * scale : 1.0f);
    return *reinterpret_cast<const float*>(timer + 0x28);
}

// ---- exact frame time ----
constexpr uint32_t kFrameTimer = 0x4BA5B0;      // fastcall(timer): sub esp,8 / push ebx / push esi (5 bytes)
constexpr uint32_t kFrameTimerCont = 0x4BA5B5;
constexpr uint32_t kTimeMultiplier = 0x5F7E64;  // float, global time multiplier
constexpr uint8_t kFrameTimerEntry[] = {0x83, 0xEC, 0x08, 0x53, 0x56};

using TimerFn = void(__fastcall*)(uint8_t*);
TimerFn g_timerOriginal = nullptr;  // trampoline
LARGE_INTEGER g_qpf;

struct TimerState {
    const uint8_t* timer;
    LONGLONG last;  // QPC at its last update
};
TimerState g_timers[16];

void __fastcall FrameTimer(uint8_t* timer) {
    // Timed from the frame's start (after the frame-cap wait - where frames are paced) rather than from this call:
    // the timer is updated at a slightly different point of each frame, which alone gave 5.5 / 2.9 ms pairs.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const uint32_t clockBefore = *reinterpret_cast<uint32_t*>(timer + 0xD4);
    g_timerOriginal(timer);
    if (*reinterpret_cast<uint32_t*>(timer + 0xD4) == clockBefore) return;  // clock didn't move: nothing updated

    TimerState* st = nullptr;
    for (TimerState& t : g_timers)
        if (t.timer == timer || !t.timer) {
            st = &t;
            break;
        }
    if (!st) return;
    const LONGLONG last = st->timer ? st->last : 0;
    const LONGLONG start = features::FrameStartQpc();
    if (start && start != last) now.QuadPart = start;  // (a second update in the same frame: its own time)
    st->timer = timer, st->last = now.QuadPart;
    if (!last || !*reinterpret_cast<int*>(timer)) return;  // first update / timer stopped (dt 0)

    double rawMs = double(now.QuadPart - last) * 1000.0 / double(g_qpf.QuadPart);
    // V-Sync: frames are shown on a fixed grid of refreshes - the frame time is a whole number of those (the measured
    // value only adds the vertical-blank wake-up jitter, +-0.25 ms), so motion matches exactly what the screen shows.
    if (const uint32_t synced = features::VsyncFps()) {
        const double period = 1000.0 / synced, k = std::floor(rawMs / period + 0.5);
        if (k >= 1 && std::fabs(rawMs - k * period) < period * 0.3) rawMs = k * period;
    }
    const double gameRawMs = *reinterpret_cast<uint32_t*>(timer + 0xC4);
    if (std::fabs(rawMs - gameRawMs) > 2.0) return;  // pause / hitch: keep the game's value
    const double scaled = rawMs * *reinterpret_cast<float*>(timer + 0x28);
    const int units = *reinterpret_cast<int*>(timer + 4);
    if (scaled > 500.0 || units <= 0) return;
    const float old = *reinterpret_cast<float*>(timer + 0xA8);
    const float dt = static_cast<float>(scaled / units * *reinterpret_cast<float*>(kTimeMultiplier));
    *reinterpret_cast<float*>(timer + 0xA8) = dt;
    const uint32_t slot = *reinterpret_cast<uint32_t*>(timer + 0xA4);
    if (slot < 30) *reinterpret_cast<float*>(timer + 0x2C + slot * 4) = dt;
    *reinterpret_cast<float*>(timer + 0xAC) += (dt - old) / 30.0f;
}

bool ApplyExactFrameTime() {
    if (!patch::Matches(kFrameTimer, kFrameTimerEntry, sizeof kFrameTimerEntry)) {
        dslog::Write("[fail] Exact frame time: unexpected bytes at 0x%08X", kFrameTimer);
        return false;
    }
    QueryPerformanceFrequency(&g_qpf);
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    std::memcpy(tramp, kFrameTimerEntry, sizeof kFrameTimerEntry);
    tramp[5] = 0xE9;
    const int32_t rel = static_cast<int32_t>(kFrameTimerCont - (reinterpret_cast<uint32_t>(tramp) + 10));
    std::memcpy(tramp + 6, &rel, 4);
    g_timerOriginal = reinterpret_cast<TimerFn>(tramp);
    return patch::WriteJump(kFrameTimer, reinterpret_cast<const void*>(&FrameTimer));
}

bool ApplyLandingFix() {
    uint8_t fcomp[6] = {0xD8, 0x1D};
    std::memcpy(fcomp + 2, &kLandThreshold, 4);
    if (!patch::Matches(kLandCompare, fcomp, sizeof fcomp)) {
        dslog::Write("[fail] High-FPS landing fix: unexpected bytes at 0x%08X", kLandCompare);
        return false;
    }
    if (!patch::HookCall(kTimescaleCall, &TimescaleAndLandThreshold, kTimescaleGetter)) return false;
    const uint32_t operand = reinterpret_cast<uint32_t>(&g_landThreshold);
    return patch::WriteValue(kLandCompare + 2, operand);
}
}  // namespace

void features::ApplyTiming() {
    patch::ApplyGroup("Frame timing", gen::kTiming);
    static bool landing = false;
    if (!landing && (landing = ApplyLandingFix())) dslog::Write("[ok]   High-FPS landing fix (character mover)");
    static bool exact = false;
    if (!exact && (exact = ApplyExactFrameTime())) dslog::Write("[ok]   Exact frame time (frame timer wrapped)");
}

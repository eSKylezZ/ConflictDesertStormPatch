// Game clock FUN_004ba370 computed ms as QPC*1000/QPF on an x87 FPU that D3D8 leaves at 24-bit precision,
// so the clock moved in 8-64 ms steps on long uptimes and the frame timer reused the previous dt when it had
// not moved (turbo at high FPS). Replaced with exact integer math; see tools/build_timer.py.
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
}

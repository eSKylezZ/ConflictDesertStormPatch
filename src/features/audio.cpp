// Sounds got louder when aiming / zooming (scopes, binoculars).
//
// Own gunshots (the main complaint): the shot sound in FUN_004A1B80 (play call 0x4A1D8E) was passed no position
// while the shooter's input block has +0x318 = 1 (aiming) - a flat 2D sound at full level in both speakers instead
// of the positional shot at the muzzle. Byte patch (scripts/patch_defs.py AUDIO): always the positional branch.
//
// Listener: the sound manager [0x63C9AC] is updated once per
// frame by the render loop (FUN_0040EF70, call 0x40EF60 -> FUN_0055D8B0(count, positions, rotations)) with one
// listener per viewport: the viewport camera's position (camera manager FUN_004D8360(cam), call 0x40EEFA) and
// rotation (FUN_004D8390). Each sound's volume / pan comes from its distance to the nearest listener (software path
// FUN_0055EB90, hardware path FUN_0055DC20 -> IDirectSound3DListener::SetPosition). Aim mode (player object +8 = 2,
// entered by FUN_0044D6F0, left by FUN_0044DC70) moves the camera from behind the soldier to his eyes - measured
// ~4.9 m forward and ~0.9 m down - so every sound jumped that much closer, own gunfire the most.
//
// The position call is hooked: while a player aims, his listener keeps the third-person camera's place relative to
// the aim camera. On entering aim the listener stays at the last third-person position until the aim camera has
// settled; that gives the offset (horizontal distance behind + height), which is then applied behind the aim
// camera along the current view direction (so it follows walking and turning, and panning still follows the view).
// When aiming ends it stays there until the camera has swung back out. Only the camera is used - the soldier's
// position is not needed.
#include <windows.h>

#include <cmath>
#include <cstdint>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "generated/patch_tables.h"

namespace {
constexpr uint32_t kListenerPosCall = 0x40EEFA;  // call FUN_004D8360 in the render loop's listener gather
constexpr uint32_t kCameraPos = 0x4D8360;        // thiscall(camera manager, cam) -> float[3], y is up, units cm
constexpr uint32_t kCameraRot = 0x4D8390;        // thiscall(camera manager, cam) -> float[3] pitch, yaw, roll
constexpr uint32_t kPlayerTable = 0x606A68;      // player objects [4]: +4 camera index, +8 view mode
constexpr uint32_t kDegToRad1 = 0x5D8458;        // the angle factors FUN_0053F750 uses (rotation -> direction)
constexpr uint32_t kDegToRad2 = 0x5D8454;
constexpr int kViewAim = 2;
constexpr int kMaxCameras = 8;
constexpr float kMaxBehind = 1000.0f;            // larger jumps are cuts (cutscene / respawn), not the aim swing
constexpr float kSettleSpeed = 300.0f;           // cm/s: the aim swing is much faster, scoped walking slower
constexpr DWORD kSettleMinMs = 150;
constexpr DWORD kSettleTimeoutMs = 800;
constexpr DWORD kReturnTimeoutMs = 1500;

enum class Phase { Normal, Entering, Aiming, Returning };

struct ListenerState {
    Phase phase;
    float third[3];     // last third-person camera position
    float behind;       // third-person camera: horizontal distance behind the aim camera
    float height;       //                      and height above it
    float aimRef[3];    // last aim camera position (reference while swinging back out)
    float prev[3];      // camera position on the previous call
    LARGE_INTEGER prevTime;
    DWORD phaseStart;
    float pos[3];       // what we hand back to the caller
};
ListenerState g_state[kMaxCameras];

using VecFn = float*(__thiscall*)(void* mgr, int cam);

const uint8_t* PlayerOfCamera(int cam) {
    for (int k = 0; k < 4; ++k) {
        const uint8_t* p = reinterpret_cast<const uint8_t* const*>(kPlayerTable)[k];
        if (p && *reinterpret_cast<const int*>(p + 4) == cam) return p;
    }
    return nullptr;
}

void Copy(float* dst, const float* src) { dst[0] = src[0], dst[1] = src[1], dst[2] = src[2]; }
float Horizontal(const float* a, const float* b) {
    const float dx = a[0] - b[0], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dz * dz);
}

// behind / height applied to `ref` against the camera's current view direction (x = sin yaw, z = cos yaw).
float* PlaceBehind(ListenerState& st, void* mgr, int cam, const float* ref) {
    const float* rot = reinterpret_cast<VecFn>(kCameraRot)(mgr, cam);
    const float yaw = rot[1] * *reinterpret_cast<const float*>(kDegToRad1) * *reinterpret_cast<const float*>(kDegToRad2);
    st.pos[0] = ref[0] - std::sin(yaw) * st.behind;
    st.pos[1] = ref[1] + st.height;
    st.pos[2] = ref[2] - std::cos(yaw) * st.behind;
    return st.pos;
}

float* Listener(void* mgr, int cam, float* camPos, int mode) {
    ListenerState& st = g_state[cam];
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = st.prevTime.QuadPart ? float(now.QuadPart - st.prevTime.QuadPart) / float(freq.QuadPart) : 0;
    const float speed = dt > 0 ? std::sqrt((camPos[0] - st.prev[0]) * (camPos[0] - st.prev[0]) +
                                           (camPos[1] - st.prev[1]) * (camPos[1] - st.prev[1]) +
                                           (camPos[2] - st.prev[2]) * (camPos[2] - st.prev[2])) / dt
                           : 0;
    Copy(st.prev, camPos);
    st.prevTime = now;
    const DWORD ms = GetTickCount();
    const bool aiming = mode == kViewAim;

    switch (st.phase) {
    case Phase::Normal:
        if (!aiming) {
            Copy(st.third, camPos);
            return camPos;
        }
        st.phase = Phase::Entering;
        st.phaseStart = ms;
        [[fallthrough]];
    case Phase::Entering:
        if (!aiming) {
            st.phase = Phase::Normal;
            Copy(st.third, camPos);
            return camPos;
        }
        // The camera only starts to swing a frame or more after the mode changes, so give it kSettleMinMs first.
        if (ms - st.phaseStart < kSettleMinMs ||
            (dt > 0 && speed > kSettleSpeed && ms - st.phaseStart < kSettleTimeoutMs)) {
            Copy(st.pos, st.third);  // aim camera still swinging in: stay where the third-person camera was
            return st.pos;
        }
        st.behind = Horizontal(st.third, camPos);
        st.height = st.third[1] - camPos[1];
        if (st.behind > kMaxBehind || std::fabs(st.height) > kMaxBehind) st.behind = st.height = 0;
        st.phase = Phase::Aiming;
        [[fallthrough]];
    case Phase::Aiming:
        if (aiming) {
            Copy(st.aimRef, camPos);
            return PlaceBehind(st, mgr, cam, camPos);
        }
        st.phase = Phase::Returning;
        st.phaseStart = ms;
        [[fallthrough]];
    case Phase::Returning:
        if (aiming) {
            st.phase = Phase::Aiming;
            Copy(st.aimRef, camPos);
            return PlaceBehind(st, mgr, cam, camPos);
        }
        if (Horizontal(camPos, st.aimRef) < st.behind * 0.9f && ms - st.phaseStart < kReturnTimeoutMs)
            return PlaceBehind(st, mgr, cam, st.aimRef);
        st.phase = Phase::Normal;
        Copy(st.third, camPos);
        return camPos;
    }
    return camPos;
}

float* __fastcall ListenerPosition(void* mgr, void*, int cam) {
    float* camPos = reinterpret_cast<VecFn>(kCameraPos)(mgr, cam);
    if (cam < 0 || cam >= kMaxCameras) return camPos;
    const uint8_t* player = PlayerOfCamera(cam);
    const int mode = player ? *reinterpret_cast<const int*>(player + 8) : -1;
    return Listener(mgr, cam, camPos, mode);
}
}  // namespace

void features::ApplyAudio() {
    static bool done = false;
    if (done) return;
    patch::ApplyGroup("Audio: positional own gunshots", gen::kAudio);
    if ((done = patch::HookCall(kListenerPosCall, &ListenerPosition, kCameraPos)))
        dslog::Write("[ok]   Audio listener keeps the third-person place while aiming");
    else
        dslog::Write("[fail] Audio listener: unexpected bytes at 0x%08X", kListenerPosCall);
}

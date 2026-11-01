// Third-person camera (2026-11-01; research ConflictGamefiles\_analysis\DeniedOps_tools\thirdperson\THIRD_PERSON.md).
// Denied Ops is first person, but its camera manager already has a chase camera on a grunt (types 4 / 6, update
// 0x8AE070: about 1 m behind the head on the aim line, used after a soldier switch / scripted cameras and by mission
// scripts' "camera on unit"). The player's camera choice CLocalPlayer::SelectCamera 0x450970 ends, every frame, in
// SetCamera(7 = first person, ...) on its grunt; asked for 6 instead, the player gets the chase camera while every
// other camera (death, vehicles, mounted guns, ladders, scripted) stays the game's.
//   SetCamera 0x8A53C0 thiscall(manager, type, float blend seconds, target, zone, a, b, c) ret 0x1C - returns early
//     when that type + target are already active, so the swap is free every frame.
//   0x446700 thiscall(manager) "first-person-like camera" (types 7, 0x17, 0x16) - the crosshair / HUD target info,
//     mounted-gun handling AND the first-person weapon + hands model ask it. Answering yes for 6 brings the crosshair
//     and the soldier / ammo panels back but also draws the first-person rifle over the third-person view (seen
//     2026-11-01), so it is left alone; dev builds hook it with Dev\ThirdPersonCrosshair = 1.
// F7 toggles (with a 0.3 s blend, the game's own camera blend); a mod level's .level sidecar line "--@camera third"
// starts its levels in third person (DS1 levels ported onto DO). Setting FeatThirdPerson (default on).
// Not handled yet: the camera can clip into walls (the chase update has no ray), shots leave the muzzle along the
// grunt's aim (a little parallax against the screen centre), first-person-only sounds / muzzle flash, no crosshair /
// soldier panel in third person (0x446700 above).
#include <windows.h>

#include <cstdint>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kSetCamera = 0x8A53C0, kGetCameraType = 0x8A2DF0, kFirstPersonLike = 0x446700;
constexpr int kFirstPerson = 7, kChase = 6;
constexpr float kToggleBlend = 0.3f;

using SetCameraFn = int(__fastcall*)(void*, void*, int, float, void*, void*, int, int, int);
SetCameraFn g_nextSetCamera = nullptr;
bool g_third = false;
bool g_blendNext = false;
bool g_keyDown = false;
uint32_t g_generation = ~0u;

bool GameInFront() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

void Update() {
    const uint32_t gen = features::SceneGeneration();
    if (gen != g_generation) {   // a level loaded: its own start mode
        g_generation = gen;
        const bool third = !_stricmp(features::SceneLevelOption("camera").c_str(), "third");
        if (third != g_third) dslog::Write("Third person: level starts in %s person", third ? "third" : "first");
        g_third = third;
    }
    const bool down = GameInFront() && (GetAsyncKeyState(VK_F7) & 0x8000);
    if (down && !g_keyDown) {
        g_third = !g_third;
        g_blendNext = true;
        dslog::Write("Third person: %s (F7)", g_third ? "on" : "off");
    }
    g_keyDown = down;
}

int __fastcall SetCamera(void* mgr, void* edx, int type, float blend, void* target, void* zone, int a, int b, int c) {
    Update();
    if (type == kFirstPerson && g_third) type = kChase;
    if (g_blendNext && (type == kFirstPerson || type == kChase)) {
        if (blend < kToggleBlend) blend = kToggleBlend;
        g_blendNext = false;
    }
    return g_nextSetCamera(mgr, edx, type, blend, target, zone, a, b, c);
}

int __fastcall FirstPersonLike(void* mgr, void* /*edx*/) {
    const int t = reinterpret_cast<int(__fastcall*)(void*, void*)>(kGetCameraType)(mgr, nullptr);
    return t == kFirstPerson || t == 0x17 || t == 0x16 || (g_third && t == kChase);
}

void Apply() {
    constexpr uint8_t kSetCameraEntry[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF8};               // push ebp; mov ebp,esp; and esp,-8
    constexpr uint8_t kFirstPersonLikeEntry[] = {0x56, 0x8B, 0xF1, 0xE8, 0xE8, 0xC6, 0x45, 0x00};   // push esi; mov esi,ecx; call GetCameraType
    if (!patch::HookFunction(kSetCamera, reinterpret_cast<void*>(&SetCamera), reinterpret_cast<void**>(&g_nextSetCamera),
                             kSetCameraEntry, sizeof kSetCameraEntry)) {
        dslog::Write("[fail] Third person: unexpected camera code at 0x%08X - off", kSetCamera);
        return;
    }
#ifndef DS_DIST
    DWORD v = 0, size = sizeof v;   // Dev\ThirdPersonCrosshair = 1: hook 0x446700 too (crosshair + first-person rifle)
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DeniedOpsFix\\Dev", "ThirdPersonCrosshair", RRF_RT_REG_DWORD, nullptr, &v,
                 &size);
    if (v && !patch::HookFunction(kFirstPersonLike, reinterpret_cast<void*>(&FirstPersonLike), nullptr,
                                  kFirstPersonLikeEntry, sizeof kFirstPersonLikeEntry))
        dslog::Write("[fail] Third person: 0x%08X not recognised", kFirstPersonLike);
#else
    (void)&FirstPersonLike;
    (void)kFirstPersonLikeEntry;
#endif
    dslog::Write("[ok]   Third person: F7 toggles; levels with --@camera third start in it");
}
}  // namespace

void features::ApplyThirdPerson() {
    if (!Get().thirdPerson) {
        dslog::Write("[off]  Third person: off (FeatThirdPerson)");
        return;
    }
    AfterUnpack(&Apply);
}

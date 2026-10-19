// Vehicle grip for mod vehicles (.vehicle "grip = X", mods.cpp): a hovercraft that slides instead of gripping.
//
// Ground vehicles are MathEngine Karma bodies on car-wheel joints (FUN_00514a80, handling from the model's .DYN - the
// field list is in research/custom_units.md). The wheel / world contacts get their parameters per contact in the
// callback LAB_00514250 (registered for material pair [0x5FC1C0] x [0x643A38]): type 2 (2-D friction), slip = speed
// x the .DYN's slip factor (FUN_0058cc10 -> params +0x2C, option 0x20), softness (FUN_0058cb70 -> +0x18, option 4) -
// but never a friction (FUN_0058cbc0 -> +0x24, option 0x10; +0x28 the second direction), and Karma's default friction is
// unlimited: the tyres never slide, whatever the slip. Every branch of the callback ends in
//   0x514513  push edi (params); call 0x58CB70 (softness)      esi = the vehicle's physics object (0xEC4 B)
// -> the call goes to GripStub: softness as before, then for a vehicle whose model has a grip, friction = grip (both
// directions, option 0x10). Physics object +0xEB8 = its render instance, instance +0xC8 = the model's first object,
// Spin damping: the chassis body (physics +0xDE8) gets 0.2 / 0.2 from FUN_0058e440 (+0x204) / FUN_0058e460 (+0x208) at
// creation (Karma velocity dampings, measured 2026-10-15: +0x204 linear - top speed 62 -> 39 km/h at 3.0 - and +0x208
// angular - coasting spin 60 -> 48 deg/s, top speed unchanged; Dev\\HandlingSpinOffset = 0x204 for tests); written
// per contact while the vehicle touches the ground.
// object +0x1BC = the model name hash (FUN_004b9ab0, the lower-cased LFSR the model lookup FUN_004dd740 / FUN_004dd6e0
// compares with).
#include <windows.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kSoftCall = 0x514514, kSetSoftness = 0x58CB70, kNameHash = 0x4B9AB0;   // model lookup's hash (FUN_004dd740)

std::unordered_map<uint32_t, features::VehicleHandling> g_handling;   // model name hash -> grip / spin damping
uint32_t g_spinOffset = 0x208;   // chassis angular velocity damping (FUN_0058e460; +0x204 = FUN_0058e440 linear, measured)
int g_generation = -1;

void Refresh() {
    if (g_generation == features::ModsGeneration()) return;
    g_generation = features::ModsGeneration();
    g_handling.clear();
    auto hash = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(kNameHash);
    for (const features::VehicleHandling& v : features::ModVehicleHandling()) {
        g_handling[hash(v.model.c_str())] = v;
        dslog::Write("Handling: %s grip %.2f, spin damping %.2f", v.model.c_str(), v.grip, v.spin);
    }
#ifndef DS_DIST
    DWORD off = 0, size = sizeof off;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "HandlingSpinOffset", RRF_RT_REG_DWORD, nullptr, &off,
                     &size) == ERROR_SUCCESS && (off == 0x204 || off == 0x208)) {
        g_spinOffset = off;
        dslog::Write("[dev]  Handling: spin damping goes to body +0x%X", off);
    }
#endif
}

void __stdcall Grip(uint8_t* params, float softness, const uint8_t* phys) {
    reinterpret_cast<void(__stdcall*)(uint8_t*, float)>(kSetSoftness)(params, softness);
    Refresh();
    if (g_handling.empty() || !phys) return;
    const uint8_t* inst = *reinterpret_cast<uint8_t* const*>(phys + 0xEB8);
    const uint8_t* obj = inst ? *reinterpret_cast<uint8_t* const*>(inst + 0xC8) : nullptr;
    if (!obj) return;
    const uint32_t h = *reinterpret_cast<const uint32_t*>(obj + 0x1BC);
    auto it = g_handling.find(h);
    if (it == g_handling.end()) {
        static std::unordered_set<uint32_t> missed;
        if (missed.size() < 8 && missed.insert(h).second) dslog::Write("[dev]  Handling: contact for model hash %08X - no grip", h);
        return;
    }
    const features::VehicleHandling& v = it->second;
    if (v.grip >= 0.0f) {
        *reinterpret_cast<float*>(params + 0x24) = v.grip;
        *reinterpret_cast<float*>(params + 0x28) = v.grip;
        *reinterpret_cast<uint32_t*>(params + 0xC) |= 0x10;
    }
    uint8_t* body = *reinterpret_cast<uint8_t* const*>(phys + 0xDE8);   // the chassis' Karma body
    if (v.spin >= 0.0f && body) *reinterpret_cast<float*>(body + g_spinOffset) = v.spin;
    static std::unordered_set<const void*> told;
    if (told.insert(phys).second)
        dslog::Write("Handling: vehicle %p - tyre friction %.2f, spin damping %.2f", phys, v.grip, v.spin);
}

// Replaces "call 0x58CB70" (stdcall(params, softness), ret 8); esi = the physics object.
__declspec(naked) void GripStub() {
    __asm {
        mov eax, dword ptr [esp + 8]   // softness
        mov ecx, dword ptr [esp + 4]   // params
        push esi
        push eax
        push ecx
        call Grip
        ret 8
    }
}
}  // namespace

void features::ApplyHandling() {
    static bool done = false;
    if (done) return;
    done = true;
    if (patch::HookCall(kSoftCall, reinterpret_cast<const void*>(&GripStub), kSetSoftness))
        dslog::Write("Handling: tyre contact hook installed");
    else
        dslog::Write("[fail] Handling: tyre contact callback not recognised - vehicle grip not applied");
}

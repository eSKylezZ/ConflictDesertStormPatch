// Widescreen display modes (2026-10-25). The launcher's Settings > Resolution list (dialog 111, combo 1020) and the
// game's own mode check only accept modes found in a fixed table of 15 sizes at 0x916708 ({u32 w, h}, .rdata): 640x480
// ... 1680x1050 - and its wide entries are typos (1152x720, 1720x1080, 1600x1080, 1900x1200, 2500x1600), so 1920x1080,
// 1920x1200, 2560x1440 and 2560x1600 never show up (dev PC, RTX 4090 at 2560x1440: 10 modes, largest 1680x1050).
// Three readers, all in the decrypted .text (unpack.cpp):
//   0x43EBA0 IsKnownMode(w, h) cdecl -> 1 / 0 (callers 0x43F8B2, 0x440B66, 0x5D895D)   -> mov eax, 1; ret
//   0x43F480 Settings combo filler, inline loop (found -> 0x43F4A4)                      -> xor eax,eax = jmp found
//   0x440A02 device has a usable mode, inline loop (found -> 0x440A31)                   -> xor eax,eax = jmp found
// The 3D view is Hor+ already (the game shipped 16:10 / 16:9 sizes in that table); nothing else changes.
#include <windows.h>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
const uint8_t kKnownOrig[] = {0x8B, 0x4C, 0x24, 0x08, 0x8B, 0x54};   // mov ecx,[esp+8]; mov edx,[esp+4]
const uint8_t kKnownNew[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};    // mov eax,1; ret
const uint8_t kXorEax[] = {0x33, 0xC0};
const uint8_t kJmpFound1[] = {0xEB, 0x20};                           // 0x43F482 -> 0x43F4A4
const uint8_t kJmpFound2[] = {0xEB, 0x2B};                           // 0x440A04 -> 0x440A31
const BytePatch kWidescreen[] = {
    {DS_PATCH_NAME("mode check: every mode"), 0x43EBA0, kKnownOrig, kKnownNew, sizeof kKnownOrig, false},
    {DS_PATCH_NAME("Settings resolution list: every mode"), 0x43F482, kXorEax, kJmpFound1, 2, false},
    {DS_PATCH_NAME("device mode check: every mode"), 0x440A04, kXorEax, kJmpFound2, 2, false},
};

#ifndef DS_DIST
// Dev\Windowed = 1 (dev builds, background tests): the present-params set-up 0x5F8D60 sets renderer +0x41D30
// (Windowed) = (adapter +0x692C == 0) with `sete cl` at 0x5F8DC6 - forced to 1, so a test run is a window
// PrintWindow can capture instead of a minimised full-screen one. Players' builds don't have it.
const uint8_t kSeteCl[] = {0x0F, 0x94, 0xC1};
const uint8_t kMovCl1[] = {0xB1, 0x01, 0x90};
const BytePatch kDevWindowed[] = {{DS_PATCH_NAME("dev: windowed"), 0x5F8DC6, kSeteCl, kMovCl1, 3, false}};
bool DevWindowed() {
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DeniedOpsFix\\Dev", "Windowed", RRF_RT_REG_DWORD, nullptr, &v,
                        &size) == ERROR_SUCCESS && v;
}
#endif

void Apply() {
    if (patch::ApplyGroup("WIDESCREEN", kWidescreen))
        dslog::Write("[ok]   Widescreen: every display mode the card lists is offered");
    else
        dslog::Write("[fail] Widescreen: unexpected game code - mode list unchanged");
#ifndef DS_DIST
    if (DevWindowed() && patch::ApplyGroup("DEV_WINDOWED", kDevWindowed)) dslog::Write("[dev] Widescreen: windowed (Dev\\Windowed)");
#endif
}
}  // namespace

void features::ApplyWidescreen() {
    if (!Get().widescreen) {
        dslog::Write("[off]  Widescreen: off (FeatWidescreen)");
        return;
    }
    AfterUnpack(&Apply);
}

// Launcher resolution list (FUN_00420900, combo 1020) only adds modes FUN_00420590(w, h) accepts - a whitelist of
// 640x480, 800x600, 1024x768 and 1280x1024. The check now accepts every mode the adapter lists (widescreen, 1440p,
// 4K / DSR modes); the list builder already drops duplicate "WxH" entries.
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint8_t kFilterOrig[] = {0x8B, 0x44, 0x24, 0x04, 0x3D, 0x00};   // mov eax, [esp+4]; cmp eax, 0x400 ...
constexpr uint8_t kFilterNew[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};    // mov eax, 1; ret
constexpr BytePatch kWidescreen[] = {
    {DS_PATCH_NAME("Launcher: list every display mode"), 0x420590, kFilterOrig, kFilterNew, sizeof kFilterOrig, false},
};
}  // namespace

void features::ApplyWidescreen() { patch::ApplyGroup("Widescreen", kWidescreen); }

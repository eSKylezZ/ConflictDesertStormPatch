// Launcher resolution list (FUN_00419ff8, combo 1020) only adds modes FUN_00419969(w, h) accepts - a whitelist of
// 640x480, 800x600, 1024x768 and 1280x1024 (as in DS2). The check now accepts every mode the adapter lists (widescreen,
// 1440p, 4K / DSR modes); the list builder already drops duplicate "WxH" entries.
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint8_t kFilterOrig[] = {0x55, 0x8B, 0xEC, 0x8B, 0x4D, 0x08};   // push ebp; mov ebp, esp; mov ecx, [ebp+8]
constexpr uint8_t kFilterNew[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};    // mov eax, 1; ret
constexpr BytePatch kWidescreen[] = {
    {DS_PATCH_NAME("Launcher: list every display mode"), 0x419969, kFilterOrig, kFilterNew, sizeof kFilterOrig, false},
};
}  // namespace

void features::ApplyWidescreen() { patch::ApplyGroup("Widescreen", kWidescreen); }

// Display modes: both resolution lists - the mode table builder FUN_0051EE90 (renderer side) and the Settings dialog's
// combo 1020 (FUN around 0x418860) - only add modes FUN_00417CF0(w, h) accepts. That predicate is a jmp into the
// SecuROM VM (slot [0x117ED8C]) and lets 4 modes through (the 4:3 / 5:4 whitelist DS2 had). Its two calls now answer
// "yes" (mov eax, 1), so every mode the adapter lists is offered (widescreen, 1440p, 4K / DSR); both lists already
// drop duplicate "WxH" entries.
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint8_t kCallTable[] = {0xE8, 0xB5, 0x8D, 0xEF, 0xFF};    // 0x51EF36 call 0x417CF0
constexpr uint8_t kCallDialog[] = {0xE8, 0x65, 0xF4, 0xFF, 0xFF};   // 0x418886 call 0x417CF0
constexpr uint8_t kYes[] = {0xB8, 0x01, 0x00, 0x00, 0x00};          // mov eax, 1
constexpr BytePatch kWidescreen[] = {
    {DS_PATCH_NAME("Mode table: keep every display mode"), 0x51EF36, kCallTable, kYes, sizeof kYes, false},
    {DS_PATCH_NAME("Settings: list every display mode"), 0x418886, kCallDialog, kYes, sizeof kYes, false},
};
}  // namespace

void features::ApplyWidescreen() { patch::ApplyGroup("Widescreen", kWidescreen); }

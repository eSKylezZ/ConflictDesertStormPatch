// HUD / text at modern resolutions.
//
// Font sets: FUN_0059A990 (font manager, called when the mode changes) picks the high-resolution font pages only when
// the screen is exactly 1024 or 1280 wide (cmp [renderer+0x41F20], 0x400 / 0x500), every other width - 1920, 2560,
// 3840 - got the low-resolution set made for 640 / 800, i.e. the smallest text at the largest screens. It now takes the
// high-resolution set from 1024 wide up.
//
// Menu aspect: the menus (front end, pause menu, loading screens) are laid out in fractions of the screen, so on a
// widescreen the 4:3 art - logo, buttons - is stretched sideways. Unless the game state [0x72912C] is 0x16 (a mission
// in play: 2 front end, 0x11 pause menu) the screen-space draws are squeezed back to 4:3 about the centre (graphics.cpp); full-
// width pieces (the background film, the frame bars) still fill the screen. The game's own cursor is a screen-space
// sprite too, so it is squeezed with the buttons and stays on what it points at. The mission HUD keeps its own layout
// (it already sizes with the screen height and anchors to the edges).
#include <windows.h>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/graphics.h"

namespace {
// 0x59A9A4: jle 0x59A9AB; cmp eax, 0x500; je 0x59A9B1   ->   jl 0x59A9AD (low-res); jmp 0x59A9B1 (high-res); nop x5
constexpr uint8_t kFontOrig[] = {0x7E, 0x05, 0x3D, 0x00, 0x05, 0x00, 0x00, 0x74, 0x04};
constexpr uint8_t kFontNew[] = {0x7C, 0x07, 0xEB, 0x09, 0x90, 0x90, 0x90, 0x90, 0x90};
constexpr BytePatch kFonts[] = {
    {DS_PATCH_NAME("Fonts: high-resolution set from 1024 wide"), 0x59A9A4, kFontOrig, kFontNew, sizeof kFontOrig, false},
};
constexpr uint32_t kInPlay = 0x16;

#ifndef DS_DIST
bool DevNoSqueeze() {   // Dev\NoSqueeze = 1: menus as the game draws them (before shots)
    static int v = -1;
    if (v < 0) {
        DWORD d = 0, size = sizeof d;
        v = RegGetValueA(HKEY_CURRENT_USER, "Software\\GlobalStormFix\\Dev", "NoSqueeze", RRF_RT_REG_DWORD, nullptr, &d,
                         &size) == ERROR_SUCCESS && d;
    }
    return v == 1;
}
#else
bool DevNoSqueeze() { return false; }
#endif
}  // namespace

void features::OnFrameHud() {
    float s = 0.0f, cx = 0.0f, w = 0.0f;
    auto r = *reinterpret_cast<const uint8_t* const*>(0x8DE724);
    if (settings::Get().hudScaling && !DevNoSqueeze() && r && *reinterpret_cast<const uint32_t*>(0x72912C) != kInPlay) {
        const int sw = *reinterpret_cast<const int*>(r + 0x41F20), sh = *reinterpret_cast<const int*>(r + 0x41F24);
        if (sw > 0 && sh > 0 && sw * 3 > sh * 4 + 3) {
            w = static_cast<float>(sw);
            cx = w * 0.5f;
            s = sh * 4.0f / 3.0f / w;
        }
    }
    graphics::SetMenuSqueeze(s, cx, w);
}

void features::ApplyHud() {
    if (!settings::Get().hudScaling) return;
    patch::ApplyGroup("HUD fonts", kFonts);
}

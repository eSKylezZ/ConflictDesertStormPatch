// Widescreen: the launcher's resolution list (Launcher_FillResolutions 0x5BE900, combo 0x3FC) only adds modes of a
// whitelist - 640x480, 800x600, 1024x768, 1280x1024 (an inline cmp chain on the mode's width, then its height). The
// chain's first compare now loads the height and jumps straight to the "add it" code, so every mode the adapter lists
// in the desktop's format shows up (widescreen, 1440p, 4K / DSR modes). The same whitelist as a function
// (Launcher_ModeAllowed 0x5BE5E0, no callers in the exe) answers 1 too, so nothing else can still reject a mode.
// Same idea as DesertStorm2Fix's widescreen.cpp (DS2 calls a filter function, TGE has it inline).
//
// The 3D view: the projection is Hor+ like DS1's (built from the real viewport), so a 16:9 mode shows more to the
// sides instead of stretching. Menus / HUD: guiscale.cpp.
//
// Films (setting FilmsFill): every Bink film - the main menu's background loop, the story films, the logos - is drawn
// by one quad (call 0x499555 -> the 2D draw 0x446F00, thiscall(renderer, 2, verts, 0x1C), pretransformed FVF 0x144)
// sized to the film's 4:3 frame at the screen's full height: 241..1679 x 0..1080 at 1920 x 1080, black at the sides.
// With FilmsFill the quad is zoomed about the screen's centre until it covers the width - the frame's top and bottom
// go past the screen's edges (on 16:9 an eighth each; the story films' own letterbox bars are what goes) - only for the
// menu background and the story films (films.cpp FilmFillsScreen): the legal screen / logos / chapter cards / briefings
// have text near the frame's top and bottom.
#include <algorithm>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
// 0x5BE99F: cmp eax, 0x400 (3D 00 04 00 00) -> mov ecx, [edx] (the height, as the chain's next instruction would);
// jmp 0x5BE9FA (the add-to-list code: push ecx; push eax; push "%dx%d" ...); nop
constexpr uint8_t kChainOrig[] = {0x3D, 0x00, 0x04, 0x00, 0x00};
constexpr uint8_t kChainNew[] = {0x8B, 0x0A, 0xEB, 0x57, 0x90};
// 0x5BE5E0: cmp eax, 0x400; ja ... -> mov eax, 1; ret
constexpr uint8_t kAllowedOrig[] = {0x3D, 0x00, 0x04, 0x00, 0x00, 0x77};
constexpr uint8_t kAllowedNew[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
constexpr BytePatch kWidescreen[] = {
    {DS_PATCH_NAME("Launcher: list every display mode"), 0x5BE99F, kChainOrig, kChainNew, sizeof kChainOrig, false},
    {DS_PATCH_NAME("Launcher: every mode allowed"), 0x5BE5E0, kAllowedOrig, kAllowedNew, sizeof kAllowedOrig, false},
};
constexpr uint32_t kFilmDrawCall = 0x499555, kDraw2D = 0x446F00;
constexpr uint32_t kRenderer = 0x79ED20;   // renderer*: +0x40690 W, +0x40694 H
using Draw2DFn = void(__thiscall*)(void*, int, uint8_t*, int);

void __fastcall FilmDraw(void* renderer, void*, int count, uint8_t* verts, int stride) {
    const auto* r = *reinterpret_cast<const uint8_t* const*>(kRenderer);
    if (r && verts && count == 2 && stride >= 8 && features::FilmFillsScreen()) {
        const float w = static_cast<float>(*reinterpret_cast<const int*>(r + 0x40690));
        const float h = static_cast<float>(*reinterpret_cast<const int*>(r + 0x40694));
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
        for (int i = 0; i < 4; ++i) {
            const float* p = reinterpret_cast<const float*>(verts + i * stride);
            x0 = std::min(x0, p[0]), x1 = std::max(x1, p[0]), y0 = std::min(y0, p[1]), y1 = std::max(y1, p[1]);
        }
        const float s = (x1 > x0 && w > 0) ? w / (x1 - x0) : 1.0f;
        if (s > 1.01f && s < 3.0f && y1 - y0 > h * 0.9f) {   // a frame narrower than the screen at full height
            const float cx = w * 0.5f, cy = h * 0.5f;
            for (int i = 0; i < 4; ++i) {
                float* p = reinterpret_cast<float*>(verts + i * stride);
                p[0] = cx + (p[0] - cx) * s;
                p[1] = cy + (p[1] - cy) * s;
            }
        }
    }
    reinterpret_cast<Draw2DFn>(kDraw2D)(renderer, count, verts, stride);
}
}  // namespace

void features::ApplyWidescreen() {
    if (Get().filmsFill && !patch::HookCall(kFilmDrawCall, reinterpret_cast<const void*>(&FilmDraw), kDraw2D))
        dslog::Write("[fail] Widescreen: film draw at 0x%08X not recognised - films keep their side bars", kFilmDrawCall);
    if (!Get().widescreen) {
        dslog::Write("[off]  Widescreen: launcher keeps the game's four modes (FeatWidescreen)");
        return;
    }
    patch::ApplyGroup("Widescreen", kWidescreen);
}

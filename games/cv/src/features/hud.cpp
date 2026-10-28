// HUD / menu art scaled to the resolution, and the in-game 2D laid out for screens wider than 4:3.
//
// Scale. Vietnam draws its 2D art (HUD, menus, fonts) at the sheets' pixel size: below 1024 wide the normal sheets
// (ds_int.img, font_*.img, made for 800x600), from 1024 wide the _hi ones (made for 1024x768, chosen in initGameSystem
// FUN_0042ffd6 / LoadFrontEnd FUN_00428699 by renderer W < 0x400). So the HUD only fills the screen as designed at
// 800x600 / 1024x768 and shrinks above (1080p: 30 % too small, 4K: half).
// Sheet scale setters (sheet object, ctor FUN_004d6823: +0x20 enabled, +0x24 x scale, +0x28 y scale; percent arguments
// x 0.01 at 0x69F958, compared with 100.0 at 0x6DF14C):
//   FUN_004d673b  thiscall(sheet, float s)                       ret 4   - flag 0 at exactly 100 %, else 1
//   FUN_004d676a  thiscall(sheet, float sx, float sy, int flag)   ret 0xC - flag 0 at 100/100, else the argument
// Both entries jump to C++ copies that first multiply by k = max(1, min(W/bw, H/bh)) * HudScalePercent/100, with W / H
// the renderer's size ([0x8842D0] +0x41A34 / +0x41A38) and bw x bh = 1024 x 768 from 1024 wide, else 800 x 600 (the
// art's design size). (MSVC __fastcall with an unused edx = thiscall here.)
//
// Layout: see "layout" below - HUD elements keep their 4:3 distance from the screen edges on wider screens.
#include <windows.h>

#include <algorithm>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kRenderer = 0x8842D0, kWidth = 0x41A34, kHeight = 0x41A38;
constexpr uint32_t kSetUniform = 0x4D673B, kSetXY = 0x4D676A;
constexpr uint8_t kUniformEntry[] = {0xD9, 0x44, 0x24, 0x04, 0xD9, 0x05, 0x58, 0xF9, 0x69, 0x00};   // fld [esp+4]; fld [0.01]
constexpr uint8_t kXYEntry[] = {0x55, 0x8B, 0xEC, 0xD9, 0x05, 0x58, 0xF9, 0x69, 0x00};             // push ebp; ...; fld [0.01]

int* Field(uint32_t offset) {
    auto r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    return r ? reinterpret_cast<int*>(r + offset) : nullptr;
}

// ---- scale ----------------------------------------------------------------------------------------------------------
float HudK() {
    const int *pw = Field(kWidth), *ph = Field(kHeight);
    if (!pw || !ph || *pw <= 0 || *ph <= 0) return 1.0f;
    const int w = *pw, h = *ph;
    const float bw = w >= 1024 ? 1024.0f : 800.0f, bh = w >= 1024 ? 768.0f : 600.0f;
    return std::max(1.0f, std::min(w / bw, h / bh)) * settings::Get().hudScalePercent / 100.0f;
}

void Store(uint8_t* sheet, float sx, float sy, int flag) {
    *reinterpret_cast<float*>(sheet + 0x24) = sx * 0.01f;
    *reinterpret_cast<float*>(sheet + 0x28) = sy * 0.01f;
    *reinterpret_cast<int*>(sheet + 0x20) = (sx == 100.0f && sy == 100.0f) ? 0 : flag;
}

void __fastcall SetUniform(uint8_t* sheet, void*, float s) {
    const float k = HudK();
    Store(sheet, s * k, s * k, 1);
}

void __fastcall SetXY(uint8_t* sheet, void*, float sx, float sy, int flag) {
    const float k = HudK();
    Store(sheet, sx * k, sy * k, flag);
}

// ---- layout -------------------------------------------------------------------------------------------------------
// HUD anchor layout FUN_0045c75e(float* out, int* view {x, y, w, h}, float anchor[2], float offset[2]) -> out, reached
// through FUN_0045c823 / FUN_0045c870(out, view, element) (anchor = the element's entry in the table at 0x6EB180,
// offset = the split-screen layout variant [0x702C18], 0 in single player) and the safe frame FUN_0042a9cc (anchors
// 0.075 / 0.925):
//   x = W * offset.x + view.w * anchor.x + view.x,   y = H * offset.y + view.h * anchor.y + view.y
// The anchor is a fraction of the view's WIDTH, so on a wide screen every element drifts in from its edge while its art
// grows with the height (1080p: compass 58 px, soldier panel 33 px too far in; worse on 21:9). The function is replaced
// by a copy that places x on the view's 4:3 width w43 = min(view.w, view.h * 4 / 3) - left third: view.x + ax * w43,
// right third: view.x + view.w - (1 - ax) * w43, middle: the view's centre + (ax - 1/2) * w43 - so every element keeps
// its 4:3 distance from its own edge; W in the offset term becomes min(W, H * 4 / 3) the same way. y is unchanged, and
// views up to 4:3 come out exactly as before.
constexpr uint32_t kAnchorLayout = 0x45C75E;
constexpr uint8_t kAnchorEntry[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x18, 0xA1, 0xD0, 0x42, 0x88, 0x00};

float* __cdecl AnchorLayout(float* out, const int* view, const float* anchor, const float* offset) {
    const int *pw = Field(kWidth), *ph = Field(kHeight);
    const float W = pw ? static_cast<float>(*pw) : 0.0f, H = ph ? static_cast<float>(*ph) : 0.0f;
    const float vx = static_cast<float>(view[0]), vy = static_cast<float>(view[1]);
    const float vw = static_cast<float>(view[2]), vh = static_cast<float>(view[3]);
    const float w43 = std::min(vw, vh * 4.0f / 3.0f), ax = anchor[0];
    float x;
    if (w43 >= vw) x = vx + vw * ax;                                  // 4:3 or narrower: as the game does it
    else if (ax < 1.0f / 3.0f) x = vx + ax * w43;
    else if (ax > 2.0f / 3.0f) x = vx + vw - (1.0f - ax) * w43;
    else x = vx + vw * 0.5f + (ax - 0.5f) * w43;
    out[0] = std::min(W, H * 4.0f / 3.0f) * offset[0] + x;
    out[1] = H * offset[1] + vh * anchor[1] + vy;
    return out;
}

void ApplyLayout() {
    if (!patch::Matches(kAnchorLayout, kAnchorEntry, sizeof kAnchorEntry)) {
        dslog::Write("[fail] HUD layout: unexpected bytes at 0x%08X - skipped", kAnchorLayout);
        return;
    }
    patch::WriteJump(kAnchorLayout, reinterpret_cast<void*>(&AnchorLayout));
    dslog::Write("[ok]   HUD layout: elements keep their 4:3 distance from the screen edges");
}
}  // namespace

void features::ApplyHud() {
    if (!settings::Get().hudScaling) return;
    if (!patch::Matches(kSetUniform, kUniformEntry, sizeof kUniformEntry) || !patch::Matches(kSetXY, kXYEntry, sizeof kXYEntry)) {
        dslog::Write("[fail] HUD scaling: unexpected bytes at the sheet scale setters - skipped");
        return;
    }
    patch::WriteJump(kSetUniform, reinterpret_cast<void*>(&SetUniform));
    patch::WriteJump(kSetXY, reinterpret_cast<void*>(&SetXY));
    dslog::Write("[ok]   HUD scaling: sheet scale setters x k (HudScalePercent %u)", settings::Get().hudScalePercent);
    ApplyLayout();
}

void features::OnFrameHud() {}

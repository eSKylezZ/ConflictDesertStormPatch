// HUD scaling: the scale setters FUN_0053c890/FUN_0053c850 jump into a cave at 0x5D7B00 that multiplies by
// k = max(1, min(W/800, H/600)) * HudScale, plus 93 layout clamps. Bytes: scripts/patch_defs.py (HUD,
// LAYOUT_CLAMPS).
//
// Icon centring: some HUD code centres an image in a tile using the tile's scaled size (FUN_0053c6d0 / 0053c710,
// size x sheet scale) but the image's raw size (FUN_0053c690 / 0053c6b0, pixels in the sheet). That only works at
// scale 1, so with the scaled HUD the soldier panel's follow / halt icon (image 0x25 in the GWInt sheet, drawn at
// 0x48e246) sat up and to the right of its tile. Those raw-size calls get the scaled size instead:
//   0x48E19B / 0x48E1F0  soldier panel order icon (width / height)
//   0x44BEDC / 0x44BFB8  blinking item icon FUN_0044bda0 (half height, same sheet)
// Menu text centring: FUN_004570d0 (font) returns a fixed line height (19 for font 0x60EDB8, else 10) that the menu
// lists use to centre their text vertically (baseline = box centre + height/2). With the font sheet scaled the
// glyphs grew but the height did not, so the text sat ~(k-1)*9.5 px too high -> replaced by the height x the font
// sheet's (0x60EDB0) vertical scale.
// Slider signs: the Options volume sliders place their '-' / '+' glyphs at bar top + half bar - 18/16 x scale + the
// font's raw height (FUN_0053a0c0 = font+0x18, unscaled) -> signs drawn in the bar's top corners. Those two calls
// (0x455EDD, 0x455F19) get the height x the font sheet scale.
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "generated/patch_tables.h"

namespace {
constexpr uint32_t kHudScaleVa = 0x5D7B00;  // float read by the cave's compute_k
constexpr uint32_t kRawWidth = 0x53C690, kRawHeight = 0x53C6B0;  // thiscall (image) -> ax: size in the sheet
constexpr uint32_t kFontRawHeight = 0x53A0C0;  // thiscall () -> ax: font+0x18
constexpr uint32_t kFontLineHeight = 0x4570D0;   // (font) -> st0; 22 callers, all menu text layout
constexpr uint32_t kBigFont = 0x60EDB8, kFontSheet = 0x60EDB0;
constexpr uint8_t kFontLineHeightOrig[] = {0x8B, 0x44, 0x24, 0x04, 0x8B};  // mov eax,[esp+4]; mov ecx,...

// Image sheet: +4 image records (24 bytes: width +0xe, height +0x10), +0x20 flags (bit 0 = scaled), +0x24/+0x28 scale.
int16_t ScaledSize(const uint8_t* sheet, int image, uint32_t sizeOffset, uint32_t scaleOffset) {
    const uint8_t* rec = *reinterpret_cast<uint8_t* const*>(sheet + 4) + image * 24;
    const int raw = *reinterpret_cast<const uint16_t*>(rec + sizeOffset);
    if (!(*reinterpret_cast<const uint32_t*>(sheet + 0x20) & 1)) return static_cast<int16_t>(raw);
    return static_cast<int16_t>(raw * *reinterpret_cast<const float*>(sheet + scaleOffset));
}
int16_t __fastcall ScaledWidth(const uint8_t* sheet, void*, int image) { return ScaledSize(sheet, image, 0xE, 0x24); }
float FontSheetScaleY() {
    const auto* sheet = *reinterpret_cast<const uint8_t* const*>(0x60EDB0);
    return sheet && (*reinterpret_cast<const uint32_t*>(sheet + 0x20) & 1) ? *reinterpret_cast<const float*>(sheet + 0x28) : 1.0f;
}
int16_t __fastcall ScaledFontHeight(const uint8_t* font) {
    return static_cast<int16_t>(*reinterpret_cast<const int16_t*>(font + 0x18) * FontSheetScaleY());
}
int16_t __fastcall ScaledHeight(const uint8_t* sheet, void*, int image) { return ScaledSize(sheet, image, 0x10, 0x28); }

float __stdcall ScaledFontLineHeight(const void* font) {
    float h = font == *reinterpret_cast<void* const*>(kBigFont) ? 19.0f : 10.0f;
    const auto* sheet = *reinterpret_cast<const uint8_t* const*>(kFontSheet);
    if (sheet && (*reinterpret_cast<const uint32_t*>(sheet + 0x20) & 1)) h *= *reinterpret_cast<const float*>(sheet + 0x28);
    return h;
}

// FUN_004570d0 -> ScaledFontLineHeight (on) or its original first bytes back (off).
void ApplyMenuTextCentring(bool on) {
    uint8_t jump[5] = {0xE9};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&ScaledFontLineHeight) - (kFontLineHeight + 5));
    std::memcpy(jump + 1, &rel, 4);
    if (patch::Matches(kFontLineHeight, on ? jump : kFontLineHeightOrig, 5)) return;
    if (!patch::Matches(kFontLineHeight, on ? kFontLineHeightOrig : jump, 5)) {
        dslog::Write("[fail] HUD menu text centring: unexpected bytes at 0x%08X - not applied", kFontLineHeight);
        return;
    }
    patch::Write(kFontLineHeight, on ? jump : kFontLineHeightOrig, 5);
}

struct CallSite {
    uint32_t site, original;
    const void* scaled;
};
const CallSite kIconCalls[] = {
    {0x48E19B, kRawWidth, reinterpret_cast<const void*>(&ScaledWidth)},
    {0x48E1F0, kRawHeight, reinterpret_cast<const void*>(&ScaledHeight)},
    {0x44BEDC, kRawHeight, reinterpret_cast<const void*>(&ScaledHeight)},
    {0x44BFB8, kRawHeight, reinterpret_cast<const void*>(&ScaledHeight)},
    {0x455EDD, kFontRawHeight, reinterpret_cast<const void*>(&ScaledFontHeight)},
    {0x455F19, kFontRawHeight, reinterpret_cast<const void*>(&ScaledFontHeight)},
};

bool CallsTo(uint32_t site, uint32_t target) {
    uint8_t code[5] = {0xE8};
    const int32_t rel = static_cast<int32_t>(target - (site + 5));
    std::memcpy(code + 1, &rel, 4);
    return patch::Matches(site, code, sizeof code);
}

// Points every icon-centring call at the scaled getter (on) or back at the raw one (off); all or nothing.
void ApplyIconCentring(bool on) {
    for (const CallSite& c : kIconCalls)
        if (!CallsTo(c.site, c.original) && !CallsTo(c.site, reinterpret_cast<uint32_t>(c.scaled))) {
            dslog::Write("[fail] HUD icon centring: unexpected call at 0x%08X - not applied", c.site);
            return;
        }
    for (const CallSite& c : kIconCalls) {
        const uint32_t from = on ? c.original : reinterpret_cast<uint32_t>(c.scaled);
        if (CallsTo(c.site, from)) patch::HookCall(c.site, on ? c.scaled : reinterpret_cast<const void*>(c.original), from);
    }
}
}  // namespace

void features::ApplyHud() {
    const auto& s = settings::Get();
    if (!s.hudScaling) {
        patch::RevertGroup("HUD scaling", gen::kHud);
        ApplyIconCentring(false);
        ApplyMenuTextCentring(false);
        return;
    }
    if (patch::ApplyGroup("HUD scaling", gen::kHud)) {
        patch::WriteValue(kHudScaleVa, static_cast<float>(s.hudScalePercent) / 100.0f);
        ApplyIconCentring(true);
        ApplyMenuTextCentring(true);
    }
}

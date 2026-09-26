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
//   0x4712DA / 0x471586  menu list items with flag 4: image 0x48 right-aligned in the list width (GWInt sheet)
// Menu text centring: FUN_004570d0 (font) returns a fixed line height (19 for font 0x60EDB8, else 10) that the menu
// lists use to centre their text vertically (baseline = box centre + height/2). With the font sheet scaled the
// glyphs grew but the height did not, so the text sat ~(k-1)*9.5 px too high -> replaced by the height x the font
// sheet's (0x60EDB0) vertical scale.
// Slider signs: the Options volume sliders place their '-' / '+' glyphs at bar top + half bar - 18/16 x scale + the
// font's raw height (FUN_0053a0c0 = font+0x18, unscaled) -> signs drawn in the bar's top corners. Those two calls
// (0x455EDD, 0x455F19) get the height x the font sheet scale.
// Info bar (item names in the inventory, pick-up messages; FUN_00507b30 on [0x63c988]): the bar is sized from the
// font's raw line height FUN_0053a0e0 (font+0x14) while its text is drawn scaled - at the top of the screen the text
// ended up above the edge, leaving an empty bar. That call (0x507B42) gets the height x the font sheet scale.
// Pop-up bar (bottom of the screen: picked-up items, warnings such as shooting a civilian; FUN_004d3420): bar and
// text box one raw line high (0x4D3507, 0x4D35E3) while the text (80 % size, FUN_0053c850(80)) is scaled - the boxed
// text routine FUN_0053abc0 skips a line that doesn't fit its box -> empty bar. Both get the scaled line height.
// Text fields on the network screens (SESSION NAME): a fixed-pixel part of the width is scaled (ApplyFieldPads).
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
constexpr uint32_t kFontLineStep = 0x53A0E0;   // thiscall () -> ax: font+0x14
constexpr uint32_t kTextHeight = 0x53A150;     // thiscall (text) -> ax: tallest glyph (scaled)
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
int16_t __fastcall ScaledFontLineStep(const uint8_t* font) {  // FUN_0053a0e0 = font +0x14, x the font sheet scale
    return static_cast<int16_t>(*reinterpret_cast<const int16_t*>(font + 0x14) * FontSheetScaleY() + 0.5f);
}
// Info bar: the text is centred on the bar with FUN_0053a150 (tallest glyph, shadow included), which left it hanging
// over the bar's bottom edge at the scaled size; 30 % of a line less centres the capitals.
int16_t __fastcall InfoBarTextHeight(const uint8_t* font, void*, const char* text) {
    // icon glyphs (control chars 1-31) are taller than the letters and pushed the line down: measured as 'H'
    char plain[256];
    size_t n = 0;
    for (; text && text[n] && n + 1 < sizeof plain; ++n)
        plain[n] = (static_cast<unsigned char>(text[n]) < 32 && text[n] != 10) ? 'H' : text[n];
    plain[n] = 0;
    const int16_t h = reinterpret_cast<int16_t(__thiscall*)(const uint8_t*, const char*)>(0x53A150)(font, plain);
    const int16_t less = static_cast<int16_t>(ScaledFontLineStep(font) * 3 / 10);
    return h > less ? static_cast<int16_t>(h - less) : 0;
}

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

// Front-end prompt line (FUN_0047a4a0, every menu incl. the pause menu): select prompt left-aligned, back prompt
// right-aligned in a box {left [esp+0x74], top, right [esp+0x7c], bottom}; left = 50, right = 750 design px x the
// sheet scale - with the layout clamps (800 wide) the right end sat at 750·k, far from the right edge on a wide
// screen. The right end's __ftol (0x47A586) now returns screen width - left: the prompts mirror each other.
__declspec(naked) void MenuPromptRight() {
    __asm {
        mov eax, 0x566ED4
        call eax                   // __ftol, as before (pops st0)
        mov eax, dword ptr ds:[0x63C924]
        mov eax, [eax + 0x40688]   // renderer width
        sub eax, [esp + 0x78]      // caller's [esp+0x74]: left
        ret
    }
}

// Tutorial / objective bar ("Rescue Foley. Press objectives (..)", FUN_00486d20): the text is measured at the scaled
// size (0x53afb0 -> rect {left, top, right, bottom} at [esp+0xc]), then padded by fixed pixels - 4 left/right, 6
// below (0x486DC6..0x486DE5) - so at 1440p the text touched the bar's bottom edge. The padding now scales with the
// font sheet; the new right end goes back in edx as before (stored at 0x486DF5).
constexpr uint32_t kBarPadSite = 0x486DC6;
constexpr uint8_t kBarPadOrig[32] = {0x8B, 0x54, 0x24, 0x18, 0x8B, 0x4C, 0x24, 0x0C, 0x83, 0xC2, 0x06, 0xB8, 0x04, 0x00, 0x00, 0x00,
                                     0x89, 0x54, 0x24, 0x18, 0x8B, 0x54, 0x24, 0x14, 0x2B, 0xC8, 0x03, 0xD0, 0x89, 0x4C, 0x24, 0x0C};
int __stdcall BarPad(int* rect) {
    float kx = 1.0f, ky = 1.0f;
    const auto* sheet = *reinterpret_cast<const uint8_t* const*>(kFontSheet);
    if (sheet) kx = *reinterpret_cast<const float*>(sheet + 0x24), ky = *reinterpret_cast<const float*>(sheet + 0x28);
    const int padX = static_cast<int>(4.0f * kx + 0.5f), padY = static_cast<int>(6.0f * ky + 0.5f);
    rect[0] -= padX;
    rect[1] += static_cast<int>(3.0f * (ky - 1.0f) + 0.5f);  // the line's space above the capitals grew with the scale
    rect[3] += padY;
    return rect[2] + padX;
}
__declspec(naked) void BarPadStub() {
    __asm {
        lea eax, [esp + 0x10]  // caller's [esp+0xc]
        push eax
        call BarPad
        mov edx, eax
        ret
    }
}

void ApplyBarPad(bool on) {
    uint8_t code[32];
    memset(code, 0x90, sizeof code);
    code[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&BarPadStub) - (kBarPadSite + 5));
    std::memcpy(code + 1, &rel, 4);
    code[5] = 0xEB, code[6] = 0x19;  // jmp 0x486DE6
    if (patch::Matches(kBarPadSite, on ? code : kBarPadOrig, 7)) return;
    if (!patch::Matches(kBarPadSite, on ? kBarPadOrig : code, on ? sizeof kBarPadOrig : 7)) {
        dslog::Write("[fail] HUD tutorial bar padding: unexpected bytes at 0x%08X", kBarPadSite);
        return;
    }
    if (on) patch::Write(kBarPadSite, code, sizeof code);
    else patch::Write(kBarPadSite, kBarPadOrig, sizeof kBarPadOrig);
}

// Network screen text fields (MULTIPLAYER -> HOST SESSION: SESSION NAME 0x45E9F7, the second field 0x45EA6C): the list
// width is 2 x a scaled image width + 250 / 150 raw pixels (the other rows use 620 x the GWInt sheet scale), so the
// scaled text ("Desert Storm_") overflowed its box. The `fadd dword [const]` becomes a call adding const x that scale.
const float kFieldPad250 = 250.0f, kFieldPad150 = 150.0f;
__declspec(naked) void FieldPad250() {
    __asm {
        push eax
        mov eax, dword ptr ds:[0x60EE18]
        fld dword ptr [eax + 0x24]
        fmul dword ptr [kFieldPad250]
        faddp st(1), st
        pop eax
        ret
    }
}
__declspec(naked) void FieldPad150() {
    __asm {
        push eax
        mov eax, dword ptr ds:[0x60EE18]
        fld dword ptr [eax + 0x24]
        fmul dword ptr [kFieldPad150]
        faddp st(1), st
        pop eax
        ret
    }
}
struct FaddSite {
    uint32_t site, constant;
    const void* stub;
};
const FaddSite kFieldPads[] = {{0x45E9F7, 0x5D90AC, reinterpret_cast<const void*>(&FieldPad250)},
                               {0x45EA6C, 0x5D899C, reinterpret_cast<const void*>(&FieldPad150)}};

void ApplyFieldPads(bool on) {
    for (const FaddSite& f : kFieldPads) {
        uint8_t orig[6] = {0xD8, 0x05}, call[6] = {0xE8, 0, 0, 0, 0, 0x90};
        std::memcpy(orig + 2, &f.constant, 4);
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(f.stub) - (f.site + 5));
        std::memcpy(call + 1, &rel, 4);
        if (patch::Matches(f.site, on ? call : orig, 6)) continue;
        if (!patch::Matches(f.site, on ? orig : call, 6)) {
            dslog::Write("[fail] HUD text field width: unexpected bytes at 0x%08X", f.site);
            continue;
        }
        patch::Write(f.site, on ? call : orig, 6);
    }
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
    {0x4712DA, kRawWidth, reinterpret_cast<const void*>(&ScaledWidth)},  // menu list item: image 0x48 right-aligned
    {0x471586, kRawWidth, reinterpret_cast<const void*>(&ScaledWidth)},  //   (item flag 4; list sheet = GWInt HUD)
    {0x455EDD, kFontRawHeight, reinterpret_cast<const void*>(&ScaledFontHeight)},
    {0x455F19, kFontRawHeight, reinterpret_cast<const void*>(&ScaledFontHeight)},
    {0x507B42, kFontLineStep, reinterpret_cast<const void*>(&ScaledFontLineStep)},  // info bar (item names ...)
    {0x507C37, kTextHeight, reinterpret_cast<const void*>(&InfoBarTextHeight)},     // info bar: text centring
    {0x4D3507, kFontLineStep, reinterpret_cast<const void*>(&ScaledFontLineStep)},  // pop-up bar: bar height
    {0x4D35E3, kFontLineStep, reinterpret_cast<const void*>(&ScaledFontLineStep)},  // pop-up bar: text box height
    {0x47A586, 0x566ED4, reinterpret_cast<const void*>(&MenuPromptRight)},             // menu prompts: right end
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
        ApplyBarPad(false);
        ApplyFieldPads(false);
        return;
    }
    if (patch::ApplyGroup("HUD scaling", gen::kHud)) {
        patch::WriteValue(kHudScaleVa, static_cast<float>(s.hudScalePercent) / 100.0f);
        ApplyIconCentring(true);
        ApplyMenuTextCentring(true);
        ApplyBarPad(true);
        ApplyFieldPads(true);
    }
}

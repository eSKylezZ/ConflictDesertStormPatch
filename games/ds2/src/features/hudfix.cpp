// HUD fixes ported from DesertStormFix (repo root src/features/hud.cpp "Play-test fixes"): places where DS2's 2D code
// mixes the scaled art (hud.cpp: sheets x k) with raw 800x600 pixel sizes, which only shows once k > 1 (missions at
// large resolutions, and the in-mission menus: pause, objectives).
#include <windows.h>
#include <intrin.h>

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
#ifndef DS_DIST
constexpr char kDevKey[] = "Software\\DesertStorm2Fix\\Dev";
DWORD DevDword(const char* name, DWORD fallback) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, kDevKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v
                                                                                                                : fallback;
}

// ---- Dev\RawProbe = 1: callers of the RAW image getters (FUN_0052b120 width +0xE, 0052b140 height +0x10, 0052b160 x
// offset +0x12, 0052b180 y offset +0x14 of the 0x1C-byte record) asked about a SCALED sheet - each distinct caller once.
// Candidates for the DS1 "raw vs scaled size" fixes. The getters are replaced by copies (same result).
uint32_t g_rawCallers[64];
int g_rawCallerCount = 0;
uint16_t Raw(const uint8_t* sheet, int image, uint32_t field, uint32_t ret, const char* what) {
    const uint16_t v = *reinterpret_cast<const uint16_t*>(*reinterpret_cast<const uint8_t* const*>(sheet + 4) +
                                                          image * 0x1C + field);
    if (*reinterpret_cast<const int*>(sheet + 0x20) && *reinterpret_cast<const float*>(sheet + 0x24) != 1.0f) {
        for (int i = 0; i < g_rawCallerCount; ++i)
            if (g_rawCallers[i] == ret) return v;
        if (g_rawCallerCount < 64) {
            g_rawCallers[g_rawCallerCount++] = ret;
            dslog::Write("[dev] RawProbe %s caller 0x%08X sheet %p image 0x%X = %u (sheet scale %.3f)", what, ret - 5,
                         sheet, image, v, *reinterpret_cast<const float*>(sheet + 0x24));
        }
    }
    return v;
}
uint16_t __fastcall RawW(const uint8_t* s, void*, int i) { return Raw(s, i, 0xE, reinterpret_cast<uint32_t>(_ReturnAddress()), "width"); }
uint16_t __fastcall RawH(const uint8_t* s, void*, int i) { return Raw(s, i, 0x10, reinterpret_cast<uint32_t>(_ReturnAddress()), "height"); }
uint16_t __fastcall RawX(const uint8_t* s, void*, int i) { return Raw(s, i, 0x12, reinterpret_cast<uint32_t>(_ReturnAddress()), "x offset"); }
uint16_t __fastcall RawY(const uint8_t* s, void*, int i) { return Raw(s, i, 0x14, reinterpret_cast<uint32_t>(_ReturnAddress()), "y offset"); }

// ---- Dev\WTextProbe = 1: the UTF-16 text draw FUN_00527710 thiscall(font, const wchar_t* text, ...) - every text the
// game draws goes through it (FUN_00529490 = the ANSI wrapper). Each distinct caller once, with the text, the font's
// line height (+0x18) and its sheet's scale.
uint32_t g_textCallers[160];
int g_textCallerCount = 0;
void __stdcall LogWText(uint32_t ret, const wchar_t* text, const uint8_t* font) {
    for (int i = 0; i < g_textCallerCount; ++i)
        if (g_textCallers[i] == ret) return;
    if (g_textCallerCount >= 160) return;
    g_textCallers[g_textCallerCount++] = ret;
    char narrow[48] = {};
    for (int i = 0; text && text[i] && i < 47; ++i) narrow[i] = text[i] < 128 ? static_cast<char>(text[i]) : '?';
    const uint8_t* sheet = font ? *reinterpret_cast<const uint8_t* const*>(font) : nullptr;
    dslog::Write("[dev] WTextProbe caller 0x%08X font %p h %d scale %.3f state 0x%X: \"%s\"", ret - 5, font,
                 font ? *reinterpret_cast<const int*>(font + 0x18) : -1,
                 sheet ? *reinterpret_cast<const float*>(sheet + 0x24) : -1.0f, *reinterpret_cast<int*>(0x63433C), narrow);
}
__declspec(naked) void WTextProbeStub() {
    __asm {
        pushad
        push ecx                    // font
        push dword ptr [esp + 0x28] // text
        push dword ptr [esp + 0x28] // return address
        call LogWText
        popad
        sub esp, 0xC                // the 8 replaced bytes
        push ebp
        mov ebp, dword ptr [esp + 0x14]
        push 0x527718
        ret
    }
}

// ---- Dev\HudStates "14@20000,13@26000": front-end states set at those ms after the mission first runs (state 0x13) -
// the pause menu (0x14, block 0 +0x348) and objectives (0x12, +0x344) without key presses in background tests.
// Per frame from the message loop's call 0x414C01 (FUN_0040b2d0).
struct Step { int state; DWORD ms; };
Step g_steps[16];
int g_stepCount = 0, g_stepPos = 0;
DWORD g_missionAt = 0;
void __cdecl FrameDev() {
    reinterpret_cast<void(__cdecl*)()>(0x40B2D0)();
    const int state = *reinterpret_cast<int*>(0x63433C);
    if (!g_missionAt) {
        if (state == 0x13) g_missionAt = GetTickCount();
        return;
    }
    if (g_stepPos >= g_stepCount || GetTickCount() - g_missionAt < g_steps[g_stepPos].ms) return;
    const int s = g_steps[g_stepPos++].state;
    *reinterpret_cast<int*>(0x62FF7C + 0x348) = s == 0x14;   // block 0: paused / objectives open
    *reinterpret_cast<int*>(0x62FF7C + 0x344) = s == 0x12;
    dslog::Write("[dev] HudStates: state 0x%X -> 0x%X", state, s);
    reinterpret_cast<void(__cdecl*)(int)>(0x43DFD0)(s);
}

void ApplyDevProbes() {
    if (DevDword("RawProbe", 0)) {
        static const uint8_t kGetter[] = {0x8B, 0x44, 0x24, 0x04, 0x8D, 0x14, 0xC5};
        const uint32_t sites[] = {0x52B120, 0x52B140, 0x52B160, 0x52B180};
        void* fns[] = {reinterpret_cast<void*>(&RawW), reinterpret_cast<void*>(&RawH), reinterpret_cast<void*>(&RawX),
                       reinterpret_cast<void*>(&RawY)};
        for (int i = 0; i < 4; ++i)
            if (patch::Matches(sites[i], kGetter, sizeof kGetter)) patch::WriteJump(sites[i], fns[i]);
        dslog::Write("[dev] RawProbe on");
    }
    static const uint8_t kTextEntry[] = {0x83, 0xEC, 0x0C, 0x55, 0x8B, 0x6C, 0x24, 0x14};
    if (DevDword("WTextProbe", 0) && patch::Matches(0x527710, kTextEntry, sizeof kTextEntry)) {
        patch::WriteJump(0x527710, reinterpret_cast<void*>(&WTextProbeStub));
        dslog::Write("[dev] WTextProbe on");
    }
    char list[256] = {};
    DWORD size = sizeof list;
    if (RegGetValueA(HKEY_CURRENT_USER, kDevKey, "HudStates", RRF_RT_REG_SZ, nullptr, list, &size) == ERROR_SUCCESS) {
        for (char* p = list; *p && g_stepCount < 16;) {
            int st = 0;
            unsigned long ms = 0;
            if (std::sscanf(p, "%x@%lu", &st, &ms) != 2) break;
            g_steps[g_stepCount++] = {st, ms};
            p = std::strchr(p, ',');
            if (!p) break;
            ++p;
        }
        if (g_stepCount && patch::HookCall(0x414C01, reinterpret_cast<void*>(&FrameDev), 0x40B2D0))
            dslog::Write("[dev] HudStates: %d steps", g_stepCount);
    }
}
#endif
}  // namespace

// ---- the fixes ---------------------------------------------------------------------------------------------------------
namespace {
// Tutorial / hint bar (top of the screen, "To fire a weapon, press MOUSE 1"; FUN_00448fb0): the text is measured
// (FUN_00527ff0 with flag 4 -> rect) and the box drawn around it with fixed pixel paddings - left -4 (0x44909C sub eax,
// 4), right +4 (0x44909F add edx, 4), bottom +6 (0x44908D add ecx, 6), top 0 - made for the unscaled font. With the font
// x k the text sat on the box's bottom edge (1440p: 13 px above the letters, 3 below). The paddings are now x k (DS1's
// BarPad), written into those imm8s whenever k changes.
struct Imm8 { uint32_t va; uint8_t op[2]; int8_t base; };
constexpr Imm8 kBarPads[] = {{0x44908D, {0x83, 0xC1}, 6}, {0x44909C, {0x83, 0xE8}, 4}, {0x44909F, {0x83, 0xC2}, 4}};
bool g_barPadsOk = false;

// Blinking item icon (FUN around 0x423D90: the held item's picture blinking in the view, framed by images 0x1A / 0x19 of
// the HUD sheet [0x62F434]) centres the picture with its SCALED width (FUN_0052b590) but its RAW height (FUN_0052b140,
// 0x423EC6 / 0x423FA2) and places the frame pieces with RAW x offsets (FUN_0052b160, 0x423E48 / 0x423F76) - DS1's
// 0x44BEDC / 0x44BFB8. Those calls get scaled values (record value x the sheet's scale while it scales, as the scaled
// getters do). (Its layout clamp 0x423DF4 was taken for a raw offset by gen_clamps.py and scaled twice - regenerated.)
int Scaled(const uint8_t* sheet, int image, uint32_t field, uint32_t scale) {
    auto records = *reinterpret_cast<const uint8_t* const*>(sheet + 4);
    const int raw = *reinterpret_cast<const int16_t*>(records + image * 0x1C + field);
    if (!*reinterpret_cast<const int*>(sheet + 0x20)) return raw;
    const float v = raw * *reinterpret_cast<const float*>(sheet + scale);
    return static_cast<int>(v < 0 ? v - 0.5f : v + 0.5f);
}
int __fastcall ScaledHeight(const uint8_t* sheet, void*, int image) { return Scaled(sheet, image, 0x10, 0x28); }
int __fastcall ScaledOffsetX(const uint8_t* sheet, void*, int image) { return Scaled(sheet, image, 0x12, 0x24); }

// Loading screen (FUN around 0x40F5xx): the level title ("Al-Hadar, Northern Iraq") is drawn with font [0x62F440] at
// 0x40F64A without setting its sheet's scale (the date / story below set theirs to 100 % = k first, 0x40F6C8) - it kept
// the front-end's 1.0 and came out half the size of the date. Its sheet gets 100 % (-> k) before that draw.
using BoxText = double(__thiscall*)(void*, const wchar_t*, int*, int, int, int, int, int, int, int);
double __fastcall LoadingTitle(void* font, void*, const wchar_t* text, int* rect, int a, int b, int c, int d, int e,
                               int f, int g) {
    if (auto f440 = *reinterpret_cast<uint8_t**>(0x62F440))
        if (auto sheet = *reinterpret_cast<void**>(f440))
            reinterpret_cast<void(__thiscall*)(void*, float)>(0x52B1A0)(sheet, 100.0f);
    return reinterpret_cast<BoxText>(0x527FF0)(font, text, rect, a, b, c, d, e, f, g);
}
}  // namespace

void features::OnHudScale(float k) {
    if (!g_barPadsOk) return;
    for (const auto& p : kBarPads) {
        const int v = static_cast<int>(p.base * k + 0.5f);
        const int8_t b = static_cast<int8_t>(v > 120 ? 120 : v);
        patch::Write(p.va + 2, &b, 1);
    }
}

void features::ApplyHudFixes() {
#ifndef DS_DIST
    ApplyDevProbes();
#endif
    int ok = 0;
    g_barPadsOk = true;
    for (const auto& p : kBarPads) {
        const uint8_t bytes[3] = {p.op[0], p.op[1], static_cast<uint8_t>(p.base)};
        if (!patch::Matches(p.va, bytes, 2)) g_barPadsOk = false;
    }
    if (g_barPadsOk) ++ok;
    else dslog::Write("[fail] HUD fixes: hint bar paddings not recognised");
    if (patch::HookCall(0x423EC6, reinterpret_cast<void*>(&ScaledHeight), 0x52B140) &&
        patch::HookCall(0x423FA2, reinterpret_cast<void*>(&ScaledHeight), 0x52B140) &&
        patch::HookCall(0x423E48, reinterpret_cast<void*>(&ScaledOffsetX), 0x52B160) &&
        patch::HookCall(0x423F76, reinterpret_cast<void*>(&ScaledOffsetX), 0x52B160))
        ++ok;
    if (patch::HookCall(0x40F64A, reinterpret_cast<void*>(&LoadingTitle), 0x527FF0)) ++ok;
    dslog::Write("[ok]   HUD fixes: %d of 3 (hint bar paddings, blinking item icon, loading screen title)", ok);
}

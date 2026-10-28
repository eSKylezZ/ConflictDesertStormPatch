// HUD / menu art scaling. The game sizes its 2D art sheets for 800x600: callers such as FUN_0040c310 pass
// 100 * W / max(800, W) (and the same for H) to the sheet scale setters, so the art only ever SHRINKS (below 800x600)
// and stays 800x600-sized above - tiny at 1440p / 4K.
//
// Setters (sheet object: +0x20 enabled, +0x24 x scale, +0x28 y scale; percent arguments, x 0.01 at 0x5DA74C):
//   FUN_0052b1a0  thiscall(sheet, float s)                       ret 4   - flag 0 at exactly 100 %, else 1
//   FUN_0052b1e0  thiscall(sheet, float sx, float sy, int flag)   ret 0xC - flag 0 at 100/100, else the argument
// Both entries jump to C++ copies that first multiply by k = max(1, min(W/800, H/600)) * HudScalePercent/100, with
// W / H the renderer's size ([0x680038] +0x41A38 / +0x41A3C). (MSVC __fastcall with an unused edx = thiscall here.)
#include <windows.h>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/graphics.h"
#include "features/menus.h"
#include "generated/layout_clamps.h"

#include <algorithm>
#include <cstdlib>
#include <iterator>

// Layout code places HUD pieces at c * max(800, W) / 800 (imul 0x51eb851f / sar 8) times a sheet scale - with the
// scaled sheet that is W/800 * k: too far out on wide screens (4K: portraits outside their frames). The 40 sites whose
// result meets a sheet scale (tools/gen_clamps.py) take the <= 800 path, so only the sheet's k remains.

namespace {
constexpr uint32_t kRenderer = 0x680038, kWidth = 0x41A38, kHeight = 0x41A3C;
constexpr uint32_t kSetUniform = 0x52B1A0, kSetXY = 0x52B1E0;
constexpr uint8_t kEntry[] = {0xD9, 0x44, 0x24, 0x04, 0xD8, 0x0D, 0x4C, 0xA7, 0x5D, 0x00};   // fld [esp+4]; fmul [0x5DA74C]

bool g_clampsOk = false, g_rawClamps = false;
bool g_on = false;    // scaling active (HudScaling; switched live by SetHudScaling - off = the game's own 1.0)
int g_raw800k = 800;
float g_lastK = 1.0f;

// Raw-offset clamp sites take their forced 800 path, whose "mov X, 800" immediate becomes 800 * k: their c * X / 800
// pixels then grow by k like the art (W/800 overshot it on wide screens - the portraits' target icon at 4K).
void UpdateRawClamps(float k) {
    if (k != g_lastK) {
        g_lastK = k;
        features::OnHudScale(k);   // hudfix.cpp: fixed pixel paddings x k
    }
    const int v = static_cast<int>(800.0f * k + 0.5f);
    if (!g_rawClamps || v == g_raw800k) return;
    g_raw800k = v;
    for (const auto& c : gen::kRawClamps) patch::Write(c.imm, &v, 4);
}

// The clamp sites follow the switch: forced 800 path while scaling, the game's c * max(800, W) / 800 without.
void SetClamps(bool on) {
    if (g_clampsOk)
        for (const auto& c : gen::kLayoutClamps) patch::Write(c.jump, on ? c.patched : c.orig, c.len);
    if (g_rawClamps) {
        for (const auto& c : gen::kRawClamps) patch::Write(c.jump, on ? c.patched : c.orig, c.len);
        const int v = 800;
        g_raw800k = 800;
        for (const auto& c : gen::kRawClamps) patch::Write(c.imm, &v, 4);
    }
}

float HudK() {
    auto r = *reinterpret_cast<const uint8_t* const*>(kRenderer);
    if (!r || !g_on) return 1.0f;
    int w = *reinterpret_cast<const int*>(r + kWidth), h = *reinterpret_cast<const int*>(r + kHeight);
    features::SplitViewSize(w, h);   // split screen: the art fits a view (splitscreen.cpp)
    if (w <= 0 || h <= 0) return 1.0f;
    // front-end virtual screen (frontend.cpp): drawn at its design size, the mapping does the scaling
    const float k = graphics::VirtualScreenOn()
                        ? 1.0f
                        : std::max(1.0f, std::min(w / 800.0f, h / 600.0f)) * settings::Get().hudScalePercent / 100.0f;
    UpdateRawClamps(k);
    return k;
}

void Store(uint8_t* sheet, float sx, float sy, int flag) {
    *reinterpret_cast<float*>(sheet + 0x24) = sx * 0.01f;
    *reinterpret_cast<float*>(sheet + 0x28) = sy * 0.01f;
    *reinterpret_cast<int*>(sheet + 0x20) = (sx == 100.0f && sy == 100.0f) ? 0 : flag;
}

// Every sheet a setter scaled, with the percentages the game asked for (before k): RescaleHudSheets puts them at the
// current k again - a HUD size change in the options, and the in-mission menus (frontend.cpp: drawn on the virtual
// screen, k = 1) and back. Forgotten at each level load (sheets of a level may be freed with it).
struct SheetRec {
    uint8_t* sheet;
    float sx, sy;
    int flag;
};
SheetRec g_sheets[64];
int g_sheetCount = 0;
void Remember(uint8_t* sheet, float sx, float sy, int flag) {
    for (int i = 0; i < g_sheetCount; ++i)
        if (g_sheets[i].sheet == sheet) {
            g_sheets[i] = {sheet, sx, sy, flag};
            return;
        }
    if (g_sheetCount < 64) g_sheets[g_sheetCount++] = {sheet, sx, sy, flag};
}

void __fastcall SetUniform(uint8_t* sheet, void*, float s) {
    const float k = HudK();
    Remember(sheet, s, s, 1);
    Store(sheet, s * k, s * k, 1);
}

// Restores of a scale the code read back from the sheet (already x k): stored as given. Text queue (loop at
// 0x50CBE0): each item sets its own percent (100 -> k, fine), then 0x50CD6F puts back the value saved before the loop.
void __fastcall SetUniformRaw(uint8_t* sheet, void*, float s) { Store(sheet, s, s, 1); }

// The small font [0x62F438] (weapon panel's ammo count, statistics tables) is never handed to a scale setter, so its
// sheet stayed at 1.0 while everything else grew. The mission set-up gives the other fonts 100 % (0x40C74E for
// [0x62F440], 0x40C7DA for [0x62F43C]); the first of those calls now also gives the small font's sheet 100 % -> k.
void __fastcall SetFontsUniform(uint8_t* sheet, void* edx, float s) {
    SetUniform(sheet, edx, s);
    auto font = *reinterpret_cast<uint8_t* const*>(0x62F438);
    if (font)
        if (auto smallSheet = *reinterpret_cast<uint8_t* const*>(font)) SetUniform(smallSheet, edx, 100.0f);
}

// The set-up call above happens before the mission's fonts exist, so the small font is (also) fixed where it draws:
// FUN_005294e0 thiscall(font, text, ...) is entered through a stub that first brings that font's sheet to k.
void __stdcall EnsureSmallFont(const uint8_t* font) {
    if (!font || font != *reinterpret_cast<uint8_t* const*>(0x62F438)) return;
    auto sheet = *reinterpret_cast<uint8_t* const*>(font);
    if (!sheet) return;
    const float k = HudK();
    if (*reinterpret_cast<const float*>(sheet + 0x24) != k || *reinterpret_cast<const float*>(sheet + 0x28) != k)
        Store(sheet, 100.0f * k, 100.0f * k, 1);
}
__declspec(naked) void SmallFontStub() {
    __asm {
        pushad
        push ecx
        call EnsureSmallFont
        popad
        sub esp, 0x58               // the 6 replaced bytes of FUN_005294e0
        push ebx
        mov ebx, ecx
        push 0x5294E6
        ret
    }
}

// Soldier panel (FUN_00467910) centres the follow / halt order icon on button image 0x52 with the button's SCALED
// size (FUN_0052b590 / 0052b5e0) but the icon's RAW size (FUN_0052b120 / 0052b140: u16 pixels from the record,
// +0xE width / +0x10 height) - off-centre once the icon sheet is scaled. The two calls (0x467DFB, 0x467E42) get the
// scaled size instead (record size x the sheet's scale while scaling is on, as the scaled getters do).
uint32_t ScaledSize(const uint8_t* sheet, int image, uint32_t field, uint32_t scale) {
    auto records = *reinterpret_cast<const uint8_t* const*>(sheet + 4);
    const uint16_t raw = *reinterpret_cast<const uint16_t*>(records + image * 0x1C + field);
    if (!*reinterpret_cast<const int*>(sheet + 0x20)) return raw;
    return static_cast<uint32_t>(raw * *reinterpret_cast<const float*>(sheet + scale) + 0.5f);
}
uint32_t __fastcall ScaledImageWidth(const uint8_t* sheet, void*, int image) { return ScaledSize(sheet, image, 0xE, 0x24); }
uint32_t __fastcall ScaledImageHeight(const uint8_t* sheet, void*, int image) { return ScaledSize(sheet, image, 0x10, 0x28); }

void __fastcall SetXY(uint8_t* sheet, void*, float sx, float sy, int flag) {
    const float k = HudK();
    Remember(sheet, sx, sy, flag);
    Store(sheet, sx * k, sy * k, flag);
}
#ifndef DS_DIST
// Dev\HudProbe = 1: logs each distinct percentage the text queue (FUN around 0x50CBE0) hands the font sheet - per item
// at 0x50CC08 (item +0xF8) and the restore at 0x50CD6F - to see which values already contain k.
float g_seen[64];
int g_seenCount = 0;
void Note(const char* where, float s) {
    for (int i = 0; i < g_seenCount; ++i)
        if (g_seen[i] == s) return;
    if (g_seenCount < 64) g_seen[g_seenCount++] = s;
    dslog::Write("[dev] HudProbe %s: %.3f (k %.3f)", where, s, HudK());
}
// Dev\TextProbe = 1: text draw FUN_00529490 thiscall(font, const char* text, x, y, flag) - each distinct caller once,
// with its text and the font sheet's x scale ([font] = sheet, +0x24) - which texts miss the HUD scale.
uint32_t g_textCallers[96];
int g_textCallerCount = 0;
void __stdcall LogText(uint32_t ret, const char* text, const uint8_t* font) {
    for (int i = 0; i < g_textCallerCount; ++i)
        if (g_textCallers[i] == ret) return;
    if (g_textCallerCount >= 96) return;
    g_textCallers[g_textCallerCount++] = ret;
    const uint8_t* sheet = font ? *reinterpret_cast<const uint8_t* const*>(font) : nullptr;
    dslog::Write("[dev] TextProbe caller 0x%08X font %p sheet scale %.3f: \"%.40s\"", ret - 5, font,
                 sheet ? *reinterpret_cast<const float*>(sheet + 0x24) : -1.0f, text ? text : "(null)");
}
__declspec(naked) void TextProbeStub() {
    __asm {
        pushad
        push ecx                    // font
        push dword ptr [esp + 0x28] // text (pushad 0x20 + ret 4 + this push 4)
        push dword ptr [esp + 0x28] // return address
        call LogText
        popad
        mov edx, dword ptr [esp + 4]   // the 5 replaced bytes
        push edi
        push 0x529495
        ret
    }
}

void __fastcall ProbeItem(uint8_t* sheet, void* edx, float s) { Note("queue item", s); SetUniform(sheet, edx, s); }
void __fastcall ProbeRestore(uint8_t* sheet, void* edx, float s) { Note("queue restore", s); SetUniformRaw(sheet, edx, s); }
#endif
}  // namespace

namespace {
// Around the in-mission menus the sheets' scale fields themselves are saved and written back (the menus set some
// sheets the mission never set again - the HUD art sheet [0x62F434] to 60 % - and the HUD screen's set-up, run when
// the mission state is entered, lays the soldier panels out from them). Saved: every remembered sheet + the HUD /
// font sheets the game keeps in globals.
struct RawSheet {
    uint8_t* sheet;
    int flag;
    float sx, sy;
};
SheetRec g_saved[64];
RawSheet g_raw[80];
int g_savedCount = 0, g_rawCount = 0;
float g_savedK = 1.0f;
constexpr uint32_t kSheetGlobals[] = {0x62F434, 0x62F4B0, 0x62F4B4};   // sheets
constexpr uint32_t kFontGlobals[] = {0x62F438, 0x62F43C, 0x62F440};    // fonts: [font] = sheet

void SaveRaw(uint8_t* sheet) {
    if (!sheet || g_rawCount >= 80) return;
    for (int i = 0; i < g_rawCount; ++i)
        if (g_raw[i].sheet == sheet) return;
    g_raw[g_rawCount++] = {sheet, *reinterpret_cast<int*>(sheet + 0x20), *reinterpret_cast<float*>(sheet + 0x24),
                           *reinterpret_cast<float*>(sheet + 0x28)};
}
}  // namespace

void features::ForgetHudSheets() { g_sheetCount = g_savedCount = g_rawCount = 0; }
void features::RescaleHudSheets() {
    const float k = HudK();
    for (int i = 0; i < g_sheetCount; ++i)
        Store(g_sheets[i].sheet, g_sheets[i].sx * k, g_sheets[i].sy * k, g_sheets[i].flag);
}
void features::SaveHudSheets() {
    std::copy(g_sheets, g_sheets + g_sheetCount, g_saved);
    g_savedCount = g_sheetCount;
    g_savedK = HudK();
    g_rawCount = 0;
    for (int i = 0; i < g_sheetCount; ++i) SaveRaw(g_sheets[i].sheet);
    for (uint32_t g : kSheetGlobals) SaveRaw(*reinterpret_cast<uint8_t**>(g));
    for (uint32_t g : kFontGlobals)
        if (auto font = *reinterpret_cast<uint8_t**>(g)) SaveRaw(*reinterpret_cast<uint8_t**>(font));
}
void features::RestoreHudSheets() {
    if (!g_savedCount && !g_rawCount) return;
    g_sheetCount = 0;   // the menus' percentages go; the mission's come back
    for (int i = 0; i < g_savedCount; ++i) Remember(g_saved[i].sheet, g_saved[i].sx, g_saved[i].sy, g_saved[i].flag);
    const float f = g_savedK > 0.0f ? HudK() / g_savedK : 1.0f;   // HUD SIZE changed in the menus
    for (int i = 0; i < g_rawCount; ++i) {
        uint8_t* s = g_raw[i].sheet;
        *reinterpret_cast<int*>(s + 0x20) = g_raw[i].flag;
        *reinterpret_cast<float*>(s + 0x24) = g_raw[i].sx * f;
        *reinterpret_cast<float*>(s + 0x28) = g_raw[i].sy * f;
    }
    g_savedCount = g_rawCount = 0;
}

void features::ApplyHud() {
    ApplyHudFixes();   // the DS1 fixes + dev probes (hudfix.cpp)
    // Installed whatever HudScaling says: off = k 1 and the clamp sites as the game has them (the game's own look),
    // so the in-game option (SetHudScaling) can switch it live.
    if (!patch::Matches(kSetUniform, kEntry, sizeof kEntry) || !patch::Matches(kSetXY, kEntry, sizeof kEntry)) {
        dslog::Write("[fail] HUD scaling: unexpected bytes at the sheet scale setters - skipped");
        return;
    }
    patch::WriteJump(kSetUniform, reinterpret_cast<void*>(&SetUniform));
    patch::WriteJump(kSetXY, reinterpret_cast<void*>(&SetXY));
    patch::HookCall(0x50CD6F, reinterpret_cast<void*>(&SetUniformRaw), kSetUniform);   // text queue restore
    patch::HookCall(0x40C74E, reinterpret_cast<void*>(&SetFontsUniform), kSetUniform); // + the small font
    patch::HookCall(0x467DFB, reinterpret_cast<void*>(&ScaledImageWidth), 0x52B120);   // follow / halt icon
    patch::HookCall(0x467E42, reinterpret_cast<void*>(&ScaledImageHeight), 0x52B140);
    constexpr uint8_t kText2Entry[] = {0x83, 0xEC, 0x58, 0x53, 0x8B, 0xD9};   // sub esp,0x58; push ebx; mov ebx,ecx
    if (patch::Matches(0x5294E0, kText2Entry, sizeof kText2Entry)) {
        patch::WriteJump(0x5294E0, reinterpret_cast<void*>(&SmallFontStub));
        patch::Write(0x5294E5, "\x90", 1);
    }
    g_clampsOk = true;
    for (const auto& c : gen::kLayoutClamps)
        if (!patch::Matches(c.jump, c.orig, c.len) && !patch::Matches(c.jump, c.patched, c.len)) {
            dslog::Write("[fail] HUD layout clamps: unexpected bytes at 0x%08X (clamp 0x%08X) - none applied", c.jump, c.site);
            g_clampsOk = false;
            break;
        }
    constexpr uint8_t k800[] = {0x20, 0x03, 0x00, 0x00};
    g_rawClamps = true;
    for (const auto& c : gen::kRawClamps)
        if ((!patch::Matches(c.jump, c.orig, c.len) && !patch::Matches(c.jump, c.patched, c.len)) ||
            !patch::Matches(c.imm, k800, 4)) {
            dslog::Write("[fail] HUD raw offsets: unexpected bytes at 0x%08X (clamp 0x%08X) - none applied", c.jump, c.site);
            g_rawClamps = false;
            break;
        }
    g_on = settings::Get().hudScaling;
    if (g_on) SetClamps(true);
#ifndef DS_DIST
    // Dev\ClampSkip "3,7,12": leave those clamp sites (table index) as the game has them - bisecting which site places what
    char list[256] = {};
    DWORD ls = sizeof list;
    if (g_on && g_clampsOk &&
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", "ClampSkip", RRF_RT_REG_SZ, nullptr, list, &ls) ==
            ERROR_SUCCESS)
        for (char* p = list; *p;) {
            char* end;
            const long i = std::strtol(p, &end, 10);
            if (end == p) break;
            if (i >= 0 && i < static_cast<long>(std::size(gen::kLayoutClamps)))
                patch::Write(gen::kLayoutClamps[i].jump, gen::kLayoutClamps[i].orig, gen::kLayoutClamps[i].len);
            p = *end ? end + 1 : end;
        }
    DWORD probe = 0, size = sizeof probe;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", "HudProbe", RRF_RT_REG_DWORD, nullptr, &probe,
                     &size) == ERROR_SUCCESS && probe) {
        patch::HookCall(0x50CC08, reinterpret_cast<void*>(&ProbeItem), kSetUniform);
        patch::HookCall(0x50CD6F, reinterpret_cast<void*>(&ProbeRestore), reinterpret_cast<uint32_t>(&SetUniformRaw));
    }
    size = sizeof probe;
    probe = 0;
    constexpr uint8_t kTextEntry[] = {0x8B, 0x54, 0x24, 0x04, 0x57};
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", "TextProbe", RRF_RT_REG_DWORD, nullptr, &probe,
                     &size) == ERROR_SUCCESS && probe && patch::Matches(0x529490, kTextEntry, sizeof kTextEntry)) {
        patch::WriteJump(0x529490, reinterpret_cast<void*>(&TextProbeStub));
        dslog::Write("[dev] TextProbe on");
    }
#endif
    dslog::Write("[ok]   HUD scaling: sheet scale setters x k (HudScaling %d, HudScalePercent %u; %u layout clamps, %u raw offsets)",
                 g_on, settings::Get().hudScalePercent, g_clampsOk ? static_cast<unsigned>(std::size(gen::kLayoutClamps)) : 0u,
                 g_rawClamps ? static_cast<unsigned>(std::size(gen::kRawClamps)) : 0u);
    menus::HudOption opt;   // OPTIONS > HUD SCALING / HUD SIZE (menus.cpp)
    opt.scaling = &HudScaling;
    opt.percent = &HudScalePercent;
    opt.set = &SetHudScale;
    menus::SetHudOption(opt);
}

// ---- the in-game option (menus.cpp rows HUD SCALING / HUD SIZE) ----------------------------------------------------
bool features::HudScaling() { return g_on; }
uint32_t features::HudScalePercent() { return settings::Get().hudScalePercent; }

// Live: the setting is saved, the clamp sites follow, every remembered sheet gets the new k at once (or, inside the
// in-mission menus = virtual screen, when the menu closes - RestoreHudSheets).
void features::SetHudScale(bool on, uint32_t percent) {
    percent = std::clamp<uint32_t>(percent, 25, 400);
    settings::Values v = settings::Get();
    if (v.hudScaling == on && v.hudScalePercent == percent && g_on == on) return;
    v.hudScaling = on;
    v.hudScalePercent = percent;
    settings::Save(v);
    if (g_on != on) {
        g_on = on;
        SetClamps(on);
    }
    if (!graphics::VirtualScreenOn()) RescaleHudSheets();
    const float k = HudK();
    if (!on) UpdateRawClamps(1.0f);   // (the paddings back to the game's)
    dslog::Write("HUD: scaling %s, size %u %% (k %.3f)", on ? "on" : "off", percent, k);
}

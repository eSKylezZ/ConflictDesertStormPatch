// Menu text that fits its box. The front-end lists centre each text in its box with the font's width measure and no
// limit, so a long label or value ran over the box edge and under the choice arrows (OPTIONS > VIDEO, DESERTSTORMFIX,
// translated texts). A text wider than its box is drawn condensed instead: the font sheet's X scale (+0x24, read by the
// width measure FUN_0053A0F0 -> FUN_0053A400 and by the glyph draw) is lowered for that one call, so the game's own
// centring sees the narrower width. Never below kMinScale of the normal width (beyond that it overflows as before).
// At 800x600 (HUD scale 1) the game switches the sheet's scaling off (+0x20 bit 0) and ignores +0x24 - it is switched on
// at 1.0 for the condensed draw; the sheet's whole state is put back after each call.
//   FUN_00456040  thiscall(screen, id, x, y) ret 0xC: a choice row - label in the left half of the row (list +0x32
//                 width), then the value through the item's draw (vtable +0x4C = FUN_00456A70)
//   FUN_00456A70  thiscall(screen, item, int rect[4], font, selected) ret 0x10: an item's text centred / aligned in
//                 rect (left [0], right [2]); plain buttons (main menu, OPTIONS) and choice values
// Item: +8 text hash (+0x20 kind 0), +0x20 kind 1 = choice: texts +0x28[+0x24]; +0x14 label hash (choice rows).
// Both entries are wrapped (broker entry hooks: the 5-byte prologues run in its trampolines). fontsharp.cpp picks its sharp copy from the larger
// of the X / Y scales, so condensed text stays sharp.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kRowDraw = 0x456040, kItemDraw = 0x456A70;
constexpr uint32_t kFindItem = 0x4E6EC0, kMeasure = 0x53A0F0, kLookup = 0x4F6730, kStrings = 0x60EDCC;
constexpr float kMinScale = 0.6f;
constexpr float kLabelShare = 0.78f;     // of the left half: the label box's rim and margins
constexpr float kChoicePad = 0.15f;      // each side of a choice value: the < > arrows
constexpr float kPlainPad = 0.08f;       // each side of a plain button's text

const uint8_t kRowEntry[] = {0x83, 0xEC, 0x2C, 0x53, 0x55};   // sub esp, 0x2c; push ebx; push ebp
const uint8_t kItemEntry[] = {0x83, 0xEC, 0x0C, 0x53, 0x56};  // sub esp, 0xc; push ebx; push esi

using RowDrawFn = void(__thiscall*)(void*, int, uint32_t, uint32_t);
using ItemDrawFn = void(__thiscall*)(void*, void*, int*, void*, int);
RowDrawFn g_rowDraw = nullptr;    // next subscriber / broker trampoline (patch::HookFunction)
ItemDrawFn g_itemDraw = nullptr;

// Sheet scale state: +0x20 bit 0 = scaling on (off at 800x600, where the HUD scale is 1: then +0x24 / +0x28 are ignored).
struct State {
    float x, y;
    uint32_t flags;
};
uint8_t* g_labelSheet = nullptr;  // while a row draw has condensed its label: that sheet
State g_labelNormal{};            //   and its state before (the value's own fit starts from it)

template <class T>
T& At(const void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(const_cast<uint8_t*>(static_cast<const uint8_t*>(base)) + offset);
}

State Get(uint8_t* sheet) { return {At<float>(sheet, 0x24), At<float>(sheet, 0x28), At<uint32_t>(sheet, 0x20)}; }
void Set(uint8_t* sheet, const State& s) {
    At<float>(sheet, 0x24) = s.x;
    At<float>(sheet, 0x28) = s.y;
    At<uint32_t>(sheet, 0x20) = s.flags;
}

const char* Text(uint32_t hash) {
    void* strings = *reinterpret_cast<void**>(kStrings);
    if (!strings || hash == 0xFFFFFFFF) return nullptr;
    return reinterpret_cast<const char*(__thiscall*)(void*, uint32_t)>(kLookup)(strings, hash);
}

int Width(void* font, const char* text) {
    return reinterpret_cast<int16_t(__thiscall*)(void*, const char*)>(kMeasure)(font, text);
}

// The sheet put in `normal`; when `text` is wider than `avail` there, its X scale lowered so it fits (scaling switched
// on at 1.0 if it was off).
void Fit(uint8_t* sheet, void* font, const char* text, float avail, const State& normal) {
    Set(sheet, normal);
    if (!text || !*text || avail <= 0) return;
    const int w = Width(font, text);
    if (w <= avail) return;
    const bool on = (normal.flags & 1) != 0;
    State c{(on ? normal.x : 1.0f) * std::max(kMinScale, avail / static_cast<float>(w)), on ? normal.y : 1.0f,
            normal.flags | 1};
    Set(sheet, c);
}

void __fastcall RowDraw(void* screen, void*, int id, uint32_t x, uint32_t y) {
    void* list = At<void*>(screen, 8);
    void* font = list ? At<void*>(list, 4) : nullptr;
    uint8_t* sheet = font ? At<uint8_t*>(font, 0) : nullptr;
    void* item = sheet ? reinterpret_cast<void*(__thiscall*)(void*, int)>(kFindItem)(list, static_cast<int16_t>(id)) : nullptr;
    if (!item) return g_rowDraw(screen, id, x, y);
    const State entry = Get(sheet);
    Fit(sheet, font, Text(At<uint32_t>(item, 0x14)), At<int16_t>(list, 0x32) * 0.5f * kLabelShare, entry);
    g_labelSheet = sheet;
    g_labelNormal = entry;
    g_rowDraw(screen, id, x, y);
    g_labelSheet = nullptr;
    Set(sheet, entry);
}

void __fastcall ItemDraw(void* screen, void*, void* item, int* rect, void* font, int selected) {
    uint8_t* sheet = font ? At<uint8_t*>(font, 0) : nullptr;
    const int kind = item ? At<int>(item, 0x20) : -1;
    if (!sheet || !rect || (kind != 0 && kind != 1)) return g_itemDraw(screen, item, rect, font, selected);
    const uint32_t hash = kind == 0 ? At<uint32_t>(item, 8) : At<uint32_t>(item, 0x28 + At<int8_t>(item, 0x24) * 4);
    const float avail = (rect[2] - rect[0]) * (1.0f - 2.0f * (kind == 1 ? kChoicePad : kPlainPad));
    const State entry = Get(sheet);
    Fit(sheet, font, Text(hash), avail, g_labelSheet == sheet ? g_labelNormal : entry);
    g_itemDraw(screen, item, rect, font, selected);
    Set(sheet, entry);
}

}  // namespace

void features::ApplyTextFit() {
    if (g_rowDraw) return;
    if (!patch::Matches(kRowDraw, kRowEntry, sizeof kRowEntry) || !patch::Matches(kItemDraw, kItemEntry, sizeof kItemEntry)) {
        dslog::Write("[fail] Menu text fitting: unexpected code - not applied");
        return;
    }
    const bool ok = patch::HookFunction(kRowDraw, reinterpret_cast<const void*>(&RowDraw),
                                        reinterpret_cast<void**>(&g_rowDraw), kRowEntry, sizeof kRowEntry) &&
                    patch::HookFunction(kItemDraw, reinterpret_cast<const void*>(&ItemDraw),
                                        reinterpret_cast<void**>(&g_itemDraw), kItemEntry, sizeof kItemEntry);
    dslog::Write(ok ? "[ok]   Menu text fitting: long labels / values condensed to their boxes"
                    : "[fail] Menu text fitting");
}

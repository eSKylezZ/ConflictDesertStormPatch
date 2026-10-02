// Options -> CONTROLS: KEYBOARD & MOUSE (the game's KEY ASSIGNMENT + MOUSE OPTIONS) and CONTROLLER (per-player pad
// layouts on a picture of the controller, console style).
//
// The game's Options item 0x7F7 "CONTROLLER" (text hash 0xB89EBF3E, pushed at 0x471BCC/0x471BDB) opens state 0x1E,
// CONTROLLER OPTIONS (class vtable 0x5D8510, ctor 0x409B90, set-up 0x409C90: items 0x817 KEY ASSIGNMENT (0xE443B70B,
// 0x409D7B) and 0x818 MOUSE OPTIONS (0xAC4ABB64, 0x409DAC), title 0xEB9A212D (0x409DD2 set-up, 0x409E56 draw);
// selection 0x409DF0: 0x817 -> state 0x20 (push at 0x409E15), 0x818 -> 0x1F (0x409E11), back -> 4).
// Here: those texts become CONTROLS / KEYBOARD & MOUSE / CONTROLLER (our hashes, answered by ControlsText), the items
// open our states 0x3F (KEYBOARD & MOUSE) and 0x40 (CONTROLLER), and KEY ASSIGNMENT / MOUSE OPTIONS go back to 0x3F
// (their `push 0x1E` at 0x445B0F / 0x45C877). Our screens are objects of the same class with copied vtables, served
// by the front-end's state -> screen lookup (coop.cpp's hook asks ControlsScreen for states >= 0x3F).
//
// PROFILES (state 0x41, third item of KEYBOARD & MOUSE): four local slots holding a copy of the keyboard and mouse
// entries of binding set 0 (registry Enhancements\KeyProfile<n>, int32[80][3], pad entries -1). Load writes one back
// into set 0 and saves it like KEY ASSIGNMENT does (FUN_00445BE0 -> current.key); co-op keyboard players can pick
// one on the join screen (applied to their set at every level load, ApplyCoopKeyProfiles).
//
// CONTROLLER screen: player 1-4, layout (DEFAULT / CLASSIC / SOUTHPAW / CUSTOM, padlayout.cpp), vibration and
// adaptive triggers on/off, context tab
// (ON FOOT / ORDERS / INVENTORY) selectors; the controller picture (PS5 or Xbox Series, Zacksly art, CC BY 3.0 -
// scripts/gen_controller_art.py) with a line from each button to the action it has in that context. Choosing an
// action and pressing a pad button moves it there (swapping with whatever used that button). Input is read
// directly (keyboard + every pad through features::ReadJoystick, so buttons are in the game's numbering).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"
#include "features/padlayout.h"
#include "generated/controller_art.h"

namespace {
constexpr uint32_t kSetState = 0x47EF20;       // cdecl (state)
constexpr uint32_t kBackPressed = 0x478380;    // cdecl (0) -> nonzero when the menu back action fired
constexpr uint32_t kListSelected = 0x4E5D80;   // list thiscall () -> activated item id (short)
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kItemCtor = 0x4E4F90, kItemSet = 0x4E4FB0;
constexpr uint32_t kDrawPanel = 0x456EB0, kDrawTitle = 0x456F40;
constexpr uint32_t kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kMainMenuScreen = 0x617A00;
constexpr uint32_t kMainMenuEnter = 0x4153B0;     // main menu vtable +0x18
constexpr uint32_t kOptCtor = 0x409B90, kOptVtable = 0x5D8510, kOptSetup = 0x409C90, kOptItemDrawCb = 0x409E90;
constexpr uint32_t kFrontEndState = 0x617C18, kGameWindow = 0x606A60, kJoystickCount = 0x754BE8;
constexpr uint32_t kRenderer = 0x63C924;
constexpr uint32_t kStateControls = 0x1E, kStateMouse = 0x1F, kStateKeys = 0x20, kStateKbm = 0x3F, kStateController = 0x40,
                   kStateProfiles = 0x41;
constexpr uint16_t kItemKeys = 0x817, kItemMouse = 0x818, kItemProfiles = 0x819, kItemProfile0 = 0x830;
constexpr uint32_t kBindingTable = 0x60687C, kSaveKeys = 0x445BE0;  // table {sets, slots, actions, data}; save set 0
constexpr int kProfiles = 4, kActions = 80, kBindSlots = 3;
constexpr char kKey[] = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Enhancements";
constexpr uint32_t kHashKeys = 0xE443B70B, kHashMouse = 0xAC4ABB64, kHashCtrlOptions = 0xEB9A212D, kHashCtrl = 0xB89EBF3E;
constexpr int kVtableEntries = 21;

enum : uint32_t { kTextControls = 0xC0080001, kTextKbm, kTextController, kTextProfiles, kTextProfile0 = 0xC0080010 };

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
void SetState(uint32_t s) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(s); }
bool BackPressed() { return reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0) != 0; }
void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }
void SetItem(void* item, uint16_t id, uint32_t hash) {
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    reinterpret_cast<Fn>(kItemSet)(item, id, hash, 0, 0x24, 0, 0, kOptItemDrawCb, 0);
}
void AddToList(void* list, void* item) {
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, item);
}
void Layout(void* screen, uint32_t title) {
    using Fn = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(screen))[20])(screen, title, 0, 0, 0x7FFF);
}
int ScreenW() { return *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(kRenderer) + 0x40688); }
int ScreenH() { return *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(kRenderer) + 0x4068C); }

// A list with `items` pooled item objects at screen +8 (as the class's own create 0x409BE0 does: large font).
bool CreateList(void* screen, int items) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, *reinterpret_cast<void**>(kLargeFont), *reinterpret_cast<void**>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, items)) return false;
    for (int i = 0; i < items; ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x24, 0x1E);
        if (item) reinterpret_cast<void*(__thiscall*)(void*)>(kItemCtor)(item);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    }
    return true;
}

// ---- KEYBOARD & MOUSE (state 0x3F) -------------------------------------------------------------------------------

uint8_t g_kbm[0x20], g_ctl[0x20];
void* g_kbmVtable[kVtableEntries];
void* g_ctlVtable[kVtableEntries];
bool g_built = false;

bool __fastcall KbmCreate(void* screen, void*) { return CreateList(screen, 3); }

// The class's set-up (list layout + two items, which now carry the CONTROLS texts), then the game's own texts.
void __fastcall KbmSetup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);
    void* list = At<void*>(screen, 8);
    SetItem(ItemAt(list, 0), kItemKeys, kHashKeys);
    SetItem(ItemAt(list, 1), kItemMouse, kHashMouse);
    SetItem(ItemAt(list, 2), kItemProfiles, kTextProfiles);
    AddToList(list, ItemAt(list, 2));
    // The list area was sized for two rows (list +0x38 row pitch, +0x40 area height, +0x42 visible rows).
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = 3;
    Layout(screen, kTextKbm);
}

int __fastcall KbmUpdate(void* screen, void*) {
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(At<void*>(screen, 8));
    if (id == kItemKeys) SetState(kStateKeys);
    else if (id == kItemMouse) SetState(kStateMouse);
    else if (id == kItemProfiles) SetState(kStateProfiles);
    if (BackPressed()) SetState(kStateControls);
    return 0;
}

void __fastcall KbmDraw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kTextKbm, 1);
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
}

// ---- PROFILES (state 0x41) ---------------------------------------------------------------------------------------

using KeyProfile = int32_t[kActions][kBindSlots];

int32_t* BindingSet(int set) {
    auto* table = *reinterpret_cast<uint32_t**>(kBindingTable);
    if (!table || !table[3] || static_cast<int>(table[0]) <= set || table[1] != kBindSlots || table[2] != kActions)
        return nullptr;
    return reinterpret_cast<int32_t*>(table[3]) + set * kBindSlots * kActions;
}
bool IsKeyOrMouse(int32_t e) { return e >= 0 && ((e >> 16) == 0 || (e >> 16) == 4); }

bool ReadProfile(int n, KeyProfile& p) {
    char name[32];
    snprintf(name, sizeof name, "KeyProfile%d", n + 1);
    DWORD size = sizeof p;
    return RegGetValueA(HKEY_LOCAL_MACHINE, kKey, name, RRF_RT_REG_BINARY, nullptr, &p, &size) == ERROR_SUCCESS &&
           size == sizeof p;
}

void WriteProfile(int n, const KeyProfile* p) {
    HKEY key = nullptr;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    char name[32];
    snprintf(name, sizeof name, "KeyProfile%d", n + 1);
    if (p) RegSetValueExA(key, name, 0, REG_BINARY, reinterpret_cast<const BYTE*>(p), sizeof *p);
    else RegDeleteValueA(key, name);
    RegCloseKey(key);
}

// Keyboard / mouse entries of a profile into a binding set, keeping the set's pad entries.
bool ApplyProfile(int set, int n) {
    KeyProfile p;
    int32_t* data = BindingSet(set);
    if (!data || !ReadProfile(n, p)) return false;
    for (int a = 0; a < kActions; ++a) {
        int32_t keep[kBindSlots];
        int count = 0;
        for (int k = 0; k < kBindSlots; ++k)
            if (IsKeyOrMouse(p[a][k])) keep[count++] = p[a][k];
        for (int k = 0; k < kBindSlots; ++k) {
            const int32_t e = data[k * kActions + a];
            if (e >= 0 && !IsKeyOrMouse(e) && count < kBindSlots) keep[count++] = e;
        }
        for (int k = 0; k < kBindSlots; ++k) data[k * kActions + a] = k < count ? keep[k] : -1;
    }
    return true;
}

uint8_t g_prof[0x20];
void* g_profVtable[kVtableEntries];
char g_status[96] = "";
DWORD g_statusTime = 0;
uint32_t g_profPrevKeys = 0, g_profPrevPad = 0;

void Status(const char* text) {
    strcpy_s(g_status, text);
    g_statusTime = GetTickCount();
}

bool __fastcall ProfCreate(void* screen, void*) { return CreateList(screen, kProfiles); }

void __fastcall ProfSetup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);
    void* list = At<void*>(screen, 8);
    for (int i = 0; i < kProfiles; ++i) {
        SetItem(ItemAt(list, i), static_cast<uint16_t>(kItemProfile0 + i), kTextProfile0 + i);
        if (i >= 2) AddToList(list, ItemAt(list, i));
    }
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + 2 * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = kProfiles;
    Layout(screen, kTextProfiles);
    g_status[0] = 0;
    g_profPrevKeys = ~0u;
    g_profPrevPad = ~0u;
}

int __fastcall ProfUpdate(void* screen, void*) {
    void* list = At<void*>(screen, 8);
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(list);
    const int current = At<int16_t>(list, 0x28);  // highlighted row
    char text[96];
    if (id >= kItemProfile0 && id < kItemProfile0 + kProfiles) {
        const int n = id - kItemProfile0;
        if (ApplyProfile(0, n)) {
            reinterpret_cast<int(__cdecl*)()>(kSaveKeys)();
            snprintf(text, sizeof text, "PROFILE %d LOADED", n + 1);
        } else {
            snprintf(text, sizeof text, "PROFILE %d IS EMPTY", n + 1);
        }
        Status(text);
    }
    // Save (S / Square / X) and clear (Delete / Triangle / Y) on the highlighted row.
    uint32_t keys = 0, pad = 0;
    if (features::GameFocused()) {
        if (features::KeyHeld('S')) keys |= 1;
        if (features::KeyHeld(VK_DELETE)) keys |= 2;
        const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), 8);
        for (int j = 0; j < count; ++j) pad |= features::ReadJoystick(j);
    }
    const uint32_t keyEdge = keys & ~g_profPrevKeys, padEdge = pad & ~g_profPrevPad;
    g_profPrevKeys = keys;
    g_profPrevPad = pad;
    if (current >= 0 && current < kProfiles) {
        if (keyEdge & 1 || padEdge & (1u << 3)) {
            KeyProfile p;
            if (const int32_t* data = BindingSet(0)) {
                for (int a = 0; a < kActions; ++a)
                    for (int k = 0; k < kBindSlots; ++k) {
                        const int32_t e = data[k * kActions + a];
                        p[a][k] = IsKeyOrMouse(e) ? e : -1;
                    }
                WriteProfile(current, &p);
                snprintf(text, sizeof text, "CURRENT KEYS SAVED TO PROFILE %d", current + 1);
                Status(text);
            }
        } else if (keyEdge & 2 || padEdge & (1u << 0)) {
            WriteProfile(current, nullptr);
            snprintf(text, sizeof text, "PROFILE %d CLEARED", current + 1);
            Status(text);
        }
    }
    if (BackPressed()) SetState(kStateKbm);
    return 0;
}

int DrawPrompt(int x, int y, overlay::Icon icon, const char* text, int iconSize);  // below

void __fastcall ProfDraw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kTextProfiles, 1);
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
    void* font = overlay::MenuFont();
    const int w = ScreenW(), h = ScreenH();
    if (g_status[0] && GetTickCount() - g_statusTime < 4000) {
        const float bottom = At<float>(screen, 0x14);
        overlay::DrawLabel(font, g_status, (w - overlay::TextWidth(font, g_status)) / 2,
                           static_cast<int>(bottom + h * 0.045f), 0xFFFFD040);
    }
    const int ps = std::max(12, overlay::TextWidth(font, "M") * 3 / 2);
    const bool pad = !features::KeyboardInUse();
    const bool xbox = features::PadStyleXbox();
    const int y = static_cast<int>(h * 0.925f);
    int x = static_cast<int>(w * 0.06f);
    using overlay::Icon;
    x = DrawPrompt(x, y, pad ? (xbox ? Icon::XbCross : Icon::PsCross) : Icon::KbEnter, "Load", ps);
    x = DrawPrompt(x, y, pad ? (xbox ? Icon::XbSquare : Icon::PsSquare) : Icon::KbS, "Save current keys", ps);
    x = DrawPrompt(x, y, pad ? (xbox ? Icon::XbTriangle : Icon::PsTriangle) : Icon::KbDelete, "Clear", ps);
    DrawPrompt(x, y, pad ? (xbox ? Icon::XbCircle : Icon::PsCircle) : Icon::KbEscape, "Back", ps);
}

// ---- CONTROLLER (state 0x40) -------------------------------------------------------------------------------------

enum Tab { kOnFoot, kOrders, kInventory, kTabs };
struct ActionRow {
    int action;
    const char* name;
};
const ActionRow kOnFootRows[] = {
    {0, "Fire"}, {2, "Action / use"}, {4, "Go"}, {5, "Stand / crouch / prone"}, {43, "Inventory"},
    {26, "Orders"}, {16, "Previous target"}, {17, "Next target"}, {1, "Next soldier"}, {3, "Previous soldier"},
    {20, "Aim mode"}, {25, "Zoom"}, {7, "Objectives"}, {6, "Pause"},
};
const ActionRow kOrderRows[] = {
    {27, "Previous order"}, {28, "Next order"}, {29, "Follow me"}, {30, "Hold position"}, {31, "Advance"},
    {32, "All: follow / hold"}, {35, "All: go prone"}, {36, "Fire at will on / off"},
};
const ActionRow kInventoryRows[] = {
    {44, "Previous item"}, {45, "Next item"}, {46, "Use / equip"}, {49, "Drop"}, {50, "Give / take weapon"},
    {51, "Give / take ammo"},
};
struct TabInfo {
    const char* name;
    const ActionRow* rows;
    int count;
    int moveAction;  // the axis action whose stick is "move" (its codes tell left or right stick)
    const char* moveName;
    const char* lookName;
};
const TabInfo kTabInfo[kTabs] = {
    {"ON FOOT", kOnFootRows, static_cast<int>(std::size(kOnFootRows)), 12, "Move", "Look / aim"},
    {"ORDERS", kOrderRows, static_cast<int>(std::size(kOrderRows)), 40, "Point advance", "Turn view"},
    {"INVENTORY", kInventoryRows, static_cast<int>(std::size(kInventoryRows)), 12, "Move", "Look / aim"},
};
const char* const kPresetNames[] = {"DEFAULT", "CLASSIC", "SOUTHPAW", "CUSTOM"};

enum Input : uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kAccept = 16, kBack = 32, kReset = 64, kView = 128 };

// Where each focusable entry was drawn last frame (screen pixels) - the directions move to the nearest one.
struct FocusSpot {
    int id;
    float x, y;
};

struct ControllerScreen {
    int player = 0;
    int tab = kOnFoot;
    int cursor = 0;  // 0 player, 1 layout, 2 vibration, 3 adaptive triggers, 4 tab, 5 deadzone, 6 look sensitivity,
                     // 7 gyro aim, 8.. action rows of the tab
    bool editing = false;  // a selector is open: Left/Right change its value
    bool xboxView = false;
    bool keyboardPrompts = false;
    bool capturing = false;
    DWORD captureStart = 0;
    uint32_t prevPad[8] = {};
    uint32_t prevKeys = 0;
    uint32_t captureRelease = 0;  // pads must release everything before a capture counts
    FocusSpot spots[40] = {};
    int spotCount = 0;
} g_cs;

constexpr int kHeaderRows = 8;

void AddSpot(int id, float x, float y) {
    if (g_cs.spotCount < static_cast<int>(std::size(g_cs.spots))) g_cs.spots[g_cs.spotCount++] = {id, x, y};
}

// The entry nearest to the current one in a direction (kUp/kDown/kLeft/kRight); the current one if there is none.
// Distance across the direction counts double, so a column / row is followed before jumping sideways.
int Neighbour(int from, uint32_t dir) {
    const FocusSpot* f = nullptr;
    for (int i = 0; i < g_cs.spotCount; ++i)
        if (g_cs.spots[i].id == from) f = &g_cs.spots[i];
    if (!f) return from;
    const bool vertical = dir == kUp || dir == kDown;
    // Left / Right: an entry on the same row first (selector rows); else any within a narrow cone (callout columns).
    for (int pass = vertical ? 1 : 0; pass < 2; ++pass) {
        int best = from;
        float bestScore = 1e30f;
        for (int i = 0; i < g_cs.spotCount; ++i) {
            const FocusSpot& s = g_cs.spots[i];
            if (s.id == from) continue;
            const float dx = s.x - f->x, dy = s.y - f->y;
            const float along = dir == kUp ? -dy : dir == kDown ? dy : dir == kLeft ? -dx : dx;
            const float across = vertical ? std::abs(dx) : std::abs(dy);
            if (along < 2.0f) continue;
            if (!vertical && across > (pass == 0 ? 4.0f : along / 2.0f)) continue;
            const float score = along + 2.0f * across;
            if (score < bestScore) bestScore = score, best = s.id;
        }
        if (best != from) return best;
    }
    return from;
}

// Newly pressed menu inputs from the keyboard and every pad (edges); pad buttons pressed this frame in `padDown`.
uint32_t ReadInput(uint32_t& padDown, int& padUsed) {
    padDown = 0;
    padUsed = -1;
    if (!features::GameFocused()) return 0;
    uint32_t in = 0;
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), 8);
    for (int j = 0; j < count; ++j) {
        const uint32_t now = features::ReadJoystick(j);
        const uint32_t edge = now & ~g_cs.prevPad[j];
        g_cs.prevPad[j] = now;
        if (!edge) continue;
        padUsed = j;
        padDown |= edge & 0xFFFF;
        if (edge & (1u << 12 | 1u << 16)) in |= kUp;
        if (edge & (1u << 14 | 1u << 18)) in |= kDown;
        if (edge & (1u << 15 | 1u << 19)) in |= kLeft;
        if (edge & (1u << 13 | 1u << 17)) in |= kRight;
        if (edge & (1u << 2)) in |= kAccept;
        if (edge & (1u << 1)) in |= kBack;
        if (edge & (1u << 3)) in |= kReset;
        if (edge & (1u << 0)) in |= kView;
    }
    static const int keys[][2] = {{VK_UP, kUp}, {VK_DOWN, kDown}, {VK_LEFT, kLeft}, {VK_RIGHT, kRight},
                                  {VK_RETURN, kAccept}, {VK_ESCAPE, kBack}, {VK_DELETE, kReset}, {VK_BACK, kReset},
                                  {VK_TAB, kView}};
    uint32_t keysNow = 0;
    for (auto& k : keys)
        if (features::KeyHeld(k[0])) keysNow |= k[1];
    const uint32_t keyEdge = keysNow & ~g_cs.prevKeys;
    g_cs.prevKeys = keysNow;
    if (keyEdge) g_cs.keyboardPrompts = true;
    if (padUsed >= 0) {
        g_cs.keyboardPrompts = false;
        g_cs.xboxView = features::JoystickType(padUsed) == features::PadType::Xbox ? true
                        : features::JoystickType(padUsed) == features::PadType::PlayStation ? false
                                                                                            : g_cs.xboxView;
    }
    return in | keyEdge;
}

void ChangeLayout(int delta) {
    const int n = static_cast<int>(padlayout::Preset::Count);
    int p = static_cast<int>(padlayout::PresetOf(g_cs.player));
    p = ((p + delta) % n + n) % n;
    if (p == static_cast<int>(padlayout::Preset::Custom)) p = ((p + delta) % n + n) % n;  // Custom only by editing
    const auto preset = static_cast<padlayout::Preset>(p);
    padlayout::Set(g_cs.player, preset, padlayout::Make(preset));
}

bool __fastcall CtlCreate(void* screen, void*) { return CreateList(screen, 1); }

void __fastcall CtlSetup(void* screen, void*) {
    g_cs.cursor = kHeaderRows;
    g_cs.capturing = false;
    g_cs.editing = false;
    g_cs.spotCount = 0;
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), 8);
    for (int j = 0; j < count; ++j) g_cs.prevPad[j] = features::ReadJoystick(j);  // ignore what is held on entry
    g_cs.prevKeys = ~0u;
    const int w = ScreenW(), h = ScreenH();
    At<float>(screen, 0x10) = h * 0.10f;  // panel: top, bottom, left, right
    At<float>(screen, 0x14) = h * 0.81f;
    At<float>(screen, 0x18) = w * 0.06f;
    At<float>(screen, 0x1C) = w * 0.94f;
}

int __fastcall CtlUpdate(void*, void*) {
    uint32_t padDown = 0;
    int padUsed = -1;
    const uint32_t in = ReadInput(padDown, padUsed);
    const TabInfo& tab = kTabInfo[g_cs.tab];
    if (g_cs.capturing) {
        // Wait until the button that started the capture is released, then take the next pad button.
        uint32_t held = 0;
        for (uint32_t m : g_cs.prevPad) held |= m;
        if (g_cs.captureRelease) g_cs.captureRelease &= held;
        else if (padDown) {
            int button = 0;
            while (!(padDown & (1u << button))) ++button;
            padlayout::Layout l = padlayout::Get(g_cs.player);
            padlayout::Assign(l, tab.rows[g_cs.cursor - kHeaderRows].action, button);
            padlayout::Set(g_cs.player, padlayout::Preset::Custom, l);
            g_cs.capturing = false;
            return 0;
        }
        if ((in & kBack && !padDown) || GetTickCount() - g_cs.captureStart > 6000) g_cs.capturing = false;
        return 0;
    }
    if (g_cs.editing) {
        // An open selector: Left/Right change the value, Accept / Back / Up / Down close it.
        const int step = (in & kRight) ? 1 : (in & kLeft) ? -1 : 0;
        if (step) {
            if (g_cs.cursor == 0) g_cs.player = (g_cs.player + step + padlayout::kPlayers) % padlayout::kPlayers;
            else if (g_cs.cursor == 1) ChangeLayout(step);
            else if (g_cs.cursor == 2) padlayout::SetVibration(g_cs.player, !padlayout::Vibration(g_cs.player));
            else if (g_cs.cursor == 3) padlayout::SetAdaptiveTriggers(g_cs.player, !padlayout::AdaptiveTriggers(g_cs.player));
            else if (g_cs.cursor == 4) g_cs.tab = (g_cs.tab + step + kTabs) % kTabs;
            else if (g_cs.cursor == 5) features::SetPadDeadzone(static_cast<uint32_t>(
                std::clamp(static_cast<int>(settings::Get().padDeadzone) + step * 5, 0, 90)));
            else if (g_cs.cursor == 6)
                padlayout::SetLookSensitivity(g_cs.player, padlayout::LookSensitivity(g_cs.player) + step * padlayout::kLookStep);
            else if (g_cs.cursor == 7) padlayout::SetGyroMode(g_cs.player, (padlayout::GyroMode(g_cs.player) + step + 3) % 3);
        }
        if (in & (kAccept | kBack | kUp | kDown)) g_cs.editing = false;
        if (g_cs.cursor >= kHeaderRows + tab.count) g_cs.cursor = kHeaderRows;
        return 0;
    }
    for (uint32_t dir : {kUp, kDown, kLeft, kRight})
        if (in & dir) g_cs.cursor = Neighbour(g_cs.cursor, dir);
    if (in & kView) g_cs.xboxView = !g_cs.xboxView;
    if (g_cs.xboxView && g_cs.cursor == 3) g_cs.cursor = 4;  // adaptive triggers: DualSense only
    if (g_cs.cursor >= kHeaderRows + tab.count) g_cs.cursor = kHeaderRows;
    if (in & kReset) padlayout::Set(g_cs.player, padlayout::Preset::Default, padlayout::Make(padlayout::Preset::Default));
    if (in & kAccept) {
        if (g_cs.cursor < kHeaderRows) {
            g_cs.editing = true;
        } else {
            g_cs.capturing = true;
            g_cs.captureStart = GetTickCount();
            g_cs.captureRelease = ~0u;
        }
    }
    if (in & kBack) SetState(kStateControls);
    return 0;
}

overlay::Icon PadIcon(int button, bool xbox) {
    const int base = static_cast<int>(xbox ? overlay::Icon::XbTriangle : overlay::Icon::PsTriangle);
    return static_cast<overlay::Icon>(base + button);
}

// Stick of an axis action: 0 left, 1 right, -1 none (axis codes 0-5 left stick, 6-11 right stick).
int StickOf(const padlayout::Layout& l, int action) {
    for (int32_t e : l.entry[action])
        if (e >= 0 && (e >> 16) == 1) return (e & 0xFFFF) < 6 ? 0 : 1;
    return -1;
}

int DrawPrompt(int x, int y, overlay::Icon icon, const char* text, int iconSize) {
    void* font = overlay::MenuFont();
    overlay::QueueIcon(icon, static_cast<float>(x), static_cast<float>(y - overlay::TextWidth(font, "M") / 2 - iconSize / 2),
                       static_cast<float>(iconSize));
    x += iconSize + iconSize / 4;
    overlay::DrawLabel(font, text, x, y, 0xFFFFFFFF);
    return x + overlay::TextWidth(font, text) + iconSize;
}

void __fastcall CtlDraw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kTextController, 1);
    const int w = ScreenW(), h = ScreenH();
    void* font = overlay::MenuFont();
    const int cap = std::max(6, overlay::TextWidth(font, "M"));  // ~cap height
    constexpr uint32_t kWhite = 0xFFFFFFFF, kGold = 0xFFFFD040, kDim = 0xFFB8B8A0;
    const TabInfo& tab = kTabInfo[g_cs.tab];
    const padlayout::Layout& layout = padlayout::Get(g_cs.player);

    // Selectors.
    char text[96];
    const char* labels[8];
    char player[32], preset[48], vibration[40], triggers[48], tabName[32], deadzone[40], look[48], gyro[48];
    snprintf(player, sizeof player, "<  PLAYER %d  >", g_cs.player + 1);
    snprintf(preset, sizeof preset, "<  LAYOUT: %s  >", kPresetNames[static_cast<int>(padlayout::PresetOf(g_cs.player))]);
    snprintf(tabName, sizeof tabName, "<  %s  >", tab.name);
    snprintf(vibration, sizeof vibration, "<  VIBRATION: %s  >", padlayout::Vibration(g_cs.player) ? "ON" : "OFF");
    snprintf(triggers, sizeof triggers, "<  ADAPTIVE TRIGGERS: %s  >", padlayout::AdaptiveTriggers(g_cs.player) ? "ON" : "OFF");
    snprintf(deadzone, sizeof deadzone, "<  DEADZONE: %u%%  >", settings::Get().padDeadzone);
    snprintf(look, sizeof look, "<  LOOK SENSITIVITY: %d%%  >", padlayout::LookSensitivity(g_cs.player));
    labels[0] = player, labels[1] = preset, labels[2] = vibration, labels[3] = triggers, labels[4] = tabName;
    static const char* const kGyroModes[] = {"OFF", "WHILE AIMING", "ALWAYS"};
    snprintf(gyro, sizeof gyro, "<  GYRO AIM (PLAYSTATION): %s  >", kGyroModes[padlayout::GyroMode(g_cs.player)]);
    labels[5] = deadzone, labels[6] = look, labels[7] = gyro;
    g_cs.spotCount = 0;
    // Centred rows in this order (up to 4), filled up to 88% of the screen width by the labels' measured widths (so they don't
    // run into each other on narrow 4:3 screens). Adaptive triggers: DualSense only (hidden on the Xbox picture); gyro
    // works on PlayStation pads but is shown on both pictures (a per-player setting); deadzone is every pad's.
    const int gap = cap * 3;
    const int order[] = {0, 1, 4, 2, 3, 7, 6, 5};
    int rows[4][8], rowCount[4] = {}, rowWidth[4] = {}, nRows = 1;
    for (int i : order) {
        if (i == 3 && g_cs.xboxView) continue;  // gyro stays: it is set per player, whatever pad drives this menu
        const int tw = overlay::TextWidth(font, labels[i]);
        int& r = nRows;
        if (rowCount[r - 1] && rowWidth[r - 1] + gap + tw > w * 0.88f && r < 4) ++r;
        rowWidth[r - 1] += (rowCount[r - 1] ? gap : 0) + tw;
        rows[r - 1][rowCount[r - 1]++] = i;
    }
    // 4:3 needs a 4th row (above the callouts, which start at 0.285 of the height).
    const int rowPitch = static_cast<int>(h * (nRows > 3 ? 0.03f : nRows > 2 ? 0.034f : 0.042f));
    for (int r = 0; r < nRows; ++r) {
        int x = (w - rowWidth[r]) / 2;
        const int y = static_cast<int>(h * (nRows > 3 ? 0.16f : nRows > 2 ? 0.175f : 0.19f)) + r * rowPitch;
        for (int k = 0; k < rowCount[r]; ++k) {
            const int i = rows[r][k];
            const int tw = overlay::TextWidth(font, labels[i]);
            const bool focused = g_cs.cursor == i;
            if (focused && g_cs.editing)
                overlay::QueueRect(static_cast<float>(x - cap / 2), y - cap * 1.6f, static_cast<float>(tw + cap),
                                   cap * 2.2f, 0x70000000);
            overlay::DrawLabel(font, labels[i], x, y, focused ? (g_cs.editing ? kWhite : kGold) : kDim);
            AddSpot(i, x + tw / 2.0f, y - cap / 2.0f);
            x += tw + gap;
        }
    }

    // Callout rows: the tab's actions plus the two sticks, split by the side of their button, sorted by height.
    const bool xbox = g_cs.xboxView;
    const gen::ControllerArt& art = xbox ? gen::kXboxArt : gen::kPs5Art;
    struct Callout {
        int row;  // index into tab.rows, -1/-2 = move / look stick
        int anchor;
        const char* name;
    };
    std::vector<Callout> side[2];
    for (int i = 0; i < tab.count; ++i) {
        const int b = padlayout::ButtonOf(layout, tab.rows[i].action);
        const int anchor = b >= 0 ? b : 11;  // unbound: listed on the right, no line
        side[art.anchor[anchor][0] < 0.5f ? 0 : 1].push_back({i, b >= 0 ? anchor : -1, tab.rows[i].name});
    }
    const int moveStick = StickOf(layout, tab.moveAction);
    if (moveStick >= 0) {
        side[moveStick].push_back({-1, 16 + moveStick, tab.moveName});
        side[1 - moveStick].push_back({-2, 17 - moveStick, tab.lookName});
    }
    const int iconSize = std::max(14, cap * 2);

    // Picture: 40% of the screen height, made smaller when the widest callout column wouldn't fit beside it (4:3).
    int widest = 0;
    for (auto& list : side)
        for (const Callout& c : list) widest = std::max(widest, overlay::TextWidth(font, c.name));
    const float column = w * 0.02f + iconSize + iconSize / 3.0f + widest + cap;  // picture edge -> text end
    const float nominalH = h * 0.40f, aspect = static_cast<float>(art.width) / art.height;
    const float pw = std::max(w * 0.25f, std::min(nominalH * aspect, w * 0.90f - 2 * column));
    const float ph = pw / aspect;
    const float px = (w - pw) / 2, py = h * 0.285f + (nominalH - ph) / 2;
    void* tex = overlay::LoadTexture(xbox ? gen::kXboxArtResource : gen::kPs5ArtResource, gen::kControllerTexW,
                                     gen::kControllerTexH);
    overlay::QueueImage(tex, px, py, pw, ph, static_cast<float>(art.width) / gen::kControllerTexW,
                        static_cast<float>(art.height) / gen::kControllerTexH);

    for (int s = 0; s < 2; ++s) {
        auto& list = side[s];
        std::stable_sort(list.begin(), list.end(), [&](const Callout& a, const Callout& b) {
            const float ya = a.anchor >= 0 ? art.anchor[a.anchor][1] : 2.0f, yb = b.anchor >= 0 ? art.anchor[b.anchor][1] : 2.0f;
            return ya < yb;
        });
        // The column keeps the full-size picture's height even when the picture shrank.
        const float top = h * 0.285f - nominalH * 0.02f, bottom = h * 0.285f + nominalH * 1.08f;
        const float pitch = list.size() > 1 ? std::min(iconSize * 1.6f, (bottom - top) / (list.size() - 1)) : 0.0f;
        const float colX = s == 0 ? px - w * 0.02f : px + pw + w * 0.02f;
        for (size_t i = 0; i < list.size(); ++i) {
            const Callout& c = list[i];
            const float y = top + pitch * i + (list.size() > 1 ? 0 : (bottom - top) / 2);
            const bool selected = c.row >= 0 && g_cs.cursor == kHeaderRows + c.row;
            if (c.row >= 0) AddSpot(kHeaderRows + c.row, colX, y);
            const uint32_t line = selected ? 0xFFFFD040 : 0xE0F0F0E0;
            if (c.anchor >= 0) {
                const float ax = px + art.anchor[c.anchor][0] * pw, ay = py + art.anchor[c.anchor][1] * ph;
                const float elbow = s == 0 ? colX + w * 0.015f : colX - w * 0.015f;
                overlay::QueueLine(ax, ay, elbow, y, selected ? 3.0f : 2.0f, line);
                overlay::QueueLine(elbow, y, colX, y, selected ? 3.0f : 2.0f, line);
                overlay::QueueRect(ax - 4, ay - 4, 8, 8, line);
            }
            // icon next to the line end, then the text
            overlay::Icon icon = c.row < 0 ? (c.anchor == 16 ? (xbox ? overlay::Icon::XbStickL : overlay::Icon::PsStickL)
                                                             : (xbox ? overlay::Icon::XbStickR : overlay::Icon::PsStickR))
                                           : PadIcon(c.anchor >= 0 ? c.anchor : 0, xbox);
            const char* name = c.name;
            if (selected && g_cs.capturing) name = (GetTickCount() / 400) % 2 ? "Press a button..." : "";
            const int textW = overlay::TextWidth(font, name);
            const float iconX = s == 0 ? colX - iconSize : colX;
            if (c.anchor >= 0) overlay::QueueIcon(icon, iconX, y - iconSize / 2.0f, static_cast<float>(iconSize));
            const int tx = s == 0 ? static_cast<int>(iconX) - iconSize / 3 - textW : static_cast<int>(iconX + iconSize + iconSize / 3);
            if (selected) overlay::QueueRect(static_cast<float>(tx - cap / 2), y - iconSize * 0.45f,
                                             static_cast<float>(textW + cap), iconSize * 0.9f, 0x60000000);
            overlay::DrawLabel(font, name, tx, static_cast<int>(y + cap / 2), selected ? kGold : kWhite);
        }
    }

    // Prompts.
    const int y = static_cast<int>(h * 0.925f);
    int x = static_cast<int>(w * 0.06f);
    const int ps = std::max(12, iconSize * 3 / 4);
    using overlay::Icon;
    const char* accept = g_cs.editing ? "Done" : g_cs.cursor < kHeaderRows ? "Change" : "Change button";
    if (g_cs.editing) {
        // An open selector: only Left/Right and closing it do anything.
        x = DrawPrompt(x, y, g_cs.keyboardPrompts ? Icon::KbArrowLeft : (xbox ? Icon::XbDpadLeft : Icon::PsDpadLeft), "", ps);
        x = DrawPrompt(x - ps, y, g_cs.keyboardPrompts ? Icon::KbArrowRight : (xbox ? Icon::XbDpadRight : Icon::PsDpadRight),
                       "Change value", ps);
        DrawPrompt(x, y, g_cs.keyboardPrompts ? Icon::KbEnter : PadIcon(2, xbox), accept, ps);
    } else if (g_cs.keyboardPrompts) {
        x = DrawPrompt(x, y, Icon::KbEnter, accept, ps);
        x = DrawPrompt(x, y, Icon::KbDelete, "Reset", ps);
        x = DrawPrompt(x, y, Icon::KbTab, xbox ? "PlayStation" : "Xbox", ps);
        DrawPrompt(x, y, Icon::KbEscape, "Back", ps);
    } else {
        x = DrawPrompt(x, y, PadIcon(2, xbox), accept, ps);
        x = DrawPrompt(x, y, PadIcon(3, xbox), "Reset", ps);
        x = DrawPrompt(x, y, PadIcon(0, xbox), xbox ? "PlayStation" : "Xbox", ps);
        DrawPrompt(x, y, PadIcon(1, xbox), "Back", ps);
    }
    snprintf(text, sizeof text, "Controller art: Zacksly (CC BY 3.0)");
    overlay::DrawLabel(font, text, static_cast<int>(w * 0.93f) - overlay::TextWidth(font, text), static_cast<int>(h * 0.80f),
                       0x90FFFFFF);
}

uint32_t __fastcall NoPrompt(void*, void*) { return 0; }

void Build() {
    for (auto [screen, vtable] :
         {std::pair{g_kbm, g_kbmVtable}, std::pair{g_ctl, g_ctlVtable}, std::pair{g_prof, g_profVtable}}) {
        reinterpret_cast<void(__thiscall*)(void*)>(kOptCtor)(screen);
        std::memcpy(vtable, reinterpret_cast<void*>(kOptVtable), sizeof g_kbmVtable);
        At<void**>(screen, 0) = vtable;
    }
    // Enter (+0x18): this class's does nothing (its set-up runs once at front-end start); the main menu's runs
    // FUN_004475b0(0) and then the set-up (+0x0c) - ours - every time the screen is entered.
    g_kbmVtable[6] = g_ctlVtable[6] = g_profVtable[6] = reinterpret_cast<void*>(kMainMenuEnter);
    g_profVtable[1] = reinterpret_cast<void*>(&ProfCreate);
    g_profVtable[3] = reinterpret_cast<void*>(&ProfSetup);
    g_profVtable[4] = reinterpret_cast<void*>(&ProfUpdate);
    g_profVtable[5] = reinterpret_cast<void*>(&ProfDraw);
    g_profVtable[13] = reinterpret_cast<void*>(&NoPrompt);
    g_profVtable[17] = reinterpret_cast<void*>(&NoPrompt);
    g_kbmVtable[1] = reinterpret_cast<void*>(&KbmCreate);
    g_kbmVtable[3] = reinterpret_cast<void*>(&KbmSetup);
    g_kbmVtable[4] = reinterpret_cast<void*>(&KbmUpdate);
    g_kbmVtable[5] = reinterpret_cast<void*>(&KbmDraw);
    g_ctlVtable[1] = reinterpret_cast<void*>(&CtlCreate);
    g_ctlVtable[3] = reinterpret_cast<void*>(&CtlSetup);
    g_ctlVtable[4] = reinterpret_cast<void*>(&CtlUpdate);
    g_ctlVtable[5] = reinterpret_cast<void*>(&CtlDraw);
    g_ctlVtable[13] = reinterpret_cast<void*>(&NoPrompt);
    g_ctlVtable[17] = reinterpret_cast<void*>(&NoPrompt);
    g_built = KbmCreate(g_kbm, nullptr) && CtlCreate(g_ctl, nullptr) && ProfCreate(g_prof, nullptr);
    dslog::Write(g_built ? "Controls: screens built" : "[fail] Controls: screens not built");
}

// Byte patches: {VA, original, new} (4-byte hashes or 1-byte state immediates).
struct Site {
    uint32_t va;
    uint32_t original, value;
    int size;
};
const Site kSites[] = {
    {0x471BCD, kHashCtrl, kTextControls, 4},        {0x471BDC, kHashCtrl, kTextControls, 4},
    {0x409D7C, kHashKeys, kTextKbm, 4},             {0x409DAD, kHashMouse, kTextController, 4},
    {0x409DD3, kHashCtrlOptions, kTextControls, 4}, {0x409E57, kHashCtrlOptions, kTextControls, 4},
    {0x409E12, kStateMouse, kStateController, 1},   {0x409E16, kStateKeys, kStateKbm, 1},
    {0x445B10, kStateControls, kStateKbm, 1},       {0x45C878, kStateControls, kStateKbm, 1},
};
}  // namespace

const char* features::ControlsText(uint32_t hash) {
    switch (hash) {
        case kTextControls: return "CONTROLS";
        case kTextKbm: return "KEYBOARD & MOUSE";
        case kTextController: return "CONTROLLER";
        case kTextProfiles: return "PROFILES";
        default: break;
    }
    if (hash >= kTextProfile0 && hash < kTextProfile0 + kProfiles) {
        static char rows[kProfiles][48];
        const int n = static_cast<int>(hash - kTextProfile0);
        KeyProfile p;
        snprintf(rows[n], sizeof rows[n], "PROFILE %d:  %s", n + 1, ReadProfile(n, p) ? "SAVED" : "EMPTY");
        return rows[n];
    }
    return nullptr;
}

bool features::KeyProfileSaved(int n) {
    KeyProfile p;
    return n >= 0 && n < kProfiles && ReadProfile(n, p);
}

void features::ApplyCoopKeyProfiles() {
    for (int player = 0; player < 4; ++player) {
        const int n = features::CoopKeyProfile(player);
        if (n >= 0 && ApplyProfile(player, n)) dslog::Write("Controls: player %d uses key profile %d", player + 1, n + 1);
    }
}

void* features::ControlsScreen(uint32_t state) {
    if (!g_built) return nullptr;
    if (state == kStateKbm) return g_kbm;
    if (state == kStateController) return g_ctl;
    if (state == kStateProfiles) return g_prof;
    return nullptr;
}

void features::OnFrameControls() {
    if (!g_built && *reinterpret_cast<void**>(kMainMenuScreen)) {
        static bool tried = false;
        if (!tried) {
            tried = true;
            Build();
        }
    }
}

void features::ApplyControls() {
    static bool done = false;
    if (done) return;
    done = true;
    for (const Site& s : kSites)
        if (!patch::Matches(s.va, &s.original, s.size)) {
            dslog::Write("[fail] Controls menu: unexpected bytes at 0x%08X - not applied", s.va);
            return;
        }
    for (const Site& s : kSites) patch::Write(s.va, &s.value, s.size);
    dslog::Write("[ok]   Controls menu: CONTROLS -> KEYBOARD & MOUSE / CONTROLLER");
}

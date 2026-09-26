// In-game menus follow the input device in use: while a pad drives them the mouse cursor is hidden and the
// keyboard prompts ("Return: Select", "Esc: Back") name the pad buttons instead.
//
// Input device: tracked once per frame (features::OnFrame, from the frame-cap hook). A pad counts as in use on
// any button/stick activity (our own XInput/DirectInput reads, non-exclusive, next to the game's); keyboard or
// mouse on any real input seen by GetLastInputInfo that is not right after pad activity.
//
// Cursor: the render loop FUN_0040ef70 draws image 0x52 (IMAGE_MOUSE_CURSOR, sheet [0x60EE28]) at the mouse
// position when [0x5e6cb4] is set. The flag read at 0x40F3A5 (mov eax,[0x5e6cb4]) becomes a call to CursorFlag().
//
// Prompts: every frame the menu draw code (e.g. 0x47a616) gets the PC_*_PROMPT hashes from the screen's vtable
// (+0x34 select, +0x44 back) and turns them into text with the string lookup FUN_004f6730 (thiscall on the table
// [0x60edcc], (hash) -> char*, 212 callers). That function is replaced by Lookup() - a faithful copy calling the
// game's own helpers - which returns "<pad button>: <the game's text after the colon>" for the prompt hashes,
// so every language keeps its wording.
// Buttons are the ones the game really uses (default.key: MENU_FORWARD = 2, MENU_BACK = 0, PAUSE = 11,
// OBJECTIVES = 8), named for the pad in use after our DualShock remap.
#include <windows.h>

#include <intrin.h>

#include <cstring>
#include <string>
#include <unordered_map>

#include "core/gamepad.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kCursorFlagSite = 0x40F3A5;  // mov eax,[0x5e6cb4] (5 bytes)
constexpr uint32_t kCursorFlag = 0x5E6CB4;
constexpr uint32_t kLookup = 0x4F6730;          // string lookup (hash -> text), thiscall, ret 4
constexpr uint32_t kLookupEnsureSorted = 0x4F6700, kLookupSearch = 0x4F6780;  // thiscall, no stack args
constexpr uint32_t kSearchHash = 0x640734, kSearchLow = 0x640738, kSearchHigh = 0x640728;
constexpr uint32_t kGameWindow = 0x606A60;

enum class Input { Keyboard, PlayStation, Xbox };
Input g_input = Input::Keyboard;
bool g_enabled = false;

// ---- input device tracking ----
bool g_padOpen = false;
DWORD g_lastKbm = 0, g_lastPadActivity = 0;

// A key (or mouse button) held right now, or the game's DirectInput mouse (DIMOUSESTATE at 0x754C24, relative)
// moved / clicked in its last poll - SetCursorPos produces neither.
constexpr uint32_t kDiMouseState = 0x754C24;
bool KeyboardOrMouseActive() {
    const auto* m = reinterpret_cast<const int32_t*>(kDiMouseState);
    if (m[0] || m[1] || m[2] || m[3]) return true;  // lX, lY, lZ, rgbButtons[4]
    for (int vk = 1; vk < 0xFF; ++vk)
        if (GetAsyncKeyState(vk) & 0x8000) return true;
    return false;
}

void SetInput(Input input) {
    if (input == g_input) return;
    g_input = input;
    dslog::Write("In game: input -> %s",
                 input == Input::Keyboard ? "keyboard/mouse" : input == Input::Xbox ? "Xbox" : "PlayStation");
}

// ---- prompts ----
struct Prompt {
    uint32_t hash;
    int button;           // game joystick button (default.key numbering), -1 = D-pad
    overlay::Icon key;    // the keyboard key the text names
};
// Hashes from catalog.dat (PC_*_PROMPT). PC_TAB_KEY_PROMPT has no known pad equivalent and stays as it is.
constexpr Prompt kPrompts[] = {
    {3812760437u, 2, overlay::Icon::KbEnter},             // PC_ACCEPT_PROMPT        Return: Accept
    {1376302336u, 2, overlay::Icon::KbEnter},             // PC_SELECT_PROMPT        Return: Select
    {620529314u, 2, overlay::Icon::KbEnter},              // PC_MORE_INFO_PROMPT     Return: More information
    {2426184306u, 0, overlay::Icon::KbEscape},            // PC_BACK_PROMPT          Esc: Back
    {745684578u, 0, overlay::Icon::KbEscape},             // PC_CANCEL_PROMPT        Esc: Cancel
    {799972218u, 0, overlay::Icon::KbEscape},             // PC_ESC_FE_PROMPT        Esc: Main Menu
    {2484684419u, 0, overlay::Icon::KbEscape},            // PC_ESC_CONTINUE_PROMPT  Esc: Continue
    {4068979723u, 11, overlay::Icon::KbEscape},           // PC_ESC_RETURN_PROMPT    Esc: Return to mission
    {947741146u, 8, overlay::Icon::KbF1},                 // PC_F1_RETURN_PROMPT     F1: Return to mission
    {2393634237u, -1, overlay::Icon::KbArrows},           // PC_DEBRIEF_PROMPT       Arrow Keys: Change soldier
    {2522449822u, -1, overlay::Icon::KbArrowsHorizontal}, // PC_LR_SELECT_PROMPT     Left/Right: Select
};

// Game button n after our remap -> physical button name.
const char* ButtonName(Input input, int button) {
    static const char* const ps[] = {"Triangle", "Circle", "Cross", "Square", "L2", "R2",
                                     "L1",       "R1",     "Share", "L3",     "R3", "Options"};
    static const char* const xb[] = {"Y", "B", "A", "X", "LT", "RT", "LB", "RB", "View", "LS", "RS", "Menu"};
    if (button < 0) return "D-pad";
    if (button > 11) return "?";
    return input == Input::Xbox ? xb[button] : ps[button];
}

// ---- button icons ----
// Pad style for icons: the pad in use, else the last pad seen, else PlayStation (the in-game remap is DualShock).
// g_forceXbox: set while a co-op player's own style is being used (context prompts).
int g_forceStyle = -1;  // -1 none, 1 PlayStation, 2 Xbox
bool XboxStyle() { return g_forceStyle >= 0 ? g_forceStyle == 2 : g_input == Input::Xbox; }

overlay::Icon PadIcon(int button) {  // game joystick button 0-15 (default.key numbering), -1 = D-pad
    using overlay::Icon;
    if (button < 0) return XboxStyle() ? Icon::XbDpadHorizontal : Icon::PsDpadHorizontal;
    const int base = static_cast<int>(XboxStyle() ? Icon::XbTriangle : Icon::PsTriangle);
    return static_cast<Icon>(base + (button & 15));
}

// DirectInput scan code -> keyboard icon (Count = none).
overlay::Icon KeyIcon(uint32_t dik) {
    using overlay::Icon;
    static const char row1[] = "1234567890", qwerty[] = "QWERTYUIOP", asdf[] = "ASDFGHJKL", zxcv[] = "ZXCVBNM";
    auto letter = [](char c) { return static_cast<Icon>(static_cast<int>(Icon::KbA) + (c - 'A')); };
    auto digit = [](char c) { return static_cast<Icon>(static_cast<int>(Icon::Kb0) + (c - '0')); };
    if (dik >= 0x02 && dik <= 0x0B) return digit(row1[dik - 0x02]);
    if (dik >= 0x10 && dik <= 0x19) return letter(qwerty[dik - 0x10]);
    if (dik >= 0x1E && dik <= 0x26) return letter(asdf[dik - 0x1E]);
    if (dik >= 0x2C && dik <= 0x32) return letter(zxcv[dik - 0x2C]);
    if (dik >= 0x3B && dik <= 0x44) return static_cast<Icon>(static_cast<int>(Icon::KbF1) + (dik - 0x3B));
    switch (dik) {
        case 0x01: return Icon::KbEscape;
        case 0x0C: return Icon::KbMinus;
        case 0x0D: return Icon::KbEquals;
        case 0x0E: return Icon::KbBackspace;
        case 0x0F: return Icon::KbTab;
        case 0x1A: return Icon::KbBracketOpen;
        case 0x1B: return Icon::KbBracketClose;
        case 0x1C: return Icon::KbEnter;
        case 0x1D: case 0x9D: return Icon::KbCtrl;
        case 0x27: return Icon::KbSemicolon;
        case 0x28: return Icon::KbApostrophe;
        case 0x29: return Icon::KbTilde;
        case 0x2A: case 0x36: return Icon::KbShift;
        case 0x2B: return Icon::KbBackslash;
        case 0x33: return Icon::KbComma;
        case 0x34: return Icon::KbPeriod;
        case 0x35: return Icon::KbSlash;
        case 0x37: return Icon::KbAsterisk;
        case 0x38: case 0xB8: return Icon::KbAlt;
        case 0x39: return Icon::KbSpace;
        case 0x3A: return Icon::KbCapslock;
        case 0x4E: return Icon::KbNumpadPlus;
        case 0x57: return Icon::KbF11;
        case 0x58: return Icon::KbF12;
        case 0x9C: return Icon::KbNumpadEnter;
        case 0xC7: return Icon::KbHome;
        case 0xC8: return Icon::KbArrowUp;
        case 0xC9: return Icon::KbPageUp;
        case 0xCB: return Icon::KbArrowLeft;
        case 0xCD: return Icon::KbArrowRight;
        case 0xCF: return Icon::KbEnd;
        case 0xD0: return Icon::KbArrowDown;
        case 0xD1: return Icon::KbPageDown;
        case 0xD2: return Icon::KbInsert;
        case 0xD3: return Icon::KbDelete;
        default: return Icon::Count;
    }
}

// Binding code (type << 16 | n: 0 key, 1 joystick axis, 2 joystick button, 4 mouse button) -> icon (Count = none).
overlay::Icon BindingIcon(uint32_t code) {
    using overlay::Icon;
    const uint32_t n = code & 0xFFFF;
    switch (code >> 16) {
        case 0: return KeyIcon(n);
        case 1: return n < 6 ? (XboxStyle() ? Icon::XbStickL : Icon::PsStickL) : (XboxStyle() ? Icon::XbStickR : Icon::PsStickR);
        case 2: return n < 16 ? PadIcon(static_cast<int>(n)) : Icon::Count;
        case 4: return n == 0 ? Icon::MouseLeft : n == 1 ? Icon::MouseRight : n == 2 ? Icon::MouseMiddle : Icon::Mouse;
        default: return Icon::Count;
    }
}

// Context prompts over objects (FUN_00450560 picks one per action type: MOUNT, UNMOUNT, PICK UP, EMBARK, CLIMB ...;
// all done with the ACTION control): "[button] MOUNT". The button is the ACTION binding (action 2 in the binding
// table [0x60687c], FUN_0040a110(set, slot, action)) of the player whose HUD is drawn - in co-op the device and icon
// style they joined with, otherwise the device in use.
constexpr uint32_t kActionPrompts[] = {
    0x14D74E2B, 0x94CE0B19, 0x43C36EAC, 0x38C30DC3, 0x0205AF3C, 0x935F10D6, 0x725DC0BB, 0x1DEA6EED, 0xA009940C,
    0x7E276392, 0x9A21D90F, 0xDAF2188F, 0x1DDF138C, 0x88DEF646, 0x6A00298D, 0x207F169C, 0x6E35CC74, 0x0153D382,
    // shown by other code (doors, cells, chains): UNLOCK, OP_DOOR "OPERATE DOOR", CUT_CHAIN, PICK_UP_OBJ
    0x2F942AB6, 0xB24B9B70, 0x1AE20DAA, 0x478FE064};
constexpr uint32_t kBindings = 0x60687C, kBindingLookup = 0x40A110;
constexpr uint32_t kActionPrompt = 0x450540, kActionPromptCall = 0x40F304;  // FUN_00450540(block), one caller
constexpr uint32_t kInputBlocks = 0x60F5B8, kInputBlockSize = 0x478;
int g_promptPlayer = 0;  // player whose context prompt is being drawn (drawn after the per-view loop)
#ifndef DS_DIST
DWORD g_forcedAction = 0;  // dev: Dev\ForceActionPrompt = action type (block +0x374; 13 UNMOUNT, 16 PICK UP ...)
#endif

void __fastcall ActionPrompt(uint8_t* block) {
    g_promptPlayer = static_cast<int>((reinterpret_cast<uintptr_t>(block) - kInputBlocks) / kInputBlockSize) & 3;
#ifndef DS_DIST
    if (g_forcedAction) *reinterpret_cast<int32_t*>(block + 0x374) = static_cast<int32_t>(g_forcedAction);
#endif
    reinterpret_cast<void(__fastcall*)(uint8_t*)>(kActionPrompt)(block);
    g_promptPlayer = 0;
}
constexpr uint32_t kActionControl = 2;

overlay::Icon BindingIcon(uint32_t code);

// The button of `action` for `player` (their device: co-op join, else the device in use), or Count.
overlay::Icon ControlIcon(int player, uint32_t action) {
    void* table = *reinterpret_cast<void**>(kBindings);
    if (!table) return overlay::Icon::Count;
    int style;
    if (features::SplitScreenPlayers() <= 1) player = 0;
    const int coopStyle = features::CoopPromptStyle(player);
    if (coopStyle >= 0) style = coopStyle;
    else style = (g_enabled && g_input != Input::Keyboard) ? (g_input == Input::Xbox ? 2 : 1) : 0;
    const int set = features::SplitScreenPlayers() > 1 ? player : 0;  // binding set = block +0x3c0 = player

    const int slots = reinterpret_cast<const int*>(table)[1];
    using Lookup = uint32_t(__thiscall*)(void*, int, int, int);
    for (int slot = 0; slot < slots; ++slot) {
        const uint32_t code = reinterpret_cast<Lookup>(kBindingLookup)(table, set, slot, action);
        if (code == 0xFFFFFFFF) continue;
        const uint32_t type = code >> 16;
        const bool padCode = type == 1 || type == 2;
        if (padCode != (style != 0)) continue;
        g_forceStyle = style ? style : -1;
        const overlay::Icon icon = BindingIcon(code);
        g_forceStyle = -1;
        if (icon != overlay::Icon::Count) return icon;
    }
    return overlay::Icon::Count;
}

// Replaces the call of FUN_004722c0 (binding code -> name) where the game fills the "%s" of its tutorial / tip
// texts with an action's bindings ("Crouch (%s)" -> "Crouch (C / Joy 3)"): an icon character instead of the name.
constexpr uint32_t kBindingName = 0x4722C0, kTutorialBindingCall = 0x486BA8;
const char* __fastcall BindingName(void* table, void*, uint32_t code) {
    if (code != 0xFFFFFFFF && overlay::GlyphsReady()) {
        const overlay::Icon icon = BindingIcon(code);
        if (icon != overlay::Icon::Count) return overlay::IconChar(icon);
    }
    return reinterpret_cast<const char*(__thiscall*)(void*, uint32_t)>(kBindingName)(table, code);
}

// Which of an action's bindings the tutorial / tip texts list (FUN_00486b00 joins set 0's slots with " / "): the game
// dropped pad codes unless [0x606368] (a joystick-present flag) was set and always kept the keys - "Press objectives
// (F1)" on a pad. The check at 0x486B5E (22 bytes) now calls ListBinding: bindings of the device in use (player 1's
// co-op device, else the last input), or all of them when the action has none for that device.
constexpr uint32_t kListCheck = 0x486B5E, kListSkip = 0x486BE9, kListKeep = 0x486B74;
bool IsPadCode(uint32_t code) { return (code >> 16) == 1 || (code >> 16) == 2; }

int __stdcall ListBinding(uint32_t code, uint32_t action) {
    const int coopStyle = features::CoopPromptStyle(0);
    const bool pad = coopStyle >= 0 ? coopStyle != 0 : (g_enabled && g_input != Input::Keyboard);
    if (IsPadCode(code) == pad) return 1;
    void* table = *reinterpret_cast<void**>(kBindings);
    const int slots = reinterpret_cast<const int*>(table)[1];
    using Lookup = uint32_t(__thiscall*)(void*, int, int, int);
    for (int slot = 0; slot < slots; ++slot) {
        const uint32_t other = reinterpret_cast<Lookup>(kBindingLookup)(table, 0, slot, action);
        if (other != 0xFFFFFFFF && IsPadCode(other) == pad) return 0;  // the device has its own binding
    }
    return 1;
}

// At 0x486B5E: eax = edx = binding code, esi = action; edx must survive.
__declspec(naked) void ListBindingStub() {
    __asm {
        push edx
        push esi
        push edx
        call ListBinding
        pop edx
        test eax, eax
        jz skip
        push kListKeep
        ret
    skip:
        push kListSkip
        ret
    }
}

// Same as FUN_004f6730: prepare the table if needed, then binary-search it through globals.
const char* GameLookup(void* table, uint32_t hash) {
    using Method = const char*(__thiscall*)(void*);
    auto t = static_cast<char*>(table);
    if (*reinterpret_cast<int*>(t + 0x60) == 0) reinterpret_cast<Method>(kLookupEnsureSorted)(table);
    uint32_t count = *reinterpret_cast<uint32_t*>(t + 0x4C);
    if (count == 0) return nullptr;
    *reinterpret_cast<uint32_t*>(kSearchLow) = 0;
    *reinterpret_cast<uint32_t*>(kSearchHash) = hash;
    *reinterpret_cast<uint32_t*>(kSearchHigh) = count - 1;
    return reinterpret_cast<Method>(kLookupSearch)(table);
}

const char* __fastcall Lookup(void* table, void* /*edx*/, uint32_t hash) {
    if (const char* own = features::CoopText(hash)) return own;
    if (const char* own = features::ControlsText(hash)) return own;
    const char* text = GameLookup(table, hash);
    if (!text) return text;
#ifndef DS_DIST
    {
        static int logged = 0;
        static uint32_t seen[512];
        static int seenCount = 0;
        const char* level = reinterpret_cast<const char*>(0x606880);
        DWORD on = 0, size = sizeof on;
        static const bool playLog = (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "PlayLog",
                                                   RRF_RT_REG_DWORD, nullptr, &on, &size), on != 0);
        if (playLog && logged < 400 && level[0] && _strnicmp(level, "frontend", 8) != 0 && strlen(text) < 40) {
            bool known = false;
            for (int i = 0; i < seenCount && !known; ++i) known = seen[i] == hash;
            if (!known && seenCount < 512) {
                seen[seenCount++] = hash;
                ++logged;
                dslog::Write("[dev]  text %08X \"%s\" from 0x%p", hash, text, _ReturnAddress());
            }
        }
    }
#endif
    const bool pad = g_enabled && g_input != Input::Keyboard;
    const bool icons = overlay::GlyphsReady();
    if (icons)
        for (uint32_t h : kActionPrompts) {
            if (h != hash) continue;
            const overlay::Icon icon = ControlIcon(g_promptPlayer, kActionControl);
            if (icon == overlay::Icon::Count) return text;
            static char ring[16][96];
            static int next = 0;
            char* out = ring[next];
            next = (next + 1) % 16;
            snprintf(out, sizeof ring[0], "%s %s", overlay::IconChar(icon), text);
            return out;
        }
    if (!pad && !icons) return text;
    for (const Prompt& p : kPrompts) {
        if (p.hash != hash) continue;
        const char* colon = strchr(text, ':');
        const char* action = colon ? colon + 1 : text;
        while (*action == ' ') ++action;
        // Built per call (the icon characters are handed out on demand); a ring of buffers keeps the pointers the
        // game holds for this frame valid.
        static char ring[32][160];
        static int next = 0;
        char* out = ring[next];
        next = (next + 1) % 32;
        if (icons)
            snprintf(out, sizeof ring[0], "%s  %s", overlay::IconChar(pad ? PadIcon(p.button) : p.key), action);
        else
            snprintf(out, sizeof ring[0], "%s: %s", ButtonName(g_input, p.button), action);
        return out;
    }
    return text;
}

// Replaces "mov eax,[0x5e6cb4]": the game's draw-cursor flag, off while a pad drives the menus.
uint32_t __cdecl CursorFlag() {
    uint32_t flag = *reinterpret_cast<uint32_t*>(kCursorFlag);
    return (g_enabled && g_input != Input::Keyboard) ? 0 : flag;
}
}  // namespace

const char* features::ControlIconText(int player, uint32_t action) {
    if (!overlay::GlyphsReady()) return nullptr;
    const overlay::Icon icon = ControlIcon(player, action);
    return icon == overlay::Icon::Count ? nullptr : overlay::IconChar(icon);
}

const char* features::GameText(uint32_t hash) {
    void* table = *reinterpret_cast<void**>(0x60EDCC);
    return table ? GameLookup(table, hash) : nullptr;
}

void features::OnFrameInput() {
    if (!g_enabled) return;
    if (!g_padOpen) {
        HWND wnd = *reinterpret_cast<HWND*>(kGameWindow);
        if (!wnd) return;
        g_padOpen = gamepad::Open(wnd);
        LASTINPUTINFO li{sizeof li, 0};
        GetLastInputInfo(&li);
        g_lastKbm = li.dwTime;
    }
    DWORD now = GetTickCount();
    gamepad::State s = gamepad::Poll();
    if (s.activity) {
        g_lastPadActivity = now;
        SetInput(s.type == gamepad::Type::Xbox ? Input::Xbox : Input::PlayStation);
    }
    // Keyboard/mouse: input Windows saw (ignored right after pad activity, in case the pad counts) that really came
    // from them - a key held, or the game's DirectInput mouse moving / clicking. On a mounted gun (block +0x454) the
    // game re-centres the cursor with SetCursorPos every frame (0x44B4E1); that moves the cursor and updates the
    // last-input time, which flipped the prompts between pad and keyboard.
    LASTINPUTINFO li{sizeof li, 0};
    GetLastInputInfo(&li);
    if (li.dwTime != g_lastKbm && now - g_lastPadActivity > 250 && KeyboardOrMouseActive()) SetInput(Input::Keyboard);
    g_lastKbm = li.dwTime;
}

void features::ApplyInGameInput() {
    g_enabled = settings::Get().controller;
    static bool hooked = false;
    if (hooked) return;  // installed even with the pad option off: Lookup also serves the co-op screen's texts
    static const uint8_t cursorSite[] = {0xA1, 0xB4, 0x6C, 0x5E, 0x00};
    static const uint8_t lookupHead[] = {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x60, 0x85, 0xC0, 0x75, 0x05, 0xE8, 0xC1,
                                         0xFF, 0xFF, 0xFF, 0x8B, 0x46, 0x4C, 0x85, 0xC0, 0x76, 0x28};
    if (!patch::Matches(kCursorFlagSite, cursorSite, sizeof cursorSite) ||
        !patch::Matches(kLookup, lookupHead, sizeof lookupHead)) {
        dslog::Write("[fail] In-game pad prompts / cursor: unexpected bytes - different exe version?");
        return;
    }
    uint8_t call[5] = {0xE8};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&CursorFlag) - (kCursorFlagSite + 5));
    memcpy(call + 1, &rel, 4);
    patch::Write(kCursorFlagSite, call, sizeof call);
    patch::WriteJump(kLookup, reinterpret_cast<const void*>(&Lookup));
    if (overlay::Install()) {
        patch::HookCall(kTutorialBindingCall, reinterpret_cast<const void*>(&BindingName), kBindingName);
        static const uint8_t listCheck[] = {0xC1, 0xE8, 0x10, 0x83, 0xF8, 0x01, 0x74, 0x05, 0x83, 0xF8, 0x02,
                                            0x75, 0x09, 0xA1, 0x68, 0x63, 0x60, 0x00, 0x85, 0xC0, 0x74, 0x75};
        if (patch::Matches(kListCheck, listCheck, sizeof listCheck))
            patch::WriteJump(kListCheck, reinterpret_cast<const void*>(&ListBindingStub));
        else
            dslog::Write("[fail] Tutorial texts: binding list check not recognised at 0x%08X", kListCheck);
        patch::HookCall(kActionPromptCall, reinterpret_cast<const void*>(&ActionPrompt), kActionPrompt);
#ifndef DS_DIST
        DWORD v = 0, size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "ForceActionPrompt", RRF_RT_REG_DWORD,
                         nullptr, &v, &size) == ERROR_SUCCESS && v) {
            g_forcedAction = v;  // and skip the "no action available" check (je at 0x45055E)
            static const uint8_t nops[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
            patch::Write(0x45055E, nops, sizeof nops);
            dslog::Write("[dev]  Action prompt forced to type %lu", v);
        }
#endif
    }
    hooked = true;
    dslog::Write("[ok]   In-game pad prompts + cursor hiding");
}

bool features::KeyboardInUse() { return g_input == Input::Keyboard; }
bool features::PadStyleXbox() { return g_input == Input::Xbox; }

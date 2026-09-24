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

#include <cstring>
#include <string>
#include <unordered_map>

#include "core/gamepad.h"
#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kCursorFlagSite = 0x40F3A5;  // mov eax,[0x5e6cb4] (5 bytes)
constexpr uint32_t kCursorFlag = 0x5E6CB4;
constexpr uint32_t kLookup = 0x4F6730;          // string lookup (hash -> text), thiscall, ret 4
constexpr uint32_t kLookupEnsureSorted = 0x4F6700, kLookupSearch = 0x4F6780;  // thiscall, no stack args
constexpr uint32_t kSearchHash = 0x640734, kSearchLow = 0x640738, kSearchHigh = 0x640728;
constexpr uint32_t kGameWindow = 0x606A60;
constexpr uint32_t kMouseX = 0x754C18, kMouseY = 0x754C1C;

enum class Input { Keyboard, PlayStation, Xbox };
Input g_input = Input::Keyboard;
bool g_enabled = false;

// ---- input device tracking ----
bool g_padOpen = false;
DWORD g_lastKbm = 0, g_lastPadActivity = 0;
LONG g_lastMouseX = 0, g_lastMouseY = 0;

void SetInput(Input input) {
    if (input == g_input) return;
    g_input = input;
    dslog::Write("In game: input -> %s",
                 input == Input::Keyboard ? "keyboard/mouse" : input == Input::Xbox ? "Xbox" : "PlayStation");
}

// ---- prompts ----
struct Prompt {
    uint32_t hash;
    int button;  // game joystick button (default.key numbering), -1 = D-pad
};
// Hashes from catalog.dat (PC_*_PROMPT). PC_TAB_KEY_PROMPT has no known pad equivalent and stays as it is.
constexpr Prompt kPrompts[] = {
    {3812760437u, 2},   // PC_ACCEPT_PROMPT        Return: Accept
    {1376302336u, 2},   // PC_SELECT_PROMPT        Return: Select
    {620529314u, 2},    // PC_MORE_INFO_PROMPT     Return: More information
    {2426184306u, 0},   // PC_BACK_PROMPT          Esc: Back
    {745684578u, 0},    // PC_CANCEL_PROMPT        Esc: Cancel
    {799972218u, 0},    // PC_ESC_FE_PROMPT        Esc: Main Menu
    {2484684419u, 0},   // PC_ESC_CONTINUE_PROMPT  Esc: Continue
    {4068979723u, 11},  // PC_ESC_RETURN_PROMPT    Esc: Return to mission (pause)
    {947741146u, 8},    // PC_F1_RETURN_PROMPT     F1: Return to mission (objectives)
    {2393634237u, -1},  // PC_DEBRIEF_PROMPT       Arrow Keys: Change soldier/screen
    {2522449822u, -1},  // PC_LR_SELECT_PROMPT     Left/Right: Select
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
    const char* text = GameLookup(table, hash);
    if (!g_enabled || g_input == Input::Keyboard || !text) return text;
    for (const Prompt& p : kPrompts) {
        if (p.hash != hash) continue;
        // One string per (language text, device) kept forever, so pointers handed to the game stay valid.
        static std::unordered_map<std::string, std::string> cache;
        std::string key = std::string(g_input == Input::Xbox ? "X" : "P") + text;
        auto it = cache.find(key);
        if (it == cache.end()) {
            const char* colon = strchr(text, ':');
            const char* action = colon ? colon + 1 : text;
            while (*action == ' ') ++action;
            it = cache.emplace(key, std::string(ButtonName(g_input, p.button)) + ": " + action).first;
        }
        return it->second.c_str();
    }
    return text;
}

// Replaces "mov eax,[0x5e6cb4]": the game's draw-cursor flag, off while a pad drives the menus.
uint32_t __cdecl CursorFlag() {
    uint32_t flag = *reinterpret_cast<uint32_t*>(kCursorFlag);
    return (g_enabled && g_input != Input::Keyboard) ? 0 : flag;
}
}  // namespace

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
    // Keyboard/mouse: any real input Windows saw (ignored right after pad activity, in case the pad counts), or
    // the game's own mouse position changing.
    LASTINPUTINFO li{sizeof li, 0};
    GetLastInputInfo(&li);
    LONG mx = *reinterpret_cast<LONG*>(kMouseX), my = *reinterpret_cast<LONG*>(kMouseY);
    bool mouseMoved = mx != g_lastMouseX || my != g_lastMouseY;
    g_lastMouseX = mx, g_lastMouseY = my;
    if ((li.dwTime != g_lastKbm || mouseMoved) && now - g_lastPadActivity > 250) SetInput(Input::Keyboard);
    g_lastKbm = li.dwTime;
}

void features::ApplyInGameInput() {
    g_enabled = settings::Get().controller;
    static bool hooked = false;
    if (hooked || !g_enabled) return;
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
    hooked = true;
    dslog::Write("[ok]   In-game pad prompts + cursor hiding");
}

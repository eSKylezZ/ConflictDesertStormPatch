// Co-op: the join screen and the session it starts (the split screen itself is splitscreen.cpp).
//
// Join screen = the menu feature's CO-OPERATIVE GAME screen (menus.cpp, front-end state menus::kCoopState), filled
// through menus::SetCoopScreenHooks: rows PLAYER 1..4 (who joined with which device; selecting a row begins). Joining is
// read here directly, per device, like DS1's join screen (and the Xbox's): keyboard Space / a pad's Cross (A) joins,
// Backspace / Circle (B) leaves, Enter / Options (Start) begins with 2+ players, Esc (or Back on a pad that hasn't
// joined) returns to the main menu. Players are numbered in join order.
//   pads: the game's own DirectInput joysticks [0x67E7DC + j*4] (count [0x67F6C0]; created by the enumeration
//         FUN_004CC690 / callback FUN_004CC370 with c_dfDIJoystick2), read raw (before any remapping): Poll vtable 25,
//         GetDeviceState vtable 9, GetDeviceInfo vtable 15 (vendor 0x054C PlayStation: Cross 1, Circle 2, Options 9;
//         others / Xbox 0x045E: A 0, B 1, Start 7).
//   begin: session = {players, joystick per player (-1 keyboard)}, then the main menu's START GAME path (its update
//         FUN_0045CA50, case item 0x3FD at 0x45CAA9: profile / difficulty / campaign exactly as in single player).
//         splitscreen.cpp's level-load hook builds the views from the session and gives each player block its device.
//   end:  the main menu (state 2) reached again.
// Without menus.h (a build without the menu feature) only the session API and the dev test hooks exist.
#include <windows.h>
#include <dinput.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#if __has_include("features/menus.h")
#include "features/menus.h"
#define DS_HAVE_MENUS 1
#endif

namespace {
constexpr uint32_t kJoysticks = 0x67E7DC, kJoystickCount = 0x67F6C0;
constexpr uint32_t kGameState = 0x63433C, kGameWindow = 0x62F41C;
constexpr int kMainMenuState = 2;
constexpr uint32_t kFrameSite = 0x414C06, kFrameFn = 0x4CAFE0;   // message loop: per-frame call after the game frame
constexpr uint32_t kStartGameCase = 0x45CAA9;                     // main menu update, case START GAME (0x3FD)

int g_players = 0;                 // session (0 = none)
int g_joystick[4] = {-1, -1, -1, -1};
bool g_sessionFresh = false;       // begun, the main menu not left yet

#ifndef DS_DIST
DWORD DevDword(const char* name, DWORD def) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v,
                        &size) == ERROR_SUCCESS ? v : def;
}
#endif

template <class T>
T& At(uintptr_t a) { return *reinterpret_cast<T*>(a); }

// ---- devices -----------------------------------------------------------------------------------------------------
enum Button { kJoin = 1, kLeave = 2, kStart = 4 };

IDirectInputDevice8A* Joystick(int j) {
    if (j < 0 || j >= 8 || j >= static_cast<int>(At<uint32_t>(kJoystickCount))) return nullptr;
    return At<IDirectInputDevice8A*>(kJoysticks + j * 4);
}

bool IsSony(IDirectInputDevice8A* dev) {
    DIDEVICEINSTANCEA info{};
    info.dwSize = sizeof info;
    return SUCCEEDED(dev->GetDeviceInfo(&info)) && LOWORD(info.guidProduct.Data1) == 0x054C;
}

// buttons held on pad j (kJoin / kLeave / kStart), 0 if absent
int PadButtons(int j) {
    IDirectInputDevice8A* dev = Joystick(j);
    if (!dev) return 0;
    dev->Poll();
    DIJOYSTATE2 s{};
    if (FAILED(dev->GetDeviceState(sizeof s, &s))) return 0;
    const bool sony = IsSony(dev);
    auto down = [&](int b) { return (s.rgbButtons[b] & 0x80) != 0; };
    return (down(sony ? 1 : 0) ? kJoin : 0) | (down(sony ? 2 : 1) ? kLeave : 0) | (down(sony ? 9 : 7) ? kStart : 0);
}

int KeyboardButtons() {
    const HWND game = At<HWND>(kGameWindow);
    if (!game || GetForegroundWindow() != game) return 0;
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    return (down(VK_SPACE) ? kJoin : 0) | (down(VK_BACK) ? kLeave : 0) | (down(VK_RETURN) ? kStart : 0);
}

void PadName(int j, char* out, size_t size) {   // short: the menu rows are narrow
    IDirectInputDevice8A* dev = Joystick(j);
    DIDEVICEINSTANCEA info{};
    info.dwSize = sizeof info;
    const WORD vendor = dev && SUCCEEDED(dev->GetDeviceInfo(&info)) ? LOWORD(info.guidProduct.Data1) : 0;
    if (vendor == 0x054C) std::snprintf(out, size, "PLAYSTATION PAD");
    else if (vendor == 0x045E) std::snprintf(out, size, "XBOX PAD");
    else std::snprintf(out, size, "CONTROLLER %d", j + 1);
}

// ---- join screen state ---------------------------------------------------------------------------------------------
int g_slots[4];            // joined devices in join order: -1 keyboard, j = joystick j
int g_joined = 0;
int g_prevKeyboard = 0, g_prevPad[8] = {};
bool g_open = false;
DWORD g_openedAt = 0;

bool Joined(int device) {
    for (int i = 0; i < g_joined; ++i)
        if (g_slots[i] == device) return true;
    return false;
}

void Leave(int device) {
    for (int i = 0; i < g_joined; ++i)
        if (g_slots[i] == device) {
            for (int k = i; k + 1 < g_joined; ++k) g_slots[k] = g_slots[k + 1];
            --g_joined;
            return;
        }
}

void ResetJoin() {
    g_joined = 0;
    g_prevKeyboard = KeyboardButtons();   // a key / button that opened the screen is still held: not a join
    for (int j = 0; j < 8; ++j) g_prevPad[j] = PadButtons(j);
    g_openedAt = GetTickCount();
#ifndef DS_DIST
    // Dev\CoopFakeJoin = n: n players joined at once (player 1 keyboard, the others pads 0, 1, ...) - background tests
    if (const DWORD n = DevDword("CoopFakeJoin", 0); n >= 1 && n <= 4) {
        for (DWORD i = 0; i < n; ++i) g_slots[g_joined++] = i == 0 ? -1 : static_cast<int>(i - 1);
        dslog::Write("[dev] co-op: %lu fake players joined", static_cast<unsigned long>(n));
    }
#endif
}

void StartSession() {
    g_players = g_joined;
    for (int i = 0; i < 4; ++i) g_joystick[i] = i < g_joined ? g_slots[i] : -1;
    g_sessionFresh = true;
    dslog::Write("Co-op: session with %d players (devices %d %d %d %d; -1 = keyboard)", g_players, g_joystick[0],
                 g_joystick[1], g_joystick[2], g_joystick[3]);
}

#ifdef DS_HAVE_MENUS
// The main menu's START GAME case: the update's prologue pushed esi; the case code ends with pop esi; ret.
__declspec(naked) int StartGameCase() {
    __asm {
        push esi
        mov eax, kStartGameCase
        jmp eax
    }
}

namespace menus = features::menus;
constexpr int kIdRow = 0x0E80;
constexpr uint32_t kTxtRow = 0xC0C30010, kTxtRowDesc = 0xC0C30020, kTxtTitle = 0xC0C30003;
wchar_t g_rowText[4][96], g_rowDesc[4][96], g_startText[96];

void UpdateTexts() {
    for (int i = 0; i < 4; ++i) {
        if (i < g_joined) {
            char name[80];
            if (g_slots[i] < 0) std::snprintf(name, sizeof name, "KEYBOARD & MOUSE");
            else PadName(g_slots[i], name, sizeof name);
            std::swprintf(g_rowText[i], 96, L"PLAYER %d: %hs", i + 1, name);
            std::swprintf(g_rowDesc[i], 96, L"Joined - Circle / B / Backspace to leave");
        } else {
            std::swprintf(g_rowText[i], 96, L"PLAYER %d: PRESS TO JOIN", i + 1);
            std::swprintf(g_rowDesc[i], 96, L"Cross / A on a controller or Space on the keyboard joins");
        }
    }
    if (g_joined >= 2) std::swprintf(g_startText, 96, L"%d players - Enter / Options / Start or Select begins", g_joined);
    else std::swprintf(g_startText, 96, L"2 to 4 players: Cross / A / Space joins, Circle / B / Backspace leaves");
    for (int i = 0; i < 4; ++i)
        if (i >= g_joined || g_joined >= 2) std::wcscpy(g_rowDesc[i], g_startText);
}

bool CoopSetup(void* screen) {
    if (!g_open) {   // entered from the main menu
        g_open = true;
        ResetJoin();
    }
    UpdateTexts();
    menus::BeginList(screen);
    for (int i = 0; i < 4; ++i) menus::AddItem(screen, i, kIdRow + i, kTxtRow + i, kTxtRowDesc + i);
    menus::EndList(screen, 4, kTxtTitle);
    return true;
}

// every front-end frame while the screen is current
int CoopUpdate(void*, int activatedId, bool backPressed) {
    bool joinedNow = false, leftNow = false;
    const int kb = KeyboardButtons(), kbEdge = kb & ~g_prevKeyboard;
    g_prevKeyboard = kb;
    if ((kbEdge & kJoin) && !Joined(-1) && g_joined < 4) g_slots[g_joined++] = -1, joinedNow = true;
    if ((kbEdge & kLeave) && Joined(-1)) Leave(-1), leftNow = true;
    bool start = (kbEdge & kStart) && Joined(-1);
    for (int j = 0; j < 8; ++j) {
        const int b = PadButtons(j), edge = b & ~g_prevPad[j];
        g_prevPad[j] = b;
        if ((edge & kJoin) && !Joined(j) && g_joined < 4) g_slots[g_joined++] = j, joinedNow = true;
        else if ((edge & kLeave) && Joined(j)) Leave(j), leftNow = true;
        if ((edge & kStart) && Joined(j)) start = true;
    }
#ifndef DS_DIST
    if (const DWORD ms = DevDword("CoopAutoBegin", 0); ms && GetTickCount() - g_openedAt > ms && g_joined >= 2) {
        dslog::Write("[dev] co-op: auto begin");
        start = true;
    }
#endif
    if (joinedNow || leftNow) dslog::Write("Co-op: %d player(s) joined", g_joined);
    UpdateTexts();
    // the menu's own select (Enter / Cross / A / click on any row) begins too - not on the press that joined
    if (!joinedNow && activatedId >= kIdRow && activatedId < kIdRow + 4) start = true;
    if (start && g_joined >= 2) {
        StartSession();
        g_open = false;
        StartGameCase();
        return 1;
    }
    if (backPressed) {
        if (leftNow) return 1;   // Circle / B of a joined pad: it left, the screen stays
        g_open = false;
        g_joined = 0;
        return 0;                // the menu goes back to the main menu
    }
    return 0;
}
#endif

// ---- per frame -------------------------------------------------------------------------------------------------------
void __cdecl OnFrame() {
    const int state = At<int>(kGameState);
    if (g_players && state == kMainMenuState && !g_sessionFresh) {
        dslog::Write("Co-op: session ended (main menu)");
        g_players = 0;
        for (int& j : g_joystick) j = -1;
    }
    if (state != kMainMenuState) g_sessionFresh = false;
#if defined(DS_HAVE_MENUS) && !defined(DS_DIST)
    // Dev\CoopAutoOpen = 1: the co-op screen opens once the main menu shows (background tests can't press keys)
    static bool autoOpened = false;
    static DWORD mainSince = 0;
    if (state == kMainMenuState && !autoOpened && DevDword("CoopAutoOpen", 0)) {
        if (!mainSince) mainSince = GetTickCount();
        if (GetTickCount() - mainSince > 2000) {
            autoOpened = true;
            dslog::Write("[dev] co-op: opening the co-op screen");
            menus::SetState(menus::kCoopState);
        }
    }
#endif
}

uint32_t g_frameFn = kFrameFn;
__declspec(naked) void FrameStub() {
    __asm {
        pushad
        call OnFrame
        popad
        jmp dword ptr [g_frameFn]
    }
}
}  // namespace

int features::CoopPlayers() { return g_players; }
int features::CoopJoystick(int player) { return player >= 0 && player < 4 ? g_joystick[player] : -1; }

void features::ApplyCoop() {
    if (!patch::HookCall(kFrameSite, reinterpret_cast<void*>(&FrameStub), kFrameFn)) {
        dslog::Write("[skip] Co-op: unknown code at the frame hook");
        return;
    }
#ifdef DS_HAVE_MENUS
    menus::AddText(kTxtTitle, L"JOIN CO-OPERATIVE GAME");
    for (int i = 0; i < 4; ++i) {
        menus::AddText(kTxtRow + i, g_rowText[i]);
        menus::AddText(kTxtRowDesc + i, g_rowDesc[i]);
    }
    UpdateTexts();
    menus::CoopScreenHooks hooks;
    hooks.setup = &CoopSetup;
    hooks.update = &CoopUpdate;
    menus::SetCoopScreenHooks(hooks);
    dslog::Write("[ok]   Co-op: join screen on the CO-OPERATIVE GAME menu (state 0x%X)", menus::kCoopState);
#else
    dslog::Write("[ok]   Co-op: session only (no menu feature in this build)");
#endif
}

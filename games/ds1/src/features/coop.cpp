// Local co-op set-up in the front-end, modelled on the Xbox version's "Join Co-operative Game" screen (Xbox state 3,
// class ctor 0x70290 in default.xbe): each player presses a button on their own device to join, then picks the
// button icons they see; START with two or more players goes on to the single-player mission flow in split screen.
// The PC's MULTIPLAYER (network) menu stays as it is - CO-OP is an extra main-menu entry.
//
// Front-end (PC): state machine FUN_0047ef20(state); FUN_00480930(state) -> that state's screen object (states
// 1..0x3d). Screens are 0x20-byte objects (base ctor 0x455920: +4 = -1, +8 item list, +0xc = 1) with a vtable
// (+0x04 create list, +0x0c set up items, +0x10 update, +0x14 draw, +0x18 enter, +0x1c leave, +0x44 back prompt,
// +0x50 set title). What we add:
//   main menu   list capacity 4 -> 5 (0x4150BD) and five item objects (0x4150CF); its set-up (vtable +0x0c) is
//               wrapped: CO-OP (id 0x401) is built on the fifth item and the item pool is rotated so it is listed
//               second (the list shows items in pool order); the selection handler (0x4152F8) sends it to the CO-OP
//               screen (state 0x48, online.cpp: SPLIT SCREEN -> 0x3E / ONLINE), where the join screen's Back returns
//   state 0x3E  FUN_00480930 returns our screen for it: a main-menu-class object with its own vtable (create / set up
//               / update ours, back prompt + back handling from the MULTIPLAYER screen)
// Joining: every frame on the screen, the game's own DirectInput joysticks (0x754BC8[], count 0x754BE8 - the devices
// the players will use in the mission, joystick index = block+0x3c8) are read raw (Poll + GetDeviceState, before our
// button remap) and typed by vendor id (Sony = PlayStation, Microsoft = Xbox). Cross / A joins, Circle / B leaves,
// left / right changes that player's prompt icons, Options / Start begins (2+ players). Keyboard: Space joins,
// Backspace leaves, Left / Right, Enter begins; Esc (or Circle / B on a pad that has not joined) goes back to the
// main menu - the game's own menu Back (Triangle) is not used here. Players are numbered in join order.
// The rows and the prompt line show the real buttons (overlay.cpp icons): each text leaves a gap of spaces where
// the icons go, measured with the game's own font, and our prompts replace the game's (getters +0x34/+0x44 = 0).
// Begin: a co-op session starts - the split flag 0x606410 / player count 0x606414 are set (the PC kept the Xbox
// logic that reads them: the uniform screen FUN_0049baf0 starts 3-4 players at "mission3.dll" when 0x606424 is 0),
// the campaign's first level is copied to the next-level slot like DESERT STORM CAMPAIGN does (FUN_0048ce40), and
// the Difficulty screen (state 5) opens -> Select Uniform (6) -> the level. splitscreen.cpp takes the player count
// and each player's device from the session. The session ends when the main menu is shown again.
// No SELECT UNIFORM screen (uniforms are picked per soldier on the Squad Loadout screen): the Difficulty screen's
// update (FUN_00413670, vtable slot 0x5D85F8) went to state 6 with `push 6; call 0x47EF20` at 0x4136CB - that call
// now runs the uniform screen's accept instead (0x49BC8D..0x49BD08: the 3-4 player "mission3.dll" rule, the new
// campaign FUN_004576c0 / FUN_00457750 on [0x60EDC0], FUN_004475b0(1)) with the uniform fixed to SAS through the
// game's setter FUN_0040a320(0) (global [0x60ED1C], also the English speech language - Training had set American),
// and the wrapped update returns 3 = start the level. The uniform also chose the starting skill sets and the rank /
// medal names (UK vs US): those are the SAS ones now.
// Mission list: in a session the uniform screen (update, vtable slot 0x5D9610) opens the game's hidden MISSION LIST
// (state 8, normally behind the CHEATS menu) instead of starting the campaign. Co-op has its own progress (registry
// value CoopProgress, one bit per mission): only the starting mission is open (Mission 1 with 2 players, Mission 3
// with 3-4 - Connors and Jones join in Mission 3), finishing a mission unlocks the next, finished ones stay open.
// Locked missions show "? ? ? ? ?", missions the group is too big for keep their title with a note; both are drawn
// grey (list colours +0x54/+0x58 swapped in front of the item draw callback 0x4596B0) and cannot be picked (the
// id -> index call at 0x4595F1 answers 12 = none). Mission table 0x5EB8A8: 12 x {level index, mission, title hash};
// level names at 0x5E6CC0[level]. A mission counts as finished when the session's next level is the next mission.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kStateMainMenu = 1, kStateCoop = 0x3E, kStateCoopMenu = 0x48;
constexpr uint32_t kSetState = 0x47EF20;           // cdecl (state)
constexpr uint32_t kScreenForState = 0x480930;
constexpr uint32_t kMainMenuScreen = 0x617A00;     // state 1's screen object
constexpr uint32_t kMainMenuCtor = 0x415020, kMainMenuVtable = 0x5D86B8, kMainMenuSetup = 0x415120;
constexpr uint32_t kMpVtable = 0x5D90C0;           // MULTIPLAYER screen (back handling / "Esc: Back" prompt)
constexpr uint32_t kMenuCapacity = 0x4150BD;       // push 4 (list capacity)
constexpr uint32_t kMenuItemCount = 0x4150CF;      // mov edi, 4 (item objects)
constexpr uint32_t kMainMenuSetupSlot = kMainMenuVtable + 0x0C;
constexpr uint32_t kMenuSelect = 0x4152F8, kMenuSelectCont = 0x4152FF, kMenuSelectDone = 0x415361;
constexpr uint32_t kItemDrawCb = 0x4153E0;         // main menu item draw callback
constexpr uint32_t kAlloc = 0x4B83D0;              // cdecl (size, tag)
constexpr uint32_t kListCtor = 0x4E5B10;           // thiscall (font, sheet, 1)
constexpr uint32_t kListReserve = 0x4E56C0;        // thiscall (count) -> nonzero
constexpr uint32_t kListAddToPool = 0x4E58C0;      // thiscall (item)
constexpr uint32_t kListSelected = 0x4E5D80;       // thiscall () -> activated item id (short)
constexpr uint32_t kItemCtor = 0x4E4F90;           // thiscall ()
constexpr uint32_t kItemSet = 0x4E4FB0;            // thiscall (id, text hash, 0, flags 0x24, 0, 0, draw cb, 0)
constexpr uint32_t kDrawPanel = 0x456EB0;           // screen thiscall (6, 0): menu panel background
constexpr uint32_t kDrawTitle = 0x456F40;           // screen thiscall (title hash, 1)
constexpr uint32_t kBackPressed = 0x478380;        // cdecl (0) -> nonzero when the menu back action fired
constexpr uint32_t kMenuFont = 0x60EDB4, kHudSheet = 0x60EE18;
constexpr uint32_t kFrontEndState = 0x617C18;       // current front-end state
constexpr uint32_t kGameWindow = 0x606A60;
constexpr uint32_t kJoysticks = 0x754BC8, kJoystickCount = 0x754BE8;  // IDirectInputDevice8A*[8], count
constexpr uint32_t kStateDifficulty = 5;
constexpr uint32_t kSplitFlag = 0x606410, kSplitPlayers = 0x606414, kSplitLoadedGame = 0x606424;
constexpr uint32_t kCampaign = 0x60EDC0;            // campaign object: +0x37 first level name
constexpr uint32_t kNextLevel = 0x5E7028;           // char*: level the next load uses
constexpr uint32_t kStateMissionList = 8;
constexpr uint32_t kUniformUpdateSlot = 0x5D9610;   // uniform screen vtable +0x10 (FUN_0049baf0)
constexpr uint32_t kMissionListScreen = 0x617C00;   // state 8's screen object
constexpr uint32_t kMissionListUpdateSlot = 0x5D8E40;  // mission list vtable +0x10 (FUN_004595d0)
constexpr uint32_t kMissionDrawCbPush = 0x459508;   // push 0x4596b0 (mission item draw callback)
constexpr uint32_t kMissionDrawCb = 0x4596B0;
constexpr uint32_t kMissionIndexCall = 0x4595F1, kItemIndex = 0x4E6F80;  // list thiscall (item id) -> list index
constexpr uint32_t kMissionTable = 0x5EB8A8, kLevelNames = 0x5E6CC0;
constexpr uint32_t kLoadPending = 0x60EC8C;         // byte: 1 = a level loads this frame
constexpr int kMissions = 12, kMissionItemId = 0x1005;

constexpr uint16_t kCoopItemId = 0x401, kModsItemId = 0x402, kSlotItemId = 0x500;
constexpr int kSlots = 4;
constexpr int kVtableEntries = 21;

// Text hashes of our own strings (answered by features::CoopText, not in catalog.dat).
enum : uint32_t {
    kHashCoop = 0xC0070001,
    kHashTitle = 0xC0070002,
    kHashSlot0 = 0xC0070010,  // .. +3
    kHashJoinPrompt = 0xC0070020,
    kHashStartPrompt = 0xC0070021,
};

void SetState(uint32_t state);

// --- players ---------------------------------------------------------------------------------------------------------

using features::PadType;
enum class Icons : int { Auto, PlayStation, Xbox, Keyboard, Count };
constexpr int kKeyboard = 8;      // device id of the keyboard (joysticks are 0..7)
// Several keyboards (multikbm.cpp Raw Input): each keyboard joins on its own, device id kRawKeyboard + its device
// number; a player's mouse is the next unpaired mouse clicked on this screen.
constexpr int kRawKeyboard = 16, kRawDevices = 32;
bool IsKeyboard(int device) { return device == kKeyboard || (device >= kRawKeyboard && device < kRawKeyboard + kRawDevices); }
constexpr int kFakeDevice = 100;  // dev builds: CoopFakeJoin test players

struct Slot {
    int device = -1;  // -1 empty, 0..7 game joystick, kKeyboard, kRawKeyboard + n, kFakeDevice + n
    PadType type = PadType::Keyboard;
    Icons icons = Icons::Auto;
    std::string name;
    int keyProfile = -1;  // keyboard player: saved key profile (controls.cpp), -1 = the current keys
    int team = 0;         // versus: team 0 / 1 (left / right on the join screen)
    int cls = 0;          // versus: class = character skill 0..3
    int field = 0;        // versus: the field left / right changes - 0 team, 1 class (R1 / R2, RB / RT, Tab)
    int mouse = -1;       // several keyboards: the player's mouse (multikbm device number), -1 none yet
};
Slot g_slots[4];
uint32_t g_prevActions[kRawKeyboard + kRawDevices];

// The running co-op session (from Begin until the main menu is shown again).
struct Session {
    int players = 0;
    int joystick[4] = {-1, -1, -1, -1};
    int style[4] = {0, 0, 0, 0};  // icon style per player: 0 keyboard, 1 PlayStation, 2 Xbox (Auto resolved)
    int keyProfile[4] = {-1, -1, -1, -1};
    int kbmKeyboard[4] = {-1, -1, -1, -1};   // several keyboards: each keyboard player's devices (multikbm.cpp)
    int kbmMouse[4] = {-1, -1, -1, -1};
    int team[4] = {0, 1, 0, 1};  // versus teams
    int cls[4] = {0, 0, 0, 0};   // versus classes (character skill)
    bool online = false;     // an online lobby session: prompt styles follow each local player's device in use
    int mission = -1;        // mission (0-11) of the level last loaded in this session
    std::string lastLevel;   // to see each level load once
} g_session;
bool g_preselected = false;  // co-op mission list: first open mission highlighted

// --- co-op progress (settings key: CoopProgress, bit per mission) --------------------------------------------------

uint32_t LoadProgress() {
    uint32_t v = 0;
    settings::ReadDword(false, "CoopProgress", v);
    return v;
}

void MarkFinished(int mission) {
    const uint32_t v = LoadProgress() | (1u << mission);
    settings::WriteDword(false, "CoopProgress", v);
    dslog::Write("Co-op: mission %d finished (progress 0x%03X)", mission + 1, v);
}

enum class MissionState { Open, Locked, TooManyPlayers };

// Missions 1-2 only have Bradley and Foley (Connors and Jones join in Mission 3), like the Xbox rule in FUN_0049baf0.
MissionState StateOf(int mission, int players) {
    if (players >= 3 && mission < 2) return MissionState::TooManyPlayers;
    const int start = players >= 3 ? 2 : 0;
    const uint32_t done = LoadProgress();
    const bool open = mission == start || (done & (1u << mission)) || (mission > 0 && (done & (1u << (mission - 1))));
    return open ? MissionState::Open : MissionState::Locked;
}

int MissionOfLevel(const char* level) {
    int found = -1;
    for (int m = 0; m < kMissions; ++m) {
        const uint32_t first = reinterpret_cast<const uint32_t*>(kMissionTable)[m * 3];
        for (uint32_t l = first; l < 19; ++l) {
            const char* name = reinterpret_cast<const char* const*>(kLevelNames)[l];
            const uint32_t next = m + 1 < kMissions ? reinterpret_cast<const uint32_t*>(kMissionTable)[(m + 1) * 3] : 18;
            if (l >= next) break;
            if (name && _stricmp(name, level) == 0) found = m;
        }
    }
    return found;
}

enum Action : uint32_t { kJoin = 1, kLeave = 2, kStart = 4, kLeft = 8, kRight = 16, kBack = 32, kCycle = 64 };

// Versus classes: the network game's character skill (player table +0x16 -> FUN_00461390 role: kit and skin).
constexpr const char* kClassNames[] = {"RIFLEMAN", "HEAVY WEAPONS", "SNIPER", "ENGINEER"};  // skill 0..3
constexpr int kClasses = 4;

int JoinedCount() {
    int n = 0;
    for (const Slot& s : g_slots) n += s.device >= 0;
    return n;
}

bool g_keepSlots = false;  // set when the co-op menu's Back returns here: the players stay joined
bool g_versus = false;     // the join screen was opened from VERSUS > SPLIT SCREEN (versus.cpp)
constexpr uint32_t kStateVersus = 0x42;

uint32_t RawKeyboardActions(int dev);  // below

void ResetSlots() {
    for (Slot& s : g_slots) s = Slot{};
    memset(g_prevActions, 0xFF, sizeof g_prevActions);  // ignore buttons already held when the screen opens
    for (int dev = 0; dev < kRawDevices; ++dev) g_prevActions[kRawKeyboard + dev] = RawKeyboardActions(dev);
}

void* Joystick(int j) { return reinterpret_cast<void**>(kJoysticks)[j]; }

// DIDEVICEINSTANCEA: guidProduct.Data1 = MAKELONG(vendor id, product id).
void Describe(int j, PadType& type, std::string& name) {
    struct Instance {
        DWORD size;
        GUID instance, product;
        DWORD devType;
        char instanceName[260], productName[260];
        GUID ffDriver;
        WORD usagePage, usage;
    } info{};
    info.size = sizeof info;
    void* dev = Joystick(j);
    using GetInfo = HRESULT(__stdcall*)(void*, Instance*);
    type = PadType::Generic;
    name = "CONTROLLER";
    if (!dev || FAILED(reinterpret_cast<GetInfo>((*reinterpret_cast<void***>(dev))[15])(dev, &info))) return;
    const WORD vid = LOWORD(info.product.Data1), pid = HIWORD(info.product.Data1);
    if (vid == 0x054C) {
        type = PadType::PlayStation;
        name = (pid == 0x0CE6 || pid == 0x0DF2) ? "DUALSENSE"
               : (pid == 0x05C4 || pid == 0x09CC) ? "DUALSHOCK 4"
                                                  : "PLAYSTATION CONTROLLER";
    } else if (vid == 0x045E) {
        type = PadType::Xbox;
        name = "XBOX CONTROLLER";
    } else if (info.productName[0]) {
        name = info.productName;
        for (char& c : name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }
}

// Raw DIJOYSTATE2 (range +-128 set by the game): lX +0, POV +32, buttons +48.
uint32_t JoystickActions(int j, PadType type) {
    void* dev = Joystick(j);
    if (!dev) return 0;
    uint8_t state[0x110] = {};
    using Method = HRESULT(__stdcall*)(void*);
    using GetState = HRESULT(__stdcall*)(void*, DWORD, void*);
    void** vt = *reinterpret_cast<void***>(dev);
    reinterpret_cast<Method>(vt[25])(dev);  // Poll
    if (FAILED(reinterpret_cast<GetState>(vt[9])(dev, sizeof state, state))) return 0;
    const LONG x = *reinterpret_cast<LONG*>(state);
    const DWORD pov = *reinterpret_cast<DWORD*>(state + 32);
    auto button = [&](int b) { return (state[48 + b] & 0x80) != 0; };
    uint32_t a = 0;
    switch (type) {
        case PadType::PlayStation:  // DirectInput: 0 Square 1 Cross 2 Circle 3 Triangle ... 9 Options
            if (button(1)) a |= kJoin;
            if (button(2)) a |= kLeave;
            if (button(9)) a |= kStart;
            if (button(5) || button(7)) a |= kCycle;  // R1, R2
            break;
        case PadType::Xbox:  // 0 A 1 B ... 5 RB, 7 Start; triggers on Z (LT +, RT -)
            if (button(0)) a |= kJoin;
            if (button(1)) a |= kLeave;
            if (button(7)) a |= kStart;
            if (button(5) || *reinterpret_cast<LONG*>(state + 8) < -64) a |= kCycle;
            break;
        default:
            if (button(0) || button(1)) a |= kJoin;
            if (button(7) || button(9)) a |= kStart;
            break;
    }
    const bool povSet = LOWORD(pov) != 0xFFFF;
    if (x < -64 || (povSet && pov > 22500 && pov < 31500)) a |= kLeft;
    if (x > 64 || (povSet && pov > 4500 && pov < 13500)) a |= kRight;
    return a;
}

uint32_t KeyboardActions() {
    auto down = [](int vk) { return features::KeyHeld(vk); };
    uint32_t a = 0;
    if (down(VK_SPACE)) a |= kJoin;
    if (down(VK_BACK)) a |= kLeave;
    if (down(VK_ESCAPE)) a |= kBack;
    if (down(VK_RETURN)) a |= kStart;
    if (down(VK_LEFT)) a |= kLeft;
    if (down(VK_RIGHT)) a |= kRight;
    if (down(VK_TAB)) a |= kCycle;
    return a;
}

// One keyboard by Raw Input (multikbm.cpp): game key codes (scan code, +0x100 extended).
uint32_t RawKeyboardActions(int dev) {
    if (features::ConsoleOpen() || features::UiKeyboardCaptured()) return 0;
    auto down = [dev](uint32_t code) { return features::KbmKeyHeld(dev, code); };
    uint32_t a = 0;
    if (down(0x39)) a |= kJoin;     // Space
    if (down(0x0E)) a |= kLeave;    // Backspace
    if (down(0x01)) a |= kBack;     // Esc
    if (down(0x1C) || down(0x11C)) a |= kStart;   // Enter, keypad Enter
    if (down(0x14B)) a |= kLeft;
    if (down(0x14D)) a |= kRight;
    if (down(0x0F)) a |= kCycle;    // Tab
    return a;
}

Slot* SlotOf(int device) {
    for (Slot& s : g_slots)
        if (s.device == device) return &s;
    return nullptr;
}

void Join(int device, PadType type, const std::string& name) {
    for (Slot& s : g_slots) {
        if (s.device >= 0) continue;
        s = Slot{device, type, Icons::Auto, name};
        s.team = static_cast<int>(&s - g_slots) % 2;  // versus: alternate teams until a player picks
        dslog::Write("Co-op: player %d joined with %s", static_cast<int>(&s - g_slots) + 1, name.c_str());
        return;
    }
}

void Leave(int device) {
    Slot* s = SlotOf(device);
    if (!s) return;
    dslog::Write("Co-op: player %d (%s) left", static_cast<int>(s - g_slots) + 1, s->name.c_str());
    for (Slot* t = s; t + 1 < g_slots + kSlots; ++t) *t = t[1];  // keep players numbered 1..n
    g_slots[kSlots - 1] = Slot{};
}

overlay::Icon ConfirmIcon(const Slot& s);  // below

void Begin() {
    g_session = Session{};
    for (const Slot& s : g_slots) {
        if (s.device < 0) continue;
        const overlay::Icon icon = ConfirmIcon(s);
        g_session.style[g_session.players] =
            icon == overlay::Icon::PsCross ? 1 : icon == overlay::Icon::XbCross ? 2 : 0;
        g_session.keyProfile[g_session.players] = IsKeyboard(s.device) ? s.keyProfile : -1;
        if (s.device >= kRawKeyboard && IsKeyboard(s.device)) {
            g_session.kbmKeyboard[g_session.players] = s.device - kRawKeyboard;
            g_session.kbmMouse[g_session.players] = s.mouse;
        }
        g_session.team[g_session.players] = s.team;
        g_session.cls[g_session.players] = s.cls;
        g_session.joystick[g_session.players++] = s.device < kKeyboard ? s.device : -1;
    }
    features::KbmSetPlayers(g_session.kbmKeyboard, g_session.kbmMouse);   // 2+ keyboards: each reads their own
    *reinterpret_cast<uint32_t*>(kSplitFlag) = 1;
    *reinterpret_cast<uint32_t*>(kSplitPlayers) = static_cast<uint32_t>(g_session.players);
    *reinterpret_cast<uint32_t*>(kSplitLoadedGame) = 0;
    if (g_versus) {  // versus: the players' devices as for co-op, then the match set-up instead of the campaign
        dslog::Write("Co-op: %d players joined for versus", g_session.players);
        features::VersusPlayersJoined();
        return;
    }
    const char* first = *reinterpret_cast<char**>(kCampaign) + 0x37;
    strcpy(*reinterpret_cast<char**>(kNextLevel), first);
    dslog::Write("Co-op: session with %d players, campaign from %s", g_session.players, first);
    SetState(2);  // the single-player menu as CO-OPERATIVE CAMPAIGN: campaign or load game
}

// Every level load of the session: the previous mission counts as finished when the next one follows it
// (or the outro follows Mission 12).
void TrackProgress() {
    if (!g_session.players || *reinterpret_cast<uint8_t*>(kLoadPending) != 1) return;
    const char* next = *reinterpret_cast<const char**>(kNextLevel);
    if (!next || g_session.lastLevel == next) return;
    g_session.lastLevel = next;
    const int mission = MissionOfLevel(next);
    if (g_session.mission >= 0 && (mission == g_session.mission + 1 ||
                                   (g_session.mission == kMissions - 1 && _stricmp(next, "Outro.dll") == 0)))
        MarkFinished(g_session.mission);
    if (mission >= 0) g_session.mission = mission;
}

void EndSession() {
    dslog::Write("Co-op: session ended");
    g_session = Session{};
    features::KbmSetPlayers(nullptr, nullptr);
    *reinterpret_cast<uint32_t*>(kSplitFlag) = 0;
}
}  // namespace

// Online co-op (online.cpp / lockstep.cpp): the same co-op session on every PC - n players, split screen, no local
// devices (lockstep feeds each player's input). The host then goes on like the join screen's Begin: the single-player
// menu as CO-OPERATIVE CAMPAIGN (campaign -> difficulty -> co-op mission list); the joiners wait for the host's level.
void features::BeginOnlineSession(int players, bool host) {
    g_session = Session{};
    g_session.players = players;
    g_session.online = true;
    *reinterpret_cast<uint32_t*>(kSplitFlag) = 1;
    *reinterpret_cast<uint32_t*>(kSplitPlayers) = static_cast<uint32_t>(players);
    *reinterpret_cast<uint32_t*>(kSplitLoadedGame) = 0;
    g_versus = false;
    dslog::Write("Co-op: online session with %d players (%s)", players, host ? "host" : "joiner");
    if (!host) return;
    strcpy(*reinterpret_cast<char**>(kNextLevel), *reinterpret_cast<char**>(kCampaign) + 0x37);
    SetState(2);
}
#ifndef DS_DIST
bool features::CoopDevBegin() {
    if (*reinterpret_cast<uint32_t*>(0x617C18) != 0x3E || JoinedCount() < 2) return false;
    Begin();
    return true;
}
#endif
void features::EndOnlineSession() {
    if (g_session.players) EndSession();
}

namespace {

void Handle(int device, uint32_t actions, PadType type, const std::string& name) {
    const uint32_t pressed = actions & ~g_prevActions[device];
    g_prevActions[device] = actions;
    if (!pressed) return;
    Slot* s = SlotOf(device);
    if (!s) {
        if (pressed & kJoin) Join(device, type, name);
        else if (pressed & (kLeave | kBack)) g_versus ? features::VersusJoinBack() : SetState(features::CoopMenuState());  // back
        return;
    }
    if (pressed & kBack) return g_versus ? features::VersusJoinBack() : SetState(features::CoopMenuState());
    if (pressed & kLeave) return Leave(device);
    if (g_versus && (pressed & kCycle) && features::VersusTeams()) {  // versus: R1 / R2 (RB / RT, Tab) - next field
        s->field ^= 1;
        return;
    }
    if (g_versus && (pressed & (kLeft | kRight))) {  // versus: left / right changes the field (deathmatch: class)
        if (s->field == 0 && features::VersusTeams()) s->team ^= 1;
        else s->cls = (s->cls + ((pressed & kRight) ? 1 : kClasses - 1)) % kClasses;
        return;
    }
    if (IsKeyboard(s->device) && (pressed & (kLeft | kRight))) {
        // Keyboard player: left / right picks the keys - current, or a saved profile.
        const int step = (pressed & kRight) ? 1 : -1;
        for (int i = 0; i < 5; ++i) {
            s->keyProfile = (s->keyProfile + 1 + step + 5) % 5 - 1;
            if (s->keyProfile < 0 || features::KeyProfileSaved(s->keyProfile)) break;
        }
        return;
    }
    const int n = static_cast<int>(Icons::Count);
    if (pressed & kLeft) s->icons = static_cast<Icons>((static_cast<int>(s->icons) + n - 1) % n);
    if (pressed & kRight) s->icons = static_cast<Icons>((static_cast<int>(s->icons) + 1) % n);
    if ((pressed & kStart) && JoinedCount() >= 2) Begin();
}

void PollDevices() {
    if (!features::GameFocused()) return;
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), kKeyboard);
    for (int j = 0; j < count; ++j) {
        PadType type;
        std::string name;
        Describe(j, type, name);
        Handle(j, JoystickActions(j, type), type, name);
    }
    // Several keyboards (Raw Input running and a keyboard seen): each joins on its own; else the merged keyboard.
    if (features::KbmStart() && features::KbmDeviceCount(false) > 0) {
        const int keyboards = features::KbmDeviceCount(false);
        for (int k = 0; k < keyboards; ++k) {
            const int dev = features::KbmKeyboardIndex(k);
            if (dev < 0 || dev >= kRawDevices) continue;
            char name[32];
            snprintf(name, sizeof name, keyboards > 1 ? "KEYBOARD %d" : "KEYBOARD & MOUSE", k + 1);
            const uint32_t actions = RawKeyboardActions(dev);
            Handle(kRawKeyboard + dev, actions, PadType::Keyboard, name);
        }
        // a keyboard player's left click on an unpaired mouse makes it theirs (first keyboard player still without one)
        const int mouse = features::KbmMouseClicked();
        bool used = mouse < 0;
        for (const Slot& s : g_slots) used = used || s.mouse == mouse;
        for (Slot& s : g_slots)
            if (!used && s.device >= kRawKeyboard && IsKeyboard(s.device) && s.mouse < 0) {
                s.mouse = mouse;
                dslog::Write("Co-op: player %d's mouse is device %d", static_cast<int>(&s - g_slots) + 1, mouse);
                break;
            }
        // a player whose devices were unplugged keeps the slot (plugging back in finds the same number)
        return;
    }
    Handle(kKeyboard, KeyboardActions(), PadType::Keyboard, "KEYBOARD & MOUSE");
}

}  // namespace

uint32_t features::CoopDeviceActions(int device, PadType& type, std::string& name) {
    if (device == kKeyboard) {
        type = PadType::Keyboard;
        name = "KEYBOARD & MOUSE";
        return features::GameFocused() ? KeyboardActions() : 0;
    }
    if (device < 0 || device >= std::min(*reinterpret_cast<int*>(kJoystickCount), kKeyboard)) return 0;
    Describe(device, type, name);
    return features::GameFocused() ? JoystickActions(device, type) : 0;
}

namespace {
// Icons of a player's chosen style (Auto = by device).
overlay::Icon ConfirmIcon(const Slot& s) {
    Icons style = s.icons;
    if (style == Icons::Auto)
        style = s.type == PadType::PlayStation ? Icons::PlayStation : s.type == PadType::Keyboard ? Icons::Keyboard : Icons::Xbox;
    switch (style) {
        case Icons::PlayStation: return overlay::Icon::PsCross;
        case Icons::Keyboard: return overlay::Icon::KbEnter;
        default: return overlay::Icon::XbCross;
    }
}

// Row layout: the text drawn by the game has a gap of spaces; the icons are drawn into it (CoopDraw).
struct RowIcons {
    std::string prefix;  // text before the gap
    overlay::Icon icons[3];
    int count = 0;
};
RowIcons g_rowIcons[4];
int IconSize();  // below (needs the screen)

// A run of spaces at least `pixels` wide in the menu font.
std::string Gap(int pixels) {
    void* font = overlay::MenuFont();
    const int space = std::max(1, overlay::TextWidth(font, "A A") - overlay::TextWidth(font, "AA"));
    return std::string(static_cast<size_t>(pixels / space + 1), ' ');
}


uint8_t* g_screen = nullptr;
bool g_panelsDue = false;      // CoopDraw ran this frame: PanelsLateDraw draws the players' panels
float g_panelsAlpha = 1.0f;    // the menus' fade when CoopDraw ran
void* g_vtable[kVtableEntries];
bool g_installed = false;

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }

void SetItem(void* item, uint16_t id, uint32_t hash) {
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    reinterpret_cast<Fn>(kItemSet)(item, id, hash, 0, 0x24, 0, 0, kItemDrawCb, 0);
}

void AddToList(void* list, void* item) {
    using Fn = void(__thiscall*)(void*, void*);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(list))[1])(list, item);
}

void SetState(uint32_t state) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(state); }

// --- main menu: the CO-OP entry ------------------------------------------------------------------------------------

// Main menu set-up: the original (SINGLE PLAYER, MULTIPLAYER, OPTIONS, QUIT on pool items 0-3), then CO-OP and MODS on
// items 4-5, MULTIPLAYER renamed VERSUS, and the pool reordered to SINGLE PLAYER, CO-OP, VERSUS, MODS, OPTIONS, QUIT.
// Optional features (start-up settings): FeatVersus off keeps the game's MULTIPLAYER (text, state 3), FeatMods off
// leaves MODS out (its item object stays unused at the end of the pool).
// The item objects are interchangeable, so the next set-up (every time the menu is entered) simply fills the
// reordered pool again.
bool g_versusItem = false;  // item 0x3FE opens VERSUS (state 0x42) instead of the game's MULTIPLAYER (state 3)
void* g_mainMenuSetupNext = reinterpret_cast<void*>(kMainMenuSetup);  // pointer hook chain (patch::HookSlot)

void __fastcall MainMenuSetup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(g_mainMenuSetupNext)(screen);
    void* list = At<void*>(screen, 8);
    void** items = At<void**>(list, 0x2C);
    void* coop = items[4];
    void* mods = items[5];
    const bool modsItem = features::ModsFolderEnabled();
    g_versusItem = features::VersusEnabled();
    SetItem(coop, kCoopItemId, kHashCoop);
    AddToList(list, coop);
    if (g_versusItem) SetItem(items[1], 0x3FE, features::VersusItemText());  // MULTIPLAYER -> VERSUS (versus.cpp)
    if (modsItem) {
        SetItem(mods, kModsItemId, features::ModsMenuItemText());  // MODS (modsmenu.cpp)
        AddToList(list, mods);
    }
    // Pool order = list order: SINGLE PLAYER, CO-OP, VERSUS, MODS, OPTIONS, QUIT (without MODS: it goes last, unlisted).
    void* const order[6] = {items[0], coop, items[1], modsItem ? mods : items[2], modsItem ? items[2] : items[3],
                            modsItem ? items[3] : mods};
    for (int i = 0; i < 6; ++i) items[i] = order[i];
    // The list area was sized for four rows (list +0x38 row pitch, +0x40 area height, +0x42 visible rows).
    const int rows = modsItem ? 6 : 5;
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (rows - 4) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = static_cast<int16_t>(rows);
    // The panel rectangle (screen +0x10..+0x1c) is laid out around the list by vtable +0x50, which the original
    // set-up already called - lay it out again with the same arguments.
    using Layout = void(__thiscall*)(void*, int, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, -1, 1, 0, 0x7FFF);
}

// Replaces "cmp si, 0x3fc; jle done" in the main menu's selection handler (si = activated item id).
__declspec(naked) void MenuSelectStub() {
    __asm {
        cmp si, kCoopItemId
        je coop
        cmp si, 0x3FE               // MULTIPLAYER -> VERSUS (state 0x42, versus.cpp) - unless FeatVersus is off
        jne notVersus
        cmp byte ptr g_versusItem, 0
        jne versus
    notVersus:
        cmp si, kModsItemId         // MODS (state 0x43, modsmenu.cpp)
        je mods
        cmp si, 0x3FC
        jle done
        push kMenuSelectCont
        ret
    mods:
        push 0x43
        mov eax, kSetState
        call eax
        add esp, 4
        push kMenuSelectDone
        ret
    versus:
        push 0x42
        mov eax, kSetState
        call eax
        add esp, 4
        push kMenuSelectDone
        ret
    coop:
        push kStateCoopMenu         // CO-OP screen (online.cpp): SPLIT SCREEN / ONLINE
        mov eax, kSetState
        call eax
        add esp, 4
    done:
        push kMenuSelectDone
        ret
    }
}

// --- state 0x3E: the join screen -------------------------------------------------------------------------------------

bool __fastcall CoopCreate(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    using ListCtor = void*(__thiscall*)(void*, void*, void*, int);
    using Reserve = int(__thiscall*)(void*, int);
    using Ctor = void*(__thiscall*)(void*);
    using AddPool = void(__thiscall*)(void*, void*);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<ListCtor>(kListCtor)(list, *reinterpret_cast<void**>(kMenuFont), *reinterpret_cast<void**>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<Reserve>(kListReserve)(list, kSlots)) return false;
    for (int i = 0; i < kSlots; ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x24, 0x1E);
        if (item) reinterpret_cast<Ctor>(kItemCtor)(item);
        reinterpret_cast<AddPool>(kListAddToPool)(list, item);
    }
    return true;
}

// Set up: the main menu's layout (its set-up adds the four pooled items), then our texts and title.
void __fastcall CoopSetup(void* screen, void*) {
    const bool keep = g_keepSlots;
    g_keepSlots = false;
    if (!keep) ResetSlots();
#ifndef DS_DIST
    DWORD fake = 0, size = sizeof fake;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "CoopFakeJoin", RRF_RT_REG_DWORD, nullptr, &fake,
                 &size);
    for (DWORD i = 0; i < fake && i < kSlots && !keep; ++i)
        g_slots[i] = Slot{kFakeDevice + static_cast<int>(i), i % 2 ? PadType::Xbox : PadType::PlayStation, Icons::Auto,
                          i % 2 ? "XBOX CONTROLLER" : "DUALSENSE", -1, static_cast<int>(i % 2),
                          static_cast<int>(i % kClasses)};
#endif
    reinterpret_cast<void(__thiscall*)(void*)>(kMainMenuSetup)(screen);
    void* list = At<void*>(screen, 8);
    for (int i = 0; i < kSlots; ++i) SetItem(ItemAt(list, i), static_cast<uint16_t>(kSlotItemId + i), kHashSlot0 + i);
    using SetTitle = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<SetTitle>((*reinterpret_cast<void***>(screen))[20])(screen, kHashTitle, 0, 0, 0x7FFF);
}

// Icon size: a little under the list's row height (list +0x34, pixels).
int IconSize() {
    if (!g_screen) return 48;
    void* list = At<void*>(g_screen, 8);
    return std::max(16, At<int16_t>(list, 0x34) * 7 / 10);
}
int DrawPrompt(int x, int y, overlay::Icon a, overlay::Icon b, overlay::Icon c, const char* action);

// Title + the players' panels (PanelsLateDraw) and the prompt line; the game's menu panel and list rows aren't drawn.
void __fastcall CoopDraw(void*, void*) {
    // The players' panels (Xbox-style, PanelsLateDraw) instead of the list's rows; the list stays for its input.
    g_panelsDue = true;
    g_panelsAlpha = overlay::MenuAlpha();

    // Prompt line (where the game draws "Return: Select" / "Esc: Back").
    auto* r = *reinterpret_cast<uint8_t**>(0x63C924);
    const int w = *reinterpret_cast<int*>(r + 0x40688), h = *reinterpret_cast<int*>(r + 0x4068C);
    const int y = h * 91 / 100;
    using overlay::Icon;
    if (JoinedCount() >= 2) DrawPrompt(w * 46 / 1000, y, Icon::PsStart, Icon::XbStart, Icon::KbEnter, "Begin");
    else DrawPrompt(w * 46 / 1000, y, Icon::PsCross, Icon::XbCross, Icon::KbSpace, "Join");
    if (JoinedCount() > 0 && g_versus) {
        if (features::VersusTeams()) {
            DrawPrompt(w * 23 / 100, y, Icon::PsR1, Icon::XbR1, Icon::KbTab, "Team / Class");
            DrawPrompt(w * 47 / 100, y, Icon::PsDpadHorizontal, Icon::XbDpadHorizontal, Icon::KbArrowsHorizontal, "Change");
        } else {
            DrawPrompt(w * 35 / 100, y, Icon::PsDpadHorizontal, Icon::XbDpadHorizontal, Icon::KbArrowsHorizontal, "Class");
        }
    } else if (JoinedCount() > 0) {
        DrawPrompt(w * 30 / 100, y, Icon::PsDpadHorizontal, Icon::XbDpadHorizontal, Icon::KbArrowsHorizontal, "Icons");
    }
    DrawPrompt(w * 66 / 100, y, Icon::PsCircle, Icon::XbCircle, Icon::KbEscape, "Back / Leave");
}

// --- the players' panels ----------------------------------------------------------------------------------------------
// Like the Xbox's JOIN CO-OPERATIVE GAME screen (default.xbe 0x70850: one panel per controller port, "Press A to Join",
// the control layout, "Waiting for other players." / "Press START when all players are ready"), for four players in
// a 2 x 2 grid, each in the player's colour (the DualSense light bar's: blue, red, green, pink). Drawn in a late draw
// (shapes first, then the game-font text over them), only on frames the screen itself drew.
constexpr uint32_t kPlayerColour[kSlots] = {0x2E5BD8, 0xD03232, 0x2EA844, 0xD85AB8};

const char* IconStyleName(Icons icons) {
    switch (icons) {
        case Icons::PlayStation: return "PLAYSTATION BUTTONS";
        case Icons::Xbox: return "XBOX BUTTONS";
        case Icons::Keyboard: return "KEYBOARD KEYS";
        default: return "BUTTONS: AUTO";
    }
}

uint32_t WithAlpha(uint32_t rgb, float alpha) {
    const int a = std::max(0, std::min(255, static_cast<int>(alpha * g_panelsAlpha * 255.0f)));
    return (static_cast<uint32_t>(a) << 24) | (rgb & 0xFFFFFF);
}

bool NeedsMouse(const Slot& s) {
    if (s.device < kRawKeyboard || !IsKeyboard(s.device) || s.mouse >= 0) return false;
    int keyboardPlayers = 0;
    for (const Slot& t : g_slots) keyboardPlayers += t.device >= kRawKeyboard && IsKeyboard(t.device);
    return keyboardPlayers >= 2;
}

void PanelsLateDraw() {
    if (!g_panelsDue) return;
    g_panelsDue = false;
    if (*reinterpret_cast<uint32_t*>(kFrontEndState) != kStateCoop || g_panelsAlpha <= 0.01f) return;
    auto* r = *reinterpret_cast<uint8_t**>(0x63C924);
    const float w = static_cast<float>(*reinterpret_cast<int*>(r + 0x40688));
    const float h = static_cast<float>(*reinterpret_cast<int*>(r + 0x4068C));
    void* small = overlay::MenuFont();
    void* large = *reinterpret_cast<void**>(0x60EDB8);
    const float gap = w * 0.012f, x0 = w * 0.08f, x1 = w * 0.92f, y0 = h * 0.17f, y1 = h * 0.83f;
    const float pw = (x1 - x0 - gap) / 2, ph = (y1 - y0 - gap) / 2;
    const float line = static_cast<float>(std::max(8, overlay::TextWidth(small, "M")));
    const float border = std::max(2.0f, h / 360.0f), band = ph * 0.2f;
    const float icon = std::min(ph * 0.2f, pw * 0.16f);
    struct Text { void* font; std::string text; float cx, y; uint32_t argb; };
    std::vector<Text> texts;
    struct IconAt { overlay::Icon icon; float x, y, size; };
    std::vector<IconAt> icons;
    auto centred = [&](void* font, const std::string& t, float cx, float y, uint32_t argb) {
        texts.push_back({font, t, cx, y, argb});
    };
    centred(large, features::CoopText(kHashTitle), w / 2, h * 0.11f, WithAlpha(0xFFFFFF, 1.0f));
    int keyboardPlayers = 0;   // several keyboards: named KEYBOARD 1, 2 ... in join order
    for (const Slot& t : g_slots) keyboardPlayers += t.device >= kRawKeyboard && IsKeyboard(t.device);
    int keyboardNumber = 0;
    for (int i = 0; i < kSlots; ++i) {
        const Slot& s = g_slots[i];
        const bool joined = s.device >= 0;
        const float px = x0 + (i % 2) * (pw + gap), py = y0 + (i / 2) * (ph + gap), cx = px + pw / 2;
        const uint32_t colour = kPlayerColour[i];
        overlay::QueueScreenRect(px, py, pw, ph, WithAlpha(0x0C120C, joined ? 0.82f : 0.6f));
        overlay::QueueScreenRect(px, py, pw, band, WithAlpha(joined ? colour : 0x404040, joined ? 0.9f : 0.55f));
        const uint32_t edge = WithAlpha(joined ? colour : 0xFFFFFF, joined ? 1.0f : 0.25f);
        overlay::QueueScreenRect(px, py, pw, border, edge);
        overlay::QueueScreenRect(px, py + ph - border, pw, border, edge);
        overlay::QueueScreenRect(px, py, border, ph, edge);
        overlay::QueueScreenRect(px + pw - border, py, border, ph, edge);
        char header[32];
        snprintf(header, sizeof header, "PLAYER %d", i + 1);
        centred(large, header, cx, py + band / 2 + line / 2, WithAlpha(0xFFFFFF, joined ? 1.0f : 0.6f));
        if (!joined) {   // "PRESS [Cross] [A] [Space] TO JOIN", the icons between the words
            const std::string a = "PRESS", b = "TO JOIN";
            const float sz = icon * 0.75f, spacing = line;
            const float wa = static_cast<float>(overlay::TextWidth(small, a.c_str()));
            const float wb = static_cast<float>(overlay::TextWidth(small, b.c_str()));
            float x = cx - (wa + 3 * sz + wb + 2 * spacing) / 2;
            const float y = py + ph * 0.6f;
            centred(small, a, x + wa / 2, y + line / 2, WithAlpha(0xD8D8D8, 1.0f));
            x += wa + spacing;
            for (overlay::Icon k : {overlay::Icon::PsCross, overlay::Icon::XbCross, overlay::Icon::KbSpace}) {
                icons.push_back({k, x, y - sz / 2, sz});
                x += sz;
            }
            x += spacing;
            centred(small, b, x + wb / 2, y + line / 2, WithAlpha(0xD8D8D8, 1.0f));
            continue;
        }
        std::string name = s.name;
        if (s.device >= kRawKeyboard && IsKeyboard(s.device)) {
            ++keyboardNumber;
            name = keyboardPlayers >= 2 ? "KEYBOARD " + std::to_string(keyboardNumber) : "KEYBOARD & MOUSE";
        }
        // name, device picture, option, status: spread evenly over the panel below its band (text y = baseline;
        // the font's cap height is about one `line`), so small resolutions fit as well as large ones
        const float spare = std::max(0.0f, ph - band - (3 * line + icon)), step = spare / 5;
        const float nameY = py + band + step + line;
        centred(small, name, cx, nameY, WithAlpha(0xFFFFFF, 1.0f));
        // the device: the pad's confirm button in the chosen style, or keyboard + mouse
        const float iy = nameY + step;
        if (IsKeyboard(s.device)) {
            icons.push_back({overlay::Icon::Keyboard, cx - icon, iy, icon});
            icons.push_back({overlay::Icon::Mouse, cx, iy, icon});
        } else {
            icons.push_back({ConfirmIcon(s), cx - icon / 2, iy, icon});
        }
        std::string option;
        char t[64];
        if (g_versus) {
            if (!features::VersusTeams()) snprintf(t, sizeof t, "< %s >", kClassNames[s.cls]);
            else if (s.field == 0) snprintf(t, sizeof t, "< TEAM %d >  %s", s.team + 1, kClassNames[s.cls]);
            else snprintf(t, sizeof t, "TEAM %d  < %s >", s.team + 1, kClassNames[s.cls]);
            option = t;
        } else if (IsKeyboard(s.device)) {
            if (s.keyProfile >= 0) snprintf(t, sizeof t, "< KEY PROFILE %d >", s.keyProfile + 1);
            else snprintf(t, sizeof t, "< CURRENT KEYS >");
            option = t;
        } else {
            option = std::string("< ") + IconStyleName(s.icons) + " >";
        }
        const float oy = iy + icon + step + line;
        centred(small, option, cx, oy, WithAlpha(0xE8E8E8, 1.0f));
        if (NeedsMouse(s)) centred(small, "CLICK YOUR MOUSE", cx, oy + step + line, WithAlpha(0xFFD040, 1.0f));
        else centred(small, "READY", cx, oy + step + line, WithAlpha(colour | 0x404040, 1.0f));
    }
    const char* status = JoinedCount() >= 2 ? "PRESS START WHEN ALL PLAYERS ARE READY" : "WAITING FOR OTHER PLAYERS";
    centred(small, status, w / 2, y1 + (h * 0.895f - y1) / 2 + line / 2, WithAlpha(0xFFFFFF, 0.85f));
    overlay::Flush();   // the shapes, then the text over them
    for (const Text& t : texts)
        overlay::DrawLabel(t.font, t.text.c_str(),
                           static_cast<int>(t.cx - overlay::TextWidth(t.font, t.text.c_str()) / 2.0f),
                           static_cast<int>(t.y), t.argb);
    for (const IconAt& k : icons) overlay::QueueIcon(k.icon, k.x, k.y, k.size, g_panelsAlpha);
}

// Prompt getters (vtable +0x34 select, +0x44 back): none - CoopDraw draws ours with the real buttons.
uint32_t __fastcall NoPrompt(void*, void*) { return 0; }

// One prompt: the three devices' buttons, then the action, from x at baseline y. Returns the end x.
int DrawPrompt(int x, int y, overlay::Icon a, overlay::Icon b, overlay::Icon c, const char* action) {
    void* font = overlay::MenuFont();
    const int h = std::max(12, IconSize() * 3 / 4);
    const int top = y - overlay::TextWidth(font, "M") / 2 - h / 2;  // centred on the text (cap height ~ M width)
    for (overlay::Icon icon : {a, b, c}) {
        overlay::QueueIcon(icon, static_cast<float>(x), static_cast<float>(top), static_cast<float>(h));
        x += h;
    }
    x += h / 4;
    overlay::DrawLabel(font, action, x, y, 0xFFFFFFFF);
    return x + overlay::TextWidth(font, action);
}

int __fastcall CoopUpdate(void* screen, void*) {
    reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(At<void*>(screen, 8));  // items only show players
    if (g_versus && features::VersusTakeStart()) return 3;  // versus Begin: load the match level
    return 0;
}

void* __cdecl ExtraScreen(uint32_t state) {
    if (state == 0x42) return features::VersusScreen();
    if (state == 0x44) return features::VersusSetupScreen();
    if (state == 0x43) return features::ModsMenuScreen();
    if (state == 0x45) return features::CampaignsScreen();
    if (state == 0x47) return features::FixMenuScreen();    // fixmenu.cpp (OPTIONS > DESERTSTORMFIX)
    if (state == 0x4C) return features::VideoMenuScreen();  // videomenu.cpp (OPTIONS > VIDEO)
    if (state == 0x4D || state == 0x4E) return features::PluginsMenuScreen(state);  // pluginsmenu.cpp (> PLUGINS)
    if (void* s = features::OnlineScreen(state)) return s;  // online.cpp (CO-OP > SPLIT SCREEN / ONLINE)
    if (void* s = features::ControlsScreen(state)) return s;
    return features::ServiceScreen(state);  // plugins' screens (dsfix.developer: CHEATS > DEVELOPER, 0x46)
}

// Front-end state -> screen: ours for kStateCoop, the CONTROLS screens' for 0x3F.., else the original (entry hook:
// the broker's trampoline runs its first instructions, then the rest).
void* g_screenForStateNext = nullptr;

__declspec(naked) void ScreenForStateStub() {
    __asm {
        mov eax, [esp + 4]
        cmp eax, kStateCoop
        jne other
        mov eax, g_screen
        ret
    other:
        cmp eax, kStateCoop
        jb original
        push eax
        call ExtraScreen
        add esp, 4
        test eax, eax
        jz none
        ret
    none:
    original:
        jmp dword ptr [g_screenForStateNext]
    }
}

bool BuildScreen() {
    static uint8_t storage[0x20];
    g_screen = storage;
    reinterpret_cast<void(__thiscall*)(void*)>(kMainMenuCtor)(g_screen);
    std::memcpy(g_vtable, reinterpret_cast<void*>(kMainMenuVtable), sizeof g_vtable);
    const auto mp = reinterpret_cast<void* const*>(kMpVtable);
    g_vtable[1] = reinterpret_cast<void*>(&CoopCreate);
    g_vtable[3] = reinterpret_cast<void*>(&CoopSetup);
    g_vtable[4] = reinterpret_cast<void*>(&CoopUpdate);
    g_vtable[5] = reinterpret_cast<void*>(&CoopDraw);
    g_vtable[13] = reinterpret_cast<void*>(&NoPrompt);
    g_vtable[17] = reinterpret_cast<void*>(&NoPrompt);
    g_vtable[9] = mp[9];    // back handling
    At<void**>(g_screen, 0) = g_vtable;
    overlay::AddLateDraw(&PanelsLateDraw);
    return CoopCreate(g_screen, nullptr);
}

bool Install() {
    static const uint8_t capacity[] = {0x6A, 0x04};
    static const uint8_t count[] = {0xBF, 0x04, 0x00, 0x00, 0x00};
    const uint32_t setup = kMainMenuSetup;
    static const uint8_t select[] = {0x66, 0x81, 0xFE, 0xFC, 0x03, 0x7E, 0x62};
    static const uint8_t lookup[] = {0x8B, 0x44, 0x24, 0x04, 0x48};
    if (!patch::Matches(kMenuCapacity, capacity, sizeof capacity) || !patch::Matches(kMenuItemCount, count, sizeof count) ||
        !patch::Matches(kMainMenuSetupSlot, &setup, 4) || !patch::Matches(kMenuSelect, select, sizeof select) ||
        !patch::Matches(kScreenForState, lookup, sizeof lookup)) {
        dslog::Write("[fail] Co-op menu: unexpected bytes - different exe version? Not applied.");
        return false;
    }
    const uint8_t capacity5[] = {0x6A, 0x06};  // 6 items: + CO-OP, MODS
    const uint8_t count5[] = {0xBF, 0x06, 0x00, 0x00, 0x00};
    return patch::Write(kMenuCapacity, capacity5, sizeof capacity5) && patch::Write(kMenuItemCount, count5, sizeof count5) &&
           patch::HookSlot(kMainMenuSetupSlot, reinterpret_cast<const void*>(&MainMenuSetup), &g_mainMenuSetupNext,
                           kMainMenuSetup) &&
           patch::WriteJump(kMenuSelect, reinterpret_cast<const void*>(&MenuSelectStub), sizeof select, select) &&
           patch::HookFunction(kScreenForState, reinterpret_cast<const void*>(&ScreenForStateStub),
                               &g_screenForStateNext, lookup, sizeof lookup);
}
}  // namespace

namespace {
// --- co-op mission list (state 8) ---------------------------------------------------------------------------------

bool CoopMissionList() {
    return g_session.players && *reinterpret_cast<uint32_t*>(kFrontEndState) == kStateMissionList;
}

// Difficulty picked: what SELECT UNIFORM's accept did, then the level (or the co-op mission list).
constexpr uint32_t kDifficultyUpdateSlot = 0x5D85F8, kDifficultyUpdate = 0x413670, kDifficultyNextCall = 0x4136CD;
constexpr uint32_t kSetUniform = 0x40A320, kNewCampaign1 = 0x4576C0, kNewCampaign2 = 0x457750;
constexpr uint32_t kCampaignStart = 0x4475B0, kNextLevelSlot = 0x5E7028;
bool g_difficultyPicked = false;

constexpr uint32_t kStateUniform = 6;

// FeatUniformSelect (decided here, so a change in the menu counts at once): the game's own flow - SELECT UNIFORM
// (state 6), whose accept does all this itself (UniformUpdate below sends a co-op session on to the mission list).
bool UniformSelect() { return settings::Get().featUniformSelect; }

void __cdecl DifficultyPicked(uint32_t state) {
    if (UniformSelect()) {
        SetState(state);
        return;
    }
    reinterpret_cast<void(__cdecl*)(int)>(kSetUniform)(0);
    const uint32_t players = *reinterpret_cast<uint32_t*>(0x606414);
    const bool own = !features::CampaignLevelPicked();   // the game's campaign (custom / converted: their own missions)
    if (own && *reinterpret_cast<uint32_t*>(0x606410) && !*reinterpret_cast<uint32_t*>(0x606424) && players >= 3 && players <= 4)
        strcpy_s(*reinterpret_cast<char**>(kNextLevelSlot), 13, "mission3.dll");   // Connors and Jones join in Mission 3
    void* campaign = *reinterpret_cast<void**>(kCampaign);
    reinterpret_cast<void(__thiscall*)(void*)>(kNewCampaign1)(campaign);
    reinterpret_cast<void(__thiscall*)(void*)>(kNewCampaign2)(campaign);
    reinterpret_cast<void(__cdecl*)(int)>(kCampaignStart)(1);
    if (g_session.players && own) SetState(kStateMissionList);   // co-op: pick the mission first
    else g_difficultyPicked = true;
}

using UpdateFn = int(__thiscall*)(void*);

void* g_difficultyUpdateNext = reinterpret_cast<void*>(kDifficultyUpdate);  // pointer hook chain

int __fastcall DifficultyUpdate(void* screen, void*) {
    g_difficultyPicked = false;
    const int r = reinterpret_cast<UpdateFn>(g_difficultyUpdateNext)(screen);
    return g_difficultyPicked ? 3 : r;
}

// Uniform screen: in a session its "start the level" (3) opens the mission list instead (reached with FeatUniformSelect).
UpdateFn g_uniformUpdate = nullptr, g_missionListUpdate = nullptr;

int __fastcall UniformUpdate(void* screen, void*) {
    const int r = g_uniformUpdate(screen);
    if (r == 3 && g_session.players && !features::CampaignLevelPicked()) {
        SetState(kStateMissionList);
        return 0;
    }
    return r;
}

// Mission list: Back goes to the screen before it in a session - SELECT UNIFORM when it is shown, else DIFFICULTY (the
// game's own target is the CHEATS menu, state 7).
int __fastcall MissionListUpdate(void* screen, void*) {
    // First frame of the co-op list: highlight the first mission the group can play (list +0x28 = current item
    // index; the highlighted id +0x30 follows from it).
    if (g_session.players && !g_preselected) {
        g_preselected = true;
        // The item width (+0x32, highlight ring +0x36 = width + margin) was made for the plain titles; ours add
        // "- 2 PLAYERS MAX". A fifth wider still ends before the scroll bar (the ring is narrow by design).
        void* list = At<void*>(screen, 8);
        const int16_t extra = static_cast<int16_t>(At<int16_t>(list, 0x32) / 5);
        At<int16_t>(list, 0x32) = static_cast<int16_t>(At<int16_t>(list, 0x32) + extra);
        At<int16_t>(list, 0x36) = static_cast<int16_t>(At<int16_t>(list, 0x36) + extra);
        for (int m = 0; m < kMissions; ++m)
            if (StateOf(m, g_session.players) == MissionState::Open) {
                At<int16_t>(At<void*>(screen, 8), 0x28) = static_cast<int16_t>(m);
                break;
            }
    }
    const int r = g_missionListUpdate(screen);
    if (g_session.players && *reinterpret_cast<uint32_t*>(kFrontEndState) == 7)
        SetState(UniformSelect() ? kStateUniform : kStateDifficulty);
    return r;
}

// id -> list index for the pick: 12 (= none) for missions the group cannot play.
int __fastcall MissionIndex(void* list, void*, int id) {
    const int index = reinterpret_cast<int(__thiscall*)(void*, int)>(kItemIndex)(list, id);
    if (g_session.players && index >= 0 && index < kMissions && StateOf(index, g_session.players) != MissionState::Open)
        return kMissions;
    return index;
}

// In front of the mission item draw callback (arg 1 = item id): grey text for missions the group cannot play.
uint32_t g_listColours[2];
bool g_coloursSaved = false;

void __cdecl BeforeMissionDraw(int id) {
    void* screen = *reinterpret_cast<void**>(kMissionListScreen);
    if (!screen) return;
    void* list = At<void*>(screen, 8);
    if (!g_coloursSaved) {
        g_listColours[0] = At<uint32_t>(list, 0x54);
        g_listColours[1] = At<uint32_t>(list, 0x58);
        g_coloursSaved = true;
    }
    const int mission = static_cast<int16_t>(id) - kMissionItemId;
    const bool grey = g_session.players && mission >= 0 && mission < kMissions &&
                      StateOf(mission, g_session.players) != MissionState::Open;
    At<uint32_t>(list, 0x54) = grey ? 0xFF7A7A7A : g_listColours[0];
    At<uint32_t>(list, 0x58) = grey ? 0xFF9A9A9A : g_listColours[1];
}

void* g_missionDrawNext = reinterpret_cast<void*>(kMissionDrawCb);  // pointer hook chain on the push's imm32

__declspec(naked) void MissionDrawStub() {
    __asm {
        push dword ptr [esp + 4]
        call BeforeMissionDraw
        add esp, 4
        jmp dword ptr [g_missionDrawNext]
    }
}

// Title texts of the co-op mission list (MISSn_TITLE hashes while it is open).
const char* MissionTitle(uint32_t hash) {
    static char text[kMissions][96];
    for (int m = 0; m < kMissions; ++m) {
        if (reinterpret_cast<const uint32_t*>(kMissionTable)[m * 3 + 2] != hash) continue;
        const char* title = features::GameText(hash);
        switch (StateOf(m, g_session.players)) {
            case MissionState::Locked: snprintf(text[m], sizeof text[m], "MISSION %d:  ? ? ? ? ?", m + 1); break;
            case MissionState::TooManyPlayers:
                snprintf(text[m], sizeof text[m], "MISSION %d: %s  -  2 PLAYERS MAX", m + 1, title ? title : "");
                break;
            default: snprintf(text[m], sizeof text[m], "MISSION %d: %s", m + 1, title ? title : ""); break;
        }
        return text[m];
    }
    return nullptr;
}

// --- co-op saves ---------------------------------------------------------------------------------------------------
// Saves are "<mission title> <n>.sav" in Documents\SCi\DesertStorm, named by FUN_004590b0 (sprintf at 0x459105,
// "%s %d.sav" / "%s%s.sav"; then " <n>" appended until the name is free). A save made with 2-4 players is named
// "Co-op <n>P <name>.sav". The save / load lists (enumerator FUN_004bf130 over "*.sav" with the callback 0x477020,
// pushed at 0x449A8F load list and 0x4751FE save list) show only the saves of the current mode: in a co-op session
// the co-op saves of that player count, otherwise the single-player ones.
// Co-op menu flow (Xbox-like): Begin -> the single-player menu (state 2) titled CO-OPERATIVE CAMPAIGN without TRAINING
// (DESERT STORM CAMPAIGN -> difficulty ..., LOAD GAME -> state 9); its Back (-> main menu) is turned into the join
// screen by OnFrameCoop.
constexpr uint32_t kSaveNamePrintf = 0x459105, kSprintf = 0x566E80;
constexpr uint32_t kSaveListCallback = 0x477020, kLoadListPush = 0x449A8F, kSaveListPush = 0x4751FE;
constexpr uint32_t kSpMenuSetupSlot = 0x5D93DC, kSpMenuSetup = 0x48CCB0;
constexpr uint32_t kSpMenuTitle = 0xA08F3D1B, kStateSinglePlayer = 2;

int CoopSavePlayers() {
    if (g_session.players >= 2) return g_session.players;
    const int split = features::SplitScreenPlayers();
    return split >= 2 ? split : 0;
}

// Players of a save by its file name ("...\Co-op 2P Rescue 1.sav" -> 2), 0 = single player.
int SavePlayers(const char* path) {
    const char* name = path;
    for (const char* c = path; *c; ++c)
        if (*c == '\\' || *c == '/') name = c + 1;
    if (_strnicmp(name, "Co-op ", 6) != 0) return 0;
    const int n = name[6] - '0';
    return n >= 2 && n <= 4 && (name[7] == 'P' || name[7] == 'p') ? n : 0;
}

int __cdecl SaveName(char* out, const char* format, uintptr_t a, uintptr_t b) {
    char name[240];
    const int length = snprintf(name, sizeof name, format, a, b);
    const int players = CoopSavePlayers();
    const char* cheats = features::CheatsActive() ? "Cheats " : "";  // cheats.cpp: the save switches them back on
    if (players) return sprintf(out, "Co-op %dP %s%s", players, cheats, name);
    if (*cheats) return sprintf(out, "%s%s", cheats, name);
    strcpy(out, name);
    return length;
}

int __cdecl SaveFilter(const char* path, void* user) {
    if (!path || SavePlayers(path) != CoopSavePlayers()) return 0;
    return reinterpret_cast<int(__cdecl*)(const char*, void*)>(kSaveListCallback)(path, user);
}

void* g_spMenuSetupNext = reinterpret_cast<void*>(kSpMenuSetup);  // pointer hook chain

void __fastcall SpMenuSetup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(g_spMenuSetupNext)(screen);
    if (!g_session.players) {
        features::CampaignsMenuItem(screen, false);  // CAMPAIGNS / CUSTOM CAMPAIGNS
        return;
    }
    void* list = At<void*>(screen, 8);
    void** items = At<void**>(list, 0x2C);
    if (At<int16_t>(list, 0x26) != 3) return;
    void* training = items[0];  // the list shows its pool in order: TRAINING goes last and out of the count
    items[0] = items[1], items[1] = items[2], items[2] = training;
    At<int16_t>(list, 0x26) = 2;
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) - At<int16_t>(list, 0x38));
    if (At<int16_t>(list, 0x42) > 2) At<int16_t>(list, 0x42) = 2;
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kSpMenuTitle, 0, 0, 0x7FFF);
    reinterpret_cast<void(__thiscall*)(void*)>(0x4E8130)(list);
    features::CampaignsMenuItem(screen, true);   // split screen + online: CAMPAIGNS / CUSTOM CAMPAIGNS too
}

bool InstallSaves() {
    const uint32_t callback = kSaveListCallback, setup = kSpMenuSetup;
    if (!patch::Matches(kLoadListPush + 1, &callback, 4) || !patch::Matches(kSaveListPush + 1, &callback, 4) ||
        !patch::Matches(kSpMenuSetupSlot, &setup, 4)) {
        dslog::Write("[fail] Co-op saves: unexpected bytes - not applied");
        return false;
    }
    // The two pushes of the list callback: pointer hooks (SaveFilter calls the game's callback itself - it serves both).
    const void* filter = reinterpret_cast<const void*>(&SaveFilter);
    return patch::HookCall(kSaveNamePrintf, reinterpret_cast<const void*>(&SaveName), kSprintf) &&
           patch::HookSlot(kLoadListPush + 1, filter, nullptr, kSaveListCallback) &&
           patch::HookSlot(kSaveListPush + 1, filter, nullptr, kSaveListCallback) &&
           patch::HookSlot(kSpMenuSetupSlot, reinterpret_cast<const void*>(&SpMenuSetup), &g_spMenuSetupNext, kSpMenuSetup);
}

bool InstallMissionList() {
    const uint32_t uniform = 0x49BAF0, missionList = 0x4595D0;
    const uint8_t push[] = {0x68, 0xB0, 0x96, 0x45, 0x00};
    if (!patch::Matches(kUniformUpdateSlot, &uniform, 4) || !patch::Matches(kMissionListUpdateSlot, &missionList, 4) ||
        !patch::Matches(kMissionDrawCbPush, push, sizeof push)) {
        dslog::Write("[fail] Co-op mission list: unexpected bytes - not applied");
        return false;
    }
    const uint32_t difficulty = kDifficultyUpdate;
    if (!patch::Matches(kDifficultyUpdateSlot, &difficulty, 4) ||
        !patch::HookCall(kDifficultyNextCall, reinterpret_cast<const void*>(&DifficultyPicked), kSetState) ||
        !patch::HookSlot(kDifficultyUpdateSlot, reinterpret_cast<const void*>(&DifficultyUpdate), &g_difficultyUpdateNext,
                         kDifficultyUpdate))
        dslog::Write("[fail] Co-op: difficulty -> level (no uniform screen) not applied");
    g_uniformUpdate = reinterpret_cast<UpdateFn>(uniform);  // set by HookSlot too (the chain's next)
    g_missionListUpdate = reinterpret_cast<UpdateFn>(missionList);
    return patch::HookSlot(kUniformUpdateSlot, reinterpret_cast<const void*>(&UniformUpdate),
                           reinterpret_cast<void**>(&g_uniformUpdate), uniform) &&
           patch::HookSlot(kMissionListUpdateSlot, reinterpret_cast<const void*>(&MissionListUpdate),
                           reinterpret_cast<void**>(&g_missionListUpdate), missionList) &&
           patch::HookSlot(kMissionDrawCbPush + 1, reinterpret_cast<const void*>(&MissionDrawStub), &g_missionDrawNext,
                           kMissionDrawCb) &&
           patch::HookCall(kMissionIndexCall, reinterpret_cast<const void*>(&MissionIndex), kItemIndex);
}
}  // namespace

// Texts of our own string hashes (the game's lookup is hooked in ingame_input.cpp); nullptr = not ours.
const char* features::CoopText(uint32_t hash) {
    static char slots[kSlots][96];
    switch (hash) {
        case kHashCoop: return "CO-OP";
        case kHashTitle: return g_versus ? "JOIN VERSUS GAME" : "JOIN CO-OPERATIVE GAME";
        default: break;
    }
    if (hash == 0xA08F3D1B && g_session.players && *reinterpret_cast<uint32_t*>(kFrontEndState) == 2)
        return "CO-OPERATIVE CAMPAIGN";
    if (CoopMissionList())
        if (const char* title = MissionTitle(hash)) return title;
    if (hash >= kHashSlot0 && hash < kHashSlot0 + kSlots) {
        const int i = static_cast<int>(hash - kHashSlot0);
        const Slot& s = g_slots[i];
        const int icon = IconSize();
        RowIcons& row = g_rowIcons[i];
        char prefix[96];
        if (s.device < 0) {
            snprintf(prefix, sizeof prefix, "PLAYER %d:  PRESS ", i + 1);
            row = RowIcons{prefix, {overlay::Icon::PsCross, overlay::Icon::XbCross, overlay::Icon::KbSpace}, 3};
            snprintf(slots[i], sizeof slots[i], "%s%s TO JOIN", prefix, Gap(icon * 3 + icon / 2).c_str());
        } else {
            if (g_versus) {  // versus: the device's icon, then the team (left / right)
                snprintf(prefix, sizeof prefix, "P%d:  %s  ", i + 1, s.name.c_str());
                row = RowIcons{prefix, {IsKeyboard(s.device) ? overlay::Icon::KbEnter : ConfirmIcon(s)}, 1};
                // the field left / right changes has the arrows; deathmatch has no teams
                if (!features::VersusTeams())
                    snprintf(slots[i], sizeof slots[i], "%s%s   < %s >", prefix, Gap(icon + icon / 4).c_str(),
                             kClassNames[s.cls]);
                else if (s.field == 0)
                    snprintf(slots[i], sizeof slots[i], "%s%s   < TEAM %d >   %s", prefix, Gap(icon + icon / 4).c_str(),
                             s.team + 1, kClassNames[s.cls]);
                else
                    snprintf(slots[i], sizeof slots[i], "%s%s   TEAM %d   < %s >", prefix, Gap(icon + icon / 4).c_str(),
                             s.team + 1, kClassNames[s.cls]);
                return slots[i];
            }
            if (IsKeyboard(s.device)) {  // its icon; left / right picks a saved key profile (shown only when chosen)
                // several keyboard players: each still needs a mouse - asked for until one is clicked
                int keyboardPlayers = 0;
                for (const Slot& t : g_slots) keyboardPlayers += t.device >= kRawKeyboard && IsKeyboard(t.device);
                if (keyboardPlayers >= 2 && s.device >= kRawKeyboard && s.mouse < 0) {
                    snprintf(slots[i], sizeof slots[i], "PLAYER %d:  %s - CLICK YOUR MOUSE", i + 1, s.name.c_str());
                    row = RowIcons{slots[i], {}, 0};
                    return slots[i];
                }
                snprintf(prefix, sizeof prefix, "PLAYER %d:  %s    < ", i + 1, s.name.c_str());
                row = RowIcons{prefix, {overlay::Icon::KbEnter}, 1};
                if (s.keyProfile >= 0)
                    snprintf(slots[i], sizeof slots[i], "%s%s  PROFILE %d >", prefix, Gap(icon + icon / 4).c_str(), s.keyProfile + 1);
                else
                    snprintf(slots[i], sizeof slots[i], "%s%s >", prefix, Gap(icon + icon / 4).c_str());
                return slots[i];
            }
            // The icon shows the chosen style; left / right changes it.
            snprintf(prefix, sizeof prefix, "PLAYER %d:  %s    < ", i + 1, s.name.c_str());
            row = RowIcons{prefix, {ConfirmIcon(s)}, 1};
            snprintf(slots[i], sizeof slots[i], "%s%s >", prefix, Gap(icon + icon / 4).c_str());
        }
        return slots[i];
    }
    return nullptr;
}

// Installing only patches code; the screen object is built by OnFrameCoop once the game's own front-end screens
// exist (the list code needs the menu font and HUD sheet).
void features::ApplyCoop() {
    if (g_installed) return;
    g_installed = Install() && InstallMissionList() && InstallSaves();
    if (g_installed) dslog::Write("[ok]   Co-op menu: CO-OP entry + join screen (state 0x%X)", kStateCoop);
    dslog::Write(UniformSelect() ? "[ok]   Uniform select: DIFFICULTY -> SELECT UNIFORM (live setting)"
                                 : "[off]  Uniform select: DIFFICULTY starts the level as SAS (live setting)");
}

void features::OnFrameCoop() {
    if (!g_installed) return;
    if (!g_screen && *reinterpret_cast<void**>(kMainMenuScreen)) {
        if (BuildScreen()) dslog::Write("Co-op: join screen created");
        else dslog::Write("[fail] Co-op: join screen could not be created");
    }
    const uint32_t state = *reinterpret_cast<uint32_t*>(kFrontEndState);
    static uint32_t lastPolled = 0;
    if (state == kStateCoop && lastPolled != kStateCoop) {  // entering: what is held already (the Esc that led here) isn't a press
        for (uint32_t& held : g_prevActions) held = ~0u;
        // Raw Input keyboards are only known once they send something - the first key of one seen later is a press
        for (int dev = 0; dev < kRawDevices; ++dev) g_prevActions[kRawKeyboard + dev] = RawKeyboardActions(dev);
    }
    lastPolled = state;
    if (state == kStateMainMenu) g_versus = false;  // CO-OP from the main menu = the co-op join screen
    if (g_screen && state == kStateCoop) PollDevices();
    if (state != kStateMissionList) g_preselected = false;
    TrackProgress();
    // Co-op menu Back: to the join screen - once the state setter FUN_0047ef20 takes calls again ([0x617C20] = 0;
    // it ignores them while busy).
    static uint32_t previous = 0;
    static bool backToJoin = false;
    if (g_session.players && state == kStateMainMenu) {
        EndSession();
        backToJoin = previous == kStateSinglePlayer;
    }
    if (backToJoin && state != kStateMainMenu) backToJoin = false;
    if (backToJoin && *reinterpret_cast<uint32_t*>(0x617C20) == 0) {
        backToJoin = false;
        g_keepSlots = true;
        SetState(kStateCoop);
    }
    previous = state;
}

int features::CoopPlayers() { return g_session.players; }
int features::CoopClass(int player) {
    return g_versus && player >= 0 && player < g_session.players ? g_session.cls[player] : -1;
}
int features::CoopTeam(int player) {
    return g_versus && player >= 0 && player < g_session.players ? g_session.team[player] : -1;
}

// VERSUS > SPLIT SCREEN: the join screen for a versus match (keepPlayers: back from the match set-up, still joined).
void features::OpenVersusJoin(bool keepPlayers) {
    if (g_session.players) EndSession();
    g_versus = true;
    g_keepSlots = keepPlayers;
    SetState(kStateCoop);
}
int features::PlayerOfJoystick(int joystick) {
    for (int p = 0; p < g_session.players; ++p)
        if (g_session.joystick[p] == joystick) return p;
    if (*reinterpret_cast<uint32_t*>(kFrontEndState) == kStateCoop)  // join screen: the slot it joined
        for (int i = 0; i < kSlots; ++i)
            if (g_slots[i].device == joystick) return i;
    return 0;
}
int features::CoopKeyProfile(int player) {
    return player >= 0 && player < 4 && player < g_session.players ? g_session.keyProfile[player] : -1;
}
int features::CoopPromptStyle(int player) {
    if (!g_session.players || player < 0 || player >= g_session.players) return -1;
    // online: a keyboard-only player's prompts are the keys, the others follow the device in use (the session had no
    // styles: every prompt showed the keyboard even with a pad in hand, user 2026-10-22)
    if (g_session.online) return features::LobbyLocal(player) && features::LobbyDevice(player) == 8 ? 0 : -1;
    return g_session.style[player];
}

int features::CoopJoystick(int player) { return player >= 0 && player < 4 ? g_session.joystick[player] : -1; }

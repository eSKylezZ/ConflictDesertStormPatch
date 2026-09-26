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
//               second (the list shows items in pool order); the selection handler (0x4152F8) sends it to our state
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

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kStateMainMenu = 1, kStateCoop = 0x3E;
constexpr uint32_t kSetState = 0x47EF20;           // cdecl (state)
constexpr uint32_t kScreenForState = 0x480930, kScreenForStateCont = 0x480935;
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
constexpr uint32_t kStateUniform = 6, kStateMissionList = 8;
constexpr uint32_t kUniformUpdateSlot = 0x5D9610;   // uniform screen vtable +0x10 (FUN_0049baf0)
constexpr uint32_t kMissionListScreen = 0x617C00;   // state 8's screen object
constexpr uint32_t kMissionListUpdateSlot = 0x5D8E40;  // mission list vtable +0x10 (FUN_004595d0)
constexpr uint32_t kMissionDrawCbPush = 0x459508;   // push 0x4596b0 (mission item draw callback)
constexpr uint32_t kMissionDrawCb = 0x4596B0;
constexpr uint32_t kMissionIndexCall = 0x4595F1, kItemIndex = 0x4E6F80;  // list thiscall (item id) -> list index
constexpr uint32_t kMissionTable = 0x5EB8A8, kLevelNames = 0x5E6CC0;
constexpr uint32_t kLoadPending = 0x60EC8C;         // byte: 1 = a level loads this frame
constexpr int kMissions = 12, kMissionItemId = 0x1005;

constexpr uint16_t kCoopItemId = 0x401, kSlotItemId = 0x500;
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

enum class PadType { Keyboard, PlayStation, Xbox, Generic };
enum class Icons : int { Auto, PlayStation, Xbox, Keyboard, Count };
constexpr int kKeyboard = 8;      // device id of the keyboard (joysticks are 0..7)
constexpr int kFakeDevice = 100;  // dev builds: CoopFakeJoin test players

struct Slot {
    int device = -1;  // -1 empty, 0..7 game joystick, kKeyboard, kFakeDevice + n
    PadType type = PadType::Keyboard;
    Icons icons = Icons::Auto;
    std::string name;
};
Slot g_slots[4];
uint32_t g_prevActions[kKeyboard + 1];

// The running co-op session (from Begin until the main menu is shown again).
struct Session {
    int players = 0;
    int joystick[4] = {-1, -1, -1, -1};
    int style[4] = {0, 0, 0, 0};  // icon style per player: 0 keyboard, 1 PlayStation, 2 Xbox (Auto resolved)
    int mission = -1;        // mission (0-11) of the level last loaded in this session
    std::string lastLevel;   // to see each level load once
} g_session;
bool g_preselected = false;  // co-op mission list: first open mission highlighted

// --- co-op progress (registry, next to the Enhancements settings) ---------------------------------------------------

constexpr char kProgressKey[] = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Enhancements";

uint32_t LoadProgress() {
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_LOCAL_MACHINE, kProgressKey, "CoopProgress", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

void MarkFinished(int mission) {
    const uint32_t v = LoadProgress() | (1u << mission);
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kProgressKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegSetValueExA(key, "CoopProgress", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
        RegCloseKey(key);
    }
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

enum Action : uint32_t { kJoin = 1, kLeave = 2, kStart = 4, kLeft = 8, kRight = 16, kBack = 32 };

int JoinedCount() {
    int n = 0;
    for (const Slot& s : g_slots) n += s.device >= 0;
    return n;
}

void ResetSlots() {
    for (Slot& s : g_slots) s = Slot{};
    memset(g_prevActions, 0xFF, sizeof g_prevActions);  // ignore buttons already held when the screen opens
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
            break;
        case PadType::Xbox:  // 0 A 1 B ... 7 Start
            if (button(0)) a |= kJoin;
            if (button(1)) a |= kLeave;
            if (button(7)) a |= kStart;
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
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    uint32_t a = 0;
    if (down(VK_SPACE)) a |= kJoin;
    if (down(VK_BACK)) a |= kLeave;
    if (down(VK_ESCAPE)) a |= kBack;
    if (down(VK_RETURN)) a |= kStart;
    if (down(VK_LEFT)) a |= kLeft;
    if (down(VK_RIGHT)) a |= kRight;
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
        g_session.joystick[g_session.players++] = s.device < kKeyboard ? s.device : -1;
    }
    *reinterpret_cast<uint32_t*>(kSplitFlag) = 1;
    *reinterpret_cast<uint32_t*>(kSplitPlayers) = static_cast<uint32_t>(g_session.players);
    *reinterpret_cast<uint32_t*>(kSplitLoadedGame) = 0;
    const char* first = *reinterpret_cast<char**>(kCampaign) + 0x37;
    strcpy(*reinterpret_cast<char**>(kNextLevel), first);
    dslog::Write("Co-op: session with %d players, campaign from %s", g_session.players, first);
    SetState(kStateDifficulty);
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
    *reinterpret_cast<uint32_t*>(kSplitFlag) = 0;
}

void Handle(int device, uint32_t actions, PadType type, const std::string& name) {
    const uint32_t pressed = actions & ~g_prevActions[device];
    g_prevActions[device] = actions;
    if (!pressed) return;
    Slot* s = SlotOf(device);
    if (!s) {
        if (pressed & kJoin) Join(device, type, name);
        else if (pressed & (kLeave | kBack)) SetState(kStateMainMenu);  // Circle / B / Esc before joining: back
        return;
    }
    if (pressed & kBack) return SetState(kStateMainMenu);
    if (pressed & kLeave) return Leave(device);
    const int n = static_cast<int>(Icons::Count);
    if (pressed & kLeft) s->icons = static_cast<Icons>((static_cast<int>(s->icons) + n - 1) % n);
    if (pressed & kRight) s->icons = static_cast<Icons>((static_cast<int>(s->icons) + 1) % n);
    if ((pressed & kStart) && JoinedCount() >= 2) Begin();
}

void PollDevices() {
    if (GetForegroundWindow() != *reinterpret_cast<HWND*>(kGameWindow)) return;
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), kKeyboard);
    for (int j = 0; j < count; ++j) {
        PadType type;
        std::string name;
        Describe(j, type, name);
        Handle(j, JoystickActions(j, type), type, name);
    }
    Handle(kKeyboard, KeyboardActions(), PadType::Keyboard, "KEYBOARD & MOUSE");
}

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

// Main menu set-up: the original (SINGLE PLAYER, MULTIPLAYER, OPTIONS, QUIT on pool items 0-3), then CO-OP on item
// 4, and the pool rotated to SINGLE PLAYER, CO-OP, MULTIPLAYER, OPTIONS, QUIT. The item objects are interchangeable,
// so the next set-up (every time the menu is entered) simply fills the rotated pool again.
void __fastcall MainMenuSetup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(kMainMenuSetup)(screen);
    void* list = At<void*>(screen, 8);
    void** items = At<void**>(list, 0x2C);
    SetItem(items[4], kCoopItemId, kHashCoop);
    AddToList(list, items[4]);
    void* coop = items[4];
    for (int i = 4; i > 1; --i) items[i] = items[i - 1];
    items[1] = coop;
    // The list area was sized for four rows (list +0x38 row pitch, +0x40 area height, +0x42 visible rows).
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = 5;
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
        cmp si, 0x3FC
        jle done
        push kMenuSelectCont
        ret
    coop:
        push kStateCoop
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
    ResetSlots();
#ifndef DS_DIST
    DWORD fake = 0, size = sizeof fake;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "CoopFakeJoin", RRF_RT_REG_DWORD, nullptr, &fake,
                 &size);
    for (DWORD i = 0; i < fake && i < kSlots; ++i)
        g_slots[i] = Slot{kFakeDevice + static_cast<int>(i), i % 2 ? PadType::Xbox : PadType::PlayStation, Icons::Auto,
                          i % 2 ? "XBOX CONTROLLER" : "DUALSENSE"};
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

// Like the MULTIPLAYER screen: panel, title, list (the main menu draws the logo instead of a title).
void __fastcall CoopDraw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kHashTitle, 1);
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);

    // Icons in the rows' gaps (row i: list +0x3c top + i * pitch +0x38, height +0x34; text centred on +0x3a/+0x32).
    void* font = At<void*>(list, 4);
    const int size = IconSize();
    const int rowX = At<int16_t>(list, 0x3A), rowW = At<int16_t>(list, 0x32);
    const int top0 = At<int16_t>(list, 0x3C), pitch = At<int16_t>(list, 0x38), rowH = At<int16_t>(list, 0x34);
    for (int i = 0; i < kSlots; ++i) {
        const char* text = features::CoopText(kHashSlot0 + i);
        const RowIcons& row = g_rowIcons[i];
        if (!text || !row.count) continue;
        int x = rowX + (rowW - overlay::TextWidth(font, text)) / 2 + overlay::TextWidth(font, row.prefix.c_str());
        const int y = top0 + i * pitch + (rowH - size) / 2;
        for (int k = 0; k < row.count; ++k, x += size)
            overlay::QueueIcon(row.icons[k], static_cast<float>(x), static_cast<float>(y), static_cast<float>(size));
    }

    // Prompt line (where the game draws "Return: Select" / "Esc: Back").
    auto* r = *reinterpret_cast<uint8_t**>(0x63C924);
    const int w = *reinterpret_cast<int*>(r + 0x40688), h = *reinterpret_cast<int*>(r + 0x4068C);
    const int y = h * 91 / 100;
    using overlay::Icon;
    if (JoinedCount() >= 2) DrawPrompt(w * 46 / 1000, y, Icon::PsStart, Icon::XbStart, Icon::KbEnter, "Begin");
    else DrawPrompt(w * 46 / 1000, y, Icon::PsCross, Icon::XbCross, Icon::KbSpace, "Join");
    if (JoinedCount() > 0)
        DrawPrompt(w * 30 / 100, y, Icon::PsDpadHorizontal, Icon::XbDpadHorizontal, Icon::KbArrowsHorizontal, "Icons");
    DrawPrompt(w * 60 / 100, y, Icon::PsCircle, Icon::XbCircle, Icon::KbEscape, "Back / Leave");
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
    return 0;
}

// Front-end state -> screen: ours for kStateCoop, else the original (its first instructions, then the rest).
__declspec(naked) void ScreenForStateStub() {
    __asm {
        mov eax, [esp + 4]
        cmp eax, kStateCoop
        jne original
        mov eax, g_screen
        ret
    original:
        dec eax
        push kScreenForStateCont
        ret
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
    const uint8_t capacity5[] = {0x6A, 0x05};
    const uint8_t count5[] = {0xBF, 0x05, 0x00, 0x00, 0x00};
    const uint8_t nops[] = {0x90, 0x90};
    return patch::Write(kMenuCapacity, capacity5, sizeof capacity5) && patch::Write(kMenuItemCount, count5, sizeof count5) &&
           patch::WriteValue(kMainMenuSetupSlot, reinterpret_cast<uint32_t>(&MainMenuSetup)) &&
           patch::WriteJump(kMenuSelect, reinterpret_cast<const void*>(&MenuSelectStub)) &&
           patch::Write(kMenuSelect + 5, nops, 2) &&
           patch::WriteJump(kScreenForState, reinterpret_cast<const void*>(&ScreenForStateStub));
}
}  // namespace

namespace {
// --- co-op mission list (state 8) ---------------------------------------------------------------------------------

bool CoopMissionList() {
    return g_session.players && *reinterpret_cast<uint32_t*>(kFrontEndState) == kStateMissionList;
}

// Uniform screen: in a session its "start the level" (3) opens the mission list instead.
using UpdateFn = int(__thiscall*)(void*);
UpdateFn g_uniformUpdate = nullptr, g_missionListUpdate = nullptr;

int __fastcall UniformUpdate(void* screen, void*) {
    const int r = g_uniformUpdate(screen);
    if (r == 3 && g_session.players) {
        SetState(kStateMissionList);
        return 0;
    }
    return r;
}

// Mission list: Back goes to the uniform screen in a session (the game's own target is the CHEATS menu, state 7).
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
    if (g_session.players && *reinterpret_cast<uint32_t*>(kFrontEndState) == 7) SetState(kStateUniform);
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

__declspec(naked) void MissionDrawStub() {
    __asm {
        push dword ptr [esp + 4]
        call BeforeMissionDraw
        add esp, 4
        mov eax, kMissionDrawCb
        jmp eax
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

bool InstallMissionList() {
    const uint32_t uniform = 0x49BAF0, missionList = 0x4595D0;
    const uint8_t push[] = {0x68, 0xB0, 0x96, 0x45, 0x00};
    if (!patch::Matches(kUniformUpdateSlot, &uniform, 4) || !patch::Matches(kMissionListUpdateSlot, &missionList, 4) ||
        !patch::Matches(kMissionDrawCbPush, push, sizeof push)) {
        dslog::Write("[fail] Co-op mission list: unexpected bytes - not applied");
        return false;
    }
    g_uniformUpdate = reinterpret_cast<UpdateFn>(uniform);
    g_missionListUpdate = reinterpret_cast<UpdateFn>(missionList);
    const uint32_t stub = reinterpret_cast<uint32_t>(&MissionDrawStub);
    return patch::WriteValue(kUniformUpdateSlot, reinterpret_cast<uint32_t>(&UniformUpdate)) &&
           patch::WriteValue(kMissionListUpdateSlot, reinterpret_cast<uint32_t>(&MissionListUpdate)) &&
           patch::Write(kMissionDrawCbPush + 1, &stub, 4) &&
           patch::HookCall(kMissionIndexCall, reinterpret_cast<const void*>(&MissionIndex), kItemIndex);
}
}  // namespace

// Texts of our own string hashes (the game's lookup is hooked in ingame_input.cpp); nullptr = not ours.
const char* features::CoopText(uint32_t hash) {
    static char slots[kSlots][96];
    switch (hash) {
        case kHashCoop: return "CO-OP";
        case kHashTitle: return "JOIN CO-OPERATIVE GAME";
        default: break;
    }
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
    g_installed = Install() && InstallMissionList();
    if (g_installed) dslog::Write("[ok]   Co-op menu: CO-OP entry + join screen (state 0x%X)", kStateCoop);
}

void features::OnFrameCoop() {
    if (!g_installed) return;
    if (!g_screen && *reinterpret_cast<void**>(kMainMenuScreen)) {
        if (BuildScreen()) dslog::Write("Co-op: join screen created");
        else dslog::Write("[fail] Co-op: join screen could not be created");
    }
    const uint32_t state = *reinterpret_cast<uint32_t*>(kFrontEndState);
    if (g_screen && state == kStateCoop) PollDevices();
    if (state != kStateMissionList) g_preselected = false;
    TrackProgress();
    if (g_session.players && state == kStateMainMenu) EndSession();
}

int features::CoopPlayers() { return g_session.players; }
int features::CoopPromptStyle(int player) {
    if (!g_session.players || player < 0 || player >= g_session.players) return -1;
    return g_session.style[player];
}

int features::CoopJoystick(int player) { return player >= 0 && player < 4 ? g_session.joystick[player] : -1; }

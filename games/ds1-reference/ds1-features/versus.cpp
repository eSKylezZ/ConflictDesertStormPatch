// Main menu MULTIPLAYER -> VERSUS: a screen with LEGACY ONLINE (the game's own network screens:
// HOST / JOIN SESSION, session name, port, Internet / LAN - state 3, kept as it was) and SPLIT SCREEN (local versus:
// co-op's join screen in versus mode -> MATCH SETUP (state 0x44) -> the MP level with a local session, below).
//
// The main menu's MULTIPLAYER item (id 0x3FE, text 0x08284D6C) gets our text VERSUS (coop.cpp's MainMenuSetup) and
// opens state 0x42 (coop.cpp's selection stub); the network screen's title, the same text hash, reads LEGACY ONLINE
// while state 3 is up (VersusText). Our screen is an object of the Options sub-screen class (like the CONTROLS
// screens, controls.cpp) with a copied vtable, served by coop.cpp's state -> screen hook.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kSetState = 0x47EF20, kBackPressed = 0x478380, kListSelected = 0x4E5D80;
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kItemCtor = 0x4E4F90, kItemSet = 0x4E4FB0;
constexpr uint32_t kDrawPanel = 0x456EB0, kDrawTitle = 0x456F40;
constexpr uint32_t kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kMainMenuScreen = 0x617A00, kFrontEndState = 0x617C18;
constexpr uint32_t kMainMenuEnter = 0x4153B0;
constexpr uint32_t kOptCtor = 0x409B90, kOptVtable = 0x5D8510, kOptSetup = 0x409C90, kOptItemDrawCb = 0x409E90;
constexpr uint32_t kStateMainMenu = 1, kStateNetwork = 3;
constexpr uint32_t kHashMultiplayer = 0x08284D6C;
constexpr uint16_t kItemLegacy = 0x840, kItemSplit = 0x841;
constexpr int kVtableEntries = 21;
enum : uint32_t {
    kTextVersus = 0xC0090001, kTextLegacy, kTextSplit, kTextSetup, kTextMode, kTextMap, kTextTeam1, kTextTeam2,
    kText5Min, kText10Min, kText15Min, kText20Min, kTextNoLimit, kTextSas, kTextDelta, kTextIraqi, kTextRussian,
    kTextScoreLimit, kText5Kills, kText10Kills, kText15Kills, kText20Kills, kText25Kills
};
constexpr uint32_t kStateVersus = 0x42, kStateSetup = 0x44;

// MATCH SETUP choices (kept for the next match): option rows show the game's own texts where it has them.
struct Match {
    int mode = 0, map = 0, time = 1, score = 0, respawn = 1, friendlyFire = 0, uniform[2] = {0, 2};
    bool pending = false;  // START pressed: the next MP level load opens the session with these
} g_match;
constexpr uint32_t kModeHashes[] = {0xD7B049A5, 0x9D66F17B};  // TEAM DEATHMATCH, DEATHMATCH
constexpr uint8_t kModeGameType[] = {1, 2};                    // game type option (0x5EBBA5)
constexpr uint32_t kMapHashes[] = {0x45680FA0, 0x45680F16, 0x45680FCD, 0x45680F4D, 0x45680F96, 0x45680F20};
constexpr uint32_t kTimeHashes[] = {kText5Min, kText10Min, kText15Min, kText20Min, kTextNoLimit};
// SCORE LIMIT: kills of a player (deathmatch) or a team (team deathmatch) that end the match.
constexpr uint32_t kScoreHashes[] = {kTextNoLimit, kText5Kills, kText10Kills, kText15Kills, kText20Kills, kText25Kills};
constexpr int kScoreLimits[] = {0, 5, 10, 15, 20, 25};
constexpr uint32_t kYesNo[] = {0x0001F3F3, 0x017E33D4};        // NO, YES (index = the game's option value)
constexpr uint32_t kUniformHashes[] = {kTextSas, kTextDelta, kTextIraqi, kTextRussian};  // the game's are too long
constexpr uint32_t kHashTimeLimit = 0x5A64B855, kHashRespawn = 0x6C6C78B9, kHashFriendlyFire = 0xA64FA548;
constexpr uint32_t kHashStart = 0xAECB17B5;  // START
enum : uint16_t {
    kRowMode = 0x850, kRowMap, kRowTime, kRowRespawn, kRowFriendly, kRowTeam1, kRowTeam2, kRowStart, kRowScore
};
// Row order in the list (pool index).
enum { kIdxMode, kIdxMap, kIdxTime, kIdxScore, kIdxRespawn, kIdxFriendly, kIdxTeam1, kIdxTeam2, kIdxStart, kSetupRows };

uint8_t g_screen[0x20];
void* g_vtable[kVtableEntries];
bool g_built = false;

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
void SetState(uint32_t s) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(s); }
void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }
void SetItem(void* item, uint16_t id, uint32_t hash) {
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    reinterpret_cast<Fn>(kItemSet)(item, id, hash, 0, 0x24, 0, 0, kOptItemDrawCb, 0);
}
void AddToList(void* list, void* item) {
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, item);
}

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
bool __fastcall Create(void* screen, void*) { return CreateList(screen, 2); }

// The class's set-up (list layout + its two items), then our two items and title.
void __fastcall Setup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);
    void* list = At<void*>(screen, 8);
    SetItem(ItemAt(list, 0), kItemLegacy, kTextLegacy);
    SetItem(ItemAt(list, 1), kItemSplit, kTextSplit);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kTextVersus, 0, 0, 0x7FFF);
}

int __fastcall Update(void* screen, void*) {
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(At<void*>(screen, 8));
    if (id == kItemLegacy) SetState(kStateNetwork);
    if (id == kItemSplit) SetState(kStateSetup);  // MATCH SETUP first, then the players join
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateMainMenu);
    return 0;
}

void __fastcall Draw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kTextVersus, 1);
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
}

// ---- MATCH SETUP (state 0x44) ----
// Option rows like the old network host screen (FUN_0045d8c0): item text -1, label hash at +0x14, draw callback
// 0x45DCD0 (-> FUN_00456040: label, then the screen's value draw vtable +0x4C, arrows), options FUN_004e5090 (up to 8
// hashes, count item +0x25), choice item +0x24; the list's left / right callbacks 0x4785E0 / 0x478690 change it
// (FUN_004e59e0, the class set-up installs only the first three) and the callback finds the screen at list +0x90.
constexpr uint32_t kItemOptions = 0x4E5090, kListCallbacks = 0x4E59E0, kOptionRowDraw = 0x45DCD0;
constexpr uint32_t kProgress = 0x60EDC0, kResetProgress = 0x4576C0, kStartLevelFlag = 0x60EE14;
uint8_t g_setup[0x20];
bool g_keepJoined = false;  // back from the join screen: its players stay joined for the next START
bool g_startLevel = false;  // the join screen's Begin: its update returns 3 (load the level) once
void* g_setupVtable[kVtableEntries];
bool g_setupBuilt = false;

void SetOptionRow(void* item, uint16_t id, uint32_t label, const uint32_t* options, int count, int choice) {
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    reinterpret_cast<Fn>(kItemSet)(item, id, 0xFFFFFFFF, 0, 0x24, static_cast<int>(label), 0, kOptionRowDraw, 0);
    uint32_t h[8];
    for (int i = 0; i < 8; ++i) h[i] = i < count ? options[i] : 0xFFFFFFFF;
    using Opts = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    reinterpret_cast<Opts>(kItemOptions)(item, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    At<int32_t>(item, 0x20) = 1;  // value from the option list (FUN_00456a70; 0 = the item's own text)
    At<int8_t>(item, 0x24) = static_cast<int8_t>(choice);
}

bool __fastcall SetupCreate(void* screen, void*) { return CreateList(screen, kSetupRows); }

void __fastcall SetupSetup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);
    void* list = At<void*>(screen, 8);
    using Callbacks = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int, int, int, int, int);
    reinterpret_cast<Callbacks>(kListCallbacks)(list, 0x478440, 0x478480, 0x478530, 0x4785E0, 0x478690, 0, 0, 0, 0, 0);
    At<void*>(list, 0x90) = screen;  // the option row draw callback's screen
    SetOptionRow(ItemAt(list, kIdxMode), kRowMode, kTextMode, kModeHashes, 2, g_match.mode);
    SetOptionRow(ItemAt(list, kIdxMap), kRowMap, kTextMap, kMapHashes, 6, g_match.map);
    SetOptionRow(ItemAt(list, kIdxTime), kRowTime, kHashTimeLimit, kTimeHashes, 5, g_match.time);
    SetOptionRow(ItemAt(list, kIdxScore), kRowScore, kTextScoreLimit, kScoreHashes, 6, g_match.score);
    SetOptionRow(ItemAt(list, kIdxRespawn), kRowRespawn, kHashRespawn, kYesNo, 2, g_match.respawn);
    SetOptionRow(ItemAt(list, kIdxFriendly), kRowFriendly, kHashFriendlyFire, kYesNo, 2, g_match.friendlyFire);
    SetOptionRow(ItemAt(list, kIdxTeam1), kRowTeam1, kTextTeam1, kUniformHashes, 4, g_match.uniform[0]);
    SetOptionRow(ItemAt(list, kIdxTeam2), kRowTeam2, kTextTeam2, kUniformHashes, 4, g_match.uniform[1]);
    SetItem(ItemAt(list, kIdxStart), kRowStart, kHashStart);
    for (int i = 2; i < kSetupRows; ++i) AddToList(list, ItemAt(list, i));
    // The list area was sized for two rows (list +0x38 row pitch, +0x40 area height, +0x42 visible rows).
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (kSetupRows - 2) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = kSetupRows;
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kTextSetup, 0, 0, 0x7FFF);
}

int Choice(void* list, int row) { return At<int8_t>(ItemAt(list, row), 0x24); }

int __fastcall SetupUpdate(void* screen, void*) {
    void* list = At<void*>(screen, 8);
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(list);
    g_match.mode = Choice(list, kIdxMode), g_match.map = Choice(list, kIdxMap), g_match.time = Choice(list, kIdxTime);
    g_match.score = Choice(list, kIdxScore);
    g_match.respawn = Choice(list, kIdxRespawn), g_match.friendlyFire = Choice(list, kIdxFriendly);
    g_match.uniform[0] = Choice(list, kIdxTeam1), g_match.uniform[1] = Choice(list, kIdxTeam2);
    // Deathmatch has no teams: TEAM 1 / TEAM 2 greyed out (item +0x10 value 2: FUN_00456a70 draws the value in the
    // list's disabled colour) - every player gets a random faction instead.
    for (int row = kIdxTeam1; row <= kIdxTeam2; ++row) {
        auto& flags = At<uint16_t>(ItemAt(list, row), 0x10);
        flags = static_cast<uint16_t>(kModeGameType[g_match.mode] == 2 ? flags | 2 : flags & ~2);
    }
    if (id == kRowStart) {  // the players join next (team choice only in team modes)
        dslog::Write("Versus: match set up - mode %d, map %d, time %d, score limit %d, respawn %d, friendly fire %d, "
                     "uniforms %d/%d",
                     g_match.mode, g_match.map, g_match.time, kScoreLimits[g_match.score], g_match.respawn,
                     g_match.friendlyFire,
                     g_match.uniform[0], g_match.uniform[1]);
        features::OpenVersusJoin(g_keepJoined);
        g_keepJoined = false;
        return 0;
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateVersus);
    return 0;
}

void __fastcall SetupDraw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kTextSetup, 1);
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
}

void BuildSetup() {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptCtor)(g_setup);
    std::memcpy(g_setupVtable, reinterpret_cast<void*>(kOptVtable), sizeof g_setupVtable);
    At<void**>(g_setup, 0) = g_setupVtable;
    g_setupVtable[6] = reinterpret_cast<void*>(kMainMenuEnter);
    g_setupVtable[1] = reinterpret_cast<void*>(&SetupCreate);
    g_setupVtable[3] = reinterpret_cast<void*>(&SetupSetup);
    g_setupVtable[4] = reinterpret_cast<void*>(&SetupUpdate);
    g_setupVtable[5] = reinterpret_cast<void*>(&SetupDraw);
    g_setupBuilt = SetupCreate(g_setup, nullptr);
    dslog::Write(g_setupBuilt ? "Versus: match setup screen built" : "[fail] Versus: match setup screen not built");
}

void Build() {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptCtor)(g_screen);
    std::memcpy(g_vtable, reinterpret_cast<void*>(kOptVtable), sizeof g_vtable);
    At<void**>(g_screen, 0) = g_vtable;
    g_vtable[6] = reinterpret_cast<void*>(kMainMenuEnter);  // enter runs the set-up every time
    g_vtable[1] = reinterpret_cast<void*>(&Create);
    g_vtable[3] = reinterpret_cast<void*>(&Setup);
    g_vtable[4] = reinterpret_cast<void*>(&Update);
    g_vtable[5] = reinterpret_cast<void*>(&Draw);
    g_built = Create(g_screen, nullptr);
    dslog::Write(g_built ? "Versus: screen built" : "[fail] Versus: screen not built");
}
}  // namespace

void* features::VersusScreen() { return g_built ? g_screen : nullptr; }
void* features::VersusSetupScreen() { return g_setupBuilt ? g_setup : nullptr; }
// Join screen Begin: like the mission list's pick (FUN_004595d0) - next level, start flag, progress reset - and the
// join screen's update returns 3 (load the level, VersusTakeStart).
void features::VersusPlayersJoined() {
    if (features::CoopPlayers() < 2) return;  // split screen: two players at least (the join screen asks the same)
    wsprintfA(*reinterpret_cast<char**>(0x5E7028), "MP_Mission%d.dll", g_match.map + 1);
    *reinterpret_cast<uint32_t*>(kStartLevelFlag) = 1;
    reinterpret_cast<void(__fastcall*)(void*)>(kResetProgress)(*reinterpret_cast<void**>(kProgress));
    g_match.pending = true;
    g_startLevel = true;
}
bool features::VersusTakeStart() {
    const bool start = g_startLevel;
    g_startLevel = false;
    return start;
}
void features::VersusJoinBack() {
    g_keepJoined = true;
    SetState(kStateSetup);
}
bool features::VersusTeams() { return kModeGameType[g_match.mode] != 2; }  // deathmatch: every player for themselves

uint32_t features::VersusItemText() { return kTextVersus; }
const char* VersusScoresPrompt();  // below (needs the session)

const char* features::VersusText(uint32_t hash) {
    switch (hash) {
        case kTextVersus: return "VERSUS";
        case kTextLegacy: return "LEGACY ONLINE";
        case kTextSplit: return "SPLIT SCREEN";
        case kTextSetup: return "MATCH SETUP";
        case kTextMode: return "GAME";
        case kTextMap: return "MAP";
        case kTextTeam1: return "TEAM 1";
        case kTextTeam2: return "TEAM 2";
        case kText5Min: return "5 MINUTES";
        case kText10Min: return "10 MINUTES";
        case kText15Min: return "15 MINUTES";
        case kText20Min: return "20 MINUTES";
        case kTextNoLimit: return "NONE";
        case kTextSas: return "SAS";
        case kTextDelta: return "US DELTA";
        case kTextIraqi: return "IRAQI";
        case kTextRussian: return "RUSSIAN";
        case kTextScoreLimit: return "SCORE LIMIT";
        case kText5Kills: return "5 KILLS";
        case kText10Kills: return "10 KILLS";
        case kText15Kills: return "15 KILLS";
        case kText20Kills: return "20 KILLS";
        case kText25Kills: return "25 KILLS";
        case 0x2FAE9B7A:  // scores screen prompt "Esc: Main Menu"
            return VersusScoresPrompt();
        case kHashMultiplayer:  // the network screens' title
            return *reinterpret_cast<uint32_t*>(kFrontEndState) == kStateNetwork ? "LEGACY ONLINE" : nullptr;
        default: return nullptr;
    }
}

// The network screen's Back (push 1 at 0x4675F5, FUN_004675a0) returns to VERSUS instead of the main menu.
static void HookPumps();

void features::ApplyVersus() {
    HookPumps();
    constexpr uint32_t kBackState = 0x4675F6;
    const uint8_t original = kStateMainMenu, ours = 0x42;
    if (patch::Matches(kBackState, &ours, 1)) return;
    if (!patch::Matches(kBackState, &original, 1)) {
        dslog::Write("[fail] Versus: network screen back not recognised at 0x%08X", kBackState);
        return;
    }
    patch::Write(kBackState, &ours, 1);
}

// ---- local versus session ----
// The network game (multiplay.cpp) is still in the exe: the host's level set-up FUN_0045f3c0 (from level init, only
// while a session is open) spawns 8 MP soldiers (object list 1, one per player-table entry) and gives the local player
// [0x610F84] theirs; the rules live in the MP_MISSIONn.dll scripts, which ask the exe whether a session is open and
// whether this machine is the server. Every host / client test reads the net object [0x770154]: net+8 -> server
// state (+0xA8 host), net+0x554 -> client state (+0xB0 client), sends only go out while net+0xB88 = 1, and the session clock is a game timer at
// net+0x43C (total ms at net+0x44C). A zeroed fake object with host = 1 makes this PC a server with no network: the
// sends fail quietly, and the three per-frame pumps (server tick, receive) are skipped for it.
namespace {
constexpr uint32_t kNetReady = 0x610F78, kNetObject = 0x770154, kSessionOpen = 0x610F60, kSessionHost = 0x610F7C, kLocalPlayer = 0x610F84;
constexpr uint32_t kMpGame = 0x610F5C, kMpLevel = 0x610F80, kMpState = 0x5EBB50, kMpPendingState = 0x5EBBAC;
constexpr uint32_t kSessionPlayers = 0x5EBBA2, kGameType = 0x5EBBA5, kMap = 0x5EBBA0, kTeamUniform = 0x5EBBA3, kPlayerTable = 0x610DA4, kPlayerEntry = 0x34;
constexpr uint32_t kTableClear = 0x610DA0, kTableClearSize = 0x68 * 4;
constexpr uint32_t kOptTimeLimit = 0x5EBBA6, kOptRespawn = 0x5EBBA9, kOptFriendlyFire = 0x5EBBAA;
constexpr uint32_t kStateBusy = 0x617C20;  // nonzero: the front-end state setter ignores calls
bool g_menuMatch = false;     // the session came from MATCH SETUP (not the dev boot)
bool g_backToVersus = false;  // a menu match ended: VERSUS once the main menu is up
constexpr uint32_t kTimerCtor = 0x4BA3A0, kListObject = 0x460830, kAttachSoldier = 0x44F260, kSoldierSkin = 0x435EF0;
constexpr uint32_t kSetExperience = 0x42D1B0;
constexpr uint32_t kUniformSide = 0x4613D0;  // uniform option -> skin side (0 SAS, 1 US, 3 Iraqi, 2 Russian)
constexpr uint32_t kTeamOf = 0x460A40, kRoleOf = 0x461390, kSideOf = 0x45F850, kSessionTime = 0x461090;
constexpr uint32_t kScript = 0x60F578, kLoadPending = 0x60EC8C, kNextLevel = 0x5E7028;
constexpr uint32_t kInputBlocks = 0x60F5B8, kInputBlockSize = 0x478;
constexpr uint32_t kTextureManager = 0x63C930, kTextureBatch = 0x54A3E0;
constexpr uint32_t kMpStateInGame = 7, kMpStateIdle = 1, kMpStateQuit = 0xB, kStateScores = 0x35;
constexpr uint32_t kJoystickCount = 0x754BE8;
constexpr uint32_t kEndMatch = 0x4638F0;  // script API +0x7A8
constexpr uint8_t kNoTeam = 3;

struct Pump {
    uint32_t site, target;
};
constexpr Pump kPumps[] = {{0x4604E5, 0x5628C0}, {0x4604FA, 0x562930}, {0x46059D, 0x5628F0}};

alignas(16) uint8_t g_net[0xC10];
alignas(16) uint8_t g_server[0x200];
alignas(16) uint8_t g_client[0x200];
bool g_session = false;
int g_dmUniform[8] = {};  // deathmatch: each player's random faction (uniform 0 SAS, 1 US Delta, 2 Iraqi, 3 Russian)
int g_players = 0;
int g_scoreLimit = 0;     // kills that end the match (0 = none)
bool g_limitReached = false;

// Host send FUN_005629e0 (thiscall net, message) - replaced: the original send for a real net object; for ours the
// message goes to the client game-message handler (type 0xA, FUN_00465310: 5-bit subtype + fields), as DirectPlay
// delivered the host's broadcasts to its own player too; client sends (FUN_00562ac0) go to the server handler (type 4,
// FUN_004649e0). The host depends on it: a death arrives as a request
// (game event kind 2, +8 = 1) -> FUN_00466220 -> FUN_004632d0 broadcasts the result (+8 = 0), and only that copy
// updates the player table (deaths, killer's kills). Message = {+0 2 = has payload, +4 type, ..., +0x24 payload bits,
// +0x28 bytes, +0x2C payload}; bit stream object 0x1A4 bytes: FUN_00561ec0(mode 1 write / 2 read), FUN_00562520
// (data, bits) loads, FUN_00561ef0 ends a mode. Sends made while a message is handled are queued and delivered after
// it, like packets would be (capped, in case two handlers keep answering each other).
constexpr uint32_t kHostSend = 0x5629E0, kServerIsHost = 0x563700, kServerSend = 0x5632C0;
constexpr uint32_t kStreamMode = 0x561EC0, kStreamEnd = 0x561EF0, kStreamLoad = 0x562520, kStreamRead = 0x561F10;
constexpr uint32_t kServerGameMessage = 0x4649E0, kClientGameMessage = 0x465310;  // handler types 4 / 0xA
constexpr uint8_t kHostSendEntry[] = {0x8B, 0x81, 0x88, 0x0B, 0x00, 0x00};
constexpr uint32_t kMessageSize = 0x1BC;
constexpr int kQueueSize = 32, kMaxPerSend = 64;
struct Queued {
    uint8_t bytes[kMessageSize];
    uint32_t handler;
};
Queued g_queue[kQueueSize];
int g_queueHead = 0, g_queueCount = 0;
bool g_delivering = false;
uint32_t g_loopedBack = 0;

void Deliver(const uint8_t* msg, uint32_t handler) {
    const uint32_t bits = At<uint32_t>(const_cast<uint8_t*>(msg), 0x24);
    if (!bits || bits > 400 * 8) return;
    alignas(4) uint8_t stream[0x1A4]{};
    alignas(4) uint8_t info[0x40]{};
    using Mode = int(__thiscall*)(void*, int);
    using End = int(__thiscall*)(void*);
    reinterpret_cast<Mode>(kStreamMode)(stream, 1);
    reinterpret_cast<int(__thiscall*)(void*, const void*, uint32_t)>(kStreamLoad)(stream, msg + 0x2C, bits);
    reinterpret_cast<End>(kStreamEnd)(stream);
    reinterpret_cast<Mode>(kStreamMode)(stream, 2);
    // Each message starts with the receive loop's flag bit (1 = system message, handled by the net layer).
    using Read = bool(__thiscall*)(void*, uint32_t*, uint32_t);
    uint32_t system = 0;
    reinterpret_cast<Read>(kStreamRead)(stream, &system, 1);
    if (system) return;
    ++g_loopedBack;
    reinterpret_cast<int(__stdcall*)(void*, void*)>(handler)(info, stream);
}

void Loopback(const uint8_t* msg, uint32_t handler) {
    if (At<int32_t>(const_cast<uint8_t*>(msg), 0) != 2) return;
#ifndef DS_DIST
    static int logged = 0;
    if (logged < 300 && At<uint32_t>(const_cast<uint8_t*>(msg), 4) != 0x11) {  // not the soldier state updates
        ++logged;
        dslog::Write("[dev]  Versus: send type 0x%X subtype %u bits %u%s", At<uint32_t>(const_cast<uint8_t*>(msg), 4),
                     (msg[0x2C] >> 1) & 0x1F, At<uint32_t>(const_cast<uint8_t*>(msg), 0x24), g_delivering ? " (nested)" : "");
    }
#endif
    if (g_queueCount == kQueueSize) {
        dslog::Write("[fail] Versus: loopback queue full, message dropped");
        return;
    }
    Queued& q = g_queue[(g_queueHead + g_queueCount++) % kQueueSize];
    std::memcpy(q.bytes, msg, kMessageSize);
    q.handler = handler;
    if (g_delivering) return;  // delivered by the loop below once the current message is done
    g_delivering = true;
    for (int n = 0; g_queueCount && n < kMaxPerSend; ++n) {
        Queued m = g_queue[g_queueHead];
        g_queueHead = (g_queueHead + 1) % kQueueSize;
        --g_queueCount;
        Deliver(m.bytes, m.handler);
    }
    if (g_queueCount) dslog::Write("[fail] Versus: loopback kept answering itself, %d messages dropped", g_queueCount);
    g_queueHead = g_queueCount = 0;
    g_delivering = false;
}

int __fastcall HostSend(uint8_t* net, void*, uint8_t* msg) {
    if (net == g_net) {
#ifndef DS_DIST
#endif
        // A broadcast: what the host's own player received. Soldier actions (type 0x14, FUN_0041e310: climb a ladder,
        // mount a gun, doors ... - the host only broadcasts them and carries them out when its own copy comes back),
        // game events (0x1A: deaths, respawns, pickups) and the player table (0x1B). Not the soldier state updates
        // (0x11): the client handler (FUN_00466fc0 -> FUN_0041eb90) applies them to every soldier but player 1's,
        // which reset players 2-4 to the last broadcast every frame (they could walk but not turn or shoot).
        const uint32_t type = At<uint32_t>(msg, 4);
        if (type == 0x14 || type == 0x1A || type == 0x1B) Loopback(msg, kClientGameMessage);
        return 0;
    }
    if (At<int32_t>(net, 0xB88) != 1) return -1;
    uint8_t* server = net + 4;
    if (reinterpret_cast<int(__thiscall*)(void*)>(kServerIsHost)(server) != 1) return -1;
    return reinterpret_cast<int(__thiscall*)(void*, void*)>(kServerSend)(server, msg);
}

// Client send FUN_00562ac0 (thiscall net, message): same shape; for our object it goes to the same server handler.
constexpr uint32_t kClientSend = 0x562AC0, kClientIsClient = 0x563F70, kClientSendQueue = 0x563F90;
int __fastcall ClientSend(uint8_t* net, void*, uint8_t* msg) {
    if (net == g_net) {
        Loopback(msg, kServerGameMessage);  // to the server
        return 0;
    }
    if (At<int32_t>(net, 0xB88) != 1) return -1;
    uint8_t* client = net + 0x550;
    if (reinterpret_cast<int(__thiscall*)(void*)>(kClientIsClient)(client) != 1) return -1;
    return reinterpret_cast<int(__thiscall*)(void*, void*)>(kClientSendQueue)(client, msg);
}

// Host shutdown FUN_0045fcf0 (session end FUN_0045fc20, MP state 0xB) closes the DirectPlay server via
// FUN_005629c0 (thiscall net) - nothing to close for ours.
constexpr uint32_t kServerCloseCall = 0x45FD0F, kServerClose = 0x5629C0;
int __fastcall ServerClose(void* net, void*) {
    if (net == g_net) return 0;
    return reinterpret_cast<int(__thiscall*)(void*)>(kServerClose)(net);
}

template <uint32_t Target>
int __fastcall PumpStub(void* net, void*) {
    if (net == g_net) return 0;
    return reinterpret_cast<int(__thiscall*)(void*)>(Target)(net);
}

int Requested() {
#ifndef DS_DIST
    DWORD v = 0, size = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "Versus", RRF_RT_REG_DWORD, nullptr, &v,
                     &size) == ERROR_SUCCESS)
        return static_cast<int>(v);
#endif
    return 0;
}

bool IsMpLevel(const char* name) { return name && _strnicmp(name, "MP_Mission", 10) == 0; }

template <class T>
T& G(uint32_t va) { return *reinterpret_cast<T*>(va); }

void OpenSession(int gameType, int map, int players) {
    std::memset(g_net, 0, sizeof g_net);
    std::memset(g_server, 0, sizeof g_server);
    std::memset(g_client, 0, sizeof g_client);
    At<void*>(g_net, 8) = g_server;        // server part net+4: state +0xA8 = host
    At<uint32_t>(g_server, 0xA8) = 1;
    At<void*>(g_net, 0x554) = g_client;    // client part net+0x550: state +0xB0 = client (no)
    At<uint32_t>(g_net, 0xB88) = 1;        // connected (FUN_0045f3a0 -> session settings 0x5EBB58 for the scores screen)
    reinterpret_cast<void*(__thiscall*)(void*)>(kTimerCtor)(g_net + 0x43C);  // session clock
    G<void*>(kNetObject) = g_net;

    // Player table like the host's session start FUN_0045fab0, then one entry per split-screen player.
    std::memset(reinterpret_cast<void*>(kTableClear), 0, kTableClearSize);
    for (int i = 0; i < 8; ++i) {
        uint8_t* e = reinterpret_cast<uint8_t*>(kPlayerTable + i * kPlayerEntry);
        e[0x15] = kNoTeam;
        e[0x2C] = 3;
        e[0x2D] = 0xFF;
        if (i < players) {
            wsprintfA(reinterpret_cast<char*>(e), "PLAYER %d", i + 1);
            const int team = features::CoopTeam(i);  // picked on the join screen, else alternating
            e[0x15] = static_cast<uint8_t>(team >= 0 ? team : i & 1);
            const int cls = features::CoopClass(i);  // character skill: kit + skin of that role (FUN_00461390)
            e[0x16] = static_cast<uint8_t>(cls >= 0 ? cls : 0);
            At<uint32_t>(e, 0x24) = 2;              // in the game
            At<uint32_t>(e, 0x28) = 1;
        }
    }
    G<uint8_t>(kSessionPlayers) = static_cast<uint8_t>(players);  // FUN_00466220 ignores entries >= this
    G<uint8_t>(kGameType) = static_cast<uint8_t>(gameType);
    G<uint8_t>(kMap) = static_cast<uint8_t>(map);
    // MATCH SETUP options: team uniforms (UI order 0 SAS, 1 US Delta, 2 Iraqi, 3 Russian; FUN_0045f850 -> skin side,
    // voice bank), time limit (FUN_00461060), respawn, friendly fire (0 no / 1 yes, the network host screen's values).
    G<uint8_t>(kTeamUniform) = static_cast<uint8_t>(g_match.uniform[0]);
    G<uint8_t>(kTeamUniform + 1) = static_cast<uint8_t>(g_match.uniform[1]);
    G<uint8_t>(kOptTimeLimit) = static_cast<uint8_t>(g_match.time);
    G<uint8_t>(kOptRespawn) = static_cast<uint8_t>(g_match.respawn);
    // Deathmatch has no teams: everyone can hit everyone (the damage code skips same-team hits without friendly fire).
    G<uint8_t>(kOptFriendlyFire) = static_cast<uint8_t>(gameType == 2 ? 1 : g_match.friendlyFire);
    G<uint32_t>(kSessionOpen) = 1;
    G<uint32_t>(kSessionHost) = 1;
    G<uint32_t>(kNetReady) = 1;
    G<uint32_t>(kLocalPlayer) = 0;
    G<uint32_t>(kMpState) = kMpStateInGame;
    G<uint32_t>(kMpPendingState) = kMpStateInGame;
    if (gameType == 2) {  // deathmatch: a random faction per player (skin, kit and voice of that side)
        LARGE_INTEGER seed;
        QueryPerformanceCounter(&seed);
        uint32_t x = static_cast<uint32_t>(seed.QuadPart) | 1;  // xorshift32 (rand()'s low bits just alternate)
        for (int& u : g_dmUniform) {
            x ^= x << 13, x ^= x >> 17, x ^= x << 5;
            u = static_cast<int>(x >> 30);
        }
        dslog::Write("Versus: deathmatch factions %d %d %d %d (0 SAS, 1 US, 2 Iraqi, 3 Russian)", g_dmUniform[0],
                     g_dmUniform[1], g_dmUniform[2], g_dmUniform[3]);
    }
    g_scoreLimit = g_menuMatch ? kScoreLimits[g_match.score] : 0;
#ifndef DS_DIST
    {
        DWORD v = 0, size = sizeof v;  // dev boot: Dev\VersusScoreLimit
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VersusScoreLimit", RRF_RT_REG_DWORD,
                         nullptr, &v, &size) == ERROR_SUCCESS && v)
            g_scoreLimit = static_cast<int>(v);
    }
#endif
    g_limitReached = false;
    g_session = true;
    g_players = players;
    dslog::Write("Versus: local session, game type %d, map %d, %d players", gameType, map, players);
}

void CloseSession() {
    g_backToVersus = g_menuMatch;
    g_menuMatch = false;
    G<void*>(kNetObject) = nullptr;
    G<uint32_t>(kSessionOpen) = 0;
    G<uint32_t>(kSessionHost) = 0;
    G<uint32_t>(kNetReady) = 0;
    G<uint32_t>(kMpGame) = 0;
    G<uint32_t>(kMpLevel) = 0;
    G<uint32_t>(kMpState) = kMpStateIdle;
    G<uint32_t>(kMpPendingState) = kMpStateIdle;
    g_session = false;
    g_players = 0;
    dslog::Write("Versus: session closed");
}

// Rematch: the same level loaded again like MP state 4 does it (FUN_0045fef0): next-level name, FUN_004576c0, load
// flag, loading-screen set-up; the session (scores) is opened afresh on the pending load.
void Rematch() {
    char* next = G<char*>(kNextLevel);
    wsprintfA(next, "MP_Mission%d.dll", G<uint8_t>(kMap) + 1);
    reinterpret_cast<void(__fastcall*)(void*)>(0x4576C0)(G<void*>(0x60EDC0));  // resets the game progress object
    G<uint8_t>(kLoadPending) = 1;
    reinterpret_cast<void(__cdecl*)()>(0x4BD000)();
    reinterpret_cast<void(__cdecl*)(int, int, int, int)>(0x4BCF70)(0, 0, 1, 1);
    G<uint32_t>(kMpState) = kMpStateInGame;
    G<uint32_t>(kMpPendingState) = kMpStateInGame;
    dslog::Write("Versus: rematch (%s)", next);
}

void* ListObject(uint32_t index, int list) {
    return reinterpret_cast<void*(__cdecl*)(uint32_t, int)>(kListObject)(index, list);
}
}  // namespace

// ---- scores screen (front-end state 0x35 at the match end) ----
// The game's scores layout is made for 800x600 (W' = max(800, W) used both as the HUD scale and in raw pixels): on a
// wide screen its columns crowded the left of the frame and the names started outside it. In a versus match its
// pieces in the front-end draw FUN_0047a4a0 are replaced (the draw itself also runs the menu transitions, so it stays):
// the panel box (0x47BD1C, FUN_0047e640(x, y, w, h, style, alpha)) draws our scoreboard instead, the outline box
// (0x47C186), the columns (0x47C18E, FUN_0046b190) and the score list (0x47C193, [0x61132C] vtable +8) are skipped.
static constexpr uint32_t kBox = 0x47E640, kScoreColumns = 0x46B190, kScoreList = 0x61132C;
static constexpr uint32_t kPanelBoxCall = 0x47BD1C, kOutlineBoxCall = 0x47C186, kColumnsCall = 0x47C18E;
static constexpr uint32_t kListDrawSite = 0x47C193;  // mov ecx, [0x61132C]; mov edx, [ecx]; call [edx+8] (11 bytes)
static constexpr uint8_t kListDrawCode[] = {0x8B, 0x0D, 0x2C, 0x13, 0x61, 0x00, 0x8B, 0x11, 0xFF, 0x52, 0x08};

static bool OurScores() { return g_session && G<uint32_t>(kFrontEndState) == kStateScores; }

static const char* TeamName(int team) {
    static const char* const names[] = {"SAS", "US DELTA", "IRAQI", "RUSSIAN"};
    const int u = G<uint8_t>(kTeamUniform + (team ? 1 : 0));
    return u >= 0 && u < 4 ? names[u] : "?";
}

static void Box(int x, int y, int w, int h, int style, int alpha) {
    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, int, int)>(kBox)(
        static_cast<uint32_t>(std::max(0, x)), static_cast<uint32_t>(std::max(0, y)), static_cast<uint32_t>(w),
        static_cast<uint32_t>(h), style, alpha);
}

static void DrawScores() {
    auto* r = G<uint8_t*>(0x63C924);
    auto* sheet = G<uint8_t*>(0x60EE18);
    void* small = overlay::MenuFont();
    void* large = G<void*>(0x60EDB8);
    if (!r || !sheet || !small || !large) return;
    const int W = At<int>(r, 0x40688), H = At<int>(r, 0x4068C);
    const float k = At<float>(sheet, 0x24);
    auto px = [k](float design) { return static_cast<int>(design * k + 0.5f); };

    struct Row {
        int player, team, kills, deaths, skill;
    } rows[8];
    int n = 0;
    for (int i = 0; i < g_players && i < 8; ++i) {
        const uint8_t* e = reinterpret_cast<const uint8_t*>(kPlayerTable + i * kPlayerEntry);
        rows[n++] = {i, e[0x15], At<int32_t>(const_cast<uint8_t*>(e), 0x20), At<int32_t>(const_cast<uint8_t*>(e), 0x1C),
                     e[0x16]};
    }
    const bool teams = G<uint8_t>(kGameType) != 2;  // 2 = deathmatch, every player for themselves
    std::sort(rows, rows + n, [teams](const Row& a, const Row& b) {
        if (teams && a.team != b.team) return a.team < b.team;
        if (a.kills != b.kills) return a.kills > b.kills;
        if (a.deaths != b.deaths) return a.deaths < b.deaths;
        return a.player < b.player;
    });

    // Result: team kills (team deathmatch) or the best player.
    char result[96];
    if (teams) {
        int score[2] = {};
        for (int i = 0; i < n; ++i) score[rows[i].team & 1] += rows[i].kills;
        if (score[0] == score[1]) wsprintfA(result, "DRAW  %d - %d", score[0], score[1]);
        else wsprintfA(result, "%s WIN  %d - %d", TeamName(score[0] > score[1] ? 0 : 1), score[0], score[1]);
    } else {
        const bool tie = n > 1 && rows[0].kills == rows[1].kills && rows[0].deaths == rows[1].deaths;
        if (tie) wsprintfA(result, "DRAW");
        else wsprintfA(result, "PLAYER %d WINS", rows[0].player + 1);
    }

    const int lineH = std::max(px(24), overlay::TextWidth(small, "M") * 2);
    const int pw = std::min(px(680), W - px(40));
    const int x0 = (W - pw) / 2;
    // Rows inside the outline with room above the first and below the last (and at the sides) - the outline's
    // corners are rounded: header, outline top half a line below it, first row 1.75 lines below (~0.8 line of room
    // over its capitals), outline bottom 0.8 lines under the last row.
    const int listTop = px(116);
    const int firstRow = lineH * 175 / 100, below = lineH * 80 / 100;
    const int ph = listTop + firstRow + lineH * (n - 1) + below + px(30);
    const int y0 = std::max(px(120), (H - ph) / 2);
    Box(x0, y0, pw, ph, 1, 6);

    const uint32_t head = 0xFFD8C878;
    const char* mode = teams ? "TEAM DEATHMATCH" : "DEATHMATCH";
    overlay::DrawLabel(small, mode, x0 + (pw - overlay::TextWidth(small, mode)) / 2, y0 + px(34), head);
    const int rw = overlay::TextWidth(large, result);
    overlay::DrawLabel(large, result, x0 + (pw - rw) / 2, y0 + px(72), 0xFFFFFFFF);

    // Columns: PLAYER, TEAM (left-aligned), KILLS, DEATHS (right-aligned).
    const int cName = x0 + px(60), cTeam = x0 + px(215), cClass = x0 + px(335), cKills = x0 + pw - px(160),
              cDeaths = x0 + pw - px(60);
    static const char* const classes[] = {"RIFLEMAN", "HEAVY WEAPONS", "SNIPER", "ENGINEER"};  // character skill
    int y = y0 + listTop;
    auto right = [small](const char* t, int x, int yy, uint32_t c) {
        overlay::DrawLabel(small, t, x - overlay::TextWidth(small, t), yy, c);
    };
    overlay::DrawLabel(small, "PLAYER", cName, y, head);
    if (teams) overlay::DrawLabel(small, "TEAM", cTeam, y, head);
    overlay::DrawLabel(small, "CLASS", cClass, y, head);
    right("KILLS", cKills, y, head);
    right("DEATHS", cDeaths, y, head);
    const int outlineTop = y + lineH / 2;
    Box(x0 + px(20), outlineTop, pw - px(40), y + firstRow + lineH * (n - 1) + below - outlineTop, 0, 6);
    y += firstRow - lineH;
    for (int i = 0; i < n; ++i) {
        y += lineH;
        const uint32_t c = !teams ? 0xFFFFFFFF : rows[i].team ? 0xFFFF9080 : 0xFF90B8FF;
        char t[32];
        wsprintfA(t, "PLAYER %d", rows[i].player + 1);
        overlay::DrawLabel(small, t, cName, y, c);
        if (teams) overlay::DrawLabel(small, TeamName(rows[i].team), cTeam, y, c);
        overlay::DrawLabel(small, classes[rows[i].skill & 3], cClass, y, c);
        wsprintfA(t, "%d", rows[i].kills);
        right(t, cKills, y, c);
        wsprintfA(t, "%d", rows[i].deaths);
        right(t, cDeaths, y, c);
    }

    // Prompts under the panel (the game's prompt line belongs to its panel): every device's button, then the action.
    using overlay::Icon;
    const int icon = std::max(16, overlay::TextWidth(small, "M") * 3 / 2);
    const int gap = px(40);
    const char* acts[2] = {"Rematch", "Leave"};
    const Icon icons[2][3] = {{Icon::PsCross, Icon::XbCross, Icon::KbEnter}, {Icon::PsCircle, Icon::XbCircle, Icon::KbEscape}};
    int total = gap;
    for (const char* a : acts) total += 3 * icon + icon / 4 + overlay::TextWidth(small, a);
    int x = (W - total) / 2;
    const int baseline = y0 + ph + px(36);
    for (int a = 0; a < 2; ++a) {
        for (Icon ic : icons[a]) {
            overlay::QueueIcon(ic, static_cast<float>(x), static_cast<float>(baseline - icon * 3 / 4), static_cast<float>(icon));
            x += icon;
        }
        x += icon / 4;
        overlay::DrawLabel(small, acts[a], x, baseline, 0xFFFFFFFF);
        x += overlay::TextWidth(small, acts[a]) + gap;
    }
}

static void __cdecl PanelBox(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int style, int alpha) {
    if (OurScores()) return DrawScores();
    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, int, int)>(kBox)(x, y, w, h, style, alpha);
}
static void __cdecl OutlineBox(uint32_t x, uint32_t y, uint32_t w, uint32_t h, int style, int alpha) {
    if (OurScores()) return;
    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, int, int)>(kBox)(x, y, w, h, style, alpha);
}
static void __cdecl ScoreColumns() {
    if (!OurScores()) reinterpret_cast<void(__cdecl*)()>(kScoreColumns)();
}
static void __cdecl ScoreList() {
    if (OurScores()) return;
    void* list = G<void*>(kScoreList);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
}

static void HookScores() {
    bool ok = patch::HookCall(kPanelBoxCall, reinterpret_cast<const void*>(&PanelBox), kBox) &&
              patch::HookCall(kOutlineBoxCall, reinterpret_cast<const void*>(&OutlineBox), kBox) &&
              patch::HookCall(kColumnsCall, reinterpret_cast<const void*>(&ScoreColumns), kScoreColumns);
    if (ok && patch::Matches(kListDrawSite, kListDrawCode, sizeof kListDrawCode)) {
        uint8_t code[sizeof kListDrawCode];
        std::memset(code, 0x90, sizeof code);
        code[0] = 0xE8;
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&ScoreList) - (kListDrawSite + 5));
        std::memcpy(code + 1, &rel, 4);
        patch::Write(kListDrawSite, code, sizeof code);
    } else if (!(ok && patch::Matches(kListDrawSite, "\xE8", 1))) {
        dslog::Write("[fail] Versus: scores screen draw not recognised - the game's scores stay");
    }
}

// The scores screen's prompt ("Esc: Main Menu", hash 0x2FAE9B7A) names our two actions in a versus match.
const char* VersusScoresPrompt() {
    if (!OurScores()) return nullptr;
    static std::string text;
    using overlay::Icon;
    text = overlay::GlyphsReady()
               ? std::string(overlay::IconChar(Icon::PsCross)) + overlay::IconChar(Icon::XbCross) +
                     overlay::IconChar(Icon::KbEnter) + " Rematch     " + overlay::IconChar(Icon::PsCircle) +
                     overlay::IconChar(Icon::XbCircle) + overlay::IconChar(Icon::KbEscape) + " Leave"
               : "Enter: Rematch     Esc: Leave";
    return text.c_str();
}

// Respawn (game event kind 3, FUN_00466460 from the client handler at 0x465C57): its local-player tail re-attaches
// block 0 and flashes view 0 white (FUN_004d2c30(view controller, 750 ms, 200, 200, 200)). The soldier object is
// reused, so other players' blocks stay attached - they only get the flash on their own view here.
static constexpr uint32_t kRespawnCall = 0x465C57, kRespawn = 0x466460, kFlash = 0x4D2C30, kViewControllers = 0x63C990;
static void __cdecl RespawnHook(uint8_t* ev) {
    reinterpret_cast<void(__cdecl*)(void*)>(kRespawn)(ev);
    if (!g_session || !ev || At<uint32_t>(ev, 8)) return;  // +8 set: a request, the handler only forwards it
    const uint32_t player = At<uint32_t>(ev, 0xC);
    if (player == G<uint32_t>(kLocalPlayer) || player >= static_cast<uint32_t>(g_players)) return;
    const uint32_t view = G<uint32_t>(kInputBlocks + player * kInputBlockSize + 0x474);
    if (void* vc = G<void*>(kViewControllers + view * 4))
        reinterpret_cast<void(__thiscall*)(void*, int, int, int, int)>(kFlash)(vc, 0x2EE, 200, 200, 200);
}

// Time limit getter FUN_00461060 (only called by the level scripts): (option [0x5EBBA6] + 1) x 5 min, 25 min = none
// (-1). Replaced by the same formula; dev builds can shorten it (Dev\VersusTimeLimitMs) to test the match end.
static constexpr uint32_t kTimeLimit = 0x461060;
static constexpr uint8_t kTimeLimitEntry[] = {0x33, 0xC0, 0xA0, 0xA6, 0xBB, 0x5E, 0x00};
static int32_t __cdecl TimeLimitMs() {
#ifndef DS_DIST
    DWORD v = 0, size = sizeof v;
    if (g_session && RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VersusTimeLimitMs", RRF_RT_REG_DWORD,
                                  nullptr, &v, &size) == ERROR_SUCCESS && v)
        return static_cast<int32_t>(v);
#endif
    const int32_t ms = (G<uint8_t>(0x5EBBA6) + 1) * 300000;
    return ms == 1500000 ? -1 : ms;
}

// "Is this soldier a network client's?" FUN_00460960(soldier): during a match, on the host, every MP soldier but the
// local player's (list index != [0x610F84]) - those are driven by the input their client sends (FUN_0042a0xx skips
// the local controller FUN_00425df0), so split-screen players 2-4 could walk but not turn or fire. In a local
// session every player is at this PC: replaced (entry jump) - nobody is remote; otherwise the original logic.
static constexpr uint32_t kIsRemoteSoldier = 0x460960;
static constexpr uint8_t kIsRemoteEntry[] = {0xE8, 0x6B, 0xF5, 0xFF, 0xFF};  // call 0x45fed0
static int __cdecl IsRemoteSoldier(void* soldier) {
    if (g_session) return 0;
    if (reinterpret_cast<int(__cdecl*)()>(0x45FED0)() != 7) return 0;
    const int index = reinterpret_cast<int(__cdecl*)(void*, int)>(0x4607E0)(soldier, 1);
    return reinterpret_cast<int(__cdecl*)()>(0x4608F0)() != 0 && index != static_cast<int>(G<uint32_t>(kLocalPlayer));
}

// Soldier skin + kit FUN_00435ef0 (thiscall soldier, role, side, player index): the game re-applies it from the
// player's team at every (re)spawn - local set-up 0x45F4C9, respawn handler 0x46659E, player joined 0x46707A. In a
// deathmatch the side is the player's random faction instead.
static constexpr uint32_t kSkinCalls[] = {0x45F4C9, 0x46659E, 0x46707A};
static void __fastcall SkinHook(void* soldier, void*, int role, int side, uint32_t player) {
    if (g_session && G<uint8_t>(kGameType) == 2 && player < 8)
        side = reinterpret_cast<int(__cdecl*)(uint32_t)>(kUniformSide)(g_dmUniform[player]);
    reinterpret_cast<void(__thiscall*)(void*, int, int, uint32_t)>(kSoldierSkin)(soldier, role, side, player);
}

bool features::VersusSession() { return g_session; }

static void HookPumps() {
    const void* stubs[] = {reinterpret_cast<const void*>(&PumpStub<0x5628C0>),
                           reinterpret_cast<const void*>(&PumpStub<0x562930>),
                           reinterpret_cast<const void*>(&PumpStub<0x5628F0>)};
    for (int i = 0; i < 3; ++i)
        if (!patch::HookCall(kPumps[i].site, stubs[i], kPumps[i].target))
            dslog::Write("[fail] Versus: net pump call at 0x%08X not recognised", kPumps[i].site);
    HookScores();
    for (uint32_t site : kSkinCalls)
        if (!patch::HookCall(site, reinterpret_cast<const void*>(&SkinHook), kSoldierSkin))
            dslog::Write("[fail] Versus: soldier skin call at 0x%08X not recognised", site);
    if (patch::Matches(kIsRemoteSoldier, kIsRemoteEntry, sizeof kIsRemoteEntry))
        patch::WriteJump(kIsRemoteSoldier, reinterpret_cast<const void*>(&IsRemoteSoldier));
    else
        dslog::Write("[fail] Versus: remote soldier check at 0x%08X not recognised", kIsRemoteSoldier);
    if (!patch::HookCall(kServerCloseCall, reinterpret_cast<const void*>(&ServerClose), kServerClose))
        dslog::Write("[fail] Versus: server close call at 0x%08X not recognised", kServerCloseCall);
    if (!patch::HookCall(kRespawnCall, reinterpret_cast<const void*>(&RespawnHook), kRespawn))
        dslog::Write("[fail] Versus: respawn call at 0x%08X not recognised", kRespawnCall);
    if (patch::Matches(kTimeLimit, kTimeLimitEntry, sizeof kTimeLimitEntry))
        patch::WriteJump(kTimeLimit, reinterpret_cast<const void*>(&TimeLimitMs));
    else
        dslog::Write("[fail] Versus: time limit getter at 0x%08X not recognised", kTimeLimit);
    if (patch::Matches(kHostSend, kHostSendEntry, sizeof kHostSendEntry))
        patch::WriteJump(kHostSend, reinterpret_cast<const void*>(&HostSend));
    else
        dslog::Write("[fail] Versus: host send at 0x%08X not recognised", kHostSend);
    if (patch::Matches(kClientSend, kHostSendEntry, sizeof kHostSendEntry))  // same first instruction
        patch::WriteJump(kClientSend, reinterpret_cast<const void*>(&ClientSend));
    else
        dslog::Write("[fail] Versus: client send at 0x%08X not recognised", kClientSend);
}

// Called by splitscreen.cpp's AssignPlayers after the level's player set-up (which cleared every block's soldier):
// player i gets MP soldier i - set up like the host's own (FUN_0045f410, which already ran for player 1 during
// level init): activated, announced to the script (event 0x35 {id, -1, session time}), skin / kit of its team.
void features::VersusAssignPlayers() {
    if (!g_session) return;
    using Attach = void(__thiscall*)(uintptr_t, void*);
    for (int i = 0; i < g_players; ++i) {
        void* soldier = ListObject(static_cast<uint32_t>(i), 1);
        if (!soldier) {
            dslog::Write("[fail] Versus: no MP soldier for player %d", i + 1);
            continue;
        }
        if (i > 0) {
            reinterpret_cast<void(__thiscall*)(void*, int)>((*reinterpret_cast<void***>(soldier))[1])(soldier, 1);
            const int32_t id = At<int32_t>(soldier, 8);
            if (void* script = G<void*>(kScript)) {
                int32_t args[3] = {id, -1, reinterpret_cast<int32_t(__cdecl*)()>(kSessionTime)()};
                reinterpret_cast<void(__cdecl*)(int, void*)>(At<void*>(script, 8))(0x35, args);
            }
        }
        reinterpret_cast<Attach>(kAttachSoldier)(kInputBlocks + i * kInputBlockSize, soldier);
        // Like FUN_0045f410 does for the host's own soldier: experience level 7 (FUN_0042d1b0 -> soldier +0x2A18).
        if (i > 0) reinterpret_cast<void(__thiscall*)(void*, int)>(kSetExperience)(soldier, 7);
        At<int32_t>(soldier, 0x2B00) = i;  // owner: each view's soldier panel shows only that player's soldier
        const bool deathmatch = G<uint8_t>(kGameType) == 2;
        if (i > 0 || deathmatch) {  // player 1's skin comes from the game's own set-up - except deathmatch factions
            const int32_t id = At<int32_t>(soldier, 8);
            const uint8_t team = reinterpret_cast<uint8_t(__cdecl*)(int32_t)>(kTeamOf)(id);
            const uint8_t skill = G<uint8_t>(kPlayerTable + i * kPlayerEntry + 0x16);
            const int role = reinterpret_cast<int(__cdecl*)(uint32_t)>(kRoleOf)(skill);
            const int side = deathmatch ? reinterpret_cast<int(__cdecl*)(uint32_t)>(kUniformSide)(g_dmUniform[i])
                                        : reinterpret_cast<int(__cdecl*)(uint32_t)>(kSideOf)(team);
            using Skin = void(__thiscall*)(void*, int, int, uint32_t);
            reinterpret_cast<Skin>(kSoldierSkin)(soldier, role, side, static_cast<uint32_t>(i));
        }
        dslog::Write("Versus: player %d -> soldier %p (id %d, team %u)", i + 1, soldier, At<int32_t>(soldier, 8),
                     G<uint8_t>(kPlayerTable + i * kPlayerEntry + 0x15));
    }
    // A rematch is loaded from the scores screen, not by the front-end, so nothing leaves that screen: back to play.
    if (G<uint32_t>(kFrontEndState) == kStateScores) SetState(0xE);
    // The skins only register their textures; the level load's batch already ran (else the other team is white).
    using LoadPending = void(__thiscall*)(void*, void*, int);
    reinterpret_cast<LoadPending>(kTextureBatch)(G<void*>(kTextureManager), nullptr, 0);
}

void features::OnFrameVersus() {
    static bool tried = false;
    if (!tried && *reinterpret_cast<void**>(kMainMenuScreen)) {
        tried = true;
        Build();
        BuildSetup();
    }
#ifndef DS_DIST
    // Dev: F6 in a versus match = player 1 kills player 2 (the soldier's damage method, vtable +0x3C (amount,
    // attacker), with twice its health like the debug command "Kill the selected Grunt" FUN_00444d80).
    static bool f6 = false;
    const bool down = features::KeyHeld(VK_F6) && features::GameFocused();
    if (down && !f6 && g_session && g_players >= 2) {
        const bool reverse = features::KeyHeld(VK_SHIFT);  // Shift+F6: player 2 kills player 1
        void* killer = ListObject(reverse ? 1 : 0, 1);
        void* victim = ListObject(reverse ? 0 : 1, 1);
        if (killer && victim) {
            const float damage = At<float>(victim, 0x54) * 2.0f;
            dslog::Write("[dev]  Versus: player %d kills player %d (health %.1f)", reverse ? 2 : 1, reverse ? 1 : 2,
                         At<float>(victim, 0x54));
            reinterpret_cast<void(__thiscall*)(void*, float, void*)>((*reinterpret_cast<void***>(victim))[0x3C / 4])(
                victim, damage, killer);
            dslog::Write("[dev]  Versus: %u messages looped back so far", g_loopedBack);
            dslog::Write("[dev]  Versus: damage method %p, health now %.1f, +0x25d4 %d",
                         (*reinterpret_cast<void***>(victim))[0x3C / 4], At<float>(victim, 0x54),
                         At<int32_t>(victim, 0x25D4));
        }
    }
    f6 = down;
#endif
    // Scores screen at the match end (front-end state 0x35, MP state 8 - the network game waited for the host's lobby
    // here): Accept (Enter / pad button 2) = rematch, the same level again; Back (Esc / pad button 1) = MP state 0xB,
    // the game's own way out (FUN_0045fc20 ends the session, FrontEnd loads, main menu).
    static bool scoresHeld = true;
    if (g_session && G<uint32_t>(kFrontEndState) == kStateScores && G<uint32_t>(kMpState) == 8 &&
        features::GameFocused()) {
        uint32_t pad = 0;
        const int count = static_cast<int>(G<uint32_t>(kJoystickCount));
        for (int j = 0; j < count && j < 8; ++j) pad |= features::ReadJoystick(j);
        const bool accept = features::KeyHeld(VK_RETURN) || (pad & (1u << 2));
        const bool back = features::KeyHeld(VK_ESCAPE) || (pad & (1u << 1));
        if (!accept && !back) {
            scoresHeld = false;
        } else if (!scoresHeld) {
            scoresHeld = true;
            if (back) {
                G<uint32_t>(kMpPendingState) = kMpStateQuit;
                dslog::Write("Versus: leaving the match");
            } else {
                Rematch();
            }
        }
    } else {
        scoresHeld = true;  // a button held when the screen appears doesn't count
    }

    // SCORE LIMIT: once a player (deathmatch) or a team (team deathmatch) has that many kills, the match ends the way
    // the level scripts end it at the time limit - script API +0x7A8 = FUN_004638f0(reason): a game event broadcast
    // (0x3C draw, 0x3D team 1 won, 0x3E team 2 won) that takes everyone to the scores screen.
    if (g_session && g_scoreLimit > 0 && !g_limitReached && G<uint32_t>(kMpState) == kMpStateInGame &&
        G<uint32_t>(kFrontEndState) == 0xE && G<uint8_t>(kLoadPending) == 0) {
        int team[2] = {}, best = 0, bestPlayer = 0;
        for (int i = 0; i < g_players && i < 8; ++i) {
            const uint8_t* e = reinterpret_cast<const uint8_t*>(kPlayerTable + i * kPlayerEntry);
            const int kills = *reinterpret_cast<const int32_t*>(e + 0x20);
            if (e[0x15] < 2) team[e[0x15]] += kills;
            if (kills > best) best = kills, bestPlayer = i;
        }
        const bool teams = G<uint8_t>(kGameType) != 2;
        const int top = teams ? std::max(team[0], team[1]) : best;
        if (top >= g_scoreLimit) {
            g_limitReached = true;
            const int reason = !teams ? 0x3C : team[0] == team[1] ? 0x3C : team[0] > team[1] ? 0x3D : 0x3E;
            dslog::Write("Versus: score limit %d reached (%s %d) - match over", g_scoreLimit,
                         teams ? "team" : "player", teams ? (team[0] >= team[1] ? 1 : 2) : bestPlayer + 1);
            reinterpret_cast<void(__cdecl*)(int)>(kEndMatch)(reason);
        }
    }

    // A match from MATCH SETUP ended (scores screen Back or the pause menu's MAIN MENU): back to VERSUS.
    // (after the FrontEnd load: the level-done code sets the main menu itself, so wait until it has been up a while)
    static int menuFrames = 0;
    const bool onMenu = G<uint32_t>(kFrontEndState) == kStateMainMenu && G<uint8_t>(kLoadPending) == 0;
    menuFrames = onMenu ? menuFrames + 1 : 0;
    if (g_backToVersus && menuFrames > 10 && G<uint32_t>(kStateBusy) == 0) {
        g_backToVersus = false;
        SetState(kStateVersus);
    }

    // On the frame a level load is pending: open the local session before an MP level, close it before anything else.
    if (G<uint8_t>(kLoadPending) != 1) return;
    const char* next = G<const char*>(kNextLevel);
    const int requested = Requested();  // dev boot: game type option
    if (IsMpLevel(next) && (requested > 0 || g_session || g_match.pending)) {
        if (g_match.pending) g_menuMatch = true;
        g_match.pending = false;
        const int gameType = requested > 0 && !g_menuMatch ? requested : kModeGameType[g_match.mode];
        OpenSession(gameType, next[10] >= '1' && next[10] <= '6' ? next[10] - '1' : 0,
                    std::max(2, features::SplitScreenPlayers()));
    } else if (g_session) {
        CloseSession();
    }
}

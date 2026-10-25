// Custom campaigns: Lua mission scripts (scripthost.cpp) played from the menus.
//
// A mod declares a campaign in Mods\<mod>\campaign.txt ("key = value" lines, # comments):
//     name = Operation Sandstorm
//     mission = sandstorm1.lua
//     mission = sandstorm2.lua
// SINGLE PLAYER (state 2; TRAINING 0x7E5, DESERT STORM CAMPAIGN 0x7E6, LOAD GAME 0x7E7) gets a fourth item CUSTOM
// CAMPAIGNS (0x7E8) when a campaign exists: the menu's list holds 3 items (push 3 at 0x48CC4D, mov edi,3 at
// 0x48CC5F - both 4 now), coop.cpp's set-up wrapper adds ours (features::CampaignsMenuItem), and the menu's update
// (vtable slot 0x5D93E0 = FUN_0048ce40) is replaced by a copy that also opens state 0x45. That screen (an Options
// sub-screen object, like versus.cpp's) lists the campaigns; picking one does what DESERT STORM CAMPAIGN does: the
// first mission into the next-level slot (*(char**)0x5E7028, a 256-byte buffer) and DIFFICULTY (state 5), whose
// accept starts the level (coop.cpp's DifficultyPicked). The scripts chain the missions: game.end_mission(2) without
// set_next_level continues with features::CampaignNext (the campaign's next mission, the front end after the last).
//
// Converted campaigns (another game's, made by the ModKit from the player's own copy: <game>\PSE\Ports\<id>\
// campaign.txt, same format; mods.cpp indexes their files): DESERT STORM CAMPAIGN reads CAMPAIGNS and opens the same
// screen listing DESERT STORM 1 (the game's own campaign) and each converted one (user decision 2026-10-24).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <utility>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kSetState = 0x47EF20, kBackPressed = 0x478380, kListSelected = 0x4E5D80, kStartupFlag = 0x4B9CD0;
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kItemCtor = 0x4E4F90, kItemSet = 0x4E4FB0, kDrawPanel = 0x456EB0, kDrawTitle = 0x456F40;
constexpr uint32_t kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kMainMenuScreen = 0x617A00;
constexpr uint32_t kMainMenuEnter = 0x4153B0, kOptCtor = 0x409B90, kOptVtable = 0x5D8510, kOptSetup = 0x409C90;
constexpr uint32_t kOptItemDrawCb = 0x409E90, kSpItemDrawCb = 0x48CF20;
constexpr uint32_t kSpCapacity = 0x48CC4E, kSpItemCount = 0x48CC60, kSpUpdateSlot = 0x5D93E0, kSpUpdate = 0x48CE40;
constexpr uint32_t kCampaign = 0x60EDC0, kNextLevel = 0x5E7028;
constexpr uint32_t kStateMainMenu = 1, kStateSp = 2, kStateDifficulty = 5, kStateLoad = 9, kStateTraining = 0x3D;
constexpr uint32_t kStateCampaigns = 0x45;
constexpr uint16_t kItemTraining = 0x7E5, kItemCampaign = 0x7E6, kItemLoad = 0x7E7, kItemCustom = 0x7E8;
constexpr uint16_t kItemFirstCampaign = 0x860;
constexpr int kMaxCampaigns = 8, kVtableEntries = 21;
// own text ids (0xC00E....): 0xC00A.... is the MODS menu's - sharing it put "CUSTOM CAMPAIGNS" over the mod list
enum : uint32_t { kTextCustom = 0xC00E0001, kTextCampaigns = 0xC00E0002, kTextFirstCampaign = 0xC00E0010 };

struct Campaign {
    std::string name, mod;
    std::vector<std::string> missions;
    bool original = false;   // the game's own campaign (CAMPAIGNS screen, first row)
};
std::vector<Campaign> g_campaigns;   // custom: Mods\<mod>\campaign.txt
std::vector<Campaign> g_ports;       // converted: PSE\Ports\<id>\campaign.txt
std::vector<Campaign> g_shown;       // the screen's rows: the custom ones, or DESERT STORM 1 + the converted ones
bool g_portsMode = false;
uint8_t g_screen[0x20];

// Converted campaigns read files of every Desert Storm level (their scenes reuse props, debris and animations from all
// of the game's archives; a level reads only its own archive) - shown only while the shared asset store serves every
// archive (SHARED ASSET STORE, on by default; user decision 2026-10-24).
bool PortsShown() {
    if (g_ports.empty()) return false;
    if (features::AssetStoreAllArchives()) return true;
    static bool told = false;
    if (!told) {
        told = true;
        features::ModLog("Campaign: %u converted campaign(s) hidden - they need SHARED ASSET STORE (OPTIONS > "
                         "DESERTSTORMFIX > PLUGINS)", static_cast<unsigned>(g_ports.size()));
    }
    return false;
}
void* g_vtable[kVtableEntries];
bool g_built = false;

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
void SetState(uint32_t s) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(s); }
void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }
void SetItem(void* item, uint16_t id, uint32_t hash, uint32_t drawCb) {
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    reinterpret_cast<Fn>(kItemSet)(item, id, hash, 0, 0x24, 0, 0, drawCb, 0);
}
void AddToList(void* list, void* item) {
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, item);
}
std::string Trim(std::string s) {
    const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    return dir.substr(0, dir.find_last_of("\\/"));
}

void LoadFrom(const std::string& root, std::vector<Campaign>& out) {
    out.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '_' || fd.cFileName[0] == '.') continue;
        FILE* f = nullptr;
        if (fopen_s(&f, (root + fd.cFileName + "\\campaign.txt").c_str(), "r") || !f) continue;
        Campaign c;
        c.mod = fd.cFileName;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            std::string s = line;
            if (const size_t hash = s.find('#'); hash != std::string::npos) s.resize(hash);
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(s.substr(0, eq)), value = Trim(s.substr(eq + 1));
            if (_stricmp(key.c_str(), "name") == 0) c.name = value;
            else if (_stricmp(key.c_str(), "mission") == 0 && !value.empty() && value.size() < 200) c.missions.push_back(value);
        }
        fclose(f);
        if (c.name.empty()) c.name = c.mod;
        for (char& ch : c.name) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));  // menu style
        if (c.missions.empty()) {
            features::ModLog("[fail] Campaign %s: campaign.txt lists no mission", c.mod.c_str());
        } else if (static_cast<int>(out.size()) < kMaxCampaigns - 1) {   // (the CAMPAIGNS screen adds DESERT STORM 1)
            features::ModLog("Campaign: %s (%s, %u missions)", c.name.c_str(), c.mod.c_str(),
                             static_cast<unsigned>(c.missions.size()));
            out.push_back(std::move(c));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void LoadCampaigns() {
    LoadFrom(GameDir() + "\\Mods\\", g_campaigns);
    LoadFrom(GameDir() + "\\PSE\\Ports\\", g_ports);
}

// The screen's rows for a mode: the custom campaigns, or DESERT STORM 1 + the converted ones.
void ShowMode(bool ports) {
    g_portsMode = ports;
    g_shown.clear();
    if (ports) {
        Campaign own;
        own.name = "DESERT STORM 1";
        own.original = true;
        g_shown.push_back(own);
        g_shown.insert(g_shown.end(), g_ports.begin(), g_ports.end());
    } else {
        g_shown = g_campaigns;
    }
}

// ---- CUSTOM CAMPAIGNS screen (state 0x45) ----
int Rows() { return static_cast<int>(g_shown.size()) < 2 ? 2 : static_cast<int>(g_shown.size()); }
// The list's item pool: room for every campaign there may be after mods are switched on in game (MODS menu, an online
// host's mods) - the screen is built once.
int PoolRows() { return kMaxCampaigns < 2 ? 2 : kMaxCampaigns; }

bool __fastcall Create(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, *reinterpret_cast<void**>(kLargeFont), *reinterpret_cast<void**>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, PoolRows())) return false;
    for (int i = 0; i < PoolRows(); ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x24, 0x1E);
        if (item) reinterpret_cast<void*(__thiscall*)(void*)>(kItemCtor)(item);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    }
    return true;
}

// The class's set-up lays out two rows (items 0 and 1); the others are added and the area grown (list +0x38 row
// pitch, +0x40 area height, +0x42 visible rows, +0x26 count) - or shrunk for a single campaign.
void __fastcall Setup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);
    void* list = At<void*>(screen, 8);
    const int n = static_cast<int>(g_shown.size());
    for (int i = 0; i < n; ++i) SetItem(ItemAt(list, i), static_cast<uint16_t>(kItemFirstCampaign + i), kTextFirstCampaign + i, kOptItemDrawCb);
    for (int i = 2; i < n; ++i) AddToList(list, ItemAt(list, i));
    const int16_t pitch = At<int16_t>(list, 0x38);
    if (n == 1) {
        At<int16_t>(list, 0x26) = 1;
        At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) - pitch);
    } else if (n > 2) {
        At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (n - 2) * pitch);
    }
    At<int16_t>(list, 0x42) = static_cast<int16_t>(n < 1 ? 1 : n);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, g_portsMode ? kTextCampaigns : kTextCustom,
                                                                       0, 0, 0x7FFF);
}

int __fastcall Update(void* screen, void*) {
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(At<void*>(screen, 8));
    const int index = id - kItemFirstCampaign;
    if (index >= 0 && index < static_cast<int>(g_shown.size())) {
        const Campaign& c = g_shown[index];
        // the game's own campaign: what DESERT STORM CAMPAIGN did (its first level, campaign [0x60EDC0] +0x37)
        const char* first = c.original ? *reinterpret_cast<char**>(kCampaign) + 0x37 : c.missions[0].c_str();
        strcpy_s(*reinterpret_cast<char**>(kNextLevel), 256, first);
        dslog::Write("Campaign: %s starts with %s", c.name.c_str(), first);
        SetState(kStateDifficulty);
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateSp);
    return 0;
}

void __fastcall Draw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, g_portsMode ? kTextCampaigns : kTextCustom,
                                                                           1);
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
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
    dslog::Write(g_built ? "Campaign: screen built" : "[fail] Campaign: screen not built");
}

// ---- SINGLE PLAYER menu ----
// FUN_0048ce40 (the menu's update) with CUSTOM CAMPAIGNS: dev start-up flag u -> TRAINING list (0x3D); TRAINING ->
// 0x3D; DESERT STORM CAMPAIGN -> the campaign's first level ([0x60EDC0] +0x37) into the next-level slot, DIFFICULTY;
// LOAD GAME -> 9; Back -> main menu.
int __fastcall SpUpdate(void* screen, void*) {
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(At<void*>(screen, 8));
    if (reinterpret_cast<int(__cdecl*)(int)>(kStartupFlag)('u') == 1) {
        SetState(kStateTraining);
        return 0;
    }
    if (id == kItemTraining) SetState(kStateTraining);
    if (id == kItemCampaign && PortsShown() && g_built) {   // CAMPAIGNS: DESERT STORM 1 + the converted ones
        ShowMode(true);
        SetState(kStateCampaigns);
    } else if (id == kItemCampaign) {
        const char* first = *reinterpret_cast<char**>(kCampaign) + 0x37;
        strcpy_s(*reinterpret_cast<char**>(kNextLevel), 256, first);
        SetState(kStateDifficulty);
    }
    if (id == kItemLoad) SetState(kStateLoad);
    if (id == kItemCustom) {
        ShowMode(false);
        SetState(kStateCampaigns);
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateMainMenu);
    return 0;
}
}  // namespace

// coop.cpp's SINGLE PLAYER set-up wrapper, after the original (3 items in the pool of 4): CUSTOM CAMPAIGNS as the
// fourth row when a campaign exists. FeatCustomMissions is read here, every time the menu is entered (live): off = the
// game's three items (the update copy and the 4-item capacity stay installed - they behave like the original then).
static bool CustomMissionsOn() { return settings::Get().featCustomMissions; }

// Also from the co-op layout (split screen and online sessions, after coop.cpp rotated TRAINING out: pool = DESERT
// STORM CAMPAIGN, LOAD GAME, TRAINING (hidden), spare; 2 rows): CUSTOM CAMPAIGNS takes TRAINING's place as the third
// row, TRAINING moves behind it.
void features::CampaignsMenuItem(void* screen, bool coop) {
    if (!g_built) return;
    void* list = At<void*>(screen, 8);
    const int16_t rows = coop ? 2 : 3;
    if (At<int16_t>(list, 0x24) < 4 || At<int16_t>(list, 0x26) != rows) return;  // capacity patch missing / other layout
    void** items = At<void**>(list, 0x2C);
    // converted campaigns: DESERT STORM CAMPAIGN reads CAMPAIGNS
    void* campaign = items[coop ? 0 : 1];
    if (PortsShown() && campaign) SetItem(campaign, kItemCampaign, kTextCampaigns, kSpItemDrawCb);
    if (g_campaigns.empty() || !CustomMissionsOn()) return;
    if (coop) std::swap(items[2], items[3]);
    void* item = items[rows];
    if (!item) return;
    SetItem(item, kItemCustom, kTextCustom, kSpItemDrawCb);
    AddToList(list, item);
    if (coop) At<int16_t>(list, 0x26) = 3;   // (the pool already held the item: AddToList may not count it again)
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = static_cast<int16_t>(rows + 1);
    // The panel (screen +0x10..+0x1C) is laid out around the list by vtable +0x50, which the original set-up called
    // for three rows (title SINGLE PLAYER 0xA08F3D1B; CO-OPERATIVE CAMPAIGN in a session through the Lookup hook) -
    // again with the same arguments, then the list's own layout.
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, 0xA08F3D1B, 0, 0, 0x7FFF);
    reinterpret_cast<void(__thiscall*)(void*)>(0x4E8130)(list);
}

// The next-level slot holds a custom / converted campaign's mission (picked on the campaigns screen): co-op starts it
// at once - no co-op mission list, no "3-4 players start in Mission 3" rule (both are the game's own campaign's).
bool features::CampaignLevelPicked() {
    const char* next = *reinterpret_cast<const char**>(kNextLevel);
    if (!next || !*next) return false;
    for (const std::vector<Campaign>* list : {&g_campaigns, &g_ports})
        for (const Campaign& c : *list)
            for (const std::string& m : c.missions)
                if (_stricmp(m.c_str(), next) == 0) return true;
    return false;
}

void* features::CampaignsScreen() { return g_built && (CustomMissionsOn() || PortsShown()) ? g_screen : nullptr; }

const char* features::CampaignsText(uint32_t hash) {
    if (hash == kTextCustom) return "CUSTOM CAMPAIGNS";
    if (hash == kTextCampaigns) return "CAMPAIGNS";
    if (hash >= kTextFirstCampaign && hash < kTextFirstCampaign + g_shown.size())
        return g_shown[hash - kTextFirstCampaign].name.c_str();
    return nullptr;
}

// The level after `level` in the campaign that has it; the front end after its last mission (or for a level that is in
// no campaign).
const char* features::CampaignNext(const char* level) {
    for (const std::vector<Campaign>* list : {&g_campaigns, &g_ports})
        for (const Campaign& c : *list)
            for (size_t i = 0; i < c.missions.size(); ++i)
                if (_stricmp(c.missions[i].c_str(), level) == 0)
                    return i + 1 < c.missions.size() ? c.missions[i + 1].c_str() : "FrontEnd.dll";
    return "FrontEnd.dll";
}

// A level of a converted campaign that is shown (the loadout screen opens before its intro, its vehicles can be
// swapped - customise.cpp / vehicles.cpp).
bool features::CampaignPortLevel(const char* level) {
    if (!level || !PortsShown()) return false;
    for (const Campaign& c : g_ports)
        for (const std::string& m : c.missions)
            if (_stricmp(m.c_str(), level) == 0) return true;
    return false;
}

void features::ApplyCampaigns() {
    if (features::ModsFolderEnabled()) LoadCampaigns();   // campaigns are Mods folders (FeatMods off: none)
    const uint8_t three = 3, four = 4;
    const uint32_t three32 = 3, four32 = 4;
    if (!patch::Matches(kSpCapacity, &three, 1) || !patch::Matches(kSpItemCount, &three32, 4) ||
        !patch::HookSlot(kSpUpdateSlot, reinterpret_cast<const void*>(&SpUpdate), nullptr, kSpUpdate)) {
        dslog::Write("[fail] Campaign: SINGLE PLAYER menu not recognised");
        return;
    }
    patch::Write(kSpCapacity, &four, 1);
    patch::Write(kSpItemCount, &four32, 4);
    dslog::Write(CustomMissionsOn() ? "[ok]   Campaign: CUSTOM CAMPAIGNS - %u custom campaign(s) (live setting)"
                                    : "[off]  Campaign: CUSTOM CAMPAIGNS hidden (live setting; %u campaign(s) found)",
                 static_cast<unsigned>(g_campaigns.size()));
    if (!g_ports.empty())
        dslog::Write("[ok]   Campaign: CAMPAIGNS - Desert Storm 1 + %u converted campaign(s)",
                     static_cast<unsigned>(g_ports.size()));
}

void features::OnFrameCampaigns() {
    static bool tried = false;
    if (!tried && *reinterpret_cast<void**>(kMainMenuScreen)) {
        tried = true;
        Build();
    }
}

// Mods switched on / off in game (mods.cpp ReloadMods): the campaign list again (the screen lists it on every entry).
void features::ReloadCampaigns() {
    if (!features::ModsFolderEnabled()) return;
    LoadCampaigns();
    dslog::Write("Campaign: %u custom / %u converted campaign(s) after the mods reload",
                 static_cast<unsigned>(g_campaigns.size()), static_cast<unsigned>(g_ports.size()));
}

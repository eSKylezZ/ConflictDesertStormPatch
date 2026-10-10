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
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
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
enum : uint32_t { kTextCustom = 0xC00E0001, kTextFirstCampaign = 0xC00E0010 };

struct Campaign {
    std::string name, mod;
    std::vector<std::string> missions;
};
std::vector<Campaign> g_campaigns;
uint8_t g_screen[0x20];
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

void LoadCampaigns() {
    g_campaigns.clear();
    const std::string root = GameDir() + "\\Mods\\";
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
        } else if (static_cast<int>(g_campaigns.size()) < kMaxCampaigns) {
            features::ModLog("Campaign: %s (%s, %u missions)", c.name.c_str(), c.mod.c_str(),
                             static_cast<unsigned>(c.missions.size()));
            g_campaigns.push_back(std::move(c));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---- CUSTOM CAMPAIGNS screen (state 0x45) ----
int Rows() { return static_cast<int>(g_campaigns.size()) < 2 ? 2 : static_cast<int>(g_campaigns.size()); }

bool __fastcall Create(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, *reinterpret_cast<void**>(kLargeFont), *reinterpret_cast<void**>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, Rows())) return false;
    for (int i = 0; i < Rows(); ++i) {
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
    const int n = static_cast<int>(g_campaigns.size());
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
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kTextCustom, 0, 0, 0x7FFF);
}

int __fastcall Update(void* screen, void*) {
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(At<void*>(screen, 8));
    const int index = id - kItemFirstCampaign;
    if (index >= 0 && index < static_cast<int>(g_campaigns.size())) {
        const Campaign& c = g_campaigns[index];
        strcpy_s(*reinterpret_cast<char**>(kNextLevel), 256, c.missions[0].c_str());
        dslog::Write("Campaign: %s starts with %s", c.name.c_str(), c.missions[0].c_str());
        SetState(kStateDifficulty);
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateSp);
    return 0;
}

void __fastcall Draw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kTextCustom, 1);
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
    if (id == kItemCampaign) {
        const char* first = *reinterpret_cast<char**>(kCampaign) + 0x37;
        strcpy_s(*reinterpret_cast<char**>(kNextLevel), 256, first);
        SetState(kStateDifficulty);
    }
    if (id == kItemLoad) SetState(kStateLoad);
    if (id == kItemCustom) SetState(kStateCampaigns);
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateMainMenu);
    return 0;
}
}  // namespace

// coop.cpp's SINGLE PLAYER set-up wrapper, after the original (3 items in the pool of 4): CUSTOM CAMPAIGNS as the
// fourth row when a campaign exists.
void features::CampaignsMenuItem(void* screen) {
    if (g_campaigns.empty() || !g_built) return;
    void* list = At<void*>(screen, 8);
    if (At<int16_t>(list, 0x24) < 4 || At<int16_t>(list, 0x26) != 3) return;  // capacity patch missing / co-op layout
    void* item = ItemAt(list, 3);
    if (!item) return;
    SetItem(item, kItemCustom, kTextCustom, kSpItemDrawCb);
    AddToList(list, item);
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = 4;
    // The panel (screen +0x10..+0x1C) is laid out around the list by vtable +0x50, which the original set-up called
    // for three rows (title SINGLE PLAYER 0xA08F3D1B) - again with the same arguments, then the list's own layout.
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, 0xA08F3D1B, 0, 0, 0x7FFF);
    reinterpret_cast<void(__thiscall*)(void*)>(0x4E8130)(list);
}

void* features::CampaignsScreen() { return g_built ? g_screen : nullptr; }

const char* features::CampaignsText(uint32_t hash) {
    if (hash == kTextCustom) return "CUSTOM CAMPAIGNS";
    if (hash >= kTextFirstCampaign && hash < kTextFirstCampaign + g_campaigns.size())
        return g_campaigns[hash - kTextFirstCampaign].name.c_str();
    return nullptr;
}

// The level after `level` in the campaign that has it; the front end after its last mission (or for a level that is in
// no campaign).
const char* features::CampaignNext(const char* level) {
    for (const Campaign& c : g_campaigns)
        for (size_t i = 0; i < c.missions.size(); ++i)
            if (_stricmp(c.missions[i].c_str(), level) == 0)
                return i + 1 < c.missions.size() ? c.missions[i + 1].c_str() : "FrontEnd.dll";
    return "FrontEnd.dll";
}

void features::ApplyCampaigns() {
    LoadCampaigns();
    const uint8_t three = 3, four = 4;
    const uint32_t three32 = 3, four32 = 4;
    const uint32_t spUpdate = kSpUpdate, ours = reinterpret_cast<uint32_t>(&SpUpdate);
    if (!patch::Matches(kSpCapacity, &three, 1) || !patch::Matches(kSpItemCount, &three32, 4) ||
        !patch::Matches(kSpUpdateSlot, &spUpdate, 4)) {
        dslog::Write("[fail] Campaign: SINGLE PLAYER menu not recognised");
        return;
    }
    patch::Write(kSpCapacity, &four, 1);
    patch::Write(kSpItemCount, &four32, 4);
    patch::Write(kSpUpdateSlot, &ours, 4);
    dslog::Write("Campaign: %u custom campaign(s)", static_cast<unsigned>(g_campaigns.size()));
}

void features::OnFrameCampaigns() {
    static bool tried = false;
    if (!tried && *reinterpret_cast<void**>(kMainMenuScreen)) {
        tried = true;
        Build();
    }
}

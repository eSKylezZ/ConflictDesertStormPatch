// Custom campaigns: Lua mission scripts (scripthost.cpp) played from the menus - DesertStormFix's campaigns.cpp for
// Conflict: Desert Storm II.
//
// A mod declares a campaign in Mods\<mod>\campaign.txt ("key = value" lines, # comments):
//     name = Operation Sandstorm
//     mission = sandstorm1.lua
//     mission = sandstorm2.lua
// SINGLE PLAYER (menus.cpp, the game's leftover single-player screen, state 3) gets a fourth row CUSTOM CAMPAIGNS while
// a campaign exists; it opens our screen (front-end state menus::kCampaignsState 0x38, a menu of the single-player
// menu's class) listing the campaigns (up to 7). Picking one opens the game's own DIFFICULTY screen (state 8); its pick
// (FUN_0045bf90: difficulty FUN_005cb2f0 on [0x680128], then state 0xA) starts the campaign's first mission instead
// (scripthost.cpp StartCustomLevel: campaign flow 0x631608 custom level). The missions chain through the end screen's
// Continue: scripthost.cpp's flow-advance hook takes set_next_level's level, else CampaignNext, else the front end.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "features/features.h"
#include "features/menus.h"

namespace menus = features::menus;

namespace {
constexpr int kMaxCampaigns = 7;   // the screen's 8 rows, one kept free
constexpr int kIdFirstCampaign = 0x880;
constexpr uint32_t kTxtTitle = 0xC0B20030, kTxtDesc = 0xC0B20031, kTxtFirstCampaign = 0xC0B20040;

struct Campaign {
    std::string name, mod;
    std::vector<std::string> missions;
    std::wstring text;
};
std::vector<std::unique_ptr<Campaign>> g_campaigns;
const Campaign* g_chosen = nullptr;
std::string g_chosenFirst;

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

// Every switched-on Mods folder's campaign.txt (read on each visit of SINGLE PLAYER / the screen: the MODS menu may
// have switched folders on or off).
void LoadCampaigns(bool report) {
    g_campaigns.clear();
    if (!features::ModsFolderEnabled()) return;
    const std::string root = GameDir() + "\\Mods\\";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '_' || fd.cFileName[0] == '.') continue;
        FILE* f = nullptr;
        if (fopen_s(&f, (root + fd.cFileName + "\\campaign.txt").c_str(), "r") || !f) continue;
        auto c = std::make_unique<Campaign>();
        c->mod = fd.cFileName;
        char line[512];
        while (fgets(line, sizeof line, f)) {
            std::string s = line;
            if (const size_t hash = s.find('#'); hash != std::string::npos) s.resize(hash);
            const size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = Trim(s.substr(0, eq)), value = Trim(s.substr(eq + 1));
            if (_stricmp(key.c_str(), "name") == 0) c->name = value;
            else if (_stricmp(key.c_str(), "mission") == 0 && !value.empty() && value.size() < 200) c->missions.push_back(value);
        }
        fclose(f);
        if (c->name.empty()) c->name = c->mod;
        for (char& ch : c->name) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));   // menu style
        if (c->missions.empty()) {
            if (report) features::ModLog("[fail] Campaign %s: campaign.txt lists no mission", c->mod.c_str());
        } else if (static_cast<int>(g_campaigns.size()) < kMaxCampaigns) {
            if (report)
                features::ModLog("Campaign: %s (%s, %u missions)", c->name.c_str(), c->mod.c_str(),
                                 static_cast<unsigned>(c->missions.size()));
            c->text.assign(c->name.begin(), c->name.end());
            g_campaigns.push_back(std::move(c));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    for (size_t i = 0; i < g_campaigns.size(); ++i) menus::AddText(kTxtFirstCampaign + static_cast<uint32_t>(i), g_campaigns[i]->text.c_str());
}

bool Available() {
    LoadCampaigns(false);
    return !g_campaigns.empty();
}

void Setup(void* screen) {
    LoadCampaigns(false);
    menus::BeginList(screen);
    for (size_t i = 0; i < g_campaigns.size(); ++i)
        menus::AddItem(screen, static_cast<int>(i), kIdFirstCampaign + static_cast<int>(i),
                       kTxtFirstCampaign + static_cast<uint32_t>(i), kTxtDesc);
    menus::EndList(screen, static_cast<int>(g_campaigns.size()), kTxtTitle);
}

void Update(void*, int id, bool) {
    const int index = id - kIdFirstCampaign;
    if (index < 0 || index >= static_cast<int>(g_campaigns.size())) return;
    g_chosen = g_campaigns[index].get();
    g_chosenFirst = g_chosen->missions[0];
    dslog::Write("Campaign: %s chosen (first mission %s) - DIFFICULTY", g_chosen->name.c_str(), g_chosenFirst.c_str());
    menus::OpenDifficulty();
}

void Start() {
    if (g_chosenFirst.empty()) return;
    dslog::Write("Campaign: starts with %s", g_chosenFirst.c_str());
    features::StartCustomLevel(g_chosenFirst.c_str());
}

// campaign lists by name for CampaignNext (kept apart from the menu's list, which is re-read on every visit)
std::vector<std::vector<std::string>> AllCampaignMissions() {
    LoadCampaigns(false);
    std::vector<std::vector<std::string>> out;
    for (const auto& c : g_campaigns) out.push_back(c->missions);
    return out;
}
}  // namespace

// The level after `level` in the campaign that has it; "FrontEnd" after its last mission (or for a level that is in no
// campaign).
const char* features::CampaignNext(const char* level) {
    static std::string next;
    next = "FrontEnd";
    if (!level) return next.c_str();
    for (const auto& missions : AllCampaignMissions())
        for (size_t i = 0; i < missions.size(); ++i)
            if (_stricmp(missions[i].c_str(), level) == 0) {
                if (i + 1 < missions.size()) next = missions[i + 1];
                return next.c_str();
            }
    return next.c_str();
}

void features::ApplyCampaigns() {
    LoadCampaigns(true);
    menus::AddText(kTxtTitle, L"CUSTOM CAMPAIGNS");
    menus::AddText(kTxtDesc, L"Play this campaign's missions");
    menus::CampaignScreenHooks hooks;
    hooks.available = &Available;
    hooks.setup = &Setup;
    hooks.update = &Update;
    hooks.start = &Start;
    menus::SetCampaignScreenHooks(hooks);
    dslog::Write("[ok]   Campaign: CUSTOM CAMPAIGNS - %u custom campaign(s)", static_cast<unsigned>(g_campaigns.size()));
}

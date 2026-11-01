// Custom campaigns for Conflict: Denied Ops (2026-10-30): Mods\<mod>\campaign.txt lists missions - shipped operations
// or mission DLLs from mods - played in order from a CUSTOM CAMPAIGNS item in the main menu (modsmenu.cpp builds the
// screen). Same file format and screens as DS1 / DS2 / The Great Escape / Vietnam (games/cv/src/features/campaigns.cpp):
//     name = Operation Sandstorm
//     unlock = progress                       (progress = missions open as they are reached (default); all = every one)
//     mission select = yes                    (no = picking the campaign starts its first not-completed mission)
//     chapter = Act I: The Landing            (optional: the missions below it, up to the next chapter, belong to it)
//     mission = M_CMB.dll | Comms Base        (mission DLL name, optionally "| title" for the lists; else the file name)
// A mission is a mission DLL by file name: an Operations.xml operation's dllname (M_CMB.dll ... M_WTR.dll, ".dll" may
// be left out), or a DLL a mod ships under a new name with a <Name>.level sidecar naming the operation whose data it
// plays on ("--@archive m_cmb", Vietnam's sidecar line): that operation is started and its LoadLibrary is pointed at the
// mod's DLL. A mod DLL with a shipped name (M_CMB.dll) replaces that mission everywhere (the LoadLibrary hook below).
//
// How Denied Ops runs an operation (Steamless copy, docs/sdk/reference/game-deniedops.md "Custom campaigns"): the game
// flow object 0xA29290 holds the Operations.xml records (0xD28 bytes each from +8, count +0x109278; record +0 id,
// +0x12C dllname, +0x210 / +0x214 "oncompletion" action / next id), the current operation +0x10720C and a state
// machine +0x1097B4 run by 0x4756A0 (thiscall flow). Starting one (what the operations map does, 0x4758F0): current =
// record, state = 5, call 0x4756A0, [0xC49D18] = 1 - state 5 plays the briefing film (record +0x7C) when it has one,
// 0xF starts the level (0x475060 -> level init 0x441820, mission DLL LoadLibraryA at 0x444452), 7 = playing. When the
// level ends the machine runs 0x10 (0x474B40: marks the record completed, saves) and 0x11, whose step 0x474D10 (one
// caller, 0x4758B6) does the record's oncompletion: 0 = start operation +0x214, 1 = back to the operations map, 2 =
// main menu ... That call is hooked: while a custom campaign mission runs, a won mission starts the campaign's next
// mission (and opens it in the lists), the last one goes back to the main menu.
// Progress: DWORD "Campaign <mod folder>" = how many missions are open, in DeniedOpsFix's settings key.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kFlow = 0xA29290;            // game flow object (operations, current operation, state machine)
constexpr uint32_t kOpCount = 0x109278, kOpCurrent = 0x10720C, kState = 0x1097B4;
constexpr uint32_t kOpDll = 0x12C, kCampaignRunning = 0x1097D4;
constexpr uint32_t kFindOp = 0x470880;          // thiscall(flow, dllname) ret 4 -> record or 0
constexpr uint32_t kFlowUpdate = 0x4756A0;      // thiscall(flow): runs the state machine one step
constexpr uint32_t kStartPending = 0xC49D18;    // set by every operation start (operations map, chain, run-through)
constexpr uint32_t kOnCompletion = 0x474D10, kOnCompletionCall = 0x4758B6;
constexpr uint32_t kToMainMenu = 0x4746C0;      // thiscall(flow): after-state 1, state 0 (main menu)
constexpr uint32_t kLoadMissionCall = 0x444452, kLoadLibrarySlot = 0x90E0D0;
constexpr char kKey[] = "Software\\PSE\\deniedops\\Plugins\\deniedopsfix";

template <class T>
T& Ref(uintptr_t a) {
    return *reinterpret_cast<T*>(a);
}

// ---- campaign.txt ----
struct Mission {
    std::string file, title;
};
struct Chapter {
    std::string name;
    size_t first = 0, count = 0;   // its missions in Campaign::missions
};
struct Campaign {
    std::string name, mod;
    std::vector<Mission> missions;
    std::vector<Chapter> chapters;   // empty = no chapters
    bool unlockAll = false;
    bool select = true;
};
std::vector<std::unique_ptr<Campaign>> g_campaigns;

enum class List { Campaigns, Chapters, Missions };
struct Row {
    std::string text;
    int index = 0;
    bool locked = false;
};
List g_list = List::Campaigns;
int g_pick = 0, g_chapter = -1;
std::vector<Row> g_rows;

// The running campaign mission (chained on a win), and the mod DLL its operation loads instead of the shipped one.
std::string g_activeMod;
int g_activeIndex = -1;
uintptr_t g_activeOp = 0;
std::string g_dllOverride, g_overrideBase;

std::string Trim(std::string s) {
    const size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? std::string() : s.substr(b, e - b + 1);
}
std::string Upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    return s;
}
bool IsYes(const std::string& v) {
    return _stricmp(v.c_str(), "yes") == 0 || _stricmp(v.c_str(), "on") == 0 || v == "1" || _stricmp(v.c_str(), "true") == 0;
}
std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    return dir.substr(0, dir.find_last_of("\\/"));
}
std::string WithDll(std::string file) {   // "M_CMB" -> "M_CMB.dll"
    if (file.find('.') == std::string::npos) file += ".dll";
    return file;
}
std::string BaseName(const char* path) {
    const char* b = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') b = p + 1;
    return b;
}

// ---- progress: how many of a campaign's missions are open (DWORD "Campaign <mod>") ----
std::string ProgressName(const Campaign& c) { return "Campaign " + c.mod; }
uint32_t ReadProgress(const Campaign& c) {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, kKey, ProgressName(c).c_str(), RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        v = 0;
    return v;
}
size_t Opened(const Campaign& c) {
    if (c.unlockAll) return c.missions.size();
    return std::min<size_t>(std::max<uint32_t>(ReadProgress(c), 1), c.missions.size());
}
void Open(const Campaign& c, size_t count) {   // missions [0, count) open
    if (c.unlockAll) return;
    count = std::min(count, c.missions.size());
    if (count <= ReadProgress(c)) return;
    const DWORD v = static_cast<DWORD>(count);
    if (RegSetKeyValueA(HKEY_CURRENT_USER, kKey, ProgressName(c).c_str(), REG_DWORD, &v, sizeof v) != ERROR_SUCCESS)
        dslog::Write("Campaign: progress of %s not saved", c.mod.c_str());
    else
        dslog::Write("Campaign: %s - %u mission(s) open", c.mod.c_str(), static_cast<unsigned>(count));
}

void LoadCampaigns(bool report) {
    g_campaigns.clear();
    if (!features::Get().mods) return;
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
            if (_stricmp(key.c_str(), "name") == 0) {
                c->name = value;
            } else if (_stricmp(key.c_str(), "unlock") == 0) {
                c->unlockAll = _stricmp(value.c_str(), "all") == 0;
            } else if (_stricmp(key.c_str(), "mission select") == 0) {
                c->select = IsYes(value);
            } else if (_stricmp(key.c_str(), "chapter") == 0) {
                if (c->chapters.empty() && !c->missions.empty()) {   // missions above the first chapter line
                    c->chapters.push_back({"PROLOGUE", 0, c->missions.size()});
                    if (report)
                        features::ModLog("Campaign %s: missions above the first chapter line are in a chapter PROLOGUE",
                                         c->mod.c_str());
                }
                c->chapters.push_back(
                    {Upper(value.empty() ? "CHAPTER " + std::to_string(c->chapters.size() + 1) : value), c->missions.size(), 0});
            } else if (_stricmp(key.c_str(), "mission") == 0) {
                const size_t bar = value.find('|');
                Mission m{Trim(value.substr(0, bar)), bar == std::string::npos ? "" : Trim(value.substr(bar + 1))};
                if (m.file.empty() || m.file.size() >= 200) continue;
                m.file = WithDll(m.file);
                if (m.title.empty()) m.title = m.file.substr(0, m.file.find_last_of('.'));
                m.title = Upper(m.title);
                c->missions.push_back(std::move(m));
                if (!c->chapters.empty()) ++c->chapters.back().count;
            }
        }
        fclose(f);
        if (c->name.empty()) c->name = c->mod;
        c->name = Upper(c->name);
        for (size_t i = 0; i < c->chapters.size();) {   // a chapter without missions has nothing to pick
            if (c->chapters[i].count) {
                ++i;
                continue;
            }
            if (report)
                features::ModLog("Campaign %s: chapter %s lists no mission - left out", c->mod.c_str(), c->chapters[i].name.c_str());
            c->chapters.erase(c->chapters.begin() + i);
        }
        if (c->missions.empty()) {
            if (report) features::ModLog("[fail] Campaign %s: campaign.txt lists no mission", c->mod.c_str());
        } else {
            if (report)
                features::ModLog("Campaign: %s (%s, %u missions, %u chapters%s%s)", c->name.c_str(), c->mod.c_str(),
                                 static_cast<unsigned>(c->missions.size()), static_cast<unsigned>(c->chapters.size()),
                                 c->unlockAll ? ", all unlocked" : "", c->select ? "" : ", no mission select");
            g_campaigns.push_back(std::move(c));
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

const Campaign* Picked() {
    return g_pick >= 0 && g_pick < static_cast<int>(g_campaigns.size()) ? g_campaigns[g_pick].get() : nullptr;
}
bool HasChapterList(const Campaign& c) { return c.chapters.size() > 1; }

// The rows of the current list, and the row to start on (the picked campaign / the newest open chapter or mission).
int BuildRows() {
    g_rows.clear();
    if (g_list == List::Campaigns) {
        for (size_t i = 0; i < g_campaigns.size(); ++i) g_rows.push_back({g_campaigns[i]->name, static_cast<int>(i), false});
        return std::clamp(g_pick, 0, std::max(0, static_cast<int>(g_rows.size()) - 1));
    }
    const Campaign* c = Picked();
    if (!c) return 0;
    const size_t opened = Opened(*c);
    int start = 0;
    if (g_list == List::Chapters) {
        for (size_t i = 0; i < c->chapters.size(); ++i) {
            const bool locked = c->chapters[i].first >= opened;
            if (!locked) start = static_cast<int>(i);
            g_rows.push_back({locked ? c->chapters[i].name + " - LOCKED" : c->chapters[i].name, static_cast<int>(i), locked});
        }
        return start;
    }
    size_t first = 0, count = c->missions.size();
    if (g_chapter >= 0 && g_chapter < static_cast<int>(c->chapters.size()))
        first = c->chapters[g_chapter].first, count = c->chapters[g_chapter].count;
    for (size_t i = first; i < first + count; ++i) {
        const bool locked = i >= opened;
        if (!locked) start = static_cast<int>(i - first);
        g_rows.push_back({locked ? c->missions[i].title + " - LOCKED" : c->missions[i].title, static_cast<int>(i), locked});
    }
    return start;
}

// ---- starting a mission ----
uintptr_t FindOp(const std::string& dll) {
    if (!Ref<uint32_t>(kFlow + kOpCount)) return 0;   // Operations.xml not read yet
    return reinterpret_cast<uintptr_t>(reinterpret_cast<void*(__fastcall*)(void*, void*, const char*)>(kFindOp)(
        reinterpret_cast<void*>(kFlow), nullptr, dll.c_str()));
}

// A mod mission DLL's <Name>.level: the "--@archive <operation>" line (also "archive = <operation>") names the shipped
// operation whose data (archives, packages, scene, sounds) the mission plays on.
std::string LevelBase(const std::string& dll) {
    const std::string level = features::ModFilePath((dll.substr(0, dll.find_last_of('.')) + ".level").c_str());
    if (level.empty()) return std::string();
    FILE* f = nullptr;
    if (fopen_s(&f, level.c_str(), "r") || !f) return std::string();
    std::string base;
    char line[512];
    while (fgets(line, sizeof line, f) && base.empty()) {
        std::string s = Trim(line);
        if (s.rfind("--@archive", 0) == 0) base = Trim(s.substr(10));
        else if (const size_t eq = s.find('='); eq != std::string::npos && _stricmp(Trim(s.substr(0, eq)).c_str(), "archive") == 0)
            base = Trim(s.substr(eq + 1));
    }
    fclose(f);
    return base.empty() ? base : WithDll(base);
}

// The operation record that plays `dll`, with the mod DLL to load instead (empty = the operation's own).
uintptr_t Resolve(const std::string& dll, std::string& override, std::string& base) {
    override.clear();
    base.clear();
    if (uintptr_t op = FindOp(dll)) return op;
    override = features::ModFilePath(dll.c_str());
    if (override.empty()) {
        features::ModLog("[fail] Campaign: %s is no Operations.xml mission and no mod has it", dll.c_str());
        return 0;
    }
    base = LevelBase(dll);
    if (base.empty()) {
        features::ModLog("[fail] Campaign: mod mission %s needs a %s.level with \"--@archive <operation>\" (e.g. m_cmb)",
                         dll.c_str(), dll.substr(0, dll.find_last_of('.')).c_str());
        return 0;
    }
    uintptr_t op = FindOp(base);
    if (!op) features::ModLog("[fail] Campaign: %s.level names %s - not an Operations.xml mission", dll.c_str(), base.c_str());
    return op;
}

bool StartResolved(uintptr_t op) {
    const uintptr_t flow = kFlow;
    Ref<uintptr_t>(flow + kOpCurrent) = op;
    Ref<uint32_t>(flow + kState) = 5;
    Ref<uint32_t>(flow + kCampaignRunning) = 1;   // as NEW CAMPAIGN (0x475BA0) / CONTINUE CAMPAIGN (0x473430) set it
    reinterpret_cast<void(__fastcall*)(void*, void*)>(kFlowUpdate)(reinterpret_cast<void*>(flow), nullptr);
    Ref<uint32_t>(kStartPending) = 1;
    return true;
}

bool StartChained(const Campaign& c, size_t index) {
    if (index >= c.missions.size()) return false;
    std::string override, base;
    const uintptr_t op = Resolve(c.missions[index].file, override, base);
    if (!op) return false;
    Open(c, index + 1);
    g_dllOverride = override;
    g_overrideBase = base;
    g_activeMod = c.mod;
    g_activeIndex = static_cast<int>(index);
    g_activeOp = op;
    g_list = List::Campaigns;   // the screen opens on the campaigns list next time (it isn't a Back to it)
    g_chapter = -1;
    dslog::Write("Campaign: %s - mission %u %s (%s%s%s)", c.name.c_str(), static_cast<unsigned>(index + 1),
                 c.missions[index].file.c_str(), reinterpret_cast<const char*>(op + kOpDll),
                 override.empty() ? "" : ", DLL from ", override.c_str());
    return StartResolved(op);
}

const Campaign* ActiveCampaign() {
    for (const auto& c : g_campaigns)
        if (c->mod == g_activeMod) return c.get();
    return nullptr;
}

// ---- hooks ----
// Level init's mission DLL load (call [LoadLibraryA] at 0x444452; the name = the operation's dllname or the level
// name): the running campaign's mod DLL for its operation, else a mod's DLL of that name, else the game's.
HMODULE __stdcall LoadMission(const char* name) {
    std::string path;
    if (name && !g_dllOverride.empty() && _stricmp(BaseName(name).c_str(), g_overrideBase.c_str()) == 0) path = g_dllOverride;
    else if (name) path = features::ModFilePath(BaseName(name).c_str());
    if (!path.empty()) {
        HMODULE m = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        features::ModLog(m ? "Mods: mission %s loaded from %s" : "[fail] Mods: mission %s not loaded from %s",
                         name, path.c_str());
        if (m) return m;
    }
    return LoadLibraryA(name);
}

// The operation's "oncompletion" step (state 0x11, after a level ended and was scored).
int __fastcall OnCompletion(void* flow, void* edx) {
    const uintptr_t op = Ref<uintptr_t>(reinterpret_cast<uintptr_t>(flow) + kOpCurrent);
    if (g_activeIndex >= 0 && op && op == g_activeOp) {
        if (g_campaigns.empty()) LoadCampaigns(false);
        const Campaign* c = ActiveCampaign();
        const size_t next = static_cast<size_t>(g_activeIndex) + 1;
        if (c) Open(*c, next + 1);
        if (c && next < c->missions.size()) {
            dslog::Write("Campaign: mission won - next %s", c->missions[next].file.c_str());
            if (StartChained(*c, next)) return 1;
        }
        dslog::Write("Campaign: %s - %s, back to the main menu", g_activeMod.c_str(),
                     c && next >= c->missions.size() ? "campaign finished" : "chain ended");
        g_activeIndex = -1;
        g_activeOp = 0;
        g_dllOverride.clear();
        reinterpret_cast<void(__fastcall*)(void*, void*)>(kToMainMenu)(flow, nullptr);
        return 1;
    }
    return reinterpret_cast<int(__fastcall*)(void*, void*)>(kOnCompletion)(flow, edx);
}

void Apply() {
    int ok = 0;
    ok += patch::HookIndirectCall(kLoadMissionCall, reinterpret_cast<const void*>(&LoadMission), kLoadLibrarySlot) ? 1 : 0;
    ok += patch::HookCall(kOnCompletionCall, reinterpret_cast<const void*>(&OnCompletion), kOnCompletion) ? 1 : 0;
    features::ModLog("%s Campaign: CUSTOM CAMPAIGNS - %u custom campaign(s), mission hooks %d of 2", ok == 2 ? "[ok]  " : "[fail]",
                     static_cast<unsigned>(g_campaigns.size()), ok);
}
}  // namespace

bool features::StartMission(const char* dll) {
    if (!dll || !*dll) return false;
    std::string override, base;
    const uintptr_t op = Resolve(WithDll(dll), override, base);
    if (!op) return false;
    g_dllOverride = override;
    g_overrideBase = base;
    g_activeIndex = -1;
    g_activeOp = 0;
    return StartResolved(op);
}

bool features::StartCampaignMission(const char* mod, int index) {
    LoadCampaigns(false);
    for (const auto& c : g_campaigns)
        if (mod && _stricmp(c->mod.c_str(), mod) == 0) return StartChained(*c, static_cast<size_t>(index));
    return false;
}

bool features::CampaignsAvailable() {
    LoadCampaigns(false);
    return !g_campaigns.empty();
}

void features::CampaignsOpened() {
    LoadCampaigns(false);
    g_list = List::Campaigns;
    g_chapter = -1;
}

std::vector<features::CampaignRow> features::CampaignRows(int* start) {
    const int s = BuildRows();
    if (start) *start = s;
    std::vector<CampaignRow> out;
    for (const Row& r : g_rows) {
        CampaignRow cr;
        for (char ch : r.text) cr.text += static_cast<wchar_t>(static_cast<unsigned char>(ch));
        cr.locked = r.locked;
        out.push_back(cr);
    }
    return out;
}

std::wstring features::CampaignTitle() {
    const Campaign* c = Picked();
    std::string t = g_list == List::Campaigns ? "CUSTOM CAMPAIGNS"
                    : g_list == List::Missions && c && g_chapter >= 0 && HasChapterList(*c) ? c->chapters[g_chapter].name
                    : c ? c->name
                        : "CUSTOM CAMPAIGNS";
    std::wstring w;
    for (char ch : t.substr(0, 48)) w += static_cast<wchar_t>(static_cast<unsigned char>(ch));
    return w;
}

void features::CampaignPick(int row) {
    if (row < 0 || row >= static_cast<int>(g_rows.size()) || g_rows[row].locked) return;
    const int index = g_rows[row].index;
    if (g_list == List::Campaigns) {
        g_pick = index;
        g_chapter = -1;
        const Campaign& c = *g_campaigns[index];
        if (!c.select) {   // straight into the first mission not yet completed
            size_t m = std::min(Opened(c), c.missions.size()) - 1;
            StartChained(c, m);
        } else {
            g_list = HasChapterList(c) ? List::Chapters : List::Missions;
        }
    } else if (g_list == List::Chapters) {
        g_chapter = index;
        g_list = List::Missions;
    } else if (const Campaign* c = Picked()) {
        StartChained(*c, static_cast<size_t>(index));
    }
}

bool features::CampaignBack() {
    const Campaign* c = Picked();
    if (g_list == List::Campaigns) return false;
    g_list = g_list == List::Missions && c && HasChapterList(*c) ? List::Chapters : List::Campaigns;
    return true;
}

void features::ApplyCampaigns() {
    if (!Get().mods || !Get().campaigns) {
        dslog::Write("[off]  Campaign: off (FeatMods / FeatCampaigns)");
        return;
    }
    LoadCampaigns(true);
    AfterUnpack(&Apply);
}

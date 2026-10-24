// Main menu MODS (port of DesertStormFix's modsmenu.cpp): every folder in <game>\Mods with ON / OFF. Choosing a row
// switches that mod by renaming its folder - "_" in front = off, the rule mods.cpp indexes by (and what the readme tells
// players) - so the menu and Explorer always agree. The last row applies the switches: "APPLY n CHANGES" while some are
// pending -> features::ReloadMods (mods.cpp) rebuilds the file index, so files opened from then on come from the new set.
// What the game read before keeps its old copy until it reads it again: mission files at the next level load, files
// read at start-up / by the front-end (menu art, texts, start-up tables) only after a restart - the row then reads
// RESTART THE GAME TO APPLY (the main menu too; the report names the files). (DS1 also reruns its table parsers; DS2
// has no table merging yet.)
//
// The screen is menus.cpp's MODS screen (front-end state menus::kModsState, an object of the game's single-player menu
// class, up to 8 rows): 7 mod rows that scroll + the Apply row. Scrolling: Down on the last mod row (or Up on the first)
// with more mods below (above) moves the window instead of the highlight - only for the keyboard / pad navigation the
// list read reports (menus.cpp NavInput), so a mouse resting on a row never scrolls; the title shows "MODS (a-b OF n)".
// Rows / title are our text hashes 0xC0B3xxxx (menus::AddText), rewritten every frame; descriptions are fixed.
//
// Main menu: "DESERTSTORM2FIX x.y.z - N MODS INSTALLED (M ACTIVE)" under the menu panel (+ a second line: "[fail]"
// lines in Mods\DesertStorm2Fix-report.txt, or RESTART THE GAME TO APPLY ALL MOD CHANGES), the game's small menu font.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "core/log.h"
#include "features/features.h"
#include "features/menus.h"

namespace menus = features::menus;

namespace {
constexpr int kVisible = 7, kMaxMods = 256;
constexpr int kIdRow0 = 0xA00, kIdApply = 0xA10, kIdNone = 0xA11;
enum : uint32_t {
    kTxtTitle = 0xC0B30001, kTxtApply, kTxtApplyDesc, kTxtNone, kTxtNoneDesc,
    kTxtRow0 = 0xC0B30010, kTxtRowDesc0 = 0xC0B30020
};

struct Mod {
    std::string name;   // without the "_"
    bool on;
};
std::vector<Mod> g_mods;
int g_offset = 0;   // first mod shown

// texts (menus::AddText keeps the pointers; rewritten every frame)
wchar_t g_title[48], g_apply[48];
wchar_t g_row[kVisible][64];

// the last Apply
features::ModReload g_last;
bool g_applied = false;
DWORD g_appliedAt = 0;
std::string g_renameFailed;

std::string ModsDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    return dir.substr(0, dir.find_last_of("\\/")) + "\\Mods";
}

// The folders as they are on disk now ("." folders are the modder's own business and not listed).
void Scan() {
    g_mods.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((ModsDir() + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        const bool off = fd.cFileName[0] == '_';
        g_mods.push_back({off ? fd.cFileName + 1 : fd.cFileName, !off});
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(g_mods.begin(), g_mods.end(), [](const Mod& a, const Mod& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    if (g_mods.size() > kMaxMods) g_mods.resize(kMaxMods);
}

int Visible() { return std::min(kVisible, static_cast<int>(g_mods.size())); }
int MaxOffset() { return std::max(0, static_cast<int>(g_mods.size()) - kVisible); }

// Switches that differ from the mods in use: folders switched on that aren't indexed, indexed ones switched off / gone.
int PendingChanges() {
    const std::vector<std::string>& loaded = features::LoadedMods();
    auto isLoaded = [&](const std::string& name) {
        for (const std::string& l : loaded)
            if (_stricmp(l.c_str(), name.c_str()) == 0) return true;
        return false;
    };
    int n = 0;
    for (const Mod& m : g_mods) n += m.on != isLoaded(m.name);
    for (const std::string& l : loaded)   // folder gone
        n += std::none_of(g_mods.begin(), g_mods.end(), [&](const Mod& m) { return _stricmp(m.name.c_str(), l.c_str()) == 0; });
    return n;
}

void Toggle(int i) {
    if (i < 0 || i >= static_cast<int>(g_mods.size())) return;
    Mod& m = g_mods[i];
    const std::string dir = ModsDir() + "\\";
    const std::string from = dir + (m.on ? "" : "_") + m.name, to = dir + (m.on ? "_" : "") + m.name;
    if (!MoveFileA(from.c_str(), to.c_str())) {
        g_renameFailed = m.name;
        dslog::Write("[fail] Mods menu: %s -> %s (error %lu)", from.c_str(), to.c_str(), GetLastError());
        return;
    }
    g_renameFailed.clear();
    m.on = !m.on;
    g_applied = false;
    dslog::Write("Mods menu: %s switched %s (Apply to use it)", m.name.c_str(), m.on ? "on" : "off");
}

void Apply() {
    g_last = features::ReloadMods();
    g_applied = true;
    g_appliedAt = GetTickCount();
    dslog::Write("Mods menu: applied - %d changed file(s), %d need a restart, %d load at the next level", g_last.changed,
                 g_last.restart, g_last.nextLevel);
    Scan();   // (the folders as the reload saw them)
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }

void UpdateTexts() {
    const int n = static_cast<int>(g_mods.size()), vis = Visible();
    if (n > kVisible)
        swprintf_s(g_title, L"MODS (%d-%d OF %d)", g_offset + 1, g_offset + vis, n);
    else
        swprintf_s(g_title, L"MODS");
    // (descriptions: the box under the list types a row's text once when it is highlighted - so they stay the same
    // whatever the row shows, and short: two lines)
    for (int k = 0; k < vis; ++k) {
        const Mod& m = g_mods[g_offset + k];
        std::wstring name = Wide(m.name.substr(0, 28));
        for (wchar_t& c : name) c = static_cast<wchar_t>(towupper(c));
        const bool failed = !g_renameFailed.empty() && _stricmp(g_renameFailed.c_str(), m.name.c_str()) == 0;
        swprintf_s(g_row[k], L"%s: %s", name.c_str(), failed ? L"CAN'T RENAME" : m.on ? L"ON" : L"OFF");
    }
    const int pending = PendingChanges();
    if (pending)
        swprintf_s(g_apply, L"APPLY %d CHANGE%s", pending, pending == 1 ? L"" : L"S");
    else if (g_applied && g_last.restart)
        swprintf_s(g_apply, L"RESTART THE GAME TO APPLY");
    else if (g_applied && GetTickCount() - g_appliedAt < 4000)
        swprintf_s(g_apply, L"CHANGES APPLIED");
    else
        swprintf_s(g_apply, L"NO CHANGES TO APPLY");
}

void Setup(void* screen) {
    Scan();
    g_offset = 0;
    g_renameFailed.clear();
    UpdateTexts();
    menus::BeginList(screen);
    if (g_mods.empty()) {
        menus::AddItem(screen, 0, kIdNone, kTxtNone, kTxtNoneDesc);
        menus::EndList(screen, 1, kTxtTitle);
        return;
    }
    const int vis = Visible();
    for (int k = 0; k < vis; ++k) menus::AddItem(screen, k, kIdRow0 + k, kTxtRow0 + k, kTxtRowDesc0 + k);
    menus::AddItem(screen, vis, kIdApply, kTxtApply, kTxtApplyDesc);
    menus::EndList(screen, vis + 1, kTxtTitle);
}

// rows 0 .. vis-1 = mods g_offset.., row vis = Apply; the list wraps at both ends
void Update(void* screen, int id, int nav, int before, bool /*back*/) {
    const int vis = Visible();
    if (vis > 0 && nav) {
        const int after = menus::SelectedRow(screen);
        if (nav > 0 && before == vis - 1 && after != before && g_offset < MaxOffset()) {
            ++g_offset;                               // down past the last mod row: scroll
            menus::SetSelectedRow(screen, vis - 1);
        } else if (nav > 0 && before == vis && after == 0) {
            g_offset = 0;                             // Apply -> wrapped to the top
        } else if (nav < 0 && before == 0 && after != 0 && g_offset > 0) {
            --g_offset;                               // up past the first mod row: scroll
            menus::SetSelectedRow(screen, 0);
        } else if (nav < 0 && before == 0 && after == vis) {
            g_offset = MaxOffset();                   // top -> wrapped to Apply: the last mods above it
        }
    }
    if (id >= kIdRow0 && id < kIdRow0 + vis) Toggle(g_offset + (id - kIdRow0));
    else if (id == kIdApply && PendingChanges()) Apply();
    UpdateTexts();
}

// ---- main menu lines ----
int InstalledMods() {   // folders in Mods, counted again every 2 s at most
    static int count = 0;
    static DWORD at = 0;
    if (at && GetTickCount() - at < 2000) return count;
    at = GetTickCount() | 1;
    count = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((ModsDir() + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do count += (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.';
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return count;
}

// Under the main menu's prompt box (SELECT), inside the monitor: the menu panel sits in the middle of the front-end's
// 1280 x 1024 screen (box left x 372, prompt box bottom y 713; text y = the line's middle), so only drawn while the 2D
// screen is 1280 x 1024 - the virtual screen of the front-end feature (frontend.cpp / ds2fix.widescreen) or a real one.
void MainMenuDraw() {
    int w = 0, h = 0;
    menus::ScreenSize(w, h);
    if (w != 1280 || h != 1024) return;
    const int installed = InstalledMods(), active = features::ModCount(), problems = features::ModProblems();
    char line1[96], line2[96];
    snprintf(line1, sizeof line1, "DESERTSTORM2FIX %s - %d MOD%s INSTALLED (%d ACTIVE)", DS_VERSION, installed,
             installed == 1 ? "" : "S", active);
    menus::DrawLabel(line1, 374, 725, 0xFFC8C8C8, true);
    if (problems > 0) {   // "[fail]" lines in the report (release builds have no other log)
        snprintf(line2, sizeof line2, "%d MOD PROBLEM%s - SEE MODS\\DESERTSTORM2FIX-REPORT.TXT", problems,
                 problems == 1 ? "" : "S");
    }
    if (g_applied && g_last.restart)   // the MODS screen's last Apply (the report names the files)
        snprintf(line2, sizeof line2, "RESTART THE GAME TO APPLY ALL MOD CHANGES");
    if (problems > 0 || (g_applied && g_last.restart)) menus::DrawLabel(line2, 374, 743, 0xFFE09040, true);
}
}  // namespace

void features::ApplyModsMenu() {
    if (!features::ModsFolderEnabled()) {
        dslog::Write("[off]  Mods menu: no MODS item (FeatMods)");
        return;
    }
    menus::AddText(kTxtTitle, g_title);
    menus::AddText(kTxtApply, g_apply);
    menus::AddText(kTxtApplyDesc, L"Use the switched mods now (some files need a restart)");
    menus::AddText(kTxtNone, L"NO MODS FOUND");
    menus::AddText(kTxtNoneDesc, L"Put mod folders in the Mods folder next to CDS II.exe");
    for (int k = 0; k < kVisible; ++k) {
        menus::AddText(kTxtRow0 + k, g_row[k]);
        menus::AddText(kTxtRowDesc0 + k, L"Select to switch this mod on or off, then Apply below");
    }
    menus::ModsScreenHooks hooks;
    hooks.setup = &Setup;
    hooks.update = &Update;
    hooks.mainMenuDraw = &MainMenuDraw;
    menus::SetModsScreenHooks(hooks);
    dslog::Write("[ok]   Mods menu: main menu MODS (state 0x%X) + mod count line", menus::kModsState);
}

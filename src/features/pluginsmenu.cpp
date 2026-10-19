// OPTIONS > DESERTSTORMFIX > PLUGINS: every PSE plugin found in PSE\Plugins (pse::PluginResults: loaded ones, then the
// refused / failed ones with the loader's reason) and, for a loaded plugin, a settings page built from what it declared
// through pse.settings (core/pse_ui_model.h BuildPage) - plugins get menu rows without writing any UI code.
//
//   state 0x4D  the list: "<NAME> <version>" / LOADED, SETTINGS, REFUSED, FAILED; the footer says why a plugin was
//               refused / failed, how many settings it has, or (DesertStormFix itself) where its own menus are
//   state 0x4E  a plugin's page: bool ON / OFF, int (its step, stops at min / max), choice (its labels, wraps) as
//               spinners like OPTIONS > VIDEO; colour = #RRGGBB, Enter opens the colour wheel (barcolour.cpp
//               OpenColourPicker); string = shown, read-only (v1); a group starts with a "- GROUP -" heading row;
//               HIDDEN settings are left out. A change = pse.settings SetInt (stored + the plugin's OnChange at once).
//               Restart settings show "*", next-level ones "+"; the footer asks for a restart once a restart setting
//               differs from what the game started with (NotePluginSettingsAtStart, PSE_MSG_POST_LOAD), and shows the
//               selected setting's help line.
//
// Both screens are Options sub-screen objects with choice rows in a scrolling window, as fixmenu.cpp / videomenu.cpp
// (same list code, W / S, pad, mouse wheel, scrollbar). Plugin texts are UTF-8: converted to the fonts' 8-bit text with
// fallbacks for missing glyphs (features::GameText). Back: page -> list -> DESERTSTORMFIX.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/pse_ui_model.h"
#include "features/features.h"
#include "features/overlay.h"
#include "loader.h"

namespace {
constexpr uint32_t kSetState = 0x47EF20, kBackPressed = 0x478380, kListInput = 0x4E5D80;
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kChoiceCtor = 0x4E5010, kItemSet = 0x4E4FB0, kChoices = 0x4E5090;
constexpr uint32_t kListCallbacks = 0x4E59E0, kChoiceDraw = 0x402E20;
constexpr uint32_t kDrawPanel = 0x456EB0, kDrawTitle = 0x456F40;
constexpr uint32_t kSmallFont = 0x60EDB4, kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kMainMenuScreen = 0x617A00;
constexpr uint32_t kRenderer = 0x63C924, kFrontEndState = 0x617C18;
constexpr uint32_t kMainMenuEnter = 0x4153B0, kOptCtor = 0x409B90, kOptVtable = 0x5D8510, kOptSetup = 0x409C90;
constexpr uint32_t kJoystickCount = 0x754BE8, kMouseWheel = 0x754C24 + 8;   // DIMOUSESTATE lZ
constexpr uint32_t kStateFixMenu = 0x47, kStateList = 0x4D, kStatePage = 0x4E;
constexpr int kVtableEntries = 21, kVisible = 8;

enum : uint32_t {
    kTextListTitle = 0xC0130001, kTextPageTitle = 0xC0130002,
    kTextListRow0 = 0xC0130010, kTextPageRow0 = 0xC0130020,     // + visible row
    kTextListVal0 = 0xC0130100, kTextPageVal0 = 0xC0130200,     // + visible row * 4 + 0 previous / 1 current / 2 next
};

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}
void SetState(uint32_t s) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(s); }
void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }
void AddToList(void* list, void* item) {
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, item);
}
using ItemSetFn = void(__thiscall*)(void*, int, uint32_t, int, int, uint32_t, int, uint32_t, int);
const PSESettingsInterface* Settings() { return pse::SettingsInterface(); }

// ---- the model ----
std::vector<pse_ui::PluginEntry> g_plugins;   // list rows (rebuilt each time the list opens)
int g_listRow = 0;                            // list row to come back to from a page
PSEPluginHandle g_pageOwner = 0;
std::string g_pageName, g_pageTitle;          // page: plugin name, title (game text)
std::vector<pse_ui::Row> g_rows;              // page rows
std::map<std::pair<PSEPluginHandle, std::string>, int32_t> g_atStart;   // restart detection

size_t CountSettings(PSEPluginHandle h, void*) { return pse_ui::VisibleSettings(Settings(), h); }

void BuildList() {
    g_plugins = pse_ui::BuildPluginList(pse::PluginResults(), {"dsfix"}, &CountSettings, nullptr);
    // DesertStormFix's own sub-plugins that declare settings of their own (dsfix.assetstore: SHARED ASSET STORE,
    // REBUILD CACHE AT NEXT START) get a row too - the external load results list only plugin DLLs. Handles are
    // index + 1 and never reused; unknown ones have no name.
    for (PSEPluginHandle h = 1; h < 4096; ++h) {
        const char* name = pse::PluginName(h);
        if (!name || !*name) break;
        if (std::strncmp(name, "dsfix.", 6) != 0 || pse::PluginDisabled(h)) continue;
        const size_t n = CountSettings(h, nullptr);
        if (!n) continue;
        pse_ui::PluginEntry e;
        e.name = name;
        const PSEPluginInfo* info = pse::PluginInfo(h);
        e.displayName = info && info->displayName && *info->displayName ? info->displayName : name;
        e.version = "";
        e.state = pse::PluginState::Loaded;
        e.handle = h;
        e.settings = n;
        g_plugins.push_back(std::move(e));
    }
}

bool PageAlive() { return g_pageOwner && !pse::PluginDisabled(g_pageOwner) && pse::PluginName(g_pageOwner)[0]; }

int32_t Current(const pse_ui::Row& row) { return PageAlive() ? Settings()->GetInt(row.owner, row.key.c_str()) : row.def; }
std::string CurrentString(const pse_ui::Row& row) {
    if (!PageAlive()) return std::string();
    char buf[512];
    Settings()->GetString(row.owner, row.key.c_str(), buf, sizeof buf);
    return buf;
}

bool RestartPending() {
    for (const pse_ui::Row& r : g_rows) {
        if (!pse_ui::Editable(r) || !pse_ui::NeedsRestart(r.flags)) continue;
        auto it = g_atStart.find({r.owner, r.key});
        if (it != g_atStart.end() && it->second != Current(r)) return true;
    }
    return false;
}

std::string Upper(const std::string& utf8, size_t max = 0) { return features::GameText(utf8.c_str(), true, nullptr, max); }

// ---- one screen (the list or the page) ----
struct Screen {
    uint32_t state, title, row0, val0;
    uint16_t firstId;
    uint8_t obj[0x20];
    void* vtable[kVtableEntries];
    bool built = false;
    int offset = 0, lastRow = 0;
    int choicesFor[kVisible];
    std::string labels[kVisible], values[kVisible][3];   // text buffers (the game keeps the pointer for the frame)
};
Screen g_list{kStateList, kTextListTitle, kTextListRow0, kTextListVal0, 0x8B0, {}, {}};
Screen g_page{kStatePage, kTextPageTitle, kTextPageRow0, kTextPageVal0, 0x8C0, {}, {}};
bool IsList(const Screen& s) { return &s == &g_list; }
Screen& Of(void* screen) { return screen == g_page.obj ? g_page : g_list; }

int Count(const Screen& s) { return IsList(s) ? std::max<int>(1, static_cast<int>(g_plugins.size())) : static_cast<int>(g_rows.size()); }
int Shown(const Screen& s) { return std::min(kVisible, std::max(2, Count(s))); }   // the class set-up always adds 2
int MaxOffset(const Screen& s) { return std::max(0, Count(s) - kVisible); }

bool Spin(const Screen& s, int r) {
    if (IsList(s) || r >= Count(s)) return false;
    return pse_ui::Spinner(g_rows[r]);
}

std::string Label(const Screen& s, int r) {
    if (r >= Count(s)) return std::string();
    if (IsList(s)) {
        if (g_plugins.empty()) return "NO PLUGINS FOUND";
        const pse_ui::PluginEntry& e = g_plugins[r];
        const std::string name = e.host ? std::string("DESERTSTORMFIX") : Upper(e.displayName, 34);
        return e.version.empty() ? name : name + " " + e.version;
    }
    const pse_ui::Row& row = g_rows[r];
    if (row.kind == pse_ui::RowKind::Heading) return "- " + Upper(row.label, 34) + " -";
    return Upper(row.label, 40);
}

// j: 0 previous, 1 current, 2 next value (spinners); others show j = 1 only.
std::string Value(const Screen& s, int r, int j) {
    if (r >= Count(s)) return std::string();
    if (IsList(s)) {
        if (g_plugins.empty()) return std::string();
        const pse_ui::PluginEntry& e = g_plugins[r];
        if (e.state == pse::PluginState::Loaded && !e.host && e.settings) return "SETTINGS";
        return pse_ui::StateText(e.state);
    }
    const pse_ui::Row& row = g_rows[r];
    const int32_t cur = Current(row);
    int32_t v = cur;
    if (Spin(s, r) && j != 1) v = pse_ui::StepValue(row, cur, j == 0 ? -1 : 1);
    if (row.kind == pse_ui::RowKind::Heading) return std::string();
    std::string text = row.kind == pse_ui::RowKind::String ? Upper(CurrentString(row), 30)
                       : row.kind == pse_ui::RowKind::Choice ? Upper(pse_ui::ValueText(row, v), 30)
                                                            : pse_ui::ValueText(row, v);
    if (row.kind == pse_ui::RowKind::String && text.empty()) text = "-";
    return text + pse_ui::WhenMarker(row.flags);
}

void SetValue(const pse_ui::Row& row, int32_t value) {
    if (!PageAlive()) return;
    const PSEResult r = Settings()->SetInt(row.owner, row.key.c_str(), value);
    dslog::Write(r == PSE_OK ? "Plugins menu: %s %s = %d%s" : "[fail] Plugins menu: %s %s = %d (error %d)",
                 g_pageName.c_str(), row.key.c_str(), Settings()->GetInt(row.owner, row.key.c_str()),
                 r == PSE_OK && pse_ui::NeedsRestart(row.flags) ? " (after a restart)" : "", static_cast<int>(r));
}

int g_pickRow = -1;   // page row whose colour the wheel is changing
void PickedColour(uint32_t colour, void*) {
    if (g_pickRow >= 0 && g_pickRow < static_cast<int>(g_rows.size())) SetValue(g_rows[g_pickRow], static_cast<int32_t>(colour));
}

// The row's id came back from the list input: choice = the item's selected text.
void Select(Screen& s, int r, int choice) {
    if (r >= Count(s)) return;
    if (IsList(s)) {
        if (g_plugins.empty()) return;
        const pse_ui::PluginEntry& e = g_plugins[r];
        if (e.state != pse::PluginState::Loaded || e.host || !e.handle) return;
        g_listRow = r;
        g_pageOwner = e.handle;
        g_pageName = e.name;
        g_pageTitle = Upper(e.displayName, 40);
        SetState(kStatePage);
        return;
    }
    const pse_ui::Row& row = g_rows[r];
    if (Spin(s, r)) {   // Left landed on "previous"; Right / Enter / click: next
        SetValue(row, pse_ui::StepValue(row, Current(row), choice == 0 ? -1 : 1));
    } else if (row.kind == pse_ui::RowKind::Colour && PageAlive()) {
        g_pickRow = r;
        features::OpenColourPicker(static_cast<uint32_t>(Current(row)), static_cast<uint32_t>(row.def), &PickedColour,
                                   nullptr);
    }
}

// ---- the game's list ----
bool __fastcall Create(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, Global<void*>(kLargeFont), Global<void*>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, kVisible)) return false;
    for (int i = 0; i < kVisible; ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x68, 0x1E);
        if (item) reinterpret_cast<void*(__thiscall*)(void*)>(kChoiceCtor)(item);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    }
    return true;
}

void RefreshRows(Screen& s, void* list) {
    using ChoicesFn = void(__thiscall*)(void*, uint32_t, uint32_t, int, int, int, int, int, int);
    for (int k = 0; k < Shown(s); ++k) {
        void* item = ItemAt(list, k);
        if (!item) continue;
        const int r = s.offset + k;
        const bool spin = Spin(s, r);
        const int key = r << 1 | (spin ? 1 : 0);
        if (s.choicesFor[k] != key) {   // only when it changes (setting them every frame swallowed Up / Down)
            s.choicesFor[k] = key;
            const uint32_t t = s.val0 + k * 4;
            if (spin) reinterpret_cast<ChoicesFn>(kChoices)(item, t, t + 1, t + 2, -1, -1, -1, -1, -1);
            else reinterpret_cast<ChoicesFn>(kChoices)(item, t + 1, static_cast<uint32_t>(-1), -1, -1, -1, -1, -1, -1);
        }
        At<int8_t>(item, 0x24) = spin ? 1 : 0;
    }
}

void __fastcall Setup(void* screen, void*) {
    Screen& s = Of(screen);
    if (IsList(s)) {
        BuildList();
        dslog::Write("Plugins menu: list opened (%zu plugins)", g_plugins.size());
    } else {
        g_rows = pse_ui::BuildPage(Settings(), PageAlive() ? g_pageOwner : 0);
        dslog::Write("Plugins menu: %s opened (%zu rows)", g_pageName.c_str(), g_rows.size());
    }
    s.offset = 0;
    s.lastRow = 0;
    for (int& c : s.choicesFor) c = -1;
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);
    void* list = At<void*>(screen, 8);
    using Callbacks = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int, int, int, int, int);
    reinterpret_cast<Callbacks>(kListCallbacks)(list, 0x478440, 0x478480, 0x478530, 0x4785E0, 0x478690, 0, 0, 0, 0, 0);
    At<void*>(list, 0x90) = screen;
    const int rows = Shown(s);
    // back on the list from a page: the plugin's row again
    int cursor = 0;
    if (IsList(s) && g_listRow > 0 && g_listRow < Count(s)) {
        s.offset = std::min(std::max(0, g_listRow - (kVisible - 1)), MaxOffset(s));
        cursor = g_listRow - s.offset;
    }
    for (int k = 0; k < rows; ++k)
        reinterpret_cast<ItemSetFn>(kItemSet)(ItemAt(list, k), s.firstId + k, 0xFFFFFFFF, 0, 0x24, s.row0 + k, 0,
                                              kChoiceDraw, 0);
    RefreshRows(s, list);
    for (int k = 2; k < rows; ++k) AddToList(list, ItemAt(list, k));
    At<int16_t>(list, 0x28) = static_cast<int16_t>(cursor);
    s.lastRow = cursor;
    At<uint16_t>(list, 0x30) = static_cast<uint16_t>(s.firstId + cursor);
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (rows - 2) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = static_cast<int16_t>(rows);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, s.title, 0, 0, 0x7FFF);
}

// Scrolling and W / S as in fixmenu.cpp.
void Scroll(Screen& s, void* list) {
    const int vis = Shown(s);
    int16_t& row = At<int16_t>(list, 0x28);
    const int prev = s.lastRow, now = row;
    if (prev == vis - 1 && now == 0) {
        if (s.offset < MaxOffset(s)) {
            ++s.offset;
            row = static_cast<int16_t>(vis - 1);
        } else {
            s.offset = 0;
        }
    } else if (prev == 0 && now == vis - 1) {
        if (s.offset > 0) {
            --s.offset;
            row = 0;
        } else {
            s.offset = MaxOffset(s);
        }
    }
    s.lastRow = row;
}

int WasdStep() {
    static int dir = 0;
    static DWORD since = 0, last = 0;
    const int now = features::KeyHeld('W') ? -1 : features::KeyHeld('S') ? 1 : 0;
    const DWORD t = GetTickCount();
    if (now != dir) {
        dir = now;
        since = last = t;
        return now;
    }
    if (now && t - since >= 400 && t - last >= 120) {
        last = t;
        return now;
    }
    return 0;
}

void Navigate(Screen& s, void* list) {
    const int vis = Shown(s);
    const bool front = features::GameFocused();
    int16_t& row = At<int16_t>(list, 0x28);
    const int step = front ? WasdStep() : 0;
    if (step) row = static_cast<int16_t>((row + step + vis) % vis);
    uint32_t pad = 0;
    for (int j = 0, n = std::min(Global<int>(kJoystickCount), 8); front && j < n; ++j) pad |= features::ReadJoystick(j);
    const bool nav = front && (step || features::KeyHeld(VK_UP) || features::KeyHeld(VK_DOWN) ||
                               (pad & (1u << 12 | 1u << 16 | 1u << 14 | 1u << 18)));
    if (nav) Scroll(s, list);
    else s.lastRow = row;
    // the real foreground only: background test runs never poll the mouse, so lZ keeps a stale value there
    const int wheel = GetForegroundWindow() == Global<HWND>(0x606A60) ? Global<int>(kMouseWheel) : 0;
    if (wheel < 0 && s.offset < MaxOffset(s)) ++s.offset;
    else if (wheel > 0 && s.offset > 0) --s.offset;
}

int __fastcall Update(void* screen, void*) {
    Screen& s = Of(screen);
    void* list = At<void*>(screen, 8);
    if (!IsList(s) && features::BarColourPickerOpen()) {   // the colour wheel has the input
        features::UpdateBarColourPicker();
        RefreshRows(s, list);
        return 0;
    }
    if (!IsList(s) && !PageAlive()) {   // the plugin failed meanwhile: back to the list (it shows why)
        SetState(kStateList);
        return 0;
    }
    Navigate(s, list);
    RefreshRows(s, list);
    const int16_t id = reinterpret_cast<int16_t(__thiscall*)(void*)>(kListInput)(list);
    const int k = id - s.firstId;
    if (k >= 0 && k < Shown(s)) {
        void* item = ItemAt(list, k);
        Select(s, s.offset + k, item ? At<int8_t>(item, 0x24) : 1);
        if (item && Spin(s, s.offset + k)) At<int8_t>(item, 0x24) = 1;
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(IsList(s) ? kStateFixMenu : kStateList);
    return 0;
}

void DrawScrollbar(const Screen& s, void* list) {
    const int vis = Shown(s);
    if (Count(s) <= vis) return;
    const float right = static_cast<float>(At<int16_t>(list, 0x3A) + At<int16_t>(list, 0x32));
    const float pitch = At<int16_t>(list, 0x38), top = At<int16_t>(list, 0x3C);
    const auto* sheet = Global<const uint8_t*>(kHudSheet);
    const float k = sheet ? *reinterpret_cast<const float*>(sheet + 0x24) : 1.0f;
    const float w = 6.0f * k, x = right + 10.0f * k;
    const float trackH = pitch * vis - 4.0f * k, y = top + 2.0f * k;
    const float thumbH = std::max(trackH * vis / Count(s), 8.0f * k);
    const float thumbY = y + (trackH - thumbH) * s.offset / std::max(1, MaxOffset(s));
    overlay::QueueRect(x, y, w, trackH, 0x80202A10);
    overlay::QueueRect(x, thumbY, w, thumbH, 0xFFE0D878);
}

// Words of `text` (game bytes) in lines no wider than `width` pixels, at most `maxLines` (the last one cut with "...").
std::vector<std::string> Wrap(void* font, const std::string& text, int width, int maxLines) {
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        const size_t sp = text.find(' ', i);
        const std::string word = text.substr(i, sp == std::string::npos ? std::string::npos : sp - i);
        i = sp == std::string::npos ? text.size() : sp + 1;
        const std::string tryLine = line.empty() ? word : line + " " + word;
        if (line.empty() || overlay::TextWidth(font, tryLine.c_str()) <= width) {
            line = tryLine;
            continue;
        }
        lines.push_back(line);
        line = word;
    }
    if (!line.empty()) lines.push_back(line);
    if (static_cast<int>(lines.size()) > maxLines) {
        lines.resize(maxLines);
        std::string& last = lines.back();
        while (!last.empty() && overlay::TextWidth(font, (last + "...").c_str()) > width) last.pop_back();
        last += "...";
    }
    for (std::string& l : lines)   // one long word
        while (l.size() > 3 && overlay::TextWidth(font, l.c_str()) > width) l.erase(l.size() - 4, 1);
    return lines;
}

int CursorRow(const Screen& s) {
    void* list = At<void*>(const_cast<uint8_t*>(s.obj), 8);
    return list ? s.offset + At<int16_t>(list, 0x28) : -1;
}

// The footer lines (game bytes) and whether they ask for something (highlighted).
std::vector<std::string> FooterLines(const Screen& s, bool& highlight) {
    highlight = false;
    std::vector<std::string> out;
    const int r = CursorRow(s);
    if (IsList(s)) {
        if (g_plugins.empty()) {
            out.push_back("PUT PLUGINS (.DLL + .PSE) INTO PSE\\PLUGINS IN THE GAME FOLDER");
            return out;
        }
        if (r < 0 || r >= static_cast<int>(g_plugins.size())) return out;
        const pse_ui::PluginEntry& e = g_plugins[r];
        out.push_back(features::GameText(e.name.c_str(), true) + (e.version.empty() ? "" : " " + e.version) + " - " +
                      pse_ui::StateText(e.state));
        if (e.state != pse::PluginState::Loaded) {
            out.push_back(Upper(e.reason.empty() ? std::string("see PSE\\Logs\\pse_loader.log") : e.reason));
            highlight = true;
        } else if (e.host) {
            out.push_back("ITS SETTINGS: OPTIONS > DESERTSTORMFIX AND OPTIONS > VIDEO");
        } else if (e.settings) {
            char b[64];
            std::snprintf(b, sizeof b, "%zu SETTING%s - SELECT TO CHANGE", e.settings, e.settings == 1 ? "" : "S");
            out.push_back(b);
        } else {
            out.push_back("NO SETTINGS");
        }
        return out;
    }
    if (r >= 0 && r < static_cast<int>(g_rows.size())) {
        const pse_ui::Row& row = g_rows[r];
        if (!row.help.empty()) out.push_back(Upper(row.help));
        if (row.kind == pse_ui::RowKind::Colour && row.help.empty()) out.push_back("SELECT: COLOUR WHEEL");
        else if (row.kind == pse_ui::RowKind::String) out.push_back("TEXT SETTING - SHOWN ONLY");
    }
    bool restart = false, nextLevel = false;
    for (const pse_ui::Row& row : g_rows) {
        if (!pse_ui::Editable(row)) continue;
        restart |= pse_ui::NeedsRestart(row.flags);
        nextLevel |= (row.flags & PSE_SETTING_NEXT_LEVEL) && !(row.flags & PSE_SETTING_LIVE);
    }
    if (RestartPending()) {
        out.push_back("RESTART THE GAME TO APPLY");
        highlight = true;
    } else if (restart || nextLevel) {
        out.push_back(std::string(restart ? "* AFTER RESTARTING THE GAME" : "") + (restart && nextLevel ? "     " : "") +
                      (nextLevel ? "+ FROM THE NEXT LEVEL" : ""));
    }
    return out;
}

void DrawFooter(const Screen& s) {
    const auto* rr = Global<const uint8_t*>(kRenderer);
    void* font = overlay::MenuFont();
    if (!rr || !font) return;
    const int W = *reinterpret_cast<const int*>(rr + 0x40688), H = *reinterpret_cast<const int*>(rr + 0x4068C);
    bool highlight = false;
    std::vector<std::string> lines;
    for (const std::string& l : FooterLines(s, highlight))
        for (const std::string& w : Wrap(font, l, W * 68 / 100, 2)) lines.push_back(w);   // clear of Select / Back
    if (lines.size() > 3) lines.erase(lines.begin(), lines.end() - 3);
    const int step = std::max(12, static_cast<int>(H * 0.032f));
    int y = static_cast<int>(H * 0.92f) - step * (static_cast<int>(lines.size()) - 1);
    for (size_t i = 0; i < lines.size(); ++i, y += step) {
        const bool last = i + 1 == lines.size();
        overlay::DrawLabel(font, lines[i].c_str(), (W - overlay::TextWidth(font, lines[i].c_str())) / 2, y,
                           highlight && last ? 0xFFE0D878 : 0xFFC8C8C8);
    }
}

void __fastcall Draw(void* screen, void*) {
    Screen& s = Of(screen);
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, s.title, 1);
    if (!IsList(s) && features::DrawBarColourPicker(screen)) return;
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
    DrawScrollbar(s, list);
    DrawFooter(s);
}

void Build(Screen& s) {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptCtor)(s.obj);
    std::memcpy(s.vtable, reinterpret_cast<void*>(kOptVtable), sizeof s.vtable);
    At<void**>(s.obj, 0) = s.vtable;
    s.vtable[6] = reinterpret_cast<void*>(kMainMenuEnter);   // enter runs the set-up every time
    s.vtable[1] = reinterpret_cast<void*>(&Create);
    s.vtable[3] = reinterpret_cast<void*>(&Setup);
    s.vtable[4] = reinterpret_cast<void*>(&Update);
    s.vtable[5] = reinterpret_cast<void*>(&Draw);
    s.built = Create(s.obj, nullptr);
    dslog::Write(s.built ? "Plugins menu: screen 0x%X built" : "[fail] Plugins menu: screen 0x%X not built", s.state);
}

const char* Keep(std::string& slot, std::string text) {
    slot = std::move(text);
    return slot.c_str();
}
}  // namespace

void* features::PluginsMenuScreen(uint32_t state) {
    Screen* s = state == kStateList ? &g_list : state == kStatePage ? &g_page : nullptr;
    if (!s) return nullptr;
    if (!s->built && Global<void*>(kMainMenuScreen)) Build(*s);
    return s->built ? s->obj : nullptr;
}

const char* features::PluginsMenuText(uint32_t hash) {
    if ((hash & 0xFFFF0000u) != 0xC0130000u) return nullptr;
    if (hash == kTextListTitle) return "PLUGINS";
    if (hash == kTextPageTitle) return g_pageTitle.c_str();
    for (Screen* s : {&g_list, &g_page}) {
        if (hash >= s->row0 && hash < s->row0 + kVisible) {
            const int k = static_cast<int>(hash - s->row0);
            return Keep(s->labels[k], Label(*s, s->offset + k));
        }
        if (hash >= s->val0 && hash < s->val0 + kVisible * 4) {
            const int k = static_cast<int>(hash - s->val0) / 4, j = static_cast<int>(hash - s->val0) % 4;
            if (j > 2) return "";
            return Keep(s->values[k][j], Value(*s, s->offset + k, j));
        }
    }
    return nullptr;
}

void features::NotePluginSettingsAtStart() {
    const PSESettingsInterface* set = Settings();
    if (!set) return;
    const PSESettingDesc* d = nullptr;
    PSEPluginHandle who = 0;
    for (uint32_t i = 0; set->Describe(0, i, &d, &who) == PSE_OK; ++i)
        if (d && d->key && d->type != PSE_SETTING_STRING) g_atStart[{who, d->key}] = set->GetInt(who, d->key);
}

std::string features::PluginsMenuSummary() {
    int loaded = 0, problems = 0;
    for (const pse::PluginLoadResult& r : pse::PluginResults()) {
        if (r.name == "dsfix") continue;   // DesertStormFix itself
        if (r.state == pse::PluginState::Loaded) ++loaded;
        else ++problems;
    }
    char b[64];
    if (problems) std::snprintf(b, sizeof b, "%d LOADED, %d NOT", loaded, problems);
    else std::snprintf(b, sizeof b, "%d LOADED", loaded);
    return b;
}

features::PluginsMenuDebug features::PluginsMenuState() {
    PluginsMenuDebug d;
    const uint32_t state = Global<uint32_t>(kFrontEndState);
    Screen* s = state == kStateList && g_list.built ? &g_list : state == kStatePage && g_page.built ? &g_page : nullptr;
    if (!s) return d;
    d.screen = IsList(*s) ? 1 : 2;
    d.row = CursorRow(*s);
    d.offset = s->offset;
    d.plugin = IsList(*s) ? std::string() : g_pageName;
    for (int r = 0; r < Count(*s); ++r) {
        d.labels.push_back(Label(*s, r));
        d.values.push_back(Value(*s, r, 1));
    }
    bool highlight = false;
    d.footer = FooterLines(*s, highlight);
    return d;
}

bool features::PluginsMenuOpen(const char* plugin) {
    if (!plugin || !*plugin) {
        SetState(kStateList);
        return Global<uint32_t>(kFrontEndState) == kStateList;
    }
    const PSEPluginHandle h = pse::FindPlugin(plugin);
    if (!h) return false;
    BuildList();
    for (size_t i = 0; i < g_plugins.size(); ++i)
        if (g_plugins[i].handle == h) g_listRow = static_cast<int>(i);
    g_pageOwner = h;
    g_pageName = plugin;
    const PSEPluginInfo* info = pse::PluginInfo(h);
    g_pageTitle = Upper(info && info->displayName ? info->displayName : plugin, 40);
    SetState(kStatePage);
    return Global<uint32_t>(kFrontEndState) == kStatePage;
}

namespace {
Screen* OpenScreen() {
    const uint32_t state = Global<uint32_t>(kFrontEndState);
    if (state == kStateList && g_list.built) return &g_list;
    if (state == kStatePage && g_page.built) return &g_page;
    return nullptr;
}
}  // namespace

// Dev server: what Up / Down / Enter / Left / Right / Esc do, without the game's key path (posted keys don't reliably
// reach the menus in background test runs).
bool features::PluginsMenuSetRow(int row) {
    Screen* s = OpenScreen();
    if (!s || row < 0 || row >= Count(*s)) return false;
    void* list = At<void*>(s->obj, 8);
    if (!list) return false;
    const int vis = Shown(*s);
    if (row < s->offset) s->offset = row;
    else if (row >= s->offset + vis) s->offset = row - vis + 1;
    s->offset = std::min(s->offset, MaxOffset(*s));
    At<int16_t>(list, 0x28) = static_cast<int16_t>(row - s->offset);
    s->lastRow = row - s->offset;
    RefreshRows(*s, list);
    return true;
}

bool features::PluginsMenuActivate(int dir) {
    Screen* s = OpenScreen();
    if (!s || (!IsList(*s) && features::BarColourPickerOpen())) return false;
    const int r = CursorRow(*s);
    if (r < 0 || r >= Count(*s)) return false;
    Select(*s, r, dir < 0 ? 0 : dir > 0 ? 2 : 1);
    return true;
}

bool features::PluginsMenuBack() {
    Screen* s = OpenScreen();
    if (!s) return false;
    SetState(IsList(*s) ? kStateFixMenu : kStateList);
    return true;
}

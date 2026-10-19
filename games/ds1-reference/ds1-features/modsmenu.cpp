// Main menu MODS: every folder in <game>\Mods with ON / OFF. Choosing one switches it by renaming
// the folder - "_" in front = off, the same rule mods.cpp uses when it indexes the folder (and what the player readme
// tells modders) - so the menu and Explorer always agree. Applying (hot reload, features::ReloadMods - no restart) is a
// prompt in the game's own prompt line between "Select" and "Back" (so it looks like every other prompt instead of
// an extra row): "[Tab / Square] Apply n changes" while switches are pending, "Changes applied" for a few
// seconds after; Tab, a pad's Square or a click on the prompt. Icon queued, text drawn with the game's font during the
// screen's draw, so both follow the menu transitions.
//
// Main menu item 0x402 (coop.cpp's MainMenuSetup, pool item 5, after VERSUS) opens state 0x43. The screen is an
// object of the Options sub-screen class (like VERSUS / the CONTROLS screens) with a copied vtable; its rows are
// our text hashes, answered with the live folder state by ModsMenuText.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kSetState = 0x47EF20, kBackPressed = 0x478380, kListSelected = 0x4E5D80;
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kItemCtor = 0x4E4F90, kItemSet = 0x4E4FB0;
constexpr uint32_t kDrawPanel = 0x456EB0, kDrawTitle = 0x456F40;
constexpr uint32_t kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kMainMenuScreen = 0x617A00;
constexpr uint32_t kMainMenuEnter = 0x4153B0;
constexpr uint32_t kOptCtor = 0x409B90, kOptVtable = 0x5D8510, kOptSetup = 0x409C90, kOptItemDrawCb = 0x409E90;
constexpr uint32_t kStateMainMenu = 1;
constexpr int kMaxMods = 256, kVisible = 8, kPoolRows = kVisible + 1, kVtableEntries = 21;  // + a filler row
constexpr uint16_t kItemRow0 = 0x850, kItemFiller = 0x86D, kItemNote = 0x86E, kItemNote2 = 0x86F;
enum : uint32_t {
    kTextTitle = 0xC00A0001, kTextItem, kTextBlank, kTextNone, kTextNone2, kTextRow0 = 0xC00A0010
};

struct Mod {
    std::string name;  // without the "_"
    bool on;
};
std::vector<Mod> g_mods;
int g_offset = 0;   // first mod shown (scrolling window)
const char* g_applyResult = nullptr;  // shown on the button for a while after APPLY
DWORD g_applyAt = 0;
int g_lastRow = 0;  // list row selected last frame
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

std::string ModsDir() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    return dir.substr(0, dir.find_last_of("\\/")) + "\\Mods";
}

// The folders as they are on disk now (".xxx" folders are the modder's own business and not listed).
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

void Toggle(int i) {
    Mod& m = g_mods[i];
    const std::string dir = ModsDir() + "\\";
    const std::string from = dir + (m.on ? "" : "_") + m.name, to = dir + (m.on ? "_" : "") + m.name;
    if (!MoveFileA(from.c_str(), to.c_str())) {
        dslog::Write("[fail] Mods menu: %s -> %s (error %lu)", from.c_str(), to.c_str(), GetLastError());
        return;
    }
    m.on = !m.on;
    g_applyResult = nullptr;
    dslog::Write("Mods menu: %s switched %s (APPLY to use it)", m.name.c_str(), m.on ? "on" : "off");
}

bool __fastcall Create(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, *reinterpret_cast<void**>(kLargeFont), *reinterpret_cast<void**>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, kPoolRows)) return false;
    for (int i = 0; i < kPoolRows; ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x24, 0x1E);
        if (item) reinterpret_cast<void*(__thiscall*)(void*)>(kItemCtor)(item);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    }
    return true;
}

// Switches that differ from the mods in use: folders switched on that aren't loaded, loaded ones switched off / gone.
int PendingChanges() {
    const std::vector<std::string>& loaded = features::LoadedMods();
    auto isLoaded = [&](const std::string& name) {
        for (const std::string& l : loaded)
            if (_stricmp(l.c_str(), name.c_str()) == 0) return true;
        return false;
    };
    int n = 0;
    for (const Mod& m : g_mods) n += m.on != isLoaded(m.name);
    for (const std::string& l : loaded) {
        bool listed = false;
        for (const Mod& m : g_mods) listed |= _stricmp(m.name.c_str(), l.c_str()) == 0;
        n += !listed;
    }
    return n;
}
bool Pending() { return PendingChanges() > 0; }

int VisibleMods() { return std::min(kVisible, static_cast<int>(g_mods.size())); }
int MaxOffset() { return std::max(0, static_cast<int>(g_mods.size()) - kVisible); }

// The list draws every row it has (no scrolling), so it gets a window of kVisible rows whose texts show
// g_mods[g_offset + row] (looked up every frame); at least two rows (the class's set-up always adds two).
void Fill(void* screen) {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);  // list layout for its two rows + two items
    void* list = At<void*>(screen, 8);
    int rows = 0;
    if (g_mods.empty()) {
        SetItem(ItemAt(list, rows++), kItemNote, kTextNone);
        SetItem(ItemAt(list, rows++), kItemNote2, kTextNone2);
    } else {
        for (int k = 0; k < VisibleMods(); ++k)
            SetItem(ItemAt(list, rows++), static_cast<uint16_t>(kItemRow0 + k), kTextRow0 + k);
        if (rows < 2) SetItem(ItemAt(list, rows++), kItemFiller, kTextBlank);
    }
    for (int i = 2; i < rows; ++i) AddToList(list, ItemAt(list, i));  // the set-up added the first two
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (rows - 2) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = static_cast<int16_t>(rows);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kTextTitle, 0, 0, 0x7FFF);
}

// Every time the screen is entered: the folders again, from the top.
void __fastcall Setup(void* screen, void*) {
    Scan();
    g_offset = 0;
    g_lastRow = 0;
    Fill(screen);
}

// Scrolling: moving down past the last row (the list wraps it to the first) while mods remain below, or up past the
// first (wrapped to the last) while mods remain above, moves the window instead of the cursor; at the very end / top
// the wrap goes through and the window jumps with it.
void Scroll(void* list) {
    const int vis = VisibleMods();
    if (vis <= 1) return;
    int16_t& row = At<int16_t>(list, 0x28);
    const int prev = g_lastRow, now = row;
    if (prev == vis - 1 && now == 0) {          // down past the last row
        if (g_offset < MaxOffset()) {
            ++g_offset;
            row = static_cast<int16_t>(vis - 1);
        } else {
            g_offset = 0;                       // wrap to the top
        }
    } else if (prev == 0 && now == vis - 1) {   // up past the first row
        if (g_offset > 0) {
            --g_offset;
            row = 0;
        } else {
            g_offset = MaxOffset();             // wrap to the end
        }
    }
    g_lastRow = row;
}

// Navigation: the mouse only highlights a row (and clicks toggle it) - it never scrolls. The game's
// list follows the pointer every frame, so hovering the last row used to look like "down past the end" again and again
// (the list raced). The window now moves only while a navigation input is held: Up / Down arrows (the list's own),
// W / S (handled here - the menus only know the arrows), a pad's D-pad / left stick; and the mouse wheel scrolls it.
constexpr uint32_t kGameWindow = 0x606A60, kJoystickCount = 0x754BE8, kMouseWheel = 0x754C24 + 8;  // DIMOUSESTATE lZ

bool Held(int vk) { return features::KeyHeld(vk); }

// W / S as Up / Down: first press, then repeating after 400 ms every 120 ms (like the arrows).
int WasdStep() {
    static int dir = 0;
    static DWORD since = 0, last = 0;
    const int now = Held('W') ? -1 : Held('S') ? 1 : 0;
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

void Navigate(void* list) {
    const int vis = VisibleMods();
    if (vis <= 0) return;
    const bool front = features::GameFocused();
    int16_t& row = At<int16_t>(list, 0x28);
    const int step = front ? WasdStep() : 0;
    if (step) row = static_cast<int16_t>((row + step + vis) % vis);
    uint32_t pad = 0;
    for (int j = 0, n = std::min(*reinterpret_cast<int*>(kJoystickCount), 8); front && j < n; ++j)
        pad |= features::ReadJoystick(j);
    const bool nav = front && (step || Held(VK_UP) || Held(VK_DOWN) || (pad & (1u << 12 | 1u << 16 | 1u << 14 | 1u << 18)));
    if (nav) Scroll(list);
    else g_lastRow = row;  // pointer moves: just follow the highlight
    const int wheel = front ? *reinterpret_cast<int*>(kMouseWheel) : 0;
    if (wheel < 0 && g_offset < MaxOffset()) ++g_offset;
    else if (wheel > 0 && g_offset > 0) --g_offset;
}

// ---- the Apply prompt ----
constexpr uint32_t kMouseX = 0x754C18, kMouseY = 0x754C1C, kMouseButtons = 0x754C24 + 12;  // DIMOUSESTATE rgbButtons
constexpr uint32_t kRenderer = 0x63C924;
struct Prompt {
    bool shown = false;
    float x = 0, y = 0, w = 0, h = 0;  // the clickable area
} g_prompt;

bool Hovered() {
    const float mx = static_cast<float>(*reinterpret_cast<int*>(kMouseX));
    const float my = static_cast<float>(*reinterpret_cast<int*>(kMouseY));
    return g_prompt.shown && mx >= g_prompt.x && mx < g_prompt.x + g_prompt.w && my >= g_prompt.y &&
           my < g_prompt.y + g_prompt.h;
}

// Tab, a pad's Square (button 3 in the game's numbering) or a left click on the prompt - each on its press.
bool ApplyPressed() {
    static bool tab = false, square = false, click = false;
    const bool front = features::GameFocused();
    uint32_t pad = 0;
    for (int j = 0, n = std::min(*reinterpret_cast<int*>(kJoystickCount), 8); front && j < n; ++j)
        pad |= features::ReadJoystick(j);
    const bool tabNow = front && Held(VK_TAB), squareNow = (pad & (1u << 3)) != 0;
    const bool clickNow = front && (*reinterpret_cast<uint8_t*>(kMouseButtons) & 0x80) && Hovered();
    const bool pressed = (tabNow && !tab) || (squareNow && !square) || (clickNow && !click);
    tab = tabNow, square = squareNow, click = clickNow;
    return pressed;
}

// In the prompt line, centred between the game's "Select" (left) and "Back" (right), in the same style (menu font,
// white, icon 1.5 x the capitals - like the CONTROLS screens' prompts).
void DrawApplyPrompt() {
    g_prompt.shown = false;
    const auto* r = *reinterpret_cast<const uint8_t* const*>(kRenderer);
    void* font = overlay::MenuFont();
    if (!r || !font || g_mods.empty()) return;
    const int W = *reinterpret_cast<const int*>(r + 0x40688), H = *reinterpret_cast<const int*>(r + 0x4068C);
    const int n = PendingChanges();
    char text[64];
    if (n) snprintf(text, sizeof text, "Apply %d change%s", n, n == 1 ? "" : "s");
    else if (g_applyResult && GetTickCount() - g_applyAt < 4000) snprintf(text, sizeof text, "%s", g_applyResult);
    else return;
    const int cap = overlay::TextWidth(font, "M"), iconSize = std::max(12, cap * 3 / 2), gap = iconSize / 4;
    const int textW = overlay::TextWidth(font, text), y = static_cast<int>(H * 0.92f);
    int x = (W - (n ? iconSize + gap : 0) - textW) / 2;
    if (n) {
        const overlay::Icon icon = features::KeyboardInUse() ? overlay::Icon::KbTab
                                   : features::PadStyleXbox() ? overlay::Icon::XbSquare
                                                              : overlay::Icon::PsSquare;
        overlay::QueueIcon(icon, static_cast<float>(x), static_cast<float>(y - cap / 2 - iconSize / 2),
                           static_cast<float>(iconSize));
        g_prompt = {true, static_cast<float>(x), static_cast<float>(y - cap / 2 - iconSize / 2),
                    static_cast<float>(iconSize + gap + textW), static_cast<float>(iconSize)};
        x += iconSize + gap;
    }
    overlay::DrawLabel(font, text, x, y, n ? 0xFFFFFFFF : 0xFFE0D878);
}

int __fastcall Update(void* screen, void*) {
    void* list = At<void*>(screen, 8);
    Navigate(list);
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(list);
    if (id >= kItemRow0 && id < kItemRow0 + VisibleMods()) Toggle(g_offset + (id - kItemRow0));
    if (ApplyPressed() && Pending()) {
        g_applyResult = features::ReloadMods() ? "Changes applied" : "Apply failed - see the log";
        g_applyAt = GetTickCount();
        Scan();  // (the folders as the reload saw them)
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateMainMenu);
    return 0;
}

// Scrollbar at the right of the mod rows: a track and a thumb sized to the part of the list on screen.
void DrawScrollbar(void* list) {
    const int n = static_cast<int>(g_mods.size()), vis = VisibleMods();
    if (n <= vis || vis <= 0) return;
    const float right = static_cast<float>(At<int16_t>(list, 0x3A) + At<int16_t>(list, 0x32));  // rows' right end
    const float pitch = At<int16_t>(list, 0x38), top = At<int16_t>(list, 0x3C);
    const auto* sheet = *reinterpret_cast<const uint8_t* const*>(kHudSheet);
    const float k = sheet ? *reinterpret_cast<const float*>(sheet + 0x24) : 1.0f;
    const float w = 6.0f * k, x = right + 10.0f * k;  // in the panel's border, right of the rows
    const float trackH = pitch * vis - 4.0f * k, y = top + 2.0f * k;
    const float thumbH = std::max(trackH * vis / n, 8.0f * k);
    const float thumbY = y + (trackH - thumbH) * g_offset / std::max(1, MaxOffset());
    overlay::QueueRect(x, y, w, trackH, 0x80202A10);
    overlay::QueueRect(x, thumbY, w, thumbH, 0xFFE0D878);
}

void __fastcall Draw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kTextTitle, 1);
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
    DrawScrollbar(list);
    DrawApplyPrompt();
}

// ---- main menu: "DESERTSTORMFIX x.y.z INSTALLED" / "N MODS INSTALLED (M ACTIVE)" at the bottom left ----
// Drawn by the main menu's own draw (class vtable 0x5D86B8 +0x14 wrapped), so it fades / zooms with the menu.
constexpr uint32_t kMainMenuDrawSlot = 0x5D86B8 + 0x14;
using DrawFn = void(__thiscall*)(void*);
DrawFn g_mainMenuDraw = nullptr;

int InstalledMods() {  // folders in Mods, counted again every 2 s at most
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

void __fastcall MainMenuDraw(void* screen, void*) {
    g_mainMenuDraw(screen);
    const auto* r = *reinterpret_cast<const uint8_t* const*>(0x63C924);
    void* font = overlay::MenuFont();
    if (!r || !font) return;
    const int H = *reinterpret_cast<const int*>(r + 0x4068C);
    const auto* sheet = *reinterpret_cast<const uint8_t* const*>(kHudSheet);
    const float k = sheet ? *reinterpret_cast<const float*>(sheet + 0x24) : 1.0f;
    char line1[64], line2[128];
    snprintf(line1, sizeof line1, "DESERTSTORMFIX %s INSTALLED", DS_VERSION);
    const int installed = InstalledMods(), active = features::ModCount(), problems = features::ModProblems();
    // problems: "[fail]" lines in Mods\DesertStormFix-report.txt (release builds have no other log)
    if (problems > 0)
        snprintf(line2, sizeof line2, "%d MOD%s INSTALLED (%d ACTIVE) - %d PROBLEM%s, SEE MODS\\DESERTSTORMFIX-REPORT.TXT",
                 installed, installed == 1 ? "" : "S", active, problems, problems == 1 ? "" : "S");
    else
        snprintf(line2, sizeof line2, "%d MOD%s INSTALLED (%d ACTIVE)", installed, installed == 1 ? "" : "S", active);
    const int left = static_cast<int>(50.0f * k), lineH = static_cast<int>(16.0f * k);  // bottom left, under the prompt
    const int y2 = static_cast<int>(H * 0.972f), y1 = y2 - lineH;  // below the menu panel
    overlay::DrawLabel(font, line1, left, y1, 0xFFE0D878);
    overlay::DrawLabel(font, line2, left, y2, 0xFFC8C8C8);
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
    dslog::Write(g_built ? "Mods menu: screen built" : "[fail] Mods menu: screen not built");
}
}  // namespace

void* features::ModsMenuScreen() { return g_built ? g_screen : nullptr; }

uint32_t features::ModsMenuItemText() { return kTextItem; }

const char* features::ModsMenuText(uint32_t hash) {
    switch (hash) {
        case kTextTitle:
        case kTextItem: return "MODS";
        case kTextBlank: return " ";
        case kTextNone: return "NO MODS FOUND";
        case kTextNone2: return "PUT MOD FOLDERS IN THE GAME'S MODS FOLDER";
        default: break;
    }
    if (hash >= kTextRow0 && hash < kTextRow0 + kVisible) {
        static char rows[kVisible][64];
        const size_t k = hash - kTextRow0, i = g_offset + k;
        if (i >= g_mods.size()) return "";
        std::string name = g_mods[i].name.substr(0, 40);
        for (char& c : name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        snprintf(rows[k], sizeof rows[k], "%s:  %s", name.c_str(), g_mods[i].on ? "ON" : "OFF");
        return rows[k];
    }
    return nullptr;
}

void features::ApplyModsMenu() {
    if (g_mainMenuDraw) return;
    g_mainMenuDraw = *reinterpret_cast<DrawFn*>(kMainMenuDrawSlot);
    const uint32_t ours = reinterpret_cast<uint32_t>(&MainMenuDraw);
    if (!patch::WriteValue(kMainMenuDrawSlot, ours)) g_mainMenuDraw = nullptr;
}

void features::OnFrameModsMenu() {
    static bool tried = false;
    if (!tried && *reinterpret_cast<void**>(kMainMenuScreen)) {
        tried = true;
        Build();
    }
}

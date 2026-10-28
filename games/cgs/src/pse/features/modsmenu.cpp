// MODS in Conflict: Global Storm's own front end: a MODS item in the main menu (after OPTIONS) that opens a MODS
// screen with one row per Mods\<name> folder ("NAME: ON" / "NAME: OFF"); Enter on a row switches the folder on / off
// (features::SetModEnabled: '_' prefix rename + re-scan of the redirect). Built from the game's own GUI objects,
// driven by the game's own menu input, navigation and Back - no dialog, overlay or hotkey.
//
// The front end is the GUI library's data (catalog.dat: Interface.gui + *.gsc screen files), parsed by plain
// (non-VM) code: screen file loader 0x597810 thiscall(screen set = [[0x736B20]+0x28], name, platforms, ver*) - its
// screens go to set +0x2D0 (count byte +0x2D4, 250 slots), then the file's event table (event -> action per control).
// After Main.gsc is loaded this adds, with the game's own parsers on byte copies of records from the player's
// Main.gsc (loaded again with the game's whole-file loader 0x52357C):
//   - BN_MODS: a copy of the main menu's BN_CHEATS button record parsed into MENU_MAIN_SCREEN (container loader
//     0x5A0270 appends to control +0x40), renamed (+0x14 hash), made visible (+0x20 |= 0x8000 - BN_CHEATS is hidden
//     until cheats are unlocked), moved after BN_OPTIONS in the list (the menu stacks its visible buttons in list
//     order), its text control's key (+0x48) = DSFIX_MODS, event Action (4) -> Select Screen (0) SCR_DSFIX_MODS;
//   - SCR_DSFIX_MODS: a copy of the SCR_GAME_MENU screen record parsed into a plain GUI screen (ctor 0x59E380,
//     vtable = a copy of the base 0x6D7D88 with the message handler +0x1C ours and SCR_GAME_MENU's slide transitions),
//     SCR_GAME_MENU's companion screens (navigation bars + prompts in front, backgrounds behind), its menu emptied
//     and filled with up to 8 copies of the BN_OPTIONS button (event Action -> Notify (2) = handler message 4 with
//     the control); more mods than rows = the last row pages ("MORE MODS - PAGE x OF y"); title key
//     DSFIX_MODS_TITLE; events Cancel (5) -> Back Screen (7), menu Up / Down -> previous / next (5 / 4).
// Everything we parse is allocated from our own buffer (ArenaSwap) - the GUI arena has no slack.
// Texts: the catalog lookup 0x686A10 thiscall(catalog, key hash) -> wchar_t* (text controls look their key up every
// draw) answers our keys. Names hash with the archive LFSR over the lower-cased name (SCR_GAME_MENU = 0xF23EA323).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kGscLoad = 0x597810;        // thiscall(set, name, platforms, ver*) ret 0xC
constexpr uint32_t kLoadFile = 0x52357C;       // cdecl(name, void** buf, uint32_t* size, int, int flags) -> nonzero
constexpr uint32_t kFree = 0x55EB77;           // cdecl(void*) - frees what kLoadFile returned
constexpr uint32_t kAlloc = 0x5A1E10;          // cdecl(size, align) - the GUI's allocator
constexpr uint32_t kChildrenLoad = 0x5A0270;   // thiscall(container, uint8_t** pos, ver*) ret 8: [count][records]
constexpr uint32_t kFindChild = 0x5A0160;      // thiscall(container, hash) ret 4: direct child or 0
constexpr uint32_t kAddEvent = 0x59FC40;       // thiscall(control, event) ret 4 -> entry {event, action, target, ctrl}
constexpr uint32_t kScreenCtor = 0x59E380;     // thiscall(screen, 0) ret 4 (0x64 bytes)
constexpr uint32_t kScreenVtable = 0x6D7D88;   // 24 methods (+0x5C Load(pos*, ver*), +0x1C Message(msg, param))
constexpr uint32_t kCatalogLookup = 0x686A10;  // thiscall(catalog, hash) ret 4 -> wchar_t*
constexpr uint32_t kSetFocus = 0x5A03A0;       // thiscall(child container, control) ret 4
constexpr uint32_t kGameMenuVtable = 0x6CE710; // SCR_GAME_MENU's class (slide transitions at +0x44 / +0x48)

constexpr uint8_t kGscLoadBytes[] = {0x33, 0xC0, 0x8A, 0x44, 0x24, 0x08, 0x83, 0xEC, 0x08};
constexpr uint8_t kCatalogBytes[] = {0x8B, 0x41, 0x38, 0x85, 0xC0};

constexpr int kEvUp = 2, kEvDown = 3, kEvAction = 4, kEvCancel = 5;
constexpr int kActSelectScreen = 0, kActNotify = 2, kActNext = 4, kActPrev = 5, kActBack = 7;
constexpr int kMsgNotify = 4;
constexpr int kMaxRows = 8;            // rows that fit between the top and bottom bars (as the main menu with MODS)
constexpr uint32_t kVisible = 0x8000;  // control +0x20 flag: drawn and reachable (hidden buttons such as BN_CHEATS lack it)

uint32_t Hash(const char* s) {
    uint32_t x = 1;
    for (; *s; ++s) {
        uint32_t c = static_cast<unsigned char>(*s);
        if (c >= 'A' && c <= 'Z') c += 32;
        for (int b = 0; b < 8; ++b) {
            uint32_t in = ((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1;
            x = (x << 1) | in;
        }
    }
    return x;
}

using GscLoadFn = int(__fastcall*)(void* set, void*, const char* name, int platforms, void* ver);
using LookupFn = const wchar_t*(__fastcall*)(void* catalog, void*, uint32_t hash);
using MessageFn = int(__fastcall*)(void* screen, void*, int msg, void* param);
GscLoadFn g_nextGscLoad = nullptr;
LookupFn g_nextLookup = nullptr;

struct Row {
    std::string folder;   // as on disk (may start with '_')
    std::wstring text;    // "NAME: ON"
    bool enabled = false;
};
std::vector<Row> g_rows;   // every Mods\ folder
bool g_noMods = false;
uint32_t g_hMods = 0, g_hTitle = 0, g_hScreen = 0, g_hButton = 0, g_hOptionsText = 0;
uint32_t g_rowCtrl[kMaxRows] = {}, g_rowText[kMaxRows] = {};
void* g_rowObj[kMaxRows] = {};
int g_rowCount = 0;        // row controls built
int g_perPage = 0;         // mod rows per page (kMaxRows - 1 with the MORE MODS row, else all)
int g_page = 0;
std::wstring g_moreText;
const wchar_t kModsText[] = L"MODS";
const wchar_t kNoModsText[] = L"NO MODS IN THE MODS FOLDER";
void* g_vtable[24] = {};

void* Alloc(uint32_t size) { return reinterpret_cast<void*(__cdecl*)(uint32_t, uint32_t)>(kAlloc)(size, 4); }
uint8_t* U8(void* p, uint32_t off) { return static_cast<uint8_t*>(p) + off; }
uint32_t& U32(void* p, uint32_t off) { return *reinterpret_cast<uint32_t*>(U8(p, off)); }
void*& Ptr(void* p, uint32_t off) { return *reinterpret_cast<void**>(U8(p, off)); }

int Pages() { return g_perPage && !g_rows.empty() ? (static_cast<int>(g_rows.size()) + g_perPage - 1) / g_perPage : 1; }
bool HasMoreRow() { return g_rowCount == kMaxRows && static_cast<int>(g_rows.size()) > kMaxRows; }
// Row i -> index into g_rows, or -1 (the MORE MODS row / an empty row on the last page).
int ModOfRow(int i) {
    if (HasMoreRow() && i == kMaxRows - 1) return -1;
    int m = g_page * g_perPage + i;
    return m < static_cast<int>(g_rows.size()) ? m : -1;
}
void ShowPage() {
    for (int i = 0; i < g_rowCount; ++i) {
        if (!g_rowObj[i]) continue;
        bool shown = g_noMods || ModOfRow(i) >= 0 || (HasMoreRow() && i == kMaxRows - 1);
        if (shown) U32(g_rowObj[i], 0x20) |= kVisible;
        else U32(g_rowObj[i], 0x20) &= ~kVisible;
    }
    wchar_t t[64];
    swprintf_s(t, L"MORE MODS - PAGE %d OF %d", g_page + 1, Pages());
    g_moreText = t;
}

void* FindChild(void* container, uint32_t hash) {
    return reinterpret_cast<void*(__fastcall*)(void*, void*, uint32_t)>(kFindChild)(container, nullptr, hash);
}

// Pointer arrays {+4 array, +8 count byte, +0xA capacity byte} (child containers, control event tables) grown to
// hold `extra` more: the game's loaders only allocate when the capacity is 0 and never grow.
void Reserve(void* table, int extra) {
    uint8_t count = *U8(table, 8), cap = *U8(table, 0xA);
    if (count + extra <= cap) return;
    int ncap = count + extra;
    if (ncap > 255) ncap = 255;
    void** fresh = static_cast<void**>(Alloc(static_cast<uint32_t>(ncap) * 4));
    memset(fresh, 0, static_cast<size_t>(ncap) * 4);
    if (count) memcpy(fresh, Ptr(table, 4), count * 4u);
    Ptr(table, 4) = fresh;
    *U8(table, 0xA) = static_cast<uint8_t>(ncap);
}

void AddEvent(void* ctrl, int event, int action, uint32_t target) {
    Reserve(U8(ctrl, 0), 1);   // the control's event table lives at +4 (array) / +8 / +0xA
    void* e = reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(kAddEvent)(ctrl, nullptr, event);
    U32(e, 4) = static_cast<uint32_t>(action);
    U32(e, 8) = target;
}

// Parses `copies` copies of one control record (bytes from its class dword on) into a child container. The
// container loader 0x5A0270 uses the control factory's result unchecked (null = crash at 0x5A0330).
bool ParseChildren(void* container, const uint8_t* rec, size_t len, int copies, void* ver) {
    std::vector<uint8_t> buf(4 + len * copies);
    U32(buf.data(), 0) = static_cast<uint32_t>(copies);
    for (int i = 0; i < copies; ++i) memcpy(buf.data() + 4 + len * i, rec, len);
    Reserve(container, copies);
    uint8_t* pos = buf.data();
    reinterpret_cast<void(__fastcall*)(void*, void*, uint8_t**, void*)>(kChildrenLoad)(container, nullptr, &pos, ver);
    if (pos != buf.data() + buf.size()) {
        dslog::Write("[fail] Mods menu: a control record parsed to %d of %u bytes - not added",
                     static_cast<int>(pos - buf.data()), static_cast<unsigned>(buf.size()));
        return false;
    }
    return true;
}

// Record offsets in a .gsc: control = [class][name hash][type hash][len][name]..., screen = [name hash][len][name]...
long FindRecord(const std::vector<uint8_t>& d, const char* name, bool screen) {
    uint32_t h = Hash(name), n = static_cast<uint32_t>(strlen(name));
    size_t lenAt = screen ? 4 : 8;
    for (size_t p = screen ? 0 : 4; p + lenAt + 4 + n <= d.size(); ++p) {
        if (U32(const_cast<uint8_t*>(d.data()), static_cast<uint32_t>(p)) != h) continue;
        if (U32(const_cast<uint8_t*>(d.data()), static_cast<uint32_t>(p + lenAt)) != n) continue;
        if (memcmp(d.data() + p + lenAt + 4, name, n)) continue;
        return static_cast<long>(screen ? p : p - 4);
    }
    return -1;
}

void RefreshRows() {
    g_rows.clear();
    for (auto& [folder, enabled] : features::ModList()) {
        Row r;
        r.folder = folder;
        r.enabled = enabled;
        g_rows.push_back(r);
    }
    for (Row& r : g_rows) {
        std::string bare = r.folder;
        while (!bare.empty() && (bare[0] == '_' || bare[0] == '.')) bare.erase(0, 1);
        r.text.clear();
        for (char c : bare) r.text += static_cast<wchar_t>(toupper(static_cast<unsigned char>(c)));
        r.text += r.enabled ? L": ON" : L": OFF";
    }
    // By the bare name: switching a mod off ('_' prefix on disk) must not move its row under the cursor.
    auto bare = [](const std::string& f) {
        size_t p = f.find_first_not_of("_.");
        return f.c_str() + (p == std::string::npos ? f.size() : p);
    };
    std::stable_sort(g_rows.begin(), g_rows.end(),
                     [&](const Row& a, const Row& b) { return _stricmp(bare(a.folder), bare(b.folder)) < 0; });
}

// Screens carry two lists of companion screens, {+0 array, +4 count byte, +6 capacity byte}: +0x50 shown in front
// (SCR_NAVIGATION_OVERLAY: the top / bottom bars and the button prompts), +0x58 behind (the menu backgrounds). The game
// classes' constructors fill them; a plain GUI screen has none, so the MODS screen borrows SCR_GAME_MENU's.
void* g_modsScreen = nullptr;
void* g_companions[2][8] = {};
bool g_companionsDone = false;
void CopyCompanions() {
    if (g_companionsDone || !g_modsScreen) return;
    void* gui = *reinterpret_cast<void**>(0x736B20);
    if (!gui) return;
    void* set = Ptr(gui, 0x28);
    void* menu = nullptr;
    uint8_t n = *U8(set, 0x2D4);
    for (int i = 0; i < n; ++i) {
        void* s = static_cast<void**>(Ptr(set, 0x2D0))[i];
        if (s && U32(s, 0x14) == 0xF23EA323) menu = s;
    }
    if (!menu || !*U8(menu, 0x50 + 4)) return;   // not filled yet
    for (int l = 0; l < 2; ++l) {
        uint32_t off = l ? 0x58 : 0x50;
        uint8_t c = *U8(menu, off + 4);
        if (c > 8) c = 8;
        memcpy(g_companions[l], Ptr(menu, off), c * sizeof(void*));
        Ptr(g_modsScreen, off) = g_companions[l];
        *U8(g_modsScreen, off + 4) = c;
        *U8(g_modsScreen, off + 6) = c;
    }
    g_companionsDone = true;
    dslog::Write("Mods menu: MODS screen shows SCR_GAME_MENU's companion screens (%u in front, %u behind)",
                 *U8(g_modsScreen, 0x54), *U8(g_modsScreen, 0x5C));
}

void* g_modsMenu = nullptr;    // the MODS screen's menu control
constexpr int kMsgOpened = 0x15, kMsgLoaded = 0x1E;

int __fastcall ScreenMessage(void* screen, void*, int msg, void* param) {
    CopyCompanions();
    if ((msg == kMsgOpened || msg == kMsgLoaded) && g_modsMenu) {
        // Focus the first row through the container's focus setter 0x5A03A0 thiscall(menu + 0x40, control): it
        // un-focuses the old one (container +0x10 = menu +0x50) and focuses the new one, which draws it highlighted.
        // Writing +0x50 alone (as the copy left it) took the input but never highlighted the row.
        RefreshRows();
        bool ours = false;
        for (int i = 0; i < g_rowCount; ++i) ours |= Ptr(g_modsMenu, 0x50) == g_rowObj[i];
        if (!ours) Ptr(g_modsMenu, 0x50) = nullptr;
        if (g_rowCount && g_rowObj[0]) {
            using FocusFn = void(__fastcall*)(void*, void*, void*);
            reinterpret_cast<FocusFn>(kSetFocus)(U8(g_modsMenu, 0x40), nullptr, g_rowObj[0]);
        }
    }
    if (msg == kMsgNotify && param) {
        uint32_t h = U32(param, 0x14);
        for (int i = 0; i < g_rowCount; ++i) {
            if (h != g_rowCtrl[i]) continue;
            if (g_noMods) return 1;
            if (HasMoreRow() && i == kMaxRows - 1) {
                g_page = (g_page + 1) % Pages();
                ShowPage();
                dslog::Write("Mods menu: page %d of %d", g_page + 1, Pages());
                return 1;
            }
            int m = ModOfRow(i);
            if (m < 0) return 1;
            std::string name = g_rows[m].folder;
            bool want = !g_rows[m].enabled;
            bool ok = features::SetModEnabled(name, want);
            dslog::Write("Mods menu: %s switched %s%s", name.c_str(), want ? "on" : "off", ok ? "" : " - rename failed");
            RefreshRows();
            return 1;
        }
    }
    return reinterpret_cast<MessageFn>(reinterpret_cast<void**>(kScreenVtable)[0x1C / 4])(screen, nullptr, msg, param);
}

const wchar_t* __fastcall Lookup(void* catalog, void* edx, uint32_t hash) {
    // The main menu (its OPTIONS text) or our screen is being drawn: SCR_GAME_MENU's lists are filled by now.
    if (!g_companionsDone && hash && (hash == g_hOptionsText || hash == g_hTitle)) CopyCompanions();
    if (hash && (hash == g_hMods || hash == g_hTitle)) return kModsText;
    for (int i = 0; i < g_rowCount; ++i) {
        if (!g_rowText[i] || hash != g_rowText[i]) continue;
        if (g_noMods) return kNoModsText;
        if (HasMoreRow() && i == kMaxRows - 1) return g_moreText.c_str();
        int m = ModOfRow(i);
        return m >= 0 ? g_rows[m].text.c_str() : L"";
    }
    return g_nextLookup(catalog, edx, hash);
}

void SetTextKey(void* parent, uint32_t childHash, uint32_t key) {
    void* t = FindChild(U8(parent, 0x40), childHash);
    if (t) U32(t, 0x48) = key;
    else dslog::Write("[fail] Mods menu: text control %08X not found", childHash);
}

void AddMainMenuItem(void* set, const std::vector<uint8_t>& d, void* ver) {
    long cheats = FindRecord(d, "BN_CHEATS", false), signin = FindRecord(d, "BN_SIGNIN_OUT", false);
    long menuScreen = FindRecord(d, "SCR_GAME_MENU", true);
    if (cheats < 0 || signin <= cheats || menuScreen < 0) {
        dslog::Write("[fail] Mods menu: main menu records not found in Main.gsc - no MODS item");
        return;
    }
    void* screen = nullptr;
    uint8_t n = *U8(set, 0x2D4);
    for (int i = 0; i < n; ++i) {
        void* s = static_cast<void**>(Ptr(set, 0x2D0))[i];
        if (s && U32(s, 0x14) == Hash("SCR_GAME_MENU")) screen = s;
    }
    void* menu = screen ? FindChild(U8(screen, 0x24), Hash("MENU_MAIN_SCREEN")) : nullptr;
    if (!menu) {
        dslog::Write("[fail] Mods menu: SCR_GAME_MENU / MENU_MAIN_SCREEN not loaded - no MODS item");
        return;
    }
    void* list = U8(menu, 0x40);
    if (!ParseChildren(list, d.data() + cheats, static_cast<size_t>(signin - cheats), 1, ver)) return;
    void** items = static_cast<void**>(Ptr(list, 4));
    uint8_t count = *U8(list, 8);
    void* item = items[count - 1];
    U32(item, 0x14) = g_hButton;
    U32(item, 0x20) |= kVisible;   // BN_CHEATS is the main menu's hidden button
    SetTextKey(item, Hash("TXT_OPTIONS_CHEATS26"), g_hMods);
    AddEvent(item, kEvAction, kActSelectScreen, g_hScreen);
    // The menu stacks its visible buttons in list order (centred), so the list position is the place on screen and
    // the Up / Down order: MODS goes right after OPTIONS.
    int at = -1;
    for (int i = 0; i + 1 < count; ++i)   // items[count - 1] is ours
        if (items[i] && U32(items[i], 0x14) == Hash("BN_OPTIONS")) at = i;
    if (at < 0) {
        dslog::Write("[fail] Mods menu: BN_OPTIONS not in the main menu - MODS left out");
        U32(item, 0x20) &= ~kVisible;
        return;
    }
    memmove(&items[at + 2], &items[at + 1], (count - 2 - at) * sizeof(void*));
    items[at + 1] = item;
    dslog::Write("Mods menu: MODS item added to the main menu (after OPTIONS)");
}

void AddModsScreen(void* set, const std::vector<uint8_t>& d, void* ver) {
    long screenAt = FindRecord(d, "SCR_GAME_MENU", true), nextScreen = FindRecord(d, "TEST", true);
    long options = FindRecord(d, "BN_OPTIONS", false), cheats = FindRecord(d, "BN_CHEATS", false);
    if (screenAt < 0 || nextScreen <= screenAt || options < 0 || cheats <= options) {
        dslog::Write("[fail] Mods menu: screen records not found in Main.gsc - no MODS screen");
        return;
    }
    if (*U8(set, 0x2D4) >= *U8(set, 0x2D6)) {
        dslog::Write("[fail] Mods menu: the screen list is full - no MODS screen");
        return;
    }
    // The screen record after its name hash: [len][name]... - renamed for the copy.
    const char* name = "SCR_DSFIX_MODS";
    uint32_t oldLen = U32(const_cast<uint8_t*>(d.data()), static_cast<uint32_t>(screenAt + 4));
    std::vector<uint8_t> rec(4 + strlen(name));
    U32(rec.data(), 0) = static_cast<uint32_t>(strlen(name));
    memcpy(rec.data() + 4, name, strlen(name));
    rec.insert(rec.end(), d.begin() + screenAt + 8 + oldLen, d.begin() + nextScreen);

    // 0x70 bytes: the base screen is 0x64; the menu screens' slide transitions (vtable +0x44 / +0x48 = 0x51E6E0 /
    // 0x51E670, taken from SCR_GAME_MENU) also use +0x64 / +0x68.
    void* screen = Alloc(0x70);
    memset(screen, 0, 0x70);
    reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(kScreenCtor)(screen, nullptr, 0);
    memcpy(g_vtable, reinterpret_cast<void*>(kScreenVtable), sizeof g_vtable);
    g_vtable[0x1C / 4] = reinterpret_cast<void*>(&ScreenMessage);
    g_vtable[0x44 / 4] = reinterpret_cast<void**>(kGameMenuVtable)[0x44 / 4];
    g_vtable[0x48 / 4] = reinterpret_cast<void**>(kGameMenuVtable)[0x48 / 4];
    Ptr(screen, 0) = g_vtable;
    uint8_t* pos = rec.data();
    using LoadFn = void(__fastcall*)(void*, void*, uint8_t**, void*);
    reinterpret_cast<LoadFn>(reinterpret_cast<void**>(kScreenVtable)[0x5C / 4])(screen, nullptr, &pos, ver);
    if (pos != rec.data() + rec.size()) {
        dslog::Write("[fail] Mods menu: screen record parsed to %d of %u bytes - no MODS screen",
                     static_cast<int>(pos - rec.data()), static_cast<unsigned>(rec.size()));
        return;   // the half-built object is left unreferenced
    }
    U32(screen, 0x14) = g_hScreen;
    void* menu = FindChild(U8(screen, 0x24), Hash("MENU_MAIN_SCREEN"));
    void* title = FindChild(U8(screen, 0x24), Hash("CTRL_TITLE_BAR_TEXT4"));
    if (!menu) {
        dslog::Write("[fail] Mods menu: the copied screen has no menu - no MODS screen");
        return;
    }
    if (title) U32(title, 0x48) = g_hTitle;
    void* list = U8(menu, 0x40);
    *U8(list, 8) = 0;   // the main menu's own buttons go; the rows take their place (stacked by the menu)
    g_noMods = g_rows.empty();
    int rows = g_noMods ? 1 : static_cast<int>(g_rows.size());
    if (rows > kMaxRows) rows = kMaxRows;
    g_rowCount = rows;
    g_perPage = static_cast<int>(g_rows.size()) > kMaxRows ? kMaxRows - 1 : kMaxRows;
    g_page = 0;
    if (!ParseChildren(list, d.data() + options, static_cast<size_t>(cheats - options), rows, ver)) {
        g_rowCount = 0;
        return;
    }
    void** items = static_cast<void**>(Ptr(list, 4));
    for (int i = 0; i < rows; ++i) {
        char key[32];
        sprintf_s(key, "DSFIX_MOD_%d", i);
        g_rowCtrl[i] = Hash(key);
        sprintf_s(key, "DSFIX_MODTXT_%d", i);
        g_rowText[i] = Hash(key);
        g_rowObj[i] = items[i];
        U32(items[i], 0x14) = g_rowCtrl[i];
        U32(items[i], 0x20) |= kVisible;
        SetTextKey(items[i], Hash("TXT_BUTTON_TEXT3"), g_rowText[i]);
        AddEvent(items[i], kEvAction, kActNotify, 0);
    }
    Ptr(menu, 0x50) = items[0];   // the menu's focused button (still the main menu's first one after the copy)
    g_modsMenu = menu;
    ShowPage();
    AddEvent(menu, kEvUp, kActPrev, 0);
    AddEvent(menu, kEvDown, kActNext, 0);
    AddEvent(screen, kEvCancel, kActBack, 0);
    uint8_t n = *U8(set, 0x2D4);
    static_cast<void**>(Ptr(set, 0x2D0))[n] = screen;
    *U8(set, 0x2D4) = static_cast<uint8_t>(n + 1);
    g_modsScreen = screen;
    g_companionsDone = false;
    CopyCompanions();
    dslog::Write("Mods menu: MODS screen added (%d row(s), %d mod(s), %d page(s))", rows,
                 static_cast<int>(g_rows.size()), Pages());
}

// The GUI allocator 0x5A1E10 is a bump arena with no slack: object = *(~[0x117D0A0] ^ [0x4FF170]) (SecuROM-obfuscated
// pointer, stub 0x517A06), +0x4C free, +0x50 used, +0x54 base; allocator 0x56342E returns 0 when full. Our copies
// filled it, and the next .gsc file's controls came back null (crash in the container loader 0x5A0330). So our parses
// allocate from a buffer of our own: the arena's fields are swapped for the duration and put back after, leaving the
// game's arena exactly as it would be without the MODS menu. The buffer is reused when the front end loads again.
constexpr uint32_t kArenaKey1 = 0x117D0A0, kArenaKey2 = 0x4FF170;
constexpr uint32_t kOwnArena = 1u << 20;
class ArenaSwap {
public:
    ArenaSwap() {
        __try {
            arena_ = reinterpret_cast<uint8_t*>(*reinterpret_cast<uint32_t*>(
                ~*reinterpret_cast<uint32_t*>(kArenaKey1) ^ *reinterpret_cast<uint32_t*>(kArenaKey2)));
            if (!arena_) return;
            memcpy(saved_, arena_ + 0x4C, sizeof saved_);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            arena_ = nullptr;
            return;
        }
        static void* buffer = VirtualAlloc(nullptr, kOwnArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buffer) {
            arena_ = nullptr;
            return;
        }
        U32(arena_, 0x4C) = kOwnArena;
        U32(arena_, 0x50) = 0;
        Ptr(arena_, 0x54) = buffer;
    }
    ~ArenaSwap() {
        if (!arena_) return;
        dslog::Write("Mods menu: %u bytes of GUI objects in its own arena", U32(arena_, 0x50));
        memcpy(arena_ + 0x4C, saved_, sizeof saved_);
    }
    bool ok() const { return arena_ != nullptr; }

private:
    uint8_t* arena_ = nullptr;
    uint32_t saved_[3] = {};
};

int __fastcall GscLoad(void* set, void* edx, const char* name, int platforms, void* ver) {
    int r = g_nextGscLoad(set, edx, name, platforms, ver);
    CopyCompanions();
    if (!name || _stricmp(name, "Main.gsc") || !ver) return r;
    void* raw = nullptr;
    uint32_t size = 0;
    using LoadFileFn = int(__cdecl*)(const char*, void**, uint32_t*, int, int);
    if (!reinterpret_cast<LoadFileFn>(kLoadFile)(name, &raw, &size, 0, 0x13) || !raw || size < 8) {
        dslog::Write("[fail] Mods menu: Main.gsc could not be read again - no MODS menu");
        return r;
    }
    std::vector<uint8_t> d(static_cast<uint8_t*>(raw), static_cast<uint8_t*>(raw) + size);
    reinterpret_cast<void(__cdecl*)(void*)>(kFree)(raw);
    uint16_t& version = *static_cast<uint16_t*>(ver);
    uint16_t saved = version;
    version = *reinterpret_cast<uint16_t*>(d.data());
    RefreshRows();
    {
        ArenaSwap arena;
        if (!arena.ok()) {
            dslog::Write("[fail] Mods menu: GUI arena not found - no MODS menu");
        } else {
            AddModsScreen(set, d, ver);
            AddMainMenuItem(set, d, ver);
        }
    }
    version = saved;
    return r;
}
}  // namespace

void features::ApplyModsMenu() {
    if (!features::Get().mods || !features::Get().modsMenu) return;
    g_hMods = Hash("DSFIX_MODS");
    g_hTitle = Hash("DSFIX_MODS_TITLE");
    g_hScreen = Hash("SCR_DSFIX_MODS");
    g_hButton = Hash("DSFIX_BN_MODS");
    g_hOptionsText = Hash("OPTIONS");   // text key of the main menu's OPTIONS button
    if (Hash("SCR_GAME_MENU") != 0xF23EA323) {
        dslog::Write("[fail] Mods menu: name hash self-check failed");
        return;
    }
    if (!patch::HookFunction(kCatalogLookup, reinterpret_cast<void*>(&Lookup), reinterpret_cast<void**>(&g_nextLookup),
                             kCatalogBytes, sizeof kCatalogBytes) ||
        !patch::HookFunction(kGscLoad, reinterpret_cast<void*>(&GscLoad), reinterpret_cast<void**>(&g_nextGscLoad),
                             kGscLoadBytes, sizeof kGscLoadBytes)) {
        dslog::Write("[fail] Mods menu: hooks not installed - no MODS menu");
        return;
    }
    dslog::Write("[ok]   Mods menu: front-end hooks installed");
}

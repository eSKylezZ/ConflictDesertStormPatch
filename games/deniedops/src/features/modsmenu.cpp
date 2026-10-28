// MODS in Conflict: Denied Ops' own main menu (2026-10-25): a MODS item after OPTIONS opens a MODS screen with one
// row per Mods\<name> folder ("NAME: ON" / "NAME: OFF"); Enter on a row switches the folder on / off
// (features::SetModEnabled: '_' prefix rename + index rebuilt at once). Built from the game's own GUI objects, drawn,
// navigated and backed out of by the game's own front end - no dialog, overlay or hotkey. Same design as Global
// Storm's (games/cgs/src/pse/features/modsmenu.cpp) - Denied Ops' GUI library is the newer version of that one.
//
// The front end is GUI data: catalog.pc.dat Interface.gui lists the screen files (Main.gsc = SCR_GAME_MENU, the main
// menu, version 0x4C). CGUIScreenManager::LoadScreenFile 0x702590 thiscall(manager, name, platforms, ver*) ret 0xC
// loads one: screens (factory 0x6FB110, Load = vtable +0x54) into manager +0x118 (count byte +0x11C, capacity +0x11E),
// then the file's event table. After Main.gsc is loaded this reads Main.gsc again (whole-file loader 0x5E51D0) and
// runs the game's own parsers on byte copies of its records:
//   - SCR_DSFIX_MODS: a copy of the SCR_GAME_MENU screen record parsed into a plain GUI screen (alloc 0x74, ctor
//     0x706CF0, vtable = a copy of the base 0x9418A4 with the message handler (+0xC) ours and SCR_GAME_MENU's slide
//     transitions (+0x3C / +0x40 / +0x4C)), its menu emptied and filled with copies of the BN_OPTIONS button, one per
//     mod (event Action -> Notify = our message 4 with the row), title text DSFIX_MODS_TITLE; events menu Up / Down
//     -> previous / next, screen Cancel -> Back to SCR_GAME_MENU.
//   - DSFIX_BN_MODS: a copy of the main menu's (hidden) BN_CHEATS button parsed into SCR_GAME_MENU's menu (container
//     loader 0x6FD1B0), made visible, text DSFIX_MODS, event Action -> Select Screen SCR_DSFIX_MODS, moved after
//     BN_OPTIONS (the menu's list order is its Up / Down order and its layout order).
// Texts: the catalog lookup 0x5EFD20 thiscall(catalog, key hash) -> wchar_t* (text controls look their key up when
// drawn) answers our keys. Names hash with the archive LFSR over the lower-cased name (SCR_GAME_MENU = 0xF23EA323).
// Code patches wait for SteamStub's hand-over (unpack.cpp).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kLoadScreenFile = 0x702590;   // thiscall(manager, name, platforms, ver*) ret 0xC
constexpr uint8_t kLoadScreenFileEntry[] = {0x0F, 0xB6, 0x44, 0x24, 0x08};   // movzx eax, byte [esp+8]
constexpr uint32_t kLookup = 0x5EFD20;           // thiscall(catalog, hash) ret 4 -> wchar_t*
constexpr uint8_t kLookupEntry[] = {0x55, 0x8B, 0x6C, 0x24, 0x08};           // push ebp; mov ebp, [esp+8]
constexpr uint32_t kLoadFile = 0x5E51D0;         // cdecl(name, void** buf, uint32_t* size, int, int flags) -> nonzero
constexpr uint32_t kFree = 0x631C80;             // cdecl(void*) - frees what kLoadFile returned
constexpr uint32_t kAlloc = 0x708760;            // cdecl(size, align) - the GUI's allocator (CLinearBuffer)
constexpr uint32_t kGuiMem = 0xC99FD0;           // -> CLinearBuffer: +0x4C free, +0x50 used, +0x58 base
constexpr uint32_t kChildrenLoad = 0x6FD1B0;     // thiscall(container, uint8_t** pos, ver*) ret 8: [count][records]
constexpr uint32_t kFindChild = 0x6FD0B0;        // thiscall(container, hash) ret 4: direct child or 0
constexpr uint32_t kSetFocus = 0x6FD2C0;         // thiscall(container, control) ret 4
constexpr uint32_t kAddEvent = 0x707E30;         // thiscall(control, event) ret 4 -> entry
constexpr uint32_t kScreenCtor = 0x706CF0;       // thiscall(screen, 0) ret 4 (0x74 bytes)
constexpr uint32_t kScreenVtable = 0x9418A4;     // 22 methods: +0xC Message(msg, param), +0x54 Load(pos*, ver*)
constexpr uint32_t kGameMenuVtable = 0x92E3AC;   // SCR_GAME_MENU's class: front-end slide transitions
constexpr int kVtableSlots = 22;
constexpr uint32_t kScreenSize = 0x74;

// Screen: +0x14 name hash, +0x28 child container, +0x54 / +0x5C companion screen lists. Control: +0x10 class, +0x14
// name hash, +0x20 flags, +0x2C y, +0x48 child container. Container: +4 array, +8 count byte, +0xA capacity byte,
// +0x10 focused child. Event entry: +0 event, +4 action, +8 target, +0xC control, +0x10 flag byte.
constexpr uint32_t kScreenChildren = 0x28, kCtrlChildren = 0x48, kPosY = 0x2C;
constexpr uint32_t kVisible = 0x8000;
constexpr int kEvUp = 2, kEvDown = 3, kEvAction = 4, kEvCancel = 5;
constexpr int kActSelectScreen = 0, kActNotify = 2, kActNext = 4, kActPrev = 5, kActBack = 7;
constexpr int kMsgNotify = 4, kMsgOpened = 0x15, kMsgLoaded = 0x1E;
constexpr int kMaxRows = 8;
constexpr uint32_t kGameMenuHash = 0xF23EA323;

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

using LoadScreenFileFn = int(__fastcall*)(void* mgr, void*, const char* name, int platforms, void* ver);
using LookupFn = const wchar_t*(__fastcall*)(void* catalog, void*, uint32_t hash);
using MessageFn = int(__fastcall*)(void* screen, void*, int msg, void* param);
LoadScreenFileFn g_nextLoad = nullptr;
LookupFn g_nextLookup = nullptr;

uint8_t* U8(void* p, uint32_t off) { return static_cast<uint8_t*>(p) + off; }
uint32_t& U32(void* p, uint32_t off) { return *reinterpret_cast<uint32_t*>(U8(p, off)); }
void*& Ptr(void* p, uint32_t off) { return *reinterpret_cast<void**>(U8(p, off)); }
float& F32(void* p, uint32_t off) { return *reinterpret_cast<float*>(U8(p, off)); }
void* Alloc(uint32_t size) { return reinterpret_cast<void*(__cdecl*)(uint32_t, uint32_t)>(kAlloc)(size, 4); }

struct Row {
    std::string folder;
    std::wstring text;
    bool enabled = false;
};
std::vector<Row> g_rows;
bool g_noMods = false;
uint32_t g_hMods = 0, g_hTitle = 0, g_hScreen = 0, g_hButton = 0;
uint32_t g_rowCtrl[kMaxRows] = {}, g_rowText[kMaxRows] = {};
void* g_rowObj[kMaxRows] = {};
int g_rowCount = 0, g_perPage = 0, g_page = 0;
std::wstring g_moreText;
void* g_vtable[kVtableSlots] = {};
void* g_modsScreen = nullptr;
void* g_modsMenu = nullptr;
void* g_companions[2][8] = {};
bool g_companionsDone = false;
const wchar_t kModsText[] = L"MODS";
const wchar_t kNoModsText[] = L"NO MODS IN THE MODS FOLDER";

void RefreshRows() {
    g_rows.clear();
    for (const features::ModFolder& m : features::ModList()) {
        Row r;
        r.folder = m.folder;
        r.enabled = m.enabled;
        for (char c : m.name) r.text += static_cast<wchar_t>(toupper(static_cast<unsigned char>(c)));
        r.text += m.enabled ? L": ON" : L": OFF";
        g_rows.push_back(r);
    }
}

int Pages() { return g_perPage ? (static_cast<int>(g_rows.size()) + g_perPage - 1) / g_perPage : 1; }
bool HasMoreRow() { return g_rowCount == kMaxRows && static_cast<int>(g_rows.size()) > kMaxRows; }
int ModOfRow(int i) {   // row -> index into g_rows, or -1 (the MORE MODS row / an empty row on the last page)
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

// Pointer arrays {+4 array, +8 count byte, +0xA capacity byte} (child containers, control event tables) grown to hold
// `extra` more: the game's loaders allocate exactly what a file needs and never grow.
void Reserve(void* table, int extra) {
    uint8_t count = *U8(table, 8), cap = *U8(table, 0xA);
    if (count + extra <= cap) return;
    int ncap = count + extra;
    if (ncap > 255) ncap = 255;
    void** fresh = static_cast<void**>(Alloc(static_cast<uint32_t>(ncap) * 4));
    if (!fresh) return;
    memset(fresh, 0, static_cast<size_t>(ncap) * 4);
    if (count) memcpy(fresh, Ptr(table, 4), count * 4u);
    Ptr(table, 4) = fresh;
    *U8(table, 0xA) = static_cast<uint8_t>(ncap);
}

void AddEvent(void* ctrl, int event, int action, uint32_t target) {
    Reserve(ctrl, 1);   // the control's own event table: +4 array, +8 count, +0xA capacity
    void* e = reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(kAddEvent)(ctrl, nullptr, event);
    U32(e, 4) = static_cast<uint32_t>(action);
    U32(e, 8) = target;
    *U8(e, 0x10) = static_cast<uint8_t>(event <= 3 ? 4 : 1);   // what the event table loader 0x70EB80 gives these
}

// Every dword equal to `from` in the control's subtree becomes `to` (a button's text key sits in its text control
// +0x50 and in its highlighted selection bar +0x9C).
void ReplaceKey(void* ctrl, uint32_t from, uint32_t to, int depth = 0) {
    for (uint32_t k = 0x40; k < 0xC0; k += 4)
        if (U32(ctrl, k) == from) U32(ctrl, k) = to;
    if (depth > 4) return;
    uint32_t cls = U32(ctrl, 0x10);
    if (!(U32(ctrl, 0x20) & 8) || cls < 2 || cls > 14) return;
    void** kids = static_cast<void**>(Ptr(ctrl, kCtrlChildren + 4));
    uint8_t n = *U8(ctrl, kCtrlChildren + 8);
    for (int i = 0; i < n; ++i)
        if (kids && kids[i]) ReplaceKey(kids[i], from, to, depth + 1);
}

// Parses `copies` copies of one control record (bytes from its class dword on) into a child container.
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
        if (*reinterpret_cast<const uint32_t*>(d.data() + p) != h) continue;
        if (*reinterpret_cast<const uint32_t*>(d.data() + p + lenAt) != n) continue;
        if (_memicmp(d.data() + p + lenAt + 4, name, n)) continue;
        return static_cast<long>(screen ? p : p - 4);
    }
    return -1;
}

void* FindScreen(void* mgr, uint32_t hash) {
    uint8_t n = *U8(mgr, 0x11C);
    for (int i = 0; i < n; ++i) {
        void* s = static_cast<void**>(Ptr(mgr, 0x118))[i];
        if (s && U32(s, 0x14) == hash) return s;
    }
    return nullptr;
}

// Companion screens {+0 array, +4 count byte, +6 capacity byte}: +0x54 drawn in front (SCR_NAVIGATION_OVERLAY - title
// bar / button prompts - and the blue panels), +0x5C behind (the menu backgrounds). The game classes' constructors fill
// them; a plain GUI screen has none, so the MODS screen shows SCR_GAME_MENU's.
void CopyCompanions() {
    if (g_companionsDone || !g_modsScreen) return;
    void* gui = *reinterpret_cast<void**>(0xBD0C40);
    if (!gui) return;
    void* menu = FindScreen(Ptr(gui, 0x24), kGameMenuHash);
    if (!menu || !*U8(menu, 0x54 + 4)) return;   // not filled yet
    for (int l = 0; l < 2; ++l) {
        uint32_t off = l ? 0x5C : 0x54;
        uint8_t c = *U8(menu, off + 4);
        if (c > 8) c = 8;
        memcpy(g_companions[l], Ptr(menu, off), c * sizeof(void*));
        Ptr(g_modsScreen, off) = g_companions[l];
        *U8(g_modsScreen, off + 4) = c;
        *U8(g_modsScreen, off + 6) = c;
    }
    g_companionsDone = true;
    dslog::Write("Mods menu: MODS screen shows SCR_GAME_MENU's companion screens (%u in front, %u behind)",
                 *U8(g_modsScreen, 0x58), *U8(g_modsScreen, 0x60));
}

int __fastcall ScreenMessage(void* screen, void*, int msg, void* param) {
    CopyCompanions();
    if ((msg == kMsgOpened || msg == kMsgLoaded) && g_modsMenu) {
        RefreshRows();
        ShowPage();
        if (g_rowCount && g_rowObj[0])   // the container's focus setter un-focuses the old row and highlights this one
            reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(kSetFocus)(U8(g_modsMenu, kCtrlChildren), nullptr,
                                                                                g_rowObj[0]);
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
            std::string folder = g_rows[m].folder;
            bool want = !g_rows[m].enabled;
            bool ok = features::SetModEnabled(folder, want);
            dslog::Write("Mods menu: %s switched %s%s", folder.c_str(), want ? "on" : "off", ok ? "" : " - rename failed");
            RefreshRows();
            return 1;
        }
    }
    return reinterpret_cast<MessageFn>(reinterpret_cast<void**>(kScreenVtable)[0xC / 4])(screen, nullptr, msg, param);
}

const wchar_t* __fastcall Lookup(void* catalog, void* edx, uint32_t hash) {
    if (hash && (hash == g_hMods || hash == g_hTitle)) {
        CopyCompanions();
        return kModsText;
    }
    for (int i = 0; i < g_rowCount; ++i) {
        if (!g_rowText[i] || hash != g_rowText[i]) continue;
        if (g_noMods) return kNoModsText;
        if (HasMoreRow() && i == kMaxRows - 1) return g_moreText.c_str();
        int m = ModOfRow(i);
        return m >= 0 ? g_rows[m].text.c_str() : L"";
    }
    return g_nextLookup(catalog, edx, hash);
}

void AddModsScreen(void* mgr, const std::vector<uint8_t>& d, void* ver) {
    long screenAt = FindRecord(d, "SCR_GAME_MENU", true), nextScreen = FindRecord(d, "TEST", true);
    long options = FindRecord(d, "BN_OPTIONS", false), download = FindRecord(d, "BN_DOWNLOAD_CONTENT", false);
    if (screenAt < 0 || nextScreen <= screenAt || options < 0 || download <= options) {
        dslog::Write("[fail] Mods menu: screen records not found in Main.gsc - no MODS screen");
        return;
    }
    if (*U8(mgr, 0x11C) >= *U8(mgr, 0x11E)) {
        dslog::Write("[fail] Mods menu: the screen list is full - no MODS screen");
        return;
    }
    // The screen record after its name hash: [len][name]... - renamed for the copy.
    const char* name = "SCR_DSFIX_MODS";
    uint32_t oldLen = *reinterpret_cast<const uint32_t*>(d.data() + screenAt + 4);
    std::vector<uint8_t> rec(4 + strlen(name));
    U32(rec.data(), 0) = static_cast<uint32_t>(strlen(name));
    memcpy(rec.data() + 4, name, strlen(name));
    rec.insert(rec.end(), d.begin() + screenAt + 8 + oldLen, d.begin() + nextScreen);

    void* screen = Alloc(kScreenSize);
    if (!screen) {
        dslog::Write("[fail] Mods menu: out of GUI memory - no MODS screen");
        return;
    }
    memset(screen, 0, kScreenSize);
    reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(kScreenCtor)(screen, nullptr, 0);
    memcpy(g_vtable, reinterpret_cast<void*>(kScreenVtable), sizeof g_vtable);
    g_vtable[0xC / 4] = reinterpret_cast<void*>(&ScreenMessage);
    for (int slot : {15, 16, 19}) g_vtable[slot] = reinterpret_cast<void**>(kGameMenuVtable)[slot];
    Ptr(screen, 0) = g_vtable;
    uint8_t* pos = rec.data();
    using LoadFn = void(__fastcall*)(void*, void*, uint8_t**, void*);
    reinterpret_cast<LoadFn>(reinterpret_cast<void**>(kScreenVtable)[0x54 / 4])(screen, nullptr, &pos, ver);
    if (pos != rec.data() + rec.size()) {
        dslog::Write("[fail] Mods menu: screen record parsed to %d of %u bytes - no MODS screen",
                     static_cast<int>(pos - rec.data()), static_cast<unsigned>(rec.size()));
        return;   // the half-built object is left unreferenced
    }
    U32(screen, 0x14) = g_hScreen;
    void* menu = FindChild(U8(screen, kScreenChildren), Hash("MENU_MAIN_SCREEN"));
    void* title = FindChild(U8(screen, kScreenChildren), Hash("CTRL_TITLE_BAR_TEXT19"));
    if (!menu) {
        dslog::Write("[fail] Mods menu: the copied screen has no menu - no MODS screen");
        return;
    }
    if (title) U32(title, 0x50) = g_hTitle;
    void* list = U8(menu, kCtrlChildren);
    // Rows stack from the menu's top button down, one main-menu step apart.
    float top = 0, step = 0;
    {
        void** own = static_cast<void**>(Ptr(list, 4));
        uint8_t n0 = *U8(list, 8);
        bool first = true;
        for (int i = 0; i < n0; ++i) {
            if (!own[i]) continue;
            float y = F32(own[i], kPosY);
            if (first || y < top) top = y;
            first = false;
        }
        for (int i = 0; i < n0; ++i) {
            if (!own[i]) continue;
            float dy = F32(own[i], kPosY) - top;
            if (dy > 0.5f && (step == 0 || dy < step)) step = dy;
        }
    }
    *U8(list, 8) = 0;   // the main menu's own buttons go; the rows take their place
    g_noMods = g_rows.empty();
    int rows = g_noMods ? 1 : static_cast<int>(g_rows.size());
    if (rows > kMaxRows) rows = kMaxRows;
    g_rowCount = rows;
    g_perPage = static_cast<int>(g_rows.size()) > kMaxRows ? kMaxRows - 1 : kMaxRows;
    g_page = 0;
    if (!ParseChildren(list, d.data() + options, static_cast<size_t>(download - options), rows, ver)) {
        g_rowCount = 0;
        return;
    }
    void** items = static_cast<void**>(Ptr(list, 4));
    const uint32_t optionsKey = Hash("Options");
    for (int i = 0; i < rows; ++i) {
        char key[32];
        sprintf_s(key, "DSFIX_MOD_%d", i);
        g_rowCtrl[i] = Hash(key);
        sprintf_s(key, "DSFIX_MODTXT_%d", i);
        g_rowText[i] = Hash(key);
        g_rowObj[i] = items[i];
        U32(items[i], 0x14) = g_rowCtrl[i];
        U32(items[i], 0x20) |= kVisible;
        F32(items[i], kPosY) = top + step * static_cast<float>(i);
        ReplaceKey(items[i], optionsKey, g_rowText[i]);
        AddEvent(items[i], kEvAction, kActNotify, 0);
    }
    Ptr(list, 0x10) = nullptr;   // the copy's focus pointed at the main menu's buttons; set on open (ScreenMessage)
    g_modsMenu = menu;
    ShowPage();
    AddEvent(menu, kEvUp, kActPrev, 0);
    AddEvent(menu, kEvDown, kActNext, 0);
    AddEvent(screen, kEvCancel, kActBack, kGameMenuHash);
    uint8_t n = *U8(mgr, 0x11C);
    static_cast<void**>(Ptr(mgr, 0x118))[n] = screen;
    *U8(mgr, 0x11C) = static_cast<uint8_t>(n + 1);
    g_modsScreen = screen;
    g_companionsDone = false;
    CopyCompanions();
    dslog::Write("Mods menu: MODS screen added (%d row(s), %d mod(s), %d page(s), top %.0f step %.0f)", rows,
                 static_cast<int>(g_rows.size()), Pages(), top, step);
}

void AddMainMenuItem(void* mgr, const std::vector<uint8_t>& d, void* ver) {
    long cheats = FindRecord(d, "BN_CHEATS", false), signin = FindRecord(d, "BN_SIGNIN_OUT", false);
    if (cheats < 0 || signin <= cheats) {
        dslog::Write("[fail] Mods menu: main menu records not found in Main.gsc - no MODS item");
        return;
    }
    void* screen = FindScreen(mgr, kGameMenuHash);
    void* menu = screen ? FindChild(U8(screen, kScreenChildren), Hash("MENU_MAIN_SCREEN")) : nullptr;
    if (!menu) {
        dslog::Write("[fail] Mods menu: SCR_GAME_MENU / MENU_MAIN_SCREEN not loaded - no MODS item");
        return;
    }
    void* list = U8(menu, kCtrlChildren);
    if (!ParseChildren(list, d.data() + cheats, static_cast<size_t>(signin - cheats), 1, ver)) return;
    void** items = static_cast<void**>(Ptr(list, 4));
    uint8_t count = *U8(list, 8);
    void* item = items[count - 1];
    U32(item, 0x14) = g_hButton;
    U32(item, 0x20) |= kVisible;   // BN_CHEATS is the main menu's hidden button
    ReplaceKey(item, Hash("CHEATS"), g_hMods);
    AddEvent(item, kEvAction, kActSelectScreen, g_hScreen);
    int at = -1;
    for (int i = 0; i + 1 < count; ++i)
        if (items[i] && U32(items[i], 0x14) == Hash("BN_OPTIONS")) at = i;
    if (at < 0) {
        dslog::Write("[fail] Mods menu: BN_OPTIONS not in the main menu - MODS left out");
        U32(item, 0x20) &= ~kVisible;
        return;
    }
    // Rows are laid out from the y fields: MODS takes the step below OPTIONS, the buttons below it move down a step.
    float y0 = F32(items[at], kPosY), step = 0;
    for (int i = 0; i + 1 < count; ++i) {
        if (!items[i] || i == at || !(U32(items[i], 0x20) & kVisible)) continue;
        float dy = F32(items[i], kPosY) - y0;
        if (dy > 0.5f && (step == 0 || dy < step)) step = dy;
    }
    if (step == 0) step = 61;
    for (int i = 0; i + 1 < count; ++i)
        if (items[i] && F32(items[i], kPosY) > y0 + 0.5f) F32(items[i], kPosY) += step;
    F32(item, kPosY) = y0 + step;
    memmove(&items[at + 2], &items[at + 1], (count - 2 - at) * sizeof(void*));   // list order = Up / Down order
    items[at + 1] = item;
    dslog::Write("Mods menu: MODS item added to the main menu (after OPTIONS, step %.0f)", step);
}

// The GUI allocator is a CLinearBuffer (0x634F70: bump pointer, no free list, "Out of heap space" box + 0 when full).
// Our copies are parsed with the buffer's fields swapped to a buffer of our own and put back after, so the game's
// own arena is exactly as it would be without the MODS menu (Global Storm's ran out with the copies in it).
constexpr uint32_t kOwnArena = 1u << 20;
class ArenaSwap {
public:
    ArenaSwap() {
        mem_ = *reinterpret_cast<uint8_t**>(kGuiMem);
        if (!mem_) return;
        static void* buffer = VirtualAlloc(nullptr, kOwnArena, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!buffer) {
            mem_ = nullptr;
            return;
        }
        saved_[0] = U32(mem_, 0x4C);
        saved_[1] = U32(mem_, 0x50);
        saved_[2] = U32(mem_, 0x58);
        U32(mem_, 0x4C) = kOwnArena;
        U32(mem_, 0x50) = 0;
        Ptr(mem_, 0x58) = buffer;
    }
    ~ArenaSwap() {
        if (!mem_) return;
        dslog::Write("Mods menu: %u bytes of GUI objects in its own arena", U32(mem_, 0x50));
        U32(mem_, 0x4C) = saved_[0];
        U32(mem_, 0x50) = saved_[1];
        U32(mem_, 0x58) = saved_[2];
    }
    bool ok() const { return mem_ != nullptr; }

private:
    uint8_t* mem_ = nullptr;
    uint32_t saved_[3] = {};
};

int __fastcall LoadScreenFile(void* mgr, void* edx, const char* name, int platforms, void* ver) {
    int r = g_nextLoad(mgr, edx, name, platforms, ver);
    CopyCompanions();
    if (!name || _stricmp(name, "Main.gsc") || !ver) return r;
    if (FindScreen(mgr, g_hScreen)) return r;   // the front end loaded again: still there
    // The loader reads "<ver +0x5C><name>".
    char path[MAX_PATH];
    sprintf_s(path, "%s%s", reinterpret_cast<const char*>(U8(ver, 0x5C)), name);
    void* raw = nullptr;
    uint32_t size = 0;
    using LoadFileFn = int(__cdecl*)(const char*, void**, uint32_t*, int, int);
    if (!reinterpret_cast<LoadFileFn>(kLoadFile)(path, &raw, &size, 0, 0x14) || !raw || size < 8) {
        dslog::Write("[fail] Mods menu: %s could not be read again - no MODS menu", path);
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
            dslog::Write("[fail] Mods menu: GUI memory not found - no MODS menu");
        } else {
            AddModsScreen(mgr, d, ver);
            if (g_modsScreen) AddMainMenuItem(mgr, d, ver);
        }
    }
    version = saved;
    return r;
}

void Apply() {
    if (!patch::HookFunction(kLookup, reinterpret_cast<void*>(&Lookup), reinterpret_cast<void**>(&g_nextLookup),
                             kLookupEntry, sizeof kLookupEntry) ||
        !patch::HookFunction(kLoadScreenFile, reinterpret_cast<void*>(&LoadScreenFile),
                             reinterpret_cast<void**>(&g_nextLoad), kLoadScreenFileEntry, sizeof kLoadScreenFileEntry)) {
        dslog::Write("[fail] Mods menu: unexpected game code - no MODS menu");
        return;
    }
    dslog::Write("[ok]   Mods menu: front-end hooks installed");
}
}  // namespace

void features::ApplyModsMenu() {
    if (!Get().mods || !Get().modsMenu) {
        dslog::Write("[off]  Mods menu: off (FeatMods / FeatModsMenu)");
        return;
    }
    g_hMods = Hash("DSFIX_MODS");
    g_hTitle = Hash("DSFIX_MODS_TITLE");
    g_hScreen = Hash("SCR_DSFIX_MODS");
    g_hButton = Hash("DSFIX_BN_MODS");
    if (Hash("SCR_GAME_MENU") != kGameMenuHash) {
        dslog::Write("[fail] Mods menu: name hash self-check failed");
        return;
    }
    AfterUnpack(&Apply);
}

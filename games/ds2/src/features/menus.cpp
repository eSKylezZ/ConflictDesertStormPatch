// Front-end menu structure like DesertStormFix's (see menus.h for the game's menu classes):
//   main menu (state 2, vtable 0x5E6F58)   SINGLE PLAYER / CO-OP / [MODS] / OPTIONS / QUIT   (was START GAME / LOAD GAME /
//                                          TRAINING / OPTIONS / QUIT, set-up FUN_0045cd90, update FUN_0045ca50)
//   single player (state 3, vtable 0x5E7968) START GAME / LOAD GAME / TRAINING, Back -> main menu. The screen is the
//                                          game's own leftover single-player menu (title SINGLE PLAYER, NEW GAME /
//                                          LOAD GAME / TRAINING, reachable only with the dev start-up flag 'u'); its
//                                          items now carry the main menu's ids and run the main menu's own handler.
//   co-op (state kCoopState = 0x36)        CO-OPERATIVE GAME: an object of the single-player menu's class with a copied
//                                          vtable; rows / input from SetCoopScreenHooks (placeholder rows until then).
//   mods (state kModsState = 0x37)         MODS: the same class / copied vtable; rows, input and the main menu's mod
//                                          line from SetModsScreenHooks (modsmenu.cpp; no MODS item without them).
//                                          Main menu draw = vtable +0x14 (0x462000, shared by the menu classes)
//                                          replaced in the main menu's vtable only.
// The main menu's original update handles OPTIONS / QUIT (and, from the single-player screen, START / LOAD / TRAINING):
// its list read `call FUN_0051fa20` at 0x45CA57 is hooked to answer the id our wrapper already read.
#include "features/menus.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace menus = features::menus;

namespace {
// ---- game addresses ------------------------------------------------------------------------------------------------
constexpr uint32_t kMainVtbl = 0x5E6F58, kSpVtbl = 0x5E7968, kVtblSlots = 21;
constexpr uint32_t kMainSetup = 0x45CD90, kMainUpdate = 0x45CA50, kSpSetup = 0x466550, kSpUpdate = 0x466460;
constexpr uint32_t kSetState = 0x43DFD0, kStateVar = 0x63433C, kBackPressed = 0x43AF40;
constexpr uint32_t kListSelect = 0x51FA20, kListSelectCall = 0x45CA57;
constexpr uint32_t kScreenForState = 0x43BBA0, kTextLookup = 0x4FBE50;
constexpr uint32_t kCreateMenus = 0x43BF40, kCreateMenusCall = 0x4149A9;
constexpr uint32_t kInitScreens = 0x43B3D0, kInitScreensCalls[] = {0x43C800, 0x43FC85};
constexpr uint32_t kMenuBackdrop = 0x40B2B0, kMenuBackdropCall = 0x43DF2E;   // FUN_0043df10: 1 for menu states
constexpr uint32_t kAlloc = 0x4BE2D0, kSpCtor = 0x466390;
constexpr uint32_t kItemDraw = 0x466500;

// main menu item ids: OPTIONS / QUIT keep the game's (its handler runs them); ours are outside its 0x3FD..0x401 table
constexpr int kIdCampaigns = 0x406;   // SINGLE PLAYER's CUSTOM CAMPAIGNS (handled by SpUpdate, never the game)
constexpr int kIdSinglePlayer = 0x403, kIdCoop = 0x404, kIdMods = 0x405, kIdOptions = 0x400, kIdQuit = 0x401, kIdStart = 0x3FD,
              kIdLoad = 0x3FE, kIdTraining = 0x3FF, kIdPad = 0x0F00;

// texts: game catalog hashes + ours (0xC0B2xxxx)
constexpr uint32_t kTxtSinglePlayer = 0xA08F3D1B, kTxtSinglePlayerDesc = 0xD6294A49, kTxtOptions = 0x29257EAE,
                   kTxtOptionsDesc = 0x66C69E69, kTxtQuit = 0xC274AD4D, kTxtQuitDesc = 0xD43842AC,
                   kTxtStart = 0x78CFAFFE, kTxtStartDesc = 0x52B21CC9, kTxtLoad = 0x9E456DC8, kTxtLoadDesc = 0x7204C787,
                   kTxtTraining = 0xE2B366E1, kTxtTrainingDesc = 0x0153A13A, kTxtMainTitle = 0x0105850E;
constexpr uint32_t kTxtCoop = 0xC0B20001, kTxtCoopDesc = 0xC0B20002, kTxtCoopTitle = 0xC0B20003,
                   kTxtCoopPlayer = 0xC0B20010,   // +0..3 placeholder rows
                   kTxtMods = 0xC0B20004, kTxtModsDesc = 0xC0B20005, kTxtCampaigns = 0xC0B20006,
                   kTxtCampaignsDesc = 0xC0B20007;
constexpr uint32_t kMainDraw = 0x462000;   // menu screens' draw (vtable +0x14: jmp FUN_0042e6c0)

std::map<uint32_t, const wchar_t*>& Texts() {
    static std::map<uint32_t, const wchar_t*> t;
    return t;
}

void* g_coopScreen = nullptr;
uint32_t g_coopVtbl[kVtblSlots];
menus::CoopScreenHooks g_coopHooks;
void* g_modsScreen = nullptr;
uint32_t g_modsVtbl[kVtblSlots];
menus::ModsScreenHooks g_modsHooks;
void* g_campaignsScreen = nullptr;
uint32_t g_campaignsVtbl[kVtblSlots];
menus::CampaignScreenHooks g_campaignHooks;
bool g_campaignPending = false;   // DIFFICULTY opened for a custom campaign (OpenDifficulty)
int g_forcedSelection = -1;   // answered by the hooked list read in the main menu's original update
bool g_fromSinglePlayer = false;   // a screen was opened from SINGLE PLAYER: its way back to the main menu ends there

// ---- small game wrappers -------------------------------------------------------------------------------------------
uint8_t* List(void* screen) { return *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(screen) + 0x108); }
template <class R = void, class... A>
R This(uint32_t fn, void* self, A... a) { return reinterpret_cast<R(__thiscall*)(void*, A...)>(fn)(self, a...); }
template <class R = void, class... A>
R Cdecl(uint32_t fn, A... a) { return reinterpret_cast<R(__cdecl*)(A...)>(fn)(a...); }

// ---- dev: scripted menu input (Dev\MenuKeys) -----------------------------------------------------------------------
// "D,D,S,B,W": one step per Dev\MenuKeyMs (default 1500 ms), from 4 s after the first menu query. U / D move the
// selection, L / R change a slider / choice, S selects, B goes back, W waits - on every front-end screen. Nothing is sent to a window and the game's
// input isn't rewritten (dev builds with MenuKeys only):
//   U / D / S  every menu list read (FUN_0051fa20, 31 calls): U / D set input block 0's action flag +0xC8 / +0xCC bit 4
//              "held" (read by the list's callbacks FUN_0043afc0 up / FUN_0043b060 down; block i at 0x630000 + i *
//              0x58C) for that one read and restore it after; S answers the highlighted item's id
//   B          the Back query FUN_0043af40 (any block's +0xD8 bit 2, block count [0x6315B0]) is replaced by a copy that
//              also answers yes once for a pending B
#ifndef DS_DIST
constexpr char kDevKey[] = "Software\\DesertStorm2Fix\\Dev";
constexpr uint32_t kListSelectCalls[] = {0x43FBBF, 0x43FBD4, 0x45BB5A, 0x45BF97, 0x45C378, 0x45C637, 0x45E8CA,
                                         0x45EA4A, 0x45EBBB, 0x4607A9, 0x460BCA, 0x46238A, 0x46261A, 0x462E92, 0x4642CA,
                                         0x464537, 0x46528A, 0x4654E1, 0x466467, 0x46682D, 0x466847, 0x466862, 0x46687D,
                                         0x46B3D8, 0x46B648, 0x46BCDB, 0x46C24A, 0x46CDF6, 0x46D808};
                                         // + 0x45CA57 / 0x45D4FA (ours: main menu, OPTIONS)
char g_keys[256] = {};
size_t g_keyPos = 0;
DWORD g_keyEvery = 1500, g_nextKey = 0;
uint32_t g_pendAddr = 0;
bool g_pendBack = false, g_pendSelect = false;

void Pump() {
    if (!g_keys[g_keyPos]) return;
    const DWORD now = GetTickCount();
    if (!g_nextKey) g_nextKey = now + 4000;
    if (now < g_nextKey) return;
    const char k = g_keys[g_keyPos];
    while (g_keys[g_keyPos] && g_keys[g_keyPos] != ',') ++g_keyPos;
    if (g_keys[g_keyPos] == ',') ++g_keyPos;
    g_nextKey = now + g_keyEvery;
    switch (k) {
        case 'D': g_pendAddr = 0x6300CC; break;
        case 'U': g_pendAddr = 0x6300C8; break;
        case 'L': g_pendAddr = 0x6300D0; break;   // left / right: +0x154 / +0x158 (callbacks FUN_0043b100 / 0043b1a0)
        case 'R': g_pendAddr = 0x6300D4; break;
        case 'S': g_pendSelect = true; break;
        case 'B': g_pendBack = true; break;
        default: break;
    }
    dslog::Write("[dev] MenuKeys: '%c' in state 0x%X", k, menus::State());
}

uint16_t __fastcall DevListSelect(void* list, void*) {
    Pump();
    const uint32_t addr = g_pendAddr;
    uint32_t old = 0;
    if (addr) {
        old = *reinterpret_cast<uint32_t*>(addr);
        *reinterpret_cast<uint32_t*>(addr) = old | 4;
        g_pendAddr = 0;
    }
    uint16_t id = This<uint16_t>(kListSelect, list);
    // Left / Right stay set until the game's input poll rewrites them: sliders read them while they draw (item draw
    // callback 0x463EC0), after the list read
    if (addr && addr != 0x6300D0 && addr != 0x6300D4) *reinterpret_cast<uint32_t*>(addr) = old;
    if (g_pendSelect && !id) {
        g_pendSelect = false;
        auto l = static_cast<uint8_t*>(list);   // highlighted item: items +0x44, index +0x40, count +0x3E; item +4 = id
        const int16_t index = *reinterpret_cast<int16_t*>(l + 0x40), count = *reinterpret_cast<int16_t*>(l + 0x3E);
        if (index >= 0 && index < count)
            id = *reinterpret_cast<uint16_t*>((*reinterpret_cast<uint8_t***>(l + 0x44))[index] + 4);
        dslog::Write("[dev] MenuKeys: select -> item %d id 0x%X", index, id);
    }
    return id;
}

int __cdecl DevBackQuery() {   // copy of FUN_0043af40
    Pump();
    bool back = g_pendBack;
    const unsigned n = *reinterpret_cast<uint16_t*>(0x6315B0);
    for (unsigned i = 0; i < n && !back; ++i) back = (*reinterpret_cast<uint8_t*>(0x6300D8 + i * 0x58C) & 2) != 0;
    if (!back) return 0;
    g_pendBack = false;
    *reinterpret_cast<int*>(0x60D4F8) = 0;
    return 1;
}

void ApplyDevInput() {
    DWORD size = sizeof g_keys, ms = 0, msSize = sizeof ms;
    if (RegGetValueA(HKEY_CURRENT_USER, kDevKey, "MenuKeys", RRF_RT_REG_SZ, nullptr, g_keys, &size) != ERROR_SUCCESS ||
        !g_keys[0])
        return;
    if (RegGetValueA(HKEY_CURRENT_USER, kDevKey, "MenuKeyMs", RRF_RT_REG_DWORD, nullptr, &ms, &msSize) ==
            ERROR_SUCCESS && ms)
        g_keyEvery = ms;
    static const uint8_t kEntry[] = {0x8B, 0x0D, 0xB0, 0x15, 0x63, 0x00, 0x33, 0xC0};   // mov ecx,[0x6315B0]; xor eax,eax
    if (!patch::Matches(kBackPressed, kEntry, sizeof kEntry)) return;
    patch::WriteJump(kBackPressed, reinterpret_cast<const void*>(&DevBackQuery));
    for (uint32_t site : kListSelectCalls)
        patch::HookCall(site, reinterpret_cast<const void*>(&DevListSelect), kListSelect);
    dslog::Write("[dev] MenuKeys \"%s\" every %lu ms", g_keys, static_cast<unsigned long>(g_keyEvery));
}

uint16_t ListSelect(void* list) { return g_keys[0] ? DevListSelect(list, nullptr) : This<uint16_t>(kListSelect, list); }
#else
void ApplyDevInput() {}
uint16_t ListSelect(void* list) { return This<uint16_t>(kListSelect, list); }
#endif

int ReadSelection(void* screen) { return ListSelect(List(screen)); }

// The navigation input the next list read acts on: the list's up / down callbacks (FUN_0043afc0 / FUN_0043b060) read
// input block +0x14C / +0x88 (up) and +0x150 / +0x8C (down) bit 4 of every block - keyboard arrows and pads; the mouse
// moves the highlight without them. (Dev MenuKeys: the pending U / D.)
int NavInput() {
#ifndef DS_DIST
    if (g_keys[0]) {
        Pump();
        if (g_pendAddr == 0x6300C8) return -1;
        if (g_pendAddr == 0x6300CC) return 1;
    }
#endif
    const unsigned n = *reinterpret_cast<uint16_t*>(0x6315B0);
    for (unsigned i = 0; i < n && i < 4; ++i) {
        const uint8_t* b = reinterpret_cast<const uint8_t*>(0x62FF7C + i * 0x58C);
        if ((b[0x150] | b[0x8C]) & 4) return 1;
        if ((b[0x14C] | b[0x88]) & 4) return -1;
    }
    return 0;
}
bool BackPressed() { return Cdecl<int>(kBackPressed, 0) != 0; }

// ---- list building (the game's set-up sequence, FUN_0045cd90 / FUN_00466550) ----------------------------------------
void Begin(void* screen, bool mainMenu) {
    uint8_t* l = List(screen);
    This(0x51EC80, l);
    This(0x51E6D0, l);
    This(0x51D860, l, 1, 1);
    This(0x51EAC0, l, -1, -1);
    auto r = *reinterpret_cast<uint8_t**>(0x680038);
    This(0x51D900, l, 0, 0, static_cast<int>(*reinterpret_cast<uint16_t*>(r + 0x41A38)),
         static_cast<int>(*reinterpret_cast<uint16_t*>(r + 0x41A3C)));
    This(0x51DC80, l, 4);
    This(0x563D50, l, 1);
    This(0x51D980, l, 0x43AF80, 0x43AFC0, 0x43B060, 0, 0, 0, 0, 0, 0, 0);
    This(0x51EBC0, l, 0x100);
    const float h = Cdecl<float>(0x439F30);   // + __ftol (0x58C3F0) in the original
    This(0x51EC20, l, static_cast<int>(h));
    *reinterpret_cast<uint32_t*>(l + 0xBC) = *reinterpret_cast<uint32_t*>(0x634350);
    *reinterpret_cast<uint16_t*>(l + 0xC0) = 0x914;
    *reinterpret_cast<void**>(l + 0xC4) = screen;
    if (mainMenu) {
        *reinterpret_cast<uint32_t*>(0x6315EC) = 0;
        This(0x42D6B0, screen);   // screen layout rectangles (main menu only)
    }
}

void Item(void* screen, int index, int id, uint32_t text, uint32_t desc, uint32_t flags) {
    uint8_t* l = List(screen);
    void* item = (*reinterpret_cast<void***>(l + 0x44))[index];
    This(0x51D4F0, item, id, desc, text, 0, flags, 0, 0, kItemDraw, 0, 0, 0x60);
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<uint32_t**>(l))[1])(l, item);
}

void End(void* screen, int count, uint32_t title, bool selectFirst) {
    uint8_t* l = List(screen);
    if (selectFirst) This(0x51F980, l);   // first enabled row
    // empty boxes up to the screen's 8 rows (FUN_0042d590: pool, first id, draw callback, flags, total rows)
    This(0x42D590, screen, *reinterpret_cast<void**>(l + 0x44), kIdPad, kItemDraw, 0, 8);
    This(0x42D950, screen, 1, title, 1);
    (void)count;
}

// ---- main menu -----------------------------------------------------------------------------------------------------
bool __fastcall MainSetup(void* screen, void*) {
    Begin(screen, true);
    Item(screen, 0, kIdSinglePlayer, kTxtSinglePlayer, kTxtSinglePlayerDesc, 0x880);
    Item(screen, 1, kIdCoop, kTxtCoop, kTxtCoopDesc, 0x880);
    int row = 2;
    if (g_modsScreen) Item(screen, row++, kIdMods, kTxtMods, kTxtModsDesc, 0x880);
    Item(screen, row++, kIdOptions, kTxtOptions, kTxtOptionsDesc, 0x880);
    Item(screen, row++, kIdQuit, kTxtQuit, kTxtQuitDesc, 0x880);
    End(screen, row, kTxtMainTitle, false);
    return true;
}

int RunMainHandler(void* screen, int id) {   // the game's main-menu update with this activated id
    g_forcedSelection = id;
    const int r = This<int>(kMainUpdate, screen);
    g_forcedSelection = -1;
    return r;
}

int __fastcall MainUpdate(void* screen, void*) {
    const int id = ReadSelection(screen);
    if (id == kIdSinglePlayer) {
        menus::SetState(menus::kSinglePlayerState);
        return 0;
    }
    if (id == kIdCoop) {
        if (g_coopScreen) menus::SetState(menus::kCoopState);
        return 0;
    }
    if (id == kIdMods) {
        if (g_modsScreen) menus::SetState(menus::kModsState);
        return 0;
    }
    return RunMainHandler(screen, id);   // OPTIONS / QUIT / nothing (+ the game's dev checks)
}

// main menu draw (vtable +0x14): the game's, then the mods menu's lines (mod count) on top
void __fastcall MainDraw(void* screen, void*) {
    This(kMainDraw, screen);
    if (g_modsHooks.mainMenuDraw) g_modsHooks.mainMenuDraw();
}

uint16_t __fastcall SelectionForMainHandler(void* list, void*) {
    if (g_forcedSelection >= 0) return static_cast<uint16_t>(g_forcedSelection);
    return This<uint16_t>(kListSelect, list);
}

// ---- single player -------------------------------------------------------------------------------------------------
bool __fastcall SpSetup(void* screen, void*) {
    Begin(screen, false);
    Item(screen, 0, kIdStart, kTxtStart, kTxtStartDesc, 0x880);
    Item(screen, 1, kIdLoad, kTxtLoad, kTxtLoadDesc, 0x880);
    Item(screen, 2, kIdTraining, kTxtTraining, kTxtTrainingDesc, 0x880);
    int rows = 3;
    if (g_campaignsScreen && g_campaignHooks.available && g_campaignHooks.available())
        Item(screen, rows++, kIdCampaigns, kTxtCampaigns, kTxtCampaignsDesc, 0x880);
    End(screen, rows, kTxtSinglePlayer, true);
    return true;
}

int __fastcall SpUpdate(void* screen, void*) {
    const int id = ReadSelection(screen);
    const bool back = BackPressed();
    if (back) {
        g_fromSinglePlayer = false;
        menus::SetState(menus::kMainMenuState);
        return 0;
    }
    if (id == kIdCampaigns && g_campaignsScreen) {
        g_fromSinglePlayer = true;
        menus::SetState(menus::kCampaignsState);
        return 0;
    }
    if (id == kIdStart || id == kIdLoad || id == kIdTraining) {
        g_fromSinglePlayer = true;
        return RunMainHandler(screen, id);
    }
    return 0;
}

// ---- custom campaigns (rows / input from SetCampaignScreenHooks: campaigns.cpp) ----------------------------------------
bool __fastcall CampaignsSetup(void* screen, void*) {
    if (g_campaignHooks.setup) g_campaignHooks.setup(screen);
    return true;
}

int __fastcall CampaignsUpdate(void* screen, void*) {
    const int id = ReadSelection(screen);
    const bool back = BackPressed();
    if (g_campaignHooks.update) g_campaignHooks.update(screen, id, back);
    if (back) menus::SetState(menus::kSinglePlayerState);
    return 0;
}

// ---- co-op ---------------------------------------------------------------------------------------------------------
bool __fastcall CoopSetup(void* screen, void*) {
    if (g_coopHooks.setup && g_coopHooks.setup(screen)) return true;
    Begin(screen, false);
    for (int i = 0; i < 4; ++i) Item(screen, i, 0x0E00 + i, kTxtCoopPlayer + i, kTxtCoopDesc, 0x880);
    End(screen, 4, kTxtCoopTitle, true);
    return true;
}

int __fastcall CoopUpdate(void* screen, void*) {
    const int id = ReadSelection(screen);
    const bool back = BackPressed();
    if (g_coopHooks.update && g_coopHooks.update(screen, id, back) == 1) return 0;
    if (back) menus::SetState(menus::kMainMenuState);
    return 0;
}

// ---- mods (rows / input from SetModsScreenHooks: modsmenu.cpp) ---------------------------------------------------------
bool __fastcall ModsSetup(void* screen, void*) {
    if (g_modsHooks.setup) g_modsHooks.setup(screen);
    return true;
}

int __fastcall ModsUpdate(void* screen, void*) {
    const int nav = NavInput();
    const int before = menus::SelectedRow(screen);
    const int id = ReadSelection(screen);
    const bool back = BackPressed();
    if (g_modsHooks.update) g_modsHooks.update(screen, id, nav, before, back);
    if (back) menus::SetState(menus::kMainMenuState);
    return 0;
}

// ---- state -> screen (FUN_0043bba0 entry: mov eax,[esp+4]; dec eax | cmp eax,0x34 ...) ------------------------------
uint32_t g_lookupCont = kScreenForState + 5;
__declspec(naked) void ScreenForStateStub() {
    __asm {
        mov eax, [esp + 4]
        cmp eax, 0x36   // kCoopState
        jne mods
        mov eax, g_coopScreen
        ret
    mods:
        cmp eax, 0x37   // kModsState
        jne campaigns
        mov eax, g_modsScreen
        ret
    campaigns:
        cmp eax, 0x38   // kCampaignsState
        jne orig
        mov eax, g_campaignsScreen
        ret
    orig:
        dec eax
        jmp dword ptr [g_lookupCont]
    }
}

// ---- state setter (FUN_0043dfd0 entry: mov eax,[0x63433C] | mov [0x634340],eax ...) ----------------------------------
// The screens START GAME / LOAD GAME / TRAINING lead to (profile, load list, training list ...) go back with
// setState(2) - from SINGLE PLAYER they return to it instead. Entering a mission (state 0x13) ends that.
int __cdecl AdjustState(int state) {
    if (*reinterpret_cast<int*>(0x63434C)) return state;   // the setter ignores calls while busy
    const int current = *reinterpret_cast<int*>(kStateVar);
    // DIFFICULTY opened for a custom campaign: its pick (-> 0xA) starts the campaign instead; any other way out forgets it
    if (g_campaignPending && current == menus::kDifficultyState && state != menus::kDifficultyState) {
        g_campaignPending = false;
        if (state == 0xA && g_campaignHooks.start) {
            dslog::Write("Menus: difficulty picked for a custom campaign");
            g_campaignHooks.start();
            return current;
        }
    }
    if (state == 0x13) g_fromSinglePlayer = false;
    if (state == menus::kMainMenuState && g_fromSinglePlayer && current != menus::kSinglePlayerState) {
        dslog::Write("Menus: back from state 0x%X -> SINGLE PLAYER", current);
        return menus::kSinglePlayerState;
    }
    return state;
}

uint32_t g_setStateCont = kSetState + 5;
__declspec(naked) void SetStateStub() {
    __asm {
        push dword ptr [esp + 4]
        call AdjustState
        add esp, 4
        mov [esp + 4], eax
        mov eax, dword ptr ds:[0x63433C]
        jmp dword ptr [g_setStateCont]
    }
}

// ---- texts (FUN_004fbe50 entry: mov eax,[ecx+0x38]; test eax,eax | jbe ...) ------------------------------------------
const wchar_t* __cdecl OwnText(uint32_t hash) {
    auto& t = Texts();
    auto it = t.find(hash);
    return it == t.end() ? features::ModText(hash) : it->second;   // then a mod's text (mods_tables.cpp)
}

uint32_t g_textCont = kTextLookup + 5;
__declspec(naked) void TextLookupStub() {
    __asm {
        push ecx
        push dword ptr [esp + 8]
        call OwnText
        add esp, 4
        pop ecx
        test eax, eax
        jz orig
        ret 4
    orig:
        mov eax, [ecx + 0x38]
        test eax, eax
        jmp dword ptr [g_textCont]
    }
}

// ---- OPTIONS (state 6, vtable 0x5E7038; front-end and in-mission pause menu): HUD SCALING / HUD SIZE rows ------------
// The game's set-up FUN_0045d1f0 adds EDIT PROFILE 0x97F, FX VOLUME 0x981 / MUSIC VOLUME 0x982 (sliders: draw callback
// 0x463EC0, item +0xB4 u16 steps, +0xB8 float value) and in the front-end CONTROLS 0x980, then fills the 8 rows with
// empty boxes (call 0x45D4D2 -> FUN_0042d590, ids from 0x984) - our rows go in just before that. Its update
// FUN_0045d4f0 reads the list at 0x45D4FA (ids 0x97F..0x983 only): our ids are handled there and answered with 0.
// The values come from the HUD feature (SetHudOption: hud.cpp, or the widescreen plugin's interface in the PSE build).
constexpr uint32_t kOptPadCall = 0x45D4D2, kPadRows = 0x42D590, kOptListCall = 0x45D4FA, kFindItem = 0x51DC90;
constexpr uint32_t kSliderDraw = 0x463EC0;
constexpr int kIdHudScaling = 0x9A0, kIdHudSize = 0x9A1;
constexpr uint32_t kTxtHudScaling = 0xC0B20020, kTxtHudScalingDesc = 0xC0B20021, kTxtHudSize = 0xC0B20022,
                   kTxtHudSizeDesc = 0xC0B20023;
constexpr int kSizeStep = 25, kSizeMin = 25, kSizeSteps = 15;   // 25 .. 400 %
menus::HudOption g_hud;
wchar_t g_hudScalingText[48], g_hudSizeText[48];
int g_sizeStep = 3;

void HudTexts() {
    swprintf_s(g_hudScalingText, L"HUD SCALING: %s", g_hud.scaling() ? L"ON" : L"OFF");
    swprintf_s(g_hudSizeText, L"HUD SIZE: %u%%", g_hud.percent());
}

void __fastcall OptionsPadRows(void* screen, void*, void* pool, int firstId, uint32_t drawCb, int flags, int rows) {
    uint8_t* l = List(screen);
    const int count = *reinterpret_cast<int16_t*>(l + 0x3E);
    if (g_hud.scaling && count + 2 <= rows) {
        HudTexts();
        auto items = *reinterpret_cast<uint8_t***>(l + 0x44);
        This(0x51D4F0, items[count], kIdHudScaling, kTxtHudScalingDesc, kTxtHudScaling, 0, 0x880, 0, 0, kItemDraw, 0, 0,
             0x60);
        reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<uint32_t**>(l))[1])(l, items[count]);
        uint8_t* size = items[count + 1];
        // pool items are typed (+0xB0: 0 plain, 1 choice, 2 slider, 3 ...; set by FUN_0051e6e0 / 0051e710 / 0051e760 /
        // 0051e7b0); only a type-2 item is moved by Left / Right in the list read (FUN_0051fa20 -> FUN_0051d720)
        This(0x51E760, size);
        // (id, description +0x24 (the box under the list), text +8 (plain rows' label), -, flags, slider label +0x14, -,
        // draw callback ...) - as the game's FX VOLUME row
        This(0x51D4F0, size, kIdHudSize, kTxtHudSizeDesc, 0, 0, 0x880, kTxtHudSize, 0, kSliderDraw, 0, 0, 0x60);
        *reinterpret_cast<uint16_t*>(size + 0xB4) = kSizeSteps;
        g_sizeStep = (static_cast<int>(g_hud.percent()) - kSizeMin) / kSizeStep;
        *reinterpret_cast<float*>(size + 0xB8) = static_cast<float>(g_sizeStep);
        reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<uint32_t**>(l))[1])(l, size);
    }
    This(kPadRows, screen, pool, firstId, drawCb, flags, rows);
}

uint16_t __fastcall OptionsListRead(void* list, void*) {
    uint16_t id = ListSelect(list);
    if (!g_hud.scaling) return id;
    bool on = g_hud.scaling();
    uint32_t percent = g_hud.percent();
    // The slider's value (+0xB8) is changed by its draw callback while Left / Right is held (a fraction of a step per
    // frame); checked here every frame, whether or not the list reports the row: any move = one whole step.
    if (auto item = This<uint8_t*>(kFindItem, list, static_cast<int>(kIdHudSize))) {
        float& v = *reinterpret_cast<float*>(item + 0xB8);
        const int before = g_sizeStep;
        if (v > g_sizeStep + 0.001f && g_sizeStep < kSizeSteps) ++g_sizeStep;
        else if (v < g_sizeStep - 0.001f && g_sizeStep > 0) --g_sizeStep;
        v = static_cast<float>(g_sizeStep);
        if (g_sizeStep != before) id = kIdHudSize;
        percent = kSizeMin + kSizeStep * static_cast<uint32_t>(g_sizeStep);
    }
    if (id != kIdHudScaling && id != kIdHudSize) return id;
    if (id == kIdHudScaling) on = !on;
    dslog::Write("Menus: OPTIONS row 0x%X -> HUD scaling %d, size %u %%", id, on, percent);
    g_hud.set(on, percent);
    HudTexts();
    return 0;
}

bool ApplyOptionRows() {
    return patch::HookCall(kOptPadCall, reinterpret_cast<const void*>(&OptionsPadRows), kPadRows) &&
           patch::HookCall(kOptListCall, reinterpret_cast<const void*>(&OptionsListRead), kListSelect);
}

// ---- screen object life ------------------------------------------------------------------------------------------
// An object of the single-player menu's class with a copied vtable (own set-up / update), or null.
void* NewScreen(uint32_t* vtbl, const void* setup, const void* update, const char* what, int state) {
    void* s = Cdecl<void*>(kAlloc, 0x128, 0x26);   // game allocator (as createGameMenus)
    if (!s) return nullptr;
    This(kSpCtor, s);
    std::memcpy(vtbl, reinterpret_cast<void*>(kSpVtbl), kVtblSlots * 4);
    vtbl[3] = reinterpret_cast<uint32_t>(setup);
    vtbl[4] = reinterpret_cast<uint32_t>(update);
    *reinterpret_cast<uint32_t**>(s) = vtbl;
    if (!reinterpret_cast<bool(__thiscall*)(void*)>(vtbl[1])(s)) {   // create: list + 8 row objects
        dslog::Write("[fail] Menus: %s screen not created", what);
        return nullptr;
    }
    dslog::Write("Menus: %s screen %p (state 0x%X)", what, s, state);
    return s;
}

void __cdecl CreateMenus() {
    Cdecl(kCreateMenus);
    g_coopScreen = NewScreen(g_coopVtbl, &CoopSetup, &CoopUpdate, "co-op", menus::kCoopState);
    if (g_modsHooks.setup) g_modsScreen = NewScreen(g_modsVtbl, &ModsSetup, &ModsUpdate, "mods", menus::kModsState);
    if (g_campaignHooks.setup)
        g_campaignsScreen = NewScreen(g_campaignsVtbl, &CampaignsSetup, &CampaignsUpdate, "custom campaigns",
                                      menus::kCampaignsState);
}

void __cdecl InitScreens() {
    Cdecl(kInitScreens);
    // as for the single-player menu: vtable +0x4C(font [0x62F440], [0x62F434], [0x634350])
    for (void* s : {g_coopScreen, g_modsScreen, g_campaignsScreen})
        if (s)
            reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t)>(
                (*reinterpret_cast<uint32_t**>(s))[0x4C / 4])(s, *reinterpret_cast<uint32_t*>(0x62F440),
                                                             *reinterpret_cast<uint32_t*>(0x62F434),
                                                             *reinterpret_cast<uint32_t*>(0x634350));
}

// FUN_0043df10 (entering a state) switches the front-end backdrop on for every menu state it knows
void __cdecl MenuBackdrop(int on) {
    const int state = *reinterpret_cast<int*>(kStateVar);
    if (state == menus::kCoopState || state == menus::kModsState || state == menus::kCampaignsState) on = 1;
    Cdecl(kMenuBackdrop, on);
}

bool SetSlot(uint32_t vtbl, int slot, uint32_t expected, const void* fn) {
    const uint32_t va = vtbl + slot * 4;
    if (!patch::Matches(va, &expected, 4)) {
        dslog::Write("[fail] Menus: vtable slot 0x%08X is not 0x%08X", va, expected);
        return false;
    }
#ifdef DS2_PSE_HOST
    // a broker pointer hook (owned by ds2fix.menus); our functions replace the game's and call it themselves
    return patch::HookSlot(va, fn, nullptr, expected);
#else
    return patch::WriteValue(va, reinterpret_cast<uint32_t>(fn));
#endif
}
}  // namespace

// ---- public ----------------------------------------------------------------------------------------------------------
void menus::AddText(uint32_t hash, const wchar_t* text) { Texts()[hash] = text; }
void* menus::CoopScreen() { return g_coopScreen; }
void menus::SetCoopScreenHooks(const CoopScreenHooks& hooks) { g_coopHooks = hooks; }
void menus::BeginList(void* screen) { Begin(screen, false); }
void menus::AddItem(void* screen, int index, int id, uint32_t textHash, uint32_t descHash, uint32_t flags) {
    Item(screen, index, id, textHash, descHash, flags);
}
void menus::EndList(void* screen, int itemCount, uint32_t titleHash) { End(screen, itemCount, titleHash, true); }
void menus::SetHudOption(const HudOption& hud) { g_hud = hud; }
void menus::SetModsScreenHooks(const ModsScreenHooks& hooks) { g_modsHooks = hooks; }
void menus::SetCampaignScreenHooks(const CampaignScreenHooks& hooks) { g_campaignHooks = hooks; }
void menus::OpenDifficulty() {
    g_campaignPending = true;
    SetState(kDifficultyState);
}
int menus::SelectedRow(void* screen) { return *reinterpret_cast<int16_t*>(List(screen) + 0x40); }
void menus::SetSelectedRow(void* screen, int row) {
    *reinterpret_cast<int16_t*>(List(screen) + 0x40) = static_cast<int16_t>(row);
}
void menus::DrawLabel(const char* text, int x, int y, uint32_t argb, bool smallFont) {
    void* font = *reinterpret_cast<void**>(smallFont ? 0x62F43C : 0x62F440);
    if (!font || !text) return;
    This(0x526E40, font, static_cast<int>(argb >> 24));   // alpha (FUN_0052b6b0 on the font's sheet)
    This(0x526E20, font, argb);
    This(0x529490, font, text, x, y, 1);
}
void menus::ScreenSize(int& w, int& h) {
    auto r = *reinterpret_cast<uint8_t* const*>(0x680038);
    w = r ? *reinterpret_cast<int*>(r + 0x41A38) : 0;
    h = r ? *reinterpret_cast<int*>(r + 0x41A3C) : 0;
}
void menus::SetState(int state) { Cdecl(kSetState, state); }
int menus::State() { return *reinterpret_cast<int*>(kStateVar); }

void features::ApplyMenus() {
    static const uint8_t kLookupEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x48, 0x83, 0xF8, 0x34};
    static const uint8_t kTextEntry[] = {0x8B, 0x41, 0x38, 0x85, 0xC0, 0x76, 0x31};
    static const uint8_t kSetStateEntry[] = {0xA1, 0x3C, 0x43, 0x63, 0x00, 0xA3, 0x40, 0x43, 0x63, 0x00};
    if (!patch::Matches(kScreenForState, kLookupEntry, sizeof kLookupEntry) ||
        !patch::Matches(kMainVtbl + 5 * 4, &kMainDraw, 4) ||
        !patch::Matches(kTextLookup, kTextEntry, sizeof kTextEntry) ||
        !patch::Matches(kSetState, kSetStateEntry, sizeof kSetStateEntry) ||
        !patch::Matches(kMainVtbl + 3 * 4, &kMainSetup, 4) || !patch::Matches(kMainVtbl + 4 * 4, &kMainUpdate, 4) ||
        !patch::Matches(kSpVtbl + 3 * 4, &kSpSetup, 4) || !patch::Matches(kSpVtbl + 4 * 4, &kSpUpdate, 4)) {
        dslog::Write("[fail] Menus: front-end code not recognised");
        return;
    }
    menus::AddText(kTxtCoop, L"CO-OP");
    menus::AddText(kTxtCoopDesc, L"Play a Co-operative Game");
    menus::AddText(kTxtCoopTitle, L"CO-OPERATIVE GAME");
    menus::AddText(kTxtMods, L"MODS");
    menus::AddText(kTxtModsDesc, L"Switch the mods in the Mods folder on or off");
    menus::AddText(kTxtCampaigns, L"CUSTOM CAMPAIGNS");
    menus::AddText(kTxtCampaignsDesc, L"Play a campaign of missions from the Mods folder");
    menus::AddText(kTxtCoopPlayer + 0, L"PLAYER 1");
    menus::AddText(kTxtCoopPlayer + 1, L"PLAYER 2");
    menus::AddText(kTxtCoopPlayer + 2, L"PLAYER 3");
    menus::AddText(kTxtCoopPlayer + 3, L"PLAYER 4");
    menus::AddText(kTxtHudScaling, g_hudScalingText);
    menus::AddText(kTxtHudScalingDesc, L"Scale the HUD and menus with the screen resolution");
    menus::AddText(kTxtHudSize, g_hudSizeText);
    menus::AddText(kTxtHudSizeDesc, L"Size of the HUD on top of the resolution scaling");

    bool ok = patch::HookCall(kListSelectCall, reinterpret_cast<const void*>(&SelectionForMainHandler), kListSelect) &&
              patch::HookCall(kCreateMenusCall, reinterpret_cast<const void*>(&CreateMenus), kCreateMenus) &&
              patch::HookCall(kMenuBackdropCall, reinterpret_cast<const void*>(&MenuBackdrop), kMenuBackdrop);
    for (uint32_t site : kInitScreensCalls)
        ok = ok && patch::HookCall(site, reinterpret_cast<const void*>(&InitScreens), kInitScreens);
    ok = ok && SetSlot(kMainVtbl, 3, kMainSetup, &MainSetup) && SetSlot(kMainVtbl, 4, kMainUpdate, &MainUpdate) &&
         SetSlot(kMainVtbl, 5, kMainDraw, &MainDraw) &&
         SetSlot(kSpVtbl, 3, kSpSetup, &SpSetup) && SetSlot(kSpVtbl, 4, kSpUpdate, &SpUpdate) &&
         patch::WriteJump(kScreenForState, &ScreenForStateStub) && patch::WriteJump(kTextLookup, &TextLookupStub) &&
         patch::WriteJump(kSetState, &SetStateStub);
    if (ok && !ApplyOptionRows()) dslog::Write("[fail] Menus: OPTIONS rows (HUD) not added");
    if (!ok) {
        dslog::Write("[fail] Menus: hooks incomplete");
        return;
    }
    ApplyDevInput();
    dslog::Write("[ok]   Menus: SINGLE PLAYER / CO-OP / OPTIONS / QUIT (co-op screen = state 0x%X)", menus::kCoopState);
}

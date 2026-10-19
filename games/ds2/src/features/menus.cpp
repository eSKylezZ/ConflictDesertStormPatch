// Front-end menu structure like DesertStormFix's (see menus.h for the game's menu classes):
//   main menu (state 2, vtable 0x5E6F58)   SINGLE PLAYER / CO-OP / OPTIONS / QUIT   (was START GAME / LOAD GAME /
//                                          TRAINING / OPTIONS / QUIT, set-up FUN_0045cd90, update FUN_0045ca50)
//   single player (state 3, vtable 0x5E7968) START GAME / LOAD GAME / TRAINING, Back -> main menu. The screen is the
//                                          game's own leftover single-player menu (title SINGLE PLAYER, NEW GAME /
//                                          LOAD GAME / TRAINING, reachable only with the dev start-up flag 'u'); its
//                                          items now carry the main menu's ids and run the main menu's own handler.
//   co-op (state kCoopState = 0x36)        CO-OPERATIVE GAME: an object of the single-player menu's class with a copied
//                                          vtable; rows / input from SetCoopScreenHooks (placeholder rows until then).
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
constexpr int kIdSinglePlayer = 0x403, kIdCoop = 0x404, kIdOptions = 0x400, kIdQuit = 0x401, kIdStart = 0x3FD,
              kIdLoad = 0x3FE, kIdTraining = 0x3FF, kIdPad = 0x0F00;

// texts: game catalog hashes + ours (0xC0B2xxxx)
constexpr uint32_t kTxtSinglePlayer = 0xA08F3D1B, kTxtSinglePlayerDesc = 0xD6294A49, kTxtOptions = 0x29257EAE,
                   kTxtOptionsDesc = 0x66C69E69, kTxtQuit = 0xC274AD4D, kTxtQuitDesc = 0xD43842AC,
                   kTxtStart = 0x78CFAFFE, kTxtStartDesc = 0x52B21CC9, kTxtLoad = 0x9E456DC8, kTxtLoadDesc = 0x7204C787,
                   kTxtTraining = 0xE2B366E1, kTxtTrainingDesc = 0x0153A13A, kTxtMainTitle = 0x0105850E;
constexpr uint32_t kTxtCoop = 0xC0B20001, kTxtCoopDesc = 0xC0B20002, kTxtCoopTitle = 0xC0B20003,
                   kTxtCoopPlayer = 0xC0B20010;   // +0..3 placeholder rows

std::map<uint32_t, const wchar_t*>& Texts() {
    static std::map<uint32_t, const wchar_t*> t;
    return t;
}

void* g_coopScreen = nullptr;
uint32_t g_coopVtbl[kVtblSlots];
menus::CoopScreenHooks g_coopHooks;
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
// selection, S selects, B goes back, W waits - on every front-end screen. Nothing is sent to a window and the game's
// input isn't rewritten (dev builds with MenuKeys only):
//   U / D / S  every menu list read (FUN_0051fa20, 31 calls): U / D set input block 0's action flag +0xC8 / +0xCC bit 4
//              "held" (read by the list's callbacks FUN_0043afc0 up / FUN_0043b060 down; block i at 0x630000 + i *
//              0x58C) for that one read and restore it after; S answers the highlighted item's id
//   B          the Back query FUN_0043af40 (any block's +0xD8 bit 2, block count [0x6315B0]) is replaced by a copy that
//              also answers yes once for a pending B
#ifndef DS_DIST
constexpr char kDevKey[] = "Software\\DesertStorm2Fix\\Dev";
constexpr uint32_t kListSelectCalls[] = {0x43FBBF, 0x43FBD4, 0x45BB5A, 0x45BF97, 0x45C378, 0x45C637, 0x45D4FA, 0x45E8CA,
                                         0x45EA4A, 0x45EBBB, 0x4607A9, 0x460BCA, 0x46238A, 0x46261A, 0x462E92, 0x4642CA,
                                         0x464537, 0x46528A, 0x4654E1, 0x466467, 0x46682D, 0x466847, 0x466862, 0x46687D,
                                         0x46B3D8, 0x46B648, 0x46BCDB, 0x46C24A, 0x46CDF6, 0x46D808};   // + 0x45CA57 (ours)
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
    if (addr) *reinterpret_cast<uint32_t*>(addr) = old;
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
    Item(screen, 2, kIdOptions, kTxtOptions, kTxtOptionsDesc, 0x880);
    Item(screen, 3, kIdQuit, kTxtQuit, kTxtQuitDesc, 0x880);
    End(screen, 4, kTxtMainTitle, false);
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
    return RunMainHandler(screen, id);   // OPTIONS / QUIT / nothing (+ the game's dev checks)
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
    End(screen, 3, kTxtSinglePlayer, true);
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
    if (id == kIdStart || id == kIdLoad || id == kIdTraining) {
        g_fromSinglePlayer = true;
        return RunMainHandler(screen, id);
    }
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

// ---- state -> screen (FUN_0043bba0 entry: mov eax,[esp+4]; dec eax | cmp eax,0x34 ...) ------------------------------
uint32_t g_lookupCont = kScreenForState + 5;
__declspec(naked) void ScreenForStateStub() {
    __asm {
        mov eax, [esp + 4]
        cmp eax, 0x36   // kCoopState
        jne orig
        mov eax, g_coopScreen
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
    return it == t.end() ? nullptr : it->second;
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

// ---- screen object life ------------------------------------------------------------------------------------------
void __cdecl CreateMenus() {
    Cdecl(kCreateMenus);
    void* s = Cdecl<void*>(kAlloc, 0x128, 0x26);   // game allocator (as createGameMenus)
    if (!s) return;
    This(kSpCtor, s);
    std::memcpy(g_coopVtbl, reinterpret_cast<void*>(kSpVtbl), sizeof g_coopVtbl);
    g_coopVtbl[3] = reinterpret_cast<uint32_t>(&CoopSetup);
    g_coopVtbl[4] = reinterpret_cast<uint32_t>(&CoopUpdate);
    *reinterpret_cast<uint32_t**>(s) = g_coopVtbl;
    if (!reinterpret_cast<bool(__thiscall*)(void*)>(g_coopVtbl[1])(s)) {   // create: list + 8 row objects
        dslog::Write("[fail] Menus: co-op screen not created");
        return;
    }
    g_coopScreen = s;
    dslog::Write("Menus: co-op screen %p (state 0x%X)", s, menus::kCoopState);
}

void __cdecl InitScreens() {
    Cdecl(kInitScreens);
    if (g_coopScreen)   // as for the single-player menu: vtable +0x4C(font [0x62F440], [0x62F434], [0x634350])
        reinterpret_cast<void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t)>(g_coopVtbl[0x4C / 4])(
            g_coopScreen, *reinterpret_cast<uint32_t*>(0x62F440), *reinterpret_cast<uint32_t*>(0x62F434),
            *reinterpret_cast<uint32_t*>(0x634350));
}

// FUN_0043df10 (entering a state) switches the front-end backdrop on for every menu state it knows
void __cdecl MenuBackdrop(int on) {
    if (*reinterpret_cast<int*>(kStateVar) == menus::kCoopState) on = 1;
    Cdecl(kMenuBackdrop, on);
}

bool SetSlot(uint32_t vtbl, int slot, uint32_t expected, const void* fn) {
    const uint32_t va = vtbl + slot * 4;
    if (!patch::Matches(va, &expected, 4)) {
        dslog::Write("[fail] Menus: vtable slot 0x%08X is not 0x%08X", va, expected);
        return false;
    }
    return patch::WriteValue(va, reinterpret_cast<uint32_t>(fn));
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
void menus::SetState(int state) { Cdecl(kSetState, state); }
int menus::State() { return *reinterpret_cast<int*>(kStateVar); }

void features::ApplyMenus() {
    static const uint8_t kLookupEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x48, 0x83, 0xF8, 0x34};
    static const uint8_t kTextEntry[] = {0x8B, 0x41, 0x38, 0x85, 0xC0, 0x76, 0x31};
    static const uint8_t kSetStateEntry[] = {0xA1, 0x3C, 0x43, 0x63, 0x00, 0xA3, 0x40, 0x43, 0x63, 0x00};
    if (!patch::Matches(kScreenForState, kLookupEntry, sizeof kLookupEntry) ||
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
    menus::AddText(kTxtCoopPlayer + 0, L"PLAYER 1");
    menus::AddText(kTxtCoopPlayer + 1, L"PLAYER 2");
    menus::AddText(kTxtCoopPlayer + 2, L"PLAYER 3");
    menus::AddText(kTxtCoopPlayer + 3, L"PLAYER 4");

    bool ok = patch::HookCall(kListSelectCall, reinterpret_cast<const void*>(&SelectionForMainHandler), kListSelect) &&
              patch::HookCall(kCreateMenusCall, reinterpret_cast<const void*>(&CreateMenus), kCreateMenus) &&
              patch::HookCall(kMenuBackdropCall, reinterpret_cast<const void*>(&MenuBackdrop), kMenuBackdrop);
    for (uint32_t site : kInitScreensCalls)
        ok = ok && patch::HookCall(site, reinterpret_cast<const void*>(&InitScreens), kInitScreens);
    ok = ok && SetSlot(kMainVtbl, 3, kMainSetup, &MainSetup) && SetSlot(kMainVtbl, 4, kMainUpdate, &MainUpdate) &&
         SetSlot(kSpVtbl, 3, kSpSetup, &SpSetup) && SetSlot(kSpVtbl, 4, kSpUpdate, &SpUpdate) &&
         patch::WriteJump(kScreenForState, &ScreenForStateStub) && patch::WriteJump(kTextLookup, &TextLookupStub) &&
         patch::WriteJump(kSetState, &SetStateStub);
    if (!ok) {
        dslog::Write("[fail] Menus: hooks incomplete");
        return;
    }
    ApplyDevInput();
    dslog::Write("[ok]   Menus: SINGLE PLAYER / CO-OP / OPTIONS / QUIT (co-op screen = state 0x%X)", menus::kCoopState);
}

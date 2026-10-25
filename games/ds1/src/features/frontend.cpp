// The front end's menu framework in dsfix.dll (taken out of coop.cpp 2026-10-25, before the split-screen / co-op
// group became a plugin DLL): the main menu's extra items and the front-end screen router. Without the co-op plugin the
// DESERTSTORMFIX / VIDEO / MODS / PLUGINS / campaign screens still open.
//
// Main menu set-up (vtable 0x5D86B8 slot 3, pointer hook): the original (SINGLE PLAYER, MULTIPLAYER, OPTIONS, QUIT on
// pool items 0-3), then CO-OP and MODS on items 4-5, MULTIPLAYER renamed VERSUS, and the pool reordered to SINGLE
// PLAYER, CO-OP, VERSUS, MODS, OPTIONS, QUIT. Optional features (start-up settings): FeatVersus off keeps the game's
// MULTIPLAYER (text, state 3), FeatMods off leaves MODS out (its item object stays unused at the end of the pool). The
// item objects are interchangeable, so the next set-up (every time the menu is entered) simply fills the reordered pool
// again. The list capacity / item count 4 -> 6 (0x4150BD / 0x4150CF); the selection handler's "cmp si, 0x3fc; jle"
// (0x4152F8) -> MenuSelectStub.
//
// State -> screen (FUN_00480930, entry hook): states 0x3E.. go to ExtraScreen first (ours, the plugins' through their
// interfaces / dsfix.services), else the game's own lookup.
#include <windows.h>

#include <cstdint>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kStateFirstExtra = 0x3E, kStateCoopMenu = 0x48;
constexpr uint32_t kSetState = 0x47EF20;           // cdecl (state)
constexpr uint32_t kScreenForState = 0x480930;
constexpr uint32_t kMainMenuVtable = 0x5D86B8, kMainMenuSetup = 0x415120;
constexpr uint32_t kMainMenuSetupSlot = kMainMenuVtable + 0x0C;
constexpr uint32_t kMenuCapacity = 0x4150BD;       // push 4 (list capacity)
constexpr uint32_t kMenuItemCount = 0x4150CF;      // mov edi, 4 (item objects)
constexpr uint32_t kMenuSelect = 0x4152F8, kMenuSelectCont = 0x4152FF, kMenuSelectDone = 0x415361;
constexpr uint32_t kItemDrawCb = 0x4153E0;         // main menu item draw callback
constexpr uint32_t kItemSet = 0x4E4FB0;            // thiscall (id, text hash, 0, flags 0x24, 0, 0, draw cb, 0)
constexpr uint16_t kCoopItemId = 0x401, kModsItemId = 0x402;
constexpr uint32_t kHashCoop = 0xC0070001;         // "CO-OP" (coop.cpp's CoopText; FrontEndText answers it without it)

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

void SetItem(void* item, uint16_t id, uint32_t hash) {
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    reinterpret_cast<Fn>(kItemSet)(item, id, hash, 0, 0x24, 0, 0, kItemDrawCb, 0);
}

void AddToList(void* list, void* item) {
    using Fn = void(__thiscall*)(void*, void*);
    reinterpret_cast<Fn>((*reinterpret_cast<void***>(list))[1])(list, item);
}

bool g_versusItem = false;  // item 0x3FE opens VERSUS (state 0x42) instead of the game's MULTIPLAYER (state 3)
void* g_mainMenuSetupNext = reinterpret_cast<void*>(kMainMenuSetup);  // pointer hook chain (patch::HookSlot)

void __fastcall MainMenuSetup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(g_mainMenuSetupNext)(screen);
    void* list = At<void*>(screen, 8);
    void** items = At<void**>(list, 0x2C);
    void* coop = items[4];
    void* mods = items[5];
    const bool modsItem = features::ModsFolderEnabled();
    g_versusItem = features::VersusEnabled();
    SetItem(coop, kCoopItemId, kHashCoop);
    AddToList(list, coop);
    if (g_versusItem) SetItem(items[1], 0x3FE, features::VersusItemText());  // MULTIPLAYER -> VERSUS (versus.cpp)
    if (modsItem) {
        SetItem(mods, kModsItemId, features::ModsMenuItemText());  // MODS (modsmenu.cpp)
        AddToList(list, mods);
    }
    // Pool order = list order: SINGLE PLAYER, CO-OP, VERSUS, MODS, OPTIONS, QUIT (without MODS: it goes last, unlisted).
    void* const order[6] = {items[0], coop, items[1], modsItem ? mods : items[2], modsItem ? items[2] : items[3],
                            modsItem ? items[3] : mods};
    for (int i = 0; i < 6; ++i) items[i] = order[i];
    // The list area was sized for four rows (list +0x38 row pitch, +0x40 area height, +0x42 visible rows).
    const int rows = modsItem ? 6 : 5;
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (rows - 4) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = static_cast<int16_t>(rows);
    // The panel rectangle (screen +0x10..+0x1c) is laid out around the list by vtable +0x50, which the original
    // set-up already called - lay it out again with the same arguments.
    using Layout = void(__thiscall*)(void*, int, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, -1, 1, 0, 0x7FFF);
}

// Replaces "cmp si, 0x3fc; jle done" in the main menu's selection handler (si = activated item id).
__declspec(naked) void MenuSelectStub() {
    __asm {
        cmp si, kCoopItemId
        je coop
        cmp si, 0x3FE               // MULTIPLAYER -> VERSUS (state 0x42, versus.cpp) - unless FeatVersus is off
        jne notVersus
        cmp byte ptr g_versusItem, 0
        jne versus
    notVersus:
        cmp si, kModsItemId         // MODS (state 0x43, modsmenu.cpp)
        je mods
        cmp si, 0x3FC
        jle done
        push kMenuSelectCont
        ret
    mods:
        push 0x43
        mov eax, kSetState
        call eax
        add esp, 4
        push kMenuSelectDone
        ret
    versus:
        push 0x42
        mov eax, kSetState
        call eax
        add esp, 4
        push kMenuSelectDone
        ret
    coop:
        push kStateCoopMenu         // CO-OP screen (online.cpp): SPLIT SCREEN / ONLINE
        mov eax, kSetState
        call eax
        add esp, 4
    done:
        push kMenuSelectDone
        ret
    }
}

void* __cdecl ExtraScreen(uint32_t state) {
    if (state == 0x3E) return features::CoopJoinScreen();  // coop.cpp (CO-OP > SPLIT SCREEN: the join screen)
    if (state == 0x42) return features::VersusScreen();
    if (state == 0x44) return features::VersusSetupScreen();
    if (state == 0x43) return features::ModsMenuScreen();
    if (state == 0x45) return features::CampaignsScreen();
    if (state == 0x47) return features::FixMenuScreen();    // fixmenu.cpp (OPTIONS > DESERTSTORMFIX)
    if (state == 0x4C) return features::VideoMenuScreen();  // videomenu.cpp (OPTIONS > VIDEO)
    if (state == 0x4D || state == 0x4E) return features::PluginsMenuScreen(state);  // pluginsmenu.cpp (> PLUGINS)
    if (void* s = features::OnlineScreen(state)) return s;  // online.cpp (CO-OP > SPLIT SCREEN / ONLINE)
    if (void* s = features::ControlsScreen(state)) return s;
    return features::ServiceScreen(state);  // plugins' screens (dsfix.developer: CHEATS > DEVELOPER, 0x46)
}

void* g_screenForStateNext = nullptr;

__declspec(naked) void ScreenForStateStub() {
    __asm {
        mov eax, [esp + 4]
        cmp eax, kStateFirstExtra
        jb original
        push eax
        call ExtraScreen
        add esp, 4
        test eax, eax
        jz original
        ret
    original:
        jmp dword ptr [g_screenForStateNext]
    }
}
}  // namespace

void features::ApplyFrontEnd() {
    static bool done = false;
    if (done) return;
    done = true;
    static const uint8_t capacity[] = {0x6A, 0x04};
    static const uint8_t count[] = {0xBF, 0x04, 0x00, 0x00, 0x00};
    const uint32_t setup = kMainMenuSetup;
    static const uint8_t select[] = {0x66, 0x81, 0xFE, 0xFC, 0x03, 0x7E, 0x62};
    static const uint8_t lookup[] = {0x8B, 0x44, 0x24, 0x04, 0x48};
    if (!patch::Matches(kMenuCapacity, capacity, sizeof capacity) || !patch::Matches(kMenuItemCount, count, sizeof count) ||
        !patch::Matches(kMainMenuSetupSlot, &setup, 4) || !patch::Matches(kMenuSelect, select, sizeof select) ||
        !patch::Matches(kScreenForState, lookup, sizeof lookup)) {
        dslog::Write("[fail] Front-end menus: unexpected bytes - different exe version? Not applied.");
        return;
    }
    const uint8_t capacity6[] = {0x6A, 0x06};  // 6 items: + CO-OP, MODS
    const uint8_t count6[] = {0xBF, 0x06, 0x00, 0x00, 0x00};
    const bool ok =
        patch::Write(kMenuCapacity, capacity6, sizeof capacity6) && patch::Write(kMenuItemCount, count6, sizeof count6) &&
        patch::HookSlot(kMainMenuSetupSlot, reinterpret_cast<const void*>(&MainMenuSetup), &g_mainMenuSetupNext, kMainMenuSetup) &&
        patch::WriteJump(kMenuSelect, reinterpret_cast<const void*>(&MenuSelectStub), sizeof select, select) &&
        patch::HookFunction(kScreenForState, reinterpret_cast<const void*>(&ScreenForStateStub), &g_screenForStateNext,
                            lookup, sizeof lookup);
    dslog::Write(ok ? "[ok]   Front-end menus: CO-OP / VERSUS / MODS on the main menu, our screens (states 0x3E..)"
                    : "[fail] Front-end menus: not installed");
}

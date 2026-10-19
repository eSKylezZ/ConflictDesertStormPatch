// Pause menu RESTART LEVEL, as on the GameCube / PS2 / Xbox pause menus: the mission starts again from its beginning.
//
// Pause screen (front-end state 0xF, class vtable 0x5D9208, object [0x617A08]): create 0x471E80 (list capacity 7, a
// pool of 7 plain items - only 4 used), set-up 0x471F30 (SAVE GAME 0x411, LOAD GAME 0x412, OPTIONS 0x413, MAIN MENU
// 0x414 from pool items 0..3, item draw 0x472290, panel laid out by vtable +0x50 with the title 0x0556F2D1, then
// 0x4E8130), update 0x472120 (list input at 0x472123; ids 0x411..0x415 handled, 0x415 = resume, the rest ignored).
// The set-up (slot 0x5D9214) is wrapped: pool item 4 becomes RESTART LEVEL (id 0x418, the catalog's own RESTART text
// 0x5AB7F860 "RESTART LEVEL" - every language has it; the PC uses it only on the MISSION FAILED screen) and is moved
// after LOAD GAME (the list shows its items in pool order). The list input call is hooked: RESTART LEVEL clears the
// pause like RESUME does ([0x60F924] = block 0 +0x36C, [0x5ED0D8]) and hands the level to console.cpp's level start,
// which loads it on the next front-end update - the state then goes to 0xE, whose leave of the pause screen un-pauses
// the sound. Online: the host's level loads are mirrored to the players (online.cpp), so only the host may restart.
// PHOTO MODE (id 0x419, pool item 5, after RESTART LEVEL; photomode.cpp) answers the update with RESUME's id 0x415 -
// the game resumes as usual - and photo mode opens once play is back (state 0xE). Greyed out where it can't open.
#include <windows.h>

#include <cstdint>

#include "core/log.h"
#include "core/net.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kPauseSetupSlot = 0x5D9214, kPauseSetup = 0x471F30;
constexpr uint32_t kPauseListInputCall = 0x472123, kListInput = 0x4E5D80;
constexpr uint32_t kItemSet = 0x4E4FB0, kRelayout = 0x4E8130, kPauseItemDraw = 0x472290, kPauseTitle = 0x0556F2D1;
constexpr uint32_t kPauseFlag = 0x60F924, kResumeFlag = 0x5ED0D8;
constexpr uint16_t kRestartItem = 0x418, kPhotoItem = 0x419, kResumeItem = 0x415;
constexpr uint32_t kRestartText = 0x5AB7F860;
constexpr uint32_t kItemPlain = 0x24, kItemDisabled = 0x02;   // item flags as the set-up passes them (SAVE GAME: 0x26)

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
using ItemSetFn = void(__thiscall*)(void*, int, uint32_t, int, int, uint32_t, int, uint32_t, int);

bool Allowed() { return net::LocalPlayer() != 2; }   // an online player: the host decides the level

void* g_pauseSetupNext = reinterpret_cast<void*>(kPauseSetup);  // pointer hook chain (patch::HookSlot)

void __fastcall PauseSetup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(g_pauseSetupNext)(screen);
    void* list = At<void*>(screen, 8);
    if (!list || At<int16_t>(list, 0x24) < 6 || At<int16_t>(list, 0x26) != 4) {   // pool 7, 4 items listed
        dslog::Write("Pause menu: unexpected list (capacity %d, items %d) - no extra rows", list ? At<int16_t>(list, 0x24) : -1,
                     list ? At<int16_t>(list, 0x26) : -1);
        return;
    }
    void** items = At<void**>(list, 0x2C);
    void* restart = items[4];
    void* photo = items[5];
    const int16_t cursor = At<int16_t>(list, 0x28);
    auto add = [&](void* item, uint16_t id, uint32_t text, bool enabled) {
        reinterpret_cast<ItemSetFn>(kItemSet)(item, id, text, 0, kItemPlain | (enabled ? 0 : kItemDisabled), 0, 0,
                                              kPauseItemDraw, 0);
        reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, item);
    };
    add(restart, kRestartItem, kRestartText, Allowed());
    add(photo, kPhotoItem, features::kPhotoModeText, features::PhotoModeAvailable());
    // SAVE GAME, LOAD GAME, RESTART LEVEL, PHOTO MODE, OPTIONS, MAIN MENU
    void* const options = items[2];
    void* const mainMenu = items[3];
    items[2] = restart;
    items[3] = photo;
    items[4] = options;
    items[5] = mainMenu;
    // two more rows: list +0x38 row pitch, +0x40 area height, +0x42 visible rows; the panel laid out again around them
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + 2 * At<int16_t>(list, 0x38));
    if (At<int16_t>(list, 0x42) < At<int16_t>(list, 0x26)) At<int16_t>(list, 0x42) = At<int16_t>(list, 0x26);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kPauseTitle, 0, 0, 0x7FFF);
    reinterpret_cast<void(__thiscall*)(void*)>(kRelayout)(list);
    At<int16_t>(list, 0x28) = cursor;
}

int16_t __fastcall PauseListInput(void* list, void*) {
    const int16_t id = reinterpret_cast<int16_t(__thiscall*)(void*)>(kListInput)(list);
    if (id == kRestartItem && Allowed() && features::RestartLevel()) {
        *reinterpret_cast<uint32_t*>(kPauseFlag) = 0;
        *reinterpret_cast<uint8_t*>(kResumeFlag) = 0;
        dslog::Write("Pause menu: RESTART LEVEL");
    }
    if (id == kPhotoItem && features::PhotoModeAvailable()) {   // the game's own RESUME, then photo mode opens
        features::RequestPhotoMode();
        dslog::Write("Pause menu: PHOTO MODE");
        return kResumeItem;
    }
    return id;   // 0x418 / 0x419 are outside the update's 0x411..0x417 range: ignored there
}
}  // namespace

void features::ApplyPauseMenu() {
    static bool done = false;
    if (done) return;
    done = true;
    const uint32_t setup = kPauseSetup;
    if (!patch::Matches(kPauseSetupSlot, &setup, 4)) {
        dslog::Write("[fail] Pause menu: unexpected pause screen - RESTART LEVEL not added");
        return;
    }
    const bool ok = patch::HookCall(kPauseListInputCall, reinterpret_cast<const void*>(&PauseListInput), kListInput) &&
                    patch::HookSlot(kPauseSetupSlot, reinterpret_cast<const void*>(&PauseSetup), &g_pauseSetupNext,
                                    kPauseSetup);
    dslog::Write(ok ? "[ok]   Pause menu: RESTART LEVEL" : "[fail] Pause menu: RESTART LEVEL");
}

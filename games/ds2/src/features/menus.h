#pragma once
#include <cstdint>

// Front-end menu structure (menus.cpp), as in DesertStormFix: the main menu reads SINGLE PLAYER / CO-OP / OPTIONS /
// QUIT; SINGLE PLAYER opens the game's own (unused) single-player menu screen (front-end state 3) with START GAME /
// LOAD GAME / TRAINING doing exactly what the main menu items did; CO-OP opens our "CO-OPERATIVE GAME" screen at
// front-end state kCoopState. The co-op feature plugs in through SetCoopScreenHooks (its own rows, input, draw).
//
// Game side (CDS II.exe):
//   state setter      FUN_0043dfd0 (cdecl(state)); current state [0x63433C], previous [0x634340], busy [0x63434C]
//   state -> screen   FUN_0043bba0 (cdecl(state), states 1..0x35, one global per screen; hooked for kCoopState)
//   menu screens      0x128-byte objects, list object at +0x108 (pool of 8 item objects at list+0x44, item count +0x3E,
//                     selected index +0x40); vtable +0xC set-up (rebuilds the items, run on every entry), +0x10 update
//                     (returns 0), +0x1C enter, +0x4C init(font, ...)
//   menu items        FUN_0051d4f0 thiscall(item; id, descHash, textHash, 0, flags 0x880, 0, 0, drawCb 0x466500, 0, 0,
//                     0x60), then list->vtable[1](item); list selection FUN_0051fa20 (thiscall list) -> activated id or 0
//   texts             FUN_004fbe50 thiscall(catalog; hash) -> wchar_t* (hooked: own texts first, see AddText)
namespace features::menus {

constexpr int kSinglePlayerState = 3;  // the game's own single-player menu screen ([0x634320])
constexpr int kCoopState = 0x36;       // our CO-OPERATIVE GAME screen (first state number the game doesn't use)
constexpr int kMainMenuState = 2;      // [0x634324]

// Own UTF-16 texts for item / title hashes (checked before the game's catalog; any language). Hashes 0xC0B2xxxx are
// used by menus.cpp; pick another range for new texts.
void AddText(uint32_t hash, const wchar_t* text);

// The CO-OPERATIVE GAME screen object (a menu screen of the game's single-player-menu class), null before the
// front-end's screens exist.
void* CoopScreen();

// Co-op screen behaviour. setup: rebuild the rows (called on every entry; use BeginList / AddItem / EndList), return
// true when done (false = the placeholder rows are built instead). update: called every front-end frame while the
// screen is current, with the activated item id (0 = none) and whether Back was pressed; return 1 when the hook handled
// Back itself, else Back returns to the main menu. Any member may be null.
struct CoopScreenHooks {
    bool (*setup)(void* screen) = nullptr;
    int (*update)(void* screen, int activatedId, bool backPressed) = nullptr;  // return 1 = Back handled by the hook
};
void SetCoopScreenHooks(const CoopScreenHooks& hooks);

// Building a menu screen's list the game's way (up to 8 rows; unused rows are filled with the game's empty boxes).
void BeginList(void* screen);
void AddItem(void* screen, int index, int id, uint32_t textHash, uint32_t descHash, uint32_t flags = 0x880);
void EndList(void* screen, int itemCount, uint32_t titleHash);

void SetState(int state);  // FUN_0043dfd0
int State();               // [0x63433C]

}  // namespace features::menus

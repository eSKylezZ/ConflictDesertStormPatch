#pragma once
#include <cstdint>

// Front-end menu structure (menus.cpp), as in DesertStormFix: the main menu reads SINGLE PLAYER / CO-OP / [MODS] /
// OPTIONS / QUIT; SINGLE PLAYER opens the game's own (unused) single-player menu screen (front-end state 3) with START GAME /
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

// OPTIONS screen rows HUD SCALING (ON / OFF) and HUD SIZE (slider 25 - 400 %), shown while a provider is set (the HUD
// feature: hud.cpp in the dinput8 build, the ds2fix.widescreen plugin's interface in the PSE build). set() applies and
// saves at once.
struct HudOption {
    bool (*scaling)() = nullptr;
    uint32_t (*percent)() = nullptr;
    void (*set)(bool on, uint32_t percent) = nullptr;
};
void SetHudOption(const HudOption& hud);

// OPTIONS screen ON / OFF rows for plugins' settings (the ds2fix.options interface, src/pse/ds2fix_options.h): after
// the HUD rows, in the order added, while the screen's 8 rows have room. The row reads "<label>: ON / OFF"; selecting
// it calls set(!get()). AddToggle returns the slot (-1 = all taken); the label / description pointers must stay valid.
constexpr int kMaxToggles = 4;
struct Toggle {
    const char* label = nullptr;
    const char* description = nullptr;
    bool (*get)() = nullptr;
    void (*set)(bool on) = nullptr;
};
int AddToggle(const Toggle& toggle);
void RemoveToggle(int index);

// MODS screen (front-end state kModsState, the mods menu feature: modsmenu.cpp). While hooks are set the main menu
// reads SINGLE PLAYER / CO-OP / MODS / OPTIONS / QUIT. setup: build the rows (BeginList / AddItem / EndList) on every
// entry. update: every front-end frame on the screen, with the activated item id (0 = none), the navigation input the
// list read saw (-1 up, +1 down, 0 none; keyboard arrows / pad, not the mouse), the selected row before that read and
// whether Back was pressed (Back always returns to the main menu after the hook). mainMenuDraw: after the main menu
// has drawn itself (DrawLabel there lands on the main menu).
constexpr int kModsState = 0x37;   // second state number the game doesn't use
struct ModsScreenHooks {
    void (*setup)(void* screen) = nullptr;
    void (*update)(void* screen, int activatedId, int nav, int rowBefore, bool backPressed) = nullptr;
    void (*mainMenuDraw)() = nullptr;
};
void SetModsScreenHooks(const ModsScreenHooks& hooks);
int SelectedRow(void* screen);              // list +0x40
// CUSTOM CAMPAIGNS screen (front-end state kCampaignsState, scripthost's campaigns.cpp): while hooks are set and
// available() says so, SINGLE PLAYER reads START GAME / LOAD GAME / TRAINING / CUSTOM CAMPAIGNS. setup: rows (BeginList /
// AddItem / EndList) on every entry; update: every front-end frame with the activated id and Back (Back returns to
// SINGLE PLAYER after the hook). OpenDifficulty(): the game's DIFFICULTY screen (state 8) for a campaign - when a
// difficulty is picked there (the screen would go on to state 0xA) start() runs instead and the menu stays put; leaving
// the screen any other way forgets the campaign.
constexpr int kCampaignsState = 0x38;   // third state number the game doesn't use
constexpr int kDifficultyState = 8;     // the game's DIFFICULTY screen ([0x634314], update FUN_0045bf90)
struct CampaignScreenHooks {
    bool (*available)() = nullptr;
    void (*setup)(void* screen) = nullptr;
    void (*update)(void* screen, int activatedId, bool backPressed) = nullptr;
    void (*start)() = nullptr;
};
void SetCampaignScreenHooks(const CampaignScreenHooks& hooks);
void OpenDifficulty();
void SetSelectedRow(void* screen, int row);

// Text with the game's menu fonts (large [0x62F440] as the prompt line, small [0x62F43C]; FUN_00526e40 alpha,
// FUN_00526e20 colour, FUN_00529490 draw(text, x, y, 1)): x = left end, y = the line's middle, in the 2D screen's pixels
// - ScreenSize(), 1280 x 1024 while the front-end's virtual screen is on (the menu panel then spans about x 372..910,
// y 200..713). Only from a draw callback.
void DrawLabel(const char* text, int x, int y, uint32_t argb, bool smallFont = false);
void ScreenSize(int& w, int& h);   // renderer [0x680038] +0x41A38 / +0x41A3C

void SetState(int state);  // FUN_0043dfd0
int State();               // [0x63433C]

}  // namespace features::menus

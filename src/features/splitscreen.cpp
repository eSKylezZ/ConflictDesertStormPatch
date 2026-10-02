// Local split screen (2-4 players), rebuilt from what the PC build kept of the console code.
//
// Kept on PC: the per-viewport render loop FUN_0040ef70 (current player DAT_0063c9e8 = viewport, camera slot
// [camMgr 0x63c9d8 + 0xec0c + i*4], per-player HUD FUN_0040e0f0(i)), the split flag 0x60ed30 (set at level start),
// and the layout builder FUN_00547b30 (thiscall renderer, table; = Xbox Renderer_BuildSplitViewports 0xbf3f0),
// which nothing calls any more. The Xbox build called it from Level_Initialise with a layout table picked by
// player count and orientation (Split_SelectLayoutTable 0x18ed0).
//
// Also kept: the local multiplayer set-up behind two globals (the "-p" start-up option): multiplayer flag
// 0x606410 and player count 0x606414 (2-4). The level loader FUN_0040de70 then adds that many player input
// blocks (FUN_00451430: block i gets joystick i and binding set i) and clears the flag for the front-end/outro;
// level init's player set-up FUN_0040bae0 creates the extra players and binds each *renderer viewport* to its
// camera and soldier; FUN_00436f70 picks which soldier each player controls for 2, 3 or 4 players.
// So the viewports must exist before the level loads (the Xbox built them in Level_Initialise) - building them
// afterwards leaves player 2's block unbound and the render loop crashes in FUN_0044de20 (block+0x30 = null).
//
// Stripped on PC: after player 1 gets their soldier (FUN_0044ada0 picks the squad member, called at 0x40BB5E in
// FUN_0040bae0), the Xbox's FUN_00018d70 did the same for players 2..N: block[i]+0x30 = player object i
// (table 0x606a68), then the soldier picker on block[i]. AssignPlayers re-adds that loop at the same call. It
// also sets each block's viewport index (block+0x474, read by the HUD: weapon display, item icon) - the PC's
// add-player FUN_00451430 never does, so every player's HUD looked up view 0.
//
// Bindings: the table [0x60687c] = {sets, slots, actions, data} is allocated with ONE set at start-up
// (FUN_00409f30: FUN_0040a040(1, 3, 0x50)), but block i evaluates set i (block+0x3c0, FUN_00450770) - players 2-4
// read past the end (crash in FUN_0040a110). The allocation is patched to 4 sets; default.key/current_key only
// load and save set 0, so sets 1-3 are copies of set 0, refreshed at every level load (picks up rebinding).
// Input devices: every block gets keyboard/mouse + joypad (block+0x38 bits 1/2); players 2-4 are pad-only
// (joystick index block+0x3c8 = player index).
//
// HUD per view. Each view's HUD is drawn in a "virtual screen": renderer W/H = view size and the view's origin =
// 0,0 (layout, the HUD cave's k and the layout clamps then fit the view), the HUD sheets scaled at level start
// (kScaledSheets) are scaled by k(view)/k(full), and every pre-transformed quad (FVF 0x144) drawn through
// FUN_00547400 (renderer's DrawPrimitiveUP(TRIANGLEFAN), 18 callers) is moved to the view's real origin.
// Wrapped this way:
//  - 0x40E512 FUN_0048bfc0(block): crosshair / target reticle (cdecl, returns the aimed target);
//  - 0x40E7EB FUN_004514d0: a loop over ALL players' blinking item icon (FUN_0044bda0(block j), placed via
//    GetViewport(j)), run once per view - replaced by the current view's player only;
//  - 0x40E827 FUN_004773d0(i) (compass, texts) and 0x40E852 FUN_004dbd60;
//  - 0x40F143 FUN_004d25d0: the view's render-list flush at the end of the loop iteration;
//  - 0x40F1DC SetViewport(full screen) after the loop: grey divider lines between the views (SetFullViewport);
//  - the soldier panel callback 0x48d9c0 -> FUN_0048d9e0 (registered with `push 0x48D9C0` at 0x48D79A for each
//    squad member; its `this` = 7th stack arg = the player's panel list: viewer block at +0x20, view index at
//    block+0x474, position at +0x24/+0x26 in screen coordinates, row height at [+8]+0x38). Called for all players
//    after the loop; draws the soldiers the viewer owns, stacking upward (the position is the cursor). The wrapper
//    uses the owner's view, makes the position view-local (origin subtracted/added back, not restored - that would
//    undo the stacking) and scales the row height to the view's HUD size. The weapon display it also draws is
//    placed view-locally (FUN_004a05d0 with block+0x474).
//
// HUD layout per view: HUD elements are placed by FUN_004a05d0(out, view, anchor): in multiplayer the anchor is
// taken from variant [table+0x70+view*4] of the anchor table 0x619a30 (9 anchors per variant, fractions of the
// view: 0 full screen, 1 top/bottom half, 2 left/right half, 3 quarter), plus a per-view offset [table+view*8]
// (fraction of the screen; the Xbox's TV-safe margin, zeroed on PC). The table (0x80 bytes, Xbox format:
// offsets[4][2], views[4]{flag,x,y,w,h}, variants[4]) is [0x606420] = field +0x10 of the split object 0x606410
// (+0 multiplayer, +4 players, +0xc orientation: 1 = top/bottom), chosen by FUN_00409b30/FUN_00409b60 - which on
// PC only kept the 1- and 2-player tables (0x5e6a48, 0x5e6ac8 top/bottom, 0x5e6b48 side by side). 3/4 players
// kept the 2-player side-by-side table (views 3-4 got variant 4, not a HUD layout) -> our own tables below.
// The split variants put the weapon/ammo display (anchor 1) in the middle of the view, over the soldier, and the
// soldier panel (anchor 0) mid-left; both go on the full-screen layout's bottom line while split screen is on.
//
// Here: on the frame a level load is pending ([0x60ec8c] = 1, the dispatcher FUN_0040de00 runs FUN_0040de70
// right after our frame hook), set the multiplayer globals and build the viewports for the level about to load
// (next level name: pointer at 0x5e7028). Later frames only rebuild if something reset the viewports.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"
#include "features/padio.h"
#include "features/padlayout.h"
#include "features/splitscreen_layout.h"

namespace {
constexpr uint32_t kRenderer = 0x63C924;           // renderer object pointer
constexpr uint32_t kViewportCount = 0x20;          // renderer + 0x20
constexpr uint32_t kBuildSplitViewports = 0x547B30;  // thiscall (renderer, table), ret 4
constexpr uint32_t kResetViewports = 0x542E50;       // thiscall (renderer): one full-screen viewport
constexpr uint32_t kSetViewport = 0x542F40, kSetFullViewportSite = 0x40F1DC;  // thiscall (renderer, D3DVIEWPORT8*)
constexpr uint32_t kSplitActive = 0x40E080;         // cdecl: 0 while a cutscene renders full screen
constexpr uint32_t kLoadPending = 0x60EC8C;        // byte: 1 = the dispatcher loads a level this frame
constexpr uint32_t kNextLevel = 0x5E7028;          // char* name of the level to load ("mission1.dll")
constexpr uint32_t kMultiplayer = 0x606410;        // local multiplayer on
constexpr uint32_t kPlayerCount = 0x606414;        // number of local players (2-4)
constexpr uint32_t kInputBlocks = 0x60F5B8;        // per-player input blocks, 0x478 bytes each
constexpr uint32_t kInputBlockSize = 0x478;
constexpr uint32_t kBlockPlayer = 0x30;            // block + 0x30: the player object it drives
constexpr uint32_t kBlockJoystick = 0x3C8;         // block + 0x3c8: joystick index (-1 = none)
constexpr uint32_t kPlayerTable = 0x606A68;        // player objects [4]
constexpr uint32_t kPickSoldier = 0x44ADA0;        // fastcall (block): hand the block its squad member
constexpr uint32_t kPickSoldierSite = 0x40BB5E;    // call kPickSoldier for player 1 in FUN_0040bae0
constexpr uint32_t kBlockDevices = 0x38;           // block + 0x38: bit 1 keyboard/mouse, bit 2 joypad
constexpr uint32_t kBindingTable = 0x60687C;       // {sets, slots, actions, uint32* data}
constexpr uint32_t kBindingSetsPush = 0x409F60;    // push 1 (sets) for FUN_0040a040(1, 3, 0x50)
constexpr uint32_t kMaxPlayers = 4;
constexpr uint32_t kOrientation = 0x60641C;        // split object +0xc: 1 = views stacked top/bottom
constexpr uint32_t kHudLayoutTable = 0x606420;     // split object +0x10: HUD layout table (see HudTable)
constexpr uint32_t kHudTable1p = 0x5E6A48;         // the game's full-screen table
constexpr uint32_t kHudScaleVa = 0x5D7B00;          // float: the user's HUD size (HUD cave)
constexpr uint32_t kHudAnchors = 0x619A30;         // float[variant][9][2], fractions of the view (filled at run time)
constexpr int kAnchorWeapon = 1;                   // weapon icon + ammo + rank
constexpr int kAnchorPanel = 0;                    // soldier panel (top of the last panel in the stack)
// HUD pass
constexpr uint32_t kScreenW = 0x40688, kScreenH = 0x4068C;  // renderer + : back buffer size used for layout
constexpr uint32_t kFullViewport = 0x40650;                 // renderer + : full-screen D3DVIEWPORT8
constexpr uint32_t kCurrentViewport = 0x40668;              // renderer + : D3DVIEWPORT8* set last
constexpr uint32_t kCurrentFvf = 0x40A58;                   // renderer + : FVF set last
constexpr uint32_t kFvfScreenQuad = 0x144;                  // XYZRHW | DIFFUSE | TEX1, 0x1c bytes
constexpr uint32_t kDrawFan = 0x547400;                     // thiscall (renderer, prims, verts, stride) ret 0xc
constexpr uint32_t kDrawFanCallers[] = {0x4D2228, 0x4D2B69, 0x52009E, 0x53CCBF, 0x53D079, 0x53D2FE,
                                        0x53D508, 0x53D88C, 0x53DBFB, 0x53E01B, 0x53E23C, 0x53E50C,
                                        0x53E840, 0x53EE1F, 0x54192E, 0x549D30, 0x54D8F6, 0x54DB29};
constexpr uint32_t kPanels = 0x4514D0, kPanelsSite = 0x40E7EB;      // thiscall (input manager): all panels
constexpr uint32_t kPanel = 0x44BDA0;                              // thiscall (block): one soldier panel
constexpr uint32_t kCurrentPlayer = 0x63C9E8;                      // view / player being rendered
constexpr uint32_t kFlushView = 0x4D25D0, kFlushViewSite = 0x40F143;  // thiscall (view controller)
constexpr uint32_t kPanelCallback = 0x48D9C0, kPanelCallbackPush = 0x48D79A;  // cdecl, this = 7th arg
constexpr uint32_t kItemBlock = 0x20, kBlockViewport = 0x474;       // item + 0x20: input block; block + 0x474
constexpr uint32_t kItemPos = 0x24;                                 // item + 0x24/+0x26: x, y (int16, screen)
constexpr uint32_t kItemList = 0x8, kListRowHeight = 0x38;          // item + 8: panel list; list + 0x38: row (int16)
constexpr uint32_t kViewports = 0x24, kViewportStride = 0x18;       // renderer + 0x24 + i * 0x18
constexpr uint32_t kReticle = 0x48BFC0, kReticleSite = 0x40E512;    // cdecl (block) -> target
constexpr uint32_t kHudDraw = 0x4773D0, kHudDrawSite = 0x40E827;    // cdecl (player)
constexpr uint32_t kHudDraw2 = 0x4DBD60, kHudDraw2Site = 0x40E852;  // thiscall (this, player, x) ret 8
// Globals -> 2D image sheets (scale +0x24/+0x28): GWInt HUD, HUD art, GWInt, NewLogo, CDSFont, Effects (also the
// zoom / crosshair art: the game re-sets its scale now and then, through the HUD cave, which gives the view's k
// inside a HUD pass and the full screen's outside - without it in this list the crosshair flipped between the two
// sizes). A sheet re-scaled while drawing gets the view's k; the pass restores the value from before it.
constexpr uint32_t kScaledSheets[] = {0x60EE18, 0x60EE1C, 0x60EE20, 0x60EE24, 0x60EDB0, 0x60EE28};

// Table read by FUN_00547b30: up to 4 entries {flag, x, y, w, h} (fractions of the screen), flag 0 ends it.
struct Entry {
    uint32_t used;
    float x, y, w, h;
};

// Xbox layout table format (default.xbe 0x234e08..0x235088; the PC keeps the first three with zero offsets).
struct HudTable {
    float offset[4][2];  // per view, fraction of the screen, added to every HUD anchor
    Entry views[4];
    uint32_t variant[4];  // anchor set per view: 1 half (top/bottom), 2 half (left/right), 3 quarter
};
static_assert(sizeof(HudTable) == 0x80, "Xbox layout table size");

// Variants as on the Xbox; views as in splitscreen_layout.h (the renderer's views are built from those).
HudTable MakeHudTable(splitscreen::Layout layout, int players) {
    HudTable t{};
    const bool side = layout == splitscreen::Layout::Vertical;
    for (int i = 0; i < 4; ++i) {
        t.variant[i] = 4;
        if (i >= players) continue;
        splitscreen::View v = splitscreen::ViewRect(layout, players, i);
        t.views[i] = {1, v.x, v.y, v.w, v.h};
        const bool quarter = v.w < 0.75f && v.h < 0.75f;
        t.variant[i] = quarter ? 3 : side ? 2 : 1;
    }
    return t;
}

int g_builtPlayers = 1;
uint32_t g_builtLayout = ~0u;
#ifndef DS_DIST
DWORD g_probeStart = 0;  // dev HUD probe (below): when split views were built
#endif

// Wanted number of views: the co-op session's players (front-end CO-OP screen); dev builds can force a count.
int WantedPlayers() {
    if (features::CoopPlayers() >= 2) return features::CoopPlayers();
#ifndef DS_DIST
    DWORD v = 0, size = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "SplitPlayers", RRF_RT_REG_DWORD, nullptr,
                     &v, &size) == ERROR_SUCCESS && v >= 2 && v <= 4)
        return static_cast<int>(v);
#endif
    return 1;
}

void Build(uintptr_t renderer, int players, uint32_t layout) {
    static HudTable hudTable;
    if (players <= 1) {
        reinterpret_cast<void(__thiscall*)(uintptr_t)>(kResetViewports)(renderer);
        *reinterpret_cast<uint32_t*>(kHudLayoutTable) = kHudTable1p;
    } else {
        hudTable = MakeHudTable(static_cast<splitscreen::Layout>(layout), players);
        *reinterpret_cast<uint32_t*>(kOrientation) = layout == static_cast<uint32_t>(splitscreen::Layout::Horizontal);
        *reinterpret_cast<HudTable**>(kHudLayoutTable) = &hudTable;
        Entry table[5]{};
        for (int i = 0; i < players; ++i) {
            splitscreen::View v = splitscreen::ViewRect(static_cast<splitscreen::Layout>(layout), players, i);
            table[i] = {1, v.x, v.y, v.w, v.h};
        }
        reinterpret_cast<void(__thiscall*)(uintptr_t, const Entry*)>(kBuildSplitViewports)(renderer, table);
    }
    g_builtPlayers = players;
    g_builtLayout = layout;
#ifndef DS_DIST
    if (players > 1) g_probeStart = GetTickCount();
#endif
    dslog::Write("Split screen: %d view(s), layout %u, renderer viewports = %u", players, layout,
                 *reinterpret_cast<uint32_t*>(renderer + kViewportCount));
}

// Replaces the player-1 soldier pick in the level's player set-up; adds players 2..N like the Xbox build.
void __fastcall AssignPlayers(uintptr_t block0) {
    using Pick = void(__fastcall*)(uintptr_t);
    reinterpret_cast<Pick>(kPickSoldier)(block0);
    if (!*reinterpret_cast<uint32_t*>(kMultiplayer)) return;
    const uint32_t players = *reinterpret_cast<uint32_t*>(kPlayerCount);
    for (uint32_t i = 0; i < players && i < kMaxPlayers; ++i)
        *reinterpret_cast<uint32_t*>(kInputBlocks + i * kInputBlockSize + kBlockViewport) = i;
    for (uint32_t i = 1; i < players && i < 4; ++i) {
        uintptr_t block = kInputBlocks + i * kInputBlockSize;
        *reinterpret_cast<uint32_t*>(block + kBlockPlayer) = reinterpret_cast<uint32_t*>(kPlayerTable)[i];
        *reinterpret_cast<uint32_t*>(block + kBlockDevices) &= ~1u;  // pad only
        reinterpret_cast<Pick>(kPickSoldier)(block);
    }
#ifndef DS_DIST
    // Dev\SplitKeyboardPlayer = n (1-4): in a test split (no co-op session) player n is keyboard & mouse only and
    // the others pad only - like a co-op session with a keyboard player.
    DWORD kbPlayer = 0, kbSize = sizeof kbPlayer;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "SplitKeyboardPlayer", RRF_RT_REG_DWORD, nullptr,
                 &kbPlayer, &kbSize);
    if (kbPlayer >= 1 && kbPlayer <= players && features::CoopPlayers() < 2)
        for (uint32_t i = 0; i < players && i < kMaxPlayers; ++i) {
            uintptr_t block = kInputBlocks + i * kInputBlockSize;
            *reinterpret_cast<uint32_t*>(block + kBlockDevices) = i + 1 == kbPlayer ? 1u : 2u;
            dslog::Write("[dev]  player %u: %s", i + 1, i + 1 == kbPlayer ? "keyboard & mouse" : "pad");
        }
#endif
    // Co-op session: each player's own device (the pad they joined with, or keyboard & mouse).
    if (features::CoopPlayers() >= 2) {
        for (uint32_t i = 0; i < players && i < kMaxPlayers; ++i) {
            uintptr_t block = kInputBlocks + i * kInputBlockSize;
            const int joystick = features::CoopJoystick(static_cast<int>(i));
            // Pad players: pad only (bit 2) on their joystick. Keyboard players: keyboard & mouse only (bit 1) - they
            // keep a joystick index (their player number, like the game sets it): FUN_00450770 ignores a block with
            // joystick -1 entirely, keyboard included; with the pad bit clear no pad is read for them.
            *reinterpret_cast<uint32_t*>(block + kBlockDevices) = joystick >= 0 ? 2u : 1u;
            *reinterpret_cast<int32_t*>(block + kBlockJoystick) = joystick >= 0 ? joystick : static_cast<int32_t>(i);
            dslog::Write("Split screen: player %u uses %s %d", i + 1, joystick >= 0 ? "joystick" : "keyboard", joystick);
        }
    }
    features::VersusAssignPlayers();  // versus: MP soldiers instead of squad members (versus.cpp)
    dslog::Write("Split screen: %u players assigned", players);
}

// ---- analog controls ----
// FUN_00450920 (block, action, opposite action) -> -1..1: the value of an axis action (move, turn, aim, ...) from the
// block's joystick axes (type 1 bindings, float per axis code at 0x754268 + (code + joystick * 0x54) * 4), or 1 for a
// held key (type 0, FUN_00539da0), without looking at the block's devices (+0x38: bit 0 keyboard/mouse, bit 1 pad) -
// the button path FUN_00450770 does. In co-op every player moved with the keyboard, and a keyboard player (whose
// block keeps a joystick index) with that pad's sticks. Replaced by the same logic with the device checks.
constexpr uint32_t kAxisValue = 0x450920;
constexpr uint32_t kBindingLookup = 0x40A110, kKeyHeld = 0x539DA0, kAxisTable = 0x754268, kKeyboardUsed = 0x60F5B0;

// Gyro aiming (DualSense / DualShock 4, CONTROLLER screen GYRO AIM): the pad's turn rate added like a stick
// deflection - 120 deg/s = full stick, then the look sensitivity on top. On foot it turns (TURN result: + = right,
// AIM_PITCH: + = down); in aim mode (block +0x318 = 1) it moves the head axes instead, never both.
float GyroLook(int player, int joystick, int action, bool aiming) {
    const int mode = padlayout::GyroMode(player);
    if (!mode || (mode == 1 && !aiming)) return 0.0f;
    float yawRight, pitchDown;
    if (!padio::Gyro(joystick, yawRight, pitchDown)) return 0.0f;
    constexpr float kFullStickDps = 120.0f;
    const bool yaw = aiming ? action == 0x15 : action == 0xB;
    const bool pitch = aiming ? action == 0x16 : action == 0xF;
    return yaw ? yawRight / kFullStickDps : pitch ? pitchDown / kFullStickDps : 0.0f;
}

float __fastcall AxisValue(uint8_t* block, void*, int action, int opposite) {
    const int joystick = *reinterpret_cast<int32_t*>(block + 0x3C8);
    if (joystick == -1) return 0.0f;
    auto* table = *reinterpret_cast<uint8_t**>(kBindingTable);
    const int set = *reinterpret_cast<int32_t*>(block + 0x3C0);
    const uint32_t slots = *reinterpret_cast<uint32_t*>(table + 4);
    const uint32_t devices = *reinterpret_cast<uint32_t*>(block + 0x38);
    using Lookup = uint32_t(__thiscall*)(void*, int, int, int);
    auto axis = [&](int act, float& value) {
        if (!(devices & 2)) return;
        for (uint32_t slot = 0; slot < slots; ++slot) {
            const uint32_t code = reinterpret_cast<Lookup>(kBindingLookup)(table, set, slot, act);
            if (code == 0xFFFFFFFF || (code & 0xFFFF0000) != 0x10000) continue;
            value = reinterpret_cast<const float*>(kAxisTable)[(code & 0xFFFF) + joystick * 0x54];
            if (value != 0.0f) *reinterpret_cast<uint32_t*>(block + 0x458) = 0;
        }
    };
    auto key = [&](int act) {
        if (!(devices & 1)) return false;
        bool held = false;
        for (uint32_t slot = 0; slot < slots; ++slot) {
            const uint32_t code = reinterpret_cast<Lookup>(kBindingLookup)(table, set, slot, act);
            if (code != 0xFFFFFFFF && (code & 0xFFFF0000) == 0 &&
                reinterpret_cast<int(__cdecl*)(uint32_t)>(kKeyHeld)(code & 0xFFFF)) {
                held = true;
                *reinterpret_cast<uint32_t*>(kKeyboardUsed) = 1;
            }
        }
        return held;
    };
    float value = 0.0f;
    axis(action, value);
    if (opposite != 0x50) {
        if (value == 0.0f) axis(opposite, value);
        else value = -value;
    }
    // Looking with a stick (TURN_LEFT / TURN_RIGHT 0xB / 0xA, AIM_YAW / AIM_PITCH 0xE / 0xF, aim mode HEAD_YAW /
    // HEAD_PITCH 0x15 / 0x16): the player's look sensitivity. Values above 1 are fine - the mouse path writes larger
    // ones into the same fields.
    const bool look = action == 0xA || action == 0xB || action == 0xE || action == 0xF || action == 0x15 || action == 0x16;
    const int player = static_cast<int>((block - reinterpret_cast<uint8_t*>(0x60F5B8)) / 0x478);
    if (look && (devices & 2)) value += GyroLook(player, joystick, action, *reinterpret_cast<int32_t*>(block + 0x318) == 1);
    if (value != 0.0f && look) value *= padlayout::LookSensitivity(player) / 100.0f;
    if (value == 0.0f) {
        if (key(action)) value = 1.0f;
        if (opposite != 0x50) {
            if (value != 0.0f) return -value;
            if (key(opposite)) value = 1.0f;
        }
    }
    return value;
}

// Mouse look: the smoothed mouse movement (FUN_00538080 on the mouse smoother 0x754BEC, thiscall (&dx, &dy) ret 8) is
// read for turning / aiming at 0x44B396 and 0x44F8D7 with esi = the player's input block, again without its device
// check - the mouse turned every player. A block without keyboard & mouse (+0x38 bit 0) now gets no movement.
constexpr uint32_t kMouseDelta = 0x538080;
constexpr uint32_t kMouseDeltaSites[] = {0x44B396, 0x44F8D7};

__declspec(naked) void MouseDeltaStub() {
    __asm {
        test byte ptr [esi + 0x38], 1
        jnz read
        mov eax, [esp + 4]
        mov dword ptr [eax], 0
        mov eax, [esp + 8]
        mov dword ptr [eax], 0
        ret 8
    read:
        mov eax, kMouseDelta
        jmp eax
    }
}

// Downed soldier: FUN_0044eee0 (per player block, every frame) gives a player whose soldier went down 5 s before
// switching them to another living squad member, and meanwhile slows the whole game down (tail jump at 0x44EFAB to the
// "Slow down time" ramp FUN_00443d00: time scale -0.5/s down to 0.2). In split screen a player may own no other
// soldier (Mission 1: Bradley alone) - the switch never happens and the game stayed slow for everyone while the other
// players could still revive him. In split screen there is no slow-down; the downed player waits.
constexpr uint32_t kSlowDownJump = 0x44EFAB, kSlowDown = 0x443D00;

__declspec(naked) void SlowDownStub() {
    __asm {
        mov eax, g_builtPlayers
        cmp eax, 2
        jl slow
        ret
    slow:
        mov eax, kSlowDown
        jmp eax
    }
}

// Soldier panels: the in-game HUD screen ([0x617A04], state 0xE; vtable 0x5D9428) builds its panel list in its
// set-up (vtable +0xC, FUN_0048d4e0) only when the screen is entered: it walks the squad's team list (character
// manager [0x63C9A0], FUN_004c9720(team [0x5F9B94], 1) = ((u32*)mgr[+4])[team * mgr.b[+0x11] + 1], linked by +0x14)
// and takes up to 4 soldiers passing InSquad. A soldier who joins mid-mission (Foley rescued in Mission 1: added to the
// list only then) got no panel until a save was loaded. The same walk runs every frame; a soldier new to that set
// re-runs the set-up (it clears the list first).
constexpr uint32_t kHudScreen = 0x617A04, kFrontEndState = 0x617C18;
constexpr uint32_t kCharacters = 0x63C9A0, kSquadTeam = 0x5F9B94;
uintptr_t g_panelSoldiers[4];

// The set-up's rule (0x48D681..0x48D6CA): game mode [0x610F5C] 0 - state [[s+0x24]+0x8C] 0, 3 or 4 and not flag
// 0x20000 in [[s+0x2C]+0xC]; mode 1 - vtable +0x4C(1).
bool InSquad(uintptr_t soldier) {
    if (!soldier) return false;
    const uint32_t mode = *reinterpret_cast<uint32_t*>(0x610F5C);
    if (mode == 0) {
        const uintptr_t info = *reinterpret_cast<uintptr_t*>(soldier + 0x24), flags = *reinterpret_cast<uintptr_t*>(soldier + 0x2C);
        if (!info || !flags) return false;
        const uint32_t state = *reinterpret_cast<uint32_t*>(info + 0x8C);
        return (state == 0 || state == 3 || state == 4) && !(*reinterpret_cast<uint32_t*>(flags + 0xC) & 0x20000);
    }
    if (mode != 1) return false;
    using Query = int(__thiscall*)(uintptr_t, int);
    return reinterpret_cast<Query>((*reinterpret_cast<void***>(soldier))[0x4C / 4])(soldier, 1) != 0;
}

int PanelSoldiers(uintptr_t (&out)[4]) {
    int n = 0;
    auto mgr = *reinterpret_cast<const uint8_t* const*>(kCharacters);
    if (!mgr) return 0;
    const uint8_t team = *reinterpret_cast<const uint8_t*>(kSquadTeam);
    if (team >= mgr[0x10] || 1 >= mgr[0x11]) return 0;
    auto lists = *reinterpret_cast<const uintptr_t* const*>(mgr + 4);
    if (!lists) return 0;
    for (uintptr_t s = lists[team * mgr[0x11] + 1]; s && n < 4; s = *reinterpret_cast<uintptr_t*>(s + 0x14))
        if (InSquad(s)) out[n++] = s;
    return n;
}

// Split screen: every player's own soldier gets a panel from the start, squad member or not (captive Foley in
// Mission 1). The set-up's walk ends at 0x48D6D5 (mov byte ptr [esp+0x10], 0) with up to 4 soldiers in [esp+0x1C..]
// and their count in bx; the players' soldiers missing there are appended (sorted by squad slot afterwards).
constexpr uint32_t kPanelWalkEnd = 0x48D6D5;
int __stdcall AddPlayerSoldiers(uintptr_t* soldiers, int count) {
    if (g_builtPlayers < 2) return count;
    for (int i = 0; i < g_builtPlayers && i < static_cast<int>(kMaxPlayers) && count < 4; ++i) {
        const uintptr_t s = *reinterpret_cast<uintptr_t*>(kInputBlocks + i * kInputBlockSize + 0x310);
        if (!s || !*reinterpret_cast<uintptr_t*>(s + 0x24)) continue;
        bool known = false;
        for (int j = 0; j < count; ++j) known = known || soldiers[j] == s;
        if (!known) soldiers[count++] = s;
    }
    return count;
}
__declspec(naked) void PanelWalkEndStub() {
    __asm {
        movsx eax, bx
        lea ecx, [esp + 0x20]  // caller's [esp+0x1c]
        push eax
        push ecx
        call AddPlayerSoldiers
        mov ebx, eax
        mov byte ptr [esp + 0x14], 0  // the replaced instruction (caller's [esp+0x10])
        ret
    }
}

void RebuildPanelsOnSquadChange() {
    if (g_builtPlayers < 2 || *reinterpret_cast<uint32_t*>(kFrontEndState) != 0xE) return;
    uintptr_t now[4] = {};
    const int n = PanelSoldiers(now);
    bool joined = false;
    for (int i = 0; i < n; ++i) {
        bool known = false;
        for (uintptr_t old : g_panelSoldiers) known = known || old == now[i];
        joined = joined || !known;
    }
    memcpy(g_panelSoldiers, now, sizeof now);
    void* screen = *reinterpret_cast<void**>(kHudScreen);
    if (joined && screen) {
        reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(screen))[3])(screen);
        dslog::Write("Split screen: squad changed (%d soldiers) - soldier panels rebuilt", n);
    }
}

// ---- HUD pass ----
struct D3DViewport {
    uint32_t x, y, w, h;
    float minZ, maxZ;
};
struct HudPass {
    bool active = false;
    float ox = 0, oy = 0;
    uintptr_t renderer = 0;
    D3DViewport* view = nullptr;
    uint32_t savedW = 0, savedH = 0, savedX = 0, savedY = 0;
    float scale = 1.0f;  // HUD size in this view / full screen (k(view) / k(full))
    float savedScale[std::size(kScaledSheets)][2] = {};
} g_hud;

// The HUD cave's scale for a screen of w x h (without the user's HUD size, which cancels out).
float CaveK(uint32_t w, uint32_t h) {
    float k = std::min(w / 800.0f, h / 600.0f);
    return std::max(1.0f, k);
}

// HUD anchors (table 0x619A30, float[variant][9][2], fractions of the view; filled by the game with constants):
// - the weapon display (anchor 1: icon + ammo + rank, drawn from the anchor to 113 art px right of it) mirrors the
//   soldier panel (anchor 0, whose frame starts 4.5 art px left of it): the art scales with the screen height, the
//   anchors with the width, so on a wide screen the weapon display sat far from the right edge.
// - split variants 1-3 (the Xbox layouts put the compass top left): the panel lines up under the compass (anchor
//   5 = its centre, radius 55 art px), on variant 0's bottom line - weapon display at its height, the panel's top
//   above it by variant 0's gap, converted to the view's size and HUD scale.
// Recomputed every frame from the game's values (saved once), so resolution / HUD size changes follow.
constexpr int kAnchorCompass = 5;
constexpr float kPanelFrame = 4.5f, kWeaponWidth = 113.3f, kCompassRadius = 55.0f;  // art px at 800x600

void PlaceHud(bool split) {
    static float orig[4][9][2];
    static bool haveOrig = false;
    auto anchors = reinterpret_cast<float(*)[9][2]>(kHudAnchors);
    auto renderer = *reinterpret_cast<uintptr_t*>(kRenderer);
    if (!haveOrig) {
        if (anchors[0][kAnchorWeapon][0] == 0 && anchors[0][kAnchorWeapon][1] == 0) return;  // not filled yet
        memcpy(orig, anchors, sizeof orig);
        haveOrig = true;
    }
    if (!renderer) return;
    const uint32_t W = *reinterpret_cast<uint32_t*>(renderer + kScreenW), H = *reinterpret_cast<uint32_t*>(renderer + kScreenH);
    if (W == 0 || H == 0 || g_hud.active) return;  // inside a HUD pass W/H are a view's
    const bool scaling = settings::Get().hudScaling;
    const float kFull = scaling ? CaveK(W, H) * *reinterpret_cast<const float*>(kHudScaleVa) : 1.0f;
    // anchor x of the weapon display for a panel frame starting `panelLeft` px into a view `vw` wide at scale k
    auto mirror = [](float panelLeft, float vw, float k) { return (vw - panelLeft - kWeaponWidth * k) / vw; };

    const float* panel0 = orig[0][kAnchorPanel];
    anchors[0][kAnchorWeapon][0] = mirror(panel0[0] * W - kPanelFrame * kFull, static_cast<float>(W), kFull);

    if (!split) {
        memcpy(anchors[1], orig[1], sizeof orig[1] * 3);
        return;
    }
    const float* weapon0 = orig[0][kAnchorWeapon];
    const float gap = weapon0[1] - panel0[1];  // fraction of the full screen, at full-screen HUD scale
    const uint32_t size[4][2] = {{W, H}, {W, H / 2}, {W / 2, H}, {W / 2, H / 2}};  // view size per variant
    for (int v = 1; v <= 3; ++v) {
        const float vw = static_cast<float>(size[v][0]), vh = static_cast<float>(size[v][1]);
        const float r = scaling ? CaveK(size[v][0], size[v][1]) / CaveK(W, H) : 1.0f;
        const float k = kFull * r;
        const float panelLeft = orig[v][kAnchorCompass][0] * vw - kCompassRadius * k;
        anchors[v][kAnchorPanel][0] = (panelLeft + kPanelFrame * k) / vw;
        anchors[v][kAnchorPanel][1] = weapon0[1] - gap * r * static_cast<float>(H) / vh;
        anchors[v][kAnchorWeapon][0] = mirror(panelLeft, vw, k);
        anchors[v][kAnchorWeapon][1] = weapon0[1];
    }
}

// Virtual screen for `view` (default: the viewport set last, i.e. the view being rendered).
bool BeginHud(D3DViewport* view = nullptr) {
    auto renderer = *reinterpret_cast<uintptr_t*>(kRenderer);
    if (!renderer || g_builtPlayers < 2 || g_hud.active) return false;
    if (!view) view = *reinterpret_cast<D3DViewport**>(renderer + kCurrentViewport);
    if (!view || reinterpret_cast<uintptr_t>(view) == renderer + kFullViewport) return false;  // cutscene
    auto& W = *reinterpret_cast<uint32_t*>(renderer + kScreenW);
    auto& H = *reinterpret_cast<uint32_t*>(renderer + kScreenH);
    g_hud = HudPass{true, static_cast<float>(view->x), static_cast<float>(view->y), renderer, view, W, H,
                    view->x, view->y};
    if (settings::Get().hudScaling) {
        const float r = CaveK(view->w, view->h) / CaveK(W, H);
        g_hud.scale = r;
        for (size_t i = 0; i < std::size(kScaledSheets); ++i) {
            auto sheet = *reinterpret_cast<uintptr_t*>(kScaledSheets[i]);
            if (!sheet) continue;
            auto scale = reinterpret_cast<float*>(sheet + 0x24);
            g_hud.savedScale[i][0] = scale[0], g_hud.savedScale[i][1] = scale[1];
            const float f = kScaledSheets[i] == 0x60EE28 ? std::sqrt(std::sqrt(r)) : r;  // crosshair / zoom: less reduced
            scale[0] *= f, scale[1] *= f;
        }
    }
    W = view->w, H = view->h;
    view->x = 0, view->y = 0;
    return true;
}

void EndHud() {
    if (!g_hud.active) return;
    *reinterpret_cast<uint32_t*>(g_hud.renderer + kScreenW) = g_hud.savedW;
    *reinterpret_cast<uint32_t*>(g_hud.renderer + kScreenH) = g_hud.savedH;
    g_hud.view->x = g_hud.savedX, g_hud.view->y = g_hud.savedY;
    if (settings::Get().hudScaling) {
        for (size_t i = 0; i < std::size(kScaledSheets); ++i) {
            auto sheet = *reinterpret_cast<uintptr_t*>(kScaledSheets[i]);
            if (!sheet) continue;
            auto scale = reinterpret_cast<float*>(sheet + 0x24);
            scale[0] = g_hud.savedScale[i][0], scale[1] = g_hud.savedScale[i][1];
        }
    }
    g_hud.active = false;
}

void __fastcall Panels(uintptr_t inputManager) {
    if (features::CustomiseOpen()) return;  // customise screen: no HUD
    const uint32_t player = *reinterpret_cast<uint32_t*>(kCurrentPlayer);
    if (player < kMaxPlayers && BeginHud()) {
        reinterpret_cast<void(__thiscall*)(uintptr_t)>(kPanel)(inputManager + player * kInputBlockSize);
        EndHud();
        return;
    }
    reinterpret_cast<void(__thiscall*)(uintptr_t)>(kPanels)(inputManager);
}

// Inventory / orders / give-take menus: opened (and laid out: list area, rows, box size from the HUD sheet and the
// view's anchors) by FUN_004515f0 / FUN_00451800 / FUN_00451a30 (thiscall block, open; flags block +0x40c/+0x410/+0x414,
// they close each other) and drawn by FUN_004544e0 (loop at 0x48D974, after the view loop) - at full-screen HUD size,
// so in split screen a few huge boxes filled the view. The three open functions (entry jump, 7-byte trampoline) and
// the draw run in the owning player's HUD pass (block +0x474 = view): laid out and scaled like that view's HUD.
constexpr uint32_t kInventoryDraw = 0x4544E0, kInventoryDrawSite = 0x48D974;
constexpr uint32_t kMenuOpen[3] = {0x4515F0, 0x451800, 0x451A30};
constexpr uint8_t kMenuOpenEntry[7] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x10};  // mov eax,[esp+4]; sub esp,0x10

D3DViewport* ViewOfBlock(uintptr_t block) {
    auto renderer = *reinterpret_cast<uintptr_t*>(kRenderer);
    if (!renderer) return nullptr;
    const uint32_t index = *reinterpret_cast<uint32_t*>(block + kBlockViewport);
    if (index >= *reinterpret_cast<uint32_t*>(renderer + kViewportCount)) return nullptr;
    return reinterpret_cast<D3DViewport*>(renderer + kViewports + index * kViewportStride);
}

void __fastcall InventoryDraw(uintptr_t block) {
    if (features::CustomiseOpen()) return;
    D3DViewport* view = ViewOfBlock(block);
    const bool pass = view && BeginHud(view);
    reinterpret_cast<void(__fastcall*)(uintptr_t)>(kInventoryDraw)(block);
    if (pass) EndHud();
}

// The originals minus their first 7 bytes (run here), called like the originals: thiscall (block, open), ret 4.
__declspec(naked) void OpenInventoryOriginal() { __asm { mov eax, [esp + 4] __asm sub esp, 0x10 __asm push 0x4515F7 __asm ret } }
__declspec(naked) void OpenOrdersOriginal() { __asm { mov eax, [esp + 4] __asm sub esp, 0x10 __asm push 0x451807 __asm ret } }
__declspec(naked) void OpenGiveOriginal() { __asm { mov eax, [esp + 4] __asm sub esp, 0x10 __asm push 0x451A37 __asm ret } }

// The inventory list (block +0x3fc; list +0x38 row pitch, +0x3c top, +0x40 area height, +0x42 visible rows, int16)
// shows 4 rows at the scaled HUD size: up to 3 more while it stays in the upper three quarters of the (view) screen.
void MoreInventoryRows(uintptr_t block) {
    auto* list = *reinterpret_cast<uint8_t**>(block + 0x3FC);
    auto renderer = *reinterpret_cast<uintptr_t*>(kRenderer);
    if (!list || !renderer || !settings::Get().hudScaling) return;
    auto& pitch = *reinterpret_cast<int16_t*>(list + 0x38);
    auto& top = *reinterpret_cast<int16_t*>(list + 0x3C);
    auto& area = *reinterpret_cast<int16_t*>(list + 0x40);
    auto& rows = *reinterpret_cast<int16_t*>(list + 0x42);
    const int limit = static_cast<int>(*reinterpret_cast<uint32_t*>(renderer + kScreenH) * 3 / 4);
    if (pitch <= 0 || rows <= 0) return;
    int extra = 0;
    while (extra < 3 && top + area + (extra + 1) * pitch <= limit) ++extra;
    area = static_cast<int16_t>(area + extra * pitch);
    rows = static_cast<int16_t>(rows + extra);
}

template <void (*Original)()>
void __fastcall OpenMenuInView(uintptr_t block, void*, int open) {
    D3DViewport* view = ViewOfBlock(block);
    const bool pass = view && BeginHud(view);
    reinterpret_cast<void(__thiscall*)(uintptr_t, int)>(Original)(block, open);
    if (open && Original == &OpenInventoryOriginal) MoreInventoryRows(block);
    if (pass) EndHud();
}

bool HookMenuOpen() {
    for (uint32_t va : kMenuOpen)
        if (!patch::Matches(va, kMenuOpenEntry, sizeof kMenuOpenEntry)) return false;
    const void* hooks[3] = {reinterpret_cast<const void*>(&OpenMenuInView<&OpenInventoryOriginal>),
                            reinterpret_cast<const void*>(&OpenMenuInView<&OpenOrdersOriginal>),
                            reinterpret_cast<const void*>(&OpenMenuInView<&OpenGiveOriginal>)};
    bool ok = true;
    for (int i = 0; i < 3; ++i) ok = patch::WriteJump(kMenuOpen[i], hooks[i]) && ok;
    return ok;
}

void __fastcall FlushView(uintptr_t viewController) {
    const bool pass = BeginHud();
    reinterpret_cast<void(__thiscall*)(uintptr_t)>(kFlushView)(viewController);
    if (pass) EndHud();
}

#ifndef DS_DIST
void ProbePanel(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uintptr_t item);
#endif
int __cdecl PanelCallback(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uintptr_t item) {
    if (features::CustomiseOpen()) return 0;
#ifndef DS_DIST
    ProbePanel(a, b, c, d, item);
#endif
    using Callback = int(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uintptr_t);
    D3DViewport* view = nullptr;
    auto renderer = *reinterpret_cast<uintptr_t*>(kRenderer);
    if (item && renderer) {
        auto block = *reinterpret_cast<uintptr_t*>(item + kItemBlock);
        const uint32_t count = *reinterpret_cast<uint32_t*>(renderer + kViewportCount);
        if (block) {
            const uint32_t index = *reinterpret_cast<uint32_t*>(block + kBlockViewport);
            if (index < count)
                view = reinterpret_cast<D3DViewport*>(renderer + kViewports + index * kViewportStride);
        }
    }
    const bool pass = view && BeginHud(view);
    // The position is also the panel stack's cursor (each drawn panel moves it up one row), so the origin is
    // subtracted and added back rather than the old value restored.
    int16_t* pos = item ? reinterpret_cast<int16_t*>(item + kItemPos) : nullptr;
    const int dx = g_hud.active ? static_cast<int>(g_hud.savedX) : 0, dy = g_hud.active ? static_cast<int>(g_hud.savedY) : 0;
    if (pos) pos[0] = static_cast<int16_t>(pos[0] - dx), pos[1] = static_cast<int16_t>(pos[1] - dy);
    // The stack's row height (list + 0x38, int16) was laid out at full-screen HUD size.
    uintptr_t list = item ? *reinterpret_cast<uintptr_t*>(item + kItemList) : 0;
    int16_t* row = list ? reinterpret_cast<int16_t*>(list + kListRowHeight) : nullptr;
    const int16_t fullRow = row ? *row : 0;
    if (row && g_hud.active) *row = static_cast<int16_t>(fullRow * g_hud.scale + 0.5f);
    int result = reinterpret_cast<Callback>(kPanelCallback)(a, b, c, d, e, f, item);
    if (row) *row = fullRow;
    if (pos) pos[0] = static_cast<int16_t>(pos[0] + dx), pos[1] = static_cast<int16_t>(pos[1] + dy);
    if (pass) EndHud();
    return result;
}

uint32_t __cdecl Reticle(uintptr_t block) {
    if (features::CustomiseOpen()) return 0;
    const bool pass = BeginHud();
#ifndef DS_DIST
    {  // play-test: the HUD sheets' scale when the crosshair is drawn, per player, when it changes
        static float last[4][6];
        const int p = static_cast<int>((block - kInputBlocks) / kInputBlockSize) & 3;
        static const uint32_t sheets[6] = {0x60EE18, 0x60EE1C, 0x60EE20, 0x60EE24, 0x60EE28, 0x60EDB0};
        float now[6];
        bool changed = false;
        for (int i = 0; i < 6; ++i) {
            auto sheet = *reinterpret_cast<uintptr_t*>(sheets[i]);
            now[i] = sheet ? *reinterpret_cast<float*>(sheet + 0x24) : 0.0f;
            changed = changed || now[i] != last[p][i];
            last[p][i] = now[i];
        }
        static int logged = 0;
        if (changed && logged < 300) {
            ++logged;
            dslog::Write("[dev]  crosshair P%d pass %d scales %.2f %.2f %.2f %.2f effects %.2f font %.2f", p + 1, pass,
                         now[0], now[1], now[2], now[3], now[4], now[5]);
        }
    }
#endif
    uint32_t target = reinterpret_cast<uint32_t(__cdecl*)(uintptr_t)>(kReticle)(block);
    if (pass) EndHud();
    return target;
}

void __cdecl HudDraw(int player) {
    if (features::CustomiseOpen()) return;
    BeginHud();
    reinterpret_cast<void(__cdecl*)(int)>(kHudDraw)(player);
    EndHud();
}

void __fastcall HudDraw2(void* self, void* /*edx*/, int player, uint32_t x) {
    if (features::CustomiseOpen()) return;
    BeginHud();
    reinterpret_cast<void(__thiscall*)(void*, int, uint32_t)>(kHudDraw2)(self, player, x);
    EndHud();
}

float g_shiftX = 0, g_shiftY = 0;  // extra move for one HUD element outside the HUD passes (match clock)

// Screen-space quads drawn during a HUD pass move to the view's origin (on a copy: callers may reuse theirs).
void __fastcall DrawFan(uintptr_t renderer, void* /*edx*/, uint32_t prims, const uint8_t* verts, uint32_t stride) {
    using Draw = void(__thiscall*)(uintptr_t, uint32_t, const void*, uint32_t);
    const uint32_t count = prims + 2;
    const bool shift = g_shiftX != 0 || g_shiftY != 0;
    if ((g_hud.active || shift) && *reinterpret_cast<uint32_t*>(renderer + kCurrentFvf) == kFvfScreenQuad &&
        stride >= 8 && count * stride <= 64 * 1024) {
        static uint8_t copy[64 * 1024];
        memcpy(copy, verts, count * stride);
        const float dx = (g_hud.active ? g_hud.ox : 0) + g_shiftX, dy = (g_hud.active ? g_hud.oy : 0) + g_shiftY;
        for (uint32_t i = 0; i < count; ++i) {
            auto v = reinterpret_cast<float*>(copy + i * stride);
            v[0] += dx, v[1] += dy;
        }
        reinterpret_cast<Draw>(kDrawFan)(renderer, prims, copy, stride);
        return;
    }
    reinterpret_cast<Draw>(kDrawFan)(renderer, prims, verts, stride);
}

// MP match clock (FUN_0047e750, from the HUD render-list item at 0x48D8A7): an LCD frame (HUD art image 0x16) with
// the session time, drawn once at 40 art px from the top-left corner - on player 1's compass in split screen. Moved
// to the centre of the screen, where the dividers meet: free in every layout (compasses top left, panels bottom left,
// weapons bottom right of each view; top centre was player 2's compass in quarters). Every quad it draws is shifted.
constexpr uint32_t kMatchClockCall = 0x48D8A7, kMatchClock = 0x47E750, kHudArt = 0x60EE18;
constexpr uint32_t kImageWidth = 0x53C6D0, kImageHeight = 0x53C710;
constexpr int kClockFrame = 0x16;
void MatchClock() {
    auto renderer = *reinterpret_cast<uintptr_t*>(kRenderer);
    auto sheet = *reinterpret_cast<uint8_t**>(kHudArt);
    if (g_builtPlayers > 1 && renderer && sheet) {
        const float w = reinterpret_cast<float(__thiscall*)(void*, int)>(kImageWidth)(sheet, kClockFrame);
        const float h = reinterpret_cast<float(__thiscall*)(void*, int)>(kImageHeight)(sheet, kClockFrame);
        const float W = static_cast<float>(*reinterpret_cast<uint32_t*>(renderer + kScreenW));
        const float H = static_cast<float>(*reinterpret_cast<uint32_t*>(renderer + kScreenH));
        g_shiftX = std::floor((W - w) * 0.5f - 40.0f * *reinterpret_cast<float*>(sheet + 0x24));
        g_shiftY = std::floor((H - h) * 0.5f - 40.0f * *reinterpret_cast<float*>(sheet + 0x28));
    }
    reinterpret_cast<void(__cdecl*)()>(kMatchClock)();
    g_shiftX = g_shiftY = 0;
}

// Divider lines between the views, like the Xbox's Split_DrawDivider (0xe8610: 2 px of 50% grey on a 480-line
// screen, centred on the edge) - drawn after the view loop, when the loop sets the full-screen viewport again
// (0x40F1DC), so the full-screen 2D after it (pause menu, messages) still covers them. Not during cutscenes
// (FUN_0040e080 = 0: the loop drew one full-screen view).
void __fastcall SetFullViewport(uintptr_t renderer, void* /*edx*/, const D3DViewport* full) {
    reinterpret_cast<void(__thiscall*)(uintptr_t, const D3DViewport*)>(kSetViewport)(renderer, full);
    const uint32_t count = *reinterpret_cast<uint32_t*>(renderer + kViewportCount);
    if (g_builtPlayers < 2 || count < 2 || count > 4 || !reinterpret_cast<int(__cdecl*)()>(kSplitActive)()) return;
    const uint32_t sw = *reinterpret_cast<uint32_t*>(renderer + kScreenW), sh = *reinterpret_cast<uint32_t*>(renderer + kScreenH);
    const float t = std::max(2.0f, std::round(sh / 240.0f));  // 2 px at 480 lines
    float lines[8][4];
    int n = 0;
    auto add = [&](float x, float y, float w, float h) {
        lines[n][0] = x, lines[n][1] = y, lines[n][2] = w, lines[n][3] = h;
        ++n;
    };
    for (uint32_t i = 0; i < count; ++i) {  // each view's right and bottom edge, unless it is the screen's
        const auto* v = reinterpret_cast<const D3DViewport*>(renderer + kViewports + i * kViewportStride);
        if (v->x + v->w + 1 < sw) add(float(v->x + v->w) - t / 2, float(v->y), t, float(v->h));
        if (v->y + v->h + 1 < sh) add(float(v->x), float(v->y + v->h) - t / 2, float(v->w), t);
    }
    overlay::FillRects(lines, n, 0xFF808080);
}

#ifndef DS_DIST
// Dev probe (Dev\HudProbe = 1): logs a batch of image draws (FUN_0053c9e0: caller, sheet, image, position,
// sheet scale, HUD pass state) ~8 s after split views were built - to find which code draws which HUD part.
constexpr uint32_t kDrawImage = 0x53C9E0;
int g_probeLeft = 0;
DWORD g_probeDelay = 8000;  // Dev\HudProbe > 1: delay in seconds

void __stdcall ProbeImage(uint32_t caller, uintptr_t sheet, int image, int x, int y) {
    if (!g_probeStart || GetTickCount() - g_probeStart < g_probeDelay || g_probeLeft <= 0) return;
    const bool panel = caller >= 0x48D9E0 && caller < 0x48F1A8;  // soldier panel / weapon display code
    if (g_hud.active && !panel) return;
    // Call chain: return addresses on the stack (a word pointing into .text right after an E8 call).
    uint32_t chain[4] = {caller}, n = 1;
    auto sp = reinterpret_cast<const uint32_t*>(&caller) + 6;
    for (int i = 0; i < 160 && n < 4; ++i) {
        uint32_t v = sp[i];
        if (v > 0x401005 && v < 0x5D7000 && *reinterpret_cast<const uint8_t*>(v - 5) == 0xE8) chain[n++] = v;
    }
    static uint32_t seen[256][2];
    static int seenCount = 0;
    const uint32_t key = chain[0] ^ (static_cast<uint32_t>(g_hud.ox) << 20) ^ (static_cast<uint32_t>(g_hud.oy) << 8) ^
                         (static_cast<uint32_t>(y) << 12) ^ static_cast<uint32_t>(x),
                   sheetKey = static_cast<uint32_t>(sheet);  // one line per caller and view
    for (int i = 0; i < seenCount; ++i)
        if (seen[i][0] == key && seen[i][1] == sheetKey) return;
    if (seenCount < 256) seen[seenCount][0] = key, seen[seenCount][1] = sheetKey, ++seenCount;
    --g_probeLeft;
    auto scale = reinterpret_cast<float*>(sheet + 0x24);
    dslog::Write("probe: %08X < %08X < %08X < %08X sheet %08X img %3d at %5d,%5d scale %.2f hud %d origin %.0f,%.0f"
                 " player %u", chain[0], chain[1], chain[2], chain[3], sheetKey, image, x, y, scale[0], g_hud.active,
                 g_hud.ox, g_hud.oy, *reinterpret_cast<uint32_t*>(kCurrentPlayer));
}

void ProbePanel(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uintptr_t item) {
    static int left = 60;
    if (!g_probeStart || GetTickCount() - g_probeStart < g_probeDelay || left <= 0) return;
    --left;
    const uintptr_t block = item ? *reinterpret_cast<uintptr_t*>(item + kItemBlock) : 0;
    const int player = block ? static_cast<int>((block - kInputBlocks) / kInputBlockSize) : -1;
    dslog::Write("panel: item %08X block P%d a %08X b %08X c %08X d %08X pos %d,%d hud %d origin %.0f,%.0f",
                 static_cast<uint32_t>(item), player + 1, a, b, c, d,
                 item ? reinterpret_cast<int16_t*>(item + kItemPos)[0] : 0,
                 item ? reinterpret_cast<int16_t*>(item + kItemPos)[1] : 0, g_hud.active, g_hud.ox, g_hud.oy);
}

__declspec(naked) void DrawImageProbe() {
    __asm {
        pushad
        push dword ptr [esp + 44]  // y      (args start at esp+36 after pushad + return address)
        push dword ptr [esp + 44]  // x
        push dword ptr [esp + 44]  // image
        push ecx                   // sheet
        push dword ptr [esp + 48]  // caller
        call ProbeImage
        popad
        sub esp, 0x94              // the overwritten first instruction
        push 0x53C9E6
        ret
    }
}

void InstallProbe() {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "HudProbe", RRF_RT_REG_DWORD, nullptr, &v,
                     &size) != ERROR_SUCCESS || !v)
        return;
    static const uint8_t head[] = {0x81, 0xEC, 0x94, 0x00, 0x00, 0x00};
    if (patch::Matches(kDrawImage, head, sizeof head)) {
        patch::WriteJump(kDrawImage, reinterpret_cast<const void*>(&DrawImageProbe));
        static const uint8_t nop = 0x90;
        patch::Write(kDrawImage + 5, &nop, 1);
    } else {
        overlay::SetImageProbe(reinterpret_cast<void*>(&ProbeImage));  // the overlay owns the entry
    }
    g_probeLeft = 120;
    if (v > 1) g_probeDelay = v * 1000;
    dslog::Write("[dev]  HUD probe on");
}
#endif

// Sets 1-3 = set 0 (the keys the player configured).
void CopyBindings() {
    auto table = *reinterpret_cast<uint32_t**>(kBindingTable);
    if (!table || table[0] < kMaxPlayers || !table[3]) return;
    const uint32_t setSize = table[1] * table[2];
    auto data = reinterpret_cast<uint32_t*>(table[3]);
    for (uint32_t s = 1; s < kMaxPlayers; ++s) memcpy(data + s * setSize, data, setSize * sizeof(uint32_t));
}

// The front-end and the outro are single-view levels (the game also clears its multiplayer flag for them).
bool IsMenuLevel(const char* name) {
    return !name || _strnicmp(name, "frontend", 8) == 0 || _strnicmp(name, "outro", 5) == 0;
}
}  // namespace

int features::SplitScreenPlayers() { return g_builtPlayers; }

// Field of view in narrow views. The projection FUN_0054fb80 (thiscall camera, vertical FOV degrees, near, far) is
// Hor+: 70 deg vertical in any view, so a side-by-side half of a 16:9 screen (8:9) saw about 64 deg across against
// 106 deg full screen. A split view narrower than 4:3 now keeps 4:3's horizontal angle and gets a taller vertical one
// instead (the FOV itself goes up, so the culling angles stored from it widen too; scope zoom keeps its ratio).
namespace {
constexpr uint32_t kProjection = 0x54FB80;
constexpr uint32_t kProjectionCalls[] = {0x40F1F6, 0x4105CD, 0x4D6715, 0x4ECD02, 0x4EF98D, 0x54F941};
int __fastcall Projection(void* camera, void*, float fov, float zNear, float zFar) {
    if (g_builtPlayers > 1 && fov > 1.0f && fov < 170.0f) {
        const uint8_t* r = *reinterpret_cast<const uint8_t* const*>(0x63C924);  // renderer: the view being drawn
        const uint8_t* vp = r ? *reinterpret_cast<const uint8_t* const*>(r + kCurrentViewport) : nullptr;
        const uint32_t w = vp ? *reinterpret_cast<const uint32_t*>(vp + 8) : 0;
        const uint32_t h = vp ? *reinterpret_cast<const uint32_t*>(vp + 0xC) : 0;
        if (w && h && w * 3 < h * 4) {
            const float half = fov * 0.5f * 0.017453292f;
            fov = 2.0f * std::atan(std::tan(half) * (4.0f / 3.0f) * static_cast<float>(h) / static_cast<float>(w)) /
                  0.017453292f;
            fov = std::min(fov, 120.0f);
        }
    }
    return reinterpret_cast<int(__thiscall*)(void*, float, float, float)>(kProjection)(camera, fov, zNear, zFar);
}
}  // namespace

// Offset of the view whose HUD is being drawn (its 2D is drawn in view-local coordinates and moved here).
bool features::SplitHudOffset(float& x, float& y) {
    if (!g_hud.active) return false;
    x = g_hud.ox;
    y = g_hud.oy;
    return true;
}

void features::ApplySplitScreen() {
    static bool hooked = false;
    if (hooked) return;
    static const uint8_t oneSet[] = {0x6A, 0x01}, fourSets[] = {0x6A, static_cast<uint8_t>(kMaxPlayers)};
    if (!patch::Matches(kBindingSetsPush, oneSet, sizeof oneSet)) {
        dslog::Write("[fail] Split screen: unexpected code at 0x%08X", kBindingSetsPush);
        return;
    }
    hooked = patch::HookCall(kPickSoldierSite, reinterpret_cast<const void*>(&AssignPlayers), kPickSoldier);
    for (uint32_t site : kProjectionCalls)
        if (!patch::HookCall(site, reinterpret_cast<const void*>(&Projection), kProjection))
            dslog::Write("[fail] Split screen: projection call at 0x%08X not recognised", site);
    for (uint32_t site : kMouseDeltaSites)
        if (!patch::HookCall(site, reinterpret_cast<const void*>(&MouseDeltaStub), kMouseDelta))
            dslog::Write("[fail] Split screen: mouse read at 0x%08X not recognised - the mouse turns every player", site);
    {
        uint8_t jump[5] = {0xE9};
        const int32_t rel = static_cast<int32_t>(kSlowDown - (kSlowDownJump + 5));
        memcpy(jump + 1, &rel, 4);
        if (patch::Matches(kSlowDownJump, jump, sizeof jump))
            patch::WriteJump(kSlowDownJump, reinterpret_cast<const void*>(&SlowDownStub));
        else
            dslog::Write("[fail] Split screen: downed-soldier slow motion not recognised at 0x%08X", kSlowDownJump);
    }
    {
        static const uint8_t walkEnd[] = {0xC6, 0x44, 0x24, 0x10, 0x00};
        uint8_t call[5] = {0xE8};
        const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&PanelWalkEndStub) - (kPanelWalkEnd + 5));
        memcpy(call + 1, &rel, 4);
        if (patch::Matches(kPanelWalkEnd, walkEnd, sizeof walkEnd)) patch::Write(kPanelWalkEnd, call, sizeof call);
        else if (!patch::Matches(kPanelWalkEnd, call, sizeof call))
            dslog::Write("[fail] Split screen: soldier panel set-up not recognised at 0x%08X", kPanelWalkEnd);
    }
    static const uint8_t axisEntry[] = {0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1};  // push ecx/ebx/ebp/esi; mov esi, ecx
    if (patch::Matches(kAxisValue, axisEntry, sizeof axisEntry))
        patch::WriteJump(kAxisValue, reinterpret_cast<const void*>(&AxisValue));
    else
        dslog::Write("[fail] Split screen: analog input function not recognised - keyboard moves every player");
    if (!hooked) {
        dslog::Write("[fail] Split screen: unexpected code at 0x%08X", kPickSoldierSite);
        return;
    }
    patch::Write(kBindingSetsPush, fourSets, sizeof fourSets);
#ifndef DS_DIST
    InstallProbe();
#endif
    bool hud = patch::HookCall(kPanelsSite, reinterpret_cast<const void*>(&Panels), kPanels) &&
               patch::HookCall(kFlushViewSite, reinterpret_cast<const void*>(&FlushView), kFlushView) &&
               patch::HookCall(kReticleSite, reinterpret_cast<const void*>(&Reticle), kReticle) &&
               patch::HookCall(kHudDrawSite, reinterpret_cast<const void*>(&HudDraw), kHudDraw) &&
               patch::HookCall(kHudDraw2Site, reinterpret_cast<const void*>(&HudDraw2), kHudDraw2) &&
               patch::HookCall(kInventoryDrawSite, reinterpret_cast<const void*>(&InventoryDraw), kInventoryDraw) &&
               HookMenuOpen();
    const bool dividers = patch::HookCall(kSetFullViewportSite, reinterpret_cast<const void*>(&SetFullViewport), kSetViewport);
    uint8_t push[5] = {0x68};  // push kPanelCallback
    memcpy(push + 1, &kPanelCallback, 4);
    if (patch::Matches(kPanelCallbackPush, push, sizeof push)) {
        auto fn = reinterpret_cast<uint32_t>(&PanelCallback);
        patch::Write(kPanelCallbackPush + 1, &fn, 4);
    } else {
        hud = false;
    }
    if (!patch::HookCall(kMatchClockCall, reinterpret_cast<const void*>(&MatchClock), kMatchClock))
        dslog::Write("[fail] Split screen: MP match clock call not recognised at 0x%08X", kMatchClockCall);
    for (uint32_t site : kDrawFanCallers)
        hud = patch::HookCall(site, reinterpret_cast<const void*>(&DrawFan), kDrawFan) && hud;
    dslog::Write("[ok]   Split screen: player set-up for 2-4 players, 4 binding sets%s%s",
                 hud ? ", HUD per view" : " (HUD per view: unexpected code, skipped)",
                 dividers ? ", divider lines" : " (divider lines: unexpected code, skipped)");
}

void features::OnFrameSplitScreen() {
    auto renderer = *reinterpret_cast<uintptr_t*>(kRenderer);
    if (!renderer) return;
    const uint32_t count = *reinterpret_cast<uint32_t*>(renderer + kViewportCount);
    if (count == 0) return;  // device not set up yet
    const uint32_t layout = settings::Get().splitScreenLayout;

    if (*reinterpret_cast<uint8_t*>(kLoadPending) == 1) {
        const char* next = *reinterpret_cast<const char**>(kNextLevel);
        const int players = IsMenuLevel(next) ? 1 : WantedPlayers();
        if (players > 1) {
            CopyBindings();
            *reinterpret_cast<uint32_t*>(kMultiplayer) = 1;
            *reinterpret_cast<uint32_t*>(kPlayerCount) = static_cast<uint32_t>(players);
        } else if (g_builtPlayers > 1) {
            *reinterpret_cast<uint32_t*>(kMultiplayer) = 0;  // back to single player (e.g. next mission)
        }
        padlayout::ApplyToGame();          // each player's controller layout into their binding set
        features::ApplyCoopKeyProfiles();  // co-op keyboard player's chosen key profile
        if (players != g_builtPlayers || count != static_cast<uint32_t>(players)) Build(renderer, players, layout);
        memset(g_panelSoldiers, 0, sizeof g_panelSoldiers);
        return;
    }
    PlaceHud(g_builtPlayers > 1);
    RebuildPanelsOnSquadChange();
    // Mid-level: only restore what something else reset (device reset after Alt+Tab), or a new orientation.
    // (the customise screen shows one full-screen view while it is up - customise.cpp puts the views back)
    if (g_builtPlayers > 1 && !features::CustomiseOpen() &&
        (count != static_cast<uint32_t>(g_builtPlayers) || layout != g_builtLayout))
        Build(renderer, g_builtPlayers, layout);
}

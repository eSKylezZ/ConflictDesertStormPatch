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
#include <cstring>
#include <iterator>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/splitscreen_layout.h"

namespace {
constexpr uint32_t kRenderer = 0x63C924;           // renderer object pointer
constexpr uint32_t kViewportCount = 0x20;          // renderer + 0x20
constexpr uint32_t kBuildSplitViewports = 0x547B30;  // thiscall (renderer, table), ret 4
constexpr uint32_t kResetViewports = 0x542E50;       // thiscall (renderer): one full-screen viewport
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
// Globals -> 2D image sheets (scale +0x24/+0x28): GWInt HUD, HUD art, GWInt, NewLogo, CDSFont. Not Effects
// [0x60EE28] (3D texture). A sheet the game re-scales while drawing gets the view's k from the HUD cave anyway.
constexpr uint32_t kScaledSheets[] = {0x60EE18, 0x60EE1C, 0x60EE20, 0x60EE24, 0x60EDB0};

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
    dslog::Write("Split screen: %u players assigned", players);
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

// Bottom HUD line of the split variants 1-3, like the full-screen layout (variant 0): the weapon display (anchor 1)
// at its bottom-right spot, and the soldier panel (anchor 0, the panel's top) above it by variant 0's gap, converted
// to the variant's view size and HUD scale - so panel and weapon display end on the same line. The game's anchors
// come back when split screen ends.
void PlaceBottomHud(bool split) {
    static float saved[3][2][2];  // [variant 1-3][anchor 0/1][x, y]
    static bool moved = false;
    auto anchors = reinterpret_cast<float(*)[9][2]>(kHudAnchors);
    if (!split) {
        if (!moved) return;
        for (int v = 1; v <= 3; ++v)
            for (int a = 0; a < 2; ++a) anchors[v][a][0] = saved[v - 1][a][0], anchors[v][a][1] = saved[v - 1][a][1];
        moved = false;
        return;
    }
    auto renderer = *reinterpret_cast<uintptr_t*>(kRenderer);
    const float* weapon = anchors[0][kAnchorWeapon];
    const float* panel = anchors[0][kAnchorPanel];
    if (!renderer || (weapon[0] == 0 && weapon[1] == 0)) return;  // table not filled yet
    if (!moved) {
        for (int v = 1; v <= 3; ++v)
            for (int a = 0; a < 2; ++a) saved[v - 1][a][0] = anchors[v][a][0], saved[v - 1][a][1] = anchors[v][a][1];
        moved = true;
    }
    const uint32_t W = *reinterpret_cast<uint32_t*>(renderer + kScreenW), H = *reinterpret_cast<uint32_t*>(renderer + kScreenH);
    const float gap = weapon[1] - panel[1];  // fraction of the full screen, at full-screen HUD scale
    const uint32_t size[4][2] = {{W, H}, {W, H / 2}, {W / 2, H}, {W / 2, H / 2}};  // view size per variant
    for (int v = 1; v <= 3; ++v) {
        const uint32_t vw = size[v][0], vh = size[v][1];
        const float scale = settings::Get().hudScaling ? CaveK(vw, vh) / CaveK(W, H) : 1.0f;
        anchors[v][kAnchorWeapon][0] = weapon[0];
        anchors[v][kAnchorWeapon][1] = weapon[1];
        anchors[v][kAnchorPanel][0] = saved[v - 1][kAnchorPanel][0];  // keep the split layout's left margin
        anchors[v][kAnchorPanel][1] = weapon[1] - gap * scale * static_cast<float>(H) / static_cast<float>(vh);
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
            scale[0] *= r, scale[1] *= r;
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
    const uint32_t player = *reinterpret_cast<uint32_t*>(kCurrentPlayer);
    if (player < kMaxPlayers && BeginHud()) {
        reinterpret_cast<void(__thiscall*)(uintptr_t)>(kPanel)(inputManager + player * kInputBlockSize);
        EndHud();
        return;
    }
    reinterpret_cast<void(__thiscall*)(uintptr_t)>(kPanels)(inputManager);
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
    const bool pass = BeginHud();
    uint32_t target = reinterpret_cast<uint32_t(__cdecl*)(uintptr_t)>(kReticle)(block);
    if (pass) EndHud();
    return target;
}

void __cdecl HudDraw(int player) {
    BeginHud();
    reinterpret_cast<void(__cdecl*)(int)>(kHudDraw)(player);
    EndHud();
}

void __fastcall HudDraw2(void* self, void* /*edx*/, int player, uint32_t x) {
    BeginHud();
    reinterpret_cast<void(__thiscall*)(void*, int, uint32_t)>(kHudDraw2)(self, player, x);
    EndHud();
}

// Screen-space quads drawn during a HUD pass move to the view's origin (on a copy: callers may reuse theirs).
void __fastcall DrawFan(uintptr_t renderer, void* /*edx*/, uint32_t prims, const uint8_t* verts, uint32_t stride) {
    using Draw = void(__thiscall*)(uintptr_t, uint32_t, const void*, uint32_t);
    const uint32_t count = prims + 2;
    if (g_hud.active && *reinterpret_cast<uint32_t*>(renderer + kCurrentFvf) == kFvfScreenQuad &&
        stride >= 8 && count * stride <= 64 * 1024) {
        static uint8_t copy[64 * 1024];
        memcpy(copy, verts, count * stride);
        for (uint32_t i = 0; i < count; ++i) {
            auto v = reinterpret_cast<float*>(copy + i * stride);
            v[0] += g_hud.ox, v[1] += g_hud.oy;
        }
        reinterpret_cast<Draw>(kDrawFan)(renderer, prims, copy, stride);
        return;
    }
    reinterpret_cast<Draw>(kDrawFan)(renderer, prims, verts, stride);
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
    if (!patch::Matches(kDrawImage, head, sizeof head)) return;
    patch::WriteJump(kDrawImage, reinterpret_cast<const void*>(&DrawImageProbe));
    static const uint8_t nop = 0x90;
    patch::Write(kDrawImage + 5, &nop, 1);
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
               patch::HookCall(kHudDraw2Site, reinterpret_cast<const void*>(&HudDraw2), kHudDraw2);
    uint8_t push[5] = {0x68};  // push kPanelCallback
    memcpy(push + 1, &kPanelCallback, 4);
    if (patch::Matches(kPanelCallbackPush, push, sizeof push)) {
        auto fn = reinterpret_cast<uint32_t>(&PanelCallback);
        patch::Write(kPanelCallbackPush + 1, &fn, 4);
    } else {
        hud = false;
    }
    for (uint32_t site : kDrawFanCallers)
        hud = patch::HookCall(site, reinterpret_cast<const void*>(&DrawFan), kDrawFan) && hud;
    dslog::Write("[ok]   Split screen: player set-up for 2-4 players, 4 binding sets%s",
                 hud ? ", HUD per view" : " (HUD per view: unexpected code, skipped)");
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
        if (players != g_builtPlayers || count != static_cast<uint32_t>(players)) Build(renderer, players, layout);
        return;
    }
    PlaceBottomHud(g_builtPlayers > 1);
    // Mid-level: only restore what something else reset (device reset after Alt+Tab), or a new orientation.
    if (g_builtPlayers > 1 && (count != static_cast<uint32_t>(g_builtPlayers) || layout != g_builtLayout))
        Build(renderer, g_builtPlayers, layout);
}

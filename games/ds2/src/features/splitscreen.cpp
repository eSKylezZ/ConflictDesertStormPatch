// Local split screen (2-4 players) in CDS II.exe, rebuilt from what the PC build kept of the console code - the same
// remnants as in DS1 (DesertStormFix splitscreen.cpp), at DS2's addresses (PORTING.md "Split screen"):
//
// Kept on PC:
//  - split object 0x626C18 (ctor FUN_0040a2d0): +0 local multiplayer flag, +4 player count, +8 mode (2; HUD code tests
//    3 / 4), +0xC orientation, +0x10 HUD layout table (Xbox format, 0x80 bytes; PC tables 0x6086D0 1p, 0x608750 2p
//    top/bottom, 0x6087D0 2p side by side - the ctor leaves 0x6087D0, the table selectors are stripped). Only the
//    start-up option -P keeps the flag (FUN_0040b190 clears it otherwise).
//  - level load FUN_0042CAD0 (called at 0x42CFC1 from the level dispatcher): input manager 0x62FE40 adds one input block
//    (FUN_00428A90; block i = 0x62FF7C + i*0x58C: +0x39C binding set, +0x3A0, +0x3A4 joystick = i) and, with the flag,
//    one more per player, then level init FUN_00412690.
//  - player set-up FUN_0040C8B0 (from level init): binds one camera per *renderer viewport* (renderer [0x680038] +0x30
//    count, +0x34 D3DVIEWPORT9[], camera manager [0x680120] +0x10 + i*4) and player i (0x62F36C[i]) to block i's
//    soldier (+0x2EC). Player 1's soldier pick FUN_00423620 (thiscall block; squad member whose owner
//    FUN_00475970 == block +0x39C) at 0x40C94A; the Xbox-style loop for players 2..N is stripped -> AssignPlayers.
//  - squad slot -> player FUN_00475610 (soldier +0xEC4 owner, by player count), render loop FUN_0040FBA0 per viewport
//    (current view [0x680134], GetViewport FUN_00536BB0, split flag [0x62F3A8] set in level init, full screen for
//    cutscene states 0x15/0x19/0x2A/0x30 of [0x63433C]), HUD pass FUN_0040DBC0 per viewport (FUN_0043AEB0(i)), HUD
//    anchors FUN_00459D10(out, view, anchor) from the layout table + anchor table 0x613FE0.
// Stripped: the viewport builder (DS1 FUN_00547B30) - the viewports are written here (the renderer's list is only
// built once, FUN_00537380 at device creation, so it is restored to one full-screen entry when split screen ends).
//
// Bindings: table [0x627078] = {sets, slots 3, actions 0x51, data} is allocated with ONE set (FUN_0040A3E0 called at
// 0x40A556 with `push esi` = 1); block i evaluates set i (+0x39C, lookup FUN_0040A470) -> 4 sets, sets 1-3 copied from
// set 0 at every split level load. Devices: block +0xC bit 0 keyboard/mouse, bit 1 pad (FUN_00422FA0); players 2-4 pad
// only. The analog path FUN_004230F0 (block, action, opposite) ignores that mask -> replaced (AxisValue).
#include <windows.h>

#include <d3d9.h>

#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/splitscreen_layout.h"

namespace {
constexpr uint32_t kRenderer = 0x680038;
constexpr uint32_t kViewportCount = 0x30, kViewports = 0x34, kViewportStride = 0x18;
constexpr uint32_t kFullViewport = 0x41A00;          // renderer + : full-screen D3DVIEWPORT9 (front-end: virtual)
constexpr uint32_t kBackBufferW = 0x94, kBackBufferH = 0x98;
constexpr uint32_t kSplit = 0x626C18;                // split object
constexpr uint32_t kMultiplayer = kSplit, kPlayerCount = kSplit + 4, kOrientation = kSplit + 0xC,
                   kHudLayoutTable = kSplit + 0x10;
constexpr uint32_t kHudTableDefault = 0x6087D0;      // what the ctor leaves there
constexpr uint32_t kLevelLoad = 0x42CAD0, kLevelLoadSite = 0x42CFC1;  // thiscall
constexpr uint32_t kFrontEndInit = 0x42C930;          // thiscall; entry 51 53 E8 <call 0x4C3450>
constexpr uint32_t kInputBlocks = 0x62FF7C, kInputBlockSize = 0x58C;
constexpr uint32_t kBlockDevices = 0xC, kBlockPlayer = 0x4, kBlockSet = 0x39C, kBlockJoystick = 0x3A4,
                   kBlockView = 0x448, kBlockSoldier = 0x2EC;
constexpr uint32_t kPlayerTable = 0x62F36C;
constexpr uint32_t kPickSoldier = 0x423620, kPickSoldierSite = 0x40C94A;
constexpr uint32_t kBindingTable = 0x627078, kBindingInit = 0x40A3E0, kBindingInitSite = 0x40A556;
constexpr uint32_t kMaxPlayers = 4;

struct Entry {
    uint32_t used;
    float x, y, w, h;
};
struct HudTable {
    float offset[4][2];
    Entry views[4];
    uint32_t variant[4];
};
static_assert(sizeof(HudTable) == 0x80, "Xbox layout table size");

HudTable MakeHudTable(splitscreen::Layout layout, int players) {
    HudTable t{};
    for (int i = 0; i < 4; ++i) {
        t.variant[i] = 4;
        if (i >= players) continue;
        splitscreen::View v = splitscreen::ViewRect(layout, players, i);
        t.views[i] = {1, v.x, v.y, v.w, v.h};
        // the console variants 1-3 (half / half / quarter) put the soldier panels at the view's top left, stacking
        // upward out of it; the full-screen anchors (fractions of the view) give the PC layout in every view
        t.variant[i] = 0;
    }
    return t;
}

int g_builtPlayers = 1;
int g_viewW = 0, g_viewH = 0;   // smallest view (pixels)
HudTable g_hudTable;

template <class T>
T& At(uintptr_t a) { return *reinterpret_cast<T*>(a); }

int WantedPlayers() {
    if (features::CoopPlayers() >= 2) return features::CoopPlayers();
#ifndef DS_DIST
    DWORD v = 0, size = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", "SplitPlayers", RRF_RT_REG_DWORD, nullptr, &v,
                     &size) == ERROR_SUCCESS && v >= 2 && v <= 4)
        return static_cast<int>(v);
#endif
    return 1;
}

// One full-screen viewport again (the renderer's own full-screen copy, which the front-end's virtual screen keeps).
void SingleViewport(uintptr_t r) {
    std::memcpy(reinterpret_cast<void*>(r + kViewports), reinterpret_cast<void*>(r + kFullViewport), kViewportStride);
    At<uint32_t>(r + kViewportCount) = 1;
}

void EndSplit() {
    if (g_builtPlayers <= 1) return;
    At<uint32_t>(kMultiplayer) = 0;
    At<uint32_t>(kHudLayoutTable) = kHudTableDefault;
    if (uintptr_t r = At<uintptr_t>(kRenderer)) SingleViewport(r);
    g_builtPlayers = 1;
    dslog::Write("Split screen: off (one view)");
}

void CopyBindingSets(int players) {
    auto t = At<uint32_t*>(kBindingTable);
    if (!t || t[0] < static_cast<uint32_t>(players)) return;
    const uint32_t per = t[1] * t[2];
    auto data = reinterpret_cast<uint32_t*>(t[3]);
    for (int s = 1; s < players; ++s) std::memcpy(data + s * per, data, per * 4);
}

// The renderer's viewport list (+0x30 count, +0x34 D3DVIEWPORT9[]) from the layout, in back-buffer pixels. Written
// again in the player set-up: the front-end's virtual screen (frontend.cpp Leave, at mission init) resets entry 0's
// size to the full screen after the level load started.
void WriteViewports(uintptr_t r, splitscreen::Layout layout, int players) {
    const float bw = static_cast<float>(At<uint32_t>(r + kBackBufferW)), bh = static_cast<float>(At<uint32_t>(r + kBackBufferH));
    for (int i = 0; i < players; ++i) {
        const splitscreen::View v = splitscreen::ViewRect(layout, players, i);
        auto e = reinterpret_cast<uint32_t*>(r + kViewports + i * kViewportStride);
        const auto x0 = static_cast<uint32_t>(v.x * bw + 0.5f), y0 = static_cast<uint32_t>(v.y * bh + 0.5f);
        const auto x1 = static_cast<uint32_t>((v.x + v.w) * bw + 0.5f), y1 = static_cast<uint32_t>((v.y + v.h) * bh + 0.5f);
        e[0] = x0;
        e[1] = y0;
        e[2] = x1 - x0;
        e[3] = y1 - y0;
        reinterpret_cast<float*>(e)[4] = 0.0f;
        reinterpret_cast<float*>(e)[5] = 1.0f;
    }
    At<uint32_t>(r + kViewportCount) = static_cast<uint32_t>(players);
    g_viewW = g_viewH = 1 << 30;
    for (int i = 0; i < players; ++i) {
        auto e = reinterpret_cast<const int*>(r + kViewports + i * kViewportStride);
        if (e[2] < g_viewW) g_viewW = e[2];
        if (e[3] < g_viewH) g_viewH = e[3];
    }
}

void BeginSplit(int players) {
    const uintptr_t r = At<uintptr_t>(kRenderer);
    if (!r) return;
    const auto layout = static_cast<splitscreen::Layout>(settings::Get().splitScreenLayout);
    g_hudTable = MakeHudTable(layout, players);
    At<uint32_t>(kMultiplayer) = 1;
    At<uint32_t>(kPlayerCount) = static_cast<uint32_t>(players);
    At<uint32_t>(kOrientation) = layout == splitscreen::Layout::Horizontal;
    At<HudTable*>(kHudLayoutTable) = &g_hudTable;
    WriteViewports(r, layout, players);
    CopyBindingSets(players);
    g_builtPlayers = players;
    dslog::Write("Split screen: %d views (%s), back buffer %ux%u, smallest view %dx%d", players,
                 layout == splitscreen::Layout::Horizontal ? "top / bottom" : "side by side", At<uint32_t>(r + kBackBufferW),
                 At<uint32_t>(r + kBackBufferH), g_viewW, g_viewH);
}

// Level dispatcher -> mission level load: split screen is set up before it adds the input blocks and runs level init.
void __fastcall LevelLoad(void* self, void*) {
    const int players = WantedPlayers();
    if (players >= 2) BeginSplit(players);
    else EndSplit();
    reinterpret_cast<void(__thiscall*)(void*)>(kLevelLoad)(self);
}

// Front-end init: back to one view.
void __stdcall FrontEndEntered() { EndSplit(); }
uint32_t g_frontEndCont = kFrontEndInit + 7, g_frontEndCall = 0x4C3450;
__declspec(naked) void FrontEndInitStub() {
    __asm {
        pushad
        call FrontEndEntered
        popad
        push ecx            // replayed: push ecx; push ebx; call 0x4C3450 (returns to 0x42C937)
        push ebx
        push g_frontEndCont
        jmp dword ptr [g_frontEndCall]
    }
}

// Player set-up: player 1's soldier pick, then the same for players 2..N (the Xbox's stripped loop).
void __fastcall AssignPlayers(uintptr_t block0, void*) {
    using Pick = void(__thiscall*)(uintptr_t);
    reinterpret_cast<Pick>(kPickSoldier)(block0);
    if (!At<uint32_t>(kMultiplayer)) return;
    const uint32_t players = At<uint32_t>(kPlayerCount);
    if (const uintptr_t r = At<uintptr_t>(kRenderer); r && g_builtPlayers == static_cast<int>(players))
        WriteViewports(r, static_cast<splitscreen::Layout>(settings::Get().splitScreenLayout), g_builtPlayers);
    for (uint32_t i = 0; i < players && i < kMaxPlayers; ++i) {
        const uintptr_t block = kInputBlocks + i * kInputBlockSize;
        At<uint32_t>(block + kBlockView) = i;
        if (i == 0) continue;
        At<uint32_t>(block + kBlockPlayer) = reinterpret_cast<uint32_t*>(kPlayerTable)[i];
        At<uint32_t>(block + kBlockDevices) = 2;   // pad only
        reinterpret_cast<Pick>(kPickSoldier)(block);
    }
    // Co-op session: each player's own device - pad players pad only (bit 1) on their joystick; keyboard players
    // keyboard & mouse only (bit 0) and keep a joystick index (their player number, as the game sets it: a block with
    // joystick -1 is ignored entirely, keyboard included).
    if (features::CoopPlayers() >= 2)
        for (uint32_t i = 0; i < players && i < kMaxPlayers; ++i) {
            const uintptr_t block = kInputBlocks + i * kInputBlockSize;
            const int joystick = features::CoopJoystick(static_cast<int>(i));
            At<uint32_t>(block + kBlockDevices) = joystick >= 0 ? 2u : 1u;
            At<int32_t>(block + kBlockJoystick) = joystick >= 0 ? joystick : static_cast<int32_t>(i);
        }
    for (uint32_t i = 0; i < players && i < kMaxPlayers; ++i) {
        const uintptr_t block = kInputBlocks + i * kInputBlockSize;
        dslog::Write("Split screen: player %u block %08X player %08X soldier %08X joystick %d set %d devices %u", i + 1,
                     block, At<uint32_t>(block + kBlockPlayer), At<uint32_t>(block + kBlockSoldier),
                     At<int32_t>(block + kBlockJoystick), At<int32_t>(block + kBlockSet), At<uint32_t>(block + kBlockDevices));
    }
}

// Binding table allocation: 4 sets instead of 1 (thiscall(table, sets, slots, actions) ret 0xC).
__declspec(naked) void BindingInitStub() {
    __asm {
        mov dword ptr [esp + 4], 4
        mov eax, kBindingInit
        jmp eax
    }
}

// ---- analog controls: FUN_004230F0 (thiscall block, action, opposite; 0x51 = none) with the device check -----------
constexpr uint32_t kAxisValue = 0x4230F0;
constexpr uint32_t kBindingLookup = 0x40A470, kKeyHeld = 0x4CC030, kAxisTable = 0x67E948, kKeyboardUsed = 0x62FE1C;
constexpr int kNoAction = 0x51;

float __fastcall AxisValue(uint8_t* block, void*, int action, int opposite) {
    const int joystick = *reinterpret_cast<int32_t*>(block + kBlockJoystick);
    if (joystick == -1) return 0.0f;
    auto* table = At<uint8_t*>(kBindingTable);
    const int set = *reinterpret_cast<int32_t*>(block + kBlockSet);
    const uint32_t slots = *reinterpret_cast<uint32_t*>(table + 4);
    const uint32_t devices = *reinterpret_cast<uint32_t*>(block + kBlockDevices);
    using Lookup = uint32_t(__thiscall*)(void*, int, int, int);
    auto axis = [&](int act, float& value) {
        if (!(devices & 2)) return;
        for (uint32_t slot = 0; slot < slots; ++slot) {
            const uint32_t code = reinterpret_cast<Lookup>(kBindingLookup)(table, set, slot, act);
            if (code == 0xFFFFFFFF || (code & 0xFFFF0000) != 0x10000) continue;
            value = reinterpret_cast<const float*>(kAxisTable)[(code & 0xFFFF) + joystick * 0x70];
            if (value != 0.0f) *reinterpret_cast<uint32_t*>(block + 0x42C) = 0;
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
                At<uint32_t>(kKeyboardUsed) = 1;
            }
        }
        return held;
    };
    float value = 0.0f;
    axis(action, value);
    if (opposite != kNoAction) {
        if (value == 0.0f) axis(opposite, value);
        else value = -value;
    }
    if (value == 0.0f) {
        if (key(action)) value = 1.0f;
        if (opposite != kNoAction) {
            if (value != 0.0f) return -value;
            if (key(opposite)) value = 1.0f;
        }
    }
    return value;
}

// Mouse look: the smoothed mouse movement (FUN_004CACF0 on the smoother 0x680018, thiscall(&dx, &dy) ret 8) is read for
// turning / aiming at 0x425BD8 and 0x428817 with esi = the player's input block, without its device check - the mouse
// would turn every player. A block without keyboard & mouse (+0xC bit 0) gets no movement.
constexpr uint32_t kMouseDelta = 0x4CACF0;
constexpr uint32_t kMouseDeltaSites[] = {0x425BD8, 0x428817};
__declspec(naked) void MouseDeltaStub() {
    __asm {
        test byte ptr [esi + 0xC], 1
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

// Downed soldier: the per-block update tail-jumps (0x427816) to the "Slow down time" ramp FUN_0041E4B0 while the player's
// soldier is down (as DS1's 0x44EFAB). In split screen the other players keep normal speed.
constexpr uint32_t kSlowDownJump = 0x427816, kSlowDown = 0x41E4B0;
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

// Divider lines (as the Xbox's Split_DrawDivider): grey, max(2, H/240) px, on every inner view edge. Drawn when the
// render loop FUN_0040FBA0 sets the full-screen viewport again after the per-view passes and the HUD pass (0x40FF4A,
// FUN_00537440 thiscall(renderer, D3DVIEWPORT9*)) - not while a cutscene renders full screen (the loop's own test).
constexpr uint32_t kSetViewport = 0x537440, kFullViewportSite = 0x40FF4A;
constexpr uint32_t kWorld = 0x68008C, kGameState = 0x63433C, kSplitRender = 0x62F3A8;

bool SplitRendering() {
    const uintptr_t world = At<uintptr_t>(kWorld);
    if (!world || (At<uint8_t>(world + 0x1B8C) & 1) || !At<uint32_t>(kSplitRender)) return false;
    switch (At<uint32_t>(kGameState)) {
        case 0x15: case 0x19: case 0x2A: case 0x30: return false;
        default: return true;
    }
}

void __fastcall SetFullViewport(uintptr_t r, void*, const D3DVIEWPORT9* vp) {
    reinterpret_cast<void(__thiscall*)(uintptr_t, const D3DVIEWPORT9*)>(kSetViewport)(r, vp);
    if (g_builtPlayers < 2 || !SplitRendering()) return;
    auto dev = At<IDirect3DDevice9*>(r + 0x4187C);
    if (!dev) return;
    const LONG bw = static_cast<LONG>(At<uint32_t>(r + kBackBufferW)), bh = static_cast<LONG>(At<uint32_t>(r + kBackBufferH));
    const LONG t = bh / 240 > 2 ? bh / 240 : 2;
    D3DRECT rects[8];
    DWORD n = 0;
    for (int i = 0; i < g_builtPlayers && i < 4 && n + 2 <= 8; ++i) {
        auto e = reinterpret_cast<const LONG*>(r + kViewports + i * kViewportStride);
        const LONG x0 = e[0], y0 = e[1], x1 = e[0] + e[2], y1 = e[1] + e[3];
        if (x1 < bw) rects[n++] = {x1 - t / 2, y0, x1 - t / 2 + t, y1};   // right edge
        if (y1 < bh) rects[n++] = {x0, y1 - t / 2, x1, y1 - t / 2 + t};   // bottom edge
    }
    if (n) dev->Clear(n, rects, D3DCLEAR_TARGET, D3DCOLOR_XRGB(128, 128, 128), 1.0f, 0);
}
}  // namespace

int features::SplitPlayers() { return g_builtPlayers; }

void features::SplitViewSize(int& w, int& h) {
    if (g_builtPlayers < 2 || g_viewW <= 0 || g_viewH <= 0) return;
    w = g_viewW;
    h = g_viewH;
}

void features::ApplySplitScreen() {
    static const uint8_t kFrontEndEntry[] = {0x51, 0x53, 0xE8, 0x19, 0x6B, 0x09, 0x00};
    static const uint8_t kAxisEntry[] = {0x51, 0x56, 0x8B, 0xF1, 0x83, 0xBE};
    if (!patch::Matches(kFrontEndInit, kFrontEndEntry, sizeof kFrontEndEntry) ||
        !patch::Matches(kAxisValue, kAxisEntry, sizeof kAxisEntry) ||
        !patch::HookCall(kLevelLoadSite, reinterpret_cast<void*>(&LevelLoad), kLevelLoad) ||
        !patch::HookCall(kPickSoldierSite, reinterpret_cast<void*>(&AssignPlayers), kPickSoldier) ||
        !patch::HookCall(kBindingInitSite, reinterpret_cast<void*>(&BindingInitStub), kBindingInit)) {
        dslog::Write("[skip] Split screen: unknown code at the hook sites");
        return;
    }
    patch::WriteJump(kFrontEndInit, reinterpret_cast<void*>(&FrontEndInitStub));
    patch::WriteJump(kAxisValue, reinterpret_cast<void*>(&AxisValue));
    patch::HookCall(kFullViewportSite, reinterpret_cast<void*>(&SetFullViewport), kSetViewport);
    for (uint32_t site : kMouseDeltaSites) patch::HookCall(site, reinterpret_cast<void*>(&MouseDeltaStub), kMouseDelta);
    static const uint8_t kSlowJmp[] = {0xE9, 0x95, 0x6C, 0xFF, 0xFF};
    if (patch::Matches(kSlowDownJump, kSlowJmp, sizeof kSlowJmp))
        patch::WriteJump(kSlowDownJump, reinterpret_cast<void*>(&SlowDownStub));
    dslog::Write("[ok]   Split screen: hooks installed (players from the co-op session / dev SplitPlayers)");
}

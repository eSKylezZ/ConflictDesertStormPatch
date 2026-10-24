// Which players' views a PC shows (lobby stage 5, user 2026-10-22: "an option if players don't want to play in split
// screen (per players choice), the ability to hold a key combination to view a specific player and what their screen
// is doing in a small window top right").
//
// Online every PC builds a view per player (the game's split screen, splitscreen.cpp). OWN VIEW in the lobby
// (lobby::View::Own, any of a PC's players): that PC shows only its own players' views - full screen for one, split
// among them for two or more; SPLIT SCREEN (the default) keeps every player's view. Local split screen is unchanged.
// Peek: hold Tab (SHOW_SCORES - a versus key) or D-pad Right / Left (only the objectives screen and menus use them) to
// see a player whose view isn't shown in a window top right; each new hold moves on to the next such player (Left:
// back). The window shows the world without that player's HUD, with the name tag (nameplates.cpp) and a frame.
//
// How: the render loop FUN_0040ef70 draws view i with viewport i (renderer +0x24 + i * 0x18), camera i and the call
// FUN_0040e0f0(i) (world + HUD, call 0x40F0CB). Each frame the viewports are laid out here (hidden ones 1 x 1 at the
// screen's corner, so the loop's clear of them touches nothing) and the call is skipped for hidden views and the
// peeked one; the peeked view is drawn after the loop (in front of the call 0x40F16F) with the loop's own steps into
// the corner rectangle, the HUD wrappers off (features::HudHidden). The layout goes back to the game's when nothing
// is hidden. The soldier panels / inventory / context prompts drawn after the loop skip hidden players
// (features::ViewHidden), splitscreen's divider lines skip hidden views.
// Prisoners (user 2026-10-22: player 2 sat in Foley's cell for the whole of Mission 1): while every player of an OWN
// VIEW PC is a captive (a squad soldier with soldier +0x284 bit 0x200 - Mission 1's Foley until the rescue; the
// release, API +0x1D8 / +0x180, clears it), that PC shows the first free player's view instead, with a notice, until one of its players is free.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/lobbynet.h"
#include "features/overlay.h"
#include "features/splitscreen_layout.h"

namespace {
constexpr uint32_t kRenderer = 0x63C924, kViewportCount = 0x20, kViewports = 0x24, kViewportStride = 0x18;
constexpr uint32_t kFullViewport = 0x40650, kDevice = 0x40530;
constexpr uint32_t kViewCall = 0x40F0CB, kViewDraw = 0x40E0F0;          // cdecl (view)
constexpr uint32_t kAfterLoopCall = 0x40F16F, kAfterLoop = 0x51BF90;     // thiscall [0x63C95C]
constexpr uint32_t kSplitActive = 0x40E080;                             // cdecl: 0 = one full-screen view (cutscene)
constexpr uint32_t kCurrentView = 0x63C9E8, kViewControllers = 0x63C990;
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478;
constexpr uint32_t kCameras = 0x63C9D8, kWorld = 0x63C948, kCameraObj = 0x63C940;
constexpr uint32_t kFrontEndState = 0x617C18, kDispatchMode = 0x60EC8C;
// the loop's steps (thiscall unless noted)
constexpr uint32_t kGetViewport = 0x547C30, kSetViewport = 0x542F40, kBlockBegin = 0x44DE20, kCameraUpdate = 0x4D8330;
constexpr uint32_t kCameraPos = 0x4D8390, kAtmosEye = 0x4EEC30, kClear = 0x546AE0, kCurrentVp = 0x542FC0;
constexpr uint32_t kVcBegin = 0x4D1DB0, kVcEnd = 0x4D1E30, kBlockEnd = 0x44DED0, kState544600 = 0x544600;
constexpr uint32_t kVcFlush = 0x4D25D0;

struct Viewport {
    uint32_t x, y, w, h;
    float minZ, maxZ;
};

uint8_t g_baseline[4 * kViewportStride];   // the game's viewports while nothing is hidden
bool g_changed = false;
uint32_t g_shown = 0xF;                    // views drawn by the loop this frame
int g_peek = -1;                           // player shown in the corner window, -1 none
int g_peekCycle = -1;                      // the last peeked player (each new hold moves on)
bool g_holding = false, g_peekPass = false;
void* g_viewNext = reinterpret_cast<void*>(kViewDraw);
void* g_afterNext = reinterpret_cast<void*>(kAfterLoop);
float g_peekFrame[4] = {};                 // the corner window drawn this frame (back-buffer pixels)
DWORD g_peekFrameAt = 0;
int g_watching = -1;                       // the free player shown while this PC's players are all captives

uint8_t* Renderer() { return *reinterpret_cast<uint8_t**>(kRenderer); }
Viewport* Vp(uint8_t* r, int i) { return reinterpret_cast<Viewport*>(r + kViewports + i * kViewportStride); }
bool SplitActive() { return reinterpret_cast<int(__cdecl*)()>(kSplitActive)() != 0; }

void Restore(uint8_t* r) {
    if (!g_changed) return;
    std::memcpy(r + kViewports, g_baseline, sizeof g_baseline);
    features::SplitHudReset();
    g_changed = false;
    dslog::Write("Views: every player's view again");
}

// The peek key: a fresh hold of Tab / a local pad's D-pad Right (next) or Left (previous).
int PeekStep() {
    int step = 0;
    if (features::KeyHeld(VK_TAB)) step = 1;
    for (int p = 0; p < 4 && !step; ++p) {
        if (!features::LobbyLocal(p)) continue;
        const int d = features::LobbyDevice(p);
        const int j = d < 0 ? 0 : d;
        if (d >= 8) continue;
        const uint32_t pad = features::ReadJoystick(j);
        if (pad & (1u << 13)) step = 1;        // D-pad right
        else if (pad & (1u << 15)) step = -1;  // D-pad left
    }
    return step;
}

// A player's soldier held captive: a squad member (unit +0x1C7 squad slot >= 0) with the prisoner bit +0x284 0x200.
bool Captive(int player) {
    __try {
        const uint8_t* soldier = *reinterpret_cast<uint8_t**>(kBlocks + player * kBlockSize + 0x310);
        if (!soldier) return false;
        const uint8_t* unit = *reinterpret_cast<uint8_t* const*>(soldier + 0x24);
        return unit && *reinterpret_cast<const int8_t*>(unit + 0x1C7) >= 0 &&
               (*reinterpret_cast<const uint32_t*>(soldier + 0x284) & 0x200) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Layout() {
    uint8_t* r = Renderer();
    if (!r) return;
    const int n = static_cast<int>(*reinterpret_cast<uint32_t*>(r + kViewportCount));
    const bool mission = *reinterpret_cast<uint32_t*>(kFrontEndState) == 0xE && *reinterpret_cast<int8_t*>(kDispatchMode) == 0;
    if (n < 2 || n > 4 || !mission || !features::LockstepActive() || !features::LobbyOnline()) {
        if (n >= 2 && n <= 4) Restore(r);
        g_shown = 0xF;
        g_peek = -1;
        g_watching = -1;
        if (!g_changed && n >= 2 && n <= 4) std::memcpy(g_baseline, r + kViewports, sizeof g_baseline);
        return;
    }
    if (!g_changed) std::memcpy(g_baseline, r + kViewports, sizeof g_baseline);
    const uint32_t all = (1u << n) - 1, local = features::LobbyLocalMask() & all;
    bool own = false;
    for (int p = 0; p < n; ++p)
        own |= ((local >> p) & 1) && lobbynet::Current().slots[p].used && lobbynet::Current().slots[p].view == lobby::View::Own;
    uint32_t shown = own && local ? local : all;
    // every local player a prisoner: watch the first free player instead
    const int was = g_watching;
    g_watching = -1;
    if (own && local) {
        bool allCaptive = true;
        for (int p = 0; p < n; ++p)
            if ((local >> p) & 1) allCaptive &= Captive(p);
        for (int p = 0; p < n && allCaptive && g_watching < 0; ++p)
            if (!((local >> p) & 1) && !Captive(p)) g_watching = p;
        if (g_watching >= 0) shown = 1u << g_watching;
    }
    if (g_watching != was)
        dslog::Write(g_watching >= 0 ? "Views: this PC's players are prisoners - watching player %d" : "Views: back to this PC's own view%.0d",
                     g_watching + 1);
    // peek: only players whose view isn't shown
    const int step = shown == all ? 0 : PeekStep();
    if (step && !g_holding) {
        for (int k = 1; k <= n; ++k) {
            const int p = ((g_peekCycle < 0 ? (step > 0 ? -1 : 0) : g_peekCycle) + step * k + n * 4) % n;
            if (!((shown >> p) & 1)) {
                g_peekCycle = p;
                break;
            }
        }
    }
    g_holding = step != 0;
    const int wasPeek = g_peek;
    g_peek = g_holding && g_peekCycle >= 0 && !((shown >> g_peekCycle) & 1) ? g_peekCycle : -1;
    if (g_peek != wasPeek && g_peek >= 0) dslog::Write("Views: peeking at player %d", g_peek + 1);
    g_shown = shown;
    if (shown == all) return Restore(r);
    // shown views: the game's layout for their count; hidden: 1 x 1 at the screen's corner
    const Viewport full = *reinterpret_cast<Viewport*>(r + kFullViewport);
    const auto orient = static_cast<splitscreen::Layout>(settings::Get().splitScreenLayout);
    int count = 0;
    for (int p = 0; p < n; ++p) count += (shown >> p) & 1;
    int k = 0;
    for (int p = 0; p < n; ++p) {
        Viewport& v = *Vp(r, p);
        const Viewport base = *reinterpret_cast<const Viewport*>(g_baseline + p * kViewportStride);
        if ((shown >> p) & 1) {
            const splitscreen::View f = splitscreen::ViewRect(orient, count, k++);
            v.x = full.x + static_cast<uint32_t>(f.x * full.w);
            v.y = full.y + static_cast<uint32_t>(f.y * full.h);
            v.w = static_cast<uint32_t>(f.w * full.w);
            v.h = static_cast<uint32_t>(f.h * full.h);
            features::SplitHudView(p, f.x, f.y, f.w, f.h);   // that rectangle's HUD layout (full / half / quarter)
        } else {
            v.x = full.x, v.y = full.y, v.w = 1, v.h = 1;
        }
        v.minZ = base.minZ, v.maxZ = base.maxZ;
    }
    if (!g_changed) dslog::Write("Views: this PC shows players mask %X of %d (own view)", shown, n);
    g_changed = true;
}

void __cdecl ViewDraw(int view) {
    if (g_changed && view >= 0 && view < 4 && SplitActive() && (!((g_shown >> view) & 1) || view == g_peek)) return;
    reinterpret_cast<void(__cdecl*)(int)>(g_viewNext)(view);
}

// The loop's body for one view (FUN_0040ef70 0x40F005..0x40F143), into the corner window.
void PeekPass(int i) {
    uint8_t* r = Renderer();
    if (!r) return;
    Viewport& vp = *Vp(r, i);
    const Viewport saved = vp;
    const Viewport full = *reinterpret_cast<Viewport*>(r + kFullViewport);
    vp.w = full.w * 3 / 10;
    vp.h = vp.w * full.h / std::max<uint32_t>(1, full.w);
    vp.x = full.x + full.w - vp.w - full.w / 40;
    vp.y = full.y + full.h / 30;
    const uint32_t savedView = *reinterpret_cast<uint32_t*>(kCurrentView);
    auto* block = reinterpret_cast<void*>(kBlocks + i * kBlockSize);
    void* vc = reinterpret_cast<void**>(kViewControllers)[i];
    auto* cams = *reinterpret_cast<uint8_t**>(kCameras);
    if (!vc || !cams) return;
    g_peekPass = true;
    *reinterpret_cast<uint32_t*>(kCurrentView) = static_cast<uint32_t>(i);
    void* v = reinterpret_cast<void*(__thiscall*)(void*, int)>(kGetViewport)(r, i);
    reinterpret_cast<void(__thiscall*)(void*, void*)>(kSetViewport)(r, v);
    *reinterpret_cast<void**>(*reinterpret_cast<uint8_t**>(kCameraObj) + 0x51C) = v;
    reinterpret_cast<void(__thiscall*)(void*)>(kBlockBegin)(block);
    reinterpret_cast<void(__thiscall*)(void*, int)>(kCameraUpdate)(cams, i);
    const float* pos = reinterpret_cast<float*(__thiscall*)(void*, uint32_t)>(kCameraPos)(
        cams, *reinterpret_cast<uint32_t*>(cams + 0xEC0C + i * 4));
    float eye[3] = {pos[0], pos[1], pos[2]};
    reinterpret_cast<void(__thiscall*)(void*, float*)>(kAtmosEye)(
        *reinterpret_cast<void**>(*reinterpret_cast<uint8_t**>(kWorld) + 0x1A58), eye);
    reinterpret_cast<void(__thiscall*)(void*, int)>(kClear)(r, 1);
    *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(vc) + 0x2C0) = reinterpret_cast<uint32_t(__thiscall*)(void*)>(kCurrentVp)(r);
    reinterpret_cast<void(__thiscall*)(void*)>(kVcBegin)(vc);
    reinterpret_cast<void(__cdecl*)(int)>(kViewDraw)(i);
    reinterpret_cast<void(__thiscall*)(void*)>(kVcEnd)(vc);
    reinterpret_cast<void(__thiscall*)(void*)>(kBlockEnd)(block);
    reinterpret_cast<void(__thiscall*)(void*, int)>(kState544600)(r, 1);
    void* dev = *reinterpret_cast<void**>(r + kDevice);
    using SetRs = HRESULT(__stdcall*)(void*, DWORD, DWORD);
    for (auto [off, state] : {std::pair<uint32_t, DWORD>{0x40814, 0x1B}, {0x407E4, 0x0F}})
        if (*reinterpret_cast<uint32_t*>(r + off) != 1) {
            *reinterpret_cast<uint32_t*>(r + off) = 1;
            if (dev) reinterpret_cast<SetRs>((*reinterpret_cast<void***>(dev))[50])(dev, state, 1);
        }
    reinterpret_cast<void(__thiscall*)(void*)>(kVcFlush)(vc);
    *reinterpret_cast<uint32_t*>(kCurrentView) = savedView;
    g_peekPass = false;
    // the frame round it, in the late draw (over the HUD)
    g_peekFrame[0] = static_cast<float>(vp.x), g_peekFrame[1] = static_cast<float>(vp.y);
    g_peekFrame[2] = static_cast<float>(vp.w), g_peekFrame[3] = static_cast<float>(vp.h);
    vp = saved;
}

// The sound listeners (one per view: camera positions / rotations gathered at 0x40EEF0.., set by FUN_0055D8B0
// thiscall(sound manager, count, float pos[][3], float rot[][3]) at 0x40EF60; each sound plays from the nearest one):
// only the views this PC shows - a hidden teammate's surroundings stay quiet, peeking doesn't change what you hear.
constexpr uint32_t kListenerCall = 0x40EF60, kSetListeners = 0x55D8B0;
void* g_listenersNext = reinterpret_cast<void*>(kSetListeners);
void __fastcall SetListeners(void* mgr, void*, int count, const float* pos, const float* rot) {
    if (g_changed && count >= 2 && count <= 4) {
        float p[4][3], r[4][3];
        int n = 0;
        for (int i = 0; i < count; ++i) {
            if (features::ViewHidden(i)) continue;
            std::memcpy(p[n], pos + i * 3, sizeof p[n]);
            std::memcpy(r[n], rot + i * 3, sizeof r[n]);
            ++n;
        }
        if (n > 0 && n < count) {
            static int logged = -1;
            if (logged != n) dslog::Write("Views: %d of %d sound listeners (the shown views)", n, count), logged = n;
            return reinterpret_cast<void(__thiscall*)(void*, int, const float*, const float*)>(g_listenersNext)(mgr, n, &p[0][0], &r[0][0]);
        }
    }
    reinterpret_cast<void(__thiscall*)(void*, int, const float*, const float*)>(g_listenersNext)(mgr, count, pos, rot);
}

void __fastcall AfterLoop(void* ecx, void*) {
    if (g_changed && g_peek >= 0 && SplitActive() && !features::HudHidden()) {
        PeekPass(g_peek);
        g_peekFrameAt = GetTickCount();
    }
    reinterpret_cast<void(__thiscall*)(void*)>(g_afterNext)(ecx);
}

// The prisoner notice: bottom centre over the watched player's view.
void WatchNotice() {
    if (g_watching < 0 || !g_changed || features::HudHidden()) return;
    uint8_t* r = Renderer();
    void* font = overlay::MenuFont();
    if (!r || !font) return;
    const lobby::Table& t = lobbynet::Current();
    std::string mine, other = "PLAYER " + std::to_string(g_watching + 1);
    for (int p = 0; p < lobby::kMaxPlayers; ++p)
        if (features::LobbyLocal(p) && t.slots[p].used && mine.empty()) mine = t.slots[p].name;
    if (g_watching < lobby::kMaxPlayers && t.slots[g_watching].used) other = t.slots[g_watching].name;
    const std::string text = (mine.empty() ? std::string("YOU ARE") : mine + " IS") + " A PRISONER - WATCHING " + other +
                             " UNTIL YOU ARE FREED (TAB / D-PAD: PEEK)";
    const Viewport full = *reinterpret_cast<Viewport*>(r + kFullViewport);
    const float line = static_cast<float>(std::max(8, overlay::TextWidth(font, "M")));
    const float tw = static_cast<float>(overlay::TextWidth(font, text.c_str()));
    const float x = full.x + (full.w - tw) / 2.0f, y = full.y + full.h * 0.80f;
    const uint32_t rgb = g_watching < lobby::kMaxPlayers && t.slots[g_watching].used ? lobby::kPalette[t.slots[g_watching].colour] : 0xFFFFFF;
    overlay::QueueScreenRect(x - line, y - line * 1.6f, tw + line * 2, line * 2.4f, 0xA0000000);
    overlay::QueueScreenRect(x - line, y + line * 0.8f, tw + line * 2, std::max(2.0f, line / 4), 0xFF000000 | rgb);
    overlay::Flush();
    overlay::DrawLabel(font, text.c_str(), static_cast<int>(x), static_cast<int>(y), 0xFFFFFFFF);
}

void LateDraw() {
    WatchNotice();
    if (g_peek < 0 || GetTickCount() - g_peekFrameAt > 100) return;
    const float x = g_peekFrame[0], y = g_peekFrame[1], w = g_peekFrame[2], h = g_peekFrame[3];
    const float t = std::max(2.0f, h / 120.0f);
    uint32_t rgb = 0xFFFFFF;
    if (g_peek < lobby::kMaxPlayers && lobbynet::Current().slots[g_peek].used)
        rgb = lobby::kPalette[lobbynet::Current().slots[g_peek].colour];
    const uint32_t c = 0xFF000000 | rgb;
    overlay::QueueScreenRect(x - t, y - t, w + 2 * t, t, c);
    overlay::QueueScreenRect(x - t, y + h, w + 2 * t, t, c);
    overlay::QueueScreenRect(x - t, y, t, h, c);
    overlay::QueueScreenRect(x + w, y, t, h, c);
    overlay::Flush();
}
}  // namespace

void features::OnFrameViews() { Layout(); }
bool features::ViewHidden(int player) {
    return g_changed && player >= 0 && player < 4 && (!((g_shown >> player) & 1) || player == g_peek);
}
bool features::ViewPeekPass() { return g_peekPass; }

void features::ApplyViews() {
    static bool done = false;
    if (done) return;
    done = true;
    const bool ok = patch::SubscribeCall(kViewCall, reinterpret_cast<const void*>(&ViewDraw), kViewDraw, 0, &g_viewNext) &&
                    patch::SubscribeCall(kAfterLoopCall, reinterpret_cast<const void*>(&AfterLoop), kAfterLoop, 0, &g_afterNext) &&
                    patch::SubscribeCall(kListenerCall, reinterpret_cast<const void*>(&SetListeners), kSetListeners, 0,
                                         &g_listenersNext);
    if (ok) overlay::AddLateDraw(&LateDraw);
    dslog::Write(ok ? "[ok]   Views: own view + peek window (online)" : "[fail] Views: render loop hooks - every view shown");
}

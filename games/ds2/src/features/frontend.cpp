// Front-end menus at any resolution. The front-end = pixel-sized 2D menus in the screen centre (a globe through its
// own viewport) framed by a 3D "monitor" whose size follows the screen - they only line up at 1280x1024 (cut off /
// misplaced elsewhere). While it runs the game is told the screen is 1280 x 1024 (graphics::kVirtualW/H, renderer
// [0x680038] +0x41A38 / +0x41A3C), so it lays out exactly as designed, and graphics.cpp maps that picture onto the real back buffer, scaled and centred.
//   device created (graphics.cpp callback): on - the start-up and first front-end use it too
//   front-end init  FUN_0042c930 (thiscall, called at 0x42CF9B; stores "FrontEnd" as the scene name)  -> on
//   mission init    FUN_00412690 (cdecl(int), called at 0x42CB64; stores the mission's scene name)    -> off
// The HUD scale k is 1 while the virtual screen is on (hud.cpp), so the menus' art isn't scaled twice.
#include <windows.h>

#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/graphics.h"

namespace {
constexpr uint32_t kRenderer = 0x680038, kWidth = 0x41A38, kHeight = 0x41A3C;
constexpr uint32_t kFrontEndInit = 0x42C930, kFrontEndCall = 0x42CF9B, kMissionInit = 0x412690,
                   kMissionCall = 0x42CB64, kEngineInit = 0x4D0A00, kEngineCall = 0x4137E3;

int g_realW = 0, g_realH = 0;

// The renderer keeps its own copy of the full-screen viewport - FUN_00537380 (called once at 0x541F67 after the device
// is made) builds it from the back buffer (+0x94 / +0x98): +0x41A00 D3DVIEWPORT9, +0x41A1C screen centre (W/2, H/2),
// +0x34 the first entry of its viewport list. The 3D camera (the front-end's monitor and globe) works from these, so
// they say kVirtualW x kVirtualH while the virtual screen is on and the real size again after.
void SetRendererViewport(int w, int h) {
    auto r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    if (!r) return;
    *reinterpret_cast<int*>(r + 0x41A08) = w;
    *reinterpret_cast<int*>(r + 0x41A0C) = h;
    *reinterpret_cast<float*>(r + 0x41A1C) = w * 0.5f;
    *reinterpret_cast<float*>(r + 0x41A20) = h * 0.5f;
    *reinterpret_cast<int*>(r + 0x3C) = w;
    *reinterpret_cast<int*>(r + 0x40) = h;
}

void __fastcall RendererViewport(void* renderer, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(0x537380)(renderer);
    if (graphics::VirtualScreenOn()) SetRendererViewport(graphics::kVirtualW, graphics::kVirtualH);
}

int* Field(uint32_t offset) {
    auto r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    return r ? reinterpret_cast<int*>(r + offset) : nullptr;
}

// realW / realH: the back buffer (device creation) or 0 = take the renderer's fields
void Enter(int realW = 0, int realH = 0) {
    int *w = Field(kWidth), *h = Field(kHeight);
    if (!w || !h) return;
    if (graphics::VirtualScreenOn()) {   // back to the front-end, or the game rewrote its fields
        *w = graphics::kVirtualW;
        *h = graphics::kVirtualH;
        SetRendererViewport(graphics::kVirtualW, graphics::kVirtualH);
        return;
    }
    if (!realW) realW = *w, realH = *h;
    // (also at a real 1280 x 1024: the mapping is then 1:1, but the art must stay unscaled - k = 1 - there too)
    g_realW = realW;
    g_realH = realH;
    *w = graphics::kVirtualW;
    *h = graphics::kVirtualH;
    graphics::SetVirtualScreen(true, g_realW, g_realH);
    SetRendererViewport(graphics::kVirtualW, graphics::kVirtualH);
}

// the start-up (logos, films, sheet / font set-up) and the front-end run on the virtual screen from the first frame:
// sheets and fonts set up at the real size would otherwise carry k on top of the mapping
void OnDeviceCreated(int w, int h) { Enter(w, h); }

void Leave() {
    if (!graphics::VirtualScreenOn()) return;
    if (int *w = Field(kWidth), *h = Field(kHeight); w && h) {
        *w = g_realW;
        *h = g_realH;
    }
    graphics::SetVirtualScreen(false);
    SetRendererViewport(g_realW, g_realH);
}

// Engine start-up FUN_004d0a00 (cdecl, 5 args; "Ivor", called at 0x4137E3 from initGameSystem) creates the device and
// then writes the real screen size into the renderer fields again. Everything initGameSystem builds after it - the font
// sheets and the front-end's monitor / globe scene (FUN_00418af0: monitor distance W * 0.234375, globe scale 640 / W,
// globe position from a table for W = 640 / 800 / 1024 / 1280) - must see the virtual size.
int __cdecl EngineInit(int a, int b, int c, int d, int e) {
    const int r = reinterpret_cast<int(__cdecl*)(int, int, int, int, int)>(kEngineInit)(a, b, c, d, e);
    if (graphics::VirtualScreenOn()) Enter();
    return r;
}

// Menu mouse: the game reads the cursor as GetCursorPos + ScreenToClient (0x4CB1B5: mov ebx, [ScreenToClient]; used
// at 0x4CB206) and its cursor setter stores SetCursorPos's point through ScreenToClient (0x4CB17C) into [0x67F6D8].
// On the virtual screen a client position is a back-buffer pixel (scaled first if a window's client area differs from
// the back buffer) mapped back into the 1280 x 1024 picture.
BOOL WINAPI ScreenToGame(HWND window, POINT* p) {
    const BOOL ok = ScreenToClient(window, p);
    if (ok && p && graphics::VirtualScreenOn()) {
        RECT rc;
        if (GetClientRect(window, &rc) && rc.right > 0 && rc.bottom > 0) {
            p->x = p->x * graphics::RealWidth() / rc.right;
            p->y = p->y * graphics::RealHeight() / rc.bottom;
        }
        graphics::RealToVirtual(p->x, p->y);
    }
    return ok;
}

// ---- the menus inside a mission (pause menu state 0x14 and its MISSION STATS / LOAD / SAVE / OPTIONS ... screens) -------
// They are the front-end's screens on the same 3D monitor, made for 1280 x 1024 with unscaled art; in a mission the
// screen is real and the sheets carry k, so text and art overflowed their boxes (any size above 800 x 600). Entering a
// state, FUN_0043df10 switches the front-end backdrop with FUN_0040b2b0(on) (only caller 0x43DF2E; on = a menu state,
// flags [0x609684] / [0x609688]) - in a mission that now also switches the virtual screen: on = the mission's renderer
// viewports (split screen: several views) and HUD sheet scales are saved, virtual screen on, sheets at k = 1; off = all
// put back. (FUN_0040b2b0 is replaced; menus.cpp hooks its call site, which still lands here.)
constexpr uint32_t kBackdrop = 0x40B2B0;
constexpr uint32_t kViewsAt = 0x30, kViewsLen = 0x64, kFullAt = 0x41A00, kFullLen = 0x24;
bool g_inMission = false, g_missionMenu = false;
uint8_t g_viewsSave[kViewsLen], g_fullSave[kFullLen];

void MissionMenu(bool on) {
    auto r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    if (!r || on == g_missionMenu) return;
    if (on) {
        if (graphics::VirtualScreenOn()) return;
        std::memcpy(g_viewsSave, r + kViewsAt, kViewsLen);
        std::memcpy(g_fullSave, r + kFullAt, kFullLen);
        features::SaveHudSheets();
        Enter();
        features::RescaleHudSheets();
        g_missionMenu = true;
    } else {
        Leave();
        std::memcpy(r + kViewsAt, g_viewsSave, kViewsLen);
        std::memcpy(r + kFullAt, g_fullSave, kFullLen);
        features::RestoreHudSheets();
        g_missionMenu = false;
    }
    dslog::Write("Front-end: mission menu %s (state 0x%X)", on ? "on - virtual screen" : "off", *reinterpret_cast<int*>(0x63433C));
}

void __cdecl Backdrop(int on) {   // copy of FUN_0040b2b0
    *reinterpret_cast<int*>(0x609684) = on;
    *reinterpret_cast<int*>(0x609688) = on != 0;
    if (g_inMission) MissionMenu(on != 0);
}

// The calls they replace: the game's functions, or - as a PSE host plugin (DS2_PSE_HOST) - the next hook of the
// broker chain on those call sites (the DS2 game layer's level events sit there too).
void* g_frontEndNext = reinterpret_cast<void*>(kFrontEndInit);
void* g_missionNext = reinterpret_cast<void*>(kMissionInit);

int __fastcall FrontEndInit(void* self, void*) {
    g_inMission = g_missionMenu = false;
    features::ForgetHudSheets();
    Enter();
    return reinterpret_cast<int(__thiscall*)(void*)>(g_frontEndNext)(self);
}

int __cdecl MissionInit(int arg) {
    Leave();
    g_missionMenu = false;
    g_inMission = true;
    features::ForgetHudSheets();
    return reinterpret_cast<int(__cdecl*)(int)>(g_missionNext)(arg);
}
}  // namespace

void features::ApplyFrontEnd() {
    if (!settings::Get().hudScaling) return;   // the same switch as the HUD: "scale the 2D to the screen"
#if defined(DS2_PSE_HOST) || defined(DS2_WIDESCREEN_PLUGIN)
    if (patch::SubscribeCall(kFrontEndCall, reinterpret_cast<void*>(&FrontEndInit), kFrontEndInit, 0, &g_frontEndNext) &&
        patch::SubscribeCall(kMissionCall, reinterpret_cast<void*>(&MissionInit), kMissionInit, 0, &g_missionNext)) {
#else
    if (patch::HookCall(kFrontEndCall, reinterpret_cast<void*>(&FrontEndInit), kFrontEndInit) &&
        patch::HookCall(kMissionCall, reinterpret_cast<void*>(&MissionInit), kMissionInit)) {
#endif
        graphics::g_onDeviceCreated = &OnDeviceCreated;
        // mov ebx, [ScreenToClient] -> mov ebx, ScreenToGame; nop
        static const uint8_t kReadOld[] = {0x8B, 0x1D, 0x60, 0xA2, 0x5D, 0x00};
        if (patch::Matches(0x4CB1B5, kReadOld, sizeof kReadOld)) {
            uint8_t code[6] = {0xBB, 0, 0, 0, 0, 0x90};
            const auto fn = reinterpret_cast<uint32_t>(&ScreenToGame);
            std::memcpy(code + 1, &fn, 4);
            patch::Write(0x4CB1B5, code, sizeof code);
        }
        patch::HookIndirectCall(0x4CB17C, reinterpret_cast<void*>(&ScreenToGame), 0x5DA260);
        patch::HookCall(kEngineCall, reinterpret_cast<void*>(&EngineInit), kEngineInit);
        patch::HookCall(0x541F67, reinterpret_cast<void*>(&RendererViewport), 0x537380);
        static const uint8_t kBackdropEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x33, 0xC9, 0x85, 0xC0};
        if (patch::Matches(kBackdrop, kBackdropEntry, sizeof kBackdropEntry))
            patch::WriteJump(kBackdrop, reinterpret_cast<void*>(&Backdrop));
        else
            dslog::Write("[fail] Front-end: menu backdrop switch 0x%08X not recognised - in-mission menus unscaled", kBackdrop);
        dslog::Write("[ok]   Front-end: laid out at 1280x1024, scaled to the screen");
    }
}

// Front end at the right aspect ratio. The menus, logo and menu background are laid out for a 4:3 screen and stretched
// to whatever the screen is (1920x1080: logo and background 33 % too wide, logo cut off at the top). While the front
// end runs the game is told the screen is 4:3 at the real height (renderer [0x8842D0] +0x41A34 W / +0x41A38 H, e.g.
// 1440 x 1080), so it lays out as designed - still >= 1024 wide, so it keeps its high-resolution art (cv_fe_hi.img) -
// and graphics.cpp maps that picture onto the real back buffer, centred (pillarboxed). Screens narrower than 4:3
// (5:4) get the full width and are letterboxed instead.
//   LoadFrontEnd  FUN_00428699 (cdecl, called at 0x428C69 from the loop's level picker FUN_00428ba3)  -> on
//   level init    FUN_004314cd (cdecl(name), "levelInitialise", called at 0x428950)                   -> off
// Loading screens too: ShowLoadingScreen FUN_0042f3d6 (cdecl, 5 args, called at 0x43175C inside level init) draws the
// mission's 4:3 picture over the whole screen, and the "PLEASE PRESS ANY KEY" frames after the load (FUN_0042c74d, the
// message loop's call at 0x43147E - hooked by frame.cpp) draw on top of it without clearing - both on the virtual
// screen, so the picture is pillarboxed like the front end. So does the load-progress callback FUN_0042d2c9 (cdecl, 5
// args; the loading bar under the picture), registered by "push 0x42D2C9" at 0x42D55D / 0x4316DE / 0x431FAE.
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/graphics.h"

namespace features {
void (*g_afterFrontEndLoad)() = nullptr;
}

namespace {
constexpr uint32_t kRenderer = 0x8842D0, kWidth = 0x41A34, kHeight = 0x41A38;
constexpr uint32_t kLoadFrontEnd = 0x428699, kLoadFrontEndCall = 0x428C69, kLevelInit = 0x4314CD,
                   kLevelInitCall = 0x428950, kShowLoading = 0x42F3D6, kShowLoadingCall = 0x43175C;

int g_realW = 0, g_realH = 0;
bool g_fit = false;

int* Field(uint32_t offset) {
    auto r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    return r ? reinterpret_cast<int*>(r + offset) : nullptr;
}

void Enter() {
    int *w = Field(kWidth), *h = Field(kHeight);
    if (!w || !h || *w <= 0 || *h <= 0 || graphics::VirtualScreenOn()) return;
    int vw = *w, vh = *h;
    if (*w * 3 > *h * 4) vw = (*h * 4 + 1) / 3;          // wider than 4:3: pillarbox
    else if (*w * 3 < *h * 4) vh = (*w * 3 + 2) / 4;     // narrower (5:4): letterbox
    if (vw == *w && vh == *h) return;                     // already 4:3 - nothing to map
    g_realW = *w;
    g_realH = *h;
    *w = vw;
    *h = vh;
    graphics::SetVirtualScreen(true, g_realW, g_realH, vw, vh);
}

void Leave() {
    if (!graphics::VirtualScreenOn()) return;
    if (int *w = Field(kWidth), *h = Field(kHeight); w && h) {
        *w = g_realW;
        *h = g_realH;
    }
    graphics::SetVirtualScreen(false);
}

void __cdecl LoadFrontEnd() {
    if (g_fit) Enter();
    reinterpret_cast<void(__cdecl*)()>(kLoadFrontEnd)();
    if (features::g_afterFrontEndLoad) features::g_afterFrontEndLoad();
}

int __cdecl LevelInit(const char* name) {
    Leave();
    return reinterpret_cast<int(__cdecl*)(const char*)>(kLevelInit)(name);
}

void __cdecl ShowLoading(const char* picture, uint32_t a, uint32_t fadeIn, uint32_t withBar, uint32_t barValue) {
    Enter();
    reinterpret_cast<void(__cdecl*)(const char*, uint32_t, uint32_t, uint32_t, uint32_t)>(kShowLoading)(
        picture, a, fadeIn, withBar, barValue);
    Leave();
}
bool g_loadingFrame = false;

constexpr uint32_t kProgress = 0x42D2C9;
constexpr uint32_t kProgressPushes[] = {0x42D55D, 0x4316DE, 0x431FAE};
void __cdecl Progress(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    const bool enter = !graphics::VirtualScreenOn();
    if (enter) Enter();
    reinterpret_cast<void(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t)>(kProgress)(a, b, c, d, e);
    if (enter) Leave();
}
}  // namespace

void features::ApplyFrontEnd() {
    // the hook also serves the dev StartLevel (devbg.cpp), so it is installed whatever the setting
    g_fit = settings::Get().hudScaling != 0;   // the same switch as the HUD: "fit the 2D to the screen"
    if (!patch::HookCall(kLoadFrontEndCall, reinterpret_cast<void*>(&LoadFrontEnd), kLoadFrontEnd)) return;
    if (g_fit && patch::HookCall(kLevelInitCall, reinterpret_cast<void*>(&LevelInit), kLevelInit) &&
        patch::HookCall(kShowLoadingCall, reinterpret_cast<void*>(&ShowLoading), kShowLoading)) {
        uint8_t push[5] = {0x68};
        const auto fn = reinterpret_cast<uint32_t>(&Progress);
        std::memcpy(push + 1, &fn, 4);
        static const uint8_t kOld[] = {0x68, 0xC9, 0xD2, 0x42, 0x00};
        for (uint32_t site : kProgressPushes)
            if (patch::Matches(site, kOld, sizeof kOld)) patch::Write(site, push, sizeof push);
        dslog::Write("[ok]   Front-end: laid out at 4:3, centred on the screen (menus, loading screens)");
    }
}

// the "press any key" frame after a level load (frame.cpp) runs on the virtual screen too
void features::LoadingFrameBegin() {
    if (!g_fit || graphics::VirtualScreenOn()) return;
    Enter();
    g_loadingFrame = graphics::VirtualScreenOn();
}
void features::LoadingFrameEnd() {
    if (!g_loadingFrame) return;
    g_loadingFrame = false;
    Leave();
}

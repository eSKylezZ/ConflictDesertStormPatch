// Per-frame hook. The message loop (FUN_00431365) runs one frame per pass while the device is fine: call 0x42C74D
// (at 0x43147E, while [0x70D5C0] is set) or the frame dispatcher FUN_0042fc03 (at 0x431485; loop mode [0x7052D8]:
// 0 front end, 1 game, 2 load / pick the next level, 3 film, 4 ...). Both calls go through OnFrame first.
#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/graphics.h"

namespace {
constexpr uint32_t kDispatch = 0x42FC03, kDispatchCall = 0x431485, kAltFrame = 0x42C74D, kAltFrameCall = 0x43147E;

void OnFrame() {
    graphics::Rehook();
    features::OnFrameHud();
#ifndef DS_DIST
    // loop mode [0x7052D8] and front-end menu state [0x7126A8] (0x16 = no menu in play): each change logged
    static int lastMode = -1, lastState = -1;
    const int mode = *reinterpret_cast<const uint8_t*>(0x7052D8), state = *reinterpret_cast<const int*>(0x7126A8);
    if (mode != lastMode || state != lastState) {
        lastMode = mode;
        lastState = state;
        dslog::Write("[dev] frame: loop mode %d, menu state 0x%X", mode, state);
    }
#endif
}

void __cdecl Dispatch() {
    OnFrame();
    reinterpret_cast<void(__cdecl*)()>(kDispatch)();
}

void __cdecl AltFrame() {
    OnFrame();
    features::LoadingFrameBegin();   // the post-load "press any key" screen: 4:3, centred (frontend.cpp)
    reinterpret_cast<void(__cdecl*)()>(kAltFrame)();
    features::LoadingFrameEnd();
}
}  // namespace

void features::ApplyFrameHook() {
    if (patch::HookCall(kDispatchCall, reinterpret_cast<void*>(&Dispatch), kDispatch) &&
        patch::HookCall(kAltFrameCall, reinterpret_cast<void*>(&AltFrame), kAltFrame))
        dslog::Write("[ok]   Frame hook: message loop 0x431485 / 0x43147E");
    else
        dslog::Write("[fail] Frame hook: unexpected bytes in the message loop");
}

// Per-frame hook. WinMain's message loop (FUN_00437A20) runs one game frame per pass while the device is fine:
// 0x437BD7 call FUN_004360C0 (the game frame) or, while [0x727BA8] is set, 0x437BD0 call FUN_0042ED50 (another loop).
// Both calls go through OnFrame first. (The frame cap will live here too.)
#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/graphics.h"

namespace {
using FrameFn = void(__cdecl*)();

void OnFrame() {
    graphics::OnFrame();
    features::OnFrameHud();
}

void __cdecl GameFrame() {
    OnFrame();
    reinterpret_cast<FrameFn>(0x4360C0)();
}
void __cdecl OtherFrame() {
    OnFrame();
    reinterpret_cast<FrameFn>(0x42ED50)();
}
}  // namespace

void features::ApplyFrameHook() {
    const bool a = patch::HookCall(0x437BD7, reinterpret_cast<void*>(&GameFrame), 0x4360C0);
    const bool b = patch::HookCall(0x437BD0, reinterpret_cast<void*>(&OtherFrame), 0x42ED50);
    dslog::Write(a && b ? "[ok]   Frame hook: game frame calls 0x437BD7 / 0x437BD0" : "[fail] Frame hook: unexpected call sites");
}

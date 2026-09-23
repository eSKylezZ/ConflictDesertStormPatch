#pragma once
#include <windows.h>

#include <cstdint>

// Menu-level gamepad reading for our own UI (launcher), independent of the game's DirectInput code.
// Xbox / XInput pads through XInput, DualShock 4 / DualSense through DirectInput (Sony VID 0x054C).
// All connected pads are OR-ed together, so a pad visible through both APIs (Steam Input) acts once.
namespace gamepad {
enum Button : uint32_t {
    kUp = 1u << 0,
    kDown = 1u << 1,
    kLeft = 1u << 2,
    kRight = 1u << 3,
    kAccept = 1u << 4,  // A / Cross
    kBack = 1u << 5,    // B / Circle
    kStart = 1u << 6,   // Start / Options
    kAlt = 1u << 7,     // Y / Triangle
};
enum class Type { None, Xbox, PlayStation };

struct State {
    uint32_t buttons = 0;
    Type type = Type::None;  // pad that was used last (or the first one found)
};

bool Open(HWND owner);  // DirectInput needs a window for its cooperative level
void Close();
State Poll();
}  // namespace gamepad

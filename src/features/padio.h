#pragma once
#include <windows.h>

#include <cstdint>

// Direct pad I/O next to the game's DirectInput: XInput for Xbox-type pads, HID reports for DualSense / DualShock 4
// (input over USB and Bluetooth, rumble, light bar, player LEDs, adaptive triggers), Windows.Gaming.Input for the
// Xbox One / Series trigger motors. See padio.cpp.
namespace padio {
// DIJOYSTATE2 (the part the game uses): range +-128 on the four axes the game reads (lX/lY left, lZ/lRz right).
struct JoyState {
    LONG x, y, z, rx, ry, rz;
    LONG slider[2];
    DWORD pov[4];
    BYTE buttons[128];
};

// Fills `s` for the game's joystick j in the game's button numbering (0 Triangle 1 Circle 2 Cross 3 Square 4 L2
// 5 R2 6 L1 7 R1 8 Select 9 L3 10 R3 11 Start 12-15 D-pad) with sticks already dead-zoned, if the pad is read
// directly (XInput / HID); false = use the DirectInput state.
bool Fill(int joystick, JoyState& s);

// Once per frame (game thread): the game's rumble table, players and layouts -> the pads' outputs.
void Update();
}  // namespace padio

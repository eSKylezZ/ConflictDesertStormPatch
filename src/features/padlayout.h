#pragma once
#include <cstdint>

// Per-player controller layouts (CONTROLLER screen). A layout is the pad part of the game's bindings: for each of
// the 80 actions up to 3 entries of type 1 (pad axis) or 2 (pad button), in the game's own codes (binding entry =
// type << 16 | code, -1 = none; buttons in the game's PS2 numbering, see controller.cpp; axis codes 3 per axis:
// negative, positive, whole - axis 0 left Y, 1 left X, 2 right Y, 3 right X).
// Keyboard and mouse entries are the game's own (KEY ASSIGNMENT, current_key) and are left alone.
namespace padlayout {
constexpr int kActions = 80, kSlots = 3, kPlayers = 4;
// Default: modern (fire R2 / RT, orders L2 / LT, targeting on the bumpers); Classic: the game's own default.key
// (fire R1); Southpaw: Default with the sticks swapped; Custom: edited on the CONTROLLER screen.
enum class Preset : uint32_t { Default, Classic, Southpaw, Custom, Count };

struct Layout {
    int32_t entry[kActions][kSlots];
};

// Which screen tab an action belongs to (swaps only happen inside one context).
enum class Context { OnFoot, Orders, Inventory, None };
Context ContextOf(int action);

Layout Make(Preset preset);  // the preset applied to default.key's pad entries
Preset PresetOf(int player);
const Layout& Get(int player);  // the player's current layout
// Stores the player's layout (registry, Enhancements\PadLayout<n>) and writes it into the game's bindings.
void Set(int player, Preset preset, const Layout& layout);

int ButtonOf(const Layout& l, int action);  // first pad button of the action, -1 = none
// Puts `action` on `button`; whatever in the same context used that button gets the action's old button.
void Assign(Layout& l, int action, int button);

// Writes every player's layout into the game's binding sets (level load, after a change).
void ApplyToGame();

// Per-player options saved with the layout: vibration (rumble motors, Xbox trigger motors; PadVibration<n>) and
// DualSense adaptive triggers (PadTriggers<n>), both on by default.
bool Vibration(int player);
void SetVibration(int player, bool on);
bool AdaptiveTriggers(int player);
void SetAdaptiveTriggers(int player, bool on);
}  // namespace padlayout

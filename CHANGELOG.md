# Changelog

## 1.0.0 – 2026-09-26

Initial release for the Steam and GOG versions of Conflict: Desert Storm (PC). One `dinput8.dll`, placed next to
`DesertStorm.exe`; the game files are not modified.

### Added

**Display**

- Widescreen resolutions in the launcher (16:9, 16:10, 21:9 – whatever the monitor supports)
- HUD, text and menus scaled to the resolution, with an adjustable HUD size
- Sharper text: the game's fonts are redrawn at the screen resolution instead of being stretched
- HUD laid out for wide screens: the weapon display mirrors the soldier panel, menu prompts sit at both screen edges
- Fullscreen, windowed and borderless window modes
- Anti-aliasing (2x / 4x / 8x MSAA) and anisotropic filtering (up to 16x)

**Frame rate**

- Frame rate limit, by default the monitor's refresh rate (max 240 fps); "Unlimited" stops at 500 fps

**Split-screen co-op (2–4 players)**

- CO-OP entry in the main menu with an Xbox-style join screen: each player joins with their own controller or
  keyboard and mouse, and every device only controls its own player
- Co-operative campaign menu: start the campaign or load a co-op save
- Co-op campaign progress: finishing a mission unlocks the next, finished missions can be replayed; 3–4 players start
  at Mission 3, as on the Xbox
- Co-op saves named with the player count ("Co-op 2P Rescue 1"); single-player and co-op saves are listed separately
- Top/bottom or side-by-side layout with divider lines, and a HUD, soldier panels, inventory and give menu for each
  player in their own view
- Every player can open the pause menu with their own controller

**Controllers**

- Xbox controllers (XInput) and DualShock 4 / DualSense in game – buttons, D-pad, both sticks and triggers, over USB
  and Bluetooth
- Rumble per weapon on Xbox, DualShock 4 and DualSense; nothing for throwing grenades or placing C4 and mines
- Xbox trigger rumble on the fire trigger, sized to the weapon
- DualSense adaptive triggers matched to the weapon in hand, including mounted guns and vehicle weapons
- Full pull to fire: a shot only fires once the trigger is pulled through
- DualSense light bar and player LEDs in each player's colour; touchpad click opens the objectives
- Button icons in menu prompts, tutorial hints and actions (Mount, Unlock, Open Door …) for the controller in use
  (PlayStation, Xbox or keyboard)
- Inventory hints: use a MediKit on yourself, change fire mode or toggle night vision without equipping the item
- In-game menus follow the device in use (mouse cursor hidden while a controller is used)
- Launcher and its settings usable with a controller, with button hints

**Controls menu (Options > CONTROLS)**

- KEYBOARD & MOUSE: the game's key assignment and mouse options, plus four saved key profiles (a keyboard player in
  co-op can pick one on the join screen)
- CONTROLLER: a layout for each player on a PS5 or Xbox Series controller picture, with presets DEFAULT (fire on
  R2 / RT), CLASSIC, SOUTHPAW and CUSTOM (rebind any button); vibration and adaptive triggers can be turned off per
  player

**Quality of life**

- Skip the intro (publisher logos and opening film)
- Skip mission cutscenes automatically (optional; the briefing is still shown)
- Discord status with the current mission, single player or split screen, and elapsed time (can be turned off)
- Mouse acceleration off by default (can still be turned on in Mouse Options)
- New settings in the launcher (Settings > Enhancements column): frame rate limit, display mode, intro and
  cutscene skipping, anti-aliasing, texture filtering, HUD scaling and size, controller support and stick deadzone,
  Discord status, split-screen layout

### Fixed (original game)

- The game ran too fast at high frame rates ("turbo" mode) – the game clock lost precision the longer the PC was on
- Soldiers stayed stuck in the helicopter in the opening film at high frame rates
- Tutorial hints only named keyboard keys, even when playing with a controller
- Only the first connected controller worked in the menus
- Gunfire and nearby sounds got louder when aiming or zooming (scopes, binoculars, mounted guns)
- The launcher's keyboard Tab/arrow order didn't follow its layout
- Mouse acceleration was always switched on

### Known issues

- Faint shadows can show through some walls, as in the original game (only soldiers and objects cast shadows; walls
  don't block the light). A proper fix is planned for version 2
- 4-player co-op with real controllers has not had a full play-through yet
- A few HUD elements are placed slightly wide on 16:9 screens
- Windowed and borderless modes are untested on multi-monitor setups

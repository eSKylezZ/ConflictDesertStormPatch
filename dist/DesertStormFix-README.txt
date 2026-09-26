DesertStormFix - enhancement patch for Conflict: Desert Storm (PC)
====================================================================

Works with the Steam and GOG versions.

Features
- Widescreen resolutions in the launcher
- Display mode: fullscreen, windowed or borderless window
- Skip the intro (publisher logos and opening film) and, optionally, the
  mission cutscenes
- Anti-aliasing (2x / 4x / 8x MSAA) and anisotropic texture filtering
  (up to 16x), which the original settings don't offer
- HUD, text and menus scaled to your resolution, with sharper text
- DualShock 4 / DualSense and Xbox controller support (buttons, D-pad,
  both sticks and triggers), with rumble on both
- DualSense: adaptive triggers (a firm "gun trigger" on the fire trigger, light
  resistance for aim mode), light bar and player lights show which player
  the pad belongs to, touchpad click opens the objectives
- Xbox One / Series controllers: trigger rumble on the fire trigger
- Options -> CONTROLS: a controller screen that shows what every button does
  on a PlayStation or Xbox controller, layouts per player (presets or your
  own), and saved keyboard & mouse profiles
- In-game menus follow the device you use: with a controller the mouse
  cursor is hidden and the prompts name the pad buttons ("Cross: Select")
  instead of the keys ("Return: Select")
- Correct game speed at high frame rates, frame rate capped to your
  monitor's refresh rate (max 240 fps)
- Characters no longer get stuck at high frame rates (for example the
  soldiers who stayed inside the helicopter in the opening film instead of
  jumping out)
- Your own gunshots no longer get louder when you aim or zoom (scopes,
  binoculars, mounted guns), and nearby sounds no longer jump closer
- Inventory hints: the item bar shows what the use button does without
  equipping the item - a MediKit heals yourself, night vision goggles toggle,
  weapons with several fire modes change mode
- Mouse acceleration is off by default (turn it on in Options -> Controls ->
  Keyboard & Mouse -> Mouse Options if you prefer it)
- Local co-op for 2-4 players in split screen (main menu -> CO-OP), with
  divider lines between the views
- Discord status: shows the mission you are playing (for example
  "Mission 4: Desert Watch"), single player or split screen, and the time
  since the mission started

Settings
  Start the game, click "Settings" in the launcher. The "Enhancements"
  section on the right has the frame rate limit, display mode, intro and
  cutscene skipping, anti-aliasing, texture filtering, HUD scaling and size,
  controller support and stick deadzone, the Discord status and the
  split-screen layout. Press OK to save.
  The launcher and settings can be used with a controller (Xbox, DualShock 4,
  DualSense): the button hints appear at the bottom once a pad is found.

Display mode
  Windowed: the game in a window at the resolution you chose (made smaller
  if it does not fit on the screen). Borderless window: the game fills the
  monitor without changing its display mode, so switching to other windows
  is instant. For the sharpest picture in borderless, choose your desktop
  resolution; other resolutions are stretched to fill the screen.
  While the game window is active the mouse stays inside it; press Alt+Tab
  or the Windows key to get it back.

Intro and cutscenes
  "Skip the intro" starts straight in the main menu. The opening film still
  plays in the menu when you leave it idle, like in the original game.
  "Skip mission cutscenes automatically" ends every mission cutscene as it
  starts; the mission briefing is still shown. Without it you can skip any
  cutscene yourself with Esc, the left mouse button or Triangle on a
  DualShock 4 / DualSense.

Anti-aliasing and texture filtering
  Anti-aliasing smooths the jagged edges of buildings, rocks and soldiers.
  If your graphics card can't do the chosen level, the highest level it
  supports is used (or none). Anisotropic filtering keeps the ground and
  walls sharp into the distance instead of blurring them; it is on (16x)
  by default. "Game default" turns it off. Both take effect the next time
  you press Play.

Discord status
  Turn it off with "Show the game in my Discord status" in the settings.
  The patch only talks to the Discord app running on your own PC (the same
  way other games do); nothing is sent anywhere else. If Discord is not
  running, nothing happens.

Co-op (local split screen, 2-4 players)
  Main menu -> CO-OP. Each player presses Cross (PlayStation), A (Xbox) or
  Space (keyboard & mouse) to join; players are numbered in the order they
  join. Left/Right changes the button icons a player sees, Circle / B /
  Backspace leaves. With two or more players, Options / Start / Enter
  continues to Difficulty, Uniform and the co-op mission list.
  Co-op has its own progress: the first mission is open (Mission 3 with 3-4
  players - Connors and Jones join the squad there), finishing a mission
  unlocks the next, and finished missions can be replayed. Missions 1-2 are
  for 2 players; with more players they are shown greyed out.
  The split layout (top/bottom or side by side) is set in the launcher's
  Settings under "Split screen".
  A keyboard player can pick a saved keyboard & mouse profile with
  Left/Right on their row (see Controls below).
  Each player uses the controller layout set for their player number.

Controllers
  Xbox controllers (and other XInput pads) are read like in modern games, so
  both triggers work at the same time. DualSense and DualShock 4 work over USB
  and Bluetooth; rumble, light bar, player lights and the DualSense's
  adaptive triggers need no extra software.
  The adaptive triggers and trigger rumble follow your layout: they sit on
  whichever trigger fires (R2 / RT unless you choose CLASSIC).
  Vibration and the adaptive triggers can be switched off per player on the
  CONTROLLER screen.

Controls (Options -> CONTROLS)
  KEYBOARD & MOUSE: the game's Key Assignment and Mouse Options, plus
  PROFILES - four slots for your keys. Select a slot to load it, S (or
  Square / X) saves your current keys into it, Delete (or Triangle / Y)
  clears it. Profiles stay on this PC.
  CONTROLLER: a picture of the controller with a line from each button to
  what it does. At the top choose the player (1-4), the layout and what the
  buttons do on foot, in the Orders menu or in the Inventory, whether the
  controller vibrates and whether a DualSense uses its adaptive triggers. Layouts: DEFAULT (fire on R2 / RT, orders on
  L2 / LT, targeting on the bumpers), CLASSIC (the original game's: fire on
  R1 / RB), SOUTHPAW (DEFAULT with the sticks swapped). To change a button, select the action, press
  Cross / A / Enter and then press the new button; whatever used that button
  swaps with it and the layout becomes CUSTOM. Square / X / Delete resets the
  player to DEFAULT, Triangle / Y / Tab switches between the PlayStation and
  Xbox picture. Layouts are saved per player and used from the next mission.

Install
  Copy dinput8.dll into the game folder, next to DesertStorm.exe.
    Steam: ...\steamapps\common\conflict_desert_storm
    GOG:   ...\GOG Games\Conflict - Desert Storm
  The game files are not modified.

Uninstall
  Delete dinput8.dll from the game folder.

Credits
  Controller button icons: "Input Prompts" by Kenney (www.kenney.nl), CC0.
  Controller pictures: "PS5 Button Icons and Controls" and "Xbox Series
  Button Icons and Controls" by Zacksly (https://zacksly.itch.io), licensed
  under CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/). Modified:
  cropped and scaled.

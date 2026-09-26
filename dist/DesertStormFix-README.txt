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
- HUD, text and menus scaled to your resolution
- DualShock 4 / DualSense controller support (buttons + D-pad)
- In-game menus follow the device you use: with a controller the mouse
  cursor is hidden and the prompts name the pad buttons ("Cross: Select")
  instead of the keys ("Return: Select")
- Correct game speed at high frame rates, frame rate capped to your
  monitor's refresh rate (max 240 fps)
- Characters no longer get stuck at high frame rates (for example the
  soldiers who stayed inside the helicopter in the opening film instead of
  jumping out)
- Local co-op for 2-4 players in split screen (main menu -> CO-OP)
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

Install
  Copy dinput8.dll into the game folder, next to DesertStorm.exe.
    Steam: ...\steamapps\common\conflict_desert_storm
    GOG:   ...\GOG Games\Conflict - Desert Storm
  The game files are not modified.

Uninstall
  Delete dinput8.dll from the game folder.

Credits
  Controller button icons: "Input Prompts" by Kenney (www.kenney.nl), CC0.

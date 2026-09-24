DesertStormFix - enhancement patch for Conflict: Desert Storm (PC)
====================================================================

Works with the Steam and GOG versions.

Features
- Widescreen resolutions in the launcher
- HUD, text and menus scaled to your resolution
- DualShock 4 / DualSense controller support (buttons + D-pad)
- In-game menus follow the device you use: with a controller the mouse
  cursor is hidden and the prompts name the pad buttons ("Cross: Select")
  instead of the keys ("Return: Select")
- Correct game speed at high frame rates, frame rate capped to your
  monitor's refresh rate (max 240 fps)
- Discord status: shows the mission you are playing (for example
  "Mission 4: Desert Watch"), single player or split screen, and the time
  since the mission started

Settings
  Start the game, click "Settings" in the launcher. The "Enhancements"
  section on the right has the frame rate limit, HUD scaling and size,
  controller support and stick deadzone, the Discord status and the
  split-screen layout. Press OK to save.
  The launcher and settings can be used with a controller (Xbox, DualShock 4,
  DualSense): the button hints appear at the bottom once a pad is found.

Discord status
  Turn it off with "Show the game in my Discord status" in the settings.
  The patch only talks to the Discord app running on your own PC (the same
  way other games do); nothing is sent anywhere else. If Discord is not
  running, nothing happens.

Split screen
  Local split screen for 2-4 players is in development. The "Split screen"
  layout setting (horizontal: views stacked top/bottom, vertical: side by
  side) is already there, but it has no effect yet - there is no way to
  start a split-screen game in this version.

Install
  Copy dinput8.dll into the game folder, next to DesertStorm.exe.
    Steam: ...\steamapps\common\conflict_desert_storm
    GOG:   ...\GOG Games\Conflict - Desert Storm
  The game files are not modified.

Uninstall
  Delete dinput8.dll from the game folder.

Credits
  Controller button icons: "Input Prompts" by Kenney (www.kenney.nl), CC0.

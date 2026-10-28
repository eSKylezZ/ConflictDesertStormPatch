GreatEscapeFix - enhancement patch for The Great Escape (PC, 2003)
====================================================================

What it does
------------
- Widescreen: the launcher's Settings... list shows every display mode your graphics card offers (16:9, 16:10,
  1440p, 4K ...), not only 640x480 / 800x600 / 1024x768 / 1280x1024. In missions the 3D view fills the whole
  widescreen picture without stretching.
- Menus and HUD scaled with the resolution: at 1920x1080 / 2560x1440 they look as they did at 800x600 instead of
  shrinking (the mouse still lines up). The objectives notebook keeps its original 4:3 layout,
  scaled to fill the screen's height.
- The main menu's background film fills a widescreen (its top and bottom are cropped a little) instead of showing
  black bars at the sides. The story films are widescreen already.
- Frame cap: the game ran as fast as it could (over 1000 fps); it now runs at your monitor's refresh rate (max 240).
- Windowed mode: the launcher's Settings... dialog now shows its Windowed box. Ticked, the game runs in a window of the
  chosen resolution, centred on the screen; at your desktop's own resolution it is a borderless full-screen window.
- Mods folder: put files in  <game folder>\Mods\<any mod name>\  and the game uses them instead of its own files of
  the same name (textures, models, text tables ...), or as new files. Mods load in folder-name order; when two mods
  have the same file, the later one wins. A folder whose name starts with _ is switched off.
  What your mods did is written to  Mods\GreatEscapeFix-report.txt  every time the game starts.
- MODS on the main menu: switch each mod in the Mods folder on or off, then choose APPLY. Some files are only read
  when the game starts - restart the game after applying to be sure every change is used.
- PlayStation and Xbox controllers: DualSense, DualShock 4 and Xbox pads work with the right button layout, the D-pad
  and both sticks. Fire is on R2 / RT by default (OPTIONS > CONTROLS > CONTROLLER > FIRE ON THE TRIGGER = OFF gives
  the game's own layout, fire on R1 / RB). Vibration, DualSense adaptive triggers and light bar, pads plugged in or
  pulled out while the game runs (pulling out player 1's pad in a mission pauses the game).
- The game follows the device you use: pick up the pad and the menus, the button prompts and the tutorial texts show
  pad buttons and the mouse pointer hides; touch the keyboard or mouse and they come back.
- The launcher, its Settings... and Advanced dialogs work with a pad, with button hints for the pad in use.
- Action prompts: "[button] Pick up fire extinguisher", "Hold [button] Take down" ... near the bottom of the screen in
  missions, with the button of the device you use (OPTIONS > CONTROLS > CONTROLLER > ACTION PROMPTS).
- Sharper menu and HUD text at high resolutions.
- OPTIONS > CONTROLS > CONTROLLER: controller support, fire button, stick deadzone, vibration, adaptive triggers, and
  SHOW BUTTON LAYOUT (a picture of your pad with what each button does).
- OPTIONS > GREATESCAPEFIX: menu & HUD scaling, HUD size, frame cap, frame limit, films filling the screen, widescreen
  modes (settings marked * apply after a restart).
- Custom missions: Lua missions and campaigns from mods start from CUSTOM MISSIONS on the main menu (shown when a mod
  has any).

Install
-------
Copy everything from the zip into the game folder (next to GreatEscape.exe): dinput8.dll and the PSE folder.
It needs GreatEscape.exe 1.0 without the disc check. The CD exe with SafeDisc doesn't run on current Windows anyway.

Uninstall
---------
Delete dinput8.dll and the PSE folder from the game folder.

Built on the Pivotal Script Extender (PSE), which other plugins can use too: PSE\Plugins\.

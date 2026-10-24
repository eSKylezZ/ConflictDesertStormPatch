# The Great Escape – game layer `pse_tge` and `pse.game.tge` (version 1)

The Great Escape (Pivotal Games, 2003) runs on the same engine as Conflict: Desert Storm, between DS1 and DS2: D3D8 and
a dinput8 import like DS1, Files.cpp / Catalog.cpp and the texture loader like DS2. PSE runs on it with the unchanged
shim (`dinput8.dll`) and `pse_loader.dll`, plus the game layer `PSE\Games\pse_tge.dll` (`pse/games/tge/`) and the host
plugin GreatEscapeFix (`PSE\Plugins\tgefix.dll`, `games/tge/`). Game id `tge`. Built and live-tested 2026-10-23.

## What runs on TGE

- **Exe**: `GreatEscape.exe` 1.0 without the disc check (SHA-256 `50F4A87E…702CDF`, PE time stamp `0x3F128EF0`). Its
  entry point is a no-CD stub (section `.mythoep`) that fills SafeDisc's import slots with `GetProcAddress` and jumps
  to the original entry `0x51FC72`; `.text` is plain on disk. The shim takes the stub's entry, so PSE starts before the
  stub runs: the layer uses only the normal import table (d3d8, dinput8). The retail SafeDisc exe (same time stamp,
  encrypted code) is accepted by the time stamp, but every hook checks the bytes it replaces, so nothing would be
  written over its encrypted code; it doesn't start on current Windows anyway (SafeDisc's driver).
- **Core events and messages**: the same as DS1 / DS2 (frame, render, level load / unload, front end ready, D3D8 device
  created / reset / lost, EndScene, Present, exit), so a plugin written against them runs unchanged.
- **Not yet**: saves (no `SAVE_*` / `LOAD_*` messages, no co-save `.pse`), `PSE_MSG_NEW_GAME`, the PLUGINS menu (it is
  DesertStormFix's).

## Getting it

```c
#include <pse/pse_game_tge.h>

const PSEGameTgeV1* game = (const PSEGameTgeV1*)pse->Query(pse->plugin, PSE_GAME_TGE_INTERFACE, 1);
if (!game || game->size < sizeof(PSEGameTgeV1)) return 0;   /* not The Great Escape */
```

Served by the built-in plugin `pse.game.tge` 1.0.0 (CLIENT_ONLY): a manifest can say `requires = pse.game.tge >= 1.0`
or `games = tge`.

## Functions

| Function | Returns |
| --- | --- |
| `DispatchMode()` | u8 `[0x66A69C]`: 0 = a level runs (Game_Update), 1 = a level load is pending (the whole load is one frame), 2..7 = films and other screens |
| `LevelTableIndex()` | i32 `[0x7DDAEC]`: the level-table entry being / last loaded, -1 = a name from the name buffer `0x7DDAFC` |
| `LevelName(buf, len)` | the level the layer saw load last: `"FE"` (front end), `"FMV02"`, `"Logo1"`, `"BRF01"`, `"Level01.dll"` …; `""` before the first load and after an unload |
| `LevelKind()` | `PSETgeLevelKind`: NONE, FRONTEND (`"FE"`), MISSION (`*.dll`), SCREEN (films, briefings, logos, attract loop, language select) |
| `GameWindow()` | HWND `[0x7DC238]` (class "The Great Escape"); any thread |
| `ExeSha256()`, `ExeKnown()` | the running exe's SHA-256 (LAA flag cleared) / 1 = the build the addresses come from; any thread |

## Events and messages

Hooks owned by `pse.game.tge`, installed by `PSEGame_Init` through the broker (`pse/games/tge/tge_events.cpp`).
INNER = `PSE_TGE_LAYER_PRIORITY_INNER` (-1000), OUTER = `PSE_TGE_LAYER_PRIORITY_OUTER` (1000).

| Site | Priority | Sends |
| --- | --- | --- |
| `0x5D5988` call Main_FrameDispatch `0x5D5810` (message loop) | INNER | `PSE_MSG_FRONT_END_READY` (first running frame after `"FE"` loaded), `pse::Frame` (tasks + `PSE_EVENT_FRAME`) |
| `0x5D5692` call Render_Frame `0x5D18D0` (cdecl int) | INNER | `PSE_EVENT_PRE_RENDER` / `POST_RENDER` |
| `0x5D0DE0` Screen_Load (entry, 6-byte prologue) | OUTER | `LEVEL_UNLOAD` of the previous level, `LEVEL_LOAD_BEGIN` / `_END` with the entry's name |
| `0x5D1DE0` Level_Init(name) (entry, 9 bytes) | OUTER | `LEVEL_LOAD_BEGIN` / `_END` with the mission's name (`"LevelA.dll"`) |
| `0x5CEE10` Level_Unload (entry, 6 bytes) | OUTER | `PSE_EVENT_LEVEL_UNLOAD` (the loaded level's name) |
| `0x5D0089` WM_DESTROY: call App_Shutdown `0x5CCA80` | OUTER | `LEVEL_UNLOAD`, `PSE_MSG_EXIT`, plugins unloaded (pse.host Shutdown) |
| `0x5F52A4` IAT d3d8!Direct3DCreate8 (thunk `0x4FE930`) | OUTER | → IDirect3D8::CreateDevice (vtable 15, OUTER): `PSE_MSG_DEVICE_CREATED`; device Reset (14, OUTER) `DEVICE_RESET`, TestCooperativeLevel (3) `DEVICE_LOST`, EndScene (35) / Present (15) (INNER) `PSE_EVENT_END_SCENE` / `PRESENT` |

How the game loads levels: everything – language select, logos, films, briefings, the front end, missions – is an entry
of one **level table** at `0x666B00` (0x28-byte entries: +4 name, +8 type: 0 front end `FE`, 1 attract `Att`, 3 mission
DLL, 4 film / briefing, 5 logo, 6 / 7 language select). Dispatch mode 1 runs Level_LoadDispatch `0x5D5710`, which loads
entry `[0x7DDAEC]` by its type: types 0 / 1 / 4 / 5 through Screen_Load, type 3 (or a name in `0x7DDAFC`) through
Mission_Load `0x5D43C0` → Level_Init. Both unload the previous level first; Screen_Load does it inside itself, after
the layer's entry hook, so the layer raises that unload in front and its unload hook stays quiet for that load.

Seen in a live run (2026-10-23): `Legal`, `Logo1` … `Logo6`, `FE`, then New Game → `FMV02`, `Chpt1`, `FMV1a`,
`LevelA.dll`.

### Log

Without a host plugin's sink every line goes to `PSE\Logs\loader.log`; GreatEscapeFix's dev build also writes them to
`GreatEscapeFix.log` next to the exe.

## IDs

`pse/ids/tge.ids.txt` (41 ids, database 0 = nothing published yet: names may still change): the functions, sites and
globals above plus the file system (File_Resolve `0x4C44D0`, File_Open `0x4C60E0`, File_InArchive `0x4C5DF0`, the
texture loader, the Global Heap) and the launcher. List: [ids-tge.md](ids-tge.md). Archive names: TGE's `.dat`
archives hash the **lower-cased** file name (DS1's upper-cased) with the same LFSR (`ge_logo0.dds` in catalog.dat).

## Build

From the repo root: `cmake --preset tge && cmake --build --preset tge` → `build-tge/tree/RelWithDebInfo/` (shim, loader,
layer, `tgefix` + manifest, PDBs); `TGE_GAME_DIR` copies the tree into an install. Release package:
`cmake --preset tge-dist && cmake --build --preset tge-dist` → `build-tge-dist/package/` +
`GreatEscapeFix-<ver>.zip`. Or from `games/tge` with its own presets `x86` / `dist`. Plugins in
`games/tge/plugins/<name>/` (none yet) are built and packaged like DS2's.

## GreatEscapeFix as a host plugin

`tgefix.dll` (`games/tge/src/tgefix.cpp`, manifest `host = true`, `requires = pse.game.tge >= 1.0`; the layer allows
the name `tgefix`). Patching = DesertStormFix's broker-backed `games/ds1/src/core/patch.cpp`; each feature is an
internal plugin. Settings: pse.settings of `tgefix` (`HKCU\Software\PSE\tge\Plugins\tgefix`): `FeatWidescreen` (1),
`FilmsFill` (1), `FeatMods` (1), `HudScaling` (1), `HudScalePercent` (100, 25..400), `FrameCap` (1), `FpsCap` (0 =
monitor refresh, 30..240) – restart, no menu yet; `Windowed` (0) – the launcher's Settings… > Windowed box, applied at
once.

### Widescreen (`tgefix.widescreen`, CLIENT_ONLY)

The launcher's Settings… resolution list (`0x5BE900`, combo `0x3FC`) only adds 640x480 / 800x600 / 1024x768 /
1280x1024 (an inline compare chain). The chain's first compare (`0x5BE99F`) now loads the height and jumps to the
add-to-list code, so every mode the adapter lists in the desktop format shows up; the same whitelist as an uncalled
function (`0x5BE5E0`) answers 1. Live: 24 modes up to 2560x1440 on the dev PC (was 4); the saved selection is kept.
In missions the 3D view fills 16:9 without stretching (the projection follows the viewport).

**Films** (setting `FilmsFill`, default on): every Bink film is drawn by one quad – call `0x499555` → the 2D draw
`0x446F00` (thiscall(renderer, 2, verts, 0x1C), pretransformed FVF 0x144) – sized to the film's frame at the screen's
height. The main menu's background loop is 4:3 (241..1679 x 0..1080 at 1920x1080: black side bars; no 3D is drawn in
the front end). With `FilmsFill` a quad narrower than the screen at full height is zoomed about the screen's centre
until it covers the width (on 16:9 an eighth of the frame goes past the top and the bottom). The story films are
already full width (FMV02: 0..1920 x 31..1049) and are left alone.

### Menu and HUD scaling (`tgefix.hud`, CLIENT_ONLY, `games/tge/src/features/guiscale.cpp`)

The GUI (GUI.cpp, `CGUI*`) is laid out for a 640 x 480 design: `CGUI::Load` `0x4BA870` (thiscall(gui, name) ret 4)
reads the renderer size (+0x40690 / +0x40694) and sets `[0x7AB304]` = W / 640, `[0x7AB300]` = H / 480, `[0x7AB2FC]` =
W − 640, `[0x7AB2F8]` = H − 480; controls are placed from those (`0x4BE930`: flag 0x40 centred – shifted by half the
extra space –, no flag stretched) and keep their pixel size, so at 1440p the menus and HUD were small. Every control
draws inside `CGUIControl::Render` `0x4BE510` (thiscall, 5 args, ret 0x14; virtual in every control class) into the 2D
draw `0x446F00` (fan of FVF 0x144 vertices; its 2D transform call `0x446F1D` → `0x443450`); the GUI's screen rectangle
is `0x4C19F0` = (0, 0, W, H). GreatEscapeFix uses DesertStorm2Fix's virtual screen: while a GUI loads or a control
renders, the renderer size reads as (W / k) x (H / k), and the vertices those controls draw are multiplied by k after
the game's own transform; k = max(1, min(W / 800, H / 600)) x HudScalePercent / 100 (800 x 600 looks as before;
1920x1080 ×1.8, 2560x1440 ×2.4). The menu cursor (image 0x56 drawn at `0x5CCB11` from the cursor globals `0x7A9D20` /
`0x7A9510`, outside any control) is wrapped the same way, and the mouse poll's `ScreenToClient` (`0x4B9205`, see
display) divides by k, so the GUI's hit tests match. Everything else – the 3D world, films, fades (`0x461730`), world
sprites – keeps real pixels. Live (1920x1080 windowed, PrintWindow): main menu, OPTIONS, pause menu, mission HUD;
hovering a menu row highlights that row, the arrow sits on the pointer (±2 px).

**The objectives notebook** (Tab in a mission; `Notebook.gui`) is a 3D book with GUI pages and text (`0x436B00`)
laid onto it, designed for 4:3: at 800x600 the book fills most of the screen and the text sits on its pages. On a
wide screen the game's own layout comes apart (the book follows the height, the page text the 640 x 480 design centred
in the width: at 1920x1080 the text slid off the left page and the map past the book's edge). GreatEscapeFix lays the
notebook out on a centred 4:3 screen of 800 x 600 scaled to the height (k = min(W / 800, H / 600), offset to the
middle): `Notebook.gui` loads for 800 x 600 (its mapping globals are kept and swapped in while it draws), and its
screens draw in that layout. Its screens are the ones its load adds to the GUI's screen list (GUI `[0x7D8EC4]` +0x8C:
+8 array, +0xC count; lookup `0x4BB0C0`): 0x82B9E96B (the book opening), 0x8C949DE9 (Objectives) and the others; the
screen being drawn is GUI +0xA4 (`CGUI::Render` points it at every screen it draws). The book: `0x58CF60` (input ebx =
the book control `[0x7DC0A8]`, no stack arguments) fits the model into the control's rectangle with a projection made
from the rectangle over the renderer size, taking its aspect from the renderer's current-viewport pointer (+0x40670,
getter `0x4413B0`, projection `0x44E540`). Its two callers (`0x58D9EF`, `0x58E91A`) run outside any control render, so
a naked stub (ebx kept) runs them in the notebook layout, with the device viewport and +0x40670 set to the 4:3 part
of the screen. Live (1920x1080, 2560x1440, 800x600): the notebook looks like the original at 800x600 at every size,
and the HUD and pause menu after it are unchanged.

### Windowed / borderless (`tgefix.display`, CLIENT_ONLY, `games/tge/src/features/display.cpp`)

The game has a windowed path but no way in: the Settings… dialog (template 108, procedure `0x5BF8B0` pushed at
`0x5BF904`) hides its Windowed box (`0x401`, `ShowWindow(…, SW_HIDE)` at `0x5BEF34`) and never reads it, and the
launcher's OK writes the adapter's fullscreen flag = 1 (`0x5BF47E`, adapter +0x66C8 = renderer +0x67AC; device
creation `0x44501E` makes a windowed device when it is 0). Also the message loop forced the fullscreen style
0x15800000 every frame the device was fine (`0x5D5945..0x5D5986`), like DS1. GreatEscapeFix: the dialog procedure is
wrapped (box shown + ticked from `Windowed`, read on OK and applied at once – the device is made after the launcher);
windowed = the OK immediate `0x5BF484` is 0, the loop's style check is skipped (cursor hidden once), the post-launcher
`SetWindowPos` (`0x5D0A80`) places a captioned window with the mode's client size centred on its monitor's work area
(shrunk keeping the aspect when it doesn't fit), not topmost; a mode as big as the monitor = a borderless window over
it. The mouse poll's `ScreenToClient` (`mov ebx, [ScreenToClient]` at `0x4B9205` → `ScreenToGame`) scales client
coordinates to the back buffer when they differ. **Fullscreen start**: the game never activates its window (both placement calls - `0x5D0A80` windowed, `0x5D0C18` fullscreen, flags 0x248 - leave activation alone), so when another program took the front while the device was made (seen: VS Code) the fullscreen device got no screen - an 800 x 600 black window over the desktop, the game paused as inactive. Both calls are hooked and bring the window to the front (DS1's `BringToFront`: AttachThreadInput + SetForegroundWindow, one Alt tap), again on `PSE_MSG_DEVICE_CREATED`, and for 5 s a thread retries while the game's active flag `[0x7DC2FC]` is 0 and the player hasn't used the keyboard / mouse. An overlay holding the foreground while the game is active (AMD's `amdow.exe`) is left alone. **Fullscreen window size**: the start-up placement sizes the window from the device's mode index before the launcher's choice is applied (`[device +0x13A8]` still 3 = 800x600 16-bit → 816 x 639), and D3D8 on current Windows presents into the window, so any other mode showed only its top-left 800 x 600 over the desktop; on `PSE_MSG_DEVICE_CREATED` a fullscreen device's window is moved to its monitor at the back buffer's size (live: 2560x1440 and 1920x1080, whole frame). Mode list: `ResolutionIndex` = index into the device's 48 modes, 0–23 16-bit (R5G6B5), 24–47 32-bit (X8R8G8B8). Live: 1920x1080 windowed on a 2560x1440 monitor (client 1920x1080,
style caption), 2560x1440 borderless (style 0x94000000, 0,0 2560x1440), D3D present parameters Windowed = 1.

### Mods folder (`tgefix.mods`, SIMULATION, `games/tge/src/features/mods.cpp`)

DesertStorm2Fix's basic resolver on TGE's addresses: `<game>\Mods\<mod>\…\<FILE.EXT>` replaces or adds a game file by
name (folders in name order, later wins, `_` / `.` prefix = off), report `Mods\GreatEscapeFix-report.txt`. Entry hooks
on File_Resolve (a mod's name → its path in `0x7AB6B8`), File_Open (paths inside `Mods\` open from disk: flags
`[0x7ACDDC]` bits 0 / 1 cleared for the call), File_InArchive (a mod's file exists), Texture_Load `0x4489C0` (a texture
a level archive hands over as a buffer is read from the mod's file instead); the Global Heap (`0x5D2C07`, 1 MB) grows
by the largest mod file (rounded up to 1 MB, cap 64 MB). Live: a recoloured `GE_LOGO0.DDS` (uncompressed 256², 262 KB)
replaced the main-menu logo at 2560x1440; heap 1 MB → 2 MB. Not covered: files the game opens itself (Bink films in
`Data\FMV`), level DLLs, sound caches; no table merging, `.strings`, MODS menu.
Dev: `HKCU\Software\GreatEscapeFix\Dev\ModsOpenLog` = 1 logs every name the resolver is asked for.

### Frame cap (`tgefix.framecap`, CLIENT_ONLY, `games/tge/src/features/framecap.cpp`)

The game runs uncapped (~1400 fps in the first mission on the dev PC). `FrameCap` (default on) and `FpsCap` (0 = the
refresh rate of the monitor the window is on, max 240; else 30..240): a `PSE_EVENT_FRAME` subscriber waits for the
next frame (high-resolution waitable timer, the last ms spun, no catch-up after a slow frame). Live: 180 fps steady on
a 180 Hz monitor. A `d3d8.dll` call-logging wrapper left in a game folder slows the game to ~20 fps by itself.

### Background test mode (`tgefix.devbg`, dev builds only, `games/tge/src/features/devbg.cpp`)

`HKCU\Software\GreatEscapeFix\Dev\Background` = 1 runs the game for scripted tests without touching the user's screen,
mouse or keyboard (as DesertStorm2Fix's): the game window is off-screen and never activated (WS_EX_NOACTIVATE |
WS_EX_TOOLWINDOW at `CreateWindowExA` `0x5D0DA0`; display.cpp places it at −30000 and never brings it to the front),
the message loop keeps running while inactive (`je` `0x5D5933` → NOPs), the device is windowed, every launcher dialog
(`DialogBoxParamA` import `0x5F51F0`) is OK'd without being shown (WS_VISIBLE taken off in WM_INITDIALOG; the launcher
= the dialog with button 0x405 and OK; `Dev\Resolution` "WxH" is picked in Settings… with 32-bit colour), cursor
clips / moves / capture are dropped (`ClipCursor` `0x5CCA12` `0x5CFFC6` `0x5D0095` `0x5D5999`, `SetCursorPos`
`0x4B81DC`, `SetCapture` `0x4BA04C`), the mouse poll's `GetCursorPos` (`0x4B91FF`) answers `Dev\MouseX` / `MouseY`
(back-buffer pixels; missing = far outside the window), message boxes (`0x5F51B4`) are logged and answered, sound is
muted (undone by the next normal start), the log is `GreatEscapeFix-test.log`. `Dev\Shot` = a .bmp path: the next
presented back buffer is written there and the value deleted. Keys can be posted (WM_KEYDOWN / WM_KEYUP with the scan
code); mouse buttons can't (the game reads them through DirectInput).

## Open

1. Saves + co-save, `PSE_MSG_NEW_GAME`.
2. A menu for the settings (HUD scaling / size, FilmsFill, the frame cap have none; the launcher's Settings… dialog
   could take them).
3. Sharper text at large HUD sizes (DS1's fontsharp: the font pages are scaled up bilinearly).
4. Publish the id database once the names settle (`scripts/gen_ids.py --publish tge`).

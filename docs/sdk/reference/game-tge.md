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
monitor refresh, 30..240), `Controller` (1), `PadDeadzone` (40, 0..90 %), `PadVibration` (1), `PadTriggers` (1),
`PadModernLayout` (1) – in the game's menus OPTIONS > GREATESCAPEFIX and OPTIONS > CONTROLS > CONTROLLER (below);
`Windowed` (0) – the launcher's Settings… > Windowed box, applied at once.

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

**Safe area / "fit" screens** (2026-10-25): screens with control flag 0x80 (the mission HUD `GE_Ingame.gui`, hash
0x82B9E96B) place their top-level controls in the TV-safe area (layout `0x4BE930` → `0x4BDF50`(…, design safe rect
48,36..592,444, target rect)). The target comes from `0x4BA600` (7.5 % in from each edge via `0x4BA510`), which reads
the renderer's CURRENT VIEWPORT (`0x4413B0` = renderer +0x40670, call at `0x4BA519`) – not the width / height its
callers pass. It is also stored at GUI +0x448 by the GUI's set-up (`0x58F389` → `0x4BAD80` → `0x4BAD40`, with the size
at GUI +0x438 / +0x43C). From the real viewport the HUD was laid out in real pixels and then scaled again: the action
icon (hand), the pickup picture, the watch / health and the compass ended up off the screen. GreatEscapeFix answers the
virtual screen from that viewport call inside its scope and runs the set-up call in the scope too. Checked 1920x1080:
hand bottom left, pickup picture top centre, watch in the top-left safe corner, as without HUD scaling.

**Loading screen cover**: when a level has loaded, `0x5843A0` (input edi = the loading screen; called at `0x5D2A1A` /
`0x5D2A29` after Level_Init and `0x58739B`) paints a black box (`0x461730`) over the LOADING control before "Please
Press Any Key" is drawn there. It runs outside any control render, so with HUD scaling the box landed in virtual
coordinates on the real screen (a black bar across the briefing art) and LOADING stayed under the prompt; the three
calls run in the scaling scope now. Checked 2560x1440 with a save load (Winter Breakout).

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

Notebook opening animation (2026-10-25): the game grows the pages with the book through the 2D draw's matrix
(renderer +0x409BC on, +0x409C0 bits 3, object [renderer +0x406D0]: scale +0x00 / +0x14, translation +0x30 / +0x34,
applied by `0x43D840` inside the transform `0x443450`), scaling about the REAL screen centre (translation = centre x
(1 - scale)). In a guiscale layout the quads are in the virtual screen (the notebook's 800 x 600), so the pages grew
from the wrong point and swooped in; guiscale's Transform re-centres the translation on the virtual screen's centre for
the draw (only when it matches a scale about the real centre). Checked frame by frame (dev `Dev\ShotBurst` =
"<prefix>|<n>": the next n presented frames as BMPs; tge_bg step `burst_NAME_N`) against the game's own layout.
The pages' controls are also recognised directly (screen +0x3C control lists of Notebook.gui's screens), since GUI
+0xA4 still names the previous screen during the opening.

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
`Data\FMV` – see Films), sound caches. Tables, texts and levels: below.
Dev: `HKCU\Software\GreatEscapeFix\Dev\ModsOpenLog` = 1 logs every name the resolver is asked for.

### Main menu MODS (`tgefix.modsmenu`, CLIENT_ONLY, `games/tge/src/features/modsmenu.cpp`)

MODS between OPTIONS and QUIT on the main menu: every folder in `<game>\Mods` with ON / OFF (`*` = changed, not applied
yet), six rows a page (more mods: a MORE MODS row pages), APPLY renames the folders (`_` in front = off) and rebuilds
the mod index; files read at start-up only change after a restart (the row then reads RESTART THE GAME TO APPLY).
Nothing without `FeatMods`.

How (2026-10-24): the menus are GUI files (`GE_FrontEnd.gui` in catalog.dat, version 0x1D, read whole by
`FUN_004C5240(name, &buffer, &size, heap, tag)` for CGUI::Load `0x4BA870`). Screens = u32 count, then per screen u32
hash (every name's hash = the archive LFSR of the lower-cased name), name (+ byte 0x40), background, controls, the hash
of the control focused first; a control = u32 type, hash, name, u16, 4 floats x y w h (640 x 480), states, children;
a text control's states are 18 bytes {u16 flags (0x10 = the text is a catalog key), u32, f32, f32, u32 colour} and its
texts {font, u32 key hash or buffer size, key / literal text, u16}. Last section = navigation: per screen an Esc table
+ one table per control {u32 direction 0..3 left / right / up / down, u32 type 1, target hash, source hash} (hidden
targets are skipped). That read is hooked for `GE_FrontEnd.gui`: the buffer is replaced (game allocator `0x4CF8C0`,
the old one freed with `0x4CF950`) by a copy with a MODS button (FE_MAIN's OPTIONS copied, y 329; GREATEST ESCAPE /
SELECT LEVEL / QUIT one row lower; FE_MAIN 9 controls, navigation rewired) and a MODS screen (the hidden CHEATS screen
copied, every name / key renamed; its spotlight keeps its hash - the game makes that custom control by hash). Texts:
the catalog lookup `FUN_0047EA90` thiscall(`[0x7DC26C]`, hash) is hooked for our keys (it runs every frame). Screen
objects come from the GUI's factory `[0x7AB31C]` = `FUN_005890B0(hash)`; ours is an object of the CHEATS class (base ctor
`0x4C1800`, a copy of vtable `0x610930` whose event method, slot 14 thiscall(msg, control) ret 8, is ours: 0x1D set-up,
0x15 shown, 4 = a control chosen; controls pass events up via `FUN_004BC890`). FE_MAIN's event method (vtable `0x60F090`
slot 14 = `0x58E400`) is wrapped: MODS opens the screen with `FUN_004BA3D0(gui [0x7D8EC4], hash)`. Rows are shown /
hidden like FE_MAIN does it (control +0x80 bit 2 + event 0x19 / 0x1A). Checked by script (background, 1920x1080): item,
keyboard navigation, toggles, paging, APPLY (folders renamed, index rebuilt), Esc back. Not tried: the mouse, a pad.

### Frame cap (`tgefix.framecap`, CLIENT_ONLY, `games/tge/src/features/framecap.cpp`)

The game runs uncapped (~1400 fps in the first mission on the dev PC). `FrameCap` (default on) and `FpsCap` (0 = the
refresh rate of the monitor the window is on, max 240; else 30..240): a `PSE_EVENT_FRAME` subscriber waits for the
next frame (high-resolution waitable timer, the last ms spun, no catch-up after a slow frame). Live: 180 fps steady on
a 180 Hz monitor. A `d3d8.dll` call-logging wrapper left in a game folder slows the game to ~20 fps by itself.

### Controller (`tgefix.controller`, CLIENT_ONLY, `games/tge/src/features/controller.cpp`)

TGE's joystick code is DS1's at other addresses: poll `0x4B98C0` (Poll + GetDeviceState into a DIJOYSTATE2),
joysticks `0x7A9D24[8]`, count `[0x7AA888]`, current `[0x7AB0AC]`, enumeration `0x4B9680` → callback `0x4B9360`
(ranges ±128 + deadzone on lX / lY / lZ / lRz, deadzone immediate `0x4B9585`), force-feedback table `0x7AB0B8` +
j·0x1C. `default.key` uses DS1's PS2-on-USB numbering (0 Triangle 1 Circle 2 Cross 3 Square 4 L2 5 R2 6 L1 7 R1
8 Select 9 L3 10 R3 11 Start 12–15 D-pad; FIRE = 7). A Logitech WingMan (product name compared at `0x4B940C` /
`0x4B942F`) gets the game's own `wingman.key` / `wingmanrumble.key` (→ `wcurrent.key` / `rcurrent.key`, chosen at
`0x5D6A61`). `Controller` (default on): right after GetDeviceState (`0x4B991A`, `cmp [0x7AB1A8], esi` → a call that
replays it) the state is rewritten per pad as on DS1 – PlayStation / other pads (DirectInput 0 Square 1 Cross 2 Circle
3 Triangle 4 L1 5 R1 6 L2 7 R2 8 Create 9 Options 10 L3 11 R3, touchpad → Select) and Xbox (vendor 0x045E: A B X Y LB
RB View Menu LS RS, right stick lRx / lRy scaled to ±128 with `PadDeadzone`, triggers on lZ → L2 / R2) to the game's
numbering; the POV hat → buttons 12–15 for every pad (WingMan pads keep their buttons). `PadDeadzone` replaces the
game's 40 %. Log line per pad: `Pad: "<name>" (vendor … product …) -> … layout`.

`PadModernLayout` (default on): shoulder buttons and triggers swapped (game 4 ↔ 6, 5 ↔ 7), so FIRE is on R2 / RT as on
DS1's Default preset; off = the game's own layout (fire on R1 / RB). Player 1's input block (blocks `0x7DCAB0` +
i·0x3FC: +0x14 devices, +0x2E0 joystick; binding evaluator `0x5B89A0`) reads one joystick: it follows the pad with a
button held or a stick past half way (button states `0x7A9DE0` + j·0x150, axes `0x7A9E98` + j·0x150).

**Direct pad I/O** (`padio.cpp`, port of DS1's): Xbox pads through XInput (triggers separate), DualSense / DualShock 4
through their HID reports (USB and Bluetooth). Output from a worker thread: rumble from the game's force-feedback table
(filled by `0x4B8750` from the rumble request `0x5B7820(block, effect, strength)`, 6 callers; the game plays it only for
pads with a DirectInput effect) – XInput motors, DualSense / DualShock 4 motors (`PadVibration`); Xbox One / Series
trigger motors (Windows.Gaming.Input) kick on each pull of the fire trigger; DualSense: blue light bar, player LED 1,
adaptive trigger on the fire trigger in missions (`PadTriggers`; needs `PadModernLayout`), which fires only once pulled
through its firm point (Xbox pads too).

**Hot-plug** (`hotplug.cpp`, port of DS1's; the pause is in ingame_input.cpp; scans at start and every 5 s as well as
on device notifications - a user's session had the game's own start-up enumeration miss the DualSense): pads plugged in or pulled out while the game runs are
opened / dropped through the game's own enumeration callback `0x4B9360` (device notifications + an enumeration on a
worker thread with its own DirectInput); a pad that comes back gets its old slot.

**Launcher with a pad** (`launcher.cpp` + `launcher_pad.cpp`, DS1's port; `core/gamepad.cpp` reads XInput + Sony pads
through the system's dinput8): the launcher (dialog 106, proc imm32 `0x5BFA13`), Settings… (108, `0x5BF905`) and
Advanced (107, `0x5BF615`) are driven with a pad – spatial focus moves, Left / Right change a drop-down, ✕ / A select,
○ / B cancel, Options / Start = OK / Play, △ / Y = Settings… (launcher) / Advanced (Settings…); a hint line with
Kenney icons follows the last device (keyboard, PlayStation, Xbox).

**The controller picture** (`controllerpic.cpp`): DS1's PS5 / Xbox art (RCDATA 301 / 302, `generated/controller_art.h`)
on TGE's overlay (`overlay.cpp`: icons, images, rectangles, lines, GDI text, late draws – drawn at the layer's
PSE_EVENT_PRESENT, priority 100), callouts from the key file's actions with the modern swap applied.

Live 2026-10-25: hooks in, front end and LevelA run, hot-plug watching, launcher hooks in (background run); **no pad was
connected – the remap, direct reads, rumble, hot-plug and the launcher's pad navigation are unverified**.

### Action prompts (`tgefix.prompts`, CLIENT_ONLY, `games/tge/src/features/contextprompts.cpp`, setting `ActionPrompts`)

"[button] Pick up fire extinguisher" / "Hold [button] Take down" near the bottom of the screen in missions while an
action is available (the game itself only pulses an icon: HUD widget `0x587E00`, slot 72 of vtable `0x60E150`).
Only while the in-game HUD is the top GUI screen (GUI `[0x7D8EC4]` +0xA4, hash +0xC = 0x82B9E96B): not over the
notebook, the pause menu or other screens.
- Available action: `FUN_005BC860(block)` stores it every frame in block +0x29C (player 1 `[0x7DCD4C]`); ACTION =
  control id 1 (table `0x6602B8`), pressed = block +0x5C bit 2. Character = `[block +0x238]`.
- Ids: 2 use, 3 cancel planting, 4 / 0xD get out, 5 climb, 6 ladder, 7 / 0xC get in, 8 reload, 0xE unlock, 0xF lock,
  0x10 locked, 0x12 pick up, 0x14 door, 0x15 show papers, 0x17 shake off, 0x18 takedown, 0x19 / 0x1A / 0x1B / 0x1C /
  0x23 use, 0x24 release (some inferred from the press switch).
- Targets: pick-up item character +0x1924 (item +0x20 = definition: +0 category, +0x5C u16 picture), door character
  +0x1700 (door +0x5C bit 2 open, 0x20 locked). Item names = the picture's IMAGE_INV_* name, read from the game's
  name -> image compare chain (`0x594000..`, 82 names).
- Timed holds: block +0x394 active, +0x3B8 remaining / +0x3BC total ms (float) -> a progress bar.
- Button = ACTION's binding in the key file (`features::KeyFilePath`: Documents\SCi\GreatEscape\current.key, else
  the game folder's current.key / default.key): the pad's (style of the pad in use, modern swap applied) while a pad
  drives, else the key / mouse button. Row ACTION PROMPTS on OPTIONS > CONTROLS > CONTROLLER. Dev `Dev\ForcePrompt` =
  an action id draws it regardless (checked: "[E] Open door", "Hold [E] Take down" at 1920x1080). Not yet seen with a
  real action in play.

### Sharper text (`tgefix.fontsharp`, CLIENT_ONLY, `games/tge/src/features/fontsharp.cpp`, with `HudScaling`)

DS1's fontsharp for TGE: text is drawn glyph by glyph through the sprite draw `0x44AC60` from the text routines
(`0x436177 0x436932 0x437054 0x4370FB 0x43727E 0x437829`, wrapped: a flag while they run), which binds the page
texture via `0x441940` at `0x44AF11` (hooked, `SubscribeCall`). Inside a text draw the page is replaced by a copy
ceil(GUI scale x sheet scale) = 2..4 times larger (bilinear, alpha edge steepened, colour contrast x1.8). Copies of
pages not drawn for 10 s are dropped. Live 2026-10-25: 256x256 DXT3 font pages -> 768x768 at 2560x1440, menus crisp.

### In-game menus (`tgefix.fixmenus`, CLIENT_ONLY, `games/tge/src/features/fixmenus.cpp` + `gui_file.cpp`)

The GUI files are patched as they are read (whole-file reader `0x4C5240`, hooked at priority 10 – outside the MODS
menu's hook, so its MODS button is already in) through an editable model of the format (`gui_file.h`: version 0x1D
header, resource lists, screens → control trees, navigation tables; byte-exact round trip on all five shipped files):
- GE_FrontEnd.gui: four copies of the hidden CHEATS screen (`~fxpad`, `~fxpic`, `~fxfix`, `~fxmis`; every control
  renamed, the spotlight keeps its hash) – **CONTROLLER** (controller support*, fire on the trigger = `PadModernLayout`,
  stick deadzone 0..90 % in 5s, vibration, adaptive triggers; SHOW BUTTON LAYOUT), **BUTTON LAYOUT** (rows hidden, panel
  widened, the controller picture of the pad in use over it), **GREATESCAPEFIX** (menu & HUD scaling*, HUD size 50..250
  % in 10s, frame cap*, frame limit auto / 30..240, films fill the screen*, widescreen modes*; * = after a restart, the
  plain row says so while one is pending), **CUSTOM MISSIONS** (scripthost.cpp `CustomMissions()`: every campaign, then
  every `.lua` level of a switched-on mod no campaign lists; 6 rows, paged; Enter starts it).
- FE_MAIN: CUSTOM MISSIONS (a copy of OPTIONS) after OPTIONS / MODS when there is anything to list; the column is laid
  out again along the down links (y 210 + 24 n), the logo moved up to y 70.
- GE_PC.gui: OPTIONS_PC gets GREATESCAPEFIX under CONTROL, CONTROLLER_SCREEN_PC gets CONTROLLER under INPUT SELECTION
  (navigation link {accept, open screen, hash}); both panels grow by one row.

Screens: objects of the CHEATS class (factory `0x5890B0`) with a copied vtable, event slot 14 ours (0x1D set-up, 0x15
shown, 4 chosen, 0x17 / 0x18 = Right / Left on a toggle row – the toggle's own index +0xBC (max +0xBD, no wrap) is put
back after each step); FE_MAIN's event slot (`0x60F0C8`) is wrapped for CUSTOM MISSIONS. Texts: keys `~fx_<n>`
answered by the hooked text lookup `0x47EA90`. Settings go through `features::SetSetting` (pse.settings + the values the
features read; vibration, triggers, the fire button, deadzone, HUD size and the frame limit apply at once).

Live 2026-10-25 (background, 1280x720): all four screens and the three buttons shown, Left / Right / Enter change the
values, BUTTON LAYOUT draws the PS5 picture inside the panel, Esc goes back, CUSTOM MISSIONS lists CampTest + LuaTest
and starts `luatest.lua`.

### In-game input (`tgefix.ingameinput`, CLIENT_ONLY, `games/tge/src/features/ingame_input.cpp`, with `Controller`)

- Input mode: player 1's block devices (`[0x7DCAC4]` = block +0x14: 1 keyboard / mouse, 2 pad – exclusive; the game
  tests the bits, but the mouse-only aim scale at `0x56DE3A` would also hit the pad with both set) follow the last
  device used (pad button / stick; key, mouse button or > 8 px mouse movement in the focused game window). INPUT
  SELECTION's stored choice (`0x5833F1` → registry) is never written.
- The menu cursor (`0x5CCAB0`, flag `[0x66A590]`) is hidden while a pad drives; in the front end the pointer then
  reads far outside the window (GetCursorPos import `0x5F5250`: hover beat the pad's up / down). Cursor moves the game
  makes itself (SetCursorPos import `0x5F5244`) and keyboard / mouse input within 0.5 s of pad use (Steam Input's
  desktop layout) don't count as keyboard / mouse.
- Menu prompts (catalog hashes 240A9A71 "esc: back", 6A0B539E "return: select", 54C77EA1, 1471D8FC, 9659879E, DB41FDEB,
  512515D0, 77BD7137, DC92997B "TAB: Return To Game") name the pad's buttons while a pad drives (key file: MENU_FORWARD
  = 2, MENU_BACK = 0); PlayStation or Xbox names.
- Key names (`0x5B0F80`, ecx = type << 16 | code; 1 pad axis, 2 pad button – "joy 3" before): "cross", "r2", "left
  stick" … with the modern swap applied.
- Tutorial texts (API +0x3D0 = `0x5A54C0`, the action's first binding; pad types skipped at `0x5A551C`): the first
  binding of the device in use.
- Player 1's pad pulled out in a mission (hotplug.cpp → `PlayerPadChanged`) pauses the game (the PAUSE read at
  `0x58FD98` answers "pressed" once; paused flag `[0x7DC068]`) with a CONTROLLER DISCONNECTED notice until it is back
  or a key is pressed.

Live 2026-10-25: hooks in, menus / key assignment unchanged with the keyboard; **everything that needs a pad press is
unverified** (no pad input in background runs). Our screens also open from the in-mission pause menu (OPTIONS >
GREATESCAPEFIX, CONTROLS > CONTROLLER > SHOW BUTTON LAYOUT, verified in a `.lua` level).

### Films (`tgefix.films`, CLIENT_ONLY, `games/tge/src/features/films.cpp`)

FilmsFill (widescreen.cpp) zooms only the menu background (`FEPAL`) and the story films (`FMV*`); the legal screen,
logos, chapter cards and briefings keep their side bars (they have text up to the frame's edges; the legal screen was
cut off). films.cpp records the name of the film the game opens (`features::FilmFillsScreen`).


Every film is a level-table entry (type 4) that Screen_Load turns into `data\FMV\<name>.bik` (names carry the region:
`FMV02PAL`, `LegalPAL`) for the film object `[0x7DC278]`. Its open calls BinkOpen at `0x4996A4` as `call [0x86EF18]` –
the no-CD stub's import dispatcher (it finds the import from the return address). That call is replaced: a mod file with
the film's name (`Mods\<mod>\FMV02PAL.bik`) plays instead. Missions never play Bink films; their mid-mission cutscenes
are in-engine EMovies (`.mvs` files listed in the scene .ENV, script API +0x9AC TriggerEMovie).

**Films on request**: `features::PlayFilm(name)` (Lua `game.film` later; dev `Dev\PlayFilm` = name) plays a Bink film
(Mods folder first, then `Data\FMV`) on the next frame in a present loop of its own – the game is frozen meanwhile and
gets no keys; Esc / Space / Enter skip; letterboxed over black; the device state is saved / restored with a state block;
binkw32 exports by name. Bink's sound system is set by the game's first film (the legal screen). Live 2026-10-25:
`LegalPAL.bik` from a mod; `FMV1aPAL` over the main menu and `Brf05Pal` in LevelA, both back to the game after.

### Mods: tables, texts, weapon / vehicle / unit files (`tgefix.modtables`, SIMULATION, `games/tge/src/features/mods_tables.cpp`)

DesertStorm2Fix's table layer on TGE. The table loader `0x5CCFB0` reads 23 tables (Uniform, Faces, Weaps, Inven, Props,
HStat, VStat, fNames, sNames, intIDs, Units, Animal, Vehicle, featStat, Structs, Feats, Doors, Targets, Switches,
UnitWpns, HSkills, VehWpns, ExMods) at start-up **and at every mission load** (Level_Init `0x5D21B9`), so merged tables
take effect at the next mission. A mod's `<TABLE>.txt` = rows appended to the game's, `<TABLE>.replace.txt` = rows
replacing the game's with the same first column (`-KEY` deletes); the merged copy goes to
`%TEMP%\GreatEscapeFix\merged\<hash>\` and is served through the Mods index. Rows the parsers would fail the load on are
left out with a `[fail]` line: Units col 4 not an HStat row, unknown experience, UnitWpns rows with an unknown unit /
type / item (TGE compares exactly, case-sensitive), more items than item slots (Units col 11 and Vehicle col 22 are set
from the UnitWpns / VehWpns rows), a VehWpns item that isn't a weapon, Vehicle col 4 not a VStat row, a second weapon of
GRENADE LAUNCHER / GRENADE LAUNCH/RIFLE / FIST, a magazine over 999.

| File | Keys |
| --- | --- |
| `.weapon` | `id`, `based on` (a Weaps.txt row; = its own id changes the game weapon), `name`, `texture[ <slot>]` (model copy), `model`, `hud icon` (a picture file or an `IMAGE_INV_*` name), `type`, `connector`, `time between shots`, `magazines`, `magazine size`, `reload time`, `damage`, `range`, `muzzle flash`, `sound` (another weapon's bank), `shot sound[ 2]` / `tail sound` (WAV files), `silenced` (no flash, hearing 5, the Welrod's sound), `sound bank`, `hearing range`, animations (`aim`, `reload`, `kneel aim`, `kneel reload`, `prone aim`, `prone reload animation`), `column N` |
| `.vehicle` | `id`, `based on`, `name`, `texture[ <slot>]`, `damaged texture[ <slot>]`, `model`, `damaged model` (a new id copies the base's VehWpns rows) |
| `.unit` | `id`, `based on`, `type` (SOLDIER, GOAT HERDER, CIVILIAN, EMIR), `model`, `second model`, `health` (own HStat row), `experience`, `voice` (speech bank), `face` (a Faces.txt row), `item = <weapon / Inven item>[, amount]` (any number, `none`), `column N`, `stat column N` |
| `.strings` | `KEY = text` or `0xHASH = text`; `[LEVELA]` / `[L01]` sections apply while that mission runs (`[all]` = always) |

Weaps.txt has 74 columns (0 id, 1 model, 3 picture, 4 category, 5 connector, 9 time between shots, 13 magazines, 14
magazine size, 15 reload, 21 damage, 22 range, 34 muzzle flash, 67 sound bank, 68 hearing); Units.txt 15; HStat.txt 22;
Vehicle.txt is comma-separated with 26 columns. Texts: the catalog lookup `0x47EA90` (thiscall(catalog `[0x7DC26C]`,
hash), hash = archive LFSR of the lower-cased key) is hooked, chained with the MODS menu's hook. Shot sounds and
picture files: below. Dev: `Dev\TextLog` = 1 logs every text hash the game asks for with its
text (e.g. `0xB7E063BB` = "new game"). Live 2026-10-25: an in-place Kar98k change, a new rifle and a new guard unit
merged (Weaps / Units / HStat / UnitWpns served, LevelA loads), "new game" renamed by a `.strings` file.

### Levels from mods (part of `tgefix.modtables`, `games/tge/src/features/mods_levels.cpp`)

- **Mission DLLs from mods**: the script DLL loader `0x590B90` calls `LoadLibraryA` at `0x590B9D` with the bare name;
  a name the Mods index has loads from there (handle `[0x7DC0F8]`, script object `[0x7DC0F4]`).
- **`.dllpatch`** (`base Level01.dll`, `base-size n`, optional `size n`, `@ <offset> <original hex | -> <new hex>`),
  **`.envpatch`** (`base GE_LEVEL01.ENV` + hunks) and **`.uses`** (`file X`, `file A = B`, `piece KIT = MODEL.EVO n`)
  are built from the player's install into `%TEMP%\GreatEscapeFix\merged\game` (named after the patch file). TGE only
  loads `<scene>.OCT` (no builder), so an `.envpatch` must keep the zone props' order, or the mod ships its own .OCT. No
  `.rmdpatch`: TGE's `.rmd` is its own format.
- **`<stem>.level`** next to a mission DLL: `--@archive level01` / `none` redirects the level archive (`0x4C5E90` at
  `0x5D1ECA` / `0x5D20FB`), the sound cache name (`0x4C55A0` at `0x5D205D`), the music bank (`0x41DB60` at `0x5D20C8`)
  and the `.FAS` name (sprintf at `0x4829D8`).

Live 2026-10-25: `LevelA.dllpatch` built `LEVELA.DLL` and LevelA ran from it; `.uses` and `LevelA.level` applied.

### Mods: sounds (`tgefix.modsound`, SIMULATION, `games/tge/src/features/mods_sound.cpp`)

Same sound library as DS1 / DS2. The caches are loose in the game folder: `default.sch` (front end, screens, films) and
`<level>.sch` (BANK / PFSM / PFST / IMUS chunks; bank key = ELF hash of `<NAME>.sbk`, case-sensitive). SetSoundCache
`0x41F950` exits the game on a chunk it can't read, so every merged cache is checked first. Its open (`0x4C5220` at
`0x41F8ED`) is hooked: a cache mods change is merged into `%TEMP%\GreatEscapeFix\sound\` and opened from there.

- A mod `.sch` named like a game cache (`LEVEL01.SCH`, `DEFAULT.SCH`) replaces that cache's banks / samples / music by
  key (a language −1 sample replaces every language's); any other `.sch` is a bank pack added to every cache. `.sch`
  names never go through the resolver.
- `.psf` streams by file name: the stream reader `0x4214D0` (PFST, `data\sound\GreatEscapeTitle.psf`) and the
  interactive music reader `0x421040` (IMUS: GEStealth01..03, GECombat01, GEChase01, GEDriving01, MissionFailure –
  only a file of exactly the game file's size, the segments point into it) take the mod's path.
- `.weapon`: `shot sound[ 2] = x.wav`, `tail sound = x.wav` make a bank named like the weapon from its current one
  (gun banks: click, distant tail, shot 1, shot 2), WAVs re-encoded to the game's 4-bit ADPCM (filter table
  `0x623E60`); `sound = <weapon id | bank>` borrows that bank into every cache. The Welrod's bank is in no cache.

Live 2026-10-25: a `.weapon` shot sound from a WAV made bank TEST_RIFLE, merged into `default.sch` and `levela.sch`,
both opened by the game, LevelA runs. Not heard by a human yet.

### Weapon pictures (`tgefix.weaponicons`, CLIENT_ONLY, `games/tge/src/features/weaponicons.cpp`)

`.weapon` `hud icon = MyGun` (MyGun.png / .dds / .tga in a mod, up to 1024²) or a game picture name (`IMAGE_INV_*`).
The picture lookup `0x594150` (name in eax, number in ax) answers `TGEFIX_ICON_<n>` with 1000 + n; there is no global
sheet – every `.gui` that shows items reads `GE_Items.img` through the sprite reader `0x44A430`, which is hooked: each
such sprite gets bigger record / UV / page arrays (game allocator `0x4CF8C0`, tag 0x18) with our images on pages of
their own (the picture's managed texture). Live 2026-10-25: pictures added to both `GE_Items` sheets, game runs; not
yet seen on screen (needs the weapon in the inventory).

### Characters: faces and uniforms

TGE has no skin tables (DS1 / DS2's CSkins). A character's body model is `<Faces.txt head type>_<Uniform.txt mesh>`
(`Gen_Ger_Soldier`), its texture `<face name>_<uniform texture>` (`POW01_POW.dds`), its normal map
`<texture>BUMP.tga`, its hat the uniform's hat model; a unit's face = Units.txt column 12, its uniform = its UNIFORM
item (Inven.txt row) in UnitWpns.txt. So new looks are: a texture file with the game's name (replaces it), new
`Faces.txt` / `Uniform.txt` / `Inven.txt` rows (`.txt` / `.replace.txt`) plus their texture files, and `.unit`
`face =` / `item = <uniform item>`. No `.skin` files.

### Lua missions and campaigns (`tgefix.scripthost`, SIMULATION, `games/tge/src/features/scripthost.cpp`)

A level whose name ends in `.lua` (`Mods\<mod>\x.lua` or the game folder) is run by Lua 5.4 (base, table, string,
math, utf8, coroutine). Level_Init's text-manifest call (`0x5D22B7`, CLoader::Load `0x590E10`, esi / edx) is hooked.

- `--@base LevelA.dll` (first lines): the game's mission DLL runs underneath (loader `0x590B90`) – it loads the scene,
  objectives and logic; the script's callbacks come after its own. Texts: a custom level's string set-up (`0x58D8A0`)
  loads `<level name>.BLF` (call `0x58D8B3` → `0x47F040`) and resets the catalog to the unshipped GenStrs.loc when it is
  missing (every text "*NO STRING*") - a `.lua` level reads its archive's / base's `.BLF` instead. Without a base the script's `init()` must load a
  scene itself (`game.load_scene("GE_LevelA.env")` + what a mission DLL's LevelSetup does), else Level_Init fails at
  the octree. `--@archive levela` (or the base's name) = the level's archive / sound cache / music / .FAS.
- Callbacks: `init()`, `update()` (every game update), `on_message(id, a, b, c, d, e, param)` (TGE's own message
  numbers; an integer / boolean result answers the game), `on_save_loaded()`. Saves hold only the base mission's block
  (TGE never asks the script for its block size).
- `game.call(slot, ...)` / `game.callf` = any of the 698 API slots (table `0x65C1A8`; numbers = int, non-integer
  numbers = float, strings, booleans); `game.read32` / `read8` / `readf`; `game.log` / `print`;
  `game.timer(id, seconds, repeating)` / `game.after(id, seconds, fn, repeating)` (API +0x278, message 0x18; timers set
  in `init()` are armed at the first update – the event queue doesn't exist yet, a timer there crashed at `0x48BD9F`);
  **`game.film(name)`** = a Bink film from the Mods folder or `Data\FMV` (`Brf05Pal`), played at the next frame with
  the game frozen (Esc / Space / Enter skip); `game.emovie(name, n)` = one of the level's in-engine cutscenes (API
  +0x9AC TriggerEMovie); `game.end_mission(2 | 1)` (API +0x21C); `game.next_level(name)` = what follows the mission;
  `game.load_scene` / `load_strings` / `load_anim` / `load_sound_bank` / `load_music_bank`; `game.updates()`.
- Campaigns: `Mods\<mod>\campaign.txt` (`name = ...`, `mission = x.lua` lines in order): after a mission the flow's
  Advance (10 calls hooked, `0x5C9B60`) starts the next mission instead (custom level: GoTo(−1) `0x5C96C0` + name
  `0x401B00`); a non-`.lua` level ends the campaign.
- Start: the main menu's CUSTOM MISSIONS (`tgefix.fixmenus`), `GreatEscape.exe +level x.lua` or `+campaign <name>`
  (the game ignores its own command line; started 30 frames into the front end).

Live 2026-10-25: `+level luatest.lua` (base LevelA) – init, updates, a 5 s timer, `game.film("Brf05Pal")` 8 s and back
into the mission; `+campaign CampTest`: ct1 `game.end_mission(2)` → ct2 started.

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
2. Pad button icons inside game text (the fonts share one char map, chars 0..0x20 → glyph 123; would need DS1's glyph
   extension + kerning table entries).
3. Action prompts: names for the ids still shown as "Use" (the log lists each id met), targets' names for doors /
   vehicles / people, player 2's prompts.
4. MODS menu: long mod names don't fit their row.
5. Publish the id database once the names settle (`scripts/gen_ids.py --publish tge`).
6. Needs a human: pads in play, rumble / triggers, hot-plug, launcher pad navigation, prompts with pad icons.

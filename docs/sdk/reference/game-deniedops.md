# Conflict: Denied Ops – game layer `pse_deniedops` and DeniedOpsFix

Conflict: Denied Ops (Pivotal Games / Eidos, 2008; engine "gamelib3", D3D9) keeps DS1's `.dat` archive index (data
from `0xC000`, LFSR name hash over the lower-cased name) under a different payload format (APKG v4 packages, EVO
0x54). PSE runs on it with the unchanged shim (`dinput8.dll`) and `pse_loader.dll`, plus the game layer
`PSE\Games\pse_deniedops.dll` (`pse/games/deniedops/`) and the host plugin DeniedOpsFix
(`PSE\Plugins\deniedopsfix.dll`, `games/deniedops/`). Game id `deniedops`.

## The exe

`ConflictDeniedOps.exe` (Steam), 7,061,504 bytes, SHA-256 `104AA5D5A4245F02ED96AA2C370E053A7736EDC49F5F6FA67846D73BECB64838`,
PE time stamp `0x478652D0`, image base `0x400000`, `SizeOfImage` `0xB4F000`. `DINPUT8.dll` is a static import, so the
shim proxy applies. The layer accepts the file by hash or by time stamp + image base.

### SteamStub

The exe is wrapped by SteamStub: the on-disk entry point (RVA `0xB4D3DB`, section `.bind`) is the stub, and **`.text`
is encrypted in the file** (the packed file and a Steamless-unpacked copy differ only in `.text` and the entry point;
`.rdata`, `.data` and the import table are plain).

- The shim still works: Windows runs `dinput8.dll`'s `DllMain` before the exe's entry point, the shim starts PSE at
  that entry (the stub's code), then the stub runs as usual. Nothing on disk is changed.
- **But PSE starts before the game's code exists in memory.** At `PSEPlugin_Load` every code address still holds
  encrypted bytes: a code patch would fail its signature check (or be overwritten by the decryption). Code patches
  must wait for the hand-over. DeniedOpsFix hooks the import slot `MSVCR80!__set_app_type` (`0x90E2F4`, plain data
  the Windows loader fills) at load time; the game's CRT start-up calls it first thing after the stub, and that call
  applies the code features (`games/deniedops/src/features/unpack.cpp`, `features::AfterUnpack`). Data in `.rdata` /
  `.data` and import slots can be patched at load time.
- Plugins for Denied Ops that patch code need the same wait (until the layer offers an event for it).

## DeniedOpsFix (host plugin `deniedopsfix`)

Settings: pse.settings of `deniedopsfix` (`HKCU\Software\PSE\deniedops\Plugins\deniedopsfix`), no in-game settings
page yet.

| Internal plugin | Setting | What it does |
| --- | --- | --- |
| `deniedopsfix.widescreen` | `FeatWidescreen` (1) | Every display mode the graphics card lists is offered (launcher Settings > Resolution, start-up checks) |
| `deniedopsfix.mods` | `FeatMods` (1) | `Mods\<mod>\**` files replace or add game files by name |
| `deniedopsfix.campaigns` | `FeatCampaigns` (1, needs `FeatMods`) | CUSTOM CAMPAIGNS item in the main menu: `campaign.txt` campaigns of shipped / mod missions; mission DLLs from mods |
| `deniedopsfix.modsmenu` | `FeatModsMenu` (1, needs `FeatMods`) | MODS item in the main menu: switch each Mods folder on / off while the game runs |

### Widescreen

The game only accepts the 15 sizes in a table at `0x916708` (`{u32 w, h}`): 640x480 ... 1680x1050. Its wide entries
are typos (1152x720, 1720x1080, 1600x1080, 1900x1200, 2500x1600), so 1920x1080, 1920x1200, 2560x1440 and 2560x1600
never appeared (dev PC, RTX 4090 at 2560x1440: 10 modes, largest 1680x1050). Three readers of the table are patched
(after the SteamStub hand-over): `Mode_IsKnown` `0x43EBA0` returns 1, and the whitelist loops of the Settings combo
filler (`0x43F482`) and the device mode check (`0x440A04`) jump straight to "found". Result on the dev PC: 24 modes up
to 2560x1440. The 3D view and the menus keep their proportions at 2560x1440 (checked against 1680x1050); menu text
is not scaled up with the resolution (it is smaller at 1440p).

### Mods folder

`<game>\Mods\<mod name>\...\<FILE.EXT>`: any file the game opens through its file system, by bare name (sub folders
don't matter). Mods load in name order, a later mod wins a name (logged). Folders starting with `_` or `.` are off.
What happened is written to `Mods\DeniedOpsFix-report.txt` at every start ("served" lines while the game runs, `[fail]`
lines for problems) - in release builds too.

Examples of names the game opens (dev log `Dev\ModsOpenLog`): `GAME.XML` (logo / attract films), `ENGINECONFIG.XML`,
`*.STA` tables (`WEAPS.STA`, `UNITS.STA`, `CSKINS_*.STA` ...), `*.GSC` menu scripts, `*.IMG` / `*.FDD` / `*.KRN`
interface art and fonts, `SHADERS\*.FXO`, `PACKAGES\*.APAK` packages, `DEFAULT.SPL`. Files inside an APKG package are
not separate files - replace the whole package.

How it works (gamelib3 Files.cpp): the game always runs in archive mode (`File_Flags` `0xBE0674` bit 0), and every
read / seek / size / close picks the stream's archive FILE (`+0x30`) or disk FILE (`+0x34`) by that global bit, so a
loose file can't be opened "from disk" for one call. `File_Open` (`0x5E24B0`) is replaced for names a mod has: the
mod's file is opened with the game's own CRT (MSVCR80 `fopen` through the exe's import slot) and the stream is
pointed at it as if it were an archive entry at offset 0 (`+0x30` = our FILE, `+0x20` = 0, `+0x2C` = size, buffers
invalidated). `File_Close` (`0x5E3960`) closes our FILE and gives the stream its archive FILE back. `File_Exists`
(`0x5E2900`) answers yes for a mod's file. Not covered: files the game fopens itself (films, saves, profile files).

Verified live (2026-10-25, real install, packed exe, dev and release builds): a mod `GAME.XML` without the
`<logomovie>` lines was served ("served Game.xml from NoLogos") and the game went straight to its PROFILE menu
instead of the logo films.

### MODS menu (2026-10-25)

The main menu gets a **MODS** item after OPTIONS. It opens a MODS screen with one row per folder under `Mods\`
("NAME: ON" / "NAME: OFF", sorted by name without the `_` prefix). Enter on a row renames the folder (`_` prefix off /
on) and rebuilds the Mods index at once - the change counts from the next file the game reads, no restart. Up to 8
rows; with more mods the last row is "MORE MODS - PAGE n OF m". Esc / BACK returns to the main menu. Everything is the
game's own menu system (its list, highlight, slides, prompts and input) - no dialog, overlay or hotkey. The Mods folder
hooks are installed whenever `Mods\` exists, even with every mod switched off, so a mod switched on here is served.

How it is built: Denied Ops' front end is GUI data (`catalog.pc.dat`: `Interface.gui` lists the screen files,
`Main.gsc` holds `SCR_GAME_MENU`, version `0x4C`; names hash with the archive LFSR over the lower-cased name).
`CGUIScreenManager::LoadScreenFile` (`0x702590`) is hooked: after `Main.gsc` loads, the file is read again
(`0x5E51D0`) and the game's own parsers run on byte copies of its records:

- `SCR_DSFIX_MODS` = a copy of the `SCR_GAME_MENU` record in a plain GUI screen (`0x74` bytes, ctor `0x706CF0`, vtable =
  base `0x9418A4` with the message handler (slot 3) replaced and `SCR_GAME_MENU`'s slide transitions (slots 15, 16,
  19)). Its menu is emptied and filled with copies of `BN_OPTIONS` (container loader `0x6FD1B0`); rows notify the
  screen (event Action → Notify), menu Up / Down → previous / next, screen Cancel → Back to `SCR_GAME_MENU`. It shows
  `SCR_GAME_MENU`'s companion screens (title bar / prompts in front, backgrounds behind: screen `+0x54` / `+0x5C`).
- `DSFIX_BN_MODS` = a copy of the main menu's hidden `BN_CHEATS` button, made visible (control flags `+0x20` bit
  `0x8000`), event Action → Select Screen `SCR_DSFIX_MODS`, moved after `BN_OPTIONS` in the menu's list (list order =
  layout and Up / Down order).
- Texts: the catalog lookup `0x5EFD20` (thiscall(catalog, key hash) → `wchar_t*`, asked every draw) answers the new
  keys. A button's text key sits in its text control (`+0x50`) and its highlighted selection bar (`+0x9C`).
- The GUI allocator (`0x708760`) is a linear buffer with no free list (object `[0xC99FD0]`: `+0x4C` free, `+0x50` used,
  `+0x58` base): the copies are parsed with those fields swapped to a buffer of DeniedOpsFix's own, so the game's arena
  is left exactly as it would be without the menu.

Objects: screen manager `[[0xBD0C40]+0x24]` (`+0x118` screens, count byte `+0x11C`, capacity `+0x11E`); screen `+0x14`
name hash, `+0x28` child container; control `+0x10` class, `+0x14` name hash, `+0x20` flags, `+0x2C` y, `+0x48` child
container; container `+4` array, `+8` count byte, `+0xA` capacity byte, `+0x10` focused child (setter `0x6FD2C0`);
event entry (`0x707E30`) `+0` event, `+4` action, `+8` target, `+0x10` flag. Events: 2 Up, 3 Down, 4 Action, 5 Cancel;
actions: 0 Select Screen (target = screen hash), 2 Notify (→ screen message 4 with the control), 4 next, 5 previous,
7 Back (target = screen). Select a screen from code: `0x6FB7C0` thiscall(gui, screen hash, 4, 0).

Verified live (2026-10-25, background runs, dev build): MODS after OPTIONS in the main menu; Enter opens the MODS
screen with the bars, prompts and highlight; Enter on a row renamed the folder on disk and re-indexed in the same run
(ModA → `_ModA`, `_ModB` → ModB); 10 mods page through MORE MODS (page 2: Up skips the hidden rows); Esc back to the
main menu with MODS focused; OPTIONS still opens. Known: on a page with fewer mods the hidden rows keep their slots
(gap above MORE MODS). Not yet tried by a human with a pad or mouse.

### Custom campaigns and mission DLLs (2026-10-30)

Modder side: [deniedops-mods.md](../deniedops-mods.md). Code: `games/deniedops/src/features/campaigns.cpp` (lists,
starting, chaining, DLL load) and `modsmenu.cpp` (the screen). Running notes: `games/deniedops/PORTING_CAMPAIGNS_STATUS.md`.

A Denied Ops mission is an **operation**: a record of `Operations.xml` naming its mission script DLL (`dllname`,
`M_CMB.dll` ... `M_WTR.dll`). The game flow object `0xA29290` holds the records (0xD28 bytes each from `+8`, count
`+0x109278`; record `+0` id, `+0x7C` briefing film, `+0x12C` dllname, `+0x1F8` completed, `+0x210` / `+0x214`
"oncompletion" action / next operation id), the current operation `+0x10720C` and a state machine `+0x1097B4` run by
`0x4756A0` (thiscall flow):

| State | What happens |
| --- | --- |
| 5 | start: the briefing film (record `+0x7C`, state 6) if there is one |
| 0xF | `0x475060`: start the level (state 7). Level init `0x441820` names the level data after the DLL (`<stem>.pc.dat`, `Assets_<name>.xml`, `<stem>.pc.sch` ...) and loads the DLL with `LoadLibraryA` at `0x444452` |
| 7 | playing |
| 0x10 | after the level: `0x474B40` marks the record completed (and saves the profile), `0x474A30` (teaser films) |
| 0x11 | `0x474D10` (one caller, `0x4758B6`): the record's oncompletion - 0 = start operation `+0x214`, 1 = operations map, 2 = main menu ... |

- **Start an operation from code** (what the operations map does, `0x4758F0`): current = record, state = 5,
  `+0x1097D4` = 1, call `0x4756A0`, `[0xC49D18]` = 1. Record by dllname: `0x470880` thiscall(flow, name) ret 4.
  `features::StartMission(dll)` does this; it works from the main menu without a profile.
- **Mission DLLs from mods**: the call `0x444452` (`call [LoadLibraryA]`, slot `0x90E0D0`) is hooked
  (`HookIndirectCall`): a switched-on mod's DLL of that name loads from its folder (full path,
  `LOAD_WITH_ALTERED_SEARCH_PATH`); while a campaign mission with a new name runs, its operation's DLL load gets the
  mod's DLL. Log line `Mods: mission M_WTR.dll loaded from ...\Mods\X\MyOp.dll`.
- **Chaining**: `0x4758B6` (`call 0x474D10`) is hooked: when the operation the campaign started reaches its
  oncompletion step (after a won mission and the EQUIPMENT UPGRADES screen), the campaign's next mission starts the same
  way, the last one goes to the main menu (`0x4746C0` thiscall(flow)). Other operations keep the game's oncompletion.
- **Mission end from code** (tests): script API slot +0xCB4 = `0x48EB70` cdecl(result), 0x10 = success (what the
  mission DLLs call). The debug key "Successful completion of mission" (`0x4389C0`) and the debug screen
  `CCompleteMission` (vtable `0x928F04`) only send message 0x31 to the mission script, which the shipped DLLs act on
  only with their own debug flag set (M_CMB `[0x1000F398]`). Dev KeyScript `win`.
- **The screen**: `SCR_DSFIX_CAMPAIGNS`, built like the MODS screen (a copy of `SCR_GAME_MENU`, 8 rows) with its main
  menu button `DSFIX_BN_CAMPAIGNS` after MODS (only while a campaign exists). Its rows show the current list (campaigns,
  chapters, missions); rows past the list are hidden; the window scrolls when the game's own focus (menu container
  `+0x10`, followed from the row text lookups every frame) reaches its edge row while the list goes on. **Cancel
  reaches a screen's Message as message 5** before its Cancel event runs: answering it (1) keeps the screen, so Back
  steps up a list (a Notify from a Cancel event never reaches the Message; a Back event aimed at the screen itself ends
  the game). Texts: `DSFIX_CAMPAIGNS`, `DSFIX_CAMPAIGNS_TITLE`, `DSFIX_CAMPTXT_<n>` through the catalog lookup hook.
- Not on the path: the game's difficulty pick (NEW CAMPAIGN > difficulty) - a campaign mission runs at the difficulty
  the game has at that moment. Lua missions run on ported levels (`--@lua`, "Lua missions" below); a campaign's own
  `.lua` mission files aren't on the path yet.

Verified live (2026-10-30, background runs, dev build): CUSTOM CAMPAIGNS after MODS; campaigns list; a 10-mission
campaign starts on its last open row with the window scrolled and Up scrolls it back; chapters with "ACT II: REMIX -
LOCKED" (Enter does nothing); Back steps up every list and leaves to the main menu; a picked mission starts with its
briefing film; won (dev KeyScript `win`) -> EQUIPMENT UPGRADES -> the next mission (Comms Base -> Sawmill), progress
1 -> 2; the last mission of a campaign returns to the main menu; `mission select = no` starts at once; a mod's
`MyOp.dll` + `--@archive m_wtr` loaded on M_WTR's level. Not yet: a human with pad / mouse, a failed mission, a loaded
profile. The original game's intermittent level-load crash (EIP around `0x6B1A24`, stack through `0x4657E4`) appeared
in 3 of ~10 loads in these runs - it predates PSE (crate harness, 2026-10-11).

### A mod's own scene on a shipped level (2026-11-01)

A mod can replace a level's whole scene: ship `<STEM>.ENV` (the scene index, e.g. `M_BAS.ENV`) and the part files it
names (any names - parts are looked up by bare name), plus the level package `FINAL_<STEM>.APAK` with the models the
scene uses. Used by the ModKit's DS1 level port (`conflict_ds_tools/ds1todolevel.py`). DeniedOpsFix then:

- builds the level's octree in memory at load (Level_Init's start-up flag `d` read at `0x4425DF` answers `'2'`) when
  the scene index came from a mod - the shipped `<STEM>.PC.OCT` indexes the shipped world;
- reads the mod's `<STEM>.level` sidecar (same file as for mod mission DLLs) for `--@<key> <value>` lines:

| Line | Effect |
| --- | --- |
| `--@script none` | the level's mission DLL still loads the scene, then its script object (`[0x9F2B3C]`) is cleared before the first update - the shipped script would look its own objects up by id |
| `--@camera third` | the level starts in the third-person camera (below) |
| `--@lua <file>` | the mod's Lua file runs as the level's mission (below, "Lua missions") |

Lighting: a static instance is lit only by the lights its entry in the scene's `.LGT` lists (`Version 316`,
`<Lights count = N>` names, `<Instances>` lines `<Inst Name = "X" NumLights = k> i0 .. </Inst>`); its shader is
`30_<tech>_VC` when the model has a vertex colour stream: texture x vertex colour x (light ambient + diffuse N.L). The
ports (ModKit `ds1todolevel`, `lighting="do"`, the default) turn the source's lights into DO lights (`ds1todolight`):
sun in slot 0, an ambient fill, the source's point / spot / directional lights, at most 8 per instance from the source's
own per-instance lists, one ambient-only light per needed ambient level; the source's baked vertex colours are kept only
as a grey occlusion term. **Every converted light uses one intensity, the sun included**: the neutral HDR value 203.3
(HDR on looks like HDR off) x `ds1todolight.GAIN` (2.5: matches DS1 Training 1's brightness in DS1 itself, measured);
slot 0 is rewritten from the atmosphere, so the ART's `SUNBRIGHT` and `SUNAMBBRIGHT` are set to ln of it too. Keeping the host level's sun (`e^7` = 1096.6) made it 5.4x every other light: sunlit ground
burnt white, shaded walls black. `lighting="baked"` = the old way (one white-ambient DLGT, the baked colours as they
are). **An instance name with `-` in it isn't matched by the `.LGT`** (DS1's zone pieces are
`TM1_ACOURSE-1236`; they stayed unlit) - the port writes `_` instead. For a mod's scene DeniedOpsFix also gives an
instance with an EMPTY light list white ambient instead of black (`0x62812B` / `0x628133` / `0x62813B` `fldz` ->
`fld1`; shipped scenes keep the game's black). Research: ConflictGamefiles `_analysis\DeniedOps_tools\lighting\LIGHTING.md`.

Soldiers (ModKit `ds1todes`, DS1 sources): the scene's soldiers, waypoints and regions go into the `.DES`. On a soldier's
`HLOI` line the int three places before the unit name is its **side** and overrides the unit's own team: M_BAS's
players have 0, its enemies 1, its Ranger `HLOU`s and friendly helicopters 2. Friendly soldiers written with 1 or 2 shot
the player within seconds (DS1 Training 1); with **0** they stay friendly.

A placed soldier doesn't walk its start waypoint by itself (not with a start waypoint, the walk flag three fields
later, a mod `.SI` or a route file - all tried); it walks when ordered. The order: **script API slot +0x5DC =
`0x4925B0` cdecl(script id, waypoint id)** (table at `0x991F90`; the object by script id `0x818000`, then the
soldier's virtual +0x188) - what the `.SI` event `SIEV_AssignHLOToNewWaypoint` runs (`CSIEVAssignHLOToNewWaypoint`,
vtable `0x91CD28`, execute `0x4A0120`). Verified: DS1 Training 1's COMMANDER walked his COMMANDER_1 chain (dev key
script `path:189:19`). Slot +0x5EC (listed elsewhere as set_path) only writes a field at +0x600 of the object.

Scene-file pitfalls (each one made the loader read past the end of a part and wait for ever on the loading screen):
the ART's `<PORTALS>` starts with a count line; a version-316 DES has cover points, teleports, position markers,
drills and grenade hints and ends with `CARRY_OVER_SECTION` / `END_CARRY_OVER` (the shipped empty `.DMP` is version
310 and has none of these). Models placed as zone pieces (type 3) need lightmaps the port doesn't make - the DS1 port
places every piece as a plain static.

### Third-person camera (2026-11-01, `deniedopsfix.thirdperson`, setting `FeatThirdPerson`, default on)

**F7** switches between first and third person (0.3 s blend); a mod level's `--@camera third` starts in third
person. The player's camera choice (`0x450970`) asks SetCamera (`0x8A53C0`) for type 7 (first person) every frame;
DeniedOpsFix asks for the game's own chase camera (type 6) instead - death, vehicle, mounted-gun, ladder and
scripted cameras are untouched. Known gaps: no crosshair / soldier panel in third person (the check that would bring
them back, `0x446700`, also switches on the first-person rifle; dev builds: `Dev\ThirdPersonCrosshair` = 1), the
camera can clip into walls, shots leave the muzzle (slight parallax against the screen centre).

### Lua missions (2026-11-01, `deniedopsfix.luamission`)

A mod level whose `.level` sidecar has `--@lua <file>` runs that Lua file (Lua 5.4) as its mission: it starts on the
first frame after the level's loading screen has gone (the file's chunk, then its global `__do_start()`), its
`__do_update()` runs every frame, and it ends with the next level load. Errors are written to the mod report
(`[fail] Lua mission: ...`) and stop the Lua; the level keeps running. Natives in the global table `dox`:

| Native | |
| --- | --- |
| `dox.call(slot, ...)` / `dox.callf(slot, ...)` | cdecl call of script API slot `slot` (byte offset into the table at `0x991F90`) with up to 12 arguments (integers, booleans, floats as float32, strings as `char*`) -> eax as an integer / st0 as a float |
| `dox.obj(script id)` | the scene object's address (`0x818000` on `[0xE55900]`), nil = none |
| `dox.r32(a)` / `dox.rf(a)` / `dox.w32(a, v)` | game memory, checked first (nil / false on an unmapped address) |
| `dox.time_ms()` | milliseconds since the mission started (real time) |
| `dox.log(text)` | a `Lua: ...` line in `DeniedOpsFix.log` |

Verified slots (cdecl, ids = script ids): +0x5DC assign to a waypoint (id, waypoint id); +0x4DC / +0x4E4 unit
activate / deactivate (id); +0x30C door open / close (door id, 1 / 0, actor id or -1); +0x308 door lock (door id,
locked); +0xCB4 end mission (0x10 = success). A scene object's position is at object +0xCC (x, y, z floats; writing
it moves the soldier). The keyboard: key table `0xC4CB98` (scan code | extended << 8 -> 1 went down, 2 held, 3 / 4 up).

ModKit `ds1todolua` builds such a file from a readable DS1 mission (`ds1todolevel.build(..., lua=<folder>)`): DS1's
logic unchanged on a runtime that emulates its DLL image and maps DS1's script API onto these natives (DS1 Training 1
runs its tutorial to the armoury, 2026-11-01).

## `pse.game.deniedops` (version 1)

Header `pse/include/pse/pse_game_deniedops.h`: `ExeSha256()`, `ExeKnown()`. No core events yet (frame, render, level
load, EXIT).

## IDs

`pse/ids/deniedops.ids.txt` (17 ids, database 0, unpublished; reference `ids-deniedops.md`). Code signatures are the
DECRYPTED bytes - verify them against a copy of the exe whose `.text` is the unpacked one (`gen_ids.py --verify-exe
deniedops --exe <copy>`); a layer may register the database only after the SteamStub hand-over.

## Build

From `games/deniedops`: `cmake --preset x86 && cmake --build --preset release` (dev tree in `build/tree/<config>/`),
`cmake --preset dist && cmake --build --preset dist` (`build-dist/package/` + `DeniedOpsFix-<ver>.zip`). From the root:
presets `deniedops-pse` / `deniedops-pse-dist`.

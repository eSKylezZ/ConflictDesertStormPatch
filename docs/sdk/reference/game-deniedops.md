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

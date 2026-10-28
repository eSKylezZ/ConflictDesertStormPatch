# Conflict: Global Storm / Global Terror – game layer `pse_cgs` and `pse.game.cgs` (version 1)

Conflict: Global Storm (Pivotal Games, 2005) runs on Direct3D 9 and is protected by SecuROM 7 (the retail exe never
runs on current Windows; the only exe that runs is the RELOADED NoCD dump). PSE runs on it with the unchanged shim
(`dinput8.dll`) and `pse_loader.dll`, plus the game layer `PSE\Games\pse_cgs.dll` (`pse/games/cgs/`) and the host
plugin GlobalStormFix (`PSE\Plugins\cgsfix.dll`, `games/cgs/src/pse/`). Game id `cgs`. Built and live-tested
2026-10-24. The standalone `dinput8.dll` build (`games/cgs/CMakeLists.txt`'s default target, no PSE involved) keeps
working side by side - PSE is opt-in (`CGS_PSE_PLUGIN` / preset `cgs-pse`), same precedent as DS2 and CV.

## What runs on CGS

- **Exe**: the RELOADED NoCD dump of `ConflictGlobal.exe` (14 475 401 bytes, PE time stamp `0x43032523`, SHA-256
  `D1EE014F6888B100C8806EAACB8FFC22D069B815CF48C3A60FFDA0EC538539A1` - verified with `certutil -hashfile` 2026-10-24).
  The retail SecuROM 7 original is never matched (and never runs).
- **SecuROM VM**: ~1080 of this exe's functions - window creation, the launcher dialogs, the resolution-filter
  predicate's *body*, the keyboard poll, command-line parsing - run inside a SecuROM virtual machine with no static
  call sites (games/cgs/CLAUDE.md "Environment"). The PSE shim's entry-point redirect happens before any of that
  runs (DllMain time, outside the VM), so PSE itself boots unaffected - confirmed live, see "Live-verified" below.
  Everything cgsfix patches is a plain (non-VM) call *site* or entry that merely calls into the VM, or a plain
  kernel32 import slot - never a VM'd function body.
- **Scope (deliberately small, per the 2026-10-24 port request)**: widescreen display modes and the Mods folder,
  plus the plumbing needed to keep testing this build safely off-screen (`cgsfix.frame`, `cgsfix.devbg`, reusing
  `games/cgs/src/features/frame.cpp` / `devbg.cpp` / `graphics.cpp` **unchanged**, copied into `src/pse/features/`).
  `hud.cpp` (menu/HUD scaling) is **not** built here; `frame.cpp`'s call to `features::OnFrameHud()` resolves to an
  inert stub in `src/pse/features/features.h`. Port HUD scaling the same way (copy unchanged + a sub-plugin) if the
  PSE build should carry it too.
- **No core game events**: the layer raises no `PSE_EVENT_FRAME` / `LEVEL_LOAD_*` / `PSE_MSG_EXIT`. The only
  non-VM'd call sites this exe has that are stable enough to hook are the ones `cgsfix.frame` already uses
  (`0x437BD7` / `0x437BD0`, WinMain's per-frame dispatch) - a future port that wants real level-load / render events
  should look for a stable entry point the VM calls *out* to, not try to hook VM'd code directly.
- `pse.game.cgs` v1 is correspondingly tiny: process identity only (`pse_game_cgs.h`). `GameWindow()` is found by
  `EnumWindows` (this process's top-level, un-owned window) rather than a known static `HWND` global, since none has
  been mapped for this exe yet.

## Mods folder (per-asset since 2026-10-25)

A file anywhere under `Mods\<mod>\` replaces the game file with the same name (case-insensitive basename; later mod
folder wins; folders starting with `_`/`.` are off), **including single assets packed inside the `.dat` archives**.
`cgsfix.mods` (`games/cgs/src/pse/features/mods.cpp`) has three layers:

1. **Packed assets - the whole-file loader.** `0x52357C` cdecl(name, void** buf, uint32_t* size, heap, flags) → 1 / 0
   is a plain wrapper around `0x523447`, whose first instruction jumps into the SecuROM VM. Everything the game reads
   whole goes through it by name: textures (`.dds`), shaders (`.fxo` / `.vso`), `Interface.gui`, the `.gsc` menu files,
   `.txt` tables (53 call sites). Its entry is hooked (signature-checked): for a name a mod has, the loader runs with
   the mod file's full path while bit 0 (archive mode) of the file-system flags byte `0x745D34` is cleared, so the
   game's own disk branch reads the loose file and allocates the buffer exactly as for an archive entry. The bit is
   restored right after. Same design as DS1 / CV.
2. **Streamed assets - models (since 2026-10-28).** Models (`.evo`, `.pi2`) never reach the whole-file loader: the
   model loader opens them through the stream open `0x52341B`, a SecuROM thunk (`jmp [0x117E678]`) whose slot is
   resolved on first use to `0x4599EB` = cdecl(name, flags) → file object / 0. That function's entry is hooked
   (signature-checked) with the same rule as 1: a name a mod has opens the mod file's full path with archive mode
   cleared; failure → `[fail]` report line and the game's own copy.
3. **Whole files opened with Win32** (archives themselves, `.sch` sound caches, films, profile files): the kernel32
   `CreateFileA` import slot `0x6C0138` redirects a read-only open whose basename a mod has.

Not covered: a file name that doesn't exist in the game at all and that the game checks for (inside the VM) before
loading; directory enumeration (`FindFirstFileA` / `FindNextFileA`); a read of an archive entry that skips both the
whole-file loader and the stream open (none seen). An asset the game has already loaded stays as it is until the game
loads it again: front-end textures load once at start-up, so switching such a mod shows at the next start.

Verified 2026-10-25 (background runs over the real install): a mod with a red `C04_INT_opt0.dds` (the interface sheet
inside `catalog.dat`) turned the menu bars and buttons red. Switched off in the MODS menu, the next start showed the
original blue. Switched back on, it was re-indexed in the same run.

## Mods menu - MODS item in the game's own main menu (live-verified 2026-10-25, default on)

`cgsfix.modsmenu` (`games/cgs/src/pse/features/modsmenu.cpp`, setting `FeatModsMenu`, default on) adds MODS to the
main menu right after OPTIONS (TRAINING, SINGLE PLAYER, MULTIPLAYER, EDIT PROFILE, OPTIONS, **MODS**, GAMESPY ID
LOGIN, EXIT TO WINDOWS). It opens a MODS screen that looks and behaves like the game's own menu screens: the top /
bottom bars, the BACK / SELECT prompts, slide transitions, gold highlight on the focused row. One row per
`Mods\<name>` folder, sorted by name ("NAME: ON" / "NAME: OFF"). Enter switches a folder on or off: a `_` prefix
rename plus a re-scan of the redirect, with no relaunch, and the row stays in place. Esc returns to the main menu
with MODS still focused. With more than 8 mods, the 8th row reads "MORE MODS - PAGE x OF y"; Enter on it shows the
next page. With no mods, a single row says "NO MODS IN THE MODS FOLDER". No dialog, overlay or hotkey is involved:
everything is driven by the game's own menu input. Assets the game already loaded change at their next load (see
"Mods folder").

How: the front end is data (catalog.dat `Interface.gui` + `*.gsc` screen files) parsed by plain, non-VM code. After
the game loads `Main.gsc` (loader `0x597810`), cgsfix parses byte copies of three records with the game's own
loaders:

- the main menu's hidden `BN_CHEATS` button, which becomes MODS;
- its `BN_OPTIONS` button, which becomes the rows;
- the `SCR_GAME_MENU` screen, which becomes the MODS screen.

It then adds the menu events (Action -> Select Screen / Notify, Cancel -> Back), and the catalog lookup `0x686A10`
answers the new text keys. Things a port of this to another screen needs to know:

- control flag `0x8000` = visible;
- the menu stacks its visible buttons in list order;
- the GUI's bump arena has no slack, so the copies are allocated from cgsfix's own buffer;
- a screen draws its bars / backgrounds through its companion-screen lists (`+0x50` / `+0x58`);
- the focus setter is `0x5A03A0`.

Details and addresses: games/cgs/CLAUDE.md "Native MODS menu".

## Getting it

```c
#include <pse/pse_game_cgs.h>

const PSEGameCgsV1* game = (const PSEGameCgsV1*)pse->Query(pse->plugin, PSE_GAME_CGS_INTERFACE, 1);
if (!game || game->size < sizeof(PSEGameCgsV1)) return 0;   /* not Conflict: Global Storm */
```

Served by the built-in plugin `pse.game.cgs` 1.0.0 (CLIENT_ONLY): a manifest can say `requires = pse.game.cgs >= 1.0`
or `games = cgs`.

## Functions

| Function | Returns |
| --- | --- |
| `GameWindow()` | the game's top-level window (found via `EnumWindows`, cached); any thread |
| `ExeSha256()` | the running exe's SHA-256, upper-case hex; any thread |
| `ExeKnown()` | 1 = the exe is the RELOADED NoCD build every address here comes from; any thread |

## GlobalStormFix as a host plugin

`games/cgs/src/pse/cgsfix.cpp` (manifest `cgsfix.pse`, `host = true`, `requires = pse.game.cgs >= 1.0`). Sub-plugins
(internal, `patch::OwnerScope` per `Apply*`):

| Plugin | Flags | What |
| --- | --- | --- |
| `cgsfix.widescreen` | CLIENT_ONLY | mode-table builder + Settings combo: every adapter mode, not just the 4:3/5:4 whitelist (`0x51EF36` / `0x418886` → `mov eax,1`) |
| `cgsfix.mods` | SIMULATION | `Mods\<mod>\**` served ahead of the archives by name - whole-file loader entry `0x52357C` (packed assets) + stream open `0x4599EB` (models) + CreateFileA import slot (whole files), see "Mods folder" |
| `cgsfix.frame` | CLIENT_ONLY | per-frame hook (`0x437BD7` / `0x437BD0`) that re-asserts the D3D9 device's vtable hooks every frame (this runtime rewrites the device's own vtable copy after `CreateDevice`) and drives the background test mode's screenshot/Present polling |
| `cgsfix.devbg` | CLIENT_ONLY | dev builds only: off-screen/muted background test mode (`HKCU\Software\GlobalStormFix\Dev`), same registry values as the standalone build (games/cgs/CLAUDE.md "What the background mode needed") |
| `cgsfix.modsmenu` | CLIENT_ONLY | MODS item in the main menu + MODS screen, see "Mods menu" |

Settings: `FeatWidescreen`, `FeatMods`, `FeatModsMenu` (all default on). Migrated once from the standalone
build's registry key (`HKLM\SOFTWARE\Pivotal\Conflict Global\Enhancements`, virtualized) into
`HKCU\Software\PSE\cgs\Plugins\cgsfix` on first load.

## PSE ID database

`pse/ids/cgs.ids.txt`, database 0 (unpublished). 9 ids, all plain call sites / globals (never a VM'd function body):
`ModeFilter_CallTable`, `ModeFilter_CallDialog`, `Frame_CallGame`, `Frame_CallOther` (site), `Renderer`, `GameState`,
`Main_ActiveFlag`, `Main_ActiveFlag2` (global), `FontManager_PickHiRes` (site). Verified 2026-10-24: `python
scripts/gen_ids.py --verify-exe cgs --exe "<game folder>\ConflictGlobal.exe"` → 0 mismatches.

## Build

From `games/cgs`: `cmake -S . -B build-pse -A Win32 -DCGS_PSE_PLUGIN=ON && cmake --build build-pse --config RelWithDebInfo`.
From the repo root: `cmake --preset cgs-pse && cmake --build --preset cgs-pse` (release package: `cgs-pse-dist`). The
standalone preset (`cgs` / `cgs-dist`) is unaffected - `CGS_PSE_PLUGIN` defaults OFF.

## Live-verified 2026-10-24

Deployed over the installed standalone build (backed up `dinput8.dll`/`.pdb`, restored after, byte-identical) in
the game folder, background mode (`HKCU\Software\GlobalStormFix\Dev\Background=1`), a real
`Mods\CgsPseTest` folder. `GlobalStormFix.log`: `pse.ids: cgs database 0, build reloaded-nocd: 9 ids, 9 resolved, 0
mismatched`, `cgsfix.widescreen` 2 patches applied, `cgsfix.mods` indexed 1 mod / 1 file and installed the
CreateFileA pointer hook, `cgsfix.frame` installed both call hooks, `cgsfix.devbg` applied its 2 patches and put the
window off-screen (806x629, never activated), D3D9 device created windowed at the forced resolution, logo films
skipped, reached the front end's looping background film - all without taking the screen, mouse or keyboard. 1 of 1
plugin loaded, start-up in 15.6 ms. Not yet verified: the launcher dialog's resolution list with a real player
watching it, and a Mods folder redirect of a whole `.dat` archive specifically. (A packed asset served from a mod was
verified 2026-10-25 - see "Mods folder".)

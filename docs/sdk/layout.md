# PSE in an install: files, start-up, missing parts

Phase 2 layout (built 2026-10-19). What is where in a game folder, what runs in which order when the
game starts, and what happens when a part is missing or fails.

## Files

```text
<game>\
  DesertStorm.exe                   the game (never changed on disk; the opt-in 4 GB patcher flips one header bit)
  dinput8.dll                       PSE shim: proxy of the system dinput8.dll + entry-point hook (pse/shim)
  DesertStormFix-Patcher.exe        4 GB patcher (DesertStormFix)
  PSE\
    pse_loader.dll                  the loader: every service (pse.log, pse.hooks, pse.messaging, pse.tasks,
                                    pse.settings, pse.serialization, pse.lua, pse.ids registry, plugin manager) -
                                    the ONLY copy in the process (pse/loader_dll + pse_core)
    Games\
      pse_ds1.dll                   Conflict: Desert Storm game layer: exe detection, ID database, pse.game.ds1,
                                    the core game events + the co-save (pse/games/ds1)
    Plugins\
      dsfix.dll + dsfix.pse         DesertStormFix: the HOST plugin, registers the 60 dsfix.<feature> sub-plugins
      dsfix.discord.dll + .pse      DesertStormFix's Discord status (an ordinary plugin, SDK only)
      dsfix.<feature>.dll + .pse    DesertStormFix features split out of dsfix.dll (24 so far; SDK + dsfix kit)
      <name>.dll + <name>.pse       third-party plugins
  Mods\                             content mods (unchanged)
  PSE\Ports\<id>\                   another Conflict game's mission maps as empty levels (made from the player's copy)
  PSE\Library\<id>\                 asset library: everything of another Conflict game, read only when a mod names it
```

`PSE\Ports\<id>\` holds another Conflict game's mission maps converted by the ModKit (`cli/ds_convert_maps.py`) from
the player's own copy: each map as an empty Desert Storm level (`<level>.lua`: scenery, collision, lights, AI routes,
the squad start - no enemies, vehicles or mission logic) plus the models, weapons, vehicles and soldier types the maps
use, and `port.txt` (source game, its exe's SHA-256, converter version). DesertStormFix indexes it like a mod folder
(before the mods, so a mod can still replace a file). A folder that also has a `campaign.txt` would be listed under
SINGLE PLAYER > CAMPAIGNS - the maps converter writes none (converted campaigns were dropped: their mission scripts
kept failing on behaviour differences). Game-derived: never shipped, never sent online (online-compat.md "Converted
campaigns").

`PSE\Library\<id>\` is the asset library (ModKit `cli/ds_build_library.py`): every model, character, vehicle, weapon,
soldier type, animation, texture and sound bank of another Conflict game, converted from the player's own copy, every
file named with the game's prefix (`DS2_`, `GE_`, `CV_`, `CG_`, later `DO_`); `library.txt` lists what each file is,
`not-converted.txt` what didn't convert and why. It is NOT indexed or loaded at start-up: DesertStormFix reads a file
from it only when something asks for that exact name and nothing else has it (mods.cpp `LibraryFind`) - a mod's
scene, model or texture reference, a `.weapon` / `.vehicle` / `.unit` / `.skin` copied out of the library into a mod,
a sound bank by name (`<PREFIX><BANK>.SCH` packs, their samples renumbered into the game's own range: 0x51000000 DS2,
0x52000000 GE, 0x53000000 CV, 0x54000000 CG, 0x55000000 DO - clear of Desert Storm's own ids, which are ELF hashes
under 0x10000000). Game-derived: never shipped, never sent online.

`PSE\Cache\assets\` (setting SHARED ASSET STORE) is the asset store DesertStormFix builds from the install's own
archives at start-up: game-derived, never shipped, safe to delete (rebuilt in ~5 s). A read-only install uses
`%LOCALAPPDATA%\PSE\ds1-<hash>\assets\` instead.
`PSE\Cache\missions\` (setting MISSIONS FROM LUA) holds the mission DLLs translated to Lua (`<Mission>.lua` +
`.stamp`, written at start-up or by `pse_build missions`): game code made from the player's DLLs, never shipped, safe to
delete (~1 s to remake all 18); read-only installs use `%LOCALAPPDATA%\PSE\ds1-<hash>\missions\`.

`PSE\Originals\` holds the game's own archives (`*.dat`), level sound caches (`*.sch`) and level script DLLs
(`Mission*.dll`, `Training*.dll`, `MP_MISSION*.dll`, `FrontEnd.dll`, `Outro.dll`), moved out of the game folder at every start once processed (plugin
`dsfix.originals`). The game finds them through a redirect of its file opens - in the DS1 game layer
(`pse/games/ds1/ds1_originals.cpp`), so it works without DesertStormFix too; a file back in the game folder (a game
update, "verify files") wins and is moved again at the next start; a read-only game folder keeps its files. Readers of
the game's files list the game folder first, then `PSE\Originals` (`pse_build` `OriginalsDir`, DesertStormFix
`features::GameFile` / `GameFiles`). **Never delete it**: to remove PSE, move its contents back into the game folder
first. Dev builds: `Dev\KeepOriginals` = 1 moves nothing.

Dev builds have a `.pdb` next to every DLL. The build assembles the same tree in `build*/tree/<config>/` (and copies it
into `DS_GAME_DIR` when that is set); the release zip has it without PDBs, plus `DesertStormFix-README.txt`.

| Binary | CMake target | Source | Links |
| --- | --- | --- | --- |
| `dinput8.dll` | `pse_shim` | `pse/shim/` (`shim.cpp` game-neutral, `proxy_dinput8.cpp` + `.def` the dinput8 exports) | nothing of PSE |
| `PSE\pse_loader.dll` | `pse_loader` | `pse/loader_dll/loader_main.cpp` | `pse_core` (= `pse/loader/*.cpp`) |
| `PSE\Games\pse_ds1.dll` | `pse_ds1` | `pse/games/ds1/` + `pse/loader/ids.cpp` | `pse_hostlink` |
| `PSE\Games\pse_ds2.dll` (Desert Storm II installs) | `pse_ds2` | `pse/games/ds2/` + `pse/loader/ids.cpp` | `pse_hostlink` |
| `PSE\Games\pse_tge.dll` (The Great Escape installs) | `tge_pse_layer` (`games/tge`) | `pse/games/tge/` + `pse/loader/ids.cpp` | `tge_pse_hostlink` |
| `PSE\Games\pse_cv.dll` (Conflict: Vietnam installs) | `cv_pse_layer` (`games/cv`) / `pse_cv` (root `pse_tests`) | `pse/games/cv/` + `pse/loader/ids.cpp` | `cv_pse_hostlink` |
| `PSE\Games\pse_cgs.dll` (Global Storm installs) | `cgs_pse_layer` (`games/cgs`) / `pse_cgs` (root `pse_tests`) | `pse/games/cgs/` + `pse/loader/ids.cpp` | `cgs_pse_hostlink` |
| `PSE\Games\pse_deniedops.dll` (Conflict: Denied Ops installs) | `deniedops_pse_layer` (`games/deniedops`) | `pse/games/deniedops/` + `pse/loader/ids.cpp` | `deniedops_pse_hostlink` |
| `PSE\Plugins\dsfix.dll` | `dsfix` | `src/` (all of DesertStormFix) | `pse_hostlink`, Lua |
| `PSE\Plugins\dsfix.discord.dll` | `dsfix_discord` | `plugins/discord/` | the SDK header only |
| `PSE\Plugins\dsfix.<feature>.dll` (shadows, audio, widescreen, textfit, mouse, menus, units, limits, difficulty, jpfixes, handling, tooltips, rumble, bodies, followers, hud, weather, timeofday, voices) | `dsfix_<feature>` (`dsfix_add_plugin`) | `plugins/<feature>/` | `dsfix_kit` (`plugins/kit/` + `src/core/patch.cpp`, on the SDK); the last eleven also dsfix.dll's interface `dsfix.services` (`src/core/dsfix_services.h`) |

## More than one game (2026-10-19)

The shim, `pse_loader.dll` and every plugin built only against the SDK are **game-neutral**: the same files go into
every supported game's folder. Per game there is one game layer `PSE\Games\pse_<game>.dll` (detection, ID database,
`pse.game.<game>`, the hooks that drive the core events, the co-save, the host plugins it allows):

| Game | Layer | Exe it detects | Host plugin it allows | Reference |
| --- | --- | --- | --- | --- |
| `ds1` Conflict: Desert Storm | `pse_ds1.dll` | `DesertStorm.exe` (Steam = GOG) | `dsfix` (DesertStormFix) | [game-ds1.md](reference/game-ds1.md) |
| `ds2` Conflict: Desert Storm II | `pse_ds2.dll` | `CDS II.exe` 1.0 disc-free | `ds2fix` (DesertStorm2Fix) | [game-ds2.md](reference/game-ds2.md) |
| `tge` The Great Escape | `pse_tge.dll` | `GreatEscape.exe` 1.0 without the disc check | `tgefix` (GreatEscapeFix) | [game-tge.md](reference/game-tge.md) |
| `cv` Conflict: Vietnam | `pse_cv.dll` | `Vietnam.exe`, Sep 2004 build (no SecuROM) | `cvfix` (VietnamFix, widescreen + Mods folder only so far) | [game-cv.md](reference/game-cv.md) |
| `cgs` Conflict: Global Storm / Global Terror | `pse_cgs.dll` | `ConflictGlobal.exe`, RELOADED NoCD (SecuROM 7, VM'd) | `cgsfix` (GlobalStormFix, widescreen + a CreateFileA-redirect Mods folder only so far) | [game-cgs.md](reference/game-cgs.md) |
| `deniedops` Conflict: Denied Ops | `pse_deniedops.dll` | `ConflictDeniedOps.exe` (Steam, SteamStub-wrapped: the shim works, code patches wait for the stub's hand-over) | `deniedopsfix` (DeniedOpsFix: widescreen, Mods folder) | [game-deniedops.md](reference/game-deniedops.md) |

A folder may hold several layers: the loader asks each `PSE\Games\pse_*.dll` in name order and the first whose
`PSEGame_Detect` answers 1 wins; the others are freed again (pse_tests: both layers installed, CDS II.exe mapped →
`pse_ds2.dll` picked). A plugin says which games it runs on in `PSEPluginInfo.games` (`{"ds1", "ds2", NULL}`; NULL =
any) - refused elsewhere with "made for …; this game is …". The settings root follows the layer's game id
(`HKCU\Software\PSE\ds2\Plugins\<plugin>`). Two DS2 trees: the root project's `pse_tree_ds2` (`build*/tree-ds2/<config>/`:
shim + loader + layer + the SDK examples, for the layer's tests) and the DS2 product's `ds2_pse_tree` in `games/ds2`
(`-DDS2_PSE_PLUGIN=ON`, root presets `ds2-pse` / `ds2-pse-dist`: `build*/tree/<config>/` = shim + loader + layer +
`ds2fix` + every `games/ds2/plugins/<name>`, the release package + zip from it); both deploy only into `DS2_GAME_DIR`,
never into the DS1 install.

`pse_hostlink` (`pse/hostlink/`) is how a DLL that is not the loader uses the loader's internal C++ declarations
(`pse/loader/loader.h`): every stateful function is forwarded through the private `pse.host` table; pure helpers
(x86 length decoder, SHA-256 + online-compat encoding, the registry migration copy) are compiled in. Only host plugins
and game layers get `pse.host` ([reference/plugins.md](reference/plugins.md#host-plugins)).

## Start-up sequence

1. **The exe loads `dinput8.dll`** (static import). The shim's `DllMain` runs under the loader lock and does only one
   thing: when loaded with the process (`lpReserved != NULL`), it overwrites the exe's entry point with a jump to its
   stub. Nothing else (a `LoadLibrary("dinput8.dll")` later, after the entry point ran, hooks nothing).
2. **Entry point** (main thread, before the CRT and `WinMain`, outside the loader lock): the stub puts the 5 entry
   bytes back, loads `<folder of dinput8.dll>\PSE\pse_loader.dll` (`LOAD_WITH_ALTERED_SEARCH_PATH`) and calls its
   export `PSELoader_Start` under SEH.
3. **`PSELoader_Start` → `pse::Start`** (`pse/loader/startup.cpp`):
   1. hashes the exe file (SHA-256 with the Large Address Aware bit cleared) and reads its image base / PE time stamp;
   2. loads every `PSE\Games\pse_*.dll` in name order and calls `PSEGame_Detect`; the first that answers 1 wins, the
      others are freed. None = **vanilla** (step 4 of the list below);
   3. `pse::Init` with the layer's game id (settings root `HKCU\Software\PSE\<gameId>`);
   4. registers the layer's plugin (`pse.game.ds1`, built in, with host rights) and calls `PSEGame_Init`. The DS1 layer
      registers the ID database (`pse.ids`: signatures checked against the still-original bytes), `pse.game.ds1` v1
      and the private `pse.game.ds1.host` v0, then installs the hooks that drive the core game events and the
      co-save (frame, render, level load / unload, save / load, D3D device - owner `pse.game.ds1`,
      [reference/game-ds1.md](reference/game-ds1.md#events-and-messages)). Host and other plugins chain on the same
      sites at their own priorities;
   5. **plugins, round 1 - host plugins**: manifests with `host = true` whose name is on the layer's list
      (`PSEGame_Info.hostPlugins`; DS1: `dsfix`). `dsfix.dll`'s `PSEPlugin_Load` = what the old `dinput8.dll` did in
      `Init`: `DesertStormFix.log` opened, `pse.host` attached, log / report sinks set, the game layer's providers set,
      the 60 `dsfix.<feature>` sub-plugins registered, **settings migration + load**, then every feature's `Apply`;
   6. **plugins, round 2 - everything else**, resolved against what is registered by then (so they may require
      `dsfix.hud` or any other sub-plugin);
   7. `PSE_MSG_POST_LOAD`, `PSE_MSG_POST_POST_LOAD`, `PSE_MSG_GAME_ENTRY`.
4. The stub jumps to the original entry point: the CRT, `WinMain`, the launcher. Everything above has already
   patched the game.

Shutdown: the game layer (DS1: the level unload in `WM_DESTROY`, `0x411404`; DS2 likewise) calls `pse.host` `Shutdown`
once - DesertStormFix's window procedure calls it too, whichever comes first: `PSE_MSG_EXIT`, then external
plugins' `PSEPlugin_Unload` in reverse order and their DLLs freed - except host plugins (their `Unload` runs, the DLL
stays: Shutdown runs from inside it).

## Logs

Every loader / plugin line goes to the host plugin's sink - DesertStormFix: `DesertStormFix.log` next to the exe (dev
builds; the release package writes no log). Lines from before the host set its sink (exe hash, game layer detection)
are kept and written into it as soon as it does. When no host plugin takes them (no `dsfix.dll`, an unknown exe), dev
builds of `pse_loader.dll` send them to the debugger output (`OutputDebugString`, DebugView); release builds drop
them. The shim itself only ever writes one `OutputDebugString` line, when something is wrong.

A game layer may set a sink of its own until a host plugin replaces it: the DS2 layer writes every line (start-up
lines included) to `PSE\Logs\loader.log`, because Desert Storm II has no shipped host plugin yet
([game-ds2.md](reference/game-ds2.md#log)). The DS1 layer doesn't (DesertStormFix always takes them).

## When parts are missing or fail

| Situation | Result |
| --- | --- |
| No `PSE\pse_loader.dll` (or it has no `PSELoader_Start`) | shim: one debugger-output line; the game runs **vanilla** (only the dinput8 proxy is active) |
| `PSELoader_Start` crashes | caught by the shim, one line; the game continues (whatever was patched before the fault stays) |
| No `PSE\Games\pse_*.dll`, or none recognises the exe (another version, another game) | vanilla: no `pse::Init`, no plugin loaded |
| The game layer's `PSEGame_Init` fails / crashes | logged; no plugins; the game runs unpatched |
| No `dsfix.dll` | PSE runs (game layer, other plugins): plugins still get frames, render / D3D events, level events, NEW_GAME, the co-save and `PSE_MSG_EXIT`; the game still finds its files in `PSE\Originals` (no frame cap); no DesertStormFix features, no `DesertStormFix.log` - every line goes to `PSE\Logs\loader.log` |
| `dsfix.pse` without `host = true`, or a renamed host plugin not on the layer's list | refused / its Load gets no `pse.host` and fails - DesertStormFix is off, other plugins run |
| `dsfix.dll`'s Load returns 0 or crashes | everything it and its sub-plugins registered through the broker is taken back, they are disabled; its DLL stays mapped (raw patches may point into it) |
| A plugin requires a host plugin that didn't load | refused ("requires dsfix.hud, a host plugin that is not loaded") |
| An old single `dinput8.dll` (DesertStormFix ≤ 3.3.0-Alpha builds before the split) | replaced by the shim on install (same file name); stale files in `PSE\Plugins` from older packages are overwritten |

## Uninstall

Delete `dinput8.dll` and the `PSE` folder (and `DesertStormFix-Patcher.exe` / `DesertStormFix-README.txt`). Settings
stay in the registry (`HKCU\Software\PSE\ds1`), saves' `.pse` co-saves stay next to the saves (the game ignores them).

## Repository layout

One repository for every game (user decision 2026-10-19; it is renamed later). The other games' patch repos were
forks of DesertStormFix's template and moved in without history.

```text
<repo>\
  pse\                    Pivotal Script Extender, game-neutral: loader/ (pse_core services), loader_dll/, shim/,
                          hostlink/, include/pse/ (the SDK headers), cmake/, examples/, tests/ (pse_tests),
                          build/ (pse_build: asset store + mission translator - source, not output)
    games\<id>\           one game layer per game: ds1/ (pse_ds1.dll), ds2/ (pse_ds2.dll), cv/ (pse_cv.dll), cgs/ (pse_cgs.dll)
    ids\<id>.ids.txt      ID databases + <id>.published.txt (frozen lists); compiled by scripts/gen_ids.py
    ids\generated\        pse_ids_<id>.h - the compiled tables the game layers register (generated, committed)
  games\<id>\             one folder per game's patch:
    ds1\                  DesertStormFix (since 2026-10-22; was src\, plugins\, dist\ and the DS1 scripts at the
                          root): src\ (host plugin dsfix + its sub-plugins, core\, features\, render\, generated\,
                          patcher\), plugins\ (its external plugins: dsfix.discord), scripts\ (patch_defs.py + the
                          generators of src\generated), dist\ (player readme). Built by the root CMakeLists.txt.
    ds2\                  DesertStorm2Fix: ds2fix.dll = the PSE host plugin + plugins\<name>\ (its external plugins),
                          packaged with the PSE runtime (-DDS2_PSE_PLUGIN=ON); the old standalone dinput8.dll too
    cv\                   VietnamFix (dinput8.dll)
    cgs\                  GlobalStormFix (dinput8.dll)
    tge\                  GreatEscapeFix (PSE only: tgefix.dll host plugin + the shim / loader / pse_tge tree)
    deniedops\            DeniedOpsFix (PSE only, started 2026-10-24: deniedopsfix.dll host plugin + the shim /
                          loader / pse_deniedops tree; widescreen modes + Mods folder 2026-10-25)
  scripts\                PSE scripts: gen_ids.py (every game's ids), the records / translator gates, run_tests.py
  assets\, third_party\   shared by every game (Kenney input prompts; Lua 5.4.7, Zydis)
  docs\sdk\               this documentation (local)
  tests\live\             live tests against the running game (DS1)
  tools\                  local, gitignored: DS1 helpers at the top, tools\ds2\, tools\cv\, tools\cgs\ per game
```

`games/ds1` is built by the root `CMakeLists.txt` together with PSE (presets `x86` / `dist`); its own CMake project
comes with the split of DesertStormFix into several plugin DLLs. The other `games/<id>` keep the layout they had as a
repo: `src/` (`core/`, `features/`, `generated/`, `dllmain.cpp`,
`dinput8.def`, `version.rc.in`), `scripts/` (its icon generators, reading the shared `assets/`), `dist/` (player
readme), `CMakeLists.txt` + `CMakePresets.json` (+ a gitignored `CMakeUserPresets.json`), `PORTING.md`, `README.md`,
and a local `CLAUDE.md` (gitignored like the root one) that says where its game layer, ids and tools are.

Builds: `cmake -S games/<id> --preset x86` (binary dirs `games/<id>/build*`, as before the move), or from the root
with `PSE_GAME=<id>` - presets `ds2`, `ds2-dist`, `ds2-pse`, `cv`, `cv-dist`, `cgs`, `cgs-dist`, `tge`, `tge-dist` (binary dirs
`build-ds2`, `build-ds2-dist`, `build-ds2fix`, `build-cv` ...; outputs `<bin>/<config>/dinput8.dll`, `<bin>/package`
+ zip). With `PSE_GAME` set only that game's project is configured (no DesertStormFix / PSE targets, so no name
clashes); the DS1 presets `x86` / `dist` are unchanged.

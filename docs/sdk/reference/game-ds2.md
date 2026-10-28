# Conflict: Desert Storm II – game layer `pse_ds2` and `pse.game.ds2` (version 1)

The second game on PSE (the proof that the core is shared). Built 2026-10-19.
The loader, the shim and every loader interface are the **same binaries** as for Desert Storm; only the game layer
`PSE\Games\pse_ds2.dll` (`pse/games/ds2/`) and its ID database (`pse/ids/ds2.ids.txt`) are new. A plugin that uses only
the loader interfaces and the core game events runs unchanged on both games – `example.hello`, `example.hello_client`
and `example.events_logger` do, as the **same DLL files** (their `games` lists say `"ds1", "ds2"`).

Header: [`pse/include/pse/pse_game_ds2.h`](../../../pse/include/pse/pse_game_ds2.h). Ids:
[ids-ds2.md](ids-ds2.md) (115, `pse/include/pse/pse_ids_ds2.h`). API 0 = unstable until SDK 1.0.

## What runs on DS2

| Part | State |
| --- | --- |
| Detection (`PSEGame_Detect`) | `CDS II.exe` 1.0 at image base 0x400000: the **disc-free** exe (SHA-256 `E9B46420DC00B13C3C48A9E9B7B3FDA29458D5EA83AC0D203F5B68FDD111542A`, Large Address Aware bit cleared) or **any other file of that build** (PE time stamp 0x3F4B1A8A: no-CD exes, changed headers - since 2026-10-22, as the single-DLL DesertStorm2Fix did; every hook checks its bytes, pse.ids its signatures, and the Direct3DCreate9 import hook needs the slot to hold d3d9.dll's function). The retail SecuROM exe (`CDS II - Original.exe`, `44EF1E16…95EF`, same PE time stamp + size) is refused: its code calls imports through SecuROM's own thunks (IAT at 0x91C000), so the layer's import / call-site hooks don't fit, and it won't start without the disc anyway |
| `pse.ids` database | 115 ids (`nodisc-1.0`), all published, every signature checked at start-up (`115 resolved, 0 mismatched`) |
| `pse.game.ds2` v1 | front-end state, dispatch mode, level name / kind, cutscene bit, split-screen views, window, exe hash |
| Core events / messages | the same set as DS1 (table below), plus `PSE_MSG_EXIT` from the layer itself |
| Co-save | `<save>.pse` next to the game's `.sav`, NEW_GAME, revert – same rules as DS1 |
| Log | the loader's `PSE\Logs\loader.log` (every loader / plugin line) while no host plugin takes the lines |
| Host plugin | the layer allows `ds2fix` (DesertStorm2Fix as a host plugin = the DS2 product, [below](#desertstorm2fix-as-a-host-plugin)) |

Verified 2026-10-19 – pse_tests (9 DS2 tests, below) and live runs on a retail install (disc-free exe, DesertStorm2Fix's `dinput8.dll` backed up and put back after each run):

- `-d Mission01.dll` boot (test harness, off-screen): the three example plugins load (`hello_client: example.hello says
  2 + 3 = 5`), `DEVICE_CREATED`, `FRAME`, `NEW_GAME mission01.dll`, `LEVEL_LOAD_BEGIN / END mission01.dll`,
  `PRE_RENDER` / `POST_RENDER` / `END_SCENE` / `PRESENT` every frame (15 000 frames), then the harness saved through the
  game's own `Save_Game` (`SAVE_BEGIN`, co-save chunk written, `SAVE_END`, `PSE DS2 Test.sav` + `.pse` on disk) and
  loaded it (`LOAD_BEGIN`, the chunk back, `LOAD_END`), closed the window: `LEVEL_UNLOAD`, `EXIT`, every plugin's
  `Unload`, exit code 0;
- normal start (logo films, menus): `LEVEL_LOAD_BEGIN / END FrontEnd`, `FRONT_END_READY FrontEnd`, no `NEW_GAME`;
- `pse.game.ds2` read in game: menus `level "FrontEnd" kind 1 front-end state 0x2 dispatch 0`, Mission 1
  `level "MISSION01" kind 2 front-end state 0x13 dispatch 1`, window yes, exe known 1;
- DesertStorm2Fix's features as the host plugin `ds2fix` on the same layer (its own background mode, Mission 1):
  every feature applied under its sub-plugin, the front-end hooks chained behind the layer's level hooks, the mission
  cutscene rendered (back-buffer screenshot), `EXIT` + unload at WM_CLOSE.

## Getting it

```c
#include <pse/pse_game_ds2.h>

const PSEGameDs2V1* game = (const PSEGameDs2V1*)pse->Query(pse->plugin, PSE_GAME_DS2_INTERFACE, 1);
if (!game || game->size < sizeof(PSEGameDs2V1)) return 0;   /* not Desert Storm II, or a host too old */
```

Served by the built-in plugin `pse.game.ds2` (1.0.0, `PSE_PLUGIN_INTERNAL | PSE_PLUGIN_CLIENT_ONLY`), registered by the
loader before any plugin: `requires = pse.game.ds2 >= 1.0` in a manifest refuses the plugin on any other game. A plugin
for both games asks for `pse.game.ds1` and `pse.game.ds2` and uses whichever it gets (`pse->gameId` says which game).

## Functions

Game thread unless noted; reads are fault-guarded (a stale pointer gives 0 / "").

| Function | Returns |
| --- | --- |
| `uint32_t FrontEndState(void)` | u32 `[0x63433C]`: 2 main menu, 0x13 a mission is running, other values = other screens; 0 before the front end |
| `uint32_t DispatchMode(void)` | u8 `[0x627158]`: 0 front end, 1 mission running, 2 a level load is pending / running (the whole load is one frame), 3 a film / briefing entry of the campaign flow, 4 other. **Not DS1's numbering** (DS1: 0 = front end or mission) |
| `uint32_t LevelName(char* buf, uint32_t bufLen)` | the scene name the world object points at (`char*` at `[0x68008C] + 0x1BBC`): `"FrontEnd"`, `"MISSION01"`, `""` before the first load; full length returned, buf NUL-terminated |
| `uint32_t LevelKind(void)` | `PSE_DS2_LEVEL_NONE` 0, `FRONTEND` 1 (`"FrontEnd"`), `MISSION` 2 (anything else, training too) |
| `int InCutscene(void)` | bit 0 of `[[0x68008C] + 0x1B8C]` (the render loop's full-screen cinematic flag) |
| `uint32_t SplitScreenPlayers(void)` | the game's split-screen object `0x626C18`: its player count (2–4) while its flag is set, else 1 (no provider needed - DS2 keeps the console code) |
| `void* GameWindow(void)` | HWND `[0x62F41C]` (class `"CDS II"`); any thread |
| `const char* ExeSha256(void)` / `int ExeKnown(void)` | as DS1: the file's SHA-256 (LAA bit cleared) / 1 = the disc-free 1.0; any thread |

Not in v1 (addresses known, waiting for a user): text lookup (`Text_Lookup` 0x4FBE50 returns UTF-16), local players /
soldiers (`Input_Blocks` 0x62FF7C, 0x58C bytes each, soldier +0x2EC – the soldier's position / health offsets aren't
mapped yet), online.

## Events and messages

Installed by `PSEGame_Init` (`pse/games/ds2/ds2_events.cpp`) through the hooks broker, owner `pse.game.ds2`; INNER /
OUTER = `PSE_DS2_LAYER_PRIORITY_INNER` (-1000) / `_OUTER` (1000), the same meaning as DS1's.

| Site | Hook | Priority | Sends |
| --- | --- | --- | --- |
| `0x414BFC` call `Main_FrameDispatch` (message loop, only while the window is active) | call site | INNER | `PSE_MSG_FRONT_END_READY` (first frame with dispatch mode 0 after the front end loaded), tasks + `PSE_EVENT_FRAME` |
| `0x410990` call `Render_Frame(1)` (`Game_Update`) | call site | INNER | `PSE_EVENT_PRE_RENDER` / `POST_RENDER` – **missions only** (the front end draws through its own path) |
| `0x42CF9B` call `FrontEnd_Init` (thiscall) | call site | OUTER | `PSE_EVENT_LEVEL_LOAD_BEGIN` / `END`, data `"FrontEnd"` |
| `0x42CB64` call `Level_Init(name)` (`Mission_Load`) | call site | OUTER | `PSE_MSG_NEW_GAME` (+ revert callbacks) when started from the front end, `LEVEL_LOAD_BEGIN` / `END`, data = the flow entry's name (`"mission01.dll"`) |
| `0x42CF1E` call `Level_Unload` (`Level_LoadDispatch`) | call site | OUTER | `PSE_EVENT_LEVEL_UNLOAD` (the loaded level's name) |
| `0x40E8E0` WM_DESTROY: call `0x40D730` | call site | OUTER | `LEVEL_UNLOAD`, then `pse.host Shutdown` = `PSE_MSG_EXIT` + external plugins unloaded (once) |
| `0x41C600` `Save_Game` entry (10-byte prologue) | function | OUTER | `PSE_MSG_SAVE_BEGIN` / co-save / `PSE_MSG_SAVE_END` |
| `0x41C7C0` `Save_Load` entry (10-byte prologue) | function | OUTER | `PSE_MSG_LOAD_BEGIN` / co-save / `PSE_MSG_LOAD_END` |
| `0x5DA2C4` IAT `Direct3DCreate9` → `IDirect3D9::CreateDevice` (slot 16) | pointer | OUTER | `PSE_MSG_DEVICE_CREATED`; hooks the device slots below |
| device `TestCooperativeLevel` (3) / `Reset` (16) / `Present` (17) / `EndScene` (42) | pointer | INNER / OUTER / INNER / INNER | `DEVICE_LOST` / `DEVICE_RESET` / `PSE_EVENT_PRESENT` / `PSE_EVENT_END_SCENE` (data = the `IDirect3DDevice9*`) |

Differences from DS1 a cross-game plugin should know: the device in `END_SCENE` / `PRESENT` / `DEVICE_*` is D3D9, not
D3D8; level names are the game's own (`"FrontEnd"`, `"mission01.dll"`; DS1: `"FrontEnd.dll"`, `"Mission1.dll"`); no
render events in the front end; `EXIT` comes from the layer (DS1: from DesertStormFix).

### Saves

The game's saver `[0x62F458]`: `Save_Game(path, flag)` writes `path + ".sav"` (flag ≠ 0 or no path = nothing written),
`Save_Load(path)` reads `path` with its extension replaced by `".sav"` (`_splitpath` rules – `"Act 1.5"` loads
`"Act 1.sav"`, as the game does). Paths are UTF-16 in the game; the messages carry the `.sav` file as UTF-8. The
co-save is `<name>.pse` next to it (format: [serialization.md](serialization.md)); a co-save left over from a save
deleted outside the game is removed before a new save of that name. `pse/games/ds2/ds2_saves.cpp`.

### Log

The loader writes every line no host plugin takes (its fallback, every build): `PSE\Logs\loader.log`, rewritten every
start, the start-up lines from before the layer included. A host plugin that sets its own sink takes over (ds2fix dev
builds: `DesertStorm2Fix.log`; ds2fix release builds have no log and leave `loader.log` running).

## Tests

- `pse/tests/test_game_ds2.cpp` (pse_tests): the built `pse_ds2.dll` detects exactly the disc-free exe (not the SecuROM
  one, not DS1; `pse_ds1.dll` refuses DS2); the ds2 database (table rules, one build, published names); co-save path
  rules + NEW_GAME + save / load; `pse.game.ds2` registration; on the **mapped CDS II.exe** (`exe::MapGame("ds2")`,
  `PSE_REQUIRE_EXE_FOR("ds2")` – `pse/tests/exe_image.*` maps one game's exe at a time at 0x400000): every signature,
  every id resolving, IAT names, call sites calling their functions, the layer's hooks (sites, kinds, priorities,
  prologues) and a priority-0 hook chaining in front of the frame hook, the interface's reads; and `pse::Start` with
  **both** `pse_ds1.dll` and `pse_ds2.dll` in `Games`: the loader picks `pse_ds2.dll`, frees `pse_ds1.dll`, loads
  `example.hello` under `ds2`, the loader's fallback log has every line.
- Live: `pse/tests/ds2_harness/` = `test.ds2_harness` (TEST ONLY, never shipped): DesertStorm2Fix's dev background mode
  as a plugin (off-screen window, launcher OK'd, windowed device, muted then unmuted at `EXIT`, no cursor clip, "press
  any key" skipped, message boxes answered), `HKCU\Software\DesertStorm2Fix\Dev` `Background` = 1 + `PseHarness` =
  level (`"FrontEnd"` = normal start) [+ `PseHarnessSave` = save path without extension]: boots the level, saves +
  loads, logs `pse.game.ds2`, closes the window. The runner used for the results above (`tools/ds2/pse_live.py
  <out> [level] [s] [--nosave] [--ds2fix]` – local, gitignored; was `../ConflictDesertStorm2Patch/tools/` before the
  2026-10-19 repo merge; env `BUILD` = the root build dir, `DS2FIX_BUILD` = where `ds2fix.dll` is) takes the DS2 agents' lock `<game>\.agent-test-lock`, backs up and restores
  `dinput8.dll/.pdb`, the logs, the dev values, `HKCU\Software\PSE\ds2` and the game's Device Settings.

## Build

CMake target `pse_ds2` (`pse/CMakeLists.txt`). `pse_tree_ds2` assembles `build*/tree-ds2/<config>/` = the shim
`dinput8.dll`, `PSE\pse_loader.dll`, `PSE\Games\pse_ds2.dll` and the three example plugins – the same files as in the
DS1 tree – and copies it into `DS2_GAME_DIR` when that is set (it replaces the folder's `dinput8.dll`). That tree is
for the layer's own tests; the DS2 **product** is built by `games/ds2` ([below](#desertstorm2fix-as-a-host-plugin)).
Never deployed into the DS1 install.

## DesertStorm2Fix as a host plugin

Since 2026-10-22 the DS2 release is DesertStorm2Fix on PSE (user decision: it replaces the standalone `dinput8.dll`,
which still builds until ds2fix has matched it in every test, then goes). `games/ds2`, CMake option
`DS2_PSE_PLUGIN=ON` (root presets `ds2-pse` → `build-ds2fix`, `ds2-pse-dist` → `build-ds2fix-dist`; local
`ds2-pse-local` deploys into the install):

| In the tree / package | Built from |
| --- | --- |
| `dinput8.dll` | the PSE shim (`pse/shim`, target `ds2_pse_shim`) |
| `PSE\pse_loader.dll` | `pse/loader` + `pse/loader_dll` (`ds2_pse_loader`) |
| `PSE\Games\pse_ds2.dll` | `pse/games/ds2` (`ds2_pse_layer`) |
| `PSE\Plugins\ds2fix.dll` + `.pse` | `games/ds2/src/pse/` + the features (`ds2fix`; `host = true`, `requires = pse.game.ds2 >= 1.0`) |
| `PSE\Plugins\<name>.dll` + `.pse` | every `games/ds2/plugins/<name>/` ([DS2 plugins](#ds2-plugins)) |
| `DesertStorm2Fix-Patcher.exe` | the 4 GB patcher: repo root `src/patcher/patcher.cpp` with `PATCHER_GAME_DS2` (target `ds2_patcher`, [below](#ds2fixmemory-gamesds2pluginsmemory-2026-10-22)) |

The shim, loader and layer are compiled from the repo's `pse/` sources inside the DS2 project (a `PSE_GAME=ds2`
configure has no root PSE project) with DS1's ship options (`ds2_ship_target`: dev = PDB; release = no PDB / timestamp /
Rich header; version resource). Every build assembles `build*/tree/<config>/` (target `ds2_pse_tree`,
`pse/cmake/assemble.cmake`); `DS2_GAME_DIR` copies it into the game (changed files only; it replaces the standalone
`dinput8.dll`; `DS_GAME_DIR` = the standalone deploy - both at once is refused). `DS_DIST`: `package/` = the tree
without PDBs + `DesertStorm2Fix-README.txt` (`games/ds2/dist/`), zipped as `DesertStorm2Fix-<ver>.zip`; the standalone
build's package moves to `package-standalone/` + `-standalone.zip` (its readme: `dist/DesertStorm2Fix-standalone-README.txt`).

What `ds2fix` does (`games/ds2/src/pse/ds2fix.cpp`):

- `PSEPlugin_Load` = the standalone `Init`: settings, then every feature's `Apply` inside its sub-plugin's owner scope
  (`ds2fix.cinematics`, `.menus`, `.splitscreen`, `.coop`, `.graphics`, `.devbg`, `.mods`, `.modsmenu` ...: internal plugins,
  SIMULATION / CLIENT_ONLY as on DS1; widescreen / HUD / front end / sharper text are the external plugin
  [`ds2fix.widescreen`](#ds2fixwidescreen-gamesds2pluginswidescreen-2026-10-22) since 2026-10-22);
- patching = DesertStormFix's broker-backed `src/core/patch.cpp` (swapped in through `games/ds2/src/pse/shim/core/`);
  hooks on the layer's sites chain (`SubscribeCall` on `0x42CF9B` / `0x42CB64`, `HookSlot` on the `Direct3DCreate9`
  import). The `IDirect3D9::CreateDevice` slot and the device slots ds2fix needs (Reset, Present, Clear, SetViewport,
  SetTransform, GetViewport, SetRenderTarget, SetScissorRect, the four Draw* calls) are broker pointer hooks per COM
  class (`src/core/comhooks.h`, owner `ds2fix.graphics`, priority 0): the layer's CreateDevice / Reset hooks (OUTER)
  run before them, its Present hook (INNER) after; the dev background mode's MessageBoxA import slot is a `HookSlot`
  too. Still raw (as on DS1: not code pointers): the start-up options pointer, the layout clamp immediates, and
  `menus.cpp`'s five game-vtable slot writes (`patch::WriteValue` - the broker doesn't list them; to move to `HookSlot`);
- settings = `pse.settings` of `ds2fix`: `HKCU\Software\PSE\ds2\Plugins\ds2fix` (`games/ds2/src/pse/settings_pse.cpp`;
  same names / ranges as the standalone reader, types + flags declared like DesertStormFix's). The first start copies
  every value of the standalone build's key `HKLM\SOFTWARE\SCi Games\CDS II\Enhancements` (virtualized: the user's
  VirtualStore copy) once (`pse::MigrateRegistryValues`, marker `MigratedFromEnhancements`); the old key is never
  changed, so the standalone build keeps its own. The game's `Device Settings` stay where the game keeps them; dev
  values stay in `HKCU\Software\DesertStorm2Fix\Dev`. A new `settings::Values` field goes into both readers;
- log: dev builds take the loader's sinks (`DesertStorm2Fix.log`; `DesertStorm2Fix-test.log` in background runs) and
  list the broker's records at the first menu frame and after each mission load (`[dev] hook …` lines, then `[dev] PSE
  hooks (<level>): n records, owners …` - the DS1 console's `pse hooks`); release builds have no log of their own and
  leave every line to the loader's `PSE\Logs\loader.log`;
- `PSE_MSG_EXIT` + the plugins' unload come from the layer at WM_DESTROY (nothing in ds2fix).

### Mods folder (`ds2fix.mods`, `games/ds2/src/features/mods.cpp`)

The basic resolver (2026-10-22; modder page [ds2-mods.md](../ds2-mods.md)): `<game>\Mods\<mod>\**` indexed by upper-case
file name at start-up (name order, later wins, `_` / `.` folders off), report `Mods\DesertStorm2Fix-report.txt`
(`features::ModLog`), setting `FeatMods` (default 1). Port of DS1's resolver part. Tables / texts / `.weapon` /
`.vehicle` / `.unit`: see "Tables and texts" below.

| What | DS2 | DS1 |
| --- | --- | --- |
| resolver (cdecl name, search paths, flags → path in `0x67CFA8`, 0x200 B, or 0); flags `[0x665E4C]` bit 0 archive mode (returns the bare name when the archive has it), 0x40 pass-through, 1 archive build | `FUN_004c3c40` (entry `A0 4C 5E 66 00`), entry hook | `FUN_004be810`, `0x63BF28` |
| open (resolve + `FUN_004c3140(path, "rb")`), whole-file reader (name, &buf, &size, heap, pool), open wrapper, exists check (name, flags) | `FUN_004c4000`, `FUN_004c49e0` (47 callers), `FUN_004c49c0`, `FUN_004c49a0` | `FUN_004bebf0`, `FUN_004bfb90`, `FUN_004be7f0` |
| opener thiscall(file, path) ret 4: flags → file +0x140; archive mode = hash (lower-case, `FUN_004c0b30`) looked up in `[0x665E54]` (`FUN_004c2320`); one file object `[0x665E48]` | `FUN_004c2ea0` (entry `81 EC 08 02 00 00`), entry hook: Mods paths open with flag bits 0 + 1 cleared (disk branch, fopen "rb") | `FUN_004bd8a0` |
| "in the archive?" cdecl(name) → 1 / 0 (resolver, character model preload `0x457411`) | `FUN_004c2810` (`81 EC 08 01 00 00`), entry hook: 1 for a mod's file | `FUN_004be360` via `0x4BFC89` |
| texture loader: extension tries .ddl (option `c` / low res) .dds .png .bmp .tga through the exists check | `FUN_005427b0` | `0x548EF4..` |
| per-texture load thiscall(texture): name +8, memory buffer +0x30 / size +0x34 (skips the file) | `FUN_00542f30` (`81 EC 24 01 00 00`), entry hook drops the buffer when a mod has the name (not seen used in Mission 1) | `FUN_00549a10`, +0x4C / +0x50 |
| Global Heap `[0x664380]`: ctor `FUN_004c0650` thiscall(size, name) called at `0x4136FC` with 1 024 000; +0x48 size, +0x4C free, +0x50 used, +0x58 buffer; alloc `FUN_004c0750`, mark / release `FUN_004c0810` / `FUN_004c0830`. Texture readers `0x5421F0` / `0x5422A0` / `0x542E20` / `0x5427B0` read whole files into it; too big = no texture ("Failed to allocate memory for file" goes to a stubbed print) | call hooked: + largest mod file (rounded to 1 MB, cap 64 MB) when it is > 256 KB, on top of what arrives; with the plugin [`ds2fix.memory`](#ds2fixmemory-gamesds2pluginsmemory-2026-10-22) it passes through (that plugin sizes the heap) | `0x40FFD7`, memory.cpp |

#### Tables and texts (`games/ds2/src/features/mods_tables.cpp`, part of `ds2fix.mods`, 2026-10-22)

Additive `<TABLE>.txt` / `<TABLE>.replace.txt`, `.strings`, `.weapon`, `.vehicle`, `.unit` (modder page
[ds2-mods.md](../ds2-mods.md)). mods.cpp calls `features::BuildModTables()` after every index build (start-up, MODS
menu Apply); merged tables and retextured model copies go to `%TEMP%\DesertStorm2Fix\merged\<content hash>\` and into
the index through `features::SetModFile` (replaces a mod's own file of that name) + `features::AddModRoot` (opener's
disk branch). Start-up tables are parsed once (loader `FUN_0040b360`, parsers listed in games/ds2/PORTING.md "Mods:
tables"); the MODS menu's Apply reports them as "restart". Rows the parsers would quit on are dropped with `[fail]`;
Units col 15 / Vehicle.txt token 51 = the unit's UnitWpns / the vehicle's VehWpns rows. Texts: menus.cpp's hook on
`Text_Lookup` (`FUN_004fbe50`, id 19) asks `features::ModText(hash)` for every hash DesertStorm2Fix doesn't own; hash =
archive LFSR over the lower-cased key (the bundles store no key names), `[Mission01]` / `[M01]` sections follow the world
scene name (`[0x68008C]` +0x1BBC). DesertStorm2Fix's own menu texts: `0xC0B2xxxx` (menus.cpp).

**MODS menu** (sub-plugin `ds2fix.modsmenu`, `games/ds2/src/features/modsmenu.cpp`, 2026-10-22): main menu SINGLE
PLAYER / CO-OP / MODS / OPTIONS / QUIT (menus.cpp, item id 0x405) → front-end state 0x37 (`menus::kModsState`, an
object of the single-player menu class like the co-op screen 0x36): 7 mod rows (`NAME: ON / OFF`, scrolling for the
keyboard / pad navigation the list read reports: input block +0x14C / +0x88 up, +0x150 / +0x8C down, bit 4) + an
Apply row. Toggle = folder rename `_Name` ↔ `Name`; Apply = `features::ReloadMods` (index + report rebuilt; changed file
names the resolver was asked for outside a mission = restart needed, mission-only ones = next level). In a mission =
the world's scene name `[0x68008C]` +0x1BBC is not "FrontEnd" (set at `0x41279C` by mission init, `0x42C94F` by the
front-end init; the front-end state `[0x63433C]` reads 0x13 at start-up too). The resolver hooks go in whenever FeatMods
is on (also with no mods); the heap is sized from every folder (on or off). Main menu line: main menu vtable +0x14
(`0x462000`, the menu classes' shared draw) replaced in the main menu's vtable only; text = font `[0x62F43C]`
(`FUN_00526e40` alpha, `FUN_00526e20` colour, `FUN_00529490(text, x, y, 1)`: x = left, y = middle of the line, in the
front-end's 1280 x 1024 virtual screen; drawn only when the 2D screen is 1280 x 1024). Item descriptions are typed
once per highlight (copied), so they stay fixed. Dev `Dev\ModsOpenLog` = 1 logs every name the resolver is asked for.
Verified 2026-10-22 by background runs (PSE + standalone, 1920x1080 / 2560x1440, `MenuKeys`): toggles rename, Apply
re-indexes (report), scrolling with 10 mods, empty state, restart case (a `WEAPS.TXT` mod), main menu lines.

Entry hooks = broker `HookFunction` in `ds2fix`, a local trampoline in the standalone build. Not covered: the game's
own fopens (writers; Bink films and music streams: `ds2fix.modsound` below), level DLLs (`LoadLibrary`). Verified 2026-10-22 by background runs, both
builds (Mission01): a 2048 x 1024 replacement of `SAS_LAND_ROVER_1.DDS` (heap 1 024 000 → 3 121 152) + tinted
`_2` / `_3`, a copy of `SAS_LANDROVER_WITH_MK19_SAW.EVO` whose wheel slot names a mod-only `DS2MODTEST_WHEEL.DDS` (green
spare wheel on screen), the same folder `_`-prefixed = vanilla.

Verified 2026-10-22 against the standalone `dinput8.dll` by background runs (front end, Mission 1, 2-player split
screen at 1920x1080 and 3440x1440, the co-op menu flow): same feature log lines and shot counts, matching screenshots;
79 broker records, every one owned by `ds2fix.<feature>` or `pse.game.ds2`; settings migration; the release package
boots to the main menu (games/ds2/PORTING.md "PSE"). Not yet: a human play-test of the package.

### `.skin` files (`ds2fix.skins`, `games/ds2/src/features/skins.cpp`)

Modder page: [ds2-mods.md](../ds2-mods.md#skin-files--a-soldiers-own-textures-body-and-name); every address and the
research: `games/ds2/PORTING.md` "Skins". Internal SIMULATION sub-plugin (bodies change hit geometry), on with
`FeatMods`; reads `*.skin` from the mods index at start-up and after the MODS menu's Apply (`features::ReloadSkins`).

| What | DS2 | Hook |
| --- | --- | --- |
| CHLOGrunt ctor thiscall(scene record, unit) ret 8; the HLO base ctor `FUN_004cf170` makes the render instance from record +0x20 (model name, char[0x44]) / +0x64 (loaded model) via `FUN_004cdef0` | `0x4835D0` (entry `8B 44 24 08 83 EC 38`) | entry detour: a `.skin` with `body` puts the body into the record for the call (restored after) - the instance is built on it, any key count |
| skin apply thiscall(soldier, skin entry) ret 4 (every soldier at the end of his ctor; entry null for non-squad) | `0x475020` (`8B 44 24 04 53`) | entry detour: squad members with names / portrait get a private copy of their entry first; after it the soldier's record {model, texture, part textures} is made (a body for a soldier made elsewhere: CAnimInstance::SetModel `0x55F6D0`, same key count only) |
| skin draw: `FUN_00556290(skin data, texture)` per skinned object of instance +0xC8 (next +0xE0) | call `0x55F334` in `FUN_0055f1f0` | call hook through a naked stub (esi = instance, edi = object): the record's texture for that object when the draw has none of its own; textures not yet loaded are loaded on their first draw (`FUN_00543430`) |

Rule matching: unit +4 name (case-insensitive; hero names also match their `_Training` unit), `squad` = unit +0x1CF ≥ 0,
uniform `[0x627154]` (0 SAS, 1 Delta); the last matching `.skin` (mod folder order, then file name) wins. Part names =
object name hash at object +0xDC, matched against the hashes in the mod body's `.evo` headers (read by the plugin).
Report lines go to `Mods\DesertStorm2Fix-report.txt` (`features::ModLog`): one per `.skin` read, one per unit dressed,
`[fail]` for a missing texture / body, unknown key, part number or name.

### Weapon pictures (`ds2fix.weaponicons`, `games/ds2/src/features/weaponicons.cpp`, 2026-10-23)

`.weapon` `hud icon = <picture file>` (modder page [ds2-mods.md](../ds2-mods.md#weapon-pictures)); internal plugin,
CLIENT_ONLY, PSE build only, applied before `ds2fix.mods` (mods_tables.cpp asks `features::AddWeaponIcon(path, base
picture, file)` → n, writes `DS2FIX_ICON_<n>` into Weaps col 5; -1 = not available → base picture + `[fail]`;
`features::WeaponPictureKnown(name)` checks a game picture name against the lookup's compare chain without calling it).

| What | DS2 | DS1 |
| --- | --- | --- |
| picture lookup: name in **eax**, image number in ax, plain `ret`; unknown = warning box `FUN_004cd600` "unable to find interface ID" + 0xFFFF. Callers Weaps 0x453170 (→ weapon def +0x84), Units 0x454AED, Inven 0x455546 | `FUN_00452170` (entry `56 57 BF 64 55 5E 00`), broker entry hook (naked stub: `DS2FIX_ICON_<n>` → 1000 + n, else the trampoline with eax intact) | `FUN_004966c0` (cdecl) |
| interface sheet ds_int.img (CSpriteRender 0x50 B, ctor `FUN_0052b290`, read `FUN_0052ad20` at 0x41448F, clean-up `FUN_0052ac30` at exit frees +4 / +8 / +0xC and each record's +4): +0 header (+6 images 178, +0xA pages 6), +4 records 0x1C B (+0 page, +4 owned ptr, +0xA x, +0xC y, +0xE w, +0x10 h, +0x12 / +0x14 offset, +0x18 name hash), +8 UVs 16 B, +0xC page objects (+0xC palette, -1 none; +0x10 IDirect3DTexture9*) | `[0x62F434]` | GWInt `[0x60EE18]` (24-byte records, no page objects) |
| image draws: all 15 bind `pages[record.page]+0x10` via SetTexture wrapper 0x5364E0 (0x52BA5E … 0x52E284) | not hooked: our images get page objects of their own | DS1 hooks the bind |

At the first frame (PSE_EVENT_FRAME) with the sheet and the device: records / UVs / pages copied into arrays from the game's
allocator `FUN_004be320` (the exit clean-up frees them), images 1000.. = the base picture's record sized to the file
(UVs over it), on pages 6.. = small page objects whose texture is the picture (WIC → managed A8R8G8B8). No texture-bind
hook, so nothing is shared with `ds2fix.widescreen`'s sharp-text hook at 0x52BA5E; the header counts stay the game's.

### Levels from mods (`ds2fix.modlevels`, `games/ds2/src/features/mods_levels.cpp`)

Modder page: [ds2-mods.md](../ds2-mods.md#levels-and-missions). SIMULATION sub-plugin, on with `FeatMods`; port of
DS1's mods.cpp `BuildReferences` / scripthost.cpp level hooks. `features::BuildModReferences` runs after every index
build (mods.cpp, start-up + MODS Apply) and adds built files through `features::AddBuiltModFile` (mods.cpp's index;
the opener hook opens that folder from disk like Mods\). Level init = `FUN_00412690` cdecl(name) [DS1 `FUN_0040c090`].

| What | DS2 | Hook |
| --- | --- | --- |
| mission DLL load `FUN_00422020` stdcall(name) (from `0x412BAB`, `*.dll` levels; GetProcAddress "LevelSetup") | `call [LoadLibraryA 0x5DA054]` at `0x42202D`, handle `[0x62FE04]` | indirect-call hook: a mod's / built DLL by full path |
| level archive "<name−4>.dat" `FUN_004c3400`(slot 1, name, a, b) | `0x41276F` (0, 0), `0x412879` (1, 0), `0x4130D0` (1, 0) | call hooks: `<stem>.level` `--@archive` |
| sound cache "<stem>.sch": strcpy `FUN_004c2300`(src, dst), then SetSoundCache `0x4B5DE0` at `0x4129DA` | `0x412963` | call hook (name) |
| music bank "<stem>Music.sbk" `FUN_004b33c0` thiscall(sound mgr, name, 0) ret 8 | `0x412A49` | call hook |
| physics pools `sprintf("%s.FAS", world +0x1BBC)` in `FUN_00509660` (from `0x4127BB`) | `0x5096BB` | call hook |
| level texts "<STEM>.BLF" (compiled locale): level init `FUN_0040f820`(world `FUN_004e2b70`("BLF"), 1) → `FUN_004fc3d0`; also CLocale `FUN_004fc600` thiscall(`[0x62F460]`, name) at `0x40F9D4` / `0x42CE97`, `FUN_004fc3d0` at `0x41F36A` (campaign +0x18 name) | `0x4127E1`, `0x40F9D4`, `0x42CE97`, `0x41F36A` | call hooks: a level on another level's archive (`.level` / `.lua` `--@archive`) loads that level's BLF - without one the game had no texts at all ("\*no string\*" on the end screen) |
| level stem: `FUN_004c3a90(name)` upper-cased into `0x62F7E0` = world `[0x68008C]` +0x1BBC; scene name +0x1BB4 (set by the scene load) | `0x412775` | - |
| script info "<scene>.si" `FUN_00432cc0` thiscall(`0x631848`, name) in LoadEnvironment `FUN_00411970` (`0x411CD7`), only with a campaign entry (`[0x631618]` != -1); entries `[0x631848 +0x54]` × 0x54 B, count +0x58, flag +0x18 (API +0x1F0 → `FUN_00440430` → `FUN_00436290`) | debug print `0x4031A0` at `0x411CE1` | call hook: loads it for a mod DLL level outside the campaign (null array crashed at `0x43629E`) |
| octree: start flag `FUN_004c0ce0`('c') at `0x412E64` → `FUN_004ea870`(1) build + write (fopen "wb" `0x58C6C3` at `0x4EA8B8`, name `FUN_004e2a80`("OCT")), free `FUN_004e2800` `0x412E99`, load `FUN_004ea9b0` `0x412EB1`; 'd' (shipped "-fl -d") = load only | as listed | mod scene → yes; file to `%TEMP%\DesertStorm2Fix\mod-scene-*.OCT`; free + load skipped |
| AI navigation: Routing_Load `FUN_004bcd10`(scene, `[0x61BB88]`) at `0x412FE7` (`.qtd` `FUN_004bc220`, `.rmd` `FUN_004ba9e0`); else build `FUN_004bce70`(world) at `0x413003` (quadtree `FUN_004bcb70`, markers `FUN_004bc9e0`, router) | as listed | mod scene without own .QTD / .RMD → load answers 0; after the build the hand-placed markers merged back |
| marker set `[0x680100]` {+0 array (count in front), +4 count, +8 islands}; marker 32 B {x, y, z, weight, u16 link[5] +0x10, u16 flags +0x1A (bit 0 generated), u8 island +0x1C}; alloc `FUN_004ba020` thiscall(set, n) ret 4, free array `FUN_004ba0f0`, islands `FUN_004bac30`; quadtree `[0x6800FC]` node 0x14 B {+0 flags (2 walkable), +2 marker, +4 children}; router `[0x6800E8]` ctor `FUN_004b9340`(n); `.rmd` v3 marker 29 B {x, y, z, weight, link[5], flags, u8} | - | `MergeHandPlaced` |

Verified 2026-10-22 by background runs (scratchpad runner `ds2modlevels/run.py`, PSE dev tree, Mission 1): `.dllpatch`
built + loaded from `%TEMP%`, `.envpatch` scene served, octree rebuilt (47 ms), navigation 918 generated + 426
hand-placed markers (shipped: 920 + 426), mission plays; bad files → `[fail]` lines, no crash; one copied soldier
appended to the scene: plays with the rebuilt octree, stuck letterboxed with nothing of the world drawn with the shipped
`.OCT` (the "stuck cinematic" from the limits test = stale octree).

### Lua missions (`ds2fix.scripthost`, `games/ds2/src/features/scripthost.cpp` + `campaigns.cpp`, 2026-10-23)

Modder page: [ds2-mods.md](../ds2-mods.md#lua-missions-and-custom-campaigns). SIMULATION sub-plugin, on with `FeatMods`
(also in the standalone `dinput8.dll`, without plugin Lua functions). Port of DS1's scripthost.cpp / campaigns.cpp;
Lua 5.4.7 (`third_party/lua`, CMake target `ds2_lua`, linked into both builds). pse.lua: `lua_bridge.h` attaches with
ds2fix's load interface (the bridge is game-neutral) - plugin namespaces, `CallGlobal`, `pse.lua.level_start` / `_end`
work as on DS1.

| What | DS2 [DS1] | Hook / use |
| --- | --- | --- |
| level init `FUN_00412690` [`0x40C090`]: "dll" → `FUN_00422020` (LoadLibrary, LevelSetup(&api)), "env" → `FUN_004220c0` + LoadEnvironment `FUN_00411970`, else the text manifest `FUN_00421f40` thiscall(`[0x62F420]`, name) | call `0x412C51` [`0x40C405`] | `.lua` → the script |
| script API table | `0x60D638`, 0x9F0 B = 636 cdecl entries, LevelSetup checks `api[0](1, 7, 0x9F0)` [`0x5ED238`, 0x8D8] | `game.call(slot, ...)` |
| script object (6 cdecl callbacks: init, update, message(id, param*), save(buf) → buf+n, load(buf) → buf+n, size()) | `[0x62FE00]`, module `[0x62FE04]` [`0x60F578`] | update `0x4104DD` (only while campaign `[0x62F454]` +8 = 0), save `0x41BEBC`, load `0x41C3D6`, size `0x46487F` (0 = SAVE greyed) |
| unload `FUN_00422090` (clears the object only with a module) | call `0x40D7A8` | object taken back, Lua state closed |
| level info (`<scene>.si`: objectives, areas) `FUN_00432cc0` thiscall(`0x631848`, name) | read by LoadEnvironment only for campaign levels | with `--@base`: the base scene's `.si` after its scene load (API +0x70 swapped during LevelSetup) |
| campaign objective list (`CMission::AddObjective` `FUN_0042f9e0`, list +0x12E8, entry +0 index, +0x14 flags 2 shown / 4 done / 8 tracked, next +0xD8) | cdecl `0x440310`(title) → index in al, `0x440330`(i, desc), `0x440390`(i) show + track, `0x440520`(i) done (sound 0xCD4), `0x440360`(i, pos*), `0x440560`(i, id); lookup `0x42ED20` | not in the API table (the `.si` reader calls them) - `game.add_objective` ... |
| `.si` objectives: API +0x5AC **activate** (state 0 → 1), +0x5B4 **complete** (→ 2, message 0x4F, then its result), +0x5BC **deactivate**, +0x5B0 / +0x5B8 state is 1 / 2 | `0x436620` / `0x4366B0` / `0x436290` | (ModKit api_slots.json calls +0x5AC "complete" - it activates) |
| end mission API +0x58C → campaign `FUN_00430810`(result) (+8) [+0x48C] | | `game.end_mission` |
| timers API +0x6CC (id, float ms, 0, repeat) → message 0x16 [+0x5A4] | event queue `[0x6800B8]` | held back until the first update |
| campaign flow `0x631608`: +8 one-mission flag (training), +0x10 entry (-1 = custom, name at +0x20), +0x14 state (2 = load), +0x120 custom name set; select by name `FUN_00430ec0`(name, 0), set custom name `FUN_0040a890`, front end `FUN_00430cf0`, advance `FUN_00430fe0`, current name `FUN_00430900` | | the 12 calls of the advance hooked (`0x42CC60` `0x42CC76` `0x42CE3A` `0x4302D8` `0x43FBF1` `0x45EC8A` `0x46153F` `0x461852` `0x464BD5` `0x464FA6` `0x46B4E6` `0x46D749`): after a `.lua` level → the next level / main menu |
| dispatcher: no entry + no custom name → `FUN_0040c830` (Pivotal's level-select dialog) | call `0x42CF3B` | restart / LOAD GAME of a custom level: campaign +0x18 (the saved level's name), else flow +0x20 |
| front-end frame: the current screen's update result → flow state `[0x63161C]` (2 = load the flow's level) | call `0x40E73C` (`FUN_0043fb50`) | 2 once when a campaign starts |
| start-up options pointer `0x60981C` → "-fl -d" | | `+level <name>` = "-d <name>" + launcher Play |
| DIFFICULTY screen state 8 (`[0x634314]`, vtable `0x5E6E08`, update `FUN_0045bf90`: items 0x803-0x806 → `FUN_005cb2f0` on `[0x680128]`, then state 0xA) | menus.cpp state setter | CUSTOM CAMPAIGNS (state 0x38) → 8 → the pick starts the campaign |
| objects by id `FUN_004cf4f0` thiscall(`[0x6800E0]`, id); health object +0x58; position = render instance (+0x30) +0x108 (or `FUN_004d2520` when +0x164); player block `0x62FF7C + n*0x58C`, soldier +0x2EC; game time `[0x68009C]` +0x10 | | `game.object / position / health / player_*`, `time_ms` |

Messages seen in Mission 1 runs: 0x5A, 0x23, 0x1B, 0x3E, 0x58, 0x3B, 0x5F at the start; 0x08 / 0x09 (waypoint / path),
0x00 / 0x01 (killed / removed), 0x0C, 0x06, 0x33, 0x20, 0x48 (mission end), 0x47 (after a loaded save), 0x16 (timer).
Mission01.dll's handler answers the queries 0x0F [0], 0x1B [2], 0x24 [1], 0x25 [2], 0x26 [2], 0x27 [2], 0x28 [0],
0x29 [1], 0x3B [2], 0x3E [1], 0x5A [0] and by return value 0x2A, 0x2B; a base-less level gives its constant answers.

Verified 2026-10-23 by background runs (scratchpad runner `ds2lua/run.py`: DS2 lock, dinput8 / PSE\ /
`HKCU\Software\PSE\ds2` / saves backed up and restored, test mods removed): Mission 1's map without a base (init,
objective added → done (flags 0xB → 7), 3 s timer, end_mission(2) → success screen → Continue → the next `.lua` level),
`--@base Mission01.dll` (Mission 1's intro drive / AI and its 10 `.si` objectives, the Lua objective = index 4, Lua
timers on top), a save during the level (block "LUA1" after the DLL's) → main menu LOAD GAME → the level again with
`game.state` restored, a two-mission campaign through SINGLE PLAYER > CUSTOM CAMPAIGNS > DIFFICULTY → both missions →
main menu, `+level` on the command line.

### Mods folder sounds (`ds2fix.modsound`, `games/ds2/src/features/mods_sound.cpp`)

Modder page: [ds2-mods.md](../ds2-mods.md#sounds); every address and the research: `games/ds2/PORTING.md` "Sounds from
the Mods folder". Internal CLIENT_ONLY sub-plugin, on with `FeatMods`; reads `*.sch` / `*.psf` / `*.bik` from the mods
index at start-up and after the MODS menu's Apply (`features::ReloadModSounds`). DS2's sound library is DS1's (same
`.sch` chunks, same ADPCM, filter table `0x6196F0`).

| What | DS2 | Hook |
| --- | --- | --- |
| CSoundManager::SetSoundCache thiscall(manager `[0x6800EC]`, name) ret 4 (mission init `0x4129DA`, front end `0x42C9A7`); exits the game on a chunk it can't read | `0x4B5DE0`; its open `FUN_004b5d60` → open wrapper `0x4C49C0` at `0x4B5D6D` | call hook: a level the mods change opens a merged copy (`%TEMP%\DesertStorm2Fix\sound\<name>`, checked chunk by chunk first) from disk |
| stream ReadFromSoundCache (CWaveDataSourcePsfStream, vtable `0x5EB560`): +0x18 path, +0x14 bytes per channel; opened by fopen | `0x4B7790` (`83 EC 0C 8D 44 24 00`) | entry detour: a mod `.psf` by file name |
| interactive music ReadFromSoundCache (CWaveDataSourceIMus, vtable `0x5EB4B4`): +0x14 path, +0x18 segment table | `0x4B7370` (`83 EC 08 8D 44 24 00`) | entry detour: a mod `.psf` of exactly the game file's size |
| BinkOpen(name, flags) | `call [0x5DA2A0]` at `0x526401` | indirect-call hook: a mod `.bik` by file name |

C++ entry points for other DS2 features (`games/ds2/src/features/features.h`, not a PSE interface): `MakeWeaponSoundBank`
(a bank from a game bank with WAV-encoded shot / tail samples, added to every level - for `.weapon` files),
`BorrowSoundBank` (a game bank of another level added to every level), `ClearWeaponSoundBanks`; for `.uses` `bank X`
lines (`ds2fix.modlevels`): `BorrowGameBank(name, file)` / `ClearGameBankReferences` (kept apart from the `.weapon` ones). Dev:
`Dev\SoundTestWeapon` = "donor;new bank;folder;shot.wav;tail.wav" calls `MakeWeaponSoundBank` at start-up.

### DS2 plugins

DesertStorm2Fix's own external plugins live in `games/ds2/plugins/<name>/`, each with a `CMakeLists.txt` that builds its
DLL with `pse_add_plugin(<target> SOURCES … MANIFEST <plugin name>.pse)` (`pse/cmake/PSEPlugin.cmake`: SDK header only,
static CRT, manifest next to the DLL). `games/ds2/CMakeLists.txt` picks every such folder up by a glob at configure time
(re-run CMake after adding one), gives each plugin target `ds2_ship_target` (so a plugin adds no version resource of
its own) and puts `<plugin name>.dll` + `.pse` into `PSE\Plugins` of the tree, the package and `DS2_GAME_DIR`.
Variables for the plugin's list file: `PSE_REPO_ABS` (repo root), `PSE_SRC` (`pse/`), `DS2_SOURCE_DIR` (`games/ds2`),
`DS_DIST`. A plugin folder still being written can be left out of a build with `-DDS2_SKIP_PLUGINS=<name>[;<name>...]`.

How a DS2 plugin works with ds2fix - all through the loader, no link to `ds2fix.dll`:

- **Load order**: manifest `requires = ds2fix >= 0.1` and `games = {"ds2", NULL}` in `PSEPlugin_Info`. ds2fix is a
  host plugin, so it and its sub-plugins are loaded (round 1) before any external plugin; a plugin may also `require` a
  sub-plugin (`ds2fix.graphics >= 0.1`).
- **Interfaces**: ds2fix publishes tables through `pse.messaging` `RegisterInterface` while it loads; a plugin finds
  them with `QueryInterface` in its own `PSEPlugin_Load`. Today: `ds2fix.graphics` v1
  (`games/ds2/src/pse/ds2fix_graphics.h`: the front end's virtual 1280 x 1024 screen - `SetVirtualScreen`,
  `VirtualScreenOn`, `RealToVirtual`, `RealWidth` / `RealHeight`, `VirtualScale` - and the game's
  `IDirect3DDevice9*`); `ds2fix.memory` v1 (`games/ds2/src/pse/ds2fix_memory.h`, served by the plugin
  [`ds2fix.memory`](#ds2fixmemory-gamesds2pluginsmemory-2026-10-22)); `ds2fix.options` v1
  (`games/ds2/src/pse/ds2fix_options.h`: plugins' ON / OFF rows in the OPTIONS screen, used by
  [`ds2fix.loadout`](#ds2fixloadout-gamesds2pluginsloadout-2026-10-25)). The other direction works the same way: a plugin registers its own table, ds2fix queries it at
  `PSE_MSG_POST_LOAD`. Tables are append-only per version (a new function = a bigger `size`, callers check it).
- **Game events**: the layer's messages and events (`PSE_MSG_DEVICE_CREATED` / `_RESET` with the D3D9 device; frame,
  render, level load / unload, saves), not ds2fix.
- **Settings**: the plugin's own `pse.settings` values (`HKCU\Software\PSE\ds2\Plugins\<name>`). A value moving out of
  ds2fix into a plugin keeps its name and is copied once from ds2fix's key (`pse::MigrateRegistryValues` with a
  marker - as DS1's `dsfix.discord` took `DiscordPresence` from `dsfix`, [ds1-plugins.md](../ds1-plugins.md)).
- **Hooks**: `pse.hooks` (the broker). A site ds2fix or the layer already hooks is chained with a priority, never
  written over; the plugin owns its records, which go when it is unloaded or crashes.

#### `ds2fix.widescreen` (`games/ds2/plugins/widescreen`, 2026-10-22)

DesertStorm2Fix's widescreen / HUD part, the first DS2 plugin (`CLIENT_ONLY`, `requires = pse.game.ds2 >= 1.0, ds2fix
>= 0.1`): every display mode in the launcher, HUD scaling (`k = max(1, min(W/800, H/600)) x HUD SIZE`) with the layout
clamps and the HUD fixes ported from DesertStormFix, the front end and the in-mission menus (pause, OPTIONS ...) on the
virtual 1280 x 1024 screen, sharper text. Built from the same sources as the standalone `dinput8.dll`
(`games/ds2/src/features/widescreen.cpp`, `hud.cpp`, `hudfix.cpp`, `frontend.cpp`, `fontsharp.cpp`); `shim/` puts
`core/patch.h` on `pse.hooks` (byte groups, call-site hooks, entry detours - all owned by the plugin) and `core/log.h`
on `pse.log`. Removing the plugin = the game's own HUD / menus at that resolution, ds2fix otherwise unchanged.

| Setting (`HKCU\Software\PSE\ds2\Plugins\ds2fix.widescreen`) | Type | Default | |
| --- | --- | --- | --- |
| `HudScaling` | bool, live | 1 | HUD and menus follow the resolution (0 = the game's 800 x 600-sized art); the front end's virtual screen follows at the next start |
| `HudScalePercent` | int 25-400, step 25, live | 100 | HUD size on top of the resolution scaling |

Both were ds2fix values: copied once from `HKCU\Software\PSE\ds2\Plugins\ds2fix` (marker `ImportedFromDs2fix`). The
player changes them in the game: OPTIONS > HUD SCALING / HUD SIZE (rows drawn by ds2fix's menus through the interface
below; applied at once - inside the pause menu when it closes).

Interface **`ds2fix.hudhost` v1** (`games/ds2/src/pse/ds2fix_hud.h`), published by ds2fix (owner `ds2fix.menus`) for
this plugin: `SplitViewSize(int* w, int* h)` (split screen: the smallest view - the HUD art is sized per view) and
`SetHudOption(const Ds2FixHudOptionV1*)` (shows the OPTIONS rows for `Scaling()` / `Percent()` / `Set(on, percent)`;
NULL hides them - done at the plugin's unload). Without it the plugin still runs (no split-screen sizes, no rows).

#### `ds2fix.memory` (`games/ds2/plugins/memory`, 2026-10-22)

DesertStorm2Fix's memory part (`CLIENT_ONLY`, `requires = pse.game.ds2 >= 1.0`, `after = ds2fix`; port of
DesertStormFix's `memory.cpp`): the game's file heap sized for the mods' big files, texture files too big for it read
outside it, the memory figures in the log, the main menu's 4 GB notice. No settings. Works without `ds2fix` (it then
only sizes the heap and logs).

| What | CDS II.exe 1.0 | DS1 |
| --- | --- | --- |
| Global Heap ("CLinearBuffer", MemBuffers.cpp): `push "Global Heap"; push 1024000; mov ecx, eax; call` at `0x4136F0..0x4136FC` → `[0x664380]`, made once in the game's start-up (after every plugin has loaded) | ctor `0x4C0650` thiscall(size, name) ret 8 (0x68-byte object from `0x4BE2D0`) | `0x40FFCB..0x40FFD7`, `[0x63C8B4]` |
| object | +0 name, +0x20 mark slots, **+0x48 size**, **+0x4C free**, +0x50 used, +0x58 buffer (size + 4: "LBUF" guard), +0x5C 1 = buffer not owned (the dtor `0x4C0710` frees it only when 0), +0x60 buffer >> 31, +0x64 marks taken | +0x28 / +0x2C / +0x30 / +0x34 / +0x38 |
| buffer | game allocator `0x4BE320`(size, pool) cdecl - **retries for ever** when memory runs out (a hang) - free `0x4BE3B0`(p). No ctor that takes a caller's buffer (DS1 `0x4C0340`) | `0x4B8450` / `0x4B8590` |
| alloc / alloc16 / pop / mark / release | `0x4C0750` thiscall(size, align) ret 8, `0x4C07A0`(size) ret 4 (16-aligned) - 0 when it doesn't fit (the "Out of heap space" print `0x4CD600` is a stub: nothing shows); `0x4C07E0`(bytes) ret 4; `0x4C0810`() → slot; `0x4C0830`(slot) ret 4 | `0x4C0420` / `0x4C0460` / `0x4C04A0` / `0x4C04D0` / `0x4C04F0` |
| whole-file reader | `0x4C49E0` cdecl(name, &buf, &size, heap, pool): resolver `0x4C3C40`(name, `[0x67D3AC]`, 0) + open; buffer = heap alloc(size + 1, 4), or with heap 0 the game allocator(size + 1, pool) | `0x4BFB90` |
| texture loaders (each: mark, read into the heap, create the texture, release) | `0x5421F0` (read call `0x54222A`), `0x5422A0` (`0x5422DA`), `0x5427B0` (`0x542811`: read + release at once), `0x542E20` (`0x542E5A`). The failure path of the first two releases slot **0** (`push eax` with eax = 0 at `0x542262` / `0x542312`, a game bug: the heap goes back to its first mark) | `0x548F20`, `0x549497`, `0x549537`, `0x549857` |

What the plugin hooks (broker records owned by `ds2fix.memory`, priority 100; the COM slots 50):

- **heap size** - call site `0x4136FC`, in front of `ds2fix.mods`' hook (priority 0): size = what arrives (1 024 000,
  or more if a plugin in front added) + `AddHeapReserve` bytes + the largest file in any `Mods` folder, on or off
  (rounded up to 1 MB; only when > 256 KB and `ds2fix`'s `FeatMods` is on; types the heap never holds - `.dll .wav .bik
  .txt .sch .sbk .lua` ... - left out), capped at **64 MB** (2 GB address space) / **256 MB** (4 GB) and a quarter of
  the largest free block. The game's ctor runs with 16 bytes, then the plugin frees that buffer and puts its own in
  (`VirtualAlloc`, +0x5C = 1); if that fails, the game allocator with the original size. Dev (`HKCU\Software\
  DesertStorm2Fix\Dev`): `HeapMB` forces a size in MB, `HeapAllocFail` = 1 fakes the reservation failing;
- **texture files outside the heap** - reader entry `0x4C49E0`: a read from one of the four texture loaders whose file
  (the resolver's answer - `ds2fix.mods`' hook names the mod's file) is bigger than the heap's free space goes through
  the game allocator instead (heap 0) - not when the largest free block is under twice its size (the allocator would
  hang). The buffer is freed when the heap's release `0x4C0830` / pop `0x4C07E0` goes back past the loader's mark (mark
  count below the one at the read, or use below it): the loader's own release, the slot-0 failure path too;
- **peak / failures** - `0x4C0750` / `0x4C07A0`: peak use, allocations that didn't fit (`[fail]` line naming the file
  being read);
- **Direct3D textures** - at `PSE_MSG_DEVICE_CREATED`, `IDirect3DDevice9::CreateTexture` (vtable slot 23) per device
  class and `IDirect3DTexture9::Release` (slot 2) per texture class, broker pointer hooks chained with `ds2fix.graphics`'
  device hooks: count + size estimate per pool, `GetAvailableTextureMem`;
- **log** - at start-up the address space (2 GB / 4 GB, largest free block), the heap it made, one line per level load
  (`Memory (mission01.dll): address space 492 / 2047 MB used (2 GB), largest free 535 MB, private 139 MB; global heap
  190 / 7144 KB, peak 5461 KB, 0 failed, 0 read outside; textures 363: 0.0 MB video + 18.7 MB managed + ...`), every
  30 s in dev builds (`Dev\MemLogSec` = seconds);
- **main menu notice** - main menu vtable `0x5E6F58` slot 5 (`+0x14` draw `0x462000`; `ds2fix.menus`' MainDraw behind
  it): `2 GB MEMORY MODE - RUN DESERTSTORM2FIX-PATCHER.EXE FOR 4 GB` (small font `[0x62F43C]`, amber, x 374 / y 305 of
  the front end's 1280 x 1024 screen, above the menu panel - the 3D monitor shows only y ~287..752 of it; `ds2fix.modsmenu` uses the rows under the panel) while the exe has no Large Address Aware flag
  on 64-bit Windows.

Interface **`ds2fix.memory` v1** (`games/ds2/src/pse/ds2fix_memory.h`, published when the heap hook went in):
`AddHeapReserve(plugin, bytes)` (before the heap is made: 1, else 0), `HeapSize()`, `HeapCap()`, `LargeAddressAware()`,
`LoadsBigTexturesOutsideHeap()`, `GetStats(Ds2FixMemoryStatsV1*)` (address space, private bytes, heap size / used /
peak / failures / reads outside, texture count + bytes per pool, D3D free MB).

**Heap room for other plugins (contract).** A plugin that needs more Global Heap (e.g. one raising engine pools carved
out of it) queries `ds2fix.memory` v1 at `PSE_MSG_POST_LOAD` (every plugin is loaded by then; the heap is made later,
in the game's start-up) and calls `AddHeapReserve(self, bytes)`. Without the plugin it subscribes the call site
`0x4136FC` itself (`HookCallSite`, expected target `0x4C0650`, a priority above 0) and adds its bytes to `size` before
calling `next`: `ds2fix.mods` (priority 0) adds the largest mod file on top of whatever arrives. With the plugin,
nothing behind it sizes the heap (it makes the heap itself; what a subscriber behind it adds to the 16 bytes it passes
is kept, but use the interface).

`ds2fix.mods` with the plugin: its hook passes the call through (`Mods: file heap sized by ds2fix.memory` in the
report), the MODS menu's "restart for a bigger heap" check uses the live heap size and `HeapCap()`, and a texture
bigger than the heap needs no restart (`LoadsBigTexturesOutsideHeap`).

Other memory limits found (2026-10-22, for `ds2fix.limits`): sub-heaps carved out of the Global Heap for good - decal
index / vertex / part heaps 0x2000 + 0x12000 + 0x1000 (`0x550B40` → `0x4C0980` thiscall(parent, size, name)); a 0x800
unnamed arena `[0x680054]` (`0x4D11AE`); per-level `CFastAllocator` pools (`CEnvObject`, `CEnvInstance` 0x1B0 B each,
`CAnimInstance`, `CVBufferObject`, `CVertexBuffer`, `_MATRIX`; set-up `0x4145F9..`, counts `[0x61B308]`... - the
`.FAS` pool sizes, as on DS1) from the game allocator, not the heap; MathEngine Karma pools ("MdtSolverPoolAlloc",
McdSpace pairs, the contact buffer: fixed sizes, errors on overflow); "Too many sounds!" (`0x4B60DC`), "Number of paths
... exceeds %d" (`0x4C492D`), active lights per instance (`0x507C55`).

**4 GB patcher.** `DesertStorm2Fix-Patcher.exe` in the package (DS1's `src/patcher/patcher.cpp` built with
`PATCHER_GAME_DS2`, target `ds2_patcher`): sets / clears the Large Address Aware bit of `CDS II.exe` (file offset
`e_lfanew + 22`) after checking it is CDS II.exe 1.0 (the disc-free hash, or another file with PE time stamp
0x3F4B1A8A); the retail SecuROM exe (its hash, or a same-build file starting at entry point RVA 0x42CC17) is never
changed, even with `/force` (exit code 6). Finds the install from its own folder, `HKLM\SOFTWARE\SCi Games\CDS II`
`ExePath` (32-bit view + the VirtualStore copy), uninstall entries named "Conflict Desert Storm II", GOG keys and the
usual folders. Backup `CDS II.exe.original`, file times kept, CLI `/status | /enable | /disable [/force] [folder...]`
(exit codes as DS1's + 6). The PSE loader hashes the exe with that bit cleared, so a patched exe is still the known
one.

#### `ds2fix.limits` (`games/ds2/plugins/limits`, 2026-10-22)

DesertStorm2Fix's engine limits for big battles and big mods (`SIMULATION`, `requires = pse.game.ds2 >= 1.0, ds2fix >=
0.1`; port of DesertStormFix's `limits.cpp`). No settings (remove the DLL for the game's own limits). Full address
table: `games/ds2/PORTING.md` "Engine limits".

- **Per-level physics pools**: DS2 sizes the dynamics manager from the level's `.FAS` (`DynamicObjects` / `MeBodies` /
  `MeModels`; the shipped levels hold just what they use, e.g. Mission 1: 20 / 90 / 111). The ctor call `0x50992C`
  (`0x4F8600` thiscall(objects, bodies, models)) is wrapped: at least 4096 physics objects, 2048 Karma bodies, 4096
  collision models; the physics list's add `0x4F6D80` is replaced (reuses freed slots, refuses when full). A mod that
  adds vehicles / dynamic props no longer needs its own `.FAS`.
- **Byte patches** (one group): Karma constraints 1000 → 8192, collision pairs / objects 800 / 800 → 16384 / 4096,
  particles 250 → 2000, events 200 → 2048, render command arena 64 KB → 1 MB, decal ring 64 KB → 512 KB, A* searches
  3 → 12 per frame, software sound voices 6 → 32, script-id table up to id 65534, save buffer + 4 MB; projectiles
  50 → 1024 (ctor call `0x413C6E`).
- **AI think budget**: the scheduler's fixed 1 soldier + 1 vehicle per frame becomes ⌈soldiers / 40⌉ (≤ 32) +
  ⌈vehicles / 20⌉ (≤ 16), set before each scheduler call (`0x4107BD`) - unchanged up to 40 / 20.
- **Saves stay the game's**: the particle / projectile writers save exactly the 250 / 50 slots the loaders read (live
  ones first), events are capped at 255 (the header counts them in a byte), the file is written up to its last non-zero
  byte but never shorter than the vanilla buffer - saves load in the game without the plugin and the other way round.
- **Global Heap**: + 2.5 MB through `ds2fix.memory`'s `AddHeapReserve` (the contract above), or its own subscription
  to `0x4136FC` (priority 200) when that plugin isn't installed.
- Dev (`HKCU\Software\DesertStorm2Fix\Dev`): `NoLimits` = 1 leaves every limit as shipped; `LimitsTestSave` /
  `LimitsTestLoad` (save path without `.sav`) + `LimitsTestAfterMs` script a save / load in a mission; the dev log
  lists the pool values every 10 s.

#### `ds2fix.loadout` (`games/ds2/plugins/loadout`, 2026-10-25)

DesertStorm2Fix's pre-mission LOADOUT screen (`SIMULATION`, `requires = pse.game.ds2 >= 1.0, ds2fix >= 0.1`; first
port of DesertStormFix's customise / loadout screen). Full address table and test notes: `games/ds2/PORTING.md`
"Loadout screen".

- **When**: once per mission level (flow name `mission…`), when the intro is over: front-end state 0x13, dispatch 1,
  no cutscene, letterbox off (`[[0x6800CC] + 8]` = 0), the player has control (input block 0 `+8`) and his vehicle (if
  he sits in one, soldier `+0x690`) moved < 5 cm in the last second - for 1 s. Not for a level loaded from a save (`PSE_MSG_LOAD_END`).
- **What**: per squad soldier (player team, unit `+0x1CF` squad role ≥ 0) MAIN WEAPON / SIDEARM / LAUNCHER, chosen from
  the hand-held weapon definitions some soldier of the level carries (so the model, animations and shot sound bank are
  loaded); sidearm / launcher can be left behind. The world is paused meanwhile (`[0x6197BC]` = 0.0001, input blocks
  off). START MISSION gives the new weapon (CreateInventoryHLO `0x4830E0`), holds it when the old one was held, then
  takes the old one (`0x47F3A0`); rounds = the old weapon's share of its allowance.
- **Saves**: at `PSE_MSG_SAVE_BEGIN` the save header's per-type object counts are taken again (the game takes them
  once at level init - a save made after objects were added or taken crashed on load, as in DS1); taken weapons and
  the weapons the screen makes are flagged not-saved (a run-time weapon has no script id, which the game's restore
  can't place); co-save chunk `KITS` v1 (64-byte records) puts swapped-in weapons back after a load; projectiles in
  flight from such a weapon name a kit weapon of the same soldier while the game saves (a loaded bullet with a null
  weapon crashed).
- **Drawing**: its own (GDI text -> one texture -> a quad at `PSE_EVENT_END_SCENE`, back buffer only); input:
  keyboard while the game window is in front, XInput pads.

| Setting (`HKCU\Software\PSE\ds2\Plugins\ds2fix.loadout`) | Type | Default | |
| --- | --- | --- | --- |
| `FeatLoadout` | bool, live | 1 | the screen after each mission's intro; OPTIONS row LOADOUT SCREEN (through `ds2fix.options`) |

Interface **`ds2fix.options` v1** (`games/ds2/src/pse/ds2fix_options.h`), published by ds2fix (owner `ds2fix.menus`):
`AddToggle(plugin, const Ds2FixToggleV1*)` / `RemoveToggle(plugin, toggle)` - up to 4 plugin ON / OFF rows
(`"<label>: ON / OFF"`, `Get()` / `Set(on)`) in the game's OPTIONS screen (state 6, the pause menu's OPTIONS) after the HUD
rows, while its 8 rows have room. The toggle table must stay valid until `RemoveToggle` (call it in `PSEPlugin_Unload`).

## Loader neutrality (found while porting)

The loader (`pse/loader`, `pse/loader_dll`, `pse/shim`) needed **no code change** for DS2: detection, plugin loading,
host rights, the broker, messaging, settings and the co-save all ran as they are. DS1 assumptions found on the way
(none of them stopped DS2) - all settled 2026-10-22:

1. ~~`pse::Config` defaulted `gameId = "ds1"` and `pse::Init` fell back to it~~ - no default now: `Init` refuses a
   missing / bad id (returns false), `registryRoot` empty = `Software\PSE\<gameId>`.
2. ~~No log file without a host plugin~~ - wrong: `pse_loader.dll` has always written every line no host sink took to
   `PSE\Logs\loader.log` (its fallback, every build). The DS2 layer's own copy of that file could never open it (the
   loader's handle was first) and is gone. Per-plugin `<plugin>.log` files (`logDir`) stay off.
3. ~~`PSE_MSG_EXIT` only from DS1's host plugin~~ - the DS1 layer sends it too, from the level unload in `WM_DESTROY`
   (`0x411404`), like the DS2 layer (Shutdown is once-only).
4. The SDK examples `example.hello` / `example.events_logger` (and `lua_native`, `ui_demo`, the layer test harness)
   declared `games = {"ds1"}` - the first two now say `{"ds1", "ds2"}` (pse_tests' expectations updated); `lua_native`
   and `ui_demo` need DS1-only parts (Lua host / menus of DesertStormFix) and stay DS1.
5. ~~`ds1_saves.cpp` handed the game's ANSI save path to `pse::cosave` (UTF-8)~~ - converted now; the save / load
   messages carry UTF-8 on both games.
6. Found with 1-5: DS1's moved game files (`PSE\Originals`) were found only through DesertStormFix's hooks - without it
   the game couldn't open its archives. The redirect is in the DS1 layer now ([game-ds1.md](game-ds1.md)).

## Open

- The SecuROM exe as a second build of the database (other IAT, call-through thunks) – only if someone needs it.
- Render events in the front end (it draws outside `Render_Frame`).
- `LauncherOpen` / `LauncherClosed` are not sent (as on DS1).
- A level started from a loaded save through the real LOAD GAME menu is not tested (the harness calls the saver
  directly and doesn't reload the level; the "no NEW_GAME after a load" rule is covered by pse_tests only).
- Missions started from the real menus (campaign flow, briefing films) were not run - only `-d` boots and the front end.

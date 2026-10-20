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
| Detection (`PSEGame_Detect`) | `CDS II.exe` 1.0 **disc-free**: SHA-256 `E9B46420DC00B13C3C48A9E9B7B3FDA29458D5EA83AC0D203F5B68FDD111542A` (Large Address Aware bit cleared), image base 0x400000. The retail SecuROM exe (`CDS II - Original.exe`, `44EF1E16…95EF`, same PE time stamp + size) is refused: its code calls imports through SecuROM's own thunks (IAT at 0x91C000), so the layer's import / call-site hooks don't fit, and it won't start without the disc anyway |
| `pse.ids` database | 115 ids (`nodisc-1.0`), all published, every signature checked at start-up (`115 resolved, 0 mismatched`) |
| `pse.game.ds2` v1 | front-end state, dispatch mode, level name / kind, cutscene bit, split-screen views, window, exe hash |
| Core events / messages | the same set as DS1 (table below), plus `PSE_MSG_EXIT` from the layer itself |
| Co-save | `<save>.pse` next to the game's `.sav`, NEW_GAME, revert – same rules as DS1 |
| Log | `PSE\Logs\loader.log` (every loader / plugin line) while no host plugin takes the lines |
| Host plugin | the layer allows `ds2fix` (DesertStorm2Fix as a host plugin: skeleton built, [below](#desertstorm2fix-as-a-host-plugin)) |

Verified 2026-10-19 – pse_tests (9 DS2 tests, below) and live runs on the user's install (`H:\SCI Games\Conflict Desert
Storm II`, DesertStorm2Fix's `dinput8.dll` backed up and put back after each run):

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

DS2 has no host plugin yet to take the log lines, so the layer writes them itself: `PSE\Logs\loader.log` (rewritten
every start; the start-up lines from before the layer are in it too; report lines prefixed `[report]`). A host plugin
that sets its own sink takes over (ds2fix: `DesertStorm2Fix.log`).

## Tests

- `pse/tests/test_game_ds2.cpp` (pse_tests): the built `pse_ds2.dll` detects exactly the disc-free exe (not the SecuROM
  one, not DS1; `pse_ds1.dll` refuses DS2); the ds2 database (table rules, one build, published names); co-save path
  rules + NEW_GAME + save / load; `pse.game.ds2` registration; on the **mapped CDS II.exe** (`exe::MapGame("ds2")`,
  `PSE_REQUIRE_EXE_FOR("ds2")` – `pse/tests/exe_image.*` maps one game's exe at a time at 0x400000): every signature,
  every id resolving, IAT names, call sites calling their functions, the layer's hooks (sites, kinds, priorities,
  prologues) and a priority-0 hook chaining in front of the frame hook, the interface's reads; and `pse::Start` with
  **both** `pse_ds1.dll` and `pse_ds2.dll` in `Games`: the loader picks `pse_ds2.dll`, frees `pse_ds1.dll`, loads
  `example.hello` under `ds2`, the layer's `loader.log` has every line.
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
DS1 tree – and copies it into `DS2_GAME_DIR` when that is set (it replaces the folder's `dinput8.dll`: DesertStorm2Fix's
own – back it up). Never deployed into the DS1 install.

## DesertStorm2Fix as a host plugin

Skeleton built 2026-10-19 in the DS2 patch (`games/ds2` since the repo merge, formerly `../ConflictDesertStorm2Patch`;
CMake option `DS2_PSE_PLUGIN`, target `ds2fix`, sources `games/ds2/src/pse/`; root preset `ds2-pse` →
`build-ds2fix/RelWithDebInfo/ds2fix.dll`): `ds2fix.dll` + `ds2fix.pse` (`host = true`, `requires = pse.game.ds2 >= 1.0`):

- `PSEPlugin_Load` = what DesertStorm2Fix's `dllmain.cpp` `Init` did: log, settings, then every feature's `Apply` inside
  its sub-plugin's owner scope (`ds2fix.widescreen`, `.hud`, `.frontend`, `.fontsharp`, `.cinematics`, `.menus`,
  `.splitscreen`, `.coop`, `.graphics`, `.devbg`);
- patching = DesertStormFix's broker-backed `src/core/patch.cpp` from this repo (a superset of DS2's `patch.h`, swapped
  in through `src/pse/shim/core/patch.h`), pse:: calls through `pse/hostlink` sources compiled in;
- two feature changes, both `#ifdef DS2_PSE_HOST` (the `dinput8.dll` build is unchanged): `frontend.cpp` subscribes to
  the layer's level call sites (`patch::SubscribeCall` at priority 0, calling `next`) instead of hooking them – a plain
  `HookCall` there fails ("does not call 0x412690") once the layer owns the site; `graphics.cpp` hooks the
  `Direct3DCreate9` import through the broker (`patch::HookSlot`), chained behind the layer's.

Still to do: settings through `pse.settings` (still `HKLM\SOFTWARE\SCi Games\CDS II\Enhancements`, no migration);
graphics.cpp's device vtable hooks through the broker (they chain by hand, outermost, after the layer's – works, but
the broker doesn't list them); a launcher / menu integration of `pse.ui`; `pse.game.ds2` providers for the co-op session;
DS2 features that hook layer sites must use `SubscribeCall` (only `frontend.cpp` does today); a ds2fix release package
(shim + loader + layer + ds2fix instead of the single `dinput8.dll`); the live test of every feature (only Mission 1 +
front-end layout seen so far).

## Loader neutrality (found while porting)

The loader (`pse/loader`, `pse/loader_dll`, `pse/shim`) needed **no code change** for DS2: detection, plugin loading,
host rights, the broker, messaging, settings and the co-save all ran as they are. DS1 assumptions found on the way
(none of them stopped DS2; fixes are proposals):

1. `pse::Config` defaults `gameId = "ds1"`, `registryRoot = "Software\PSE\ds1"`, and `pse::Init` falls back to
   `"ds1"` when no game id is given (`loader.h`, `registry.cpp`). `pse::Start` always passes the layer's id, so only
   tests / embedders that call `Init` directly get DS1 - better: no default (refuse an empty id).
2. No log file without a host plugin: `pse::Start` sets no `logDir` and no file sink, so on a game without a host plugin
   every line (start-up, plugin load results, crashes) is lost in release builds and only in `OutputDebugString` in dev
   builds. The DS2 layer works around it (`PSE\Logs\loader.log`); the loader itself should do this (`Logs\loader.log`, and
   `<plugin>.log` through `logDir`).
3. `PSE_MSG_EXIT` (+ plugin unload) only comes from DS1's host plugin (DesertStormFix's window procedure). The DS2 layer
   sends it from WM_DESTROY itself; the DS1 layer could do the same (Shutdown is once-only, so dsfix calling it too is
   harmless).
4. The SDK examples `example.hello` / `example.events_logger` (and `lua_native`, `ui_demo`, the layer test harness)
   declared `games = {"ds1"}` - the first two now say `{"ds1", "ds2"}` (pse_tests' expectations updated); `lua_native`
   and `ui_demo` need DS1-only parts (Lua host / menus of DesertStormFix) and stay DS1.
5. Not the loader but the DS1 layer: `ds1_saves.cpp` hands the game's ANSI save path to `pse::cosave`, which reads paths
   as UTF-8 - a save folder with non-ASCII characters would miss its co-save. The DS2 layer converts (UTF-16 → UTF-8).

## Open

- The SecuROM exe as a second build of the database (other IAT, call-through thunks) – only if someone needs it.
- Render events in the front end (it draws outside `Render_Frame`).
- `LauncherOpen` / `LauncherClosed` are not sent (as on DS1).
- A level started from a loaded save through the real LOAD GAME menu is not tested (the harness calls the saver
  directly and doesn't reload the level; the "no NEW_GAME after a load" rule is covered by pse_tests only).
- Missions started from the real menus (campaign flow, briefing films) were not run - only `-d` boots and the front end.

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
      <name>.dll + <name>.pse       third-party plugins
  Mods\                             content mods (unchanged)
```

`PSE\Cache\assets\` (only when `Dev\PseStore` is set, dev builds) is the asset store DesertStormFix builds from the
install's own archives at start-up: game-derived, never shipped,
safe to delete (rebuilt in ~5 s). A read-only install uses `%LOCALAPPDATA%\PSE\ds1-<hash>\assets\` instead.
`PSE\Cache\missions\` (only with `Dev\MissionSource = lua`, dev builds) holds the mission DLLs translated to Lua
(`<Mission>.lua` + `.stamp`, written on a level's first start or by `pse_build missions`;
the mission translator): game code made from the player's DLLs, never shipped, safe to
delete (~45 ms per mission to remake); read-only installs use `%LOCALAPPDATA%\PSE\ds1-<hash>\missions\`.

Dev builds have a `.pdb` next to every DLL. The build assembles the same tree in `build*/tree/<config>/` (and copies it
into `DS_GAME_DIR` when that is set); the release zip has it without PDBs, plus `DesertStormFix-README.txt`.

| Binary | CMake target | Source | Links |
| --- | --- | --- | --- |
| `dinput8.dll` | `pse_shim` | `pse/shim/` (`shim.cpp` game-neutral, `proxy_dinput8.cpp` + `.def` the dinput8 exports) | nothing of PSE |
| `PSE\pse_loader.dll` | `pse_loader` | `pse/loader_dll/loader_main.cpp` | `pse_core` (= `pse/loader/*.cpp`) |
| `PSE\Games\pse_ds1.dll` | `pse_ds1` | `pse/games/ds1/` + `pse/loader/ids.cpp` | `pse_hostlink` |
| `PSE\Games\pse_ds2.dll` (Desert Storm II installs) | `pse_ds2` | `pse/games/ds2/` + `pse/loader/ids.cpp` | `pse_hostlink` |
| `PSE\Plugins\dsfix.dll` | `dsfix` | `src/` (all of DesertStormFix) | `pse_hostlink`, Lua |
| `PSE\Plugins\dsfix.discord.dll` | `dsfix_discord` | `plugins/discord/` | the SDK header only |

## More than one game (2026-10-19)

The shim, `pse_loader.dll` and every plugin built only against the SDK are **game-neutral**: the same files go into
every supported game's folder. Per game there is one game layer `PSE\Games\pse_<game>.dll` (detection, ID database,
`pse.game.<game>`, the hooks that drive the core events, the co-save, the host plugins it allows):

| Game | Layer | Exe it detects | Host plugin it allows | Reference |
| --- | --- | --- | --- | --- |
| `ds1` Conflict: Desert Storm | `pse_ds1.dll` | `DesertStorm.exe` (Steam = GOG) | `dsfix` (DesertStormFix) | [game-ds1.md](reference/game-ds1.md) |
| `ds2` Conflict: Desert Storm II | `pse_ds2.dll` | `CDS II.exe` 1.0 disc-free | `ds2fix` (DesertStorm2Fix, skeleton) | [game-ds2.md](reference/game-ds2.md) |

A folder may hold several layers: the loader asks each `PSE\Games\pse_*.dll` in name order and the first whose
`PSEGame_Detect` answers 1 wins; the others are freed again (pse_tests: both layers installed, CDS II.exe mapped →
`pse_ds2.dll` picked). A plugin says which games it runs on in `PSEPluginInfo.games` (`{"ds1", "ds2", NULL}`; NULL =
any) - refused elsewhere with "made for …; this game is …". The settings root follows the layer's game id
(`HKCU\Software\PSE\ds2\Plugins\<plugin>`). The DS2 install tree is assembled by `pse_tree_ds2` into
`build*/tree-ds2/<config>/` (deployed only into `DS2_GAME_DIR`, never into the DS1 install).

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

Shutdown: DesertStormFix's window procedure (`WM_DESTROY`) calls `pse.host` `Shutdown`: `PSE_MSG_EXIT`, then external
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
| No `dsfix.dll` | PSE runs (game layer, other plugins): plugins still get frames, render / D3D events, level events, NEW_GAME and the co-save (no `PSE_MSG_EXIT`, no frame cap); no DesertStormFix features, no `DesertStormFix.log` |
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
    games\<id>\           one game layer per game: ds1/ (pse_ds1.dll), ds2/ (pse_ds2.dll); cv / cgs: none yet
    ids\<id>.ids.txt      ID databases + <id>.published.txt (frozen lists); compiled by scripts/gen_ids.py
  src\                    DesertStormFix = DS1's patch (host plugin dsfix + its sub-plugins); moves to games\ds1 later
  plugins\                DesertStormFix's external plugins (dsfix.discord ...)
  games\<id>\             the other games' patches, each its own CMake project:
    ds2\                  DesertStorm2Fix (dinput8.dll; ds2fix.dll = the PSE host plugin, -DDS2_PSE_PLUGIN=ON)
    cv\                   VietnamFix (dinput8.dll)
    cgs\                  GlobalStormFix (dinput8.dll)
    ds1-reference\        DesertStormFix feature sources the ports started from (not built; one shared copy)
  scripts\                DS1 generators (patch tables, icons, gen_ids.py for every game's ids)
  assets\, third_party\   shared by every game (Kenney input prompts; Lua 5.4.7, Zydis)
  docs\sdk\               this documentation (local)
  tests\live\             live tests against the running game (DS1)
  tools\                  local, gitignored: DS1 helpers at the top, tools\ds2\, tools\cv\, tools\cgs\ per game
```

Each `games/<id>` keeps the layout it had as a repo: `src/` (`core/`, `features/`, `generated/`, `dllmain.cpp`,
`dinput8.def`, `version.rc.in`), `scripts/` (its icon generators, reading the shared `assets/`), `dist/` (player
readme), `CMakeLists.txt` + `CMakePresets.json` (+ a gitignored `CMakeUserPresets.json`), `PORTING.md`, `README.md`,
and a local `CLAUDE.md` (gitignored like the root one) that says where its game layer, ids and tools are.

Builds: `cmake -S games/<id> --preset x86` (binary dirs `games/<id>/build*`, as before the move), or from the root
with `PSE_GAME=<id>` - presets `ds2`, `ds2-dist`, `ds2-pse`, `cv`, `cv-dist`, `cgs`, `cgs-dist` (binary dirs
`build-ds2`, `build-ds2-dist`, `build-ds2fix`, `build-cv` ...; outputs `<bin>/<config>/dinput8.dll`, `<bin>/package`
+ zip). With `PSE_GAME` set only that game's project is configured (no DesertStormFix / PSE targets, so no name
clashes); the DS1 presets `x86` / `dist` are unchanged.

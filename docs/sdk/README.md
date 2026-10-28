# Pivotal Script Extender (PSE) – SDK docs

PSE is a script-extender-style plugin host for Pivotal Games' PC titles (Conflict: Desert Storm first). Phase 2 layout
is built: the shim `dinput8.dll` starts `PSE\pse_loader.dll`, the game layer `PSE\Games\pse_ds1.dll` recognises the
exe, and DesertStormFix is the host plugin `PSE\Plugins\dsfix.dll` ([layout.md](layout.md)). **Second game (2026-10-19):**
Conflict: Desert Storm II runs on the same shim + loader with its layer `PSE\Games\pse_ds2.dll`
([reference/game-ds2.md](reference/game-ds2.md)); SDK-only plugins run on both as the same DLL. **Third game
(2026-10-23):** The Great Escape (`PSE\Games\pse_tge.dll`, host plugin GreatEscapeFix,
[reference/game-tge.md](reference/game-tge.md)).
**API version 0 is unstable**; it is frozen as v1 with SDK 1.0.

| Page | What |
| --- | --- |
| [layout.md](layout.md) | The files in an install, the start-up sequence (shim → loader → game layer → host plugin → plugins), what happens when a part is missing, uninstall |
| [API_CHANGELOG.md](API_CHANGELOG.md) | Every interface version and what changed |
| [ds1-plugins.md](ds1-plugins.md) | DesertStormFix's plugins: the internal ones left in dsfix.dll (the host layer) and the 33 plugin DLLs - names, flags, owner scopes, shared hook sites, what happens without each |
| [ds1-host-layer.md](ds1-host-layer.md) | The host layer dsfix.dll keeps (settings, dsfix.host / dsfix.services / dsfix.overlay, menus, frame dispatcher, ...) and, per plugin, which host services it uses - the map for porting a feature to another game |
| [data-files.md](data-files.md) | PSE data files (Lua-table literals, canonical form) and the records: `ds1:` IDs, record types, the schema |
| [mod-layout.md](mod-layout.md) | Mod folders for all six games: files found by name in any sub folder, the organised layout new mods use (evos, dds, anims, sounds, tables, defs, levels), paths that must stay put, the mod priority list (load order: the mod loaded last wins) |
| [online-compat.md](online-compat.md) | Online: the plugin-set check – what is compared (simulation plugins, host fingerprint, mods), messages, flags guidance for plugin authors |
| [ds1-settings.md](ds1-settings.md) | Where DesertStormFix's settings live (pse.settings plugin `dsfix`), the one-time move from the old key, every value |

Guides:

| Guide | What |
| --- | --- |
| [guides/first-plugin.md](guides/first-plugin.md) | Build `example.hello` from the SDK only: exports, manifest, log, setting (its menu page comes free), event, interface, CMake helper; adding Lua functions (`example.lua_native`); drawing / pop-ups with `pse.ui` (`example.ui_demo`) |

Reference, one page per interface:

| Interface | Page |
| --- | --- |
| `pse.hooks` – byte ownership, chained hooks | [reference/hooks.md](reference/hooks.md) |
| `pse.log` | [reference/log.md](reference/log.md) |
| `pse.messaging` – messages, events, cross-plugin interfaces | [reference/messaging.md](reference/messaging.md) |
| `pse.tasks` | [reference/tasks.md](reference/tasks.md) |
| `pse.settings` – registry-backed settings; automatic menu pages + launcher rows | [reference/settings.md](reference/settings.md) |
| `pse.ui` – drawing over the game, texts, pop-ups, keyboard focus (served by the host: DesertStormFix in DS1); how settings show up in the menus | [reference/ui.md](reference/ui.md) |
| `pse.serialization` – co-save | [reference/serialization.md](reference/serialization.md) |
| `pse.lua` – Lua functions for mission scripts (namespaces, accessor marshalling, CallGlobal, crash guard) | [reference/lua.md](reference/lua.md) |
| `pse.vfs` – game files from providers (name / name hash + context → bytes; the asset store is one) | [reference/vfs.md](reference/vfs.md) |
| Plugin DLLs – manifest (`.pse`), load order, exports, lifecycle, failure handling | [reference/plugins.md](reference/plugins.md) |
| Mission patches – a mod's `Mission1.patch.lua` changes the readable Lua made from the player's own DLL (shareable: no game code) | [reference/mission-patches.md](reference/mission-patches.md) |
| Mission replacements – a mod's Lua (`--@replaces Mission1.dll`) runs instead of a mission DLL | [reference/mission-replacement.md](reference/mission-replacement.md) |
| Desert Storm II Mods folder – what a DS2 mod can replace today (basic resolver) | [ds2-mods.md](ds2-mods.md) |

Game interfaces (one per game, served by the host):

| Interface | Page |
| --- | --- |
| `pse.game.ds1` – Conflict: Desert Storm: front-end state, level + title, catalog text, local players, online session, window, exe hash (read-only, v1) | [reference/game-ds1.md](reference/game-ds1.md) |
| `pse.game.ds2` – Conflict: Desert Storm II: front-end state, dispatch mode, level name / kind, cutscene, split-screen views, window, exe hash (read-only, v1); the DS2 game layer, its events, saves, tests and the `ds2fix` host plugin skeleton | [reference/game-ds2.md](reference/game-ds2.md) |
| `pse.game.tge` – The Great Escape: dispatch mode, level-table entry, level name / kind, window, exe hash (read-only, v1); the TGE game layer, its events and the `tgefix` host plugin (widescreen modes, Mods folder) | [reference/game-tge.md](reference/game-tge.md) |
| `pse.ids` – stable ids → addresses per exe build (Address Library), verified by signatures at start-up | [reference/ids.md](reference/ids.md); DS1 list (468 ids): [reference/ids-ds1.md](reference/ids-ds1.md); DS2 list (115 ids): [reference/ids-ds2.md](reference/ids-ds2.md); TGE list (41 ids, unpublished): [reference/ids-tge.md](reference/ids-tge.md) |

Headers: [`pse/include/pse/pse_api.h`](../../pse/include/pse/pse_api.h) (loader),
[`pse/include/pse/pse_vfs.h`](../../pse/include/pse/pse_vfs.h) (`pse.vfs`),
[`pse/include/pse/pse_game_ds1.h`](../../pse/include/pse/pse_game_ds1.h) (Desert Storm),
[`pse/include/pse/pse_game_ds2.h`](../../pse/include/pse/pse_game_ds2.h) (Desert Storm II),
[`pse/include/pse/pse_game_tge.h`](../../pse/include/pse/pse_game_tge.h) (The Great Escape),
[`pse/include/pse/pse_ids.h`](../../pse/include/pse/pse_ids.h) + [`pse_ids_ds1.h`](../../pse/include/pse/pse_ids_ds1.h) +
[`pse_ids_ds2.h`](../../pse/include/pse/pse_ids_ds2.h) (ids; the constants are generated from
[`pse/ids/ds1.ids.txt`](../../pse/ids/ds1.ids.txt) / [`ds2.ids.txt`](../../pse/ids/ds2.ids.txt) by `scripts/gen_ids.py`).

## Status (phases 1 + 2)

- **Phase 2 split (2026-10-19)**: `dinput8.dll` = the game-neutral shim (`pse/shim`), `PSE\pse_loader.dll` = the one
  copy of the loader services + start-up (`pse/loader_dll`, `pse/loader/startup.cpp`), `PSE\Games\pse_ds1.dll` = the
  DS1 game layer (`pse/games/ds1`: detection by SHA-256, the `pse.ids` database, `pse.game.ds1`), DesertStormFix =
  the host plugin `PSE\Plugins\dsfix.dll` (`host = true`, private `pse.host` through `pse/hostlink`,
  [reference/plugins.md](reference/plugins.md#host-plugins)) that registers the 60 `dsfix.<feature>` sub-plugins.
  Missing loader / game layer / unknown exe = vanilla game ([layout.md](layout.md)).
- Built and tested: all interfaces above, the plugin registry, the DS1 host glue (`src/core/pse_host.cpp`).
- In the game: every DesertStormFix feature was first an internal plugin `dsfix.<feature>` (60); since 2026-10-25 they
  are plugin DLLs around a small host layer ([ds1-plugins.md](ds1-plugins.md), [ds1-host-layer.md](ds1-host-layer.md)),
  and everything `core/patch` does goes through the broker owned by that plugin – byte groups, call hooks, indirect
  call hooks, jumps (`jmp@0x…` groups) – with the old semantics kept ([reference/hooks.md](reference/hooks.md#ds1-host-desertstormfixs-corepatch-on-the-broker)).
  First shared chain: the texture bind `0x53CC9B` (weaponicons → barcolour → fontsharp).
- Phase 2 started: the first game interface `pse.game.ds1` v1 ([reference/game-ds1.md](reference/game-ds1.md)) and
  the first real external plugin, **`dsfix.discord`** (Discord status, `plugins/discord/`), built from the SDK headers
  only, loaded from `<game>\PSE\Plugins`, shipped in the release zip ([ds1-plugins.md](ds1-plugins.md#external-plugins)).
- Phase 2 groundwork: external plugin DLLs (`pse/loader/plugins.cpp`: manifests, dependency resolution, SEH-guarded
  load, clean disable, unload) with three examples in `pse/examples/` built by `pse_add_plugin()`
  (`pse/cmake/PSEPlugin.cmake`) and loaded by `pse_tests`; the loader loads `<game>\PSE\Plugins` in two rounds (host
  plugins, then the rest).
- Not yet: entry hooks with trampolines in `src/` are still `WriteJump` + hand-made trampolines (owned as `jmp@`
  groups, not `HookFunction` chains), vtable / IAT slot writes are plain `patch::Write`, DesertStormFix split into
  several plugin DLLs (one host plugin today), no UI (`pse.lua` v1 is built: [reference/lua.md](reference/lua.md); `pse.vfs` v1 is built: [reference/vfs.md](reference/vfs.md)), game interface v1 read-only.

## Rule

Every change to an interface, message, event, Lua function or the plugin layout updates its reference page and
API_CHANGELOG.md in the same change.

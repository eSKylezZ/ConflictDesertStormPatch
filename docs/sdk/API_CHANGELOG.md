# PSE API changelog

Every change to a PSE interface, ID database or game layer, with what changed and how to migrate. The player-facing
changes are in the repo's `CHANGELOG.md`.

## API 0 (unstable, phase 1)

First implementation, inside DesertStormFix's `dinput8.dll`. Anything may still change until SDK 1.0.

- 2026-10-19 – **phase 2 layout** ([layout.md](layout.md)). No public interface changed (`PSE_API_VERSION` stays 0);
  plugin DLLs built against API 0 load as before. What changed around them:
  - the loader runs in `PSE\pse_loader.dll`, started by the shim `dinput8.dll` at the exe's entry point; the game is
    recognised by `PSE\Games\pse_<game>.dll` (`PSEGame_Info` / `PSEGame_Detect` / `PSEGame_Init`, private ABI
    `pse/loader/host_api.h`); unknown exe = no plugins;
  - `pse.game.ds1` 1.0.0 is served by the game layer (was: DesertStormFix's DLL); unchanged table;
  - DesertStormFix is the host plugin `dsfix` (`PSE\Plugins\dsfix.dll`, external, `host = true`), its features
    `dsfix.<feature>` stay internal sub-plugins (`PSE_PLUGIN_INTERNAL`) - `dev.pse_plugins()` now also reports
    `external` / `host` / `disabled`;
  - manifest key `host` (private), two load rounds (host plugins first; a plugin may `require` a host's sub-plugin);
  - new PRIVATE table `pse.host` v0 for host plugins / game layers only ([reference/plugins.md](reference/plugins.md#host-plugins));
  - online plugin-set check: a host plugin counts as built in (`=` hash, covered by the host fingerprint) - so the
    entry for `dsfix` is unchanged between a dev build and the release package;
  - DS1 private interface `pse.game.ds1.host` v0 (game layer ↔ DesertStormFix: providers, level titles).

- 2026-10-19 – **second game: Conflict: Desert Storm II** ([reference/game-ds2.md](reference/game-ds2.md)).
  `PSE_API_VERSION` stays 0; the loader, shim and loader interfaces are unchanged. New:
  - game layer `PSE\Games\pse_ds2.dll` (`pse/games/ds2/`, game id `ds2`): detects `CDS II.exe` 1.0 disc-free, drives
    the same core events / messages as DS1 (+ `PSE_MSG_EXIT` from its own WM_DESTROY hook), the co-save, and writes
    `PSE\Logs\loader.log`; allows the host plugin name `ds2fix`;
  - `pse.game.ds2` 1.0.0 (new header `pse/include/pse/pse_game_ds2.h`): `FrontEndState`, `DispatchMode`, `LevelName`,
    `LevelKind`, `InCutscene`, `SplitScreenPlayers`, `GameWindow`, `ExeSha256`, `ExeKnown`; `PSE_DS2_LAYER_PRIORITY_*`;
  - **ds2 ID database 1** (`pse/ids/ds2.ids.txt`, `pse/include/pse/pse_ids_ds2.h`): ids 1–115 published, build
    `nodisc-1.0`; `scripts/gen_ids.py` knows the DS2 exe (`DEFAULT_EXES`) and per-game patch tables (`PATCH_DEFS`);
  - examples `example.hello` and `example.events_logger`: `games` = `{"ds1", "ds2"}` (the same DLLs run on both);
  - tests: `pse/tests/exe_image.*` maps either game's exe (`exe::MapGame`, `PSE_REQUIRE_EXE_FOR`), reservation
    0x400000..0x9A6000; `test_game_ds2.cpp`; TEST ONLY plugin `test.ds2_harness`; CMake `pse_ds2`, `pse_tree_ds2`
    (`build*/tree-ds2/`, `DS2_GAME_DIR`).

- 2026-10-19 – **the game layer owns the core game events** ([reference/game-ds1.md](reference/game-ds1.md#events-and-messages)).
  `PSE_API_VERSION` stays 0; plugins built before keep working. Additive:
  - `pse_api.h`: messages `PSE_MSG_FRONT_END_READY` (10, data = level name), `PSE_MSG_DEVICE_CREATED` (11),
    `PSE_MSG_DEVICE_RESET` (12), `PSE_MSG_DEVICE_LOST` (13) (data = the device, `dataLen` 0) - sent by game layers;
  - `pse_game_ds1.h`: `PSE_DS1_LAYER_PRIORITY_INNER` (-1000) / `_OUTER` (1000), where the layer's hooks sit;
  - the DS1 layer (`PSE\Games\pse_ds1.dll`) now raises `PSE_EVENT_FRAME` (with the tasks), `PRE_RENDER` /
    `POST_RENDER`, `END_SCENE` / `PRESENT`, `LEVEL_LOAD_BEGIN` / `END` / `UNLOAD` and sends `NEW_GAME`, `SAVE_*` /
    `LOAD_*` (+ the co-save) and the new messages from its own broker hooks - **also without DesertStormFix**
    (before: only `PSE_EVENT_FRAME` and the save messages existed, both driven by DesertStormFix; PRE / POST_RENDER,
    END_SCENE, PRESENT and the LEVEL_* events were documented but never raised);
  - documentation fixes: `LEVEL_LOAD_END` / `LEVEL_UNLOAD` carry the level name, `NEW_GAME` the level name and
    `LOAD_BEGIN` / `LOAD_END` the save path (as the DS1 layer always sent them);
  - private `pse.game.ds1.host` v0 grew `SetLegacyCosave` (appended; DesertStormFix's `.sav.dsfix` reader).
  - Example `pse/examples/events_logger` (`example.events_logger`): logs all of it to a file and keeps a co-save chunk.

- 2026-10-19 – **`pse.vfs` 1** (new, `pse/include/pse/pse_vfs.h`, [reference](reference/vfs.md)): providers
  (`RegisterProvider` / `UnregisterProvider`, priority order, answer by name or the game's name hash in a context),
  `Exists`, `ResolvePath` (disk file + offset + length), `Read`, `ReadAlloc` / `Free`, `SetNameHash` / `NameHash`,
  `ListProviders`; external providers under the crash guard, removed with their plugin. In its own header (not
  `pse_api.h`); `PSE_API_VERSION` stays 0. DS1: only the dev-only asset store uses it so far
  (`Dev\PseStore`); the game's archive lookups ask it only when that is on.
  Loader internals (not SDK): `detail::ForgetVfs` / `ResetVfs`, `GuardedVfsLocate` / `GuardedVfsRead`. New internal
  sub-plugin `dsfix.assetstore` (60 now). Not in v1: `MergedDir`, table-merge helpers.
- 2026-10-19 – **`pse_build` 1.0.0** (not a plugin interface): `pse/build` library + `pse_build.exe`
  – asset store format 1 (`store.pack` / `store.idx` / `manifests\*.man` / `stamp.txt`).
  A different store format or builder major version rebuilds the cache.

- `pse.hooks` 1 (broker: byte ownership, chained call-site / function / indirect-call / pointer hooks) –
  [reference](reference/hooks.md)
  - 2026-10-19: no interface change. DS1 host: DesertStormFix's `core/patch` call hooks, indirect call hooks and jumps
    now go through the broker too, each feature is an internal plugin `dsfix.<feature>` owning its patches
    ([ds1-plugins.md](ds1-plugins.md)); compatibility rules in the reference ("DS1 host"). `HookCall` with a function
    that already runs first at the site now returns true (was false).
  - 2026-10-19 (later): no interface change. DS1 host: `core/patch` gained `HookSlot` / `UnhookSlot` (broker
    `HookPointer`), `HookFunction` / `UnhookFunction` / `PrologueLength` (broker `HookFunction`), `WriteJump(site, fn,
    len, original)`, `WriteCall`, `RevertCode` (checked `jmp@` / `call@` byte groups). The features' vtable / script
    API / callback-push slot writes, hand-written call / jmp bytes and hand-made entry trampolines use them, so those
    bytes have owners and shared sites chain (e.g. the image draw entry `0x53C9E0`: overlay + the dev HUD probe).
    What is still raw and why: reference "DS1 host" → "Still raw".
  - 2026-10-19 (later): no interface change. DS1 host: the D3D8 COM vtable hooks (graphics.cpp device / IDirect3D8
    slots, memory.cpp texture `Release`) are broker pointer hooks now, one record per COM class (`src/core/comhooks.h`),
    owners `dsfix.graphics` / `dsfix.memory`, priority 0 - other plugins can chain on Present / EndScene / … in front
    of or behind DSFix ([reference](reference/hooks.md) "COM vtables (D3D8)"). The clock call sites and
    `timeGetTime` / `GetTickCount` import slots shared by `dsfix.determinism` (200) and `dsfix.lockstep` (100) are
    priority chains ("Clocks"); lockstep's / determinism's other entry jumps and slot writes are broker hooks.
- `pse.ui` 1 (new, 2026-10-19) – [reference](reference/ui.md), header `pse/pse_ui.h`. Served by the game's host
  (DS1: DesertStormFix, registered before other plugins load; may be NULL without it): `DrawRect` / `DrawText` /
  `DrawImage` / `LoadImage` / `TextWidth` / `GetScreen` (per-frame overlay, screen fractions or pixels, game fonts,
  `PSE_UI_FOLLOW_MENU` fade), `RegisterText` / `TextHash` (game text hashes handed out per plugin + key, DS1
  `0xC4000000 | plugin << 12 | n` – replaces hand-picked `0xC0xx` ranges for plugins), `ShowPopup`, `CaptureKeyboard`
  / `ReadKey`. Deferred: own front-end screens, pause-menu items, editable strings, icon characters. Example
  `pse/examples/ui_demo`.
- `pse.log` 1 – [reference](reference/log.md)
- `pse.messaging` 1 – [reference](reference/messaging.md)
- `pse.tasks` 1 – [reference](reference/tasks.md)
- `pse.settings` 1 – [reference](reference/settings.md)
  - 2026-10-19 (later): no interface change. DS1: declared settings now show up without plugin UI code – OPTIONS >
    DESERTSTORMFIX > PLUGINS lists the plugins (load state, refusal reason) and builds a settings page per plugin from
    `Describe`; `PSE_SETTING_LAUNCHER` bool / int / choice settings of external plugins get launcher rows (the
    `dsfix.discord` checkbox stays where it was). Reference: "In the menus and the launcher".
  - 2026-10-19: no interface change. DesertStormFix's own settings now live in plugin `dsfix`'s key
    (`HKCU\Software\PSE\ds1\Plugins\dsfix`), declared through `pse.settings`, copied once from the old `Enhancements`
    key ([ds1-settings.md](ds1-settings.md)). Loader internals (not SDK): `pse::PluginSettingsKey`,
    `pse::MigrateRegistryValues` (`pse/loader/settings_migrate.cpp`). Undeclared names in a plugin's key are
    documented as left alone (host / plugin raw values may share it).
- `pse.serialization` 1 (co-save format "PSE\x01") – [reference](reference/serialization.md)
  - 2026-10-19: no interface change. DS1 calls it now: `<name>.pse` next to each `<name>.sav`, written after the game's
    save, read after the game's load; `PSE_MSG_SAVE_BEGIN/END`, `PSE_MSG_LOAD_BEGIN/END` (data = the save path) and
    `PSE_MSG_NEW_GAME` (data = the level name; a game level started from the front end without a save) are sent
    (reference "DS1"). DesertStormFix's `.sav.dsfix` sidecar is replaced by chunks of `dsfix.loadout` / `dsfix.gear`
    / `dsfix.vehicles` ([ds1-plugins.md](ds1-plugins.md#co-save-chunks)); old sidecars are still read. Loader
    internals (not SDK): `pse::cosave::LoadBytes(data, len, label)`.
- `pse.game.ds1` 1, fix (2026-10-19): `LookupText` / `LevelTitle` return "" instead of crashing when asked while a
  level load has freed the text tables (no table change).
- `pse.game.ds1` 1 (2026-10-19, new; header `pse/pse_game_ds1.h`) – [reference](reference/game-ds1.md): the first game
  interface, read-only. `PSEGameDs1V1 {size, version, FrontEndState, DispatchMode, LevelName, LevelKind, InCutscene,
  TextHash, LookupText, LevelTitle, SplitScreenPlayers, LocalPlayerCount, GetLocalPlayer, GetOnline, GameWindow,
  ExeSha256, ExeKnown}`, types `PSEDs1LevelKind`, `PSEDs1Soldier`, `PSEDs1Vec3`, `PSEDs1Player`, `PSEDs1OnlineRole`,
  `PSEDs1Online`. Served by the built-in plugin `pse.game.ds1` 1.0.0 (manifests: `requires = pse.game.ds1 >= 1.0`)
  through `pse.messaging` `RegisterInterface`, so `PSELoadInterface.Query` finds it. First user: the external plugin
  `dsfix.discord` (was DesertStormFix's internal feature; its setting `DiscordPresence` moved to its own key –
  [ds1-settings.md](ds1-settings.md#discordpresence-moved-to-dsfixdiscord)).
- `pse.ids` 1 + **ds1 ID database 1** (2026-10-19, new; headers `pse/pse_ids.h`, generated `pse/pse_ids_ds1.h`) –
  [reference](reference/ids.md), [DS1 list](reference/ids-ds1.md): stable numeric ids with names → address per known
  exe build, checked against a byte signature once at start-up (mismatch / unknown build → 0 + one log line).
  `PSEIdsInterface {size, version, Resolve, ResolveByName, Name, IdOf, Info, InfoAt, Build}`, types `PSEId`,
  `PSEIdKind`, `PSEIdStatus`, `PSEIdInfo`, `PSEIdBuildInfo`; macros `PSE_ID(game, name)`, `PSE_RESOLVE(ids, game,
  name)`, C++ `PSEResolveAs<T>`. ds1 database 1: ids 1–468 published (260 functions, 69 sites, 87 globals, 15
  vtables, 37 data), build `steam-gog-1.0` (PE timestamp 0x594D0000, SHA-256 `D712F5EC…`). Source of truth
  `pse/ids/ds1.ids.txt`, frozen list `pse/ids/ds1.published.txt`, compiler `scripts/gen_ids.py` (also fills /
  verifies signatures from the exe). Host side (not SDK): `pse/loader/ids.*` (`pse::ids::RegisterGameForProcess`),
  generated table `src/generated/pse_ids_ds1.h`.
- `pse.lua` 1 (2026-10-19, new; block "pse.lua" appended to `pse_api.h`) – [reference](reference/lua.md): Lua functions
  for mission scripts without linking Lua. `PSELuaInterface {size, version, RegisterNamespace, RegisterFunction,
  ArgCount, Type, CheckInteger, CheckNumber, CheckString, CheckTable, ToBoolean, Length, GetField, GetIndex, Next,
  NewNil, NewBoolean, NewInteger, NewNumber, NewString, NewTable, SetField, SetIndex, Return, PushNil, PushBoolean,
  PushInteger, PushNumber, PushString, PushTable, Error, FunctionName, CallGlobal, LastError, ScriptRunning}`, types
  `PSELuaCall` (opaque), `PSELuaSlot`, `PSELuaType`, `PSELuaNative`; `PSE_LUA_RETURN` / `PSE_LUA_ERROR`,
  `PSE_LUA_NUL`, `PSE_E_SCRIPT` (11, a `#define` – `PSEResult` itself unchanged); events `pse.lua.level_start` /
  `pse.lua.level_end` (data = level name). Natives of external plugins are crash-guarded ("crashed in the Lua function
  ns.fn"); a crashed / unloaded plugin's namespace is freed and its functions raise "not loaded". DS1: `.lua` levels
  get every namespace before the script runs (scripthost.cpp). Loader internals (not SDK): `pse/loader/lua_iface.*` incl. the
  private table `pse.lua.host` v1 (host plugins / game layers / built-in plugins only), header-only binding
  `pse/loader/lua_bridge.h`, `detail::GuardedLuaNative` (guard.cpp), `detail::ForgetLua` (plugin
  cleanup). Example `example.lua_native` (`pse/examples/lua_native`). `pse_tests` now links `lua_static`.
- Crash guard (2026-10-19, no header change) – [reference](reference/plugins.md#crash-guard-callbacks-after-load):
  every call into an **external** plugin after `Load` is SEH-guarded too – message listeners (incl.
  `PSE_MSG_POST_LOAD` / `PSE_MSG_POST_POST_LOAD`), event subscribers, tasks, settings `OnChange`, co-save save / load /
  revert, `PSEPlugin_Unload`. A fault disables the plugin like a failed `Load` (everything it registered removed,
  result `failed` with the reason, ERROR log + `[fail]` report line) but its DLL stays mapped; the delivery in
  progress goes on with the others. Built-in plugins are called directly as before. ~2 ns per guarded call. Not
  guardable: hook detours, cross-plugin interface calls (the calling plugin is blamed). Loader internals (not SDK):
  `pse/loader/guard.cpp` (`detail::Guarded*`, `Call*`, `PluginCrashed`). Examples `example.crash_postload`,
  `example.crash_frame`, `example.crash_task`, `example.crash_callbacks` (`pse/examples/crash_later`).
- Online plugin-set check (2026-10-19, no header change) – [online-compat.md](online-compat.md): protocol 1
  (`pse::compat::kProtocol`), `PSEC` game-command lines; simulation plugins must match by name + version + kind + DLL
  SHA-256, built-in ones by name + version + host simulation fingerprint, switched-on mods by name + content hash;
  a mismatch refuses the join on both sides. Loader internals (not SDK): `pse/loader/compat.cpp` (`pse::compat::Set`,
  `Encode`, `Assembler`, `Compare`, `LocalPlugins`, `Sha256`). Example `example.sim_rules` (SIMULATION-flagged).
  Plugin authors: the flags now matter online – see "For plugin authors" there.
- Plugin DLLs (2026-10-19) – [reference](reference/plugins.md): `<Name>.pse` manifest (`name`, `requires = a >= x.y`,
  `optional`, `conflicts`, `after`), exports `PSEPlugin_Info` (read from the file before loading – must be constant
  data, keep `extern` on it in C++), `PSEPlugin_Load` (non-zero = loaded; 0 or a crash = everything the plugin
  registered is removed and it is disabled), optional `PSEPlugin_Unload` (reverse load order). Load order:
  topological, ties by name. `PSE_MSG_POST_LOAD` / `PSE_MSG_POST_POST_LOAD` now sent by the loader after the plugins.
  CMake helper `pse_add_plugin()` (`pse/cmake/PSEPlugin.cmake`). No header change.

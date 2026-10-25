# PSE API changelog

Every change to a PSE interface, ID database or game layer, with what changed and how to migrate. The player-facing
changes are in the repo's `CHANGELOG.md`.

## API 0 (unstable, phase 1)

First implementation, inside DesertStormFix's `dinput8.dll`. Anything may still change until SDK 1.0.

- 2026-10-24 – **New interface `dsfix.services` v1** (`games/ds1/src/core/dsfix_services.h`, published by `dsfix.dll`
  at the end of its Load): game texts, control icons, online state, the host's synced values, mods' vehicle handling,
  plus a text provider and a trigger feel provider for features in their own DLLs
  ([ds1-plugins.md](ds1-plugins.md#external-plugins)). **Plugin layout**: `dsfix.difficulty`, `dsfix.jpfixes`,
  `dsfix.handling`, `dsfix.tooltips`, `dsfix.rumble` are their own DLLs on it (`requires = dsfix >= 3.3`); their
  settings `FeatVeryHard` / `FeatJpFixes` / `FeatRumble` / `FeatTooltips` moved to their own keys (copied once,
  [ds1-settings.md](ds1-settings.md#featshadowfix--feataimaudio--mouseacceleration-and-four-more-moved-to-their-plugins)).
  The split-screen weapon-name byte fix `0x454558` is `dsfix.splitscreen`'s now. dsfix kit: `kit::DeclareBool` takes
  `flags` (default `PSE_SETTING_LIVE`; 0 = restart) and `kit::Services()`. OPTIONS > DESERTSTORMFIX plugin rows honour
  a setting declared without `PSE_SETTING_LIVE` ("ON *", restart footer). Dev server: `dev.plugin_setting_flags`,
  `dev.plugin_restart_pending`. Same day, appended to v1: `DsfixSetting`, `ModsFolderEnabled`, `SkinBody`,
  `SkinNormalMap`, `ModReport`, `LoadoutScreenOpen`, `GiveSpawnedWeapon`, `GiveSpawnedMedikit`, `HostValue("followers")`
  - and `dsfix.bodies` (mod bodies + multi-part draw) / `dsfix.followers` (setting `FeatFollowers`) are their own DLLs
  too. The mid-level skin work the loadout screen needs stays in dsfix.dll as the new internal `dsfix.skins`
  (skin-apply entry hook, chained with `dsfix.bodies`'). `core/patch.cpp` `HookFunction`: an entry another plugin's
  copy of patch.cpp already hooked (its jump in place of the prologue) is chained onto through the broker instead of
  refused as "unexpected bytes". Later the same day: `SetSettingsListener` appended to v1 (called after the launcher
  saves DesertStormFix's settings) and **`dsfix.hud`** (HUD / menu scaling) is its own DLL too - its settings stay
  DesertStormFix's (`HudScaling`, `HudScalePercent`). Dev server: `dev.settings_changed()`. Then **`dsfix.weather`**
  (weather + weather sounds + footprints, one DLL) publishing the new interface **`dsfix.weather` v1**
  (`plugins/weather/dsfix_weather.h`) for dsfix.dll's loadout row, Lua `game.weather` and the online token;
  `dsfix.services` v1 gained `GameFocused` and `DrawPuddleReflections` (`DsfixPuddlePart` / `DsfixWaterLook`) and
  `HostValue("weather")`. The raw `Weather` value moved to the plugin's key. dsfix kit: `kit::DeclareInt` / `GetInt` /
  `SetInt`. Dev server: `dev.weather()`. Build: the install-tree file list goes to `pse/cmake/assemble.cmake` as a
  generated file (`-DFILES_FROM=`; the command line had passed cmd.exe's 8191 characters). Then **`dsfix.timeofday`**
  with the new interface **`dsfix.timeofday` v1** (`plugins/timeofday/dsfix_timeofday.h`) for the loadout TIME OF DAY /
  TIME FLOW rows and the online token; `dsfix.services` v1 gained `LinkedFrom()` and `HostValue("timeofday_on" /
  "timeofday" / "timeflow")`. The raw `TimeOfDay` / `TimeFlow` values moved to the plugin's key. Then **`dsfix.voices`**
  with **`dsfix.voices` v1** (`plugins/voices/dsfix_voices.h`) for the VOICE row and the online token;
  `dsfix.services` v1 gained `SkinVoice`, `SpeechLanguage` and `HostValue("voices" / "voice0".."voice3")`; the raw
  `Voice<SP|Coop><role>` values moved to the plugin's key. Then **`dsfix.fontsharp`** (sharper text, last subscriber of
  the shared glyph bind `0x53CC9B`); its setting `FeatSharpText` moved to the plugin's key (imported once). No API
  change: it uses `DsfixSetting("HudScaling")` and `SetSettingsListener`. Then **`dsfix.developer`** (CHEATS >
  DEVELOPER + debug keys); `dsfix.services` v1 gained **`SetScreenProvider(plugin, fn)`** (`DsfixScreenProvider`:
  a plugin's screen object for a front-end state dsfix.dll doesn't serve) - check `size` before calling it.
  Then the new interface **`dsfix.overlay` v1** (`src/core/dsfix_overlay.h`: DesertStormFix's overlay drawing as plain C;
  the kit implements `overlay.h` on it), `dsfix.services` v1 gained `MissionCutsceneSkip`, `DevBackground` and
  `QuickStartLevel`, and **`dsfix.cinematics`** / **`dsfix.display`** (with interface **`dsfix.display` v1**) are their
  own DLLs.
  Then **`dsfix.photo`** (photo mode, frames, free camera, CHEATS menu, pause menu items) as one DLL, on the new
  generated cross-DLL tables (`games/ds1/scripts/gen_host_calls.py`): **`dsfix.host`** (`src/core/dsfix_host.spec`,
  what the plugins call in dsfix.dll) and **`dsfix.photo`** (`plugins/photo/photo.spec`) - DesertStormFix-internal,
  not part of the SDK (the layout hash changes with every build). `CheatsUnlocked` moved to `dsfix.photo`'s key.
- 2026-10-25 – **`dsfix.online`**: online co-op (connection, lobby, plugin-set check + mod sync, lockstep / host
  authority, own view, nameplates, determinism harness) is one plugin DLL on the generated tables (`plugins/online/online.spec`;
  `dsfix.host` gained `CompatLocalPluginsText` / `DsfixSettingsValues`). The internal plugins `dsfix.nameplates`,
  `dsfix.views`, `dsfix.determinism`, `dsfix.lockstep` and `dsfix.authority` are gone; their hooks are owned by
  `dsfix.online`. The loader's online check is split: `pse/loader/compat.cpp` (pure: encoding, comparison, SHA-256) and
  `compat_local.cpp` (`LocalPlugins`, loader state). Generator: the wrappers' table getter is named per group.
- 2026-10-25 – **`dsfix.missions`**: the Lua script host, Lua films, translated missions + `x86rt`, the co-op mission
  scripts and the dev API trace are one plugin DLL (`plugins/missions/missions.spec`). The internal plugins
  `dsfix.scripthost`, `dsfix.missionrt`, `dsfix.missionsource`, `dsfix.apitrace` and `dsfix.coopscripts` are gone; the
  setting `TranslatedMissions` moved to `dsfix.missions`' key (copied once). `pse.lua.host` stays host-plugins-only:
  dsfix.dll passes its table on (`dsfix.host` `LuaHostTable`; `lua_bridge.h` gained `AttachTable`). Plugin Lua
  namespaces (pse.lua) are unchanged for plugin authors. `features::SetScriptKit` takes the four slots as strings.
- 2026-10-25 – **Manifest key `adopts`** ([reference/plugins.md](reference/plugins.md),
  [reference/serialization.md](reference/serialization.md)): `adopts = old.name, ...` hands a plugin the co-save chunks
  saved under those names while no plugin of that name is loaded; the next save writes them under its own name only (no
  old copies kept). For plugins that merge or rename older ones. Loader: `SetPluginAdopts` / `PluginAdopting`.
- 2026-10-25 – **`dsfix.loadout`**: the briefing / loadout screen, kits, gear, vehicle swaps, the mission overview and
  mid-level skins are one plugin DLL (`plugins/loadout/loadout.spec`). The internal plugins `dsfix.customise`,
  `dsfix.gear`, `dsfix.vehicles`, `dsfix.overview` and `dsfix.skins` are gone; `GEAR` / `VEHS` co-save chunks are
  written under `dsfix.loadout` now (old names adopted). `dsfix.host` gained text-row versions of mods.cpp's struct lists
  (`ModGearRows`, `ModPouchSetRows`, `ModVehicleRows`, `SkinVariantRows`), `BorrowVehicleDataList`, `SetLoadoutSpin`.
- 2026-10-25 – **`dsfix.timing`** and **`dsfix.barcolour`**: frame timing (exact frame time, high-FPS landing /
  physics) and the menu bar / HUD colour with its colour wheel are plugin DLLs (`plugins/timing/`,
  `plugins/barcolour/barcolour.spec`; the PLUGINS menu's colour settings use that wheel through the interface
  `dsfix.barcolour`). `scripts/gen_host_calls.py` passes `long long` / `LONGLONG`. The developer console and
  diagnostics stay in dsfix.dll (host layer).
- 2026-10-25 – **`dsfix.graphics`**: the D3D8 device hooks (MSAA, anisotropy, V-Sync, scene fog, photo capture),
  the D3D9 / D3D9On12 / DXGI renderers and OPTIONS > VIDEO are one plugin DLL (`plugins/graphics/graphics.spec`; the
  HDR / water shaders compile into it). DesertStormFix's overlay now draws on the game layer's `PSE_EVENT_END_SCENE`
  (first subscriber) instead of from that EndScene hook, so it works without the plugin. The internal plugin
  `dsfix.videomenu` is `dsfix.restart` now (only the `+restart` wait); the frame cap, frame timing and frame recorder
  stay in dsfix.dll.
- 2026-10-25 – **`dsfix.coop`**: split screen, co-op, the CONTROLS menu / pad layouts, controllers, hot-plug, pad I/O,
  several keyboards / mice and VERSUS are one plugin DLL (`plugins/coop/coop.spec`; the controller pictures moved to its
  `coop.rc`). The main menu's CO-OP / VERSUS / MODS items and the front-end screen router stay in dsfix.dll as the new
  internal plugin `dsfix.frontend` (`features/frontend.cpp`). The internal plugins `dsfix.splitscreen`,
  `dsfix.controls`, `dsfix.controller`, `dsfix.hotplug`, `dsfix.multikbm` and `dsfix.versus` are gone (`dsfix.coop` is
  the DLL's name now).
- 2026-10-23 – **pse.lua on Desert Storm II**: `ds2fix` (sub-plugin `ds2fix.scripthost`) owns a Lua state for `.lua`
  levels and binds it with `lua_bridge.h` like DS1 - plugin namespaces, `CallGlobal`, `pse.lua.level_start` /
  `level_end` now also on DS2 ([lua.md](reference/lua.md#ds2)). No API change (the bridge was game-neutral already).
  DS2 modders: [ds2-mods.md](ds2-mods.md#lua-missions-and-custom-campaigns).

- 2026-10-23 – **New optional plugin export `PSEPlugin_SimFingerprint`** (`pse_api.h`, `PSEPluginSimFingerprintFn`;
  [online-compat.md](online-compat.md#external-plugins-declared-simulation-fingerprint)). A SIMULATION plugin DLL that
  exports it is compared in the online plugin-set check by `fp:` + SHA-256 of the returned string instead of the DLL
  file's SHA-256, so a dev build and a release build of one source connect. Plugins without it: unchanged. Loader
  `compat::kFingerprintPrefix`; new example `pse/examples/sim_fingerprint`. DesertStormFix's kit plugins export one
  (`dsfix::kSimRevision` moved to `games/ds1/src/core/simrevision.h`).

- 2026-10-23 – **Plugin layout: six more DesertStormFix features are their own DLLs** – `dsfix.widescreen`,
  `dsfix.textfit`, `dsfix.mouse`, `dsfix.menus` (CLIENT_ONLY), `dsfix.units`, `dsfix.limits` (SIMULATION, with a
  declared fingerprint). Same names as the internal sub-plugins they replace. New interface **`dsfix.limits` v1**
  (`games/ds1/plugins/limits/dsfix_limits.h`: `HeapExtra()`), replacing the in-DLL call `features::LimitsHeapExtra`.
  Setting `MouseAcceleration` moved to `dsfix.mouse`'s key (copied once).

- 2026-10-24 – **TGE: films fill the screen, objectives notebook, frame cap, background tests** (internal plugins
  `tgefix.widescreen`, `tgefix.hud`, `tgefix.framecap`, dev-only `tgefix.devbg`; [game-tge.md](reference/game-tge.md)).
  No interface changed. New `tgefix` settings `FilmsFill` (the main menu's 4:3 background film zoomed to the screen's
  width), `FrameCap`, `FpsCap`. The objectives notebook is laid out on a centred 4:3 screen scaled to the height, as at 800x600
  (on wide screens its text had slid off the book's pages). Dev builds: `HKCU\Software\GreatEscapeFix\Dev\Background` = 1 runs the game off-screen for scripted
  tests (back-buffer shots via `Dev\Shot`).

- 2026-10-24 – **TGE: menu / HUD scaling and windowed mode** (internal plugins `tgefix.hud`, `tgefix.display`,
  [game-tge.md](reference/game-tge.md#menu-and-hud-scaling-tgefixhud-client_only-gamestgesrcfeaturesguiscalecpp)).
  No interface changed. New `tgefix` settings `HudScaling`, `HudScalePercent`, `Windowed` (launcher Settings… >
  Windowed). The main menu's 4:3 bars are its background film's shape, not a mask (documented, kept).
  Fullscreen start fixed: the game window is brought to the front (it could start as a black 800 x 600 box behind
  another program), and a fullscreen window covers the screen at the chosen mode (other modes than 800x600 showed only
  their top-left 800 x 600).

- 2026-10-23 – **New game: The Great Escape** (game id `tge`, [game-tge.md](reference/game-tge.md)). New game layer
  `PSE\Games\pse_tge.dll` (`pse/games/tge/`), new interface **`pse.game.tge` v1** (`pse/include/pse/pse_game_tge.h`:
  DispatchMode, LevelTableIndex, LevelName, LevelKind, GameWindow, ExeSha256, ExeKnown; read-only), new ID database
  `pse/ids/tge.ids.txt` (41 ids, database 0 – none published yet, names may change; [ids-tge.md](reference/ids-tge.md)).
  The core events and messages are the same as DS1 / DS2 (no saves / co-save, no `NEW_GAME` yet). Host plugin
  `tgefix` (GreatEscapeFix, `games/tge/`): widescreen launcher modes, Mods folder. Shim and loader unchanged (the
  shim's "unsupported exe" text names GreatEscape.exe).

- 2026-10-23 – **DS2: weapon pictures from mods** (internal plugin `ds2fix.weaponicons`,
  [game-ds2.md](reference/game-ds2.md#weapon-pictures-ds2fixweaponicons-gamesds2srcfeaturesweaponiconscpp-2026-10-23),
  modder page [ds2-mods.md](ds2-mods.md#weapon-pictures)). No interface changed. `.weapon` `hud icon` now takes a picture
  file in the mod (`.png` / `.dds` / `.tga`); a broker entry hook on DS2's picture lookup `0x452170` (owner
  `ds2fix.weaponicons`, CLIENT_ONLY). Game picture names work as before.

- 2026-10-23 – **Plugin layout: `dsfix.shadows` and `dsfix.audio` are their own DLLs** (`PSE\Plugins\dsfix.shadows.dll`,
  `dsfix.audio.dll` + manifests; [ds1-plugins.md](ds1-plugins.md#external-plugins)). No interface changed. They were
  internal sub-plugins of `dsfix.dll` with the same names; now ordinary external CLIENT_ONLY plugins built on the new
  dsfix kit (`games/ds1/plugins/kit`, public SDK only). Their settings `FeatShadowFix` / `FeatAimAudio` moved from
  `dsfix`'s key to each plugin's own key (copied once, [ds1-settings.md](ds1-settings.md#featshadowfix--feataimaudio--mouseacceleration-and-four-more-moved-to-their-plugins)).
  Code that looked them up with `FindPlugin` still finds them; their hooks are owned by the DLL plugins.

- 2026-10-22 – **Repository: DesertStormFix moved to `games/ds1/`** ([layout.md](layout.md#repository-layout)). No
  interface changed. `src/` → `games/ds1/src/`, `plugins/` → `games/ds1/plugins/`, `dist/` → `games/ds1/dist/`,
  DS1's generator scripts (`patch_defs.py`, `gen_patch_tables.py`, icon / art / anim generators) →
  `games/ds1/scripts/`; the generated PSE id tables `pse_ids_<game>.h` → `pse/ids/generated/` (`scripts/gen_ids.py`
  writes them there; include path `pse/ids`). The root CMake still builds DS1 + PSE; DS2's PSE build takes the shared
  `core/patch.*` and patcher from `games/ds1/src`.

- 2026-10-22 – **DS1 game events: soldier damage and deaths** ([game-ds1.md](reference/game-ds1.md#game-events-soldier-damage-and-deaths)).
  `PSE_API_VERSION` stays 0; `pse.game.ds1` stays v1 (nothing appended to the table). New in `pse_game_ds1.h`: the
  named events `ds1.soldier_damage` (before - a subscriber may change `amount`, the first write-side hook of the game
  interface), `ds1.soldier_damaged`, `ds1.soldier_killed` (macros `PSE_DS1_EVENT_SOLDIER_*`) and `PSEDs1DamageEvent`.
  Raised by the DS1 layer from an entry hook on `Soldier_TakeDamage` `0x424200` (OUTER). Same day: `ds1.vehicle_damage`
  / `_damaged` / `_destroyed` (`0x43E1F0`), `ds1.item_picked_up` (`PSEDs1PickUpEvent`, `0x432070` + its AddItem call),
  `ds1.objective_completed` (`PSEDs1ObjectiveEvent`, script API +0x4C0) and `ds1.mission_end` (`PSEDs1MissionEndEvent`,
  `0x4578A0`), and `ds1.script_message` (`PSEDs1ScriptMessageEvent`: every message the game sends the mission script,
  through a copy of the script object the layer installs after each level init). `example.events_logger` logs them.

- 2026-10-22 – **Loader / game layers: no DS1 defaults, DS1 runs without DesertStormFix** ([game-ds1.md](reference/game-ds1.md#events-and-messages),
  [game-ds2.md](reference/game-ds2.md#loader-neutrality-found-while-porting)). `PSE_API_VERSION` stays 0; no public
  interface changed. Loader internals: `pse::Config` has no default game (`gameId` null, `registryRoot` empty =
  `Software\PSE\<gameId>`), `pse::Init` returns bool and refuses a missing / bad id. DS1 layer: sends `PSE_MSG_EXIT`
  itself at `WM_DESTROY` (as DS2); the redirect to `PSE\Originals` (the exe's five file imports) moved here from
  DesertStormFix - **without DesertStormFix the game used to stop at "Couldn't open GenStrs.loc"**; the save / load
  messages carry the save path in UTF-8 (was the ANSI path; DS2 already sent UTF-8) and the co-save is found with it.
  DS2 layer: its own `loader.log` writer removed (the loader's fallback writes that file).

- 2026-10-22 – **DS1: several mods can change one level** ([data-files.md](data-files.md#several-mods-one-level)).
  `PSE_API_VERSION` stays 0; no interface changed. `.envpatch` files of the same scene from several mods are merged
  line by line (different lines all apply, adds at one place all kept in mod order, overlapping changes: the later
  mod wins + a `conflict:` line); before, only the first mod's patch was used and the others were dropped without a
  word. `.rmdpatch` files merge per route marker. Two patches adding objects with one script id = a `[fail]` line. A
  mod shipping the whole file now says that the patches aren't used. Placed-reference record edits report conflicts
  (`conflict: <id> <field>: A / B (B wins)`) like table records. New C++ `pse/records/scenepatch.h`
  (`MergeEnvPatches`, `MergeRmdPatches`, `SceneScriptIds`); ModKit `placed.apply_edits` reports the same conflicts.
  A `based_on` copy may ask for its script id (`script_id = n`; taken = the next free one + `[fail]`). ModKit
  editor: DS1 `save_mod` saves objects / waypoints / regions as `records\<level>.placed.lua` + only the rest as
  `.envpatch` (was a full scene copy), `open_mod_level`.

- 2026-10-22 – **DS1 placed-reference edits read by the game** ([data-files.md](data-files.md#placed-references-what-a-level-puts-in-its-world)).
  `PSE_API_VERSION` stays 0; no interface changed. A mod's `records\*.lua` edits of a level's objects / waypoints /
  regions (`ds1:<level>/<kind>/<editor id>`) now reach the game at start-up: DesertStormFix applies them to the
  level's scene (a mod's copy or one its `.envpatch` built, else the game's) into `merged\game\<LEVEL>.ENV` - no
  ModKit step. The table-record merge skips those ids (they used to be `[fail] ... no record type mission1`). The
  level part of an id ignores case. New C++ `pse/records/placed.h` (`PlacedLevels`, `ApplyPlaced`), the twin of the
  ModKit's `placed.apply_edits` / new `placed.merged_scenes` (`scripts/records_placed_gate.py`); `pse_build records
  merged` also writes the edited scenes. ModKit: a table into a non-vector field or a number into a vector field is
  a `[fail]` line now (was a Python error).

- 2026-10-22 – **DS2 levels from mods** ([ds2-mods.md](ds2-mods.md#levels-and-missions), [reference/game-ds2.md](reference/game-ds2.md#levels-from-mods-ds2fixmodlevels-gamesds2srcfeaturesmods_levelscpp)).
  `PSE_API_VERSION` stays 0; no interface changed. New internal sub-plugin `ds2fix.modlevels` (SIMULATION): mission
  DLLs from mods (by full path), `.dllpatch`, `<Level>.level` (`--@archive mission01` / `none`), `.uses` (`file`,
  `piece`; `bank` not yet), `.envpatch`, `.rmdpatch` (DS2 route files v3) built from the player's install into
  `%TEMP%\DesertStorm2Fix\merged\game`; a scene a mod changes gets its octree rebuilt (unless the mod ships a `.OCT`)
  and its AI walk map + route markers rebuilt with the level's hand-placed markers kept (unless it ships `.QTD` / `.RMD` /
  `.rmdpatch`). **A mod that shipped a changed `<SCENE>.ENV` together with the shipped `.OCT`** keeps that `.OCT` (no
  rebuild). C++ (features.h): `BuildModReferences`, `AddBuiltModFile`. ModKit: `kits.extract` cuts DS2 (EOBJ 0x35)
  pieces; editor `save_mod` writes `.envpatch` / `.rmdpatch` on DS2 (`Game.game_refs`).

- 2026-10-22 – **DS2 mod tables, texts, `.weapon` / `.vehicle` / `.unit`** ([ds2-mods.md](ds2-mods.md#tables--rows-added-replaced-or-removed), [reference/game-ds2.md](reference/game-ds2.md#tables-and-texts-gamesds2srcfeaturesmods_tablescpp-part-of-ds2fixmods-2026-10-22)).
  `PSE_API_VERSION` stays 0; no interface changed. `ds2fix.mods` now merges a mod's `<TABLE>.txt` (rows added) and
  `<TABLE>.replace.txt` (rows replaced / `-KEY` removed) with the game's table instead of replacing the whole file
  (**a mod that shipped a whole table now adds every row a second time - ship only its new rows, or the changed rows as
  `.replace.txt`**), drops rows the start-up parsers would quit on (`[fail]`), sets Units / Vehicle item and gun slots;
  `.strings` texts (UTF-8, `[all]` / `[Mission01]` sections, keys by name or `0x` hash), readable `.weapon` (DS2
  columns: DS1's + 4 tracer columns at 45-48), `.vehicle`, `.unit`. C++ (features.h): `BuildModTables`, `ModText`,
  `SetModFile`, `AddModRoot`.

- 2026-10-22 – **DS2 mod sounds** ([ds2-mods.md](ds2-mods.md#sounds), [reference/game-ds2.md](reference/game-ds2.md#mods-folder-sounds-ds2fixmodsound-gamesds2srcfeaturesmods_soundcpp)).
  `PSE_API_VERSION` stays 0; no interface changed. New internal sub-plugin `ds2fix.modsound` (CLIENT_ONLY): `.sch`
  sound packs added to every level, a `.sch` named like a level cache replaces that level's banks / samples (a
  language -1 sample replaces every language), `.psf` front-end music (any length) and mission music (same size only),
  `.bik` films by name. C++ (features.h, for DS2's `.weapon`): `MakeWeaponSoundBank`, `BorrowSoundBank`,
  `ClearWeaponSoundBanks`. A mod `.sch` named like a level is no longer a whole-file replacement but merged (a whole copy
  still behaves the same). ModKit `sound.decode_stream` reads DS2's `PSF\x81` streams.

- 2026-10-22 – **DS2 `.skin` files** ([ds2-mods.md](ds2-mods.md#skin-files--a-soldiers-own-textures-body-and-name), [reference/game-ds2.md](reference/game-ds2.md#skin-files-ds2fixskins-gamesds2srcfeaturesskinscpp)).
  `PSE_API_VERSION` stays 0; no interface changed. New internal sub-plugin `ds2fix.skins` (SIMULATION): mod format
  `.skin` on DS2 (`soldier`, `uniform`, `texture`, `texture <part>`, `body`, `first name` / `last name`, `portrait`) -
  per-soldier textures (whole character or per skin part) and bodies (any key count, built at creation) without
  touching shared files. Not DS1's `.skin` layout (DS2 has no uniform-variant / recruit rows; the soldier is picked by
  unit name).

- 2026-10-22 – **DS2 plugin `ds2fix.limits`** ([reference/game-ds2.md](reference/game-ds2.md#ds2fixlimits-gamesds2pluginslimits-2026-10-22)).
  `PSE_API_VERSION` stays 0; no interface changed. New external SIMULATION plugin (`games/ds2/plugins/limits`): engine
  pools raised for big battles (per-level physics pools from the `.FAS` raised to ≥ 4096 / 2048 / 4096, constraints,
  collision pairs, particles, projectiles, events, render arena, decals, A*, voices, AI think budget), saves kept
  readable by the unmodified game. First user of `ds2fix.memory`'s `AddHeapReserve` (2.5 MB).

- 2026-10-22 – **DS2 plugin `ds2fix.memory` + 4 GB patcher** ([reference/game-ds2.md](reference/game-ds2.md#ds2fixmemory-gamesds2pluginsmemory-2026-10-22)).
  `PSE_API_VERSION` stays 0. New external plugin `ds2fix.memory` (`games/ds2/plugins/memory`, CLIENT_ONLY): the Global
  Heap sized for mod files (cap 64 MB / 256 MB with 4 GB), texture files too big for it read outside it, memory log
  lines, main menu 4 GB notice. New plugin interface `ds2fix.memory` v1 (`games/ds2/src/pse/ds2fix_memory.h`:
  `AddHeapReserve`, `HeapSize`, `HeapCap`, `LargeAddressAware`, `LoadsBigTexturesOutsideHeap`, `GetStats`) - plugins
  that need heap room call `AddHeapReserve` at `PSE_MSG_POST_LOAD`. `ds2fix.mods`' heap hook passes through while the
  plugin is there and grows on top of what arrives otherwise. `DesertStorm2Fix-Patcher.exe` (DS1's patcher built with
  `PATCHER_GAME_DS2`; new exit code 6 = an exe that is never changed, the SecuROM one) ships in the DS2 package.

- 2026-10-22 – **DS2 MODS menu** ([ds2-mods.md](ds2-mods.md), [reference/game-ds2.md](reference/game-ds2.md#mods-folder-ds2fixmods-gamesds2srcfeaturesmodscpp)).
  `PSE_API_VERSION` stays 0; no interface changed. New internal sub-plugin `ds2fix.modsmenu` (CLIENT_ONLY): main menu
  MODS (front-end state 0x37), mods switched in game, the file index rebuilt live (Apply).

- 2026-10-22 – **DS1 online sessions, round 3** ([online-compat.md](online-compat.md#when-something-looks-different-on-two-pcs-the-state-dump)).
  `PSE_API_VERSION` stays 0; no plugin interface changed. New game commands `EA … CSV` / `CSN` / `CS` (cutscene skip
  vote), `EA … PU` (pick-ups), `EA … VX` (vehicle destroyed on its owner's PC); DesertStormFix's simulation revision
  is now 5, so older builds refuse to connect. New setting FeatStreamerMode ([ds1-settings.md](ds1-settings.md)). New
  file `DesertStormFix-sync.csv` (every build, online): a state dump for comparing two PCs.

- 2026-10-22 – **DS1 multi-part skins** (`dsfix.bodies`, [ds1-plugins.md](ds1-plugins.md)). No interface changed. A
  soldier body with several skinned parts (ModKit exporter 1.8+) now shows each extra part's own texture on the squad
  too; the squad's uniform covers only the main skin (the last skinned object, the slot the shipped bodies use).

- 2026-10-22 – **DS1 online lobby** ([online-compat.md](online-compat.md#the-lobby-and-the-loadout-screen-desertstormfix-same-channel)).
  `PSE_API_VERSION` stays 0; no plugin interface changed. DesertStormFix's game commands now include `LB` (lobby player
  table / requests) and `LO` (loadout screen online) next to `PSEC`; PC numbers are stable per session (host 1,
  joiners 2..), several players per PC. New setting FeatNameplates ([ds1-settings.md](ds1-settings.md)).

- 2026-10-22 – **DS2 plugin `ds2fix.widescreen`** ([reference/game-ds2.md](reference/game-ds2.md#ds2fixwidescreen-gamesds2pluginswidescreen-2026-10-22)).
  `PSE_API_VERSION` stays 0. Widescreen modes, HUD scaling + fixes, the virtual-screen front end / in-mission menus and
  sharper text left `ds2fix` (sub-plugins `ds2fix.widescreen` / `.hud` / `.frontend` / `.fontsharp` are gone) for the
  external plugin `ds2fix.widescreen`; its settings `HudScaling` / `HudScalePercent` moved to
  `HKCU\Software\PSE\ds2\Plugins\ds2fix.widescreen` (copied once from ds2fix's key). New plugin interface
  `ds2fix.hudhost` v1 (`games/ds2/src/pse/ds2fix_hud.h`, served by ds2fix): split-screen view size + the OPTIONS rows
  HUD SCALING / HUD SIZE.

- 2026-10-22 – **DesertStorm2Fix on PSE** ([reference/game-ds2.md](reference/game-ds2.md#desertstorm2fix-as-a-host-plugin)).
  `PSE_API_VERSION` stays 0; no loader interface changed. The DS2 release is the PSE tree (shim, loader, `pse_ds2.dll`,
  host plugin `ds2fix` + `games/ds2/plugins/<name>`); `ds2fix` settings moved to `HKCU\Software\PSE\ds2\Plugins\ds2fix`
  (copied once from `HKLM\SOFTWARE\SCi Games\CDS II\Enhancements`). New plugin interface `ds2fix.graphics` v1
  (`games/ds2/src/pse/ds2fix_graphics.h`: the front end's virtual screen + the D3D9 device) for DS2 plugins.
- 2026-10-21 – **record limits**: schema `max_len` (text / link columns) and `max_rows` (tables); mods' record edits past them are refused with `[fail]` report lines (C++ `pse/records/forms` = ModKit `forms.py`), and the unit / vehicle table checks now also run for tables only record edits changed ([data-files.md](data-files.md#changing-records-from-a-mod)).
- 2026-10-20 – **PSE data files + records** (not a plugin interface yet): `pse/records/data.h` reader / canonical writer, `pse_build data canon|check`, records schema `pse/records/ds1/schema.lua` (19 tables, `ds1:<type>/<key>` IDs), [data-files.md](data-files.md).
- 2026-10-20 – **mission replacements** ([reference/mission-replacement.md](reference/mission-replacement.md)).
  `PSE_API_VERSION` stays 0; no interface changed. New mod format: `Mods\<Mod>\<Stem>.lua` with `--@replaces
  <Stem>.dll` in its header runs (in the translated-mission host: `x86rt`, `game.log`, plugin namespaces) instead of
  that mission DLL, whatever the MISSIONS FROM LUA setting; a mod's own DLL still wins, a failing script falls back to
  the DLL. ModKit: readable missions wrap every named script API slot; example `examples/restored_missions` (was training_restored).

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

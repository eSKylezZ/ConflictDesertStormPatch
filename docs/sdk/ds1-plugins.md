# DesertStormFix features as PSE plugins (DS1)

DesertStormFix is the **host plugin** `dsfix` = `<game>\PSE\Plugins\dsfix.dll` + `dsfix.pse` (`host = true`, phase 2
layout: [layout.md](layout.md), [reference/plugins.md](reference/plugins.md#host-plugins)). Every one of its features is
an **internal sub-plugin** `dsfix.<feature>` compiled into `dsfix.dll` and registered through `pse.host`. The plugins
own the bytes their feature patches, so the hooks broker (`dev.pse_hooks()`, the dev console, a crash report) shows
which feature patched what. `dsfix` itself is the core plugin: settings ([ds1-settings.md](ds1-settings.md)) and every
patch made outside a feature's scope.

## How it works

- The table is `kFeatures` in `src/dllmain.cpp` (`pse_host::Feature {name, display, flags, apply}`). `PSEPlugin_Load`
  (the loader calls it before any other plugin, at the exe's entry point) runs `Init(pse)`: `pse_host::Init(pse)`
  (attaches `pse.host`, log + report sinks, the game layer's providers), then `pse_host::RegisterFeatures` registers
  every sub-plugin (before `settings::Load()`); then
  `Init()` runs each row's `Apply*` function in table order inside `pse_host::RunAs(name, fn)` = a
  `patch::OwnerScope` for that plugin. Several rows may share one plugin (`dsfix.dev`: `ApplyDevBackground` +
  `ApplyDevHooks`).
- The launcher's re-apply path (`features::OnSettingsChanged`, launcher.cpp) and the DESERTSTORMFIX menu's direct
  re-applies (fixmenu.cpp: performance overlay) run the same functions in the same scopes.
- Hooks a feature makes later, at run time, use a scope in the feature's own code:
  `patch::OwnerScope owner(pse_host::Find("dsfix.overlay"));` – overlay.cpp `overlay::Install`. In a kit plugin `pse_host::Find` answers the plugin itself
  (lockstep.cpp `Install`, authority.cpp `AuthorityStart` in `dsfix.online`).
- Outside any scope a patch keeps the owner its site / group had before (a live toggle re-applying a group, a feature
  re-hooking after a Write); only a brand-new patch outside a scope belongs to `dsfix`. In the 2026-10-19 live run
  none did (386 records, 45 owners, 0 owned by plain `dsfix`).
- `patch::*` → broker mapping and compatibility rules: [reference/hooks.md](reference/hooks.md#ds1-host-desertstormfixs-corepatch-on-the-broker).

Flags: **SIMULATION** = changes what the game simulates (online peers must match, [online-compat.md](online-compat.md)); **CLIENT_ONLY** =
cosmetic, UI or local input (may differ between peers). When in doubt a feature is SIMULATION. Version = DesertStormFix's
(3.3.0); all are `PSE_PLUGIN_INTERNAL`.

## The plugins

| Plugin | Feature | Flags | Apply functions (Init order) |
| --- | --- | --- | --- |
| `dsfix.restart` | The `+restart <pid>` wait of OPTIONS > VIDEO's APPLY & RESTART (first: the new process waits for the old one before anything touches the game's files); the page itself is `dsfix.graphics` since 2026-10-25 | CLIENT_ONLY | `ApplyRestart` |
| `dsfix.dev` | Developer test hooks | SIMULATION | `ApplyDevBackground`, `ApplyDevHooks` |
| `dsfix.timing` | Frame timing + physics | SIMULATION | `ApplyTiming` |
| `dsfix.framecap` | Frame cap | CLIENT_ONLY | `ApplyFrameCap` |
| `dsfix.launcher` | Launcher settings | CLIENT_ONLY | `ApplyLauncher` |
| `dsfix.ingame_input` | Menus follow the input device | CLIENT_ONLY | `ApplyInGameInput` |
| `dsfix.frontend` | Front-end menus: the main menu's CO-OP / VERSUS / MODS items and the screen router (state -> screen `FUN_00480930`, states 0x3E..) for dsfix.dll's and the plugins' screens; was part of `dsfix.coop` until 2026-10-25 | CLIENT_ONLY | `ApplyFrontEnd` |
| `dsfix.modsmenu` | MODS menu | CLIENT_ONLY | `ApplyModsMenu` |
| `dsfix.fixmenu` | DESERTSTORMFIX menu | CLIENT_ONLY | `ApplyFixMenu` |
| `dsfix.barcolour` | Menu bar / HUD colour | CLIENT_ONLY | `ApplyBarColour` |
| `dsfix.mods` | Mods folder | SIMULATION | `ApplyMods` |
| `dsfix.assetstore` | PSE asset store (setting SHARED ASSET STORE, [ds1-settings.md](ds1-settings.md)): `pse.vfs` provider `ds1.store`, hooks `0x4BE360` / `0x4BD8A0` (priority 100) | SIMULATION | `ApplyAssetStore` |
| `dsfix.memory` | Global Heap + big textures | SIMULATION | `ApplyMemory` |
| `dsfix.weaponicons` | Weapon pictures from mods | CLIENT_ONLY | `ApplyWeaponIcons` |
| `dsfix.diagnostics` | Diagnostics + performance overlay | CLIENT_ONLY | `ApplyDiagnostics` |
| `dsfix.originals` | Original game files: moves `*.dat` / `*.sch` / level script DLLs (Mission / Training / MP_MISSION / FrontEnd / Outro) to `PSE\Originals` ([layout.md](layout.md)); the game finds them through the game layer's redirect (since 2026-10-22, [game-ds1.md](reference/game-ds1.md#events-and-messages) "Game files" - these hooks were here before). Two rows: the folders for DesertStormFix's own readers before `dsfix.mods`, the move after the rest of this table (the mission translation in `dsfix.missions` finds moved DLLs in `PSE\Originals` too) | SIMULATION | `ApplyOriginalsRedirect`, `ApplyOriginalsMove` |
| `dsfix.campaigns` | Custom campaigns | SIMULATION | `ApplyCampaigns` |
| `dsfix.console` | Developer console | SIMULATION | `ApplyConsole` |
| `dsfix.overlay` | Overlay drawing (icons, late draws); serves `pse.ui` ([reference/ui.md](reference/ui.md)). Draws on the game layer's `PSE_EVENT_END_SCENE` (first subscriber; until 2026-10-25 from `dsfix.graphics`' EndScene hook), its glyph hooks are made at run time | CLIENT_ONLY | `overlay::ApplyEndScene` |

Also built in, not a feature: **`pse.game.ds1`** (version 1.0.0, CLIENT_ONLY) – the game interface
([reference/game-ds1.md](reference/game-ds1.md)) - since the phase 2 split served by the game layer
`PSE\Games\pse_ds1.dll` (registered by the loader before `dsfix` loads; its private `pse.game.ds1.host` v0 takes
DesertStormFix's split-screen / online providers and gives back the level-title texts).

## External plugins

DesertStormFix's own plugins that are separate DLLs, built from the SDK only (`pse/include` + `pse_add_plugin`): sources
in `plugins/<name>/`, targets in the top-level `CMakeLists.txt` (`DSFIX_PLUGINS`). Every build makes them
(`build*/pse_plugins/<config>/`); a dev deploy (`DS_GAME_DIR`, not DIST) copies DLL + PDB + manifest into
`<game>\PSE\Plugins` when the plugin relinks; the release package ships them in `PSE\Plugins\` inside the zip (and
`dist-local` installs them into GOG with the rest). Loaded by the loader after `dsfix` (second load round,
[reference/plugins.md](reference/plugins.md)), so they may `require` any `dsfix.<feature>`. Removing the files = the feature is gone, nothing else changes.

| Plugin | Files | Flags | Needs | What |
| --- | --- | --- | --- | --- |
| `dsfix.discord` 1.0.0 | `plugins/discord/discord.cpp`, `dsfix.discord.pse` | CLIENT_ONLY | `pse.game.ds1 >= 1.0` | Discord status (was the internal feature `src/features/discord.cpp` until 2026-10-19) |
| `dsfix.shadows` 1.0.0 | `plugins/shadows/shadows.cpp`, `dsfix.shadows.pse` | CLIENT_ONLY | – | Shadow fix (was the internal feature `src/features/shadows.cpp` until 2026-10-23); setting `FeatShadowFix` |
| `dsfix.audio` 1.0.0 | `plugins/audio/audio.cpp`, `dsfix.audio.pse` | CLIENT_ONLY | – | Audio while aiming (was `src/features/audio.cpp` until 2026-10-23); setting `FeatAimAudio`, byte group AUDIO |
| `dsfix.widescreen` 1.0.0 | `plugins/widescreen/` | CLIENT_ONLY | – | Widescreen launcher modes (byte group WIDESCREEN; until 2026-10-23 in dsfix.dll) |
| `dsfix.textfit` 1.0.0 | `plugins/textfit/` | CLIENT_ONLY | – | Menu text fitting (until 2026-10-23 in dsfix.dll) |
| `dsfix.mouse` 1.0.0 | `plugins/mouse/` | CLIENT_ONLY | – | Mouse acceleration off by default; setting `MouseAcceleration` (own key since 2026-10-23) |
| `dsfix.menus` 1.0.0 | `plugins/menus/` | CLIENT_ONLY | – | Menu fixes (until 2026-10-23 in dsfix.dll) |
| `dsfix.units` 1.0.0 | `plugins/units/` | SIMULATION (fingerprint) | – | Unit tables: empty item slots skipped (until 2026-10-23 in dsfix.dll) |
| `dsfix.limits` 1.0.0 | `plugins/limits/` (+ `dsfix_limits.h`) | SIMULATION (fingerprint) | – | Engine limits (byte group LIMITS, hooks); publishes interface **`dsfix.limits` v1** (`DsfixLimitsV1 {size, version, HeapExtra()}`): `dsfix.memory` adds `HeapExtra()` to the Global Heap when the game creates it (without the plugin: nothing) |
| `dsfix.difficulty` 1.0.0 | `plugins/difficulty/` | SIMULATION (fingerprint) | `dsfix >= 3.3` | VERY HARD / VERY EASY (until 2026-10-24 in dsfix.dll); setting `FeatVeryHard` (restart); its two DIFFICULTY rows' texts through `dsfix.services`' text provider |
| `dsfix.jpfixes` 1.0.0 | `plugins/jpfixes/` | SIMULATION (fingerprint) | `dsfix >= 3.3` | Japanese-release fixes; setting `FeatJpFixes` (live; online the host's value: `HostValue("jpfixes")`, dsfix's hostsync reads the setting from this plugin's key) |
| `dsfix.handling` 1.0.0 | `plugins/handling/` | SIMULATION (fingerprint) | `dsfix >= 3.3` | Mod vehicles' grip / spin damping (`.vehicle`, read through `VehicleHandlingAt`) |
| `dsfix.tooltips` 1.0.0 | `plugins/tooltips/` | CLIENT_ONLY | `dsfix >= 3.3` | Inventory hints; setting `FeatTooltips`; texts / button icons from `dsfix.services`. The split-screen weapon-name fix (`0x454558`) it used to carry is `dsfix.splitscreen`'s now |
| `dsfix.rumble` 1.0.0 | `plugins/rumble/` | CLIENT_ONLY | `dsfix >= 3.3` | Per-weapon rumble rows + trigger feel; setting `FeatRumble`; the feel reaches dsfix.dll's pad I/O through the trigger feel provider |
| `dsfix.bodies` 1.0.0 | `plugins/bodies/` | SIMULATION (fingerprint) | `dsfix >= 3.3` | Soldier bodies from mods (`.skin` `body =`, normal maps) - only with the Mods folder on; multi-part bodies: the squad's uniform on the main skin only (jump at `0x503384`, always). Chains on the skin apply entry with dsfix.dll's `dsfix.skins` |
| `dsfix.followers` 1.0.0 | `plugins/followers/` | SIMULATION (fingerprint) | `dsfix >= 3.3` | Playable followers (the Emir, Franklin) with a pistol + MediKit (`GiveSpawnedWeapon` / `GiveSpawnedMedikit`); setting `FeatFollowers` (live; online the host's: `HostValue("followers")`; dsfix's hostsync and coopscripts read it from this plugin's key, no plugin = off) |
| `dsfix.hud` 1.0.0 | `plugins/hud/` | CLIENT_ONLY | `dsfix >= 3.3` | HUD / menu scaling (byte group HUD + the HUD cave, icon / text / bar fixes, layout offsets; until 2026-10-24 in dsfix.dll). Its settings stay DesertStormFix's (`HudScaling`, `HudScalePercent`: launcher, OPTIONS > VIDEO), read with `DsfixSetting` and re-applied by `SetSettingsListener`. Without the DLL: the game's 800x600-sized HUD |
| `dsfix.weather` 1.0.0 | `plugins/weather/` (+ `dsfix_weather.h`) | SIMULATION (fingerprint) | `dsfix >= 3.3` | Weather, weather sounds, footprints + puddles in one DLL (were dsfix.dll's `dsfix.weather` / `dsfix.footprints` until 2026-10-24). Settings stay DesertStormFix's (`FeatWeather`, `Footprints`, `FeatFootprintsAll`, `FeatFootprintsWeather`); the remembered WEATHER choice is its hidden setting `Weather` (copied once). Publishes interface **`dsfix.weather` v1** (`DsfixWeatherV1 {size, version, Label, Choice, SetChoice, ScriptWeather, ForHost, Kind}`) for dsfix.dll's loadout WEATHER row, Lua `game.weather` and the online token; without the plugin: no WEATHER row, `game.weather` errors, online sends no weather. Shares the call `0x40E356` with `dsfix.online`'s nameplates (broker chain 50 / 100) |
| `dsfix.timeofday` 1.0.0 | `plugins/timeofday/` (+ `dsfix_timeofday.h`) | SIMULATION (fingerprint) | `dsfix >= 3.3` | Time of day + day / night cycle (until 2026-10-24 in dsfix.dll): the script API's time slots ruled by the loadout rows' choice. Its choices are hidden settings `TimeOfDay` / `TimeFlow` (copied once); `FeatTimeOfDay` / `FeatLoadout` stay DesertStormFix's. Publishes **`dsfix.timeofday` v1** (`TimeLabel`, `FlowLabel`, `Time`, `Flow`, `Set`, `Continues`, `Rule`) for dsfix.dll's TIME OF DAY / TIME FLOW rows and the online token; without it: no rows, missions keep their own time |
| `dsfix.voices` 1.0.0 | `plugins/voices/` (+ `dsfix_voices.h`) | SIMULATION (fingerprint) | `dsfix >= 3.3` | Squad voices (SAS / Delta accents, `.skin` voices) + YOUR SOLDIER TALKS (byte group PLAYER_VOICE); until 2026-10-24 in dsfix.dll. Its VOICE choices are hidden settings `Voice<SP|Coop><role>` (copied once); `FeatVoices` / `FeatPlayerVoice` stay DesertStormFix's; the accent banks stay mods.cpp's (`SoundCache`). Publishes **`dsfix.voices` v1** (`Label`, `Choice`, `SetChoice`, `CoopChoice`, `Text`, `HasOwnVoice`) for dsfix.dll's VOICE row and the online token; without it: no VOICE row, everyone his own bank |
| `dsfix.fontsharp` 1.0.0 | `plugins/fontsharp/fontsharp.cpp` | CLIENT_ONLY | `dsfix >= 3.3` | Sharper text (until 2026-10-24 dsfix.dll's `dsfix.fontsharp`): the font sheet's pages swapped for enlarged copies at the glyph texture bind `0x53CC9B` (last subscriber, priority 0). Setting `FeatSharpText` in its own key; reads DesertStormFix's `HudScaling` (`DsfixSetting` + `SetSettingsListener`). The texture reader is shared with dsfix.dll's barcolour.cpp as the header `src/core/texpixels.h`. Without it: the game's soft text |
| `dsfix.developer` 1.0.0 | `plugins/developer/developer.cpp` | SIMULATION (fingerprint) | `dsfix >= 3.3` | CHEATS > DEVELOPER page (front-end state 0x46: debug displays, time speed, complete mission, resurrect) + Pivotal's debug key map (byte group DEBUG_KEYS, call `0x4473B5`); until 2026-10-24 dsfix.dll's `dsfix.developer`. Page through `SetScreenProvider`, texts `0xC00D....` through `SetTextProvider`. Only with DesertStormFix's `FeatCheats` on at start-up. Without it: dsfix.dll's CHEATS row DEVELOPER does nothing |
| `dsfix.cinematics` 1.0.0 | `plugins/cinematics/cinematics.cpp` | SIMULATION (fingerprint) | `dsfix >= 3.3` | Intro / cutscene skip (until 2026-10-24 in dsfix.dll): the movie-cancel queries' flag reads (calls `0x48676E` / `0x4867ED`) + the first-boot / intro-film flags. When to skip is dsfix.dll's (`MissionCutsceneSkip`); `SkipIntro` / `SkipCutscenes` stay DesertStormFix's settings. Without it nothing is skipped, also not the automatic overview after the briefing |
| `dsfix.display` 1.0.0 | `plugins/display/` (+ `dsfix_display.h`) | CLIENT_ONLY | `dsfix >= 3.3` | Display mode: fullscreen / windowed / borderless window (until 2026-10-24 in dsfix.dll). Reads DesertStormFix's `DisplayMode` / `Renderer` (`DsfixSetting`, re-applied through `SetSettingsListener`). Publishes **`dsfix.display` v1** (`PlaceGameWindow(w, h, mode)`) for OPTIONS > VIDEO's live mode change; without it: fullscreen only |
| `dsfix.photo` 1.0.0 | `plugins/photo/` (photomode, photoframes, freecam, cheats, pausemenu + `photo.spec`, `photo.rc`) | SIMULATION (fingerprint) | `dsfix >= 3.3` | Photo mode + its frames, the free camera, the extended CHEATS menu and the pause menu's extra items, one DLL (they call each other throughout; until 2026-10-24 dsfix.dll's `dsfix.cheats` / `dsfix.freecam` / `dsfix.pausemenu`). Calls into dsfix.dll through `dsfix.host`; publishes `dsfix.photo` (cheat flags, texts, free-camera state, photo capture around Present, zoom). The remembered unlock `CheatsUnlocked` moved to its own key (copied once). Without it: no photo mode, the game's own CHEATS screen, no free camera, no extra pause items |
| `dsfix.online` 1.0.0 | `plugins/online/` (net, online, onlinecompat, modsync, lobby, lobbynet, lobbymenu, onlinepause, lockstep, authority, puppetsync, hostsync, viewmode, nameplates, determinism, glue + `online.spec`) | SIMULATION (fingerprint) | `dsfix >= 3.3` | Online co-op as one DLL (until 2026-10-25 dsfix.dll's `dsfix.nameplates` / `dsfix.views` / `dsfix.determinism` / `dsfix.lockstep` / `dsfix.authority` and the connection code): connection + join codes, the online menus and lobby, the plugin-set check and mod sync, owner-mode host authority, the host's settings token, own view + peek, nameplates, the determinism harness (dev). Compiles the loader's pure online-check code (`pse/loader/compat.cpp`); the list of loaded plugins comes from dsfix.dll as text (`dsfix.host` `CompatLocalPluginsText`). Publishes `dsfix.online` (frame calls, lockstep queries, lobby, screens / texts, the console's `net` commands). Settings stay DesertStormFix's. Without it: no online menus or sessions; split screen and single player unchanged |
| `dsfix.missions` 1.0.0 | `plugins/missions/` (scripthost, movie, missionsource, missionrt, apitrace, coopscripts + `missions.spec`) | SIMULATION (fingerprint) | `dsfix >= 3.3` | Mission scripts as one DLL (until 2026-10-25 dsfix.dll's `dsfix.scripthost` / `dsfix.missionrt` / `dsfix.missionsource` / `dsfix.apitrace` / `dsfix.coopscripts`): the Lua host of `.lua` levels and its films, translated mission DLLs (MISSIONS FROM LUA, mod `--@replaces` Lua) with the `x86rt` runtime, the co-op branches of the shipped scripts, the dev API trace. Has its own Lua and the mission translator. The loader's private `pse.lua.host` (host plugins only) is handed on by dsfix.dll (`dsfix.host` `LuaHostTable`), so plugin Lua functions work as before. Its setting `TranslatedMissions` moved to its own key (copied once from `dsfix.missionsource`'s). Publishes `dsfix.missions`. Without it: no `.lua` levels, every mission runs as its DLL |
| `dsfix.loadout` 1.0.0 | `plugins/loadout/` (customise, loadout, gear, vehicles, overview, skinapply, glue + `loadout.spec`) | SIMULATION (fingerprint) | `dsfix >= 3.3` | The mission briefing + loadout screen, kits / unlocks / carried kits, soldier gear, vehicle swaps, the mission overview and mid-level skins as one DLL (until 2026-10-25 dsfix.dll's `dsfix.customise` / `dsfix.loadout` / `dsfix.gear` / `dsfix.vehicles` / `dsfix.overview` / `dsfix.skins`). mods.cpp's gear / pouch-set / vehicle / skin-variant lists come over `dsfix.host` as text rows (`glue.cpp` rebuilds them per mods generation). One co-save callback set writes `KITS`, `GEAR` and `VEHS` ([below](#co-save-chunks)); its manifest `adopts = dsfix.gear, dsfix.vehicles` so saves from before keep theirs. Its remembered choices stay raw values in DesertStormFix's key. Publishes `dsfix.loadout`. Without it: no briefing / loadout screen, the game's own kits and vehicles |
| `dsfix.coop` 1.0.0 | `plugins/coop/` (splitscreen, coop, controls, padlayout, multikbm, controller, hotplug, padio, versus, glue + `coop.spec`, `coop.rc` = the CONTROLLER pictures) | SIMULATION (fingerprint) | `dsfix >= 3.3` | Split screen, the co-op join screen + session + saves, the CONTROLS menu and pad layouts, controllers in game, hot-plug, direct pad I/O (rumble, triggers, light bar), several keyboards / mice and VERSUS as one DLL (until 2026-10-25 dsfix.dll's `dsfix.splitscreen` / `dsfix.coop` / `dsfix.controls` / `dsfix.controller` / `dsfix.hotplug` / `dsfix.multikbm` / `dsfix.versus`). The main menu items and the screen router stay in dsfix.dll (`dsfix.frontend`). Re-applies the controller settings when the launcher saves (services `SetSettingsListener`). Settings stay DesertStormFix's. Publishes `dsfix.coop`. Without it: one player, the game's own pad handling, no CONTROLS / co-op / versus screens |
| `dsfix.graphics` 1.0.0 | `plugins/graphics/` (graphics, videomenu, the renderers d3d8to9 / shader8 / presenter / water + `graphics.spec`; the HDR / water shaders are compiled into it) | CLIENT_ONLY | `dsfix >= 3.3` | The D3D8 device hooks (MSAA, anisotropic filtering, V-Sync, the loadout screen's scene fog, the photo capture around Present), the Direct3D 8 on 9 / D3D9On12 / own DXGI presenter renderers with HDR, bloom and puddle reflections, and OPTIONS > VIDEO (until 2026-10-25 dsfix.dll's `dsfix.graphics` / `dsfix.videomenu`). Re-applies texture filtering when the launcher saves (services `SetSettingsListener`). The frame cap, frame timing, frame recorder and the `+restart` wait stay in dsfix.dll; the overlay draws there on `PSE_EVENT_END_SCENE`. Publishes `dsfix.graphics`. Without it: the system's D3D8 as the game made it (no MSAA / anisotropy / V-Sync, frame cap only), no VIDEO page, puddles as plain decals |

The SIMULATION ones declare a simulation fingerprint (below), so a dev build and the release package still connect
online. They load after `dsfix.dll`'s own features (the loader's second round), still before the game's WinMain.

**dsfix.services v1** (`src/core/dsfix_services.h`, 2026-10-24) is dsfix.dll's interface for the split-out features
that still need something of it - published by `features::RegisterServices` (services.cpp) at the end of dsfix's Load,
so it exists before the second round. `kit::Services()` queries it once (an error line without it); such a plugin's
manifest says `requires = dsfix >= 3.3`. `DsfixServicesV1 {size, version}` then:

| Member | What |
| --- | --- |
| `GameText(hash)` | the game's catalog text (current language) or NULL |
| `ControlIconText(player, action)` | icon character of player 0-3's button for a `default.key` action, NULL = no icon |
| `OnlineMission()` | 1 while an online mission runs |
| `HostValue(name)` | the host's value of a synced setting online (`"jpfixes"`, `"followers"`), else -1 |
| `ModsGeneration()` | changes when the Mods folder is re-read (MODS menu) |
| `VehicleHandlingCount()` / `VehicleHandlingAt(i, &model, &grip, &spin)` | mods' `.vehicle` handling (< 0 = the game's) |
| `SetTextProvider(plugin, fn)` | `fn(hash)` is asked by dsfix's text lookup before the catalog (one per plugin, NULL removes) |
| `SetTriggerFeelProvider(plugin, fn)` | `fn(player)` → `DsfixTriggerFeel` for dsfix's pad I/O (adaptive triggers, full pull) |
| `DsfixSetting(key)` | DesertStormFix's own bool / int setting by registry name (`"FeatLoadout"`), 0 if unknown |
| `ModsFolderEnabled()` / `SkinBody(texture)` / `SkinNormalMap(texture)` / `ModReport(line)` | Mods folder read; the body / normal map a `.skin` gives that texture (`""` = none); a log + mod report line |
| `LoadoutScreenOpen()` | the briefing / loadout screen is up |
| `GiveSpawnedWeapon(soldier, weapon, mags)` / `GiveSpawnedMedikit(soldier)` | a weapon / MediKit made from the level's own scene record, given to the soldier (NULL / 0 = no record) |
| `SetSettingsListener(plugin, fn)` | `fn()` after DesertStormFix's own settings changed in the launcher (OK / Set to Defaults) - re-read `DsfixSetting` |
| `GameFocused()` | the game window is in front (or treated so: run in background / tests) |
| `DrawPuddleReflections(dev8, parts, n, look)` | puddle reflections through dsfix.dll's D3D9 renderers 1-3 (`DsfixPuddlePart`, `DsfixWaterLook`; 0 on Renderer 0) |
| `LinkedFrom()` | the level this one continues from (linked missions), `""` if none |
| `SkinVoice(texture)` / `SpeechLanguage()` | the voice bank a `.skin` gives that texture (`""` = none); the game's speech language (0 UK / 6 US English, -1 another) |
| `SetScreenProvider(plugin, fn)` | `fn(state)` returns the screen object for a front-end state dsfix.dll doesn't serve (its state -> screen hook on `FUN_00480930` asks after its own states), or NULL. One per plugin, NULL removes it; dropped at `PSE_MSG_EXIT`. Check `size` before calling (appended 2026-10-24) |
| `MissionCutsceneSkip()` | 1 = DesertStormFix wants the running mission cutscene skipped: SKIP CUTSCENES (online: the host's), the automatic overview after the briefing, the restart after a vehicle change (appended 2026-10-24) |
| `DevBackground()` / `QuickStartLevel()` | dev background test mode (off-screen, never activated); the level `+level <name>` asked for, or NULL (appended 2026-10-24) |

The providers are plain cross-plugin calls (no crash guard); dsfix forgets them all at `PSE_MSG_EXIT`, before the
loader unloads the plugins. Without `dsfix.rumble` the trigger feel is `DEFAULT` for every weapon; without a text
provider the hashes fall through to the game's catalog.

**`dsfix.overlay` v1** (`src/core/dsfix_overlay.h`, published by dsfix.dll's `features/overlay_iface.cpp` next to
dsfix.services, 2026-10-24) is DesertStormFix's overlay drawing (`features/overlay.h`) as a plain-C table: queued icons /
images / quads / rectangles / lines, texture loaders (`LoadTextureImage` for an encoded image at a given texture size,
`LoadTextureFile`, `LoadTextureBytes`), `CreateTexture` / `UploadTexture` / `ReleaseTexture`, the late draws
(`SetLateDraw`, `AddLateDraw`, `SetTopDraw`, `Flush`, `FillRects`), icon glyphs (`IconChar`, `GlyphsReady`) and the game's
font (`MenuFont`, `TextWidth`, `DrawLabel`). Kit plugins don't use the table directly: the kit's overlay client
(`plugins/kit/overlay_client.cpp`) implements `overlay.h` on it (`LoadTexture(resource)` reads the plugin's own RCDATA),
so a moved feature's drawing code compiles unchanged. Every DLL has its own static C runtime, which is why these
interfaces pass no C++ objects.

**`dsfix.host` and the group interfaces** (2026-10-24; DesertStormFix-internal, not for other plugins - the layout
changes with every build) carry the ordinary `features::` / `net::` / `padio::` calls between dsfix.dll and
DesertStormFix's group plugins. They are generated by `games/ds1/scripts/gen_host_calls.py` from spec files (one
declaration per line, as in its header): `src/core/dsfix_host.spec` = what the plugins call in dsfix.dll (dsfix.dll's
`features/host_iface.cpp` publishes the table; the kit's `plugins/kit/host_client.cpp`, compiled into every kit
plugin, defines those functions as calls through it), `plugins/<group>/<group>.spec` = what dsfix.dll calls in that
group plugin (the plugin publishes it with the generated `Register<Group>Interface()`; dsfix.dll's
`features/plugin_links.cpp` defines the functions as calls through it, answering the spec's `= default` without the
plugin). So the moved files compile unchanged. Calls cross as plain C: bool as int, `std::string` as `const char*`,
a string vector as one string separated by the control character 0x1E, a reference to a plain struct as a pointer; anything else is refused.
Each table has a layout hash; a plugin built against another layout logs one line and its calls answer nothing.
`scripts/run_tests.py` runs `gen_host_calls.py --check`.

**The dsfix kit** (`plugins/kit/`, static library `dsfix_kit`, 2026-10-23) is how a feature leaves `dsfix.dll` without
being rewritten: it compiles DesertStormFix's `src/core/patch.cpp` into the plugin and supplies what that file and
`core/log.h` expect from the host, on the public SDK only – `pse::HooksInterface()` (= the plugin's `pse.hooks`),
`pse::PluginName()`, `pse_host::Core()` (= the plugin itself, so every patch is owned by it) and `dslog::Write` (=
`pse.log`, so the lines land in `DesertStormFix.log` as `[dsfix.<name>] INFO …`). The feature keeps its `patch::` /
`dslog::` code; `settings::Get()` reads become `kit::DeclareBool` / `kit::GetBool` (its own pse.settings key; the old
value is copied once from `dsfix`'s key, marker `ImportedFromDsfix`, as for Discord) with a change callback for live
settings (flags argument: `PSE_SETTING_LIVE` by default, 0 = restart - OPTIONS > DESERTSTORMFIX then shows "ON *" and
asks for a restart after a change, measured against the values at `PSE_MSG_POST_LOAD`), `DevDword` becomes `kit::DevDword`. CMake: `dsfix_add_plugin(<target> <folder> <description>)` builds
`plugins/<folder>/*.cpp` + its manifest and adds it to `DSFIX_PLUGINS` (tree, deploy, release zip). OPTIONS >
DESERTSTORMFIX keeps the rows (`Kind::Plugin`, OFF and fixed when the DLL is missing). Removing the DLL = the game's own
behaviour. Every kit plugin exports `PSEPlugin_SimFingerprint` (kit.cpp: DesertStormFix version, `dsfix::kSimRevision`,
the patch tables, its name and version), so a SIMULATION one is compared online like dsfix.dll's own features, not by
its file hash ([online-compat.md](online-compat.md#external-plugins-declared-simulation-fingerprint)). Exports per
plugin: one `DSFIX_KIT_PLUGIN(name, display, flags, load)` line (kit.h); `kit::OnFrame(fn)` = per-frame work (was a
`Timed(...)` line in launcher.cpp); a function another feature called directly becomes a published interface (example:
`dsfix.limits`), or a `dsfix.services` member when dsfix.dll itself is the other side. Cross-feature calls left in
dsfix.dll are what keeps the rest inside for now.

**dsfix.discord** – uses `pse.log`, `pse.settings`, `pse.messaging` (`PSE_EVENT_FRAME`) and `pse.game.ds1` (`LevelName`,
`LevelKind`, `LevelTitle`, `SplitScreenPlayers`, `ExeKnown`), nothing else of the game or DesertStormFix:

- Per frame (game thread) a snapshot {mode menus / single player / split screen, players, level, title}; front end,
  `intro` (the front end's film scene – the internal version showed it as a mission called "intro") and outro =
  "In the menus"; the title (`LevelTitle`, "Mission 4: Desert Watch") is looked up once per level and logged:
  `[dsfix.discord] INFO Discord: level Mission1.env -> "Mission 1: Rescue"`.
- A sender thread (the old code unchanged): Discord IPC pipe `\\.\pipe\discord-ipc-0..9`, handshake with Discord's own
  app id for the game `1124357225184505886`, `SET_ACTIVITY` (details = title or "In the menus", state = "Single player"
  / "Split screen · n players", start time per level) when the snapshot changes, at most every 4 s; wakes every 2 s,
  retries a missing Discord every 10 s, logs Discord's reply (`Discord: connected`, `Discord: {"cmd":"SET_ACTIVITY"…`).
  `PSEPlugin_Unload` stops it (waits up to 3 s; a thread still in a pipe call pins the DLL instead of freeing it).
- Setting `DiscordPresence` (bool, LIVE | LAUNCHER, default 1) in its own key
  `HKCU\Software\PSE\ds1\Plugins\dsfix.discord`; `OnChange` starts / stops the status. The launcher's Enhancements
  checkbox and OPTIONS > DESERTSTORMFIX > DISCORD STATUS read and write it through `pse.settings` (greyed out / OFF
  when the plugin isn't installed) – these two are kept as they were (its checkbox keeps its place in the column); the
  setting also appears on the plugin's own page under OPTIONS > DESERTSTORMFIX > PLUGINS, like every external plugin's
  settings ([reference/ui.md](reference/ui.md#settings-need-no-ui-code)), and the launcher's generic plugin rows skip
  it (`launcher.cpp` `Placed`). One-time import of the old value: [ds1-settings.md](ds1-settings.md#discordpresence-moved-to-dsfixdiscord).

## Shared hook sites

| Site | Chain (priority, runs first → last) |
| --- | --- |
| `0x53CC9B` (`call 0x544220`, texture bind of the plain image draw `FUN_0053c9e0`) | `dsfix.weaponicons` 200 (a mod weapon's picture; only with FeatMods) → `dsfix.barcolour` 100 (recoloured menu / HUD page) → `dsfix.fontsharp` 0 (enlarged font page; its own DLL since 2026-10-24) → `0x544220`. Constants in `features.h` (`kGlyphBind*`). Each passes the texture it chose to its `next`; the later ones only replace their own pages, so the earlier replacement wins (same result as the old hand-written calls). |

## Co-save chunks

What DesertStormFix's own plugins keep in a save's co-save `<name>.pse` (pse.serialization,
[reference/serialization.md](reference/serialization.md#ds1-when-the-co-save-is-written-and-read)). Encoders / decoders
and the old-sidecar reader: `src/core/pse_saves_format.cpp` (unit tests `pse/tests/test_ds1_saves.cpp`). All version 1,
little endian, `str` = u16 length + bytes (no NUL), counts u32.

| Plugin | Tag | Layout | What |
| --- | --- | --- | --- |
| `dsfix.loadout` | `KITS` | u32 n, n × {i32 role, str weapon id, i32 reserve, i32 magazine, u8 held} | weapons made at run time (loadout screen, carried kits, mod weapons: object id −1, which the game's save drops), handed back after the save restore |
| `dsfix.loadout` | `GEAR` | u32 roles, roles × {i32 role, u32 n, n × {str connector, str model}} (model "" = nothing on it) | the squad's hats / packs / face pieces / pouches per squad role (connector names "BIP01 HAT CONNECTOR" …), put back on the first play frame after the restore |
| `dsfix.loadout` | `VEHS` | u32 n, n × {str original unit, str new unit} | the vehicle swaps the level was built with; the loaded save's level is built with them instead of the remembered choice |

A part writes its chunk only when it has something (no spawned weapons = no `KITS`). Until 2026-10-25 `GEAR` was written by `dsfix.gear` and `VEHS` by `dsfix.vehicles`; `dsfix.loadout` adopts both names (manifest `adopts`, [reference/serialization.md](reference/serialization.md)), so those saves load and their next save holds the chunks under `dsfix.loadout` only. Role = squad slot (unit
+0x1C7: 0 Bradley, 1 Foley, 2 Connors, 3 Jones). Saves from DesertStormFix ≤ 3.3.0-Alpha carry the same data as lines
of `<name>.sav.dsfix` (`role|id|reserve|magazine|held`, `G|role|connector|model` with `-` = nothing,
`V|ORIGINAL|NEW`); they are read into these chunks when the save has no `.pse`.

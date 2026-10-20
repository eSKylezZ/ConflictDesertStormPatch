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
  `patch::OwnerScope owner(pse_host::Find("dsfix.lockstep"));` – lockstep.cpp `Install`, authority.cpp
  `AuthorityStart`, vehicles.cpp `InstallTriLog` (dev), overlay.cpp `overlay::Install`, scripthost.cpp's dormancy hook.
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
| `dsfix.videomenu` | Video options / restart | CLIENT_ONLY | `ApplyRestart` |
| `dsfix.widescreen` | Widescreen resolutions | CLIENT_ONLY | `ApplyWidescreen` |
| `dsfix.dev` | Developer test hooks | SIMULATION | `ApplyDevBackground`, `ApplyDevHooks` |
| `dsfix.hud` | HUD / menu scaling | CLIENT_ONLY | `ApplyHud` |
| `dsfix.controller` | Controllers in game | CLIENT_ONLY | `ApplyController` |
| `dsfix.hotplug` | Controller hot-plug | CLIENT_ONLY | `ApplyPadAccess` |
| `dsfix.timing` | Frame timing + physics | SIMULATION | `ApplyTiming` |
| `dsfix.framecap` | Frame cap | CLIENT_ONLY | `ApplyFrameCap` |
| `dsfix.launcher` | Launcher settings | CLIENT_ONLY | `ApplyLauncher` |
| `dsfix.ingame_input` | Menus follow the input device | CLIENT_ONLY | `ApplyInGameInput` |
| `dsfix.splitscreen` | Split screen | SIMULATION | `ApplySplitScreen` |
| `dsfix.display` | Display mode | CLIENT_ONLY | `ApplyDisplay` |
| `dsfix.cinematics` | Intro / cutscene skip | SIMULATION | `ApplyCinematics` |
| `dsfix.graphics` | MSAA / anisotropy / V-Sync | CLIENT_ONLY | `ApplyGraphics` |
| `dsfix.coop` | Co-op set-up + saves | SIMULATION | `ApplyCoop` |
| `dsfix.versus` | Versus | SIMULATION | `ApplyVersus` |
| `dsfix.cheats` | Cheats menu | SIMULATION | `ApplyCheats` |
| `dsfix.developer` | Developer page + debug keys | SIMULATION | `ApplyDebugKeys` |
| `dsfix.freecam` | Free camera | CLIENT_ONLY | `ApplyFreeCam` |
| `dsfix.modsmenu` | MODS menu | CLIENT_ONLY | `ApplyModsMenu` |
| `dsfix.fixmenu` | DESERTSTORMFIX menu | CLIENT_ONLY | `ApplyFixMenu` |
| `dsfix.pausemenu` | Pause menu items | CLIENT_ONLY | `ApplyPauseMenu` |
| `dsfix.fontsharp` | Sharper text | CLIENT_ONLY | `ApplyFontSharp` |
| `dsfix.textfit` | Menu text fitting | CLIENT_ONLY | `ApplyTextFit` |
| `dsfix.barcolour` | Menu bar / HUD colour | CLIENT_ONLY | `ApplyBarColour` |
| `dsfix.controls` | Controls menu | CLIENT_ONLY | `ApplyControls` |
| `dsfix.mouse` | Mouse acceleration | CLIENT_ONLY | `ApplyMouse` |
| `dsfix.menus` | Menu fixes | CLIENT_ONLY | `ApplyMenus` |
| `dsfix.followers` | Followers | SIMULATION | `ApplyFollowers` |
| `dsfix.difficulty` | VERY HARD / VERY EASY | SIMULATION | `ApplyDifficulty` |
| `dsfix.jpfixes` | Japanese-release fixes | SIMULATION | `ApplyJpFixes` |
| `dsfix.units` | Unit / vehicle tables | SIMULATION | `ApplyUnits` |
| `dsfix.handling` | Vehicle handling | SIMULATION | `ApplyHandling` |
| `dsfix.limits` | Engine limits | SIMULATION | `ApplyLimits` |
| `dsfix.tooltips` | Inventory tooltips | CLIENT_ONLY | `ApplyTooltips` |
| `dsfix.rumble` | Weapon rumble | CLIENT_ONLY | `ApplyRumble` |
| `dsfix.mods` | Mods folder | SIMULATION | `ApplyMods` |
| `dsfix.assetstore` | PSE asset store (setting SHARED ASSET STORE, [ds1-settings.md](ds1-settings.md)): `pse.vfs` provider `ds1.store`, hooks `0x4BE360` / `0x4BD8A0` (priority 100) | SIMULATION | `ApplyAssetStore` |
| `dsfix.memory` | Global Heap + big textures | SIMULATION | `ApplyMemory` |
| `dsfix.weaponicons` | Weapon pictures from mods | CLIENT_ONLY | `ApplyWeaponIcons` |
| `dsfix.bodies` | Soldier bodies from mods | SIMULATION | `ApplyBodies` |
| `dsfix.timeofday` | Time of day | SIMULATION | `ApplyTimeOfDay` |
| `dsfix.footprints` | Footprints + puddles | CLIENT_ONLY | `ApplyFootprints` |
| `dsfix.weather` | Weather | SIMULATION | `ApplyWeather` |
| `dsfix.voices` | Voices | SIMULATION | `ApplyVoices` |
| `dsfix.overview` | Mission overview | SIMULATION | `ApplyOverview` |
| `dsfix.loadout` | Loadout + unlocks | SIMULATION | `ApplyLoadout` |
| `dsfix.customise` | Briefing / loadout screen | SIMULATION | `ApplyCustomise` |
| `dsfix.gear` | Soldier gear | SIMULATION | `ApplyGear` |
| `dsfix.diagnostics` | Diagnostics + performance overlay | CLIENT_ONLY | `ApplyDiagnostics` |
| `dsfix.audio` | Audio while aiming | CLIENT_ONLY | `ApplyAudio` |
| `dsfix.shadows` | Shadow fix | CLIENT_ONLY | `ApplyShadows` |
| `dsfix.scripthost` | Lua mission scripts | SIMULATION | `ApplyScriptHost` |
| `dsfix.missionrt` | Translated mission runtime: Lua native `x86rt` for translated missions | SIMULATION | `ApplyMissionRt` |
| `dsfix.missionsource` | Translated missions: setting MISSIONS FROM LUA (`TranslatedMissions`, [ds1-settings.md](ds1-settings.md)), dev `Dev\MissionSource`; hook at the level's DLL load `0x40C3EB` | SIMULATION | `ApplyMissionSource` |
| `dsfix.campaigns` | Custom campaigns | SIMULATION | `ApplyCampaigns` |
| `dsfix.console` | Developer console | SIMULATION | `ApplyConsole` |
| `dsfix.determinism` | Determinism tests (dev) | SIMULATION | `ApplyDeterminism` |
| `dsfix.lockstep` | Online lockstep | SIMULATION | (none: hooks made at run time) |
| `dsfix.authority` | Online host authority | SIMULATION | (none: hooks made at run time) |
| `dsfix.vehicles` | Vehicle swaps | SIMULATION | (none: hooks made at run time) |
| `dsfix.overlay` | Overlay drawing (icons, late draws); serves `pse.ui` ([reference/ui.md](reference/ui.md)) | CLIENT_ONLY | (none: hooks made at run time) |

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
| `0x53CC9B` (`call 0x544220`, texture bind of the plain image draw `FUN_0053c9e0`) | `dsfix.weaponicons` 200 (a mod weapon's picture; only with FeatMods) → `dsfix.barcolour` 100 (recoloured menu / HUD page) → `dsfix.fontsharp` 0 (enlarged font page) → `0x544220`. Constants in `features.h` (`kGlyphBind*`). Each passes the texture it chose to its `next`; the later ones only replace their own pages, so the earlier replacement wins (same result as the old hand-written calls). |

## Co-save chunks

What DesertStormFix's own plugins keep in a save's co-save `<name>.pse` (pse.serialization,
[reference/serialization.md](reference/serialization.md#ds1-when-the-co-save-is-written-and-read)). Encoders / decoders
and the old-sidecar reader: `src/core/pse_saves_format.cpp` (unit tests `pse/tests/test_ds1_saves.cpp`). All version 1,
little endian, `str` = u16 length + bytes (no NUL), counts u32.

| Plugin | Tag | Layout | What |
| --- | --- | --- | --- |
| `dsfix.loadout` | `KITS` | u32 n, n × {i32 role, str weapon id, i32 reserve, i32 magazine, u8 held} | weapons made at run time (loadout screen, carried kits, mod weapons: object id −1, which the game's save drops), handed back after the save restore |
| `dsfix.gear` | `GEAR` | u32 roles, roles × {i32 role, u32 n, n × {str connector, str model}} (model "" = nothing on it) | the squad's hats / packs / face pieces / pouches per squad role (connector names "BIP01 HAT CONNECTOR" …), put back on the first play frame after the restore |
| `dsfix.vehicles` | `VEHS` | u32 n, n × {str original unit, str new unit} | the vehicle swaps the level was built with; the loaded save's level is built with them instead of the remembered choice |

A plugin writes its chunk only when it has something (no spawned weapons = no `KITS`). Role = squad slot (unit
+0x1C7: 0 Bradley, 1 Foley, 2 Connors, 3 Jones). Saves from DesertStormFix ≤ 3.3.0-Alpha carry the same data as lines
of `<name>.sav.dsfix` (`role|id|reserve|magazine|held`, `G|role|connector|model` with `-` = nothing,
`V|ORIGINAL|NEW`); they are read into these chunks when the save has no `.pse`.

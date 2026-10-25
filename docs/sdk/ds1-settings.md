# DesertStormFix settings (Conflict: Desert Storm)

Where DesertStormFix keeps its settings since 2026-10-19 (3.3.0 development), how the old ones were moved, and every
value it stores. Code: `src/core/settings.cpp` / `.h`. No ini files – everything is changed from the launcher or the
in-game menus.

## Where

`HKCU\Software\PSE\ds1\Plugins\dsfix` – the [pse.settings](reference/settings.md) key of the internal plugin `dsfix`
(DesertStormFix's core, `src/core/pse_host.cpp`). Per Windows user, no registry virtualisation involved.

The Steam and the GOG install share it (as they already shared the old key and still share the game's own
`Device Settings`): both read and write the same values, and whichever runs first does the migration below.

Not moved:

- the game's own launcher settings, `HKLM\SOFTWARE\Pivotal Games\Conflict Desert Storm\Device Settings` (virtualised
  to `HKCU\Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\...`) – the game reads them itself;
  `settings::ReadDword(true, …)` / `WriteDword(true, …)` still go there;
- the dev-build values in `HKCU\Software\DesertStormFix\Dev`.

## Migration from the Enhancements key

Before, the values lived in `HKLM\SOFTWARE\Pivotal Games\Conflict Desert Storm\Enhancements`, which for the
manifest-less 32-bit exe Windows virtualises into
`HKCU\Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\Pivotal Games\Conflict Desert Storm\Enhancements`.

At the first start of a build with the new key (`settings::Load`, right after `pse_host::Init` in `dllmain.cpp`), when
the new key has no `MigratedFromEnhancements` value, `pse::MigrateRegistryValues` (`pse/loader/settings_migrate.cpp`)
copies **every** value of the old key – all names, all registry types (DWORD, string, binary) – into the new key,
except names the new key already has. Sources, first one that has a name wins:

1. `HKLM\SOFTWARE\Pivotal Games\Conflict Desert Storm\Enhancements`, read the way the game always did (inside the game
   that is the virtualised view: VirtualStore copy over real HKLM),
2. the VirtualStore path above directly (in case virtualisation is off),
3. `HKCU\SOFTWARE\Pivotal Games\Conflict Desert Storm\Enhancements` (where the loadout / vehicle choices went when
   HKLM refused the write).

Then `MigratedFromEnhancements` = 1 (REG_DWORD) is written and the copy never runs again. The old key is never changed
or deleted – an older DesertStormFix build still finds its values there (but changes made since aren't in it). A
failed copy leaves the marker unset, so it is retried at the next start. The log says what happened:
`Settings: moved to HKCU\Software\PSE\ds1\Plugins\dsfix - n value(s) copied from m old key(s), k already there: …`.

Test scripts that preset values (`tools/bg.py`) make the same copy first (`ensure_settings_migrated`), so a test
that writes and then removes a value before the game's first start can't lose the user's value.

## Declared values (pse.settings)

Every field of `settings::Values` is declared through `pse.settings` (`Declare` once in `Load`, `GetInt` to read,
`SetInt` in `Save` – only changed values are written). All REG_DWORD. Flags: L = `PSE_SETTING_LIVE`, N =
`PSE_SETTING_NEXT_LEVEL`, R = restart (no flag), W = `PSE_SETTING_LAUNCHER` (also in the launcher), H =
`PSE_SETTING_HIDDEN`. Out-of-range stored values are clamped as before.

| Value | Type | Range / choices | Default | Flags | Menu text |
| --- | --- | --- | --- | --- | --- |
| HudScaling | bool | | 1 | R W | HUD & MENU SCALING |
| HudScalePercent | int | 25–400 | 100 | R W | HUD SIZE |
| Controller | bool | | 1 | R W | CONTROLLER SUPPORT |
| PadDeadzone | int | 0–90 (%) | 40 | L W | STICK DEADZONE |
| FpsCap | int | 0–1000; 0 or > 240 reads as 240 | 240 | L W | FRAME RATE LIMIT |
| FpsCapToRefresh | bool | | 1 | L W | LIMIT TO MONITOR REFRESH |
| SplitScreenLayout | choice | TOP / BOTTOM, SIDE BY SIDE | 0 | N W | SPLIT SCREEN LAYOUT |
| DisplayMode | choice | FULLSCREEN, WINDOWED, BORDERLESS | 0 | L W (APPLY NOW) | DISPLAY MODE |
| SkipIntro | bool | | 0 | R W | SKIP INTRO |
| SkipCutscenes | bool | | 0 | L W | SKIP CUTSCENES |
| Antialiasing | int | 0–16 (MSAA samples) | 0 | L W (APPLY NOW) | ANTI-ALIASING |
| Anisotropy | int | 0–16 | 16 | L W | TEXTURE FILTERING |
| VSync | bool | | 1 | L W (APPLY NOW) | V-SYNC |
| PerfOverlay | bool | | 0 | L W | PERFORMANCE OVERLAY |
| SkipLauncher | bool | | 0 | L W | LAUNCHER AT START |
| Renderer | choice | D3D8, D3D9, D3D9On12, D3D12 + flip (release builds use at most 2) | 0 | R W | RENDERER |
| Hdr | bool | | 0 | L | HDR |
| HdrPaperWhite | int | 80–500 (nits) | 200 | L | HDR PAPER WHITE |
| HdrPeak | int | 0–10000 (nits, 0 = display's) | 0 | L | HDR PEAK BRIGHTNESS |
| HdrHighlights | int | 0–100 (%) | 100 | L | HDR HIGHLIGHTS |
| Bloom | choice | OFF, SUBTLE, STRONG | 0 | L | BLOOM |
| Fov1 … Fov4 | int | 50–110 (degrees) | 70 | L | FIELD OF VIEW - PLAYER n |
| MenuBarCustom | bool | | 0 | L | MENU BAR COLOUR |
| MenuBarColour | colour | 0xRRGGBB | 0x1D3909 | L H | (colour picker) |
| HudColourCustom | bool | | 0 | L | HUD COLOUR |
| HudColour | colour | 0xRRGGBB | 0x34571B | L H | (colour picker) |
| Footprints | choice | OFF, GAME (12 S), LONG (2 MIN), PERMANENT | 1 | L | FOOTPRINTS |

Optional features (`g_toggles`, OPTIONS > DESERTSTORMFIX, group "OPTIONAL FEATURES"), all bool, L unless marked R:
FeatLoadout (1), FeatUniformSelect (0), FeatTimeOfDay (1), FeatFreeGear (0), FeatLoadoutSpin (1),
FeatLoadoutLookAhead (1), FeatVoices (1), FeatPlayerVoice (0), FeatNameplates (1), FeatStreamerMode (1: join / reply codes and IP addresses hidden on the online screens), FeatMultiKbm (1), FeatCoopScripts (1), FeatMods (1, R),
FeatCustomMissions (1), FeatVersus (1, R), FeatCheats (1, R), FeatWeather (1), FeatFootprintsAll (0),
FeatFootprintsWeather (1), FeatRunInBackground (0), FeatDevConsole (0).

## FeatShadowFix / FeatAimAudio / MouseAcceleration (and four more) moved to their plugins

Since 2026-10-23 the shadow fix, the aiming audio fix and the mouse acceleration fix are their own plugin DLLs
([ds1-plugins.md](ds1-plugins.md#external-plugins)), and their settings are those plugins' own:
`HKCU\Software\PSE\ds1\Plugins\dsfix.shadows` value `FeatShadowFix` (menu SHADOW FIX, group GRAPHICS, default 1),
`...\dsfix.audio` value `FeatAimAudio` (AIMING AUDIO FIX, group AUDIO, default 1) and `...\dsfix.mouse` value
`MouseAcceleration` (MOUSE ACCELERATION, group CONTROLS, default 0; the game's MOUSE OPTIONS > ACCELERATION writes it) –
bool, REG_DWORD, LIVE. OPTIONS > DESERTSTORMFIX keeps both rows (they read / write through `pse.settings`; OFF and
fixed when the DLL is missing); each also has a page under PLUGINS. No longer `settings::Values` fields. One-time
import, done by the dsfix kit before the plugin declares the setting: a value missing from the plugin's key is copied
from plugin `dsfix`'s key while the plugin's `ImportedFromDsfix` marker is absent; the marker is written at
`PSE_MSG_POST_LOAD`. Log: `dsfix.shadows: FeatShadowFix 0 copied from plugin dsfix's settings`.

The same since 2026-10-24 for six more (bool, REG_DWORD, default 1, imported the same way):

| Plugin key `...\Plugins\<plugin>` | Value | Menu row (group) | Flags |
| --- | --- | --- | --- |
| `dsfix.difficulty` | `FeatVeryHard` | VERY EASY & VERY HARD (GAMEPLAY) | restart (not LIVE): the DIFFICULTY menu is built once per front-end load |
| `dsfix.jpfixes` | `FeatJpFixes` | JAPANESE RELEASE FIXES (GAMEPLAY) | LIVE; online the host's value (dsfix.dll's hostsync reads it from this key for the `.J` field; no plugin = off) |
| `dsfix.rumble` | `FeatRumble` | ENHANCED RUMBLE (CONTROLS) | LIVE (the weapon rows are rewritten at once) |
| `dsfix.tooltips` | `FeatTooltips` | INVENTORY HINTS (HUD) | LIVE |
| `dsfix.followers` | `FeatFollowers` | PLAYABLE FOLLOWERS (GAMEPLAY) | LIVE; online the host's value (hostsync `.F` and coopscripts' player cap read it from this key; no plugin = off) |
| `dsfix.fontsharp` | `FeatSharpText` | SHARPER TEXT (GRAPHICS) | LIVE; only with DesertStormFix's `HudScaling` on (the plugin re-reads it when the launcher saves) |

A restart row counts as changed against the value it had at `PSE_MSG_POST_LOAD` (the plugins exist then), not the one
OPTIONS > DESERTSTORMFIX saw at its own Apply.

## DiscordPresence moved to dsfix.discord

Since 2026-10-19 the Discord status is the external plugin `dsfix.discord` ([ds1-plugins.md](ds1-plugins.md#external-plugins)),
and its setting is that plugin's own: `HKCU\Software\PSE\ds1\Plugins\dsfix.discord`, value `DiscordPresence` (bool,
REG_DWORD, default 1, LIVE | LAUNCHER, menu text DISCORD STATUS, group SYSTEM). It is no longer a `settings::Values`
field; the launcher's "Show the game in my Discord status" checkbox and OPTIONS > DESERTSTORMFIX > DISCORD STATUS read
and write it with `pse.settings` `GetInt` / `SetInt` on that plugin (the plugin's `OnChange` turns the status on / off
at once). Without the plugin the checkbox is greyed out and the row stays OFF.

One-time import, done by the plugin itself in `PSEPlugin_Load` before it declares the setting (plugins can't call the
host's `pse::MigrateRegistryValues`; plain registry calls on the documented key layout): when its key has neither
`ImportedFromDsfix` nor `DiscordPresence`, it copies `DiscordPresence` from plugin `dsfix`'s key
(`HKCU\Software\PSE\ds1\Plugins\dsfix`, where the Enhancements migration put it), then writes `ImportedFromDsfix` = 1
(REG_DWORD; not written if the copy failed, so it is retried). Log: `Discord: DiscordPresence 0 copied from
HKCU\Software\PSE\ds1\Plugins\dsfix`. The old value stays in `dsfix`'s key, unused (`settings::Save` no longer writes
it). A user who never had the value gets the default (on).

## Shared asset store: dsfix.assetstore

The asset store (one shared copy of every archive file and level sound cache) is a DesertStormFix sub-plugin with settings of its own (declared
by `src/features/assetstore.cpp` at start-up), so they live in that plugin's key
`HKCU\Software\PSE\ds1\Plugins\dsfix.assetstore` and appear in **OPTIONS > DESERTSTORMFIX > PLUGINS > Shared asset
store** (the PLUGINS list shows DesertStormFix sub-plugins that declare settings, next to the external plugins):

| Value | Type | Default | Flags | Menu text | Meaning |
| --- | --- | --- | --- | --- | --- |
| SharedAssetStore | bool | **0** | R | SHARED ASSET STORE | 1 = build / check the store at start-up (progress window on the first start) and serve every archive file and level sound cache from it; mods.cpp's Borrow* copies are switched off |
| RebuildCache | bool | 0 | R | REBUILD CACHE AT NEXT START | 1 = build the store again at the next start (with the window), then set back to 0 |

The default stays OFF until the user decides to turn it on for everyone. Dev builds: `HKCU\Software\DesertStormFix\Dev\PseStore` = 1 / 2 overrides `SharedAssetStore` (1 = store +
global fallback, 2 = exact copies only), `PseRebuild` ≥ 2 forces a rebuild, `PseNoBorrow` = 1 switches the Borrow*
copies off even with `PseStore` = 2 (the control run). Release builds read only the two values above.

## Translated missions: dsfix.missions

Declared by `plugins/missions/missionsource.cpp` at start-up; key `HKCU\Software\PSE\ds1\Plugins\dsfix.missions`
(until 2026-10-25 `dsfix.missionsource`'s key - copied once), menu **OPTIONS > DESERTSTORMFIX > PLUGINS > Mission
scripts**:

| Value | Type | Default | Flags | Menu text | Meaning |
| --- | --- | --- | --- | --- | --- |
| TranslatedMissions | bool | **0** | R | MISSIONS FROM LUA | 1 = a shipped `Mission*.dll` / `Training*.dll` on the verified list runs as its stage-1 Lua translation; others, mod DLLs and failures keep the DLL |

Default OFF until a human play-test. Dev builds: `HKCU\Software\DesertStormFix\Dev\MissionSource` = `lua` / `dll`
overrides it; `MissionUnverified` = 1 also runs DLLs that aren't on the verified list; `PseRebuild` (missing = 1)
regenerates every mission at each start. With the setting on, every `Mission*.dll` / `Training*.dll` is translated at
start-up (before the launcher, ~1 s the first time); release builds then only regenerate a mission whose stamp (DLL
SHA-256 + translator build) changed.

## Other values in the same key (raw registry)

Values whose names are built at run time (per player / role / level) or that are binary can't be declared, so
`settings::ReadDword(false, …)` / `WriteDword(false, …)`, `ReadString` / `WriteString`, `ReadBinary` /
`WriteBinary` / `DeleteValue` read and write them directly in `HKCU\<settings::Key()>`. pse.settings ignores names it
wasn't given, so both share the key.

| Value | Type | Written by | Content |
| --- | --- | --- | --- |
| MigratedFromEnhancements | DWORD | settings.cpp | 1 = the old key was copied |
| StartPending | DWORD | launcher.cpp | 1 from launcher OK until 300 frames ran (start failed → launcher shown) |
| CheatsUnlocked | DWORD | **moved 2026-10-24** to plugin `dsfix.photo`'s own key (hidden int, copied once from here) | 1 = CHEATS menu unlocked |
| CoopProgress | DWORD | coop.cpp | bit per finished co-op mission |
| KeyProfile1 … 4 | binary 960 B | controls.cpp | int32[80][3] keyboard / mouse bindings (pad entries −1) |
| PadPreset1 … 4 | DWORD | padlayout.cpp | 0 Default, 1 Classic, 2 Southpaw, 3 Custom |
| PadLayout1 … 4 | binary 960 B | padlayout.cpp | int32[80][3] pad bindings, only while the preset is Custom |
| PadVibration1 … 4, PadTriggers1 … 4 | DWORD | padlayout.cpp | 0 / 1 (default 1) |
| PadGyro1 … 4 | DWORD | padlayout.cpp | 0 off, 1 aiming, 2 always |
| PadLook1 … 4 | DWORD | padlayout.cpp | look sensitivity 25–200 % |
| Loadout\<SP\|Coop\>\<role\> | REG_SZ | customise.cpp | remembered loadout "uniform;face;main;sidearm;launcher;grenades;gear…" |
| Carry\<SP\|Coop\>\<role\>, Carry\<SP\|Coop\>From | REG_SZ | customise.cpp | kit carried into the next linked mission + the level it came from |
| UnlockedWeaponsSP, UnlockedWeaponsCoop | REG_SZ | loadout.cpp | "id;id;…" |
| TimeOfDay, TimeFlow | DWORD | **moved 2026-10-24** to plugin `dsfix.timeofday`'s own key (hidden int settings, copied once from here) | 0–4 each |
| Voice\<SP\|Coop\>\<role\> | DWORD | **moved 2026-10-24** to plugin `dsfix.voices`'s own key (hidden int settings, copied once from here) | 0 auto, 1 SAS, 2 Delta, 3 own |
| Weather | DWORD | **moved 2026-10-24** to plugin `dsfix.weather`'s own key (hidden int setting, copied once from here) | 0–12 (MISSION DEFAULT … RANDOM) |
| Vehicles\<SP\|Coop\>\<Level\> | REG_SZ | vehicles.cpp | "ORIGINAL=NEW;…" vehicle swaps |

## Testing

- Unit (tier 1): `pse/tests/test_settings_migrate.cpp` – copy of every type, marker, run-once, existing values win,
  source order, no old key, bad arguments, migrated values read by `Declare`.
- Live (tier 3): `tests/live/test_settings_live.py` – the game's key is the new one, marker present, every stored
  value is what the game uses, a change (dev `dev.set_setting`) is written to the new key and not to the old one
  (restored afterwards).
- Live (tier 3): `tests/live/test_discord_plugin.py` – `dsfix.discord` is an external plugin, its key holds
  `ImportedFromDsfix` (+ `DiscordPresence` once changed), `dev.setting("DiscordPresence")` is gone from the core, a
  change with `dev.set_plugin_setting` is stored and reaches the plugin's `OnChange` (restored afterwards).

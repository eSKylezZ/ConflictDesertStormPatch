# Changelog

## 3.0.0 – 2026-10-10

### Added

**Mission briefing**

- A MISSION BRIEFING screen replaces the game's briefing popup after each mission's intro, with tabs: BRIEFING (the
  map and the mission's story), MISSION (the level's map with the objectives marked and numbered, the squad's
  position and each objective's description), OVERVIEW and SQUAD (uniform, face, voice and kit per soldier); Tab or
  L2 / R2 switches tabs, L1 / R1 (Q / E) picks the soldier
- TIME OF DAY for any mission (the mission's own, dawn, day, dusk, night) and TIME FLOW (the mission's own, or a
  running day / night cycle at 1 game hour per 10, 5, 2 or 1 real minutes); the lighting, sun / moon, shadows and the
  enemies' sight follow the hour
- VOICE per soldier: the uniform's actors (AUTO), SAS (British) or Delta (American) whatever he wears, or a uniform
  mod's own voice
- The night-vision objective fly-over can be replayed from the OVERVIEW tab as often as you like

**Cheats and developer tools**

- The game's hidden CHEATS menu (OPTIONS > CHEATS) can be unlocked again – with the keyboard on the main menu or a
  button code on any PlayStation or Xbox controller – and stays unlocked: trooper level, enemy level, invulnerable,
  infinite health (new), infinite ammo, enemy AI on / off and the mission list
- Saves made with cheats on are named "Cheats …" and keep their cheats when loaded
- Free camera (with cheats unlocked): F6 or L3 + R3 in a mission, flown with the keyboard or a controller
- DEVELOPER page (CHEATS > DEVELOPER): the original developers' debug views – AI route markers and walk map, AI
  senses, health bars, engine statistics, wireframe – plus game speed 0.25–4x, complete mission and resurrect all

**Custom missions and campaigns**

- Custom missions written in Lua – made with the ModKit's world editor or by hand – run without any other tools:
  objectives, rules, timers, patrols, cutscene cameras, radio lines, title sequences with their own music, the
  mission briefing and save games
- Brand-new levels made in the world editor: their own ground, objects, patrol routes, trigger regions and AI walk
  map
- Custom campaigns: a mod's `campaign.txt` lists its missions, played from SINGLE PLAYER > CUSTOM CAMPAIGNS, with
  Continue going on to the next mission
- `DesertStorm.exe +level <mission>.lua` starts a custom mission straight away
- Edited levels get their AI walk map and visibility data rebuilt automatically, so moved or new objects block the AI
  and show up correctly
- Level mods from the editor ship without any of the game's files: the parts they reuse are built from your own
  installation when the game starts
- Mission scripts can keep enemy soldiers active while the player can't see them (the game puts unseen enemies to
  sleep, so a patrol route otherwise waits until the player comes near)

**Followers**

- The Kuwaiti Emir (Mission 2) and the missile expert Franklin (Missions 11A / 11B) can be taken over like a squad
  member; each carries a pistol and a MediKit

**Mods**

- Uniforms: their own voice (`voice`, `voice from`, `voice line N`, `voice folder` – your own WAV files for any of
  the 47 lines) and their own normal map (`normal map`), with or without their own body
- Cutscene and objective voice-over: a mod's `Sound\SAS\`, `Sound\Delta\` or `Sound\` file with the game's name
  (`CS_M01_S01.psf`, `OBJ_M1_1A.psf` …) replaces that scene's audio
- Whole sound sets per level: a mod `.sch` named like a level's sound file replaces the matching sounds
- Tables can now change or delete the game's rows (`<TABLE>.replace.txt`); a `.weapon` based on itself changes that
  game weapon in place
- `.strings` files change any text of the game, also per mission
- Files that exist only in a mod are found everywhere the game checks for a file (e.g. vehicle tracks and wheels)

### Changed

- The campaign goes from DIFFICULTY straight to the mission: no SELECT UNIFORM screen – uniforms (and voices) are
  picked per soldier on the briefing screen. Everyone starts with the SAS skill sets and UK rank names
- The night-vision objective fly-over no longer plays by itself after the briefing (see OVERVIEW)
- Linked missions (5A, 12A, 12B) open the briefing too: map, objectives and overview as usual, the carried kit shown
  without changes, and a chosen time of day continues from the previous part
- Co-op: DIFFICULTY opens the co-op mission list, whose Back returns to DIFFICULTY
- The cheats menu no longer switches on the developer views that came with it (pad codes in key lists and the like)
- "OK to overwrite this file?" now starts on YES
- Settings are kept even on PCs where the game can't write to its usual registry place

### Fixed

- Vehicles and loose objects were thrown around at high frame rates (Mission 1: the fuel truck by the start flew off
  and landed facing backwards)
- Saving over an existing save could pick a different save than the one selected when the mouse pointer rested over
  the list
- Weapons chosen on the briefing screen, carried from a linked mission or from mods were missing after loading a save
- A uniform chosen on the briefing screen was drawn black (with a white portrait) after loading a save
- Mods: a body from a mod was drawn without a normal map (flat folds and pockets)
- An "active lights" message box could interrupt loading a level; the game's information messages now go to the mod
  report instead

### Known issues

- MODS menu: after applying mod changes twice, starting Basic Training crashed once – restart the game after changing
  mods if that happens
- Mission 11B: pressing Space within the first seconds of the level crashed the game twice in testing
- 4-player co-op, split-screen VERSUS and controller hot-plugging still need full play-throughs with real controllers

## 2.0.0 – 2026-10-02

### Added

**Squad loadout**

- A SQUAD LOADOUT screen after each mission's intro: every soldier turns in front of a plain background and can get
  a different uniform, face, main weapon, sidearm, launcher and grenades
- Four factions on the UNIFORM row: SAS, US Delta, Russian and Iraqi (the old multiplayer uniforms); changing
  faction hands out that faction's standard weapons
- Weapons unlock as the squad finds them – starting kits and anything picked up, enemy weapons included; single
  player and co-op keep separate lists
- Choices are remembered for the next mission; linked missions (4 → 5A, 11B → 12A → 12B) keep the kit the squad
  carried at the end
- Co-op: players take turns, each editing only the soldiers they control with their own controller
- Swapping a launcher keeps its rockets, the squad always keeps at least one launcher when the mission gives one,
  and story items (airstrike designators, laser designator, C4 detonator) are never swapped

**VERSUS (local split screen)**

- The main menu's MULTIPLAYER is now VERSUS: LEGACY ONLINE (the game's original network screens) and SPLIT SCREEN
- Split-screen versus for 2–4 players on the game's six multiplayer maps: Team Deathmatch or Deathmatch, time limit,
  score limit, respawn and friendly fire set on a MATCH SETUP screen
- Each player picks a team and a class (rifleman, heavy weapons, sniper, engineer) on the join screen; in
  Deathmatch every player gets a random faction
- Respawning, a match clock between the views and a scoreboard sized for the screen, with Rematch / Leave

**Split screen**

- Views narrower than 4:3 (side by side) keep a 4:3 field of view instead of a narrow slit
- Tutorial and tip texts show the buttons of every device that joined (keyboard, PlayStation and Xbox icons)

**Mods**

- A Mods folder in the game folder: new uniforms and recruits (`.skin`), new weapons (`.weapon`) as readable text
  files – stats, silencers, sounds from other guns or your own WAV files, textures, HUD / inventory pictures, models
  and animations made in Blender, soldier bodies
- Mods add to the game's tables instead of replacing them, so several mods work together
- A uniform or recruit can bring its own body mesh, worn only with that skin
- A file with a game file's name replaces it in every level (textures, models, animations)
- Main menu MODS: switch each mod on or off and apply the changes without restarting
- Main menu shows the patch version and how many mods are installed and active
- Every start writes `Mods\DesertStormFix-report.txt` with what each mod added and any problems
- Tools, a modding guide and example mods: ConflictDesertStormModKit

**Smoothness**

- V-Sync in fullscreen (launcher setting, on by default): every frame is shown for the same time, even on drivers
  that ignore the game's request; never above 240 fps
- Exact frame timing: the game no longer moves in whole milliseconds, which caused a slight stutter
- Performance overlay (launcher setting, F11 in game): frame rate, frame times, 1 % lows and a frame-time graph
- Frame recorder (Ctrl+F11): writes every frame's timing to a CSV file to help track down stutter reports

**Controllers**

- Plug controllers in and out while the game runs; a returning controller gets its player back
- "Controller disconnected": a mission pauses and names the player whose controller was lost
- Options → CONTROLS → CONTROLLER: look sensitivity per player, stick deadzone, gyro aiming (DualSense /
  DualShock 4: off, while aiming, always)
- DualSense light bar shows the soldier's health in missions
- Loadout screen prompts with button icons; pause / objectives prompts follow a remapped PAUSE / OBJECTIVES button
- CONTROLLER screen: Left / Right stay on the row of settings

### Fixed

- Soldier and object shadows no longer stretch through floors and walls below them
- A hitch every few seconds while no PlayStation controller was connected
- A few HUD offsets were placed too wide on 16:9 screens (mainly the old network lobby screens)
- The CONTROLLER screen's settings ran off the panel at 4:3
- A mod weapon based on the M16A2 stopped the game at start-up with an error box (now a plain assault rifle)
- A `.weapon` file's `magazines` setting is used on the loadout screen

### Known issues

- VERSUS with real controllers, 4-player co-op and the loadout screen after loading a save still need full
  play-throughs

## 1.0.0 – 2026-09-26

Initial release for the Steam and GOG versions of Conflict: Desert Storm (PC). One `dinput8.dll`, placed next to
`DesertStorm.exe`; the game files are not modified.

### Added

**Display**

- Widescreen resolutions in the launcher (16:9, 16:10, 21:9 – whatever the monitor supports)
- HUD, text and menus scaled to the resolution, with an adjustable HUD size
- Sharper text: the game's fonts are redrawn at the screen resolution instead of being stretched
- HUD laid out for wide screens: the weapon display mirrors the soldier panel, menu prompts sit at both screen edges
- Fullscreen, windowed and borderless window modes
- Anti-aliasing (2x / 4x / 8x MSAA) and anisotropic filtering (up to 16x)

**Frame rate**

- Frame rate limit, by default the monitor's refresh rate (max 240 fps); "Unlimited" stops at 500 fps

**Split-screen co-op (2–4 players)**

- CO-OP entry in the main menu with an Xbox-style join screen: each player joins with their own controller or
  keyboard and mouse, and every device only controls its own player
- Co-operative campaign menu: start the campaign or load a co-op save
- Co-op campaign progress: finishing a mission unlocks the next, finished missions can be replayed; 3–4 players start
  at Mission 3, as on the Xbox
- Co-op saves named with the player count ("Co-op 2P Rescue 1"); single-player and co-op saves are listed separately
- Top/bottom or side-by-side layout with divider lines, and a HUD, soldier panels, inventory and give menu for each
  player in their own view
- Every player can open the pause menu with their own controller

**Controllers**

- Xbox controllers (XInput) and DualShock 4 / DualSense in game – buttons, D-pad, both sticks and triggers, over USB
  and Bluetooth
- Rumble per weapon on Xbox, DualShock 4 and DualSense; nothing for throwing grenades or placing C4 and mines
- Xbox trigger rumble on the fire trigger, sized to the weapon
- DualSense adaptive triggers matched to the weapon in hand, including mounted guns and vehicle weapons
- Full pull to fire: a shot only fires once the trigger is pulled through
- DualSense light bar and player LEDs in each player's colour; touchpad click opens the objectives
- Button icons in menu prompts, tutorial hints and actions (Mount, Unlock, Open Door …) for the controller in use
  (PlayStation, Xbox or keyboard)
- Inventory hints: use a MediKit on yourself, change fire mode or toggle night vision without equipping the item
- In-game menus follow the device in use (mouse cursor hidden while a controller is used)
- Launcher and its settings usable with a controller, with button hints

**Controls menu (Options > CONTROLS)**

- KEYBOARD & MOUSE: the game's key assignment and mouse options, plus four saved key profiles (a keyboard player in
  co-op can pick one on the join screen)
- CONTROLLER: a layout for each player on a PS5 or Xbox Series controller picture, with presets DEFAULT (fire on
  R2 / RT), CLASSIC, SOUTHPAW and CUSTOM (rebind any button); vibration and adaptive triggers can be turned off per
  player

**Quality of life**

- Skip the intro (publisher logos and opening film)
- Skip mission cutscenes automatically (optional; the briefing is still shown)
- Discord status with the current mission, single player or split screen, and elapsed time (can be turned off)
- Mouse acceleration off by default (can still be turned on in Mouse Options)
- New settings in the launcher (Settings > Enhancements column): frame rate limit, display mode, intro and
  cutscene skipping, anti-aliasing, texture filtering, HUD scaling and size, controller support and stick deadzone,
  Discord status, split-screen layout

### Fixed (original game)

- The game ran too fast at high frame rates ("turbo" mode) – the game clock lost precision the longer the PC was on
- Soldiers stayed stuck in the helicopter in the opening film at high frame rates
- Tutorial hints only named keyboard keys, even when playing with a controller
- Only the first connected controller worked in the menus
- Gunfire and nearby sounds got louder when aiming or zooming (scopes, binoculars, mounted guns)
- The launcher's keyboard Tab/arrow order didn't follow its layout
- Mouse acceleration was always switched on

### Known issues

- Faint shadows can show through some walls, as in the original game (only soldiers and objects cast shadows; walls
  don't block the light). A proper fix is planned for version 2
- 4-player co-op with real controllers has not had a full play-through yet
- A few HUD elements are placed slightly wide on 16:9 screens
- Windowed and borderless modes are untested on multi-monitor setups

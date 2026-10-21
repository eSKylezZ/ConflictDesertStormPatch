DesertStormFix 3.2 - enhancement patch for Conflict: Desert Storm (PC)
========================================================================

Works with the Steam and GOG versions.

Features
- Widescreen resolutions in the launcher
- Display mode: fullscreen, windowed or borderless window
- Skip the intro (publisher logos and opening film) and, optionally, the
  mission cutscenes
- Anti-aliasing (2x / 4x / 8x MSAA) and anisotropic texture filtering
  (up to 16x), which the original settings don't offer
- HUD, text and menus scaled to your resolution, with sharper text; every
  text bar and box (item names, pick-ups, warnings, objectives, text entry)
  fits the larger text
- DualShock 4 / DualSense and Xbox controller support (buttons, D-pad,
  both sticks and triggers); the menus work with whichever pad you pick up
- Rumble per weapon: a short kick sized to the gun, nothing for throwing
  grenades or placing C4 and mines; explosions, hits and damage still rumble
- DualSense: adaptive triggers that match the weapon in your hands (light for
  pistols and SMGs, a firm break for rifles, heavy for sniper rifles and
  shotguns, steady for machine guns, very heavy for launchers, free for
  grenades, C4 and the knife; mounted and vehicle guns have their own), light
  resistance for aim mode, light bar and player lights show which player the
  pad belongs to, touchpad click opens the objectives
- Full pull to fire: a shot only fires once the trigger is pulled through
  (DualSense past its resistance, Xbox and DualShock 4 at the same point)
- Xbox One / Series controllers: trigger rumble on the fire trigger, sized to
  the weapon
- Options -> CONTROLS: a controller screen that shows what every button does
  on a PlayStation or Xbox controller, layouts per player (presets or your
  own), and saved keyboard & mouse profiles
- In-game menus follow the device you use: with a controller the mouse
  cursor is hidden and prompts, tutorial hints and actions (Mount, Unlock,
  Open Door ...) show the button icons of that controller or the keys
- Correct game speed at high frame rates, frame rate capped to your
  monitor's refresh rate (max 240 fps)
- V-Sync in fullscreen (on by default): every frame shown for the same time,
  no tearing, even on graphics drivers that ignore the game's request
- Exact frame timing: no more slight stutter from whole-millisecond steps
- Performance overlay (F11 in game, or the launcher setting): frame rate,
  frame times, 1 % lows, a frame-time graph, memory used (and whether the
  4 GB patch is on), texture memory and the game's file buffer
- High-resolution textures from mods: the game's 1 MB limit per texture file
  is gone (the buffer grows to what the mods need)
- Shadows of soldiers and objects no longer reach through the floors and
  walls below them
- Characters no longer get stuck at high frame rates (for example the
  soldiers who stayed inside the helicopter in the opening film instead of
  jumping out)
- Your own gunshots no longer get louder when you aim or zoom (scopes,
  binoculars, mounted guns), and nearby sounds no longer jump closer
- Inventory hints: the item bar shows what the use button does without
  equipping the item - a MediKit heals yourself, night vision goggles toggle,
  weapons with several fire modes change mode
- Mouse acceleration is off by default (turn it on in Options -> Controls ->
  Keyboard & Mouse -> Mouse Options if you prefer it)
- Local co-op for 2-4 players in split screen (main menu -> CO-OP), with
  divider lines between the views
- Split-screen versus for 2-4 players on the multiplayer maps (main menu ->
  VERSUS -> SPLIT SCREEN): Team Deathmatch or Deathmatch
- Mission briefing before each mission: the map with the objectives, the
  objective fly-over on demand, time of day, and uniform, face, voice and
  weapons per soldier
- Vehicles and loose objects behave at high frame rates as they do at 30 fps
- Cheats menu (hidden in the original game) and a developer page with the
  original developers' debug views; free camera
- The Kuwaiti Emir and the missile expert Franklin can be taken over like a
  squad member
- Custom missions and campaigns made with the ModKit's world editor
- Mods folder support: new uniforms, weapons, sounds, texts and models,
  switched on and off in game (main menu -> MODS)
- Discord status: shows the mission you are playing (for example
  "Mission 4: Desert Watch"), single player or split screen, and the time
  since the mission started

Settings
  Everything the launcher offers is also in the game: OPTIONS > VIDEO has
  the resolution, display mode, V-Sync, frame rate limit, anti-aliasing,
  texture filtering, texture resolution, draw distance, shrubs, shadows,
  lighting, moving vegetation, detail textures, decals, environment maps,
  HUD scaling and size and the split-screen layout. OPTIONS > DESERTSTORMFIX
  has controller support, intro / cutscene skipping, the Discord status and
  the performance overlay. Values marked * need a restart (APPLY > RESTART
  NOW does it), + take effect from the next level; resolution, display mode,
  V-Sync and anti-aliasing change with APPLY > APPLY NOW in the main menu
  (choose APPLY again within 15 seconds to keep the new mode, otherwise the
  old one comes back).
  To start without the launcher: OPTIONS > VIDEO > LAUNCHER AT START > SKIP
  (or "Start without this launcher" in its Settings). Hold Shift while the
  game starts to see the launcher again; it also comes back by itself if the
  last start did not get as far as the game.
  In the launcher: click "Settings"; the "Enhancements" section on the right
  has our settings. Press OK to save. The launcher and settings can be used
  with a controller (Xbox, DualShock 4, DualSense): the button hints appear
  at the bottom once a pad is found.
  Our settings are kept for your Windows user in the registry
  (HKEY_CURRENT_USER\Software\PSE\ds1\Plugins\dsfix), shared by every copy of
  the game on the PC (Steam and GOG). The first start of this version copies
  the settings of older versions over once; the old ones are left as they
  were.

Display mode
  Windowed: the game in a window at the resolution you chose (made smaller
  if it does not fit on the screen). Borderless window: the game fills the
  monitor without changing its display mode, so switching to other windows
  is instant. For the sharpest picture in borderless, choose your desktop
  resolution; other resolutions are stretched to fill the screen.
  While the game window is active the mouse stays inside it; press Alt+Tab
  or the Windows key to get it back.

Intro and cutscenes
  "Skip the intro" starts straight in the main menu. The opening film still
  plays in the menu when you leave it idle, like in the original game.
  "Skip mission cutscenes automatically" ends every mission cutscene as it
  starts; the mission briefing is still shown. Without it you can skip any
  cutscene yourself with Esc, the left mouse button or Triangle on a
  DualShock 4 / DualSense.

Anti-aliasing and texture filtering
  Anti-aliasing smooths the jagged edges of buildings, rocks and soldiers.
  If your graphics card can't do the chosen level, the highest level it
  supports is used (or none). Anisotropic filtering keeps the ground and
  walls sharp into the distance instead of blurring them; it is on (16x)
  by default. "Game default" turns it off. Both take effect the next time
  you press Play.

Renderer (new, experimental)
  Launcher > Settings > Renderer, or OPTIONS > VIDEO > RENDERER (takes
  effect after a restart):
    Direct3D 8 (original)  the game's own renderer - the default.
    Direct3D 9             the game drawn through Direct3D 9.
    Direct3D 12 (9On12)    the same, run on Direct3D 12 by Windows' own
                           D3D9-on-12 layer.
  The picture is the same in all three. If something looks or runs wrong
  with a new one, switch back to "Direct3D 8 (original)". HDR and bloom
  are not in this release yet (they need a renderer still being tested).

Discord status
  Turn it off with "Show the game in my Discord status" in the settings.
  The patch only talks to the Discord app running on your own PC (the same
  way other games do); nothing is sent anywhere else. If Discord is not
  running, nothing happens.

Co-op (local split screen, 2-4 players)
  Main menu -> CO-OP. Each player presses Cross (PlayStation), A (Xbox) or
  Space (keyboard & mouse) to join; players are numbered in the order they
  join. Left/Right changes the button icons a player sees, Circle / B /
  Backspace leaves. With two or more players, Options / Start / Enter
  continues to Difficulty and the co-op mission list.
  Co-op has its own progress: the first mission is open (Mission 3 with 3-4
  players - Connors and Jones join the squad there), finishing a mission
  unlocks the next, and finished missions can be replayed. Missions 1-2 are
  for 2 players; with more players they are shown greyed out.
  The split layout (top/bottom or side by side) is set in OPTIONS > VIDEO >
  SPLIT SCREEN LAYOUT (or the launcher's Settings under "Split screen").
  A keyboard player can pick a saved keyboard & mouse profile with
  Left/Right on their row (see Controls below).
  Each player uses the controller layout set for their player number.

Controllers
  Xbox controllers (and other XInput pads) are read like in modern games, so
  both triggers work at the same time. DualSense and DualShock 4 work over USB
  and Bluetooth; rumble, light bar, player lights and the DualSense's
  adaptive triggers need no extra software.
  The adaptive triggers and trigger rumble follow your layout: they sit on
  whichever trigger fires (R2 / RT unless you choose CLASSIC), and their feel
  follows the weapon you hold, or the mounted / vehicle gun you fire.
  In missions the fire trigger has to be pulled through before a shot fires:
  on a DualSense past its resistance (while adaptive triggers are on), on Xbox
  and DualShock 4 pads at the same point. Automatic fire keeps going until you
  ease off a little.
  Vibration and the adaptive triggers can be switched off per player on the
  CONTROLLER screen.
  Controllers can be plugged in or out while the game runs. A controller that
  comes back gets its player back. If a player's controller disconnects
  during a mission, the game pauses and says whose it was.
  In missions a DualSense's light bar shows your soldier's health (green,
  yellow, red; it pulses when you are badly hurt or down) - its player
  lights still show the player number.
  Gyro aiming (DualSense, DualShock 4): turn or tilt the controller to aim,
  either while aiming or all the time - off unless you switch it on.

Turning features on and off (Options -> DESERTSTORMFIX)
  Widescreen, HUD scaling, controllers and split screen are always on.
  Everything else can be switched ON / OFF here: the mission briefing &
  loadout screen, the SAS / US Delta uniform select screen (off = skipped),
  time of day, squad voice accents, "your soldier talks" (the soldier you
  control also calls out enemies, shouts incoming grenades and says "man
  down", "I'm hit", "enemy down" etc. like the AI-led soldiers; off by
  default), playable followers, enhanced rumble,
  inventory hints, the aiming audio and shadow fixes, sharper text, mods,
  weather options,
  custom campaigns, the VERSUS menu, extended cheats, Discord status,
  skipping the intro / cutscenes and the performance overlay. Off means the
  original game's behaviour. Rows marked * apply after restarting the game.
  MENU BAR COLOUR: Left/Right switches between the game's green (GAME) and
  your colour (CUSTOM); Enter / Cross / A opens a colour wheel. Pick the
  colour with the mouse (drag on the wheel and the brightness bar), the
  arrow keys / WASD / D-pad / left stick, and set the brightness with Q / E,
  Page Up / Down, the mouse wheel or L1 / R1 (LB / RB). Delete / Square / X
  goes back to the game's green, Enter / Cross / A keeps the colour,
  Esc / Circle / B cancels. The menu buttons change at once.
  HUD COLOUR works the same for the green parts of the in-game HUD: the
  soldier panels and name bars, the weapon panel, the inventory rows, the
  health bar frame, the mission timer and the bar along the top that shows
  the weapon name and pick-ups. Item pictures, map markers and the health
  bar's fill keep their colours.
  With a light colour (yellow, gold ...) the text on the bars turns dark by
  itself so it stays readable; on darker colours it keeps the game's own.
  FOOTPRINTS: your squad leaves footprints in sand and dirt (as in the
  original game, where they vanish after 12 seconds). GAME (12 S), LONG
  (2 MIN), PERMANENT (until the room for marks on the ground runs out -
  the oldest go first) or OFF. FOOTPRINTS: EVERYONE - every soldier near
  you leaves them, enemies too. WEATHER WIPES FOOTPRINTS - wind, rain, snow
  and sand blur them away, faster the stronger the weather (off = they stay
  for their full time whatever the weather). Cutscenes still clear them.

Controls (Options -> CONTROLS)
  KEYBOARD & MOUSE: the game's Key Assignment and Mouse Options, plus
  PROFILES - four slots for your keys. Select a slot to load it, S (or
  Square / X) saves your current keys into it, Delete (or Triangle / Y)
  clears it. Profiles stay on this PC.
  CONTROLLER: a picture of the controller with a line from each button to
  what it does. The D-pad / arrow keys move around the picture and the
  settings at the top; Cross / A / Enter opens a setting, Left/Right changes
  it, Cross / A / Enter again closes it. At the top choose the player (1-4), the layout and what the
  buttons do on foot, in the Orders menu or in the Inventory, whether the
  controller vibrates and whether a DualSense uses its adaptive triggers,
  gyro aiming (OFF / WHILE AIMING / ALWAYS), the look sensitivity (how fast
  the stick turns the view, 25-200 %) and the stick deadzone (shared by all
  controllers, the same setting as in the launcher). Layouts: DEFAULT (fire on R2 / RT, orders on
  L2 / LT, targeting on the bumpers), CLASSIC (the original game's: fire on
  R1 / RB), SOUTHPAW (DEFAULT with the sticks swapped). To change a button, select the action, press
  Cross / A / Enter and then press the new button; whatever used that button
  swaps with it and the layout becomes CUSTOM. Square / X / Delete resets the
  player to DEFAULT, Triangle / Y / Tab switches between the PlayStation and
  Xbox picture. Layouts are saved per player and used from the next mission.

Online co-op needs the same plugins and mods
  CO-OP -> ONLINE: when a friend connects, both games compare what changes
  the game: the DesertStormFix version, the plugins in PSE\Plugins that
  affect gameplay, and the mods switched on in the MODS menu (their files).
  If anything differs the join is refused before a mission starts, and both
  the HOST and the JOIN screen list the differences (also in
  DesertStormFix-online.log). Use the same release and the same plugin and
  mod files on both PCs; cosmetic plugins (such as the Discord status) may
  differ.

Versus (local split screen, 2-4 players)
  Main menu -> VERSUS -> SPLIT SCREEN. On MATCH SETUP choose the game (Team
  Deathmatch or Deathmatch), the map, time limit, score limit, respawn and
  friendly fire, then START. Players join like in co-op; Left/Right picks
  the team, Up/Down the class (rifleman, heavy weapons, sniper, engineer).
  Options / Start / Enter starts the match. At the end: Rematch or Leave.
  VERSUS -> LEGACY ONLINE has the game's original network screens.

Mission briefing (start of each mission)
  After the mission intro a MISSION BRIEFING screen opens in place of the
  game's briefing popup, with tabs (Tab or L2/R2 switches; Enter / Cross / A
  starts the mission from any tab):
  BRIEFING - the map and the mission's story (Left/Right or Up/Down turn
  the pages of a long one).
  MISSION - the level's map with every objective marked and numbered and
  the squad's position, the objectives (Up/Down shows each one's
  description and highlights it on the map), and TIME OF DAY / TIME FLOW.
  OVERVIEW - plays the mission's night-vision fly-over of the objectives
  with its radio narration (it no longer plays by itself after the
  briefing); Esc / Enter / Cross / Circle skips it, then the screen is back.
  CHARACTER and LOADOUT - each soldier in front of a plain background; the
  world is paused but the squad keeps moving. Up/Down picks a row,
  Left/Right changes it, Q/E (L1/R1) the soldier. The camera goes to what
  the row changes (the hat, the face, the back, each pouch, the hands).
  A/D or the right stick turn it, W/S, the mouse wheel or the right stick
  zoom; R / R3 switches the turning by itself (OPTIONS > DESERTSTORMFIX >
  LOADOUT CAMERA SPIN) on or off. Your soldiers face straight ahead when
  the screen opens, even if you were looking up or down just before
  (OPTIONS > DESERTSTORMFIX > LOADOUT: LOOK AHEAD); C / L3 (RESET VIEW)
  straightens them up at any time.
  CHARACTER: UNIFORM (SAS, US Delta, Russian or Iraqi plus uniforms from
  mods, mixed teams allowed; a new faction hands out its standard weapons
  and puts on its own hat, pack and pouches), SOLDIER (the face and name),
  VOICE, HEADGEAR (caps, bush hats, helmets, berets, shemaghs ...),
  BACKPACK (packs, radio or none), FACE (sunglasses, glasses, cigar,
  beards and moustaches) and POUCHES (the belt, hip and chest pouches as
  one set: SAS, US, Russian or Iraqi per role, or none). Hats, packs and
  face pieces of every faction fit every soldier.
  LOADOUT: MAIN WEAPON, SIDEARM, LAUNCHER and GRENADES (both kinds, or only
  frag / only smoke).
  OPTIONS > DESERTSTORMFIX > FREE POUCH CHOICE turns POUCHES into one row
  per spot (belt back / left / right, hips, chest), each taking any pouch,
  bottle, holster, satchel, torch, wrench or grenade - a piece made for
  another spot can sit a little off there. The campaign no longer asks for SAS
  or Delta before a mission - every soldier's uniform is picked here.
  VOICE: AUTO speaks with the uniform's actors (SAS British, Delta
  American) or a uniform mod's own voice; SAS or DELTA keeps that voice
  whatever he wears (English version of the game; mission cutscenes and
  briefings keep the British voices).
  TIME OF DAY (MISSION tab, the whole squad): the mission's own, dawn,
  day, dusk or night (the world, sun or moon, shadows and the enemies' sight
  follow it; enemies see less at night) - and TIME FLOW - the mission's own,
  or a running day / night cycle at 1 game hour per 10, 5, 2 or 1 real
  minutes, from the chosen time. Remembered for the next missions.
  WEATHER (MISSION tab): the mission's own, clear, overcast, light rain,
  rain, thunderstorm, light snow, snow, blizzard, dust, sandstorm, fog or
  random. Rain, snow and sand fall around you (not under roofs or indoors),
  the sky, light and fog change, the wind gusts, bends the bushes and
  carries smoke and dust, thunderstorms bring lightning and thunder, and
  there is rain, wind and blowing-sand sound. In rain the ground darkens
  as it gets wet and puddles form on flat, low ground near you; after the
  rain they dry up over a minute or two. With the Direct3D 9 renderers
  (OPTIONS > VIDEO > RENDERER, or the launcher) puddles mirror the world
  around them and ripple in the rain; the original renderer shows them as
  dark wet patches. Footprints, puddles and bullet holes now stay through
  cutscenes. Fog, rain, snow and sand
  also cut how far the enemies can see. A change on the screen happens over
  a few seconds. Remembered for the next missions; online the host's
  weather is used. Snow-covered ground needs a texture mod (the weather
  doesn't repaint the levels).
  VEHICLE (MISSION tab, missions where the squad has a vehicle: Mission 8's
  Humvee, Mission 9's Bradley): swap it for another ground vehicle with
  seats for its crew - Humvees, Land Rover, Bradley, Abrams, T-62, T-72,
  BMPs, Shilka, trucks, any faction - or a vehicle or skin from a mod.
  Starting the mission with a new vehicle restarts it with that vehicle
  (the intro is skipped, the screen comes back on the MISSION tab). The
  choice is remembered per mission; a saved game keeps the vehicles it was
  saved with. Not in online co-op.
  Weapons unlock as you find them: every weapon your squad starts a mission
  with or picks up (enemy weapons too) can be chosen later - single player
  and co-op keep separate lists. Weapons from mods are always there. Your
  choices are remembered and set again at the next mission.
  Linked missions keep the squad's kit: Mission 4 continues into 5A, and
  11B into 12A and 12B, with what the soldiers carried at the end. Their
  briefing shows the map, objectives and overview as usual; the CHARACTER
  and LOADOUT tabs show the carried kit ("KIT CARRIED OVER FROM ...", no changes), and a
  chosen time of day / day-night cycle continues from where the previous
  part ended. Loading a saved game keeps the saved kit (no briefing).
  Co-op: the players take turns, player 1 first - each changes only the
  soldiers they control, with their own controller or the keyboard, and
  Enter / Cross / A passes to the next player.
  Weapons, uniforms and gear chosen here are kept in saved games. Each save gets a
  small file of the same name ending in .dsfix - copy it along when you move
  a save.

Followers
  In Mission 2 the Kuwaiti Emir and in Missions 11A / 11B the missile expert
  Franklin can be taken over like a squad member (squad switch keys / buttons).
  Each carries a pistol and a MediKit; they can't pick things up, use mounted
  guns or give orders.

Cheats, developer page and free camera
  The original game's CHEATS menu (OPTIONS -> CHEATS) is unlocked on the main
  menu: hold Left Shift and type DESERTWATCH, or press Square Square Circle
  Circle L1 L1 R1 R1 L2 L2 R2 R2 (PlayStation) / X X B B LB LB RB RB LT LT
  RT RT (Xbox). It stays unlocked. Rows: TROOPER LEVEL, ENEMY LEVEL,
  INVULNERABLE, INFINITE HEALTH, INFINITE AMMO, ENEMY AI, COMMANDEER ANY
  VEHICLE, MISSION LIST and DEVELOPER. Saves made with cheats on are named
  "Cheats ..." and keep them.
  COMMANDEER ANY VEHICLE: get into any ground vehicle with seats - trucks,
  APCs, tanks, enemy ones too, even with their crew still inside (aircraft
  are left alone). Vehicles the game made static (e.g. Mission 4's tank)
  become drivable from the next mission start. For the session only.
  DEVELOPER: the original developers' debug views (AI route markers and walk
  map, AI senses, health bars, engine statistics, wireframe), their debug
  keys (DEBUG KEYS: YES - the original keyboard shortcuts; some are plain
  keys, so switch it off for normal play), game speed 0.25-4x, complete
  mission and resurrect all.
  Free camera (cheats unlocked): F6 or L3 + R3 in a mission. Fly with WASD,
  Q / E up and down, Shift faster (controller: sticks, L2 / R2, L1 faster);
  Esc / Start or F6 again returns to your soldier.

Custom missions and campaigns
  Missions made with the ModKit's world editor (or written by hand in Lua)
  come as mods and need nothing else installed. A mod with a campaign.txt
  adds its missions to SINGLE PLAYER -> CUSTOM CAMPAIGNS (shown when one is
  installed); Continue at the end of a mission goes on to the next.
  "DesertStorm.exe +level <name>.lua" starts a custom mission straight away
  (press a key at PLEASE PRESS ANY KEY).
  Level mods made with the editor don't contain the game's files: the parts
  they reuse are built from your own game at start-up (in your temporary
  folder, %TEMP%\DesertStormFix), so the first start after installing one
  can take a little longer. Edited levels get their AI walk map and
  visibility data rebuilt when they load.
  Mission DLLs: a mod can also bring a mission's own script DLL. Put it in
  the mod's folder (for example Mods\MyMod\Mission6.dll) and the game uses
  it instead of its own Mission6.dll while the mod is switched on. A
  Mission6.dllpatch (made by the ModKit) changes your own Mission6.dll the
  same way without shipping the game's file; if your game's DLL is a
  different version the patch is left out (Mods\DesertStormFix-report.txt
  says so). A DLL with a new name can be started with
  "DesertStorm.exe +level <name>.dll"; a <name>.level file next to it with
  the line "--@archive mission6" makes it use Mission 6's map and sounds.

Developer console (Options -> DESERTSTORMFIX -> DEVELOPER CONSOLE: ON)
  Press the key left of 1 (` or ~) in the menus or in a mission. Commands:
  level <name> starts any level at once (mission6, Mission6.dll, a mod's
  .dll or .lua mission - a mod's file wins over the game's), levels lists
  them, restart reloads the current level, mods lists the mods in use,
  help. Tab completes names, Up / Down repeats commands, Esc closes. The game
  keeps running while it is open (only the keyboard goes to the console).
  A level started here is single player and doesn't continue the campaign.

Mods (new skins and other content, no programming needed)
  Make a folder called Mods in the game folder and put each mod in its own
  folder inside it, for example Mods\PinkSAS\. Files can sit in any sub
  folders. Mods load in name order; to switch one off, put _ in front of
  its folder name (Mods\_PinkSAS).
  Each start writes Mods\DesertStormFix-report.txt: what the mods added
  and any problems ([fail] lines) - look there when a mod doesn't show up.
  The main menu says "N PROBLEMS" when there are some.
  Tools for making mods:
  - Blender add-on (import / export models, soldiers, animations):
    https://www.nexusmods.com/games/conflictdesertstorm/mods/2
  - Mod Kit (step-by-step guide docs/MAKING_MODS.md, example mods, sound
    and texture tools): https://github.com/eSKylezZ/ConflictDesertStormModKit
  - New files (textures .dds / .tga, meshes .evo, animations .prb, sounds)
    are found by their file name, so give them names of their own. A file
    with a game file's name replaces it everywhere (e.g. CLIFFRPT.dds turns
    the cliffs in Mission 1) - same size and format as the original.
  - For experts: tables (.txt named like a game table, e.g. Weaps.txt) hold
    only your NEW rows: they are added to the game's rows and to other
    mods' rows, so several mods can add to the same table. To change or
    delete the game's rows use <TABLE>.replace.txt (e.g. Weaps.replace.txt):
    a row replaces the game row with the same first column, "-KEY" deletes
    one. A .weapon whose "based on" is its own id changes that game weapon.
  - Texts: a .strings file ("KEY = text" lines) changes any text of the
    game by its key; lines under [M01] only apply in Mission 1's texts,
    [all] everywhere.
  - Level sounds: a .sch named like a level's sound file (e.g. MISSION1.SCH)
    replaces the sounds it contains in that level.
  New uniforms: a .skin file plus the texture - the soldier's own texture
  layout, face included, so he stays the same man in different clothes.
  Example, a pink SAS uniform for Bradley, picked on the briefing's CHARACTER
  tab (UNIFORM row, shown as "SAS (PINK)"):
    Mods\PinkSAS\SASPink_01.dds   the texture: DXT1 or DXT5, 512 x 512, same
                                  layout as the soldier's own (Bradley's
                                  SAS one is HERO01_UK_01)
    Mods\PinkSAS\PinkSAS.skin
      uniform      = SAS          # SAS, Delta, Russian or Iraqi: its side
      uniform name = SAS (Pink)   # shown on the UNIFORM row
      soldier      = Bradley      # Bradley, Foley, Connors or Jones
      texture      = SASPink_01   # file name without .dds
      portrait     = PinkPortrait # optional: PinkPortrait.dds, 64 x 64,
                                  # DXT3 (shown on the soldier panel)
  New recruits (other men for the SOLDIER row): leave out "uniform name"
  and give "first name = ..." and "last name = ...".
  A uniform or recruit with its own body: add "body = MYBODY" (MYBODY.evo
  in the mod folder, made with the Blender add-on's exporter (see Tools
  above) from that soldier's own body - Bradley HERO01_ARMSTRONG, Connors
  HERO02_RAMIREZ, Jones HERO03_JONES, Foley HERO04_FOLEY - so the skeleton
  fits). The soldier wears that body while he wears the skin. Give the skin
  a texture name of its own: the body goes with the texture.
  A uniform with its own normal map (the bumps of folds and pockets): add
  "normal map = MYBUMP" (MYBUMP.tga or .dds in the mod folder, the size
  and layout of the soldier's own, e.g. HERO01BUMP_UK_01 for Bradley SAS;
  the ModKit's "ds_texture.py normal" saves one in the game's format).
  Works with or without a body; other uniforms keep the normal one.
  A uniform or recruit with a voice of its own (the soldier speaks with it
  while he wears the skin; VOICE AUTO on the loadout screen):
      voice        = Delta     # the actors to start from: SAS or Delta
      voice from   = Foley     # whose lines: Bradley, Foley, Connors, Jones
                               # (default: the soldier's own)
      voice line 12 = my.wav   # replace line 12 (PCM WAV, mono or stereo)
      voice folder = lines     # every ...line01.wav ... in that folder
  The ModKit's "ds_sound.py wav <game>\mission3.sch Bradley out --accent
  delta" saves a soldier's 47 lines with those numbers and what each is
  (the ModKit's guide has the full list; 37 and 47 are never played).
  Cutscene and objective audio: cutscenes are played by the engine, their
  sound is one file per scene (Data\Sound\English = SAS voices, American =
  Delta). A mod's Sound\SAS\CS_M01_S01.psf, Sound\Delta\... or Sound\...
  (all languages) replaces that scene's sound (whole mix; keep the length;
  "ds_sound.py stream" / "psf" convert to and from WAV).
  Keep textures compressed (DXT). Bigger textures than the game's own work
  (2048 x 2048, 4096 x 4096); they cost memory, so for a big texture pack
  turn on 4 GB memory (see below) and watch the memory line of the
  performance overlay (F11).
  New gear (hats, packs, face pieces, pouches): a .gear file, then picked
  on the briefing's CHARACTER tab. Example, a pink beret:
      id       = PINK_BERET      # model name: letters, digits, _
      slot     = head            # head, back, face or pouch
      name     = Pink Beret      # shown on the row
      based on = IR_BERET_RED    # a game piece ...
      texture  = PinkBeret       # ... with PinkBeret.dds from the folder
  Or "model = MYHAT" for a model the mod ships (MYHAT.evo).
  Pouch sets for the POUCHES row: a .pouches file - "name = ..." and one
  line per spot (belt, belt left, belt right, hip left, hip right, chest
  left, chest right) naming a game pouch (e.g. UK_AMMOPOUCH01) or a .gear
  pouch id; spots left out stay empty.
  Vehicle skins and new vehicles: a .vehicle file, then picked on the
  briefing's MISSION tab (VEHICLE row). Example, a sand-coloured Humvee:
      id        = DESERT_HUMVEE      # vehicle name: letters, digits, _
      based on  = US_VEH_Hum_Vee     # a game vehicle (VEHICLE.TXT): seats,
                                     # handling, guns and sounds are its
      name      = Humvee (Desert)    # shown on the row
      texture humvee01 = DesertHumvee01   # per texture slot of the model,
      texture humvee02 = DesertHumvee02   # .dds files from the folder
  "texture = X" for a model with one texture, "damaged texture <slot> = X"
  for the wreck, or "model = MYTANK" for a model the mod ships (MYTANK.evo;
  MYTANK.DYN for its tracks / wheels, else the base vehicle's is used) and
  "damaged model = X". "faction = ..." is shown next to the name.
  "engine sound = X" gives it its own engine (a sound bank X in a .sch
  bank pack in the folder, laid out like the game's Wheels / Tracks banks)
  and "grip = 0.3" a tyre grip (the game's vehicles never slide; low values
  drift and slide like a hovercraft); "spin damping = 30" stops a sliding
  vehicle from spinning on after you let go of the steering (the game's
  value is 0.2). Its handling otherwise comes from the
  model's .DYN file (mass, suspension, steering; the ModKit's guide lists
  the fields).
  New soldier types: a .unit file, then placed in a mission with the
  ModKit editor (or a scene file) like any game soldier. Example:
      id         = Iraqi_Militia     # max 35 characters, no commas
      based on   = Iraqi_Conscript   # a game soldier type (UNITS.TXT)
      type       = SOLDIER           # SOLDIER, CIVILIAN, GOAT HERDER,
                                     # EMIR, MISSILE EXPERT or POW
      model      = IraqInfantry01_Sandy  # any soldier model (standard
                                     # skeleton - the game's or a mod's)
      health     = 60
      experience = Veteran           # Rookie, Green, Trained, Regular,
                                     # Veteran, Expert, Professional ...
      voice      = Iraqi_Civilian    # speech bank (.sbk), any level's
      item       = IR_WPN_AKSU       # one line per weapon / item; none =
      item       = MediKit, 1        # no kit; no item lines = the base's
  "column N = x" / "stat column N = x" set any other UNITS.TXT / HSTAT.TXT
  column. Raw UNITS.TXT / UNITWPNS.TXT / VEHICLE.TXT rows are checked too:
  rows the game can't load (too long a name, an unknown type, base stats,
  experience, propulsion, a kit row for a missing unit) are left out and
  listed in Mods\DesertStormFix-report.txt, and the item / gun slot count
  follows the kit rows (a slot without a row used to crash the game).
  New weapons: a .weapon file - start from a weapon of the game and list
  only what changes. Example, a silenced M249 SAW for Connors:
    Mods\SilencedSAW\SilencedSAW.weapon
      id       = US_WPN_SAW_Silenced     # the new weapon's name
      based on = US_WPN_SAW_lightmg      # copy of the normal SAW
      silenced = yes
  Settings (# starts a comment; only "id" and "based on" are required):
    id                   name of the new weapon
    based on             weapon it starts as (see the list below)
    silenced             yes = no muzzle flash, the MP5SD's quiet shot and
                         enemies only hear it up close
    sound                another weapon's gunshot, e.g. US_WPN_M60E3
    sound bank           a sound bank by name, e.g. from a .sch bank pack in
                         the mod folder
    shot sound           your own gunshot: a WAV file in the mod folder
                         (mono or stereo, 16-bit, e.g. 22050 Hz); "shot
                         sound 2" for a second variation (the game picks
                         one at random), "tail sound" for the distant echo
    display name         name shown in the inventory, e.g. M249 SAW (Silenced)
    texture              the gun's texture: file name without .dds, same
                         layout as the original (DXT1, e.g. 128 x 32 for
                         the SAW); "texture <part> = X" for another part,
                         e.g. texture LMG01_M249AmmoBox = MyAmmoBox
    muzzle flash         yes / no
    hearing range        how far enemies hear it (80 = normal gun, 8 = quiet)
    damage               per bullet (SAW 15, M16A2 14, Barrett 80)
    magazine size        rounds per magazine
    magazines            magazines a soldier gets when it is chosen on the
                         briefing's LOADOUT tab, the one in the gun included
                         (the HUD shows the spare ones: 7 shows as 6)
    reload time          seconds
    time between shots   seconds (0.10 = 600 rounds a minute)
    range                (SAW 850, pistol 340, sniper rifle 1000)
    model                a new mesh: e.g. MYSAW for MYSAW.evo in the mod folder
                         (a silenced version needs one with a suppressor, an
                         unsilenced version of a silenced gun one without),
                         made with the Blender add-on's exporter (based
                         on the gun it replaces, so hands and muzzle stay
                         in place)
    hud icon             the weapon's picture in the HUD, the inventory and on
                         pick-ups: a PNG in the mod folder (file name
                         without .png; transparent background, 1 pixel =
                         1 pixel of the 800 x 600 HUD, the SAW's is 115 x 35)
                         - give a weapon that looks different a matching one
    type                 category text
    faction              sides that get it first on their loadout rows:
                         SAS, Delta, Russian, Iraqi, allied, eastern, all
    aim animation, reload animation, throw animation and their "prone"
    versions             .prb animations by name (a mod folder can also
                         replace a game animation or model by using its name)
    column N             any other column of Weaps.txt (for experts)
  A .weapon file with only a game weapon's id (no "based on") makes that
  weapon choosable on the loadout screen as it is, e.g. "id = IR_WPN_AK47"
  and "faction = Iraqi".
  Sounds are found in the game's missions and added wherever they are
  missing, so a borrowed gunshot works in every mission.
  To start from a game sound, the Mod Kit saves a weapon's sounds as WAV
  files: python cli/ds_sound.py wav mission3.sch M16A2 out
  Examples of everything above (uniforms, recruits, weapons, models and
  animations made in Blender): the Mod Kit, folder examples.
  Weapon names: US_WPN_M16A2, US_WPN_M16, US_WPN_HKPSG1,
  US_WPN_BarrettSniper, US_WPN_AccuracyInter, US_WPN_M60E3,
  US_WPN_SAW_lightmg, US_WPN_MP5SilencedSubMG, US_WPN_Remmington870,
  US_WPN_FranchiSPAS, US_WPN_SIGP228, US_WPN_Beretta92F,
  US_WPN_DesertEagle, US_WPN_LAW66, US_WPN_LAW80, US_WPN_FIM92,
  US_WPN_GrenadeFrag, US_WPN_GrenadeSmoke.
  The game allows only one rifle with a grenade launcher: a weapon based on
  US_WPN_M16A2 becomes an assault rifle without one. New weapons can't be
  based on US_WPN_GrenadeFrag (Mods\DesertStormFix-report.txt says so) -
  smoke grenades are fine.

Download
  Latest version: https://www.nexusmods.com/games/conflictdesertstorm/mods/1

Install
  Copy everything in the zip into the game folder, next to DesertStorm.exe:
    dinput8.dll                  starts the Pivotal Script Extender (PSE)
    PSE\pse_loader.dll           the extender itself
    PSE\Games\pse_ds1.dll        Conflict: Desert Storm support
    PSE\Plugins\dsfix.dll        DesertStormFix (+ dsfix.pse)
    PSE\Plugins\dsfix.discord.*  Discord status - delete both files to drop it
    DesertStormFix-Patcher.exe   only if you want 4 GB memory (below)
  Keep the folder structure: dinput8.dll and the PSE folder must both be there.
  Updating from an older version: overwrite everything (the old dinput8.dll
  is replaced by the new one of the same name).
    Steam: ...\steamapps\common\conflict_desert_storm
    GOG:   ...\GOG Games\Conflict - Desert Storm
  The game files are not modified.

4 GB memory (optional)
  The game is a 32-bit program and normally gets 2 GB of memory. Run
  DesertStormFix-Patcher.exe (it can stay in the game folder, or run from
  anywhere) and press "Turn 4 GB on" to let it use 4 GB on 64-bit Windows -
  room for bigger mods and missions. It finds the Steam and GOG installs by
  itself ("Add folder..." for others).
  - It changes one flag in DesertStorm.exe (Large Address Aware), nothing
    else, and only on the Steam / GOG version. The original is kept as
    DesertStorm.exe.original; "Turn 4 GB off" makes the exe the original
    again.
  - Close the game first. After that, start the game the usual way.
  - Steam's "Verify integrity of game files" or a GOG update puts the
    original exe back: just run the patcher again.
  - A game in Program Files needs administrator rights; Windows asks.
  - Until it's on, the main menu shows "2 GB MEMORY MODE" at the bottom right.

Shared asset store (optional, off by default)
  OPTIONS -> DESERTSTORMFIX -> PLUGINS -> Shared asset store -> SHARED ASSET
  STORE ON keeps one copy of all the game's files and level sounds in
  PSE\Cache\assets (about 350 MB; %LOCALAPPDATA%\PSE\ds1-... if the game
  folder can't be written). It is made from your own game files at the next
  start - a small window shows the progress (a few seconds; Cancel, Esc or
  Circle / B starts the game without it this time). Then every level can use
  vehicles, weapons and sounds of the other levels without copying files.
  It is rebuilt by itself when the game files change; REBUILD CACHE AT NEXT
  START does it on demand. Deleting the PSE\Cache folder is always safe (it
  is built again at the next start while the setting is on).

Missions from Lua (optional, off by default)
  OPTIONS -> DESERTSTORMFIX -> PLUGINS -> Translated missions -> MISSIONS
  FROM LUA ON (takes effect at the next start) runs the game's mission
  scripts (Mission*.dll / Training*.dll) as Lua made from your own game
  files, in PSE\Cache\missions (all made at start-up: about a second the
  first time, again only after a game or DesertStormFix update). They play exactly like the originals - checked
  frame by frame against the DLLs - and saves work either way. A mission
  that isn't on the checked list, or comes from a mod, keeps its DLL.
  This is the groundwork for editing missions; there is nothing new to see
  in game yet.

Original game files (PSE\Originals)
  At start-up DesertStormFix moves the game's archives (*.dat), level sound
  caches (*.sch) and mission scripts (Mission*.dll, Training*.dll) into
  PSE\Originals once it has read them; the game finds them there. Nothing is
  deleted. If a game update or Steam / GOG "verify files" puts a file back,
  it is moved again at the next start. Don't delete PSE\Originals - the game
  needs those files.

Uninstall
  First move everything in PSE\Originals back into the game folder. Then
  delete dinput8.dll and the PSE folder from the game folder (deleting only
  PSE\pse_loader.dll or PSE\Plugins\dsfix.dll also turns everything off - the
  game then runs as it came, once the original files are back). If you turned
  4 GB on, run
  DesertStormFix-Patcher.exe and press "Turn 4 GB off" first (or rename
  DesertStorm.exe.original back to DesertStorm.exe).

Updating from 1.0 or 2.0
  Replace dinput8.dll and copy the PSE folder. Your settings, key profiles, controller layouts,
  loadout choices and saved games are kept. The campaign no longer asks for
  SAS or Delta: every soldier starts with the SAS skills and ranks, and the
  uniform is picked per soldier on the briefing screen.

Stutter reports
  Press Ctrl+F11 in game to start recording frame times, play for a minute,
  press Ctrl+F11 again and attach the CSV file it writes next to
  DesertStorm.exe to your report.

Known issues
  4-player co-op and split-screen versus with real controllers still need a
  full play-through - please report any problems.
  Windowed and borderless modes are untested on setups with several
  monitors.
  After switching mods on / off twice in the MODS menu, starting a mission
  crashed once in testing - restart the game after changing mods if it does.
  Mission 11B: pressing Space within the first seconds of the level crashed
  the game twice in testing.

Credits
  Controller button icons: "Input Prompts" by Kenney (www.kenney.nl), CC0.
  Controller pictures: "PS5 Button Icons and Controls" and "Xbox Series
  Button Icons and Controls" by Zacksly (https://zacksly.itch.io), licensed
  under CC BY 3.0 (https://creativecommons.org/licenses/by/3.0/). Modified:
  cropped and scaled.
  Mission scripts run on Lua 5.4 (https://www.lua.org), Copyright (C)
  1994-2024 Lua.org, PUC-Rio, MIT license:
    Permission is hereby granted, free of charge, to any person obtaining a
    copy of this software and associated documentation files (the
    "Software"), to deal in the Software without restriction, including
    without limitation the rights to use, copy, modify, merge, publish,
    distribute, sublicense, and/or sell copies of the Software, and to
    permit persons to whom the Software is furnished to do so, subject to
    the following conditions:
    The above copyright notice and this permission notice shall be included
    in all copies or substantial portions of the Software.
    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
    OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
    MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
    CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
    TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
    SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
  Missions from Lua are translated with the Zydis disassembler
  (https://zydis.re), Copyright (C) 2014-2024 Florian Bernd and Joel Hoener,
  MIT license (the same text as Lua's above).

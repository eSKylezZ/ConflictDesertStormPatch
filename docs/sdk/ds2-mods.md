# Desert Storm II – the Mods folder (what a mod can replace today)

DesertStorm2Fix (standalone `dinput8.dll` or the PSE host plugin `ds2fix`, sub-plugin `ds2fix.mods`) reads loose files
from `<game>\Mods\`. A mod replaces or adds whole files by name, `.skin` files dress particular soldiers and sound
packs / level sounds / music / films, table rows (`.txt` / `.replace.txt`), texts (`.strings`) and readable
`.weapon` / `.vehicle` / `.unit` files are merged in (below).

## Layout

```
<game>\Mods\<mod name>\...any sub folders...\<FILE.EXT>
```

- Every file in a mod folder (sub folders included) is found by its **file name** – the folder it sits in doesn't
  matter, case doesn't matter. `Mods\PinkRover\textures\SAS_LAND_ROVER_1.DDS` replaces the game's
  `SAS_LAND_ROVER_1.DDS` wherever the game loads it.
- Mods are applied in name order; when two mods ship the same file name, the later one wins (the report says which).
- A mod folder whose name starts with `_` or `.` is switched off (`_PinkRover`).
- The folders are read when the game starts. Players switch mods in game: main menu **MODS** lists every folder with
  ON / OFF (choosing a row renames the folder: `_` in front = off, so the menu and Explorer agree) and **APPLY n
  CHANGES** rebuilds the file list at once. What the game had already read keeps its old copy until it reads the file
  again: a mission's files from the next level start, files read at start-up or by the menus (front-end art, fonts,
  texts, the `.txt` tables, `FrontEnd.sch`) only after a restart – the menu then says **RESTART THE GAME TO APPLY**
  and the report names those files (`… was read at start-up / by the menus - the change shows after a restart`).
  A mod folder added while the game runs shows up the next time the MODS screen opens.
- The main menu shows `DESERTSTORM2FIX x.y.z - N MODS INSTALLED (M ACTIVE)` under the menu (+ `n MOD PROBLEMS - SEE
  MODS\DESERTSTORM2FIX-REPORT.TXT` when the report has `[fail]` lines).
- Setting `FeatMods` (default on; shown as MODS among DesertStorm2Fix's optional features where its settings are listed)
  switches the whole folder off.

## What can be replaced or added

Anything the game opens by name through its file system, from the archives (`*.dat`) or the game folder:

| Kind | Examples | Notes |
| --- | --- | --- |
| Textures | `.dds`, `.tga`, `.png`, `.bmp`, `.ddl` | replace a texture by its name; a new name (one a mod's model refers to) works too. Keep the format the game uses (DXT1 / DXT3 / DXT5 `.dds`). Size is free (powers of two); see the limit below |
| Models | `.evo` | a model exported from the ModKit / Blender add-on, or a game model with changed texture slots |
| Animations | `.prb` | |
| Scenes / level data | `.env`, `.rmd`, `.qtd`, `.oct`, `.dyn`, `.rfx`, `.fas` | a whole replacement, or as changes against the game's own file (`.envpatch` / `.rmdpatch`, see [Levels and missions](#levels-and-missions)); a changed scene gets its octree and AI navigation rebuilt |
| Tables | `.txt`, `.replace.txt` (catalog tables) | merged, not whole files: a mod's `.txt` = rows added, `.replace.txt` = rows replaced / removed - see [Tables](#tables--rows-added-replaced-or-removed) |
| Texts, weapons, vehicles, soldier types | `.strings`, `.weapon`, `.vehicle`, `.unit` | readable files the plugin turns into texts / table rows / model copies - see [Texts](#texts-strings), [Weapons](#weapons-weapon), [Vehicles](#vehicles-vehicle), [Soldier types](#soldier-types-unit) |
| Sounds | `.sch`, `.psf`, `.sbk` | merged in, not whole files: `.sch` sound packs / level sound replacements, `.psf` music - see [Sounds](#sounds) |
| Films | `.bik` | by file name (`Logo1PAL.bik`, `B01PAL.bik`, `F01PalUk.bik` …) - see [Sounds](#sounds) |

A file only a mod has (a new texture a mod's model uses, …) passes the game's "is this file there?" checks too.

Mission script DLLs (`MissionNN.dll`) load from mods too - see [Levels and missions](#levels-and-missions) - and so do
Lua missions (`.lua` levels) and campaigns - see [Lua missions](#lua-missions-and-custom-campaigns). Not covered:
files the game writes (saves, settings).

## `.skin` files – a soldier's own textures, body and name

A `.skin` file (any name, `.skin` extension, anywhere in a mod folder) changes how particular soldiers look – their own
textures, another body model, and for the squad their name and portrait – **without replacing a file other soldiers
share**. Text, one `key = value` per line, `#` starts a comment, keys are not case sensitive:

```
# Mods\FoxMod\fox.skin
soldier = Bradley                 # who wears it
uniform = Delta                   # optional: only in Delta (or SAS) missions
body = FOX                        # FOX.evo in the mod (exported with the ModKit's Blender add-on on a DS2 soldier body)
texture = FOX_BODY                # optional: one texture over every part of the body
texture FOX_HEAD = FOX_FACE       # optional: one part's texture, by the part's object name ...
texture 2 = FOX_GEAR              # ... or by its number (1 = first skin part in the model)
first name = JACK                 # squad members only: the name on the soldier panel
last name = FOX
portrait = FOX_PORTRAIT           # squad members only: the panel picture (a texture)
```

| Key | Meaning |
| --- | --- |
| `soldier` (or `unit`) | Who: unit names from `UNITS.TXT` (column 1), comma-separated: `Bradley`, `Foley`, `Jones`, `Ramirez` (= Connors; `Connors` works too), `Iraqi_Conscript`, `US_Guard`, …; `rifleman` / `heavy` / `engineer` / `sniper` = the four heroes; `squad` = every squad member; `all` = everyone. A hero's name also covers his `_Training` unit. Required |
| `uniform` | `SAS`, `Delta` or `any` (default): the campaign's uniform choice. The squad wears a different body + texture per uniform (UNITS.TXT columns 3 / 4), so a texture painted for one doesn't fit the other – use two `.skin` files |
| `texture` | One texture for the whole character (every skin part). Without `body` it must fit the soldier's own body's UV layout – start from his own texture (`Rifleman_DF_Standard.dds` …) |
| `texture <part> = X` | One part's texture: `<part>` = the object name of a skin part of the `body` (as named in Blender / the `.evo`), or its number (1 = the first skin part in the model). Beats `texture` for that part |
| `body` | Another model for those soldiers: `<NAME>.evo` in a mod (or a game model's name). It must be a DS2 character on the soldier skeleton (export it from a DS2 soldier body); extra skin objects (a body split into several parts, one texture each) are fine. Without `texture` lines the body's parts keep **their own textures** (the TEXL names in the `.evo`) |
| `first name`, `last name` | Squad members: the name the game's texts use for him (messages, mission stats; ≤ 35 characters). The soldier panel's name plate is not changed |
| `portrait` | Squad members: the soldier panel's portrait (a texture: 64 x 64 `.png` / `.dds` like the game's `*_Portrait02`) |
| `name` | Ignored (a label for you) |

Textures are file names without extension (`X.dds`, `.tga`, `.png` in a mod folder; DXT `.dds` recommended); the
`.skin` names them, so they are only ever used for the soldiers it names – other soldiers keep the game's textures. When
several `.skin` files name the same soldier, the last one (by mod folder, then file name) wins.

What the game does with it: a soldier is dressed when he is created (level start, reinforcements, a loaded save) - his
body is built on the `.skin`'s model from the start, so a body exported from a DS2 soldier with extra skin objects (the
add-on splits big characters into several, one texture each) works. A `.skin` changed in the MODS menu applies to
soldiers created after that (the next level).
Far-away enemies switch to the game's low-detail models; those show the `texture` line (if any) and else the
game's own low-detail texture.

Report lines (`Mods\DesertStorm2Fix-report.txt`):

```
Skins: fox.skin (FoxMod) - soldier BRADLEY (Delta uniform), body FOX, 2 part texture(s), name JACK FOX
[ok]   Skins: 1 .skin file(s)
Skins: fox.skin applied to Bradley (3 skin part(s), 2 part texture(s), 1 of the body's own, body FOX)
[fail] Skins: fox.skin - texture FOX_FACE isn't in any mod (needs FOX_FACE.dds / .tga / .png in a mod folder)
[fail] Skins: fox.skin - body FOX didn't load as a character model
[fail] Skins: texture FOX_GEAR didn't load (missing, or not a texture file)
```

## Tables – rows added, replaced or removed

The game's data tables are text files in `catalog.dat` (one row per line; get them with the ModKit's archive extract):
`Weaps.txt` (weapons), `Inven.txt` (other items), `Props.txt`, `HStat.txt` (soldier base stats), `VStat.txt`,
`fNames.txt` / `sNames.txt`, `CSkins_<SAS|US>_<R|S|H|C>.txt`, `ConnObjs.txt`, `Units.txt` (soldier types), `Vehicle.txt`
(separated by spaces, not commas), `featStat.txt`, `Structs.txt`, `Feats.txt`, `Doors.txt`, `Targets.txt`,
`Switches.txt`, `UnitWpns.txt` (soldiers' kits), `HSkills.txt`, `VehWpns.txt` (vehicles' guns), `AlternativeVehicles.txt`,
`ExMods.txt`, and the AI rule files `MovementStateRules.txt`, `UpperBodyStateRules.txt`, `VehicleStateRules.txt`. A mod
never ships a whole table:

| File in a mod folder | What happens |
| --- | --- |
| `<TABLE>.txt` (e.g. `UnitWpns.txt`) | **its rows are added** to the game's rows (and to the rows other mods add) – several mods can each add rows to the same table |
| `<TABLE>.replace.txt` (e.g. `Weaps.replace.txt`) | each row **replaces** every game row with the same first column (case doesn't matter) at that row's place; `-KEY` on a line of its own **removes** that key's rows; a key the table doesn't have is added at the end. When two mods replace the same key the later mod wins |
| a `.txt` the game doesn't have | used as it is (a new file another of the mod's files reads) |

Blank lines and lines starting with `#` or `//` are left out of the row tables. The merged table is written to
`%TEMP%\DesertStorm2Fix\merged\` and the game reads that copy (report: `Mods: served Weaps.txt from MyMod (…)`). Tables
are read once when the game starts: a change made in the MODS menu needs a restart (the menu says so).

Example – the SAW's magazine (column 16) from 100 to 123, and a mod's new soldier type (a `.unit` file) gets a pistol:

```
# Mods\MyMod\Weaps.replace.txt - one whole Weaps.txt row: copy the game's and change what you want
US_WPN_SAW_lightmg,LMG01_M249SAW,-1,-1,IMAGE_WEAPON_SAW,MACHINE GUN,Bip01 GunR Connector,-1,0,1,0.10,3,0.50,1,10,123,2.00,…

# Mods\MyMod\UnitWpns.txt - unit, item, item type, amount
Iraqi_Sniper,US_WPN_TEST_PISTOL,WEAPON,3
```

**Rows the game can't load are left out** with a `[fail]` line in the report – DS2 quits at start-up on a bad row of these
tables:

| Table | Checked (mod rows) |
| --- | --- |
| `Weaps.txt` | id at most 35 characters, 79 columns, model name at most 34; only one weapon each of the types `GRENADE LAUNCH/RIFLE`, `GRENADE LAUNCHER`, `GRENADE`, `FIST` |
| `HStat.txt` | name at most 35 characters |
| `Units.txt` | 15 columns, name at most 35 characters, model names at most 34, type `SOLDIER` / `CIVILIAN` / `GOAT HERDER` / `EMIR` / `MISSILE EXPERT` / `POW`, base stats (column 6) an `HStat.txt` row, experience (column 11) `Rookie`, `Green`, `Trained`, `Regular`, `Veteran`, `Expert`, `Professional`, `Guard`, `Crack`, `Elite`, `Specialist`, `Skill One` … `Skill Four` (case matters) |
| `UnitWpns.txt` | the unit is a `Units.txt` row, the item type one the game knows (`WEAPON`, `MEDI KIT`, `BINOCULARS`, `NIGHT VISION GOGGLES`, `C4 DETONATOR`, `BODY ARMOUR`, `HEAD ARMOUR`, `RADIO`, `KEY`, …), the item a `Weaps.txt` row (type `WEAPON`) or an `Inven.txt` row |
| `VehWpns.txt` | the vehicle is a `Vehicle.txt` row, the item a weapon |

`Units.txt` column 15 (item slots) is set to the number of the unit's `UnitWpns.txt` rows and `Vehicle.txt` column 51
(gun slots) to its `VehWpns.txt` rows – more rows than slots would stop the game, fewer leave empty slots (a report line
says when it changed one). The game's own rows are only checked for their references (a base stat a mod removed …).

DS2 has no loadout screen: a new weapon reaches the game by giving it to a unit in `UnitWpns.txt` (every soldier of that
type gets the unit's kit when he is made, then the kit his level's scene lists for him) – or change a game weapon in
place (`based on` = its own id, below), so everyone who carries it gets the change.

**Bigger kits for soldiers the shipped levels place** (a pistol or an AK added to Bradley, a knife added to the Iraqi
conscripts) work: every kit item is an object made with its soldier, so each level's later objects come one place later
than the level's shipped octree (`<scene>.OCT`, what the game culls and collides with) expects – without help vehicles
sank into the ground or fell through it and Mission 1's opening never handed over. While a mod gives any unit more
`UnitWpns.txt` rows than the game, DesertStorm2Fix rebuilds each level's octree from the loaded scene at level start
(about 50 ms; the report says `UnitWpns.txt gives Bradley 1 more item(s) than the game - level octrees are rebuilt …`,
the log `octree for the scene (bigger unit kits) … rebuilt`). Swapping an item (same number of rows) or adding one that
stacks (a second MediKit) never needed it.

## Texts (`.strings`)

A `.strings` file (any name) replaces texts the game shows – menu items, weapon names, mission texts – or adds new ones.
UTF-8 (or ANSI) text, `KEY = text` per line, `#` comments, `\n` = a new line in the text:

```
# Mods\MyMod\menus.strings
S_PLAYER = SINGLE PLAYER (MODDED)
0x29257EAE = MORE OPTIONS
US_WPN_M16A2 = M16A2 ASSAULT RIFLE
[Mission01]
US_WPN_M16A2 = M16 (FIRST MISSION ONLY)
[all]
```

DS2's text files keep only a hash of each key (the archive hash of the lower-cased key), not its name. Write a key as
its name – DS2 uses most of Desert Storm's names (`S_PLAYER` = SINGLE PLAYER; DS1's `catalog.dat` has them all), and a
weapon's or vehicle's name is the text of its id – or as the hash itself, `0x` + 8 hex digits (the ModKit's
`evo.archive_hash(key, upper=False)` computes it; DesertStorm2Fix's own menu texts are listed in
[reference/game-ds2.md](reference/game-ds2.md)). `[Mission01]` (or `[M01]`, `[Training02]`, `[T02]`) starts texts that
apply only while that level runs (missions reuse their keys); `[all]` goes back to every level. A mod's text wins over
the game's; with several mods the later one wins. Texts change at once (the next time the game draws them), also after
the MODS menu's Apply.

## Weapons (`.weapon`)

A `.weapon` file (any name) makes a new weapon from a game weapon, or changes a game weapon in place, with named
settings instead of 79 numbered columns:

```
# Mods\RedM16\m16.weapon - the M16A2 itself changed: every soldier carrying one gets it
id = US_WPN_M16A2
based on = US_WPN_M16A2
texture = RED_M16              # RED_M16.dds in the mod becomes the model's main texture
magazine size = 77
display name = RED M16A2

# Mods\MyMod\pistol.weapon - a new weapon (give it to a unit in UnitWpns.txt)
id = US_WPN_TEST_PISTOL
based on = US_WPN_DesertEagle
display name = TEST PISTOL
damage = 40
```

| Key | Weaps.txt column (1-based) | Notes |
| --- | --- | --- |
| `id` | 1 | the weapon's id (at most 35 characters, no commas / spaces); `based on` = the same id changes the game weapon in place |
| `based on` | | the row it starts from (a game weapon or one another `.weapon` / mod adds) |
| `model` | 2 | an `.evo` in the game or a mod |
| `texture = X`, `texture <slot> = X` | | a copy of the model with texture X (`X.dds` / `.tga` in a mod or a game texture): `texture` = the model's main texture (the one named like the model, else the first one listed) wherever the model uses it; `<slot>` = another texture's name in the model. The copy is made from the game's file as `<ID>.evo` (`<ID>_MOD.evo` for an in-place change) |
| `hud icon` | 5 | the weapon's picture (HUD weapon panel, inventory, pick-up icon): a picture file in the mod (`hud icon = MyGun` = `MyGun.png` / `.dds` / `.tga`, see [Weapon pictures](#weapon-pictures)) or a picture name the game has (`IMAGE_WEAPON_SAW` …) |
| `type` | 6 | `ASSAULT RIFLE`, `MACHINE GUN`, `SUB-MACHINE GUN`, `PISTOL`, … |
| `connector` | 7 | where it's held (`Bip01 GunR Connector`) |
| `time between shots` | 11 | seconds |
| `magazines`, `magazine size` | 15, 16 | |
| `reload time` | 17 | seconds |
| `damage`, `range` | 23, 24 | |
| `muzzle flash` | 36 | yes / no |
| `tracer red` / `tracer green` / `tracer blue` / `tracer alpha` | 45 – 48 | 0 … 1 (new in DS2) |
| `aim animation`, `reload animation`, `throw animation`, `prone aim animation`, `prone reload animation`, `prone throw animation` | 58 – 63 | `.prb` names |
| `sound bank`, `sound = <weapon id or bank>` | 68 | another weapon's shot sound (its bank is added to every level) |
| `hearing range` | 69 | |
| `silenced = yes` | | muzzle flash off, hearing range 8, the MP5SD's shot sound |
| `shot sound`, `shot sound 2`, `tail sound` | | own WAV files (see [Sounds](#sounds)) |
| `display name` (or `name`) | | the weapon's name in the game (the text keyed by its id) |
| `column N` | N | any other column |

A `.weapon` with only `id` (a game weapon) and `display name` just renames it. The game allows one weapon each of the
types `GRENADE LAUNCH/RIFLE` (a copy of the M16A2 becomes an `ASSAULT RIFLE`), `GRENADE LAUNCHER`, `GRENADE` and `FIST`.
DS2's `Weaps.txt` is DS1's with four tracer-colour columns inserted after column 44: a DS1 column number from 45 on is 4
higher here (DS1's `.weapon` files work once their `column N` lines are moved).

### Weapon pictures

`hud icon = MyGun` in a `.weapon` draws `MyGun.png` (or `.dds` / `.tga`; give the name with or without the extension)
wherever the game shows that weapon's picture - the HUD weapon panel, the inventory rows, the pick-up icon - tinted,
faded and scaled with the HUD like the game's own pictures.

```
# Mods\GoldM16\m16.weapon - the M16A2 with its own picture
id = US_WPN_M16A2
based on = US_WPN_M16A2
hud icon = GoldM16          # GoldM16.png in the mod folder
```

- **Size**: one picture pixel = one pixel of the game's 800 x 600 HUD; the game's weapon pictures are about 85-120 x
  23-38 (the M16A2's is 93 x 33). Draw yours at that size, with a transparent background (PNG with alpha, or DDS
  DXT3 / DXT5); up to 1024 x 1024 loads, bigger than 256 x 96 is noted in the report. The picture keeps the base
  weapon's placement on the panel.
- **Formats**: anything Windows' image decoders read (PNG, DDS DXT1-5, TGA, BMP, JPEG); paletted / odd DDS formats may not.
- Needs DesertStorm2Fix on PSE (the internal plugin `ds2fix.weaponicons`); the standalone `dinput8.dll` keeps the base
  weapon's picture and says so in the report. A file that won't load or a name that is neither a mod picture nor a game
  picture = a `[fail]` line, the base weapon's picture is used.
- A weapon whose `hud icon` changes is read at start-up (Weaps.txt): after switching such a mod on in the MODS menu,
  restart the game.

## Vehicles (`.vehicle`)

```
# Mods\PinkRover\rover.vehicle
id = UK_VEH_LandRover_SAS        # a Vehicle.txt row (column 1)
based on = UK_VEH_LandRover_SAS  # the same id: the game vehicle changed in place
texture = PINK_ROVER             # a copy of its model (and its .DYN) with this texture
damaged texture = PINK_ROVER_DMG # the wreck model's texture
```

Keys: `id`, `based on`, `texture[ <slot>]`, `damaged texture[ <slot>]`, `model`, `damaged model`, `name` (the text keyed
by the id). **In place** (`based on` = its own id) a texture change replaces the vehicle's model file itself with the
retextured copy (same name): a level's scene names the model of each vehicle it places, so every vehicle using that
model changes (the other Land Rover variants have models of their own). A **new id** copies the base row, its model
(`<ID>.evo` + `<ID>.DYN`) and its `VehWpns.txt` guns; it only appears where a level's scene or script places it.

## Soldier types (`.unit`)

```
# Mods\ToughGuys\conscript.unit - every Iraqi conscript gets 333 health
id = Iraqi_Conscript
based on = Iraqi_Conscript
health = 333

# a new type (placed by a level's scene / script)
id = Iraqi_Sniper
based on = Iraqi_Conscript
experience = Veteran
item = US_WPN_HKPSG1, 10
item = MediKit, 1
```

| Key | Units.txt column (1-based) | Notes |
| --- | --- | --- |
| `id`, `based on` | 1 | `based on` = the same id changes the game unit in place |
| `type` | 2 | `SOLDIER`, `CIVILIAN`, `GOAT HERDER`, `EMIR`, `MISSILE EXPERT`, `POW` |
| `model` (SAS body), `delta model` | 3, 4 | an `.evo` in the game or a mod (a soldier body on the soldier skeleton) |
| `health` | `HStat.txt` column 2 | a copy of the unit's base stats row (named `<ID>`, or `<ID>_MOD` for an in-place change) with this health – other units sharing the game's row keep theirs |
| `stat column N` | `HStat.txt` column N | the same copy |
| `experience` | 11 | |
| `voice` | 12 | speech bank (`Bradley.sbk` …) |
| `item = X[, amount]` | `UnitWpns.txt` | the kit, one line per item (`item = none` = nothing). Without `item` lines a new unit copies the base unit's kit and an in-place change keeps the unit's own. A bigger kit for units the shipped levels place is fine (level octrees are rebuilt – see [Tables](#tables--rows-added-replaced-or-removed)) |
| `column N` | N | any other column (15 = item slots comes from the kit) |

## Report lines (tables, texts, weapons, units)

```
Mods: 3 text(s) from MyMod\menus.strings
Mods: model US_WPN_M16A2_MOD = Rifle02_M16wM203 with 1 texture slot(s) changed
Mods: game weapon US_WPN_M16A2 changed (RedM16\m16.weapon)
Mods: weapon US_WPN_TEST_PISTOL (MyMod\pistol.weapon) based on US_WPN_DesertEagle, model Pistol01_DesertEagle, sound bank DESERTEAGLE
Mods: game unit Iraqi_Conscript changed (ToughGuys\conscript.unit)
Mods: WEAPS.TXT - 2 key(s) replaced
Mods: WEAPS.TXT = game rows + 1 mod part(s)
Mods: Units.txt Bradley - item slots 6 -> 7 (its UnitWpns.txt rows)
[fail] Mods: UNITWPNS.TXT row "Ghost_Unit,US_WPN_M16A2,WEAPON,1" left out - unit "Ghost_Unit" isn't a Units.txt row
[fail] Mods: UNITS.TXT row "Bad_Unit,…,Superhuman,…" left out - unknown experience "Superhuman"
[ok]   Mods: 4 table(s) merged, 4 text(s) (+ 1 for single levels), 2 .weapon, 0 .vehicle, 1 .unit file(s)
[fail] Mods: GoldM16\m16.weapon - hud icon "GoldM16": no such picture file (.png / .dds / .tga) in a mod or game picture name - US_WPN_M16A2's picture is used
[fail] Mods: GoldM16\m16.weapon - picture …\Mods\GoldM16\GoldM16.png not loaded (PNG / DDS / TGA up to 1024 x 1024) - the IMAGE_WEAPON_M16M203 picture is used
Mods: served Weaps.txt from MyMod (…\DesertStorm2Fix\merged\A1BD7858\WEAPS.TXT)
```

## Levels and missions

Sub-plugin `ds2fix.modlevels` (on with `FeatMods`). Port of Desert Storm's mission-DLL and "game data by reference"
support, so a level mod ships **no game files** - only its own files and changes against the player's install.

### Mission DLLs from a mod

A level's script is `<Level>.dll` (`Mission01.dll` … `Training04.dll`). A DLL with that name in a mod folder is loaded
from the mod instead of the game folder - a changed copy of a shipped mission, or a new mission with a new name
(`MyOp.dll`, started like any level, e.g. from the campaign of a future mission list or the dev `StartLevel`).

Don't ship a changed copy of a game DLL - ship the change as a **`.dllpatch`**: text, built into the full DLL from the
player's own copy at start-up. Every original byte is checked; a DLL of another version is refused (`[fail]` in the
report) and the game's own DLL is used.

```
# Mods\MyMission\Mission01.dllpatch  ->  Mission01.dll
base Mission01.dll            # the shipped DLL in the game folder
base-size 110592              # its size (another version = refused)
size 110592                   # optional: the result's size (bigger = bytes appended)
@ 15c3f 5361627265 4d6f644c76  # @ <file offset hex> <original bytes hex | - past the base's end> <new bytes hex>
```

The patch's file name is the DLL it makes: `MyOp.dllpatch` with `base Mission01.dll` = a copy of Mission 1's script
named `MyOp.dll`. The ModKit writes these (`conflict_ds_tools/gamerefs.make_dll_patch`, the editor's DLL export).

### `<Level>.level` - which level data a mission uses

The game names a level's data after the level: `<level>.dat` (archive), `<level>.sch` (sounds), `<level>Music.sbk`,
`<LEVEL>.FAS` (physics pool sizes). A new mission DLL on a shipped map says which level's data it uses in a text file
next to it, named like the DLL:

```
# Mods\MyMission\MyOp.level
--@archive mission01          # mission01.dat + mission01.sch + mission01Music.sbk + MISSION01.FAS
```

`--@archive none` = no level archive at all (the level's files are loose in the mod). The scene, routes and walk map
are named after the scene the DLL loads, not after the level.
The level's texts (`<LEVEL>.BLF`) come from that level too (no BLF of its own = no texts at all in the game's menus).

A level whose name isn't in the game's campaign table also gets its scene's script info (`<scene>.si`, per-level flags
the mission script switches) - the game itself reads it only for campaign levels, and Mission 1's script crashed
without it.

### Game data by reference: `.uses`, `.envpatch`, `.rmdpatch`

Built from the player's own archives (any `*.dat` of the install) into `%TEMP%\DesertStorm2Fix\merged\game` at start-up
(and by the MODS menu's Apply) and then served like the mod's own files. A real file of the same name in a mod wins.

| File | Makes | Content |
| --- | --- | --- |
| `<name>.uses` | the files it names | `file X` = the game's own X under the same name; `file A = B` = the game's B under the name A; `piece KIT_X = MODEL.EVO <n>` = object n of a level-piece model as a one-object prop `KIT_X.EVO` (ModKit `kits.py`); `#` comments. `bank X` = the game's sound bank X (from whichever level cache has it, with its samples) added to every level, so a level that lacks it can play it (see [Sounds](#sounds)) |
| `<NAME>.envpatch` | `<NAME>.ENV` | `base MISSION01.ENV`, then hunks `@ <start> <delete> <insert>` (0-based line of the base scene) followed by the inserted lines (ModKit `gamerefs.make_env_patch`; the editor's Save as mod writes it) |
| `<NAME>.rmdpatch` | `<NAME>.RMD` | `base MISSION01.RMD`, `count <n>`, then `<i> x y z l0 l1 l2 l3 l4 flags weight` for every route marker that differs from the base's marker i (Desert Storm II route files, version 3) |

Report lines: `Mods: MISSION01.ENV = MISSION01.ENV + MISSION01.envpatch (MyMod)`, `Mods: MyMod uses 3 game file(s) from
your install`, `[fail] Mods: MyMod: X.envpatch doesn't fit this game: hunk "@ …" doesn't fit MISSION01.ENV`,
`[fail] Mods: MyMod uses NOT_THERE.DDS, which isn't in this game's data`.

### Changed scenes: octree and AI navigation

A scene a mod changes (a `<SCENE>.ENV` in a mod or built from an `.envpatch`) can't use the shipped `<SCENE>.OCT`: the
octree indexes the scene's objects by their place in the scene, so after an added prop or soldier it draws and collides
with the wrong objects (in Mission 1: nothing of the world drawn, the intro stuck letterboxed for good). The plugin
builds the octree from the changed scene while the level loads (Mission 1: 47 ms; written only to `%TEMP%`, never to
the game folder) - unless the mod ships its own `<SCENE>.OCT`.

The AI's walk map (`.QTD`) and route markers (`.RMD`) are rebuilt the same way when the mod ships neither a `.QTD`, a
`.RMD` nor an `.rmdpatch` for the scene: the game builds the walk map and one route marker per walkable cell from the
changed scene (objects you add block the AI's paths), and the level's hand-placed route markers (the first ones in the
shipped route file - mission scripts use them by number) are put back in front, linked to the new ones. Log line:
`Mods: AI navigation for the mod scene MISSION01 built from the scene: 918 markers + 426 hand-placed from MISSION01.RMD
(1 links lost), …`. Ship a `.rmdpatch` (or `.RMD`) when you edited the route network yourself.

**Adding objects to a mission scene**: append them (the editor does) - existing objects keep their places and script
ids; give a new soldier / vehicle a script id no other object has. With the rebuilt octree an added soldier no longer
stops Mission 1 (that was the stale octree, not the mission script).

## Lua missions and custom campaigns

Sub-plugin `ds2fix.scripthost` (on with `FeatMods`; standalone `dinput8.dll` too). Desert Storm's Lua mission scripts
for Desert Storm II: a level whose name ends in `.lua` runs a Lua 5.4 script instead of a `MissionNN.dll`, and
`campaign.txt` chains such levels into a campaign on the SINGLE PLAYER menu. The `game.*` API is Desert Storm's where
DS2 has the same function (differences below) - the Desert Storm pages on Lua missions apply otherwise.

```lua
-- Mods\MyOp\myop.lua
--@archive mission01            -- mission01.dat / .sch / Music.sbk / MISSION01.FAS (no line: <name>.dat if there is one, else none)
local obj
function init()                 -- the level is loading (as a mission DLL's LevelSetup)
    game.load_sound_bank("Mission01.sbk")
    game.load_strings("M01Strs.loc")
    game.load_scene("Mission01.env")
    game.load_map_image("GWMission1.img", 0)
    game.set_level_bounds(-24940, 0, -20840, 22940, 0, 27040)   -- min x y z, max x y z (cm)
    game.set_mission_index(0)
    obj = game.add_objective("REACH THE BRIDGE", "Get the squad to the bridge")
    game.show_objective(obj)
    game.timer(1, 30)                                            -- seconds
end
function update() end           -- every frame while the mission runs
function on_message(id, a, b, c, d, e, param)
    if id == game.EVENT.TIMER and a == 1 then
        game.complete_objective(obj)
        game.end_mission(2)    -- 2 success, 1 failure
    end
end
```

Header lines (first 2 KB): `--@archive <level>` / `--@archive none` (the level's data, as a DLL's `<Level>.level`),
`--@base Mission01.dll` = a shipped (or mod) mission's own logic runs first - its intro, AI, objectives, triggers - and
the script adds to it (callbacks: the DLL's, then Lua's), `--@scene X.ENV` = the scene the base mission loads instead of
its own (default: `<name>.ENV` when a mod has it, else the base's). With a base the level also gets the base scene's
`.si` (the mission's objectives / areas - its script uses them by number).

Starting one: from a campaign (below), from another level (`game.set_next_level`), `CDS II.exe +level myop.lua`
(launcher Play pressed by itself; the ModKit editor's Play), or the dev `StartLevel`.

| `game.` | What |
| --- | --- |
| `call(slot, ...)` / `callf(slot, ...)` | any script API function by its table offset (DS2's table: ModKit `editor/games/ds2/api_slots.json`) - integer / float result |
| `log(...)`, `print(...)` | DesertStorm2Fix's log (`[script] name: ...`) |
| `read32 / readf / read8(address)` | game memory (nil if unreadable) |
| `load_scene`, `load_strings`, `load_sound_bank`, `load_map_image(file, image)`, `set_level_bounds(min, max)`, `set_mission_index(i)`, `anim(state, file)`, `set_time_of_day(h)` / `get_time_of_day()`, `set_briefing_text(hash, slot)` | level set-up (API +0x70, +0x80, +0x34, +0x74 / +0x78, +0x7C, +0x590, +0x04, +0xA4 / +0xA8, +0x5D8) |
| `add_objective(title[, description])` -> index, `show_objective`, `complete_objective`, `hide_objective`, `reopen_objective`, `objective_flags`, `attach_objective(i, id)`, `objective_marker(i, x, y, z)` | the objectives screen / compass list; title / description = text (a string) or a text hash |
| `end_mission(result)`, `set_next_level(name)`, `next_mission()` | 2 success / 1 failure; after a success the end screen's Continue goes to `set_next_level`'s level, else the campaign's next mission, else the main menu |
| `timer(id, seconds[, repeating])`, `stop_timer(id)` | `on_message(game.EVENT.TIMER, id)` |
| `activate(id)`, `deactivate(id)`, `layers` + `show_layer` / `hide_layer`, `set_team(id, team)` | objects by script id |
| `object(id)`, `position(id)`, `health(id)`, `distance(id, x, y, z)`, `player_soldier(n)`, `player_position(n)` | positions in cm |
| `regions` + `in_region`, `player_in_region` | spheres / boxes; `on_message(game.EVENT.ENTERED / LEFT, name, player)` |
| `state` | saved with the game (numbers, strings, booleans, tables of those; 64 KB); `loading_save()` in `init()` |
| `text(name, text)` -> hash, `popup(text[, seconds])` | own texts (any language); the message bar |
| `allow_pause(on)`, `time_ms()`, `float(bits)` | |

Messages: `on_message(id, a, b, c, d, e, param)` gets every message the game sends a mission script - **DS2's message
ids are not Desert Storm's**. Known: `COMPLETE` 0x11 (the level's `.si` says the mission is complete) and `FAILED` 0x12
(squad dead ...) - a `.lua` level without a base ends the mission on these unless `on_message` returns `false`; `TIMER`
0x16; `OBJECTIVE_DONE` 0x4F (a base mission's objective). Queries the game asks (it reads the answer back) get Mission
1's own answers by default; return an integer / boolean to answer yourself.

Plugin Lua functions (pse.lua, PSE build only): every namespace a plugin registers is there in a `.lua` level.

### Custom campaigns

```
# Mods\MyCampaign\campaign.txt
name = Operation Sandstorm
mission = sandstorm1.lua
mission = sandstorm2.lua
```

SINGLE PLAYER shows **CUSTOM CAMPAIGNS** while a switched-on mod has a `campaign.txt` (up to 7 campaigns). Picking one
opens the game's DIFFICULTY screen; the difficulty starts the first mission. Each mission's success continues with the
next (`set_next_level` overrides), the last one returns to the main menu, a failure too.

Report / log lines: `Campaign: OPERATION SANDSTORM (MyCampaign, 2 missions)`, `[fail] Campaign X: campaign.txt lists no
mission`, `[fail] script myop.lua: run: ...` (Lua errors with a traceback; the level then runs without its script).

## Sounds

Sounds live in the level sound caches next to the exe (`mission01.sch` … `training04.sch`, `FrontEnd.sch`): one chunk
file per level with the **banks** (a named set of sounds: a gun's shots, a hero's voice lines, an engine …) and the
**samples** they play (4-bit ADPCM; speech once per language). The format is Desert Storm's, so the ModKit's
`cli/ds_sound.py` works on DS2's files as they are:

```
python ds_sound.py list "<game>\mission01.sch"                     banks (by key) and their samples
python ds_sound.py wav "<game>\mission01.sch" M16A2 out             a bank's samples as WAV (guns: click, tail, shot1, shot2)
python ds_sound.py pack "<game>\mission01.sch" AK47 MyPack.sch --rename MYBANK     a bank + its samples as a pack
python ds_sound.py stream "<game>\Data\Sound\PC_FrontEndMusic.psf" music.wav       the front-end music as WAV
python ds_sound.py psf my_music.wav PC_FrontEndMusic.psf           WAV -> stream (44.1 kHz stereo)
```

Bank names are what the tables call them: a weapon's in `WEAPS.TXT` **column 68** (`M16A2`, `SAW-LIGHTMG`, `AK47`,
`MP5SILENCEDSUBMG` …; DS1 had it in column 64), the squad's voices `Bradley` / `Foley` / `Jones` / `Ramirez`. The game
adds `.sbk` and keys the bank by the ELF hash of that name (`sound.bank_key` in the ModKit).

| Kind | File in a mod folder | What happens |
| --- | --- | --- |
| Sound pack | `<any name>.sch` that no level uses | Every bank and sample in it that a level's cache doesn't have yet is added to **every level** (the front end too). Make one with `ds_sound.py pack` (`--rename` gives the bank its own name, so a weapon row can point at it) |
| Level sounds | `<LEVEL>.SCH` named like the game's (`MISSION01.SCH`, `FRONTEND.SCH`) | Replaces that level's banks (same key), samples (same language + id) and music entries; a sample of **language -1** replaces that id in **every language** (one WAV for a speech line in all languages). Chunks the level doesn't have are added. Only what is in your file changes - start from `ds_sound.py wav` of the bank, edit the WAVs, and build the file with the ModKit's `sound.sample_chunk(pcm, rate, id)` + `sound.build([...])` |
| Front-end music | `PC_FrontEndMusic.psf` | A plain stream, any length (`ds_sound.py psf` from a 44.1 kHz stereo WAV) |
| Mission music | `PC_Mobeus.psf`, `PC_Sandstorm.psf`, `PC_FirstLight.psf`, `PC_Firefight.psf`, `PC_UmmQasr.psf`, `PC_HouseToHouse.psf`, `PC_Win.psf`, `PC_Lose.psf` | Interactive music: the level cache holds a table of segments pointing into the file, so a replacement must keep **exactly the game file's size and segment layout** (each segment re-encoded to its old length); another size is refused with a `[fail]` line and the game's file plays. No ModKit tool for these yet |
| Films | `<film>.bik` with the game's name (`Logo1PAL.bik` … `Logo5PAL.bik`, `AmodePAL.bik`, briefings `B01PAL.bik` … `B10PAL.bik`, `F01PalUk.bik` / `F01PalUs.bik` … (SAS / Delta), `FINPALUK.bik`, `FOuPALUS.bik` …) | The mod's Bink file plays instead (sound included) |
| Weapon sounds | `.weapon` `shot sound = x.wav` / `tail sound = x.wav` | The plugin builds a new bank from the weapon's current one with your WAV(s) encoded in, added to every level |
| Game bank by reference | `.uses` line `bank X` | Bank X of the game (e.g. `SAW-LIGHTMG`, which Mission01 doesn't have) is copied out of the first level cache that has it and added to every level - for editor mods that use another level's sounds without shipping them; an unknown name = `[fail]` |
| Bank file | `<NAME>.sbk` | Only read when no cache has a bank of that name; it is the original tool's bank file (starts with `V` + a version; nothing ships one) - use a `.sch` pack instead |

WAV files: PCM 8 / 16 / 24 / 32-bit or 32-bit float, any channel count (mixed to mono for samples), up to 65 535 Hz;
the game's own samples are 11-22 kHz mono. DS2 has no streamed speech (DS1's cutscene `CS_*.psf` / objective `OBJ_*.psf`
streams don't exist here): the squad's lines are samples in every level cache, replaced with a level `.SCH`; cutscenes
are Bink films with their sound inside.

Merged caches go to `%TEMP%\DesertStorm2Fix\sound\` and the game reads them from there; a level whose sounds no mod
changes reads its own file. A pack or level file the game couldn't read is refused before the game sees it (the game
would quit with "Cache file … seems to be corrupt"). Changes from the MODS menu apply at the next level load (caches are
read per level, the front end's when the menus load again). Report lines:

```
Sounds: ZZPACK.sch (MyMod) - bank pack, added to every level
Sounds: MISSION01.SCH (MyMod) - sounds of MISSION01.SCH replaced
Sounds: MISSION01.SCH - 7 sound(s) replaced, 0 added from 1 mod file(s)
Sounds: MISSION01.SCH + 2 bank(s) from packs, 31760 bytes (…\DesertStorm2Fix\sound\mission01.sch)
Sounds: stream PC_FRONTENDMUSIC.PSF -> …\Mods\MyMod\PC_FrontEndMusic.psf
Sounds: film Logo1PAL.bik -> …\Mods\MyMod\Logo1PAL.bik
[fail] Sounds: BAD.sch (MyMod) - unknown chunk (only BANK, PFSM, PFST, IMUS), not used
[fail] Sounds: …\PC_Lose.psf is 2928476 bytes, the game's PC_Lose.psf 2928508 - mission music (interactive, in segments) must keep the game file's size and layout; the game's own is played
```

## Size limit

The game reads textures (and some other files: models, animations, scenes) whole into a 1 000 KB file heap (the
"Global Heap"); a file that doesn't fit isn't loaded (a texture just stays missing). DesertStorm2Fix makes that heap
bigger by the largest file in the Mods folder (switched-off folders included, so a mod switched on in the MODS menu fits
without a restart; rounded up to 1 MB), so a 2048 x 2048 DXT texture (5.3 MB with mipmaps) is fine:

| | Most the heap grows by | Texture files bigger than the heap |
| --- | --- | --- |
| `ds2fix.mods` alone (no `ds2fix.memory.dll`) | 64 MB | not loaded; a bigger file added while the game runs needs a restart (the menu says so); over 64 MB = `[fail]` in the report |
| with the plugin `ds2fix.memory` (in the package), 2 GB | 64 MB (and at most a quarter of the largest free block of address space) | read outside the heap (through the game's own allocator) - any size that fits in memory, no restart |
| with `ds2fix.memory` and the 4 GB patch (`DesertStorm2Fix-Patcher.exe`) | 256 MB | the same, with twice the address space |

Models (`.evo`), animations and other non-texture files must still fit the heap. Keep textures DXT-compressed and at
most 2048 x 2048 for sane loading times and video memory (the game has 2 GB of address space for everything unless the
4 GB patch is on). The log (`PSE\Logs\loader.log`, `DesertStorm2Fix.log` in dev builds) has a `Memory (<level>):`
line per level load: address space used, largest free block, heap use / peak, texture memory.

## The report

`Mods\DesertStorm2Fix-report.txt` is written at every start and again by the MODS menu's Apply (also in release builds,
which have no log):

```
Mods: PinkRover - 5 file(s)
[ok]   Mods: 1 mod(s), 5 file(s)
Mods: file heap 1024000 -> 3121152 bytes (largest mod file SAS_LAND_ROVER_1.DDS, 1048704 bytes)
Mods: file heap sized by ds2fix.memory           (instead of the line above when the plugin is installed)
Mods: served SAS_Land_Rover_1.dds from PinkRover (…\Mods\PinkRover\textures\SAS_LAND_ROVER_1.DDS)
```

- `served X from <mod>` – the game asked for `X` and got the mod's file (one line per file and run).
- `<mod> replaces <other mod>'s X` – two mods ship the same name; the later one is used.
- `switched off` – a `_` / `.` folder that was skipped.
- `[fail] …` – a problem (a path too long, a file too big, game code not recognised).

Getting the game's files to start from: the ModKit (`ConflictDesertStormModKit`, `conflict_ds_tools/archive.py` /
the Blender add-on's archive extract) exports the `.dat` archives by name. Don't redistribute the game's own files –
ship only what you changed.

Implementation notes (addresses, hooks): [reference/game-ds2.md](reference/game-ds2.md) "Mods folder".

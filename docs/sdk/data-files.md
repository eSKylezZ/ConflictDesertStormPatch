# PSE data files and records

PSE stores data - records, mod edits, mission data - as **Lua tables with literals only**. A data file is plain Lua
that any Lua reader accepts, but nothing in it runs: PSE reads it with its own parser, which refuses everything that
isn't a literal. Logic stays in ordinary `.lua` scripts.

Reader / writer: C++ `pse/records/data.h` (`pse::data::Parse` / `Write`), Python `conflict_ds_tools/pse_data.py` in the
ModKit (`loads` / `dumps` / `canonical`). Both write **the same bytes** for the same content; `scripts/records_gate.py`
checks that on thousands of generated files. Command line: `pse_build data canon <file> [<out>]`,
`pse_build data check <file>` (exit 0 = already canonical).

## The format

```lua
-- Comments before "return" are the file's header.
return {
  id = "ds1:weapon/us_wpn_m16a2",
  -- a comment on its own line belongs to the entry below it
  damage = 14,
  range = 850.0,
  silenced = false,
  model = "Rifle01_M16A2",
  fire_modes = { 1, 4 },
  ["a key that isn't a name"] = "ok",
  [3] = "integer keys are fine",
}
```

Allowed: tables, strings (`"..."`, `'...'`, `[[...]]` with Lua's escapes), integers (decimal or `0x` hex, 64-bit),
floats, `true` / `false`, and `-` in front of a number. Keys: `name =`, `["string"] =`, `[integer] =`; values without a
key are the table's list part. Refused, with `line:column`: names, calls, operators, `nil`, `...`, duplicate keys, an
integer key that overlaps the list items.

Comments: full-line comments (`--`, `--[[ ]]`) in front of an entry stay with that entry; comments after a value on
the same line, or after the last entry of a table, are dropped.

## Canonical output

The editor and every tool save files in one fixed form, so a saved file only changes where its content changed:

- header comments, then `return` and the table; `\n` line endings; a final newline;
- list items first in their order, then keyed entries: `id` first, integer keys ascending, then names in byte order;
- a table goes on one line (`{ 1, 2, 3 }`, `{ x = 1, y = 2 }`, `{}`) when it has no comments, holds only plain values
  and the whole line fits in 100 bytes; otherwise one entry per line, two spaces per level, `,` after every entry;
- strings in `"..."`: `\\` `\"` `\n` `\r` `\t` escaped, other control bytes as `\ddd`, everything else (UTF-8) as is;
- integers in decimal; floats the shortest text that reads back to the same number (`0.1`, `1.0`, `1e+16`,
  `1.5e-05` - scientific below 1e-4 and from 1e16).

## Records (Desert Storm)

The game's data tables (`Weaps.txt`, `Units.txt`, `Vehicle.txt` ... in `catalog.dat`) become records with stable,
namespaced IDs: `ds1:<type>/<key in lower case>`, for example `ds1:weapon/us_wpn_m16a2`, `ds1:unit/bradley`,
`ds1:vehicle_stats/tankstat_m1a1`. IDs never depend on load order and never shift.

| Type | From | One record per |
| --- | --- | --- |
| `weapon`, `item`, `unit`, `unit_stats`, `experience_level`, `vehicle`, `vehicle_stats`, `propulsion`, `feature`, `feature_stats`, `structure`, `door`, `switch`, `target` | `WEAPS`, `INVEN`, `UNITS`, `HSTAT`, `EXMODS`, `VEHICLE`, `VSTAT`, `PROPS`, `FEATS`, `FEATSTAT`, `STRUCTS`, `DOORS`, `SWITCHES`, `TARGETS` | table row |
| `unit_kit`, `unit_skills`, `vehicle_weapons`, `role_gear`, `mp_kit` | `UNITWPNS`, `HSKILLS`, `VEHWPNS`, `CONNOBJS`, `MPSKINITEMS` | key, with its rows in `rows = { ... }` |

A record has `id`, `key` (the game's own spelling) and named fields; links to other records are their IDs
(`stats = "ds1:unit_stats/us_infantryman00"`). Columns whose meaning isn't known yet are `colN` (N = the 1-based
column, as in a `.weapon` file's `column N`). A text or link cell holding the game's "nothing" (`-1`) is left out.

How each table reads is described by the schema `pse/records/ds1/schema.lua` (column names, kinds `int` / `fixed1` … `fixed9` (a float written with that many decimals) /
`yesno` / `text` / `ref`, link targets, a `note` with the meaning and unit, and limits: `max_len` on a text / link
column = the most characters the game's loader takes, `max_rows` on a table = the most records it keeps). The records are made **from the player's own files** (they are game data and
are never shipped) and turn back into the game's tables byte for byte - checked for all 19 tables.

## Placed references (what a level puts in its world)

Each level's scene (`<LEVEL>.ENV`) becomes a list of references with **editor IDs** - the same names the readable
mission scripts use (`obj.HERO_FOLEY`, `wp.CELL_BLOCK_GUARD_0`, `rgn.PICKUP_BLACKHAWK_TRIGGER`):

| Kind | ID | Fields |
| --- | --- | --- |
| object | `ds1:mission1/object/hero_foley` | `base` (its record: `ds1:unit/foley`, `ds1:vehicle/bmp1` ...), `editor_id`, `script_id`, `pos` / `rot` `{ x, y, z }`, `in_play`, `scene = { ... }` (the scene record's other fields as they are) |
| waypoint | `ds1:mission1/waypoint/cell_block_guard_0` | `editor_id`, `number`, `pos`, `rot`, `wait_ms`, `next` (the next waypoint's ID: the chain a patrol walks) |
| region | `ds1:mission1/region/pickup_blackhawk_trigger` | `editor_id`, `index`, `shape` (sphere / box), `pos` and the shape's fields |

All 18 levels: 6 866 references, every object linked to its base record. ModKit: `conflict_ds_tools/placed.py`,
`cli/ds_records.py -o <folder> --levels` (also `--check`: the tables' round trip; `--merged`: the tables with the
mods' record edits).

**Changing placed references** (ModKit `placed.write_envpatches(game_dir, mod_dir, edits)`, 2026-10-21): the same edit
syntax as table records, ids of one level's placed references:

```lua
return {
  { id = "ds1:mission3/object/cs_hero_foley", pos = { x = 26467.0, y = 807.55, z = 62020.6 } },
  { id = "ds1:mission3/object/cs_hero_jones", delete = true },          -- taken out of play
  { id = "ds1:mission3/object/my_extra_guard", based_on = "ds1:mission3/object/cs_hero_foley", in_play = true },
  { id = "ds1:mission3/waypoint/goat03_01", wait_ms = 7777 },
}
```

- Objects: `pos`, `rot`, `in_play`, `base` (a record id: the object's unit), `scene = { field = value }`; `based_on` makes
  a copy with the next free script id - or `script_id = n` if that one is free (mission logic refers to objects by
  it; a taken one gives the next free id and a `[fail]` line) -, its editor ID becomes its scene name. `delete` takes an object out of play - its
  line stays, because the game's precomputed level data refers to scene objects by their place in the file.
- Waypoints: `pos`, `rot`, `wait_ms`, `next` (a waypoint id or -1). Regions: `pos` and the shape's fields.
- The level part of an id ignores case (`ds1:Mission1/...` = `ds1:mission1/...`); the editor ID part doesn't.
- **In the game**: put the edits in `Mods\<Mod>\records\*.lua` like table records - DesertStormFix applies every
  switched-on mod's placed edits at start-up (mod order, files by name) to the level's scene: a mod's own
  `<LEVEL>.ENV` or one a `.envpatch` builds if there is one, else the game's. The result is
  `%TEMP%\DesertStormFix\merged\game\<LEVEL>.ENV`; the mod report lists copies, objects taken out of play and every
  refused edit (`[fail] MyMod/edits.lua: ds1:mission1/object/x pos can't be set: ...`). Two mods giving the same field
  different values: the later mod wins and the report says so, as for table records
  (`conflict: ds1:mission1/object/base_guard_1 pos: ModA / ModB (ModB wins)`; a `scene = { ... }` part counts as
  `scene.<field>`, `delete` as an `in_play` value).
- A level whose scene comes from the mods (placed edits, a `.envpatch`, a mod's own `<LEVEL>.ENV`) gets its culling
  octree rebuilt from that scene at load (~50 ms; written to `%TEMP%\DesertStormFix\rebuilt-<SCENE>.OCT`, never over
  the game's), unless a mod ships the level's `.OCT` too. Before 2026-10-23 the shipped octree was used, and objects
  the edit added (copies of props) were in play but never drawn.
- Or ship the changes as a `.envpatch`: ModKit `placed.write_envpatches(game_dir, mod_dir, edits)` writes the level's
  `<LEVEL>.envpatch` (only the changed lines, no game data). Both can be used together - the record edits apply to the
  patched scene. One catch: an edit names the object by its editor ID in *that* scene, so if a patch renames an
  object's stat name, an edit using the old name gets a `[fail]` line.

### Several mods, one level

Like plugins editing the same cell in Fallout or Skyrim, any number of mods can change one level:

- **Record edits** (`records\*.lua`) merge field by field: one mod moving a guard and another changing his unit both
  apply; only the same field set to different values is a conflict (the later mod wins). This is the best way for
  objects, waypoints and regions - prefer it over a `.envpatch` for them. The Conflict Editor's `save_mod` does this
  by itself (ModKit 1.8+): changed and copied objects, waypoints and regions go to `records\<level>.placed.lua`, only
  the rest to `<LEVEL>.envpatch`; `open_mod_level` opens the level as the mod has it.
- **`.envpatch` files** of the same level from several mods are merged line by line before the game reads the scene:
  changes to different lines all apply; objects several mods add at the same place (the end of the object list) are
  all kept, in mod order; two mods changing the same lines - the later mod's change wins
  (`conflict: MISSION1.ENV lines 2036-2036: ModA / ModB (ModB wins)`). A patch of another base scene can't be merged:
  the later mod's patch is used alone. The report lists the merge (`MISSION1.ENV = MISSION1.ENV + MISSION1.envpatch
  (ModA) + MISSION1.envpatch (ModB)`).
- **Script ids**: two patches that each add an object with the same new script id give the level two objects with
  one id - the report has a `[fail]` line for it. A record edit's `based_on` copy takes the next free id after every
  patch, so it never collides.
- **`.rmdpatch` files** (route markers) merge per marker the same way; the marker count is the largest.
- A mod that ships the whole `<LEVEL>.ENV` (or `.RMD`) replaces the level's file: the other mods' patches of it are not
  used (the report says so); their record edits still apply on top.

## Changing records from a mod

A mod puts record edits in `Mods\<Mod>\records\*.lua` (data files; any names). Each file is a list of edits:

```lua
return {
  -- change fields; every other field keeps the game's (or an earlier mod's) value
  { id = "ds1:weapon/us_wpn_m16a2", magazine_size = 60, damage = 16 },
  -- a new record: a copy of another one with changes (key = the name the game's tables use; default: the id's
  -- name in upper case)
  { id = "ds1:weapon/my_rifle", based_on = "ds1:weapon/us_wpn_m16a2", key = "MY_RIFLE", damage = 30 },
  -- remove a record
  { id = "ds1:feature/nu_prop_crate5#2", delete = true },
  -- set fields back to the game's "nothing" (only fields that have one, e.g. a model column that can be -1)
  { id = "ds1:item/medikit", clear = { "model" } },
}
```

- Mods apply in load order (folder names, case-insensitive; folders starting with `_` or `.` are off), each mod's
  files by name.
- **Field by field**: one mod changing a weapon's damage and another changing its range both apply. Two mods giving
  the *same* field *different* values is a conflict - the later mod wins and the mod report says so:
  `Records: conflict: ds1:weapon/us_wpn_m16a2 damage: ModA / ModB (ModB wins)`.
- A list field (the `rows` of a kit, a vehicle's weapons) is one field: set the whole list.
- Problems (an unknown id, a new record without `based_on`, a field that can't be cleared, a file that isn't a data
  file) are `[fail]` lines in `Mods\DesertStormFix-report.txt`; the rest of the edits still apply.
- **Limits** (the schema's `max_len` / `max_rows`): the game reads text fields into fixed-size buffers, so a longer
  value would overwrite memory. A field set longer than its column allows isn't changed
  (`[fail] MyMod/edits.lua: ds1:unit/bradley speech_bank is 44 characters - the game reads at most 35; not changed`);
  a new record whose key is too long, or one added to a full table, isn't added
  (`... ds1:weapon/my_rifle not added - its key is 40 characters, the game reads at most 35`). A link given as a
  record ID is that record's key, so only a link written as a plain name is measured. On top of that, DesertStormFix's
  own table checks (unit / vehicle tables: column counts, unknown types, rows the game would drop) also run on tables
  only record edits changed.
- The changed tables are what the game loads, and they are the base for `.txt` / `.replace.txt` table parts and
  `.weapon` / `.unit` files of other mods.
- A mod never ships game data: the records are made from the player's tables at start-up (the schema
  `PSE\Records\ds1\schema.lua` comes with DesertStormFix).

Status (2026-10-20): format, schema, generator, field-level merge and the game loading the merged tables are built
(C++ `pse/records/forms.h` in DesertStormFix, the same in the ModKit's `conflict_ds_tools/forms.py`; both give the same
bytes - `scripts/records_forms_gate.py`). Column names (2026-10-20), placed references with editor IDs and the game reading placed edits (2026-10-22; C++
`pse/records/placed.h` = Python `placed.py`, `scripts/records_placed_gate.py`) are done; the
limit checks are built and the schema has `max_len` for every text / link column the game reads (2026-10-21, from
the loaders' buffers: 35 characters for most names, 34 for models, connectors and the units' speech bank, 31 for sound
banks and the weapons' reload / throw animations, 33 for the aim animation, 15 for a weapon's movement model). No
table has a fixed row count; the per-owner limits (25 items per unit, 6 gear pieces per role and faction, 25 MP kit
items) are checked by the game when it loads.

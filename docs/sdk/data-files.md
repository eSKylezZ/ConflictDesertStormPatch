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

How each table reads is described by the schema `pse/records/ds1/schema.lua` (column names, kinds `int` / `fixed2` /
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
mods' record edits). Placed references are read-only for now - changing them comes with the editor.

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
bytes - `scripts/records_forms_gate.py`). Column names (2026-10-20) and placed references with editor IDs are done; the
limit checks are built and the schema has `max_len` for every text / link column the game reads (2026-10-21, from
the loaders' buffers: 35 characters for most names, 34 for models, connectors and the units' speech bank, 31 for sound
banks and the weapons' reload / throw animations, 33 for the aim animation, 15 for a weapon's movement model). No
table has a fixed row count; the per-owner limits (25 items per unit, 6 gear pieces per role and faction, 25 MP kit
items) are checked by the game when it loads.

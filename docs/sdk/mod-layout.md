# Mod folders – layout and load order (all six games)

How the patches find a mod's files, how a mod should arrange them, and which mod wins when two ship the same file.
Applies to DesertStormFix (DS1), GreatEscapeFix (TGE), DesertStorm2Fix (DS2), VietnamFix (CV), GlobalStormFix (CGS) and
DeniedOpsFix (DO).

## Where files are found

`<game>\Mods\<mod name>\...` - every file under a mod's folder, **at any depth**, is found by its bare file name (upper
case, folder ignored): `Mods\MyMod\evos\props\CRATE.EVO` replaces the game's `CRATE.EVO` exactly like
`Mods\MyMod\CRATE.EVO` does. A folder name starting with `_` or `.` switches the whole mod off.

## Organised layout (new mods)

New mods made with the ModKit (Blender add-ons, editor, porter, example builder) keep their files in folders by type.
Mods made before stay flat - all files in the mod's top folder - and work unchanged; the tools keep a flat mod flat and
rewrite an existing file where it already is.

| Folder | Files |
| --- | --- |
| `evos\` | models (`.evo`), vehicle data (`.dyn`), editor sources of generated models (`.road.json`, `.terrain.json`) |
| `dds\` | textures (`.dds`, `.tga`, `.png`, `.bmp`), image sheets (`.img`, e.g. minimaps) |
| `anims\` | animations (`.prb`, Global Storm `.ani`) |
| `sounds\` | sound caches / bank packs (`.sch`), banks (`.sbk`), `.wav` sources, streams (`.psf`) |
| `tables\` | table parts (`.txt`, `.replace.txt`), texts (`.strings`) |
| `defs\` | `.weapon`, `.skin`, `.unit`, `.vehicle`, `.gear`, `.pouches` |
| `levels\` | scenes and their parts (`.env`, Global Storm / Denied Ops split parts, `.si`, `.rda` …), patches (`.envpatch`, `.rmdpatch`, `.uses`, `.dllpatch`), routes, octrees, `.lua` / `.dll` levels, `.level`, editor `.editor.json` |
| top folder | `campaign.txt`, readme, `records\`, `frames\`, anything else |

The ModKit's `conflict_ds_tools/modlayout.py` is the reference (`path_for`, `find`); the folders are only for people -
the games never look at them.

### Paths that must stay where they are

These are read from a fixed place, so they never move into the layout folders:

| Game | Path | Why |
| --- | --- | --- |
| DS1 | `MODNAME\records\*.lua` | record edits ([data-files.md](data-files.md)) are read only straight under the mod folder; a `.lua` anywhere else is a level |
| DS1 | `MODNAME\frames\*.png` | photo-mode frames |
| DS1, DS2, TGE | `MODNAME\campaign.txt` | custom campaigns are read from the mod's top folder |
| DS1 | `.psf` streams | the language is the folder the file is in: `SAS` / `UK` / `BRITISH` / `ENGLISH`, `DELTA` / `US` / `AMERICAN`, `FRENCH`, `GERMAN`, `SPANISH`, `ITALIAN`, `JAPANESE`, `POLISH`; any other folder = every language. In the organised layout: `sounds\SAS\CS_M01_S01.psf`, `sounds\CS_M01_S01.psf` |

## Load order – the mod priority list

When two mods ship a file of the same name, **the mod loaded last wins**. DS1 (2026-10-25): the order is the player's
mod priority list - the settings value `ModOrder` ([ds1-settings.md](ds1-settings.md)), every mod folder name (without
the `_`), in load order:

- the first start takes the folder names alphabetically (the order before the list existed);
- a new mod goes to the **end** (it loads last and wins); a mod that's gone drops off; switching a mod off keeps its
  place;
- the MODS screen numbers the mods in load order; **Left / Right** (pad: **L1 / R1**, D-pad left / right) moves the
  highlighted mod, and the change counts as one of the changes to apply;
- the mod report (`Mods\DesertStormFix-report.txt`) starts with `Mods: load order (the one loaded last wins a file
  name): …` and names every file a later mod replaces (`Mods: B replaces A's GUN.EVO`);
- online, both PCs must load the shared mods in the same order; a joiner takes the host's order with the host's mods
  ([online-compat.md](online-compat.md)).

The other five games still load mods alphabetically by folder name (the same "last wins" rule); their priority list
comes next.

### The same file name twice in one mod

DS1 (2026-10-25): inside one mod the same rule holds with a fixed order - folder by folder, names sorted
case-insensitively, the last one wins: `evos\GUN.EVO` and `old\GUN.EVO` -> `old\GUN.EVO` is used, and the report says so:
`[warn] Mods: MyMod has two files named GUN.EVO (evos\GUN.EVO, old\GUN.EVO) - old\GUN.EVO is used`. The other five
games use whichever the folder listing gives last (to be made the same). The ModKit never writes a second copy (it
rewrites the one the mod has) and lists duplicates (`modlayout.duplicates`).

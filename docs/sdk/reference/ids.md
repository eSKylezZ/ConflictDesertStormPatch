# pse.ids (version 1) – stable ids for game addresses

PSE's answer to SKSE's Address Library. Every game function, global, code site, vtable or data table a
plugin may need has a **numeric id** and a **name** (`ds1` id 167 = `Soldier_TakeDamage`). The game layer knows the
address of each id for every exe build it supports, checks each one at start-up against a short byte signature taken
from that build, and hands out the address for the exe that is actually running. One plugin binary then works on
every build the database covers (Steam, GOG, retail CD, other regions …) – and gets **0, never a wrong address**, where
it doesn't.

Header: [`pse/include/pse/pse_ids.h`](../../../pse/include/pse/pse_ids.h). Id constants per game (generated):
[`pse/include/pse/pse_ids_ds1.h`](../../../pse/include/pse/pse_ids_ds1.h). The DS1 list with descriptions:
[ids-ds1.md](ids-ds1.md). API 0 = unstable until SDK 1.0; the interface is versioned like the loader interfaces.

| Game | Database | Builds | Ids |
| --- | --- | --- | --- |
| `ds1` Conflict: Desert Storm | [`pse/ids/ds1.ids.txt`](../../../pse/ids/ds1.ids.txt), version 1 | `steam-gog-1.0` (SHA-256 `D712F5EC…`, Steam = GOG) | 468 (all published): 260 functions, 69 sites, 87 globals, 15 vtables, 37 data |
| `ds2` Conflict: Desert Storm II | [`pse/ids/ds2.ids.txt`](../../../pse/ids/ds2.ids.txt), version 1 | `nodisc-1.0` (`CDS II.exe` disc-free, SHA-256 `E9B46420…`) | 115 (all published, 2026-10-19): 46 functions, 27 sites, 26 globals, 2 vtables, 14 data - list: [ids-ds2.md](ids-ds2.md), constants `pse/include/pse/pse_ids_ds2.h` |

**Two games, one scheme.** Every game has its own numbers (`ds2` id 2 is not `ds1` id 2), so a plugin always passes the
game id: `PSE_RESOLVE(ids, ds2, Main_FrameDispatch)`. Where a job is the same on both games the ds2 database uses the
ds1 name (`Main_FrameDispatch`, `Site_FrameDispatchCall`, `Level_Init`, `Save_Game`, `World`, `FrontEnd_State`,
`Main_DispatchMode`, `Window_Handle`, `Iat_CreateWindowExA` …) - calling conventions, offsets and value meanings
differ (`ids-ds2.md` descriptions), so a cross-game plugin still branches on `pse->gameId`. `NULL` / `""` as the game id
= the running game (a process registers one database: its layer's).

## Getting it

```c
#include <pse/pse_ids.h>
#include <pse/pse_ids_ds1.h>

const PSEIdsInterface* ids = (const PSEIdsInterface*)pse->Query(pse->plugin, PSE_IDS_INTERFACE, 1);
uint32_t takeDamage = ids ? PSE_RESOLVE(ids, ds1, Soldier_TakeDamage) : 0;
if (!takeDamage) {
    /* this exe isn't covered (or the bytes there aren't what the database expects): leave the feature off */
}
```

- Served by the game layer (phase 1: registered by the DS1 host before any feature patches, owner = the loader `pse`)
  through `pse.messaging` `RegisterInterface(…, "pse.ids", 1, table)`, so `Query` and `QueryInterface` both find it,
  already in `PSEPlugin_Load`. A manifest can't `requires =` it (it isn't a plugin); check the pointer.
- `PSE_ID(ds1, Name)` expands to the constant `PSE_ID_ds1_Name` – a misspelt name is a compile error, never another
  id. `PSE_RESOLVE(ids, ds1, Name)` = `ids->Resolve("ds1", PSE_ID(ds1, Name))`.
- C++: `PSEResolveAs<Fn>(ids, "ds1", PSE_ID(ds1, Name))` returns the address as a typed pointer (`nullptr` when 0).
- Resolve once (in `Load` or `PSE_MSG_POST_LOAD`) and keep the address: it never changes during the process.
- Ids say *where*, not *how*: calling conventions, argument layouts and structure offsets are in the description
  column of [ids-ds1.md](ids-ds1.md) and in PSELib. A plugin built on raw addresses sets
  `PSE_PLUGIN_USES_RAW_ADDRESSES`. Where `pse.game.ds1` covers the job, use it instead (stable wrappers).
- Hooking an id: use `pse.hooks` with the resolved address (`HookFunction` on a `function`, `HookCallSite` on a
  `site` that is a `call`, `HookPointer` on a vtable / IAT `data` slot). The hook broker checks the expected bytes
  again at hook time.

## Functions

Every function takes `gameId` = `"ds1"` …; `NULL` or `""` = the running game (the first database registered – a
process runs one game). Unknown game → 0 / `NULL` / `PSE_E_NOT_FOUND`. All functions: **any thread**, any phase
after the game layer registered its database (before any plugin loads). The tables never change afterwards.

| Function | Returns |
| --- | --- |
| `uint32_t Resolve(gameId, id)` | the address for the running exe; 0 = unknown id, no address for this build, or the signature didn't match |
| `uint32_t ResolveByName(gameId, name)` | the same by name (case-sensitive). For consoles, scripts and logs; shipped plugins use ids |
| `const char* Name(gameId, id)` | the id's name (static string), also when it doesn't resolve on this build; `NULL` = unknown id |
| `PSEId IdOf(gameId, name)` | the id of a name, 0 = none |
| `PSEResult Info(gameId, id, PSEIdInfo* out)` | `PSE_OK` + name, kind, status, address; `PSE_E_NOT_FOUND` for an unknown id (out filled, status `PSE_ID_UNKNOWN`); `PSE_E_INVALID_ARG` when `out->size` is too small |
| `PSEResult InfoAt(gameId, index, PSEIdInfo* out)` | the `index`-th id in id order (0 .. `idCount`-1) for listing; `PSE_E_NOT_FOUND` past the end |
| `PSEResult Build(gameId, PSEIdBuildInfo* out)` | which build runs (`build` = `""` when the exe is unknown), its notes / SHA-256, PE timestamp + image size, database version, id / resolved / mismatch / build counts |

`out->size` must be set to `sizeof` the struct before `Info` / `InfoAt` / `Build`; later versions append fields.

### Kinds

| Kind | Value | The address is |
| --- | --- | --- |
| `PSE_ID_KIND_FUNCTION` | 1 | a function's entry point |
| `PSE_ID_KIND_GLOBAL` | 2 | a variable in `.data` / `.bss` (the variable's address – read it to get the pointer / value) |
| `PSE_ID_KIND_SITE` | 3 | an instruction inside a function: a `call` / jump / compare to hook, or an immediate operand (`Imm_*`: the operand's first byte) |
| `PSE_ID_KIND_VTABLE` | 4 | a vtable's first slot |
| `PSE_ID_KIND_DATA` | 5 | a table, constant, single vtable slot (`*_Slot`) or IAT slot (`Iat_*`) |

### Status

| Status | Meaning | `Resolve` |
| --- | --- | --- |
| `PSE_ID_OK` | the build has an address and its signature matched | the address |
| `PSE_ID_NOT_IN_BUILD` | the id exists, the database has no address for the running build | 0 |
| `PSE_ID_MISMATCH` | the bytes in the running exe don't match the signature (a modified exe?) – one log line per id | 0 |
| `PSE_ID_UNKNOWN_BUILD` | the running exe is no build the database knows – one log line | 0 |
| `PSE_ID_UNKNOWN` | no such id | 0 |

Log lines (sender `pse`, in `DesertStormFix.log` / `pse.log`):

```text
pse.ids: ds1 database 1, build steam-gog-1.0: 468 ids, 468 resolved, 0 mismatched, 0 not in this build
pse.ids: ds1 id 167 Soldier_TakeDamage: bytes at 0x00424200 don't match the steam-gog-1.0 signature - resolves to 0
pse.ids: ds1: unknown exe (PE timestamp 0x12345678, image size 0x403000) - none of the 468 ids resolve; plugins using them must switch off
```

## Stability rules

1. **An id is never reused, renumbered, renamed or given another kind once published.** New ids take the next free
   number; an id that turns out to be useless stays (its description can say "don't use"), its number is never handed
   out again.
2. Published ids are listed in `pse/ids/<game>.published.txt`, written only by `gen_ids.py --publish`.
   `scripts/gen_ids.py` fails when a published id is missing, renamed or changed kind; `pse_tests` checks the
   generated table against the same list (`ids: ds1 every published id still exists …`).
3. Names are unique per game (also ignoring case), `[A-Za-z][A-Za-z0-9_]*`, at most 63 characters, `Area_Thing`
   style (`FrontEnd_SetState`, `Site_FrameDispatchCall`, `Iat_TimeGetTime`).
4. Adding ids or builds is a minor change (database version + 1, API_CHANGELOG line). Nothing else ever changes.
5. Addresses are absolute VAs at the exe's preferred base. All supported Pivotal exes have no relocations; a module
   loaded elsewhere is treated as an unknown build.

## Signatures and the build check

- The build is picked by the running exe's **PE TimeDateStamp + SizeOfImage** (cheap, read from the mapped header;
  the 4 GB patcher's flag doesn't change them). If several builds share them (e.g. a no-CD exe with the same header),
  the one with the most matching signatures wins. The exe is not hashed at start-up; `sha256` is documentation.
- Each address carries a **signature**: 4–16 bytes expected in the original exe, `??` = any byte. Code and
  `.rdata` ids are checked at their own address (12 bytes of code, 8 of data, 12 for vtables). Globals and IAT
  slots (whose contents change at run time) are checked through a code window that uses them: `@0x<va>:<bytes>` =
  8 bytes around an instruction referencing the address. `-` = not verified (avoid).
- Signatures are checked **once, when the game layer registers the database – before anything patches the game**.
  A plugin's own later patch at an id's address doesn't change what `Resolve` returns.
- Reading is fault-safe (`VirtualQuery` + SEH): an unreadable address is a mismatch, never a crash.

## Database source format

`pse/ids/<game>.ids.txt` is the source of truth (committed); `scripts/gen_ids.py` compiles it.

```text
game ds1 Conflict: Desert Storm
database 1
build steam-gog-1.0 594D0000 00403000 D712F5EC…3E164 Steam + GOG DesertStorm.exe (identical), 4 GB flag ignored

[ids]
# id  name                kind      description
167   Soldier_TakeDamage  function  thiscall(soldier, float amount, attacker) ret 8 (vtable slot 15)

[addresses steam-gog-1.0]
# id  va          signature
167   0x00424200  83EC0C53568BF157D94654D8
```

- `build <name> <PE timestamp hex> <SizeOfImage hex> <file SHA-256 | -> <notes>` – one line per known exe.
- `[ids]`: `<id> <name> <kind> <description>`; kind = `function` / `site` / `global` / `vtable` / `data`.
- `[addresses <build>]`: `<id> <va> <signature>`; signature = hex (`??` wildcards), `@0x<va2>:<hex>`, `-`, or `?`
  (to be filled from the exe).

Generated (never edit by hand):

| File | What | Used by |
| --- | --- | --- |
| `pse/ids/generated/pse_ids_<game>.h` | the table (`pse_ids_<game>::kGame`, `pse::ids::GameDef`) | the game layer (registration), `pse_tests` |
| `pse/include/pse/pse_ids_<game>.h` | `#define PSE_ID_<game>_<Name> <id>u` + `PSE_IDS_<GAME>_DATABASE_VERSION` | plugins |
| `docs/sdk/reference/ids-<game>.md` | the list with addresses and descriptions | people |

## Adding an id

1. Find the address in the original exe (Ghidra / REA / `tools/rx.py`); check the name isn't taken.
2. Append a line to `[ids]` with the **next free number** (`max id + 1`) and to `[addresses <build>]` with `?` as the
   signature – for every build where you know the address.
3. `python scripts/gen_ids.py --fill-signatures ds1` (needs `pefile`; finds the Steam / GOG exe, or `--exe <path>`)
   fills the signatures from the exe; `python scripts/gen_ids.py --verify-exe ds1` re-checks them all.
4. `python scripts/gen_ids.py --publish ds1` freezes the new ids (appends to `ds1.published.txt`, database version
   + 1) and regenerates the three files. Until published, an id may still be renumbered or renamed.
5. Run `pse_tests` (`ids:` tests: table rules, published list, every signature against the mapped exe), add an
   API_CHANGELOG.md line ("ds1 database n: ids x–y").

## Adding another exe build (retail CD, other region, no-CD …)

1. Read its PE timestamp and SizeOfImage (`python -c "import pefile; p=pefile.PE('X.exe'); print(hex(p.FILE_HEADER.TimeDateStamp), hex(p.OPTIONAL_HEADER.SizeOfImage))"`)
   and its SHA-256 (with the 4 GB flag cleared). Add a `build` line with a new name (`retail-uk-1.0` …).
2. Add an `[addresses <build>]` section: for each id, the address in that exe (match functions by their bytes,
   strings, call structure – Ghidra Version Tracking / BSim help) and `?`. Ids you can't find are simply left out:
   they resolve to 0 (`PSE_ID_NOT_IN_BUILD`) on that build.
3. `--fill-signatures ds1 --exe <that exe>`, `--verify-exe ds1 --exe <that exe>`, then `gen_ids.py`. No id changes;
   the database version goes up with the next `--publish` (or bump `database` by hand when only addresses were added).
4. The game layer's exe detection (`PSEGame_Detect`) must accept that exe too – today it accepts only the known hash.

## Adding a game

What DS2 needed (2026-10-19): `pse/ids/<game>.ids.txt` (header `game <id> <title>`, one `build` line, ids + `?`
signatures), an entry in `DEFAULT_EXES` (and, if the game has a byte-patch table, `PATCH_DEFS`) in
`scripts/gen_ids.py`, then `--fill-signatures <game>` / `--verify-exe <game>` / `--publish <game>`; the layer registers
`pse_ids_<game>::kGame` first thing in `PSEGame_Init`. Hand-picked signatures are fine where the address' own bytes are
zero (DS2's HUD tables: `@0x006086F0:…`, a window further into the table). Tests: the exe in `pse/tests/exe_image.cpp`'s
`kGames` list (`MapGame("<game>")`), a `test_game_<game>.cpp` checking every signature on the mapped exe.

## Host side (game layers)

`pse/loader/ids.h` / `ids.cpp` (game-neutral, only `loader.h` declarations: builds into `pse_core` or, through
`pse_hostlink`, into a game layer DLL):

- `pse::ids::RegisterGameForProcess(const GameDef&, PSEPluginHandle owner, RegisterSummary* = nullptr)` – the main
  module's header + live memory. `owner` serves the interface and logs. Wired (phase 2, 2026-10-19): the DS1 game
  layer calls it first thing in `PSEGame_Init` (`pse/games/ds1/ds1_layer.cpp`, owner = the layer's own plugin
  `pse.game.ds1`, `ids.cpp` compiled into `pse_ds1.dll` through `pse_hostlink`) - the loader runs that before any
  plugin, so every signature is checked against the original bytes.
- `pse::ids::RegisterGame(def, ImageView, owner, summary)` – any image (tests map the original exe).
- `pse::ids::Interface()`, `SignatureMatches`, `ValidSignature`, `ProcessImage`, `ResetForTests`.

Tests: `pse/tests/test_ids.cpp` – tier 1 (table rules, published ids, generated constants, the API on a synthetic
database: build pick, wildcards, mismatch, unknown build, unreadable memory, bad tables) and tier 2 (every DS1
signature against the mapped original exe; spot checks: vtable slots hold the functions of the same name, IAT slots
name their imports, a call site calls its function, `Hash_Name` computes `WEAPONS.RFX` = `0x4475E8D7` through its id).

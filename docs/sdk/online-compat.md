# Online: the plugin-set check

Online co-op runs the same mission on every PC (lockstep inputs + host authority, DesertStormFix `lockstep.cpp` /
`authority.cpp`). Anything that changes what happens in the world must therefore be the same on both PCs: the same
DesertStormFix build, the same simulation plugins, the same content mods.
decided that a join is **refused** when they differ. This page says what is compared, how, what the players see, and
what plugin authors have to do.

Code: `pse/loader/compat.cpp` (loader side, game-neutral: the list, its encoding, the comparison – unit-tested in
`pse/tests/test_compat.cpp`), `src/features/onlinecompat.cpp` (DesertStormFix: fingerprint, mod hashes, the exchange,
the refusal), `src/features/online.cpp` (HOST / JOIN screens, START).

## When

As soon as a peer is connected (net `HELLO` / `WELCOME` done), **both** PCs send their list to the other one as
reliable, in-order game commands (`net::SendCommandTo`, lines starting `PSEC `). Each PC compares the other's list
with its own as soon as both are there. Nothing waits for a mission: the result is known within a second on a normal
connection.

| Result | Host | Joiner |
| --- | --- | --- |
| waiting (lists not both there yet) | `PLAYER 2: comparing plugins and mods ...`; START refused ("still comparing") | `Host: comparing plugins and mods ...` |
| match | `Player 2: same plugins and mods - ok`; START allowed | `Host: same plugins and mods - ok` |
| differ | `PLAYER 2 REFUSED - plugins / mods differ:` + the differences; the player is dropped 6 s later (the joiner normally leaves first, 1.5–4 s, once its own list was acknowledged) | `JOIN REFUSED - plugins / mods differ from the host's:` + the differences; it leaves the session; the screen keeps the list until JOIN / HOST / Back |
| no list within 30 s | refused: "player 2 sent no plugin / mod list (an older DesertStormFix? update both PCs)" | same, about the host |

Both sides also write every step and every difference to `DesertStormFix-online.log` (every build) and
`DesertStormFix.log` (dev builds), and a `[fail]` line to `Mods\DesertStormFix-report.txt`.

**Enforcement** (both sides, so an old or modified peer can't slip through): the host's START needs every connected
player to have passed; the host's level mirroring ("START <level> …", also on restarts / Continue) is sent only to
players that passed (`[fail] Online: START not sent to player n …` otherwise); a joiner ignores START until the host
has passed (`[fail] Online: START from the host ignored …`). An older build that doesn't know the check never sends a
list and is refused after 30 s; its own START handling would ignore our `PSEC` lines (unknown commands are skipped).

## What is compared

Each PC sends a `pse::compat::Set`:

| Field | DS1 value | Must match |
| --- | --- | --- |
| protocol | `pse::compat::kProtocol` (1) | yes – a difference stops the comparison ("online check version …") |
| game id | `"ds1"` | yes |
| loader API | `PSE_API_VERSION` | yes |
| host | `"DesertStormFix <version>"` (the game layer in phase 1) | yes |
| host simulation fingerprint | see below | yes – covers every built-in plugin |
| plugins | every loaded plugin (`pse::compat::LocalPlugins`): name, version, flags, hash | simulation ones: see below |
| mods | every switched-on mod (`features::LoadedMods`): folder name, content hash - **in load order**: the mods both PCs use must come in the same relative order (mod priority list, [mod-layout.md](mod-layout.md); "mod load order differs (here A, B; on the host B, A)") | yes, all of them |

### Plugins

A plugin counts as **simulation** when it has `PSE_PLUGIN_SIMULATION`, or neither flag (the safe
default). For every plugin that is simulation **on either side**:

- it must exist on both sides (`missing myname.alienfaction 1.2.0 (the host has it)` /
  `player 2 is missing myname.alienfaction 1.2.0`);
- same version (`dsfix.limits 1.0.1 here vs 1.0.0 on the host`);
- same kind: simulation on both (`… simulation here, client-only on the host`), built in on both or a DLL on both;
- **external DLL**: the same **SHA-256 of the DLL file** (`myname.alienfaction 1.2.0: a different file here than on the
  host (SHA-256 1a2b3c4d vs 9f8e7d6c)`). Two builds of one version with different code would desync – the version
  alone isn't enough. A DLL that declares a simulation fingerprint is compared by that instead (below).
- **built in** (`PSE_PLUGIN_INTERNAL`, the `dsfix.*` features + `pse`, `pse.game.ds1`): name + version only; their code
  is covered by the host fingerprint (one line, not one per feature:
  `DesertStormFix 3.3.0-Alpha: a different build here than on the host (simulation fingerprint 01234567 vs fedcba98)`).

`PSE_PLUGIN_CLIENT_ONLY` plugins may differ, be missing, or have other versions (HUD colour, photo mode, Discord,
overlays). Plugins that crashed or failed to load are not in the list (their hooks are gone).

### Why built-in plugins aren't compared by file hash

The obvious rule – hash `dinput8.dll` – would refuse every game between a **dev build** (logging, dev test hooks,
PDB-linked: the Steam copy) and the **release package** of the same source (the GOG copy, every player's install):
they are different files that simulate the same. Instead the host sends a **simulation fingerprint**:

```text
SHA-256( "dsfix-sim DesertStormFix <DS_VERSION> <kSimRevision>\n"
         + for each byte-patch group of games/ds1/src/generated/patch_tables.h (games/ds1/scripts/patch_defs.py):
             group name, then per patch: address, length, anyOriginal, original bytes, patched bytes )
```

It is the same for every build of the same source and version, and changes when the version or any byte patch
changes. Its limit: a C++ change that alters the simulation without a version bump or a patch-table change is not
seen – such a change must bump `dsfix::kSimRevision` in `games/ds1/src/core/simrevision.h` (or the version; releases
always do). External plugins have no such limit: their DLL is hashed – unless they declare a fingerprint.

### External plugins: declared simulation fingerprint

The same problem for a plugin DLL (2026-10-23): a SIMULATION plugin may export

```c
__declspec(dllexport) const char* PSE_CALL PSEPlugin_SimFingerprint(void);
```

and the check then sends `fp:` + SHA-256 of that string as the plugin's hash instead of the file's SHA-256 (loader
`compat::LocalPlugins`, called under SEH; NULL / "" / a crash = the file hash). Equal strings connect whatever the
files; a different string, or a fingerprint on one side and a file hash on the other, refuses the join:
`example.sim_fingerprint 1.0.0: different simulation code here than on the host (fingerprint 1a2b3c4d vs none)`.
The string is the author's promise – change it whenever the plugin's effect on the game changes (example:
`pse/examples/sim_fingerprint`). Without the export nothing changes.

DesertStormFix's own split-out plugins (dsfix kit, [ds1-plugins.md](ds1-plugins.md#external-plugins)) all export
`dsfix-kit DesertStormFix <DS_VERSION> rev <dsfix::kSimRevision> plugin <name> <x.y.z> tables <FNV-1a 64 of the patch
tables>` – the same inputs as the host fingerprint, so one `kSimRevision` bump covers dsfix.dll and every kit plugin.
Verified: the dev build and the release build of `dsfix.shadows` / `dsfix.audio` give the same string.

### Mods

Every mod switched on in the MODS menu (folder `Mods\<name>`, as `features::LoadedMods` lists them) must be switched
on on both PCs (name compared without case) and have the same **content hash**: SHA-256 over the mod's files sorted by
path (path relative to the mod folder, lower case, `/` separators, then that file's own SHA-256). The whole folder
counts – simpler and safer than deciding which files feed the simulation (tables, `.weapon`, `.lua`, models, sounds,
scene files all can); a different readme refuses too, which is the conservative side. The hashing runs on a worker
thread when an online session starts and caches each file by path + size + write time, so a second session is
instant; the menus never stall. Messages: `missing mod X (the host has it switched on)`, `mod X is switched on here,
not on player 2`, `mod X differs (its files aren't the same)`.

### Converted campaigns

Every folder in `PSE\Ports` (converted campaigns, see layout.md) is compared like a SIMULATION plugin named
`port:<id>` with the folder's content hash (same hashing as mods). Unlike mods it is never offered for download: each
player converts from their own copy of the source game. A missing or different conversion refuses the join and names
`port:<id>`.

(The older hostsync `M` field – a hash of the enabled mod *names* sent with START – stays as a second warning.)

## Wire format

Game commands (`net::SendCommandTo`, reliable, in order, ≤ 249 characters each), all starting `PSEC <id>` (`id` =
hex, new for every list sent – a resent list replaces a half-received one):

```text
PSEC 1A2B B <protocol hex> <api hex> <game> <host> <fingerprint>
PSEC 1A2B P <name>:<version hex>:<flags hex>:<hash>  <name>:...      (as many lines as needed, entries never split)
PSEC 1A2B M <name>:<hash>  ...
PSEC 1A2B E <plugin count> <mod count>
```

Tokens are percent-encoded (`%20` for a space, `%3A` for `:`, `%` alone = empty); `hash` is `-` for client-only
plugins, `=` for built-in ones, else 64 hex digits. The `E` line's counts must match what arrived, else the list is
rejected. `pse::compat::Encode` / `Assembler` / `Compare` are pure functions (no game, no network) – see the tests.

### The lobby and the loadout screen (DesertStormFix, same channel)

Other DesertStormFix commands share these game commands; a PC ignores lines it doesn't know. They are DesertStormFix's
own protocol (built-in plugins, covered by the host fingerprint), listed here so plugin authors know the prefixes taken.

```text
LB T <version> <slot> <slot> <slot> <slot>   the host's player table (slot "-" or pc,device,colour,view,ready,<hex name>)
LB J <device> <hex name>                     a PC asks for one more player (device: -1 keyboard + pad 0, 0..7 pad, 8 keyboard)
LB L <player>   LB N <player> <hex name>   LB C <player> <colour>   LB V <player> <0|1>   LB R <player> <0|1>
LB D <player> <device>                       requests for that PC's own players only (the host applies them)
LO #<seed> K <soldier id> <kit>              a soldier's kit on the loadout screen (applied to the other PCs' copy)
LO #<seed> D                                 a joiner's players are done;  LO #<seed> G  every PC is done (from the host)
EA #<seed> CSV <player>                      a joiner's player votes to skip the cutscene (the host counts)
EA #<seed> CSN <mask>  /  EA #<seed> CS      host: who has voted (indicator) / every player voted - skip now
EA #<seed> PU <soldier> <item>               a PC's own soldier picked an item up - the others repeat it
EA #<seed> VX <vehicle>                      a vehicle was destroyed on its owner's PC - the others destroy it too
```

`LO` lines carry the mission start's seed; lines from another start are dropped. Rules and wire format of the table:
`src/features/lobby.h` (tests `pse/tests/test_lobby.cpp`).

### When only the mods differ: the joiner gets the host's mods

Same build and plugins but other switched-on mods is not a refusal: the check reports "same plugins, different
mods", START waits, and the joiner offers to make its mods match the host's (`src/features/modsync.cpp`): switch on a
switched-off copy with the host's content, download a mod it lacks (or has another version of), switch off a mod the
host doesn't use – after the player says yes (mods may contain scripts / DLLs). Then the mods reload in game, the
joiner sends its list again and the check passes. Nothing is deleted: a replaced version stays as `_Name.yours`.
The joiner also takes the host's **load order** (a `LOAD ORDER  as the host's` row when that is all that differs):
right before the reload its priority list becomes the host's mods in the host's order, then its own others
(`modorder::Adopt`; saved - the settings value `ModOrder` is this PC's list from then on); mods just downloaded or
switched on take their host place instead of the end.

```text
MS Q <xid> <name hex>                  joiner -> host: send me that mod (one of the host's switched-on mods only)
MS M <xid> <bytes> <files> <hash>      host: manifest, then MS F <xid> <index> <size> <path hex> per file, MS E <xid>
MS X <xid> <reason hex>                host: refused
MS G <xid>  /  MS D <xid>  /  MS C <xid>   joiner: go / got it / cancelled
raw 'M' 'D' xid, chunk, 1200 bytes     host -> joiner (the mod's files end to end)
raw 'M' 'A' xid, next missing chunk, chunks held, 256-bit map   joiner -> host (resends + window)
```

The download is checked against the host's content hash before it is used. Paths are at most 100 characters, a mod at
most 2 GB.

## When something looks different on two PCs: the state dump

During an online mission every PC writes `DesertStormFix-sync.csv` next to `DesertStorm.exe` (every build; rewritten
at each game start, 256 MB at most). Once a second it lists every soldier and vehicle as that PC sees it. A line
starting `#` marks each mission start: UTC ms, `mission start <seed>` (the same on every PC), the level's scene,
this PC's number and `host` / `joiner`. The columns:

| Column | Meaning |
| --- | --- |
| `utc_ms`, `mission_ms` | this PC's clock (UTC, ms) and ms since the mission started on this PC |
| `kind`, `id` | `S` soldier / `V` vehicle, the scene script id (the same on every PC) |
| `owner_pc`, `here` | the player (1-4) whose PC runs it (vehicles: the driver's, else the host's); `own` = this PC runs it, `puppet` = this PC follows the owner's records, `mounted` = another PC's soldier in a vehicle, `runs` = a vehicle every PC runs itself |
| `team`, `in_play`, `health` | team byte, in play, health |
| `x y z`, `rot_x rot_y rot_z` | position (cm) and rotation (degrees) |
| `aim_pitch`, `aim_yaw` | soldiers' look angles |
| `move`, `stance`, `hold`, `item` | movement state, stance, weapon held up / lowered, the held item's name hash |
| `anim0`, `anim0_t`, `anim1`, `anim1_t` | the render instance's two animation channels: the playing animation's name hash and its time |
| `vehicle_or_driver`, `target`, `asleep` | soldiers: the vehicle they sit in, the soldier they target, asleep (unseen AI); vehicles: the driver |
| `vx vy vz`, `turret0 turret1`, `flags` | vehicles: chassis velocity, gun angles; the object's flags |

For a problem report, send `DesertStormFix-online.log` and `DesertStormFix-sync.csv` from every PC.
`tools/sync_compare.py host.csv joiner.csv [--id N] [--kind S|V]` (local tool) pairs the rows by mission seed,
object and nearest UTC time and lists, per object, where the PCs disagree.

## For plugin authors

- **Set your flag.** `PSE_PLUGIN_CLIENT_ONLY` only if the plugin changes nothing any other PC could notice in the
  simulation: drawing, sound, UI, input devices, Discord, logs. Anything that touches game state, AI, physics,
  weapons, random draws, timing, level scripts or saves is `PSE_PLUGIN_SIMULATION`. When in doubt, leave both off
  (= simulation): a refused join is annoying, a desync mid-mission is worse.
- Every change to a simulation plugin's DLL refuses joins with the old one – ship versions together, and bump
  `version` with every release so the message says `1.2.1 vs 1.2.0` instead of "a different file".
- A client-only plugin must not change anything that feeds the simulation even indirectly (e.g. calling the game's
  random functions from a draw callback changes the random sequence).
- Settings a plugin reads that change the simulation (difficulty-like rules) must come from the host while online –
  today only DesertStormFix's own settings are sent (hostsync `.D` draw distance, `.J` Japanese fixes, `.W` weather …
  in START). A `pse.online` hook for plugins' own handshake data is planned not built.

## Tests

- Tier 1 (`pse_tests`): `test_compat.cpp` – escaping, encode → assemble round trip with many plugins + mods within
  the line limit, assembler restarts / damaged lists, every comparison rule both ways (the verdict is symmetric),
  SHA-256 known answers, the local plugin list (built in = `=`, disabled left out).
- Live (two copies on one PC, session scratchpad `online_compat_test.py <out> match mismatch mods`): Steam = dev build
  hosts, GOG = release package joins (installed inside the install lock, put back after).
- The everyday online tools (`tools/online_menu_test.py`, `online_start_test.py`, `net_test.py`, `lockstep_soak.py`) run with mods off on both copies (`net_test.ONLINE_ENH`) so the check
  passes; `online_start_test.py` asserts "plugins and mods match" before it presses START.

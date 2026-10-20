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
| mods | every switched-on mod (`features::LoadedMods`): folder name, content hash | yes, all of them |

### Plugins

A plugin counts as **simulation** when it has `PSE_PLUGIN_SIMULATION`, or neither flag (the safe
default). For every plugin that is simulation **on either side**:

- it must exist on both sides (`missing kyle.alienfaction 1.2.0 (the host has it)` /
  `player 2 is missing kyle.alienfaction 1.2.0`);
- same version (`dsfix.limits 1.0.1 here vs 1.0.0 on the host`);
- same kind: simulation on both (`… simulation here, client-only on the host`), built in on both or a DLL on both;
- **external DLL**: the same **SHA-256 of the DLL file** (`kyle.alienfaction 1.2.0: a different file here than on the
  host (SHA-256 1a2b3c4d vs 9f8e7d6c)`). Two builds of one version with different code would desync – the version
  alone isn't enough.
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
         + for each byte-patch group of src/generated/patch_tables.h (scripts/patch_defs.py):
             group name, then per patch: address, length, anyOriginal, original bytes, patched bytes )
```

It is the same for every build of the same source and version, and changes when the version or any byte patch
changes. Its limit: a C++ change that alters the simulation without a version bump or a patch-table change is not
seen – such a change must bump `kSimRevision` in `src/features/onlinecompat.cpp` (or the version; releases always
do). External plugins have no such limit: their DLL is hashed.

### Mods

Every mod switched on in the MODS menu (folder `Mods\<name>`, as `features::LoadedMods` lists them) must be switched
on on both PCs (name compared without case) and have the same **content hash**: SHA-256 over the mod's files sorted by
path (path relative to the mod folder, lower case, `/` separators, then that file's own SHA-256). The whole folder
counts – simpler and safer than deciding which files feed the simulation (tables, `.weapon`, `.lua`, models, sounds,
scene files all can); a different readme refuses too, which is the conservative side. The hashing runs on a worker
thread when an online session starts and caches each file by path + size + write time, so a second session is
instant; the menus never stall. Messages: `missing mod X (the host has it switched on)`, `mod X is switched on here,
not on player 2`, `mod X differs (its files aren't the same)`.

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

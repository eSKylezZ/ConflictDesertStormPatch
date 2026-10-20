# `pse.hooks` – the hooks broker (version 1)

Header: `pse/include/pse/pse_api.h` (`PSEHooksInterface`). Implementation: `pse/loader/hooks.cpp`, length decoder
`pse/loader/x86len.cpp`. Tests: `pse/tests/test_hooks.cpp` (`pse_tests.exe hooks`).

API version 0 = unstable: this interface may still change before SDK 1.0.

## Purpose

Every change a plugin makes to the game's code or data goes through the broker, so that

- each patched byte has an **owner** – two plugins can never silently overwrite each other, and a crash report can name
  the plugin that patched near the fault;
- several plugins can hook the **same** call site, function, IAT call or vtable slot – the hooks are **chained**
  instead of the last one winning (the SKSE trampoline problem);
- hooks can be removed again at run time (live feature toggles), and the site gets its original bytes back when the
  last one goes.

Get the table with `pse->Query(pse->plugin, PSE_HOOKS_INTERFACE, 1)`.

## Ownership model

| Owner | Bytes it owns |
| --- | --- |
| byte group (`PatchGroup`) | every byte of every patch in the group, owned by the plugin that applied it |
| hook site | the site's bytes (call 5, indirect call 6, pointer 4, function = prologue length); `OwnerOf` reports the plugin at the head of the chain |

Rules:

- A byte group may not overlap **any** owned byte – another plugin's group, the same plugin's *other* group, or any hook
  site (also the plugin's own). Result `PSE_E_CONFLICT`, nothing written, one log line naming both plugins:
  `[conflict] second (beta) at 0x…: byte 0x… is owned by alpha (byte group 'first')`.
- A hook may not overlap a byte group or a hook site of another kind / start address. A hook of the same kind at the
  same address joins that site's chain.
- The same plugin applying the same group again is fine (idempotent). A changed set under the same name replaces the old
  one (tunable values: the old group is reverted first and comes back if the new one fails).
- `RevertGroup` restores the bytes saved at `PatchGroup` time and releases ownership.

## Chaining model

```
             site bytes                 subscriber next variables
call site:   E8 -> HookB  ...  HookB: nextB -> HookC
                               HookC: nextC -> HookA
                               HookA: nextA -> original target / trampoline / jmp [iat] stub / old pointer
```

- Higher `priority` runs first; equal priority = registration order.
- `*next` is written by the broker (aligned 4-byte stores) when the hook is added and whenever its neighbour changes.
  Call `next` exactly like the original function; not calling it skips the rest of the chain and the original.
- Removing a hook makes its predecessor (or the site) point at its successor. The removed hook's own `next` keeps its
  last value, so a thread still running inside it finishes through the chain.
- When the last subscriber goes, the site gets its original bytes back. A function hook's trampoline stays allocated.
- Rewiring goes back to front (a link is changed only after the link it points to is right), so the chain is always
  complete for code running on another thread. Writing the 5 / 6 site bytes themselves is not atomic: install and
  remove hooks on the game thread, where the hooked code isn't running.

What "the original" is per kind:

| Kind | Original (end of the chain) |
| --- | --- |
| `HookCallSite` | the call's original target |
| `HookFunction` | a trampoline: the moved prologue + `jmp entry+len` |
| `HookIndirectCall` | a stub `jmp dword ptr [iatSlot]` – follows the slot live |
| `HookPointer` | the value the slot held before the first hook |

## Functions

All functions take the broker's lock (any thread is safe), but install hooks from the game thread (above). Log lines go
through `pse.log` (in tests: the callbacks given to `pse::hooks::SetEnvironment`; default `OutputDebugStringA` with
names `plugin#<n>`).

### `PatchGroup(plugin, group, patches, count)`

All-or-nothing group of byte patches. Each `PSEBytePatch {va, original, patched, len, anyOriginal, name}` site must
hold `original` or already `patched` (e.g. the exe on disk carries it; revert then writes `original`), or anything when
`anyOriginal` (a tunable value – revert restores what was there). Patches inside one group may not overlap.

Results: `PSE_OK`; `PSE_E_INVALID_ARG` (plugin 0, NULL, len 0, overlapping patches); `PSE_E_MISMATCH` (unexpected
bytes – different exe?); `PSE_E_CONFLICT`; `PSE_E_ACCESS` (unmapped / VirtualProtect failed).

```c
static const uint8_t orig[] = {0x0F, 0x84, 0xA2, 0x00, 0x00, 0x00}, nops[] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
PSEBytePatch p = {0x4488B3, orig, nops, 6, 0, "all aspect ratios"};
if (hooks->PatchGroup(pse->plugin, "widescreen", &p, 1) != PSE_OK) { /* feature off */ }
```

### `RevertGroup(plugin, group)`

Restores the group's saved bytes. `PSE_E_NOT_FOUND` if this plugin has no such group (the name is per plugin);
`PSE_E_MISMATCH` (nothing written) if a non-`anyOriginal` site holds neither the patched nor the saved bytes.

### `HookCallSite(plugin, site, expectedTarget, fn, priority, next, outId)`

`site` holds `E8 rel32`. `expectedTarget` is the call's **original** target, also when other plugins have already
hooked the site. `PSE_E_MISMATCH` if it isn't.

```c
static int (__cdecl* s_next)(int);
static int __cdecl MyDraw(int view) { /* before */ int r = s_next(view); /* after */ return r; }
PSEHookId id;
hooks->HookCallSite(pse->plugin, 0x40F1DC, 0x542F40, (void*)&MyDraw, 0, (void**)&s_next, &id);
```

### `HookFunction(plugin, entry, prologueLen, expectedPrologue, fn, priority, next, outId)`

Entry detour. `prologueLen` (5..64) must end on an instruction boundary (`PSE_E_INVALID_ARG` otherwise). The moved
instructions go into a trampoline; the entry gets `jmp rel32` to the first subscriber, the rest of the prologue `int3`.
`expectedPrologue` (optional) is compared with the **original** bytes (`PSE_E_MISMATCH`). A later subscriber may give
a shorter length (still on a boundary) but not a longer one (`PSE_E_UNSUPPORTED`).

Relocation in the trampoline:

| In the prologue | Becomes |
| --- | --- |
| `E8` / `E9 rel32` | same, re-targeted |
| `0F 8x rel32` | same, re-targeted |
| `EB rel8`, `7x rel8` | `E9 rel32`, `0F 8x rel32` (prefixes such as branch hints kept) |
| `E0`–`E3` (loop*, jecxz), any rel16 branch (`66` prefix) | refused: `PSE_E_UNSUPPORTED` |
| a branch whose target is inside the moved bytes | refused: `PSE_E_UNSUPPORTED` |
| an instruction the decoder doesn't know | refused: `PSE_E_UNSUPPORTED` |
| anything else | copied (32-bit code has no IP-relative data operands) |

Not detected: jumps from *elsewhere* in the function into the prologue bytes, and `call $+5; pop` tricks (the pushed
address would be the trampoline's). Both are rare in MSVC 2002 code; check the function before hooking it.

```c
static const uint8_t pro[] = {0x83, 0xEC, 0x08, 0x53, 0x56};   // sub esp,8; push ebx; push esi
hooks->HookFunction(pse->plugin, 0x4BA5B0, 5, pro, (void*)&MyTimer, 0, (void**)&s_timerNext, &id);
```

### `HookIndirectCall(plugin, site, iatSlot, fn, priority, next, outId)`

`site` holds `FF 15 <iatSlot>` (6 bytes) → `E8 rel32` + `90`. The chain ends in `jmp dword ptr [iatSlot]`, so a
changed IAT slot (or a `HookPointer` on it) is followed live. `fn` has the API's signature (usually `__stdcall`).

### `HookPointer(plugin, slot, fn, priority, next, outId)`

A 4-byte code pointer in memory (vtable slot, callback table, IAT slot). The chain ends at the value the slot held
before the first hook; the last `Unhook` writes it back.

### `Unhook(plugin, id)`

Removes one hook. `PSE_E_NOT_FOUND` (unknown id), `PSE_E_CONFLICT` (the hook belongs to another plugin),
`PSE_E_INVALID_ARG` (id 0). Logs a warning (and restores anyway) if the site was changed outside the broker.

Common hook results: `PSE_E_INVALID_ARG` (plugin 0, `fn` / `next` NULL, `next` not 4-byte aligned), `PSE_E_EXISTS`
(this `next` variable already belongs to a hook – each hook needs its own), `PSE_E_CONFLICT`, `PSE_E_MISMATCH`,
`PSE_E_NO_MEMORY`, `PSE_E_ACCESS`.

### `AllocCode(plugin, size)`

Executable (RWX) memory, 16-byte aligned, filled with `int3`. Bump allocation over 64 KB `VirtualAlloc` chunks; never
freed while the game runs (32-bit: everything is in rel32 reach of the image). NULL for plugin 0, size 0 or > 16 MB.

### `ListHooks(out, max)`

Fills up to `max` `PSEHookInfo` entries and returns the total (call with `max` 0 to size the array). Order: byte groups
(one entry per patch: `kind` `PSE_HOOK_BYTES`, `id` 0, `name` = group name), then hook sites, one entry per subscriber
in chain order (`chainPosition` 0 = runs first, `priority`, `id`; `name` NULL). `va` / `len` = the bytes owned.
`ownerName` / `name` stay valid until the next broker call that changes state.

```c
uint32_t n = hooks->ListHooks(NULL, 0);
PSEHookInfo* list = calloc(n, sizeof *list);
hooks->ListHooks(list, n);
for (uint32_t i = 0; i < n; ++i)
    log->Write(me, PSE_LOG_INFO, "%08X +%u %s #%u %s", list[i].va, list[i].len, list[i].ownerName,
               list[i].chainPosition, list[i].name ? list[i].name : "");
```

### `OwnerOf(va)`

Plugin owning that byte (for a hook site: the plugin at the head of its chain), or 0.

## Instruction length decoder (`pse::x86::Decode`)

Table-driven, 32-bit mode: legacy prefixes (`66 67 F0 F2 F3`, segment `26 2E 36 3E 64 65`, up to the 15-byte limit),
the one-byte map (ModRM / SIB / disp8 / disp32, 16-bit addressing with `67`, `moffs`, imm8 / imm16 / imm16-32 by
operand size, `enter`, far pointers, `ret imm16`, group 3 `F6` / `F7` with an immediate only for `test`), x87
`D8`–`DF`, the two-byte map `0F xx` (SSE / MMX, jcc rel32, setcc, cmovcc, bt / shld / shrd imm8, 3DNow! suffix byte),
three-byte `0F 38 xx` and `0F 3A xx` (imm8). Undefined opcodes and VEX / EVEX / XOP encodings (`C4` / `C5` / `62` with
mod = 3, `8F` with reg ≠ 0) return 0 – a hook over them is refused.

## DS1 host: DesertStormFix's core/patch on the broker

DesertStormFix's features patch through `src/core/patch.h`; since 2026-10-19 every one of those calls goes through
the broker, owned by the feature's internal plugin ([ds1-plugins.md](../ds1-plugins.md)). The functions keep the
semantics they had before the broker. Tests: `pse/tests/test_patch_compat.cpp` (`pse_tests.exe patch`, compiles
`patch.cpp` with stubs), live `tests/live/test_pse.py`.

### Owner

`patch::OwnerScope scope(plugin)` (RAII, per thread, nests) sets the owner of everything patched while it lives;
`patch::CurrentOwner()` = the scope's plugin, else `dsfix`. Outside any scope a hook / jump at a site that had one
before gets that site's last owner, and a byte group always keeps the owner it was first applied by.

### Mapping

| `patch::` | Broker | Notes |
| --- | --- | --- |
| `ApplyGroup(name, …)` | `PatchGroup(owner, name, …)` | Owner = whoever holds a group of that name (found in this file's records, else by name in `ListHooks`), else the scope. Group names are unique across all features. |
| `RevertGroup(name, …)` | `RevertGroup(owner, name)` | Found by name whatever scope is active – a live toggle from the in-game menu reverts the right group. Not applied = success. |
| `HookCall(site, fn, expected)` | `HookCallSite(owner, site, original, fn, priority, next)` | `next` = an internal slot (fn calls the original itself, as before). |
| `HookIndirectCall(site, fn, iat)` | `HookIndirectCall(owner, site, iat, fn, 0, next)` | internal `next` |
| `WriteJump(site, fn)` | `PatchGroup(owner, "jmp@0x<site>", {site, 5 bytes, anyOriginal})` | Re-jumping the same site replaces the group (new owner = scope). |
| `WriteJump(site, fn, len, original)` | `PatchGroup(owner, "jmp@0x<site>", {site, len bytes: jmp + nops, original})` | A jump over whole instructions inside a function; the nops belong to the same group. `original` (len bytes) is checked unless null; already in place = true. |
| `WriteCall(site, fn, len, original)` | `PatchGroup(owner, "call@0x<site>", …)` | `call fn` over code that is not a call (the stub replays what it replaced), rest nop. Same rules as above. |
| `RevertCode(site)` | `RevertGroup(owner, "jmp@…" / "call@…")` | Takes a `WriteJump` / `WriteCall` back (features switching off). |
| `SubscribeCall(site, fn, original, priority, &next)` | `HookCallSite(owner, site, original, fn, priority, &next)` | For shared sites: the caller's own `next`, called like the original. |
| `HookSlot(slot, fn, &next, expected, priority)` | `HookPointer(owner, slot, fn, priority, &next)` | Vtable / IAT / script API table slot, or the imm32 of a `push <callback>`. `next` = the variable the feature kept the old value in (null = internal); `expected` = the slot's original value (0 = not checked). Same fn again = true. |
| `UnhookSlot(slot, fn)` | `Unhook` | The slot gets its original value back when its last hook goes. |
| `HookFunction(entry, fn, &next, expected, expectedLen, prologueLen, priority)` | `HookFunction(owner, entry, len, nullptr, fn, priority, &next)` | Entry detour. `len` = `prologueLen`, or (0) the shortest run of whole instructions ≥ 5 (`PrologueLength`). `expected` / `expectedLen` (any length) are compared with the entry before the first hook. `next` null = a replacement that never runs the original. Same fn again = true; a second feature joins the chain. |
| `UnhookFunction(entry, fn)` | `Unhook` | The prologue comes back when the last hook goes. |
| `Write` / `WriteValue` | – (direct write) | See "Writes over hooks" below. |

### `HookCall` on a site that is already hooked

| Situation | Result | Same as before the broker? |
| --- | --- | --- |
| (a) the site doesn't call `expected` | `false`, `[fail] call hook at …: does not call …` | yes |
| (b) `expected` = the hook that runs first (hand-chaining: `fn` calls that hook itself) | `fn` joins the chain at that hook's priority + 1, the site calls `fn` | yes (site bytes identical) |
| (c) `expected` = the original target, `fn` different | `false`, `[fail] call hook at …: already hooked …` – a broker chain would accept it, but `fn` would never run | yes |
| (d) `fn` already runs first there | `true`, nothing added | **changed**: was `false` (the site no longer called `expected`) |
| `fn` = the site's original target, `expected` = the first hook (a feature switching itself off, hud.cpp `ApplyIconCentring`) | every hook of ours there removed, original bytes back | yes (site bytes identical) |
| bytes owned by a byte group / jump / another plugin's hook | written outside the broker, `[warn] call hook at …: byte … belongs to … - hooked outside the broker` | yes |

`HookIndirectCall`: same `fn` again = `true`; any other hook there = `false` (as before). `WriteJump` over bytes the
broker gives to someone else: written outside the broker with a `[warn] jump at …` line.

**Sites the game layer owns** (2026-10-19; [game-ds1.md](game-ds1.md#events-and-messages)): the DS1 layer hooks the
frame call `0x4116EC`, the render call `0x40FD9E`, `Level_Init`'s call `0x40E05D`, the unload calls `0x40DF18` /
`0x411404`, the `Save_Game` / `Save_Load` entries and the `Direct3DCreate8` import slot before DesertStormFix loads.
`HookCall` on such a site (no record of this file there) would fall into the last row - a raw write over the layer's
hook - so the features that share them use `SubscribeCall` with priority 0 instead: framecap (`0x4116EC`, in front of
the layer), the frame recorder (`0x40FD9E`, in front), scripthost (`0x40E05D`, behind); graphics' `HookSlot` on the
import slot chains behind the layer's (its "is the slot still the real `Direct3DCreate8`" check accepts a slot the
broker owns). The old dsfix.loadout save / load call-site hooks are gone.

### Writes over hooks

`patch::Write` over bytes of a call / indirect-call / pointer / function hook or a `jmp@` / `call@` group made through
`core/patch` first hands them back to the broker (`Unhook` – the broker restores the original bytes – or
`RevertGroup`), then writes, so no stale record is left (a feature putting a call or an old slot value back by writing
it: controller.cpp's off path, scripthost.cpp before 2026-10-19). A `call X` written exactly over a call site whose
chain contains `X` - or a 4-byte write of `X` exactly over a pointer slot whose chain contains `X` - removes only the
hooks in front of `X`. Bytes of a real byte group (tunable values such as the HUD scale float `0x5D7B00` or the pad
deadzone `0x538997`) are written over as before; dev builds log one `[note] patch::Write at … over bytes of byte
group …` per address. `HookSlot` on a slot inside a byte group (or another hook kind) writes outside the broker with
`[warn] pointer hook at …`; `UnhookSlot` restores that too.

### What uses what (2026-10-19)

- **Pointer hooks (`HookSlot`)**: front-end screen vtable slots (coop main menu / SP menu set-up, difficulty /
  uniform / mission list updates, campaigns SP update, difficulty set-up, fixmenu Options create / set-up, menus
  YES / NO enter, pausemenu set-up, modsmenu main menu draw), script API table slots (difficulty +0x41C, timeofday
  +0x78 / +0x7C / +0x80 / +0x90, scripthost +0x44 for the length of a base mission's LevelSetup), `push <callback>`
  imm32s (coop save / load list filter and mission draw, splitscreen soldier panel, launcher dialog procs, hotplug dev
  NoPads), IAT slots (graphics `Direct3DCreate8` `0x5D8308`, dev `DirectSoundCreate8` `0x5D8030` + determinism's
  DetNoSound in front of it, the clock imports below), the dev GlobalFocusAudio DirectSound vtable slot, and the D3D8
  COM vtables (below). Each hook's `next` is the variable the feature already called the old value through, so two
  features on one slot chain.
- **Function hooks (`HookFunction`)**: the hand-made trampolines are gone (timing frame timer, memory heap / file
  read, mods resolver / archive open / stream load, textfit row / item draw, shadows builders, bodies skin apply,
  weaponicons picture lookup, splitscreen menu-open ×3, coop state → screen, overlay image draw, dev / voices probes),
  and entry `jmp`s that replace a function are detours without `next` (difficulty getters, splitscreen AxisValue, hud
  menu line height, display ClipCursor, limits physics add, diagnostics boxes / file open, versus ×4, ingame_input
  string lookup, dev time scale, barcolour dev probe, lockstep's sound-playing query `0x55E130`, determinism's dev
  mouse / pad poll blocks `0x5386D0` / `0x539340`), plus lockstep's button evaluator `0x450770` and raw keyboard
  queries `0x539DA0` / `0x539DD0` / `0x539E10` / `0x539820` and determinism's dev random-table draw loggers
  `0x4B7DF0` / `0x4B7E30` / `0x4B7E60` / `0x4B7E90` (their stubs go on through `next`). Every prologue is checked
  on the mapped exe (tier 2: `pse_tests patch image`). The image draw entry `0x53C9E0` is a shared chain: overlay (priority 100) → the dev HUD
  probe (0).
- **Code groups (`WriteJump(len, original)` / `WriteCall`)**: jumps out of the middle of functions (coop main menu
  select, difficulty id → level, units item slot, jpfixes ×4, splitscreen slow-motion tail jump, mods exists tail
  jump, the 17 hud layout-offset spans) and calls over non-call instructions (controller poll, overview message 0x12,
  versus scores list, ingame_input cursor flag, splitscreen panel walk end, hud field widths, mouse ×2). Longer custom
  sequences are named byte groups: hud tutorial bar padding, footprints gate, followers control calls, display style
  check + `mov ebp, ScreenToGame`, jpfixes route scan, limits weapon-restore `je`.

### COM vtables (D3D8)

graphics.cpp hooks the `IDirect3D8` object's `CreateDevice` (slot 15) and the device's slots 3 TestCooperativeLevel,
14 Reset, 15 Present, 20 CreateTexture, 31 SetRenderTarget, 35 EndScene, 50 SetRenderState, 63 SetTextureStageState,
72 DrawPrimitiveUP, 73 DrawIndexedPrimitiveUP, 76 SetVertexShader (dev builds also 16 GetBackBuffer, 25
CreateRenderTarget, 28 CopyRects, 30 GetFrontBuffer); memory.cpp hooks texture `Release` (slot 2). Owners
`dsfix.graphics` / `dsfix.memory`, priority **0** (`features::kD3dHookPriority`). They are installed at run time (first
`Direct3DCreate8` / `CreateDevice` / `CreateTexture`) through `src/core/comhooks.h`:

- A vtable is one table per COM class, shared by every object of it, so a pointer hook (`patch::HookSlot` → broker
  `HookPointer`) on `&vtable[index]` catches every object. `comhooks::Table` keeps one record per class (vtable address,
  up to 4): its own `next` per slot, so a second class - another D3D8 implementation, a wrapper DLL - has its own chain
  ending at its own original. Hooks find the right `next` from the object's vtable pointer, which other plugins'
  hooks don't change. Re-hooking a class (CreateDevice again, a second device, `Reset`) is a no-op.
- Renderer 0: the system `d3d8.dll` (on Windows 10/11 the device vtable lies in private memory, not in the DLL image -
  stable for the process: the game creates its device once and only `Reset`s it). Renderer 1-3: the D3D8-on-D3D9
  wrapper's own C++ classes (`src/render/d3d8to9.cpp`, vtables in DesertStormFix's image); they're hooked the same way,
  because the features work at the D3D8 interface (MSAA present parameters, filter states, V-Sync pacing, photo
  capture, overlay at EndScene). The wrapper's internal reference counting doesn't go through the vtable.
- The `Direct3DCreate8` import slot `0x5D8308` is a plain pointer hook (`dsfix.graphics`).

**Chaining on D3D from another plugin**: take the device from `[0x755050]` (or the `IDirect3D8` from renderer
`[0x63C924]` + 0x4052C) once it exists, and `HookPointer` the slot address `vtable + index * 4` with your own `next`:

| priority | runs | sees |
| --- | --- | --- |
| > 0 | before DSFix | the game's call (e.g. Present before V-Sync pacing / photo capture, EndScene before DSFix's overlay is drawn) |
| < 0 | after DSFix | what DSFix passes on (MSAA in the present parameters, anisotropic filter states, the overlay already drawn), right in front of D3D |

A plugin that hooks before the device exists has to wait for it (a later "pse.ds1 d3d events" interface may hand the
device out); hooking the slot of one class does not hook another class's.

### Clocks (lockstep + determinism)

The game timers' clock calls `0x4BA3E4` / `0x4BA5B7` (`call 0x4BA370`, `SubscribeCall`, one `next` per site) and the
`timeGetTime` / `GetTickCount` import slots `0x5D82F4` / `0x5D806C` (`HookSlot`) are shared chains of
`dsfix.determinism` (dev test `Dev\DetTest`, priority **200**, `features::kClockDeterminism`) and `dsfix.lockstep`
(online, **100**, `kClockLockstep`), in that order whatever order they were installed in. Each answers with its
virtual clock while it is active (determinism: from its test level's load; lockstep: armed and not in owner mode, game
thread only for the imports), else calls `next` - so the determinism test's clock wins, then lockstep's, then the real
one. (Before the broker both wrote the same slots / call sites: the second one failed or overwrote the first.)
Determinism's `DirectSoundCreate8` hook `0x5D8030` (DetNoSound) has priority 200 too and never calls on - ahead of
dev.cpp's GlobalFocusAudio (0).

### Still raw

- **Data / immediate values** written with `Write` / `WriteValue` (not hooks): menu list capacities and item counts
  (coop, campaigns, difficulty), binding-set count (splitscreen), tunables changed at run time (cinematics flags,
  display fullscreen imm, hotplug coop-level imm, mouse acceleration imm, jpfixes lift / drop bytes and impulse-scale
  operands, timing landing-threshold operand, rumble effect table, controls key-name sites), data pointers (units
  field-format strings, the dev / scripthost start-up option string `0x5E7050`), single-byte branch flips (versus back
  state, tooltips weapon names, dev RunInBackground and lockstep's keep-running `0x411390`, ingame_input dev
  `0x45055E`). The HUD scale float and the pad deadzone live inside real byte groups and are written over them
  (`[note]`).
- **Owned by other features, unchanged here**: authority.cpp's soldier / vehicle vtable slots and entry jumps,
  cheats.cpp's vtable slots / flag operands, loadout.cpp / gear.cpp / vehicles.cpp entry jumps.

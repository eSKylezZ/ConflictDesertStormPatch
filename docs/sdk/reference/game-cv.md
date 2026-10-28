# Conflict: Vietnam – game layer `pse_cv` and `pse.game.cv` (version 1)

Conflict: Vietnam (Pivotal Games, 2004) runs on Direct3D 9. PSE runs on it with the unchanged shim (`dinput8.dll`)
and `pse_loader.dll`, plus the game layer `PSE\Games\pse_cv.dll` (`pse/games/cv/`) and the host plugin VietnamFix
(`PSE\Plugins\cvfix.dll`, `games/cv/src/pse/`). Game id `cv`. Built and live-tested 2026-10-24. The standalone
`dinput8.dll` build (`games/cv/CMakeLists.txt`'s default target, no PSE involved) keeps working side by side - PSE
is opt-in (`CV_PSE_PLUGIN` / preset `cv-pse`), same as Desert Storm II's `DS2_PSE_PLUGIN`.

## What runs on CV

- **Exe**: `Vietnam.exe`, the Sep 2004 build without SecuROM sections (SHA-256 `636576CA…710C`, PE time stamp
  `0x414ECA49`). The earlier retail / "1.0 Fixed" exes (time stamp `0x411BBEAF`) are a *different* compile - every
  address here is mapped against the Sep 2004 build, so `PSEGame_Detect` refuses them rather than silently
  mismatching (a build with that time stamp but an unknown hash is still accepted, as DS1/DS2/TGE do for "another
  file of the known build").
- **Scope (deliberately small)**: widescreen display modes and the Mods folder only, per the 2026-10-24 port request
  - the same two features the standalone build ships, reused *unchanged* (`games/cv/src/features/widescreen.cpp`,
    `mods.cpp`) with a header that routes their `core/patch.h` calls through the shared broker
    (`games/ds1/src/core/patch.cpp`) instead of CV's own plain `patch.cpp`. `games/cv/src/features/devbg.cpp` +
    `graphics.cpp` are reused too (unchanged), so the PSE build can still be tested off-screen and muted exactly like
    the standalone (games/cv/CLAUDE.md "Testing"). `hud.cpp` / `frontend.cpp` / `frame.cpp` / `probe.cpp` are **not**
    built here - out of scope for this port, and neither widescreen nor mods needs them.
- **No core game events yet**: unlike DS1/DS2/TGE, this layer does not hook the frame loop, render call, level
  load/unload or window shutdown. `games/cv/CLAUDE.md`'s addresses for those are call *sites*, not verified function
  entries / prologues (the exact byte-for-byte prologue `HookFunction` needs wasn't re-derived for this port) - so
  there is no `PSE_EVENT_FRAME`, no `LEVEL_LOAD_*`, no `PSE_MSG_EXIT` here. Widescreen and mods don't need any of
  them (both patch once at `PSEPlugin_Load`, before any level exists). A future port that wants those events should
  verify Screen_Load / Level_Init / Level_Unload / the window proc the way the TGE layer's events were mapped
  (`pse/games/tge/tge_events.h`'s table) before wiring them up.
- `pse.game.cv` v1 is correspondingly tiny: exe identification only (`pse_game_cv.h`).

## Getting it

```c
#include <pse/pse_game_cv.h>

const PSEGameCvV1* game = (const PSEGameCvV1*)pse->Query(pse->plugin, PSE_GAME_CV_INTERFACE, 1);
if (!game || game->size < sizeof(PSEGameCvV1)) return 0;   /* not Conflict: Vietnam */
```

Served by the built-in plugin `pse.game.cv` 1.0.0 (CLIENT_ONLY): a manifest can say `requires = pse.game.cv >= 1.0`
or `games = cv`.

## Functions

| Function | Returns |
| --- | --- |
| `ExeSha256()` | the running exe's SHA-256 (Large Address Aware flag cleared), upper-case hex; any thread |
| `ExeKnown()` | 1 = the exe is the Sep 2004 build every address here comes from; any thread |

## VietnamFix as a host plugin

`games/cv/src/pse/cvfix.cpp` (manifest `cvfix.pse`, `host = true`, `requires = pse.game.cv >= 1.0`). Sub-plugins
(internal, `patch::OwnerScope` per `Apply*`):

| Plugin | Flags | What |
| --- | --- | --- |
| `cvfix.widescreen` | CLIENT_ONLY | launcher resolution list: every adapter mode, not just the 640x480/800x600/1024x768/1280x1024 whitelist (`Launcher_ModeFilter` `0x419969` → `mov eax,1; ret`) |
| `cvfix.mods` | SIMULATION | `Mods\<mod>\**` served ahead of the `.dat` archives - `File_Resolve` `0x5308F5`, `File_Open` `0x52FA17`, `File_Exists` `0x53154E` hooked via `patch::HookFunction` broker entry detours (not the standalone's own hand-rolled trampolines - same logic, broker-owned). Hooked even with no mods (2026-10-24) so the MODS menu can switch one on while the game runs |
| `cvfix.modsmenu` | CLIENT_ONLY | MODS item in the game's own main menu (between LOAD GAME and OPTIONS) opening front-end state `0x39`, a screen of the game's OPTIONS class listing every `Mods\<mod>` folder ON / OFF; Enter toggles (renames `_<mod>` ↔ `<mod>`, re-indexes the Mods folder, no restart), Esc returns. Up to 128 mods; more than 10 scroll in a 10-row window with the game's own list input (2026-10-25). A mod switched on or off here applies to the next mission load in the same session. Hooks: main menu vtable set-up slot (`0x6AB76C`), list input call `0x460AF9`, `ScreenForState` `0x443316`, catalog text `0x56BE90` (hashes `0xC0A10001` / `0xC0A10002`); main menu capacity `0x460A35` 4 → 5. Details: `games/cv/CLAUDE.md` "MODS menu" |
| `cvfix.graphics` | CLIENT_ONLY | `graphics::Install()` - D3D9 device hooks the background test mode needs (CV's runtime rewrites its per-device vtable after `CreateDevice`; re-hooked every frame) |
| `cvfix.devbg` | CLIENT_ONLY | dev builds only: off-screen/muted background test mode (`HKCU\Software\VietnamFix\Dev`), same registry values as the standalone (`games/cv/CLAUDE.md` "Testing") |

No settings are declared yet (widescreen / mods have no on/off switch here, matching the standalone build - both
just apply at load). `features::g_afterFrontEndLoad` (a dev hook `frontend.cpp` normally wires up) is defined as an
inert `nullptr` in `cvfix.cpp` since `frontend.cpp` isn't built here - `devbg.cpp`'s `StartLevel` dev value is
therefore a no-op on this build (no auto-boot into a mission); background runs still work for verifying widescreen /
mods, since both log at `PSEPlugin_Load`, before any level loads.

## PSE ID database

`pse/ids/cv.ids.txt`, database 0 (unpublished, like TGE's). 5 ids: `Launcher_ModeFilter`, `File_Resolve`,
`File_Open`, `File_Exists` (function), `File_Flags` (global). `python scripts/gen_ids.py --verify-exe cv` checks all
5 against the real `Vietnam.exe`.

## Build

From `games/cv`: `cmake -S . -B build-pse -A Win32 -DCV_PSE_PLUGIN=ON && cmake --build build-pse --config RelWithDebInfo`.
From the repo root: `cmake --preset cv-pse && cmake --build --preset cv-pse` (release package: `cv-pse-dist`). The
standalone preset (`cv` / `cv-dist`) is unaffected - `CV_PSE_PLUGIN` defaults OFF.

## Live-verified 2026-10-24

Deployed over the installed standalone build (backed up, restored after) in the game folder, run in
the existing `Mods\EditorTest` mod and `HKCU\Software\VietnamFix\Dev` background mode. `VietnamFix.log`:
`pse.ids: cv database 0, build nocd-200409: 5 ids, 5 resolved, 0 mismatched`, `cvfix.widescreen` 1 patch applied,
`cvfix.mods` found `EditorTest` (2 files) and hooked all three file-layer sites through the broker
(`jmp@0x005308F5` / `0x0052FA17` / `0x0053154E`), 1 of 1 plugin loaded, the game ran to its `QuitAfterMs` self-exit
without taking the screen. Not yet verified: a mod file actually served in a running mission (the standalone build's
2026-10-10 "Huey stands in front of the start" check wasn't re-run here, since `frontend.cpp` / level boot aren't
built into this plugin yet - see "Scope" above) and a real player resolution list in the launcher dialog (background
mode OKs the launcher unseen).

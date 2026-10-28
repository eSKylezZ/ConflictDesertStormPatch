# Porting DesertStormFix / DesertStorm2Fix features to Conflict: Global Storm

GlobalStormFix started from VietnamFix's frame (itself from DS2). The DS1 feature sources to port from are DesertStormFix's own:
[games/ds1/src/features/](../ds1/src/features/) and [games/ds1/plugins/](../ds1/plugins/). GT port map with addresses from static analysis:
the Global Terror analysis notes (`GlobalTerror.md`, not in the repo) (section 5). SecuROM's VM runs ~1080 of
the game's functions, so hooks go through import slots / D3D / DirectInput objects where the code is virtualised.

## Done (2026-10-10)

| Feature | GT implementation |
| --- | --- |
| Plugin frame | exe id (timestamp 0x43032523, NoCD EP 0x2A8EB9), settings `HKLM\SOFTWARE\Pivotal\Conflict Global\Enhancements`, GlobalStormFix.log |
| Background test mode | devbg.cpp + tools/cgs/bg_run.py (import-slot hooks, key injection, Bink pacing, film skip) |
| Widescreen modes | both calls of the VM'd mode filter answer yes |
| HUD / text | high-res fonts from 1024 wide; menus squeezed to 4:3 (the mission HUD already fits) |
| D3D9 hooks | Direct3DCreate9 import slot 0x6C03B4 -> CreateDevice / device vtable, re-hooked every frame |
| Frame hook | 0x437BD7 / 0x437BD0 (game frame calls in WinMain) |

## Next candidates (from the port map)

Frame cap + sub-ms frame timing (`FUN_0055E2B0` clock, `FUN_0055E467` timer, hook point ready), high-fps checks,
skip intro (Bink: the BinkOpen / BinkWait hooks from devbg.cpp, made a setting), MSAA / anisotropy / V-Sync (D3D9
hooks in place), borderless window (`FUN_00435D16`), controller (DirectInput joystick slots `0x77192C`), Discord
presence, launcher settings column (dialogs 110 / 111 via the DialogBoxParamA import slot).

---

(Older notes from the Vietnam / DS2 copy follow.)

# Porting DesertStormFix features to Conflict: Vietnam

VietnamFix starts from DesertStormFix's plugin frame. The DS1 feature sources to port from are DesertStormFix's own:
[games/ds1/src/features/](../ds1/src/features/) and [games/ds1/plugins/](../ds1/plugins/) (DS1 byte patches: [games/ds1/scripts/patch_defs.py](../ds1/scripts/patch_defs.py)).
Each feature moves into `src/features/` and `CMakeLists.txt` once its DS2 addresses are mapped and checked.

## What carried over as-is

| Part | State |
| --- | --- |
| `dinput8.dll` proxy (`dllmain.cpp`, `dinput8.def`) | works: CDS II.exe imports DINPUT8.DLL; loaded, entry hook ran, game window opened (2026-10-10) |
| Entry-point hook | works on both builds: the retail exe enters SecuROM's loader (RVA 0x42CC17), the disc-free one a stub (RVA 0x3AC35D); `.text` is plain code in both, so patching before either is safe |
| `core/patch` (verified byte patches, call hooks) | builds; nothing patched yet |
| `core/settings` | key `HKLM\SOFTWARE\SCi Games\CDS II\Enhancements` (the game's own keys: `SOFTWARE\SCi Games\CDS II\` + `Device Settings`, `Window Placement`; value `Language`) |
| `core/log` | `VietnamFix.log` next to the exe |
| `core/gamepad` (XInput + Sony DirectInput, used by the launcher pad UI) | builds, game-independent |
| Prompt / controller icon assets + generators | copied (`scripts/gen_*`, `src/generated/`; the Kenney / controller sources = the repo root's `assets/`, shared by every game) |
| Lua 5.4.7 | the repo root's `third_party/lua` (shared; same 5.4.7 sources) for the script host; not built yet |

## DS1 -> DS2 address matching (first pass)

A masked byte-signature search (DS1 code around each patch site, rel32 / image addresses wildcarded) over DS2's
`.text` found almost nothing: DS2 is the same engine rebuilt (other code generation, struct offsets, new code), not a
patched copy. Only these matched:

| DS1 | DS2 | What |
| --- | --- | --- |
| `0x53C890` | `0x52B1E0` (unique) | image sheet scale setter (x, y, flag) - HUD scaling hook point |
| `0x4F7980` | 3 candidates `0x565100` / `0x565490` / `0x565910` | EVO object reader (check in Ghidra) |
| `0x55F540` | 3 candidates `0x55C570` / `0x55CD10` / `0x5601A0` | level sound cache (.sch) loader |

Everything else needs anchors instead: strings (e.g. `levelInitialise`, error texts, file names, registry key
names), imports (`Direct3DCreate9`, `QueryPerformanceCounter`, `DirectInput8Create`, `CreateWindowExA`), and call
structure from those. The Ghidra project has DS2 at `/ds2/CDS II.exe` (analysed: 7530 functions).

## Feature-by-feature

Order = rough value / effort. "Engine-generic" = the DS1 code needs only addresses; "D3D9" = the DS1 code talks to
Direct3D 8 and needs rework for DS2's Direct3D 9 renderer.

| Feature (DS1 file) | Port notes |
| --- | --- |
| Widescreen launcher modes (`widescreen.cpp`, patch_defs) | find DS2's launcher resolution filter (4:3 epsilon compare); check whether DS2 already allows widescreen |
| Frame timing clock + frame cap (`timing.cpp`, `framecap.cpp`) | find the QPC-based ms clock (x87 precision bug likely still there) and the per-frame dispatcher in the message loop |
| High-FPS physics / landing fixes (`timing.cpp`) | same engine (Karma, character mover) - re-find the mover's landing threshold and the world step call |
| HUD / text scaling (`hud.cpp`, cave in patch_defs) | hook point found (`0x52B1E0`); sheet layout (+0x20/+0x24/+0x28) to confirm; layout clamps must be re-generated (`gen_clamps.py` logic) |
| Controller remap + pad I/O + rumble (`controller.cpp`, `padio.cpp`, `rumble.cpp`, `hotplug.cpp`) | padio's XInput / HID / WGI code is game-independent; needs DS2's joystick slot array, poll function, FF table |
| Display mode windowed / borderless (`display.cpp`) | D3D9 present parameters; window creation / style loop |
| MSAA / anisotropy / V-Sync (`graphics.cpp`) | D3D9 rework (vtable indices differ from D3D8; `Direct3DCreate9` import) |
| Overlay icons / text (`overlay.cpp`, `fontsharp.cpp`) | D3D9 rework |
| Discord presence (`discord.cpp`) | IPC client is generic; needs level name / text lookup addresses; DS2's Discord app id to look up |
| Skip intro / cutscenes (`cinematics.cpp`) | DS2 uses Bink (`binkw32.dll`) films - different from DS1 |
| Mods folder / resolver (`mods.cpp`) | archive format is the same as DS1 (ModKit verified) - find the resolver / archive opener |
| Launcher settings UI + pad navigation (`launcher.cpp`, `launcher_pad.cpp`) | map DS2's launcher dialog ids |
| Split screen / co-op (`splitscreen.cpp`, `coop.cpp`) | DS2's PC build: check what of the console split-screen code survived (as in DS1) |
| Script host, editor support (`scripthost.cpp`, `campaigns.cpp`, ...) | DS2 has its own mission DLL API table - after the editor work |

## Verify each port

- Patch sites verified byte-for-byte by `patch::ApplyGroup` (a different exe skips the feature).
- Launch the game's exe in the install, read `VietnamFix.log`, kill the process afterwards.

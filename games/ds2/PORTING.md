# Porting DesertStormFix features to Conflict: Desert Storm II

DesertStorm2Fix starts from DesertStormFix's plugin frame. The DS1 feature sources are kept unbuilt in
[games/ds1-reference/ds1-features/](../ds1-reference/ds1-features/) (DS1 byte patches: [games/ds1-reference/patch_defs.py](../ds1-reference/patch_defs.py)).
Each feature moves into `src/features/` and `CMakeLists.txt` once its DS2 addresses are mapped and checked.

## Ported so far (2026-10-10)

| Feature | DS2 addresses | State |
| --- | --- | --- |
| Widescreen / 4K modes in the launcher (`widescreen.cpp`) | resolution whitelist `FUN_00420590` (640x480, 800x600, 1024x768, 1280x1024 only) -> `mov eax,1; ret`; list builder `FUN_00420900` | every adapter mode listed (verified up to 2560x1440 on a 1440p monitor; 4K appears with DSR) |
| HUD scaling (`hud.cpp`) | sheet scale setters `FUN_0052b1a0` (uniform, ret 4) / `FUN_0052b1e0` (x, y, flag, ret 0xC) jump to C++ copies x k = max(1, min(W/800, H/600)) * HudScalePercent; renderer `[0x680038]` W `+0x41A38` / H `+0x41A3C` | missions verified by background runs at 4K, 3440x1440, 2560x1440, 1920x1200, 1600x1200, 1280x1024 |
| Layout clamps (`generated/layout_clamps.h`, `tools/ds2/gen_clamps.py --cpp`) | 41 sites `c * max(800, W) / 800 * sheet scale` take the 800 path (incl. helper `FUN_0040c280` = max(800,W), used by the soldier panel's target icon); 15 raw-offset sites: forced path + their `mov X, 800` imm32 rewritten to 800*k at run time | portraits / target icons / panels aligned at every aspect |
| Text queue restore | loop at `0x50CBE0`: items set their own percent (100 -> k), `0x50CD6F` restores the saved (already scaled) value -> raw setter | |
| Small font `[0x62F438]` (ammo count, stats tables) | never handed to a setter (sheet stayed 1.0): set-up call `0x40C74E` also scales it, and text routine `FUN_005294e0` is entered through a stub that brings that font's sheet to k | ammo count scaled at 4K / 1440p |
| Front-end menus | designed for 1280x1024: pixel-sized 2D menus framed by a 3D monitor placed from the screen width (`FUN_00418af0`: distance W x 0.234375, globe scale 640/W, globe position table for W = 640/800/1024/1280) - lined up only at 1280x1024 | virtual 1280x1024 screen (`frontend.cpp` + `graphics.cpp`): renderer W/H fields + its viewport copy (+0x41A08.., `FUN_00537380` at 0x541F67) say 1280x1024 from device creation (re-set after engine init `0x4137E3`, front-end init `0x42CF9B`; real again at mission init `0x42CB64`); XYZRHW UP draws, viewports, scissors, clears mapped x min(W/1280, H/1024), centred; HUD k = 1 there; menu mouse `ScreenToClient` (0x4CB1B5, 0x4CB17C) mapped back. 4K = the 1280x1024 picture x2.11 |
| Menu structure (`menus.cpp`, `menus.h`) | state setter `FUN_0043dfd0` (current `[0x63433C]`), state -> screen `FUN_0043bba0` (states 1..0x35), `createGameMenus` `FUN_0043bf40`; main menu state 2 (vtable `0x5E6F58`: set-up `FUN_0045cd90`, update `FUN_0045ca50`), the unused single-player menu state 3 (vtable `0x5E7968`); items `FUN_0051d4f0`, list read `FUN_0051fa20`, Back query `FUN_0043af40`, text lookup `FUN_004fbe50` (UTF-16) | main menu SINGLE PLAYER / CO-OP / OPTIONS / QUIT; state 3 START GAME / LOAD GAME / TRAINING via the main menu's own handler; Back from their screens returns to it; CO-OP = state 0x36 "CO-OPERATIVE GAME" (hooks for the co-op work in `menus.h`). Verified by background runs with `Dev\MenuKeys` |

Dev test mode (`devbg.cpp`, dev builds only): `tools/ds2/bg_run.py <out> <WxH> [s] [level]` runs the game off-screen, windowed,
never focused, muted, with launcher + Settings OK'd unseen (`Dev\Resolution`; `DS_FORCE=WxH` rewrites the picked mode -
4K / 21:9 without such a monitor), "press any key" waits skipped (`0x40FB50`, `0x4440ED`), message boxes logged and
answered, cursor clip / moves dropped, back-buffer screenshots every 2 s (`Dev\ShotDir`). Restores the user's Device
Settings (VirtualStore copy) and dev values afterwards. Muting is remembered by Windows per app -> `Dev\MutedByTest`,
the next normal start unmutes. `DS_DEV="A=1;B=2"` adds dev values (`HudProbe`, `TextProbe`, `ClampSkip`). Menu navigation in tests: `DS_DEV="MenuKeys=S,D,S,W,B;MenuKeyMs=2500"`
(U / D / S select / B back / W wait; menus.cpp feeds them to the game's list reads and Back query, nothing goes to a window).

## What carried over as-is

| Part | State |
| --- | --- |
| `dinput8.dll` proxy (`dllmain.cpp`, `dinput8.def`) | works: CDS II.exe imports DINPUT8.DLL; loaded, entry hook ran, game window opened (2026-10-10) |
| Entry-point hook | works on both builds: the retail exe enters SecuROM's loader (RVA 0x42CC17), the disc-free one a stub (RVA 0x3AC35D); `.text` is plain code in both, so patching before either is safe |
| `core/patch` (verified byte patches, call hooks) | builds; nothing patched yet |
| `core/settings` | key `HKLM\SOFTWARE\SCi Games\CDS II\Enhancements` (the game's own keys: `SOFTWARE\SCi Games\CDS II\` + `Device Settings`, `Window Placement`; value `Language`) |
| `core/log` | `DesertStorm2Fix.log` next to the exe |
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
| Split screen / co-op (`splitscreen.cpp`, `coop.cpp`) | done 2026-10-10: 2-4 players + join screen on the menu feature's CO-OP screen - see "Split screen" below |
| Script host, editor support (`scripthost.cpp`, `campaigns.cpp`, ...) | DS2 has its own mission DLL API table - after the editor work |

## Verify each port

- Patch sites verified byte-for-byte by `patch::ApplyGroup` (a different exe skips the feature).
- Launch `H:\SCI Games\Conflict Desert Storm II\CDS II.exe`, read `DesertStorm2Fix.log`, kill the process afterwards.

## Split screen (2026-10-10, `splitscreen.cpp`, `splitscreen_layout.h`, `coop.cpp`)

DS2's PC build kept the same console remnants as DS1 (DS1 addresses in brackets). Verified by background runs: 2 / 3 / 4
views in Mission 1 at 1280x1024 and 1920x1080, each with its own camera, soldier, HUD (panels, weapon, compass) and grey
dividers; and the join flow main menu -> CO-OP -> 2 / 3 fake players -> START GAME path (profile, difficulty) ->
briefing film -> Mission 1 in 2 / 3 views (that run needed the menu feature's `menus.cpp`, copied in temporarily).

| What | DS2 | Notes |
| --- | --- | --- |
| split object | `0x626C18` (ctor `FUN_0040a2d0`, called at `0x5D75D5`) [`0x606410`] | +0 local MP flag, +4 player count, +8 mode (ctor 2; HUD code tests 3 / 4), +0xC orientation (`FUN_0040a290`), +0x10 HUD layout table, +0x14 (`[0x626C2C]`, another flag). Start-up option `-P` keeps the flag (`0x40B205`), otherwise cleared |
| layout tables (Xbox format, 0x80 B: offsets[4][2], views {used,x,y,w,h}[4] at +0x20, variant[4] at +0x70) | `0x6086D0` 1p, `0x608750` 2p top/bottom, `0x6087D0` 2p side by side (ctor default) [`0x5E6A48..`] | the table selectors are stripped; ours go into +0x10 with variant 0 for every view (the console variants 1-3 put the soldier panels at the view's top left, stacking upward out of it) |
| HUD anchors | `FUN_00459d10(out, view, anchor)` -> `FUN_00459c30` = view origin + view size x anchor + screen size x offset; anchor table `0x613FE0` float[variant][9][2] [`FUN_004a05d0`, `0x619A30`] | 11 callers; anchor 0 soldier panels (bottom left, stacking up), 1 weapon / ammo; compass top right |
| level dispatcher / level load | `FUN_0042cef0` (called through a vtable; mission -> `call FUN_0042cad0` at `0x42CFC1`; front-end `FUN_0042c930` at `0x42CF9B`) [`FUN_0040de70`] | `FUN_0042cad0`: input manager `0x62FE40` reset (`FUN_00428d40`), one block (`FUN_00428a90`), one more per player with the flag, then level init `FUN_00412690` |
| input blocks | block i = `0x62FF7C + i*0x58C` (manager +0x13C; count u16 `[0x6315B0]`) [`0x60F5B8 + i*0x478`] | +4 player object, +0xC devices (bit 0 keyboard/mouse, bit 1 pad; block init `FUN_00423710`; `0x42846C` clears the pad bit while no joystick exists), +0x2EC soldier, +0x39C binding set, +0x3A0, +0x3A4 joystick (-1 = block off), +0x448 view index, +0x41C / +0x42C last-input flags |
| players | `0x62F36C[4]` [`0x606A68`] | |
| player set-up | `FUN_0040c8b0` (level init `0x413247`; also `0x4144EC`, `0x449377`) [`FUN_0040bae0`] | per renderer viewport: camera `[[0x680120]+0x10+i*4]`, player i <- block i's soldier; `FUN_00422ed0` = thiscall(block, soldier) set the block's soldier; player 1's pick `FUN_00423620` at `0x40C94A` (squad member with owner == block +0x39C, lowest squad slot unit +0x1CF) - the loop for players 2..N is stripped |
| squad slot -> player | `FUN_00475610` (soldier +0xEC4 owner by player count, jump table `0x4756C4`), owner getter `FUN_00475970` [`FUN_00436f70`, +0x2B00] | 2p: slots 0+2 -> P1, 1+3 -> P2 (as DS1); 3p: P1 = Foley + Jones |
| renderer viewports | renderer `[0x680038]` +0x30 count, +0x34 D3DVIEWPORT9[] (0x18 each), full screen `+0x41A00`, current `+0x41A18`, back buffer +0x94 / +0x98, device +0x4187C; reset `FUN_00537380` (only at device creation, `0x541F67`), GetViewport `FUN_00536bb0`, SetViewport `FUN_00537440` [`+0x20/+0x24`, `FUN_00542e50`, `FUN_00547c30`, `FUN_00542f40`] | the builder (DS1 `FUN_00547b30`) is gone - the plugin writes the list. frontend.cpp's virtual screen writes entry 0's size (+0x3C/+0x40) at mission init, after the split set-up -> rewritten in the player set-up |
| render loop | `FUN_0040fba0` (from `0x410990`) [`FUN_0040ef70`] | per viewport: current view `[0x680134]`, `FUN_0040dd00(i)` world, view controllers `0x6800D0[4]`; full screen when `[[0x68008C]+0x1B8C]` bit 0, state `[0x63433C]` 0x15/0x19/0x2A/0x30, or split flag `[0x62F3A8]` = 0 (set by level init). HUD pass `FUN_0040dbc0` (per view: SetViewport, `FUN_0043aeb0(i)`); full viewport again at `0x40FF4A` |
| soldier panels | draw loop `FUN_00466920` over all blocks (anchor 0 of view i) -> panel list; per soldier `FUN_00467910` (callback `0x468570`, viewer block = this+0x128; skips soldiers whose owner != viewer +0x39C); set-up `0x468590` | vehicle passengers have no panel (4 players in the Mission 1 jeep: only the driver's) |
| bindings | table `[0x627078]` {sets, slots 3, actions 0x51, data}, allocated by `FUN_0040a3e0` called at `0x40A556` (`push esi` = 1 set) [`0x60687C`, `0x409F60`]; lookup `FUN_0040a470(set, slot, action)`; button eval `FUN_00422fa0` (checks +0xC) | 4 sets (call hooked), sets 1-3 = copies of set 0 at each split level load |
| analog | `FUN_004230f0` (thiscall block, action, opposite; 0x51 = none; axis floats `0x67E948 + (code + joystick*0x70)*4`, key held `FUN_004cc030`, keyboard-used `[0x62FE1C]`) [`FUN_00450920`] | no device check -> replaced by `AxisValue` (entry jump) |
| mouse look | smoother `FUN_004cacf0` on `0x680018`, read at `0x425BD8` / `0x428817` with esi = block [`FUN_00538080`, `0x44B396` / `0x44F8D7`] | stub: no movement for blocks without bit 0 |
| downed slow motion | tail jump `0x427816` -> "Slow down time" `FUN_0041e4b0` [`0x44EFAB` -> `FUN_00443d00`] | none in split screen |
| joysticks | DirectInput devices `0x67E7DC[8]`, count `[0x67F6C0]`, enumeration `FUN_004cc690` / callback `FUN_004cc370` (c_dfDIJoystick2 at `0x6009AC`), states from `0x67E828` (stride 0x1C0) | read raw by the join screen |
| front-end | state setter `FUN_0043dfd0`, state `[0x63433C]` (0x13 = mission running), screen per state `FUN_0043bba0`; main menu (state 2) update `FUN_0045ca50`, START GAME case `0x45CAA9` (item 0x3FD) | co-op Begin runs that case |
| per frame | message loop `FUN_00414ac0`: `0x414BFC` `FUN_00412040`, `0x414C01` `FUN_0040b2d0`, `0x414C06` `FUN_004cafe0` | coop.cpp hooks `0x414C06` (a frame cap port may want the same loop - merge carefully) |
| campaign flow | flow object `0x631608` (+0x10 entry index into `0x60C450`, 32-byte entries: Logo / AMODE / FIN / B01 briefing / mission01.dll / B02 ...; select by name `FUN_00430ec0`, current name `FUN_00430900`); `FUN_0042ccd0` plays the entry's Bink film | not needed (the START GAME path does it) |

Plugin (`splitscreen.cpp`): the level-load call `0x42CFC1` is wrapped - with 2-4 players wanted (co-op session or dev
`Dev\SplitPlayers`) it sets the split object (flag, count, orientation from `SplitScreenLayout`, our layout table),
writes the viewports and copies the binding sets before the game adds the input blocks; `0x40C94A` (`AssignPlayers`)
re-adds the players 2..N loop (block +4 = player, +0x448 = view, pad only; co-op session: each player's device and
joystick) and rewrites the viewports; the front-end init (`FUN_0042c930` entry, trampoline) and a 1-player load put
back one view. HUD art scale k comes from the smallest view (hud.cpp `features::SplitViewSize`). Dividers: Clear rects
after `0x40FF4A`.

Co-op (`coop.cpp`): fills the menu feature's CO-OPERATIVE GAME screen (`menus::SetCoopScreenHooks`; compiled only when
`features/menus.h` exists): rows PLAYER 1-4 (device), joining per device (keyboard Space / Backspace / Enter, pads
Cross-A / Circle-B / Options-Start), selecting a row or Start begins with 2+ players -> session + START GAME path. The
session ends on the main menu. Dev: `CoopFakeJoin` = n, `CoopAutoBegin` = ms, `CoopAutoOpen` = 1 (with the menu
feature's `MenuKeys` the whole flow runs in a background test).

Open: real pads (joining, P2-P4 movement / aiming) and keyboard joining need a human; in 3-player mode the half view
uses the quarter views' HUD scale; pause / objectives screens and cutscenes in split screen not checked in play;
vehicles with several players (only the driver has a panel; the Mission 1 jeep only drives on while an AI drives);
save / load of a co-op game (no co-op save naming yet).

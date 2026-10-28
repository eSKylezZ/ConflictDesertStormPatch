"""Byte patches the DLL applies to DesertStorm.exe (image base 0x400000), per feature.
Run  python scripts/gen_patch_tables.py  after changing anything here (regenerates src/generated/patch_tables.h).

Each patch: (name, VA, original bytes, patched bytes, any_original)
  original      - what the site must contain before patching (the DLL refuses to patch anything else)
  any_original  - site holds a value the DLL sets from settings (HUD scale, deadzone, pad layout): not checked
Code caves live in section padding that is mapped but unused: .text 0x5D7B00.., .rdata 0x5E5CB0..
"""


def ZEROS(n):
    return "00" * n


# HUD scaling cave at 0x5D7B04: compute_k = max(1, min(W/800, H/600)) * HudScale, hooks for the sheet-scale
# setters FUN_0053c890 / FUN_0053c850 (+ raw versions for save/restore callers), init for HUD art + font sheets.
HUD_CAVE = (
    "0AD7A33A0E74DA3A00000000A124C9630085C0742DDB8088060400D80D047B5D00DB808C060400D80D087B5D00DBF1DBC1DDD9D9"
    "E8DBF1DAC1DDD9D80D007B5D00C3D9E8EBF5E8C1FFFFFFEB02D9E8D9442404D80D64845D00D8C9D95924D9442408D80D64845D00"
    "DEC9D959288B44240C8179240000803F750B8179280000803F750233C0894120C20C00E87CFFFFFFEB02D9E8D9442404D80D6484"
    "5D00DEC9D95124D9592833C08179240000803F0F95C0894120C204008B0D1CEE600085C9740A680000C842E8BFFFFFFF8B0DB0ED"
    "600085C9740A680000C842E8ABFFFFFFE967C9E9FF"
)

# Launcher resolution list FUN_004487f0: drop the 4:3-only filter.
WIDESCREEN = [
    ("Launcher: allow non-4:3 display modes", 0x004488B3, "0F84A2000000", "909090909090", False),
]

# Resolution-scaled HUD / menus (setter hooks, save/restore callers, init).
HUD = [
    ("HUD: cave code", 0x005D7B04, ZEROS(229), HUD_CAVE, False),
    ("HUD: scale multiplier", 0x005D7B00, "00000000", "00000000", True),
    ("HUD: scale setter (x,y) -> hook", 0x0053C890, "D9442404D8", "E9B5B20900", False),
    ("HUD: scale setter (uniform) -> hook", 0x0053C850, "D9442404D8", "E93AB30900", False),
    ("HUD: scale HUD art + font on load", 0x00410CC3, "E888380600", "E8F46E1C00", False),
    ("HUD: map scale save/restore 1", 0x00454B68, "E8237D0E00", "E8E42F1800", False),
    ("HUD: map scale save/restore 2", 0x00454BC3, "E8C87C0E00", "E8892F1800", False),
    ("HUD: map scale save/restore 3", 0x00454C37, "E8547C0E00", "E8152F1800", False),
    ("HUD: map scale save/restore 4", 0x00454CDC, "E8AF7B0E00", "E8702E1800", False),
    ("HUD: map scale save/restore 5", 0x004551C3, "E8C8760E00", "E889291800", False),
    ("HUD: map scale save/restore 6", 0x00455247, "E844760E00", "E805291800", False),
    ("HUD: text queue scale restore 1", 0x00508068, "E8E3470300", "E829FB0C00", False),
    ("HUD: text queue scale restore 2", 0x0050837E, "E8CD440300", "E813F80C00", False),
    ("HUD: keep scale flag after restore", 0x0050838C, "896820", "909090", False),
]

# Joypads: the button / axis fix-up after GetDeviceState is C++ (src/features/controller.cpp); here only the
# stick deadzone the DLL sets from settings.
CONTROLLER = [
    ("Pad: stick deadzone", 0x00538997, "A00F0000", "A00F0000", True),
]

# Game clock FUN_004ba370: exact integer QPC -> ms (was 24-bit float, turbo at high FPS).
TIMING = [
    ("Timing: exact game clock (FUN_004ba370)", 0x004BA370,
     "83EC088D44240050FF15F4805D00DF6C240083C408D80D6C845D00DF2DA03A6200DEF9E93CCB0A009090",
     "505054FF15F4805D00585A8B0DA03A6200F7F169C0E80300005068E803000092F72424F7F1595903C1C3",  # QPC/f*1000 + QPC%f*1000/f, f = QPF low dword
     False),
]

# Own gunshots: the shot sound in FUN_004a1b80 was played without a position (flat 2D, full level in both
# speakers) while the shooter's input block has +0x318 = 1 (aiming) -> louder than the positional shot when not
# aiming. The branch picking the 2D call always takes the positional one.
AUDIO = [
    ("Audio: own gunshots stay positional while aiming", 0x004A1D71, "740A", "EB0A", False),
]

# Scene loader (FUN_004f8d00) "Unable to find parent - Object %s / Parent %s": the object's name came from
# FUN_004d12a0 (its internal id as "0x%X"); the record's own name ([esp+0x4e4], read by FUN_004bf6f0) instead.
# Same idea as a third-party modded exe ("DesertStorm_mem.exe").
SCENE_MESSAGES = [
    ("Scene loader: object name in the missing-parent message", 0x004F9485, "8BCB50E8137EFDFF50",
     "508D8CE4E804000051", False),
]

# CHEATS > COMMANDEER ANY VEHICLE (with the enterable filter in cheats.cpp, hook at 0x433B98):
# - seat finder FUN_0043cfd0 refuses a vehicle with a soldier of another team in any seat (jne at 0x43D039);
# - vehicle set-up FUN_00437cd0 / FUN_00438db0 / FUN_00439350: only vehicles whose render instance has flag 0x200000
#   load their sound bank (0x438598) and build their physics body (FUN_00438a80(dyn, 1) at 0x438DE5 / 0x439385) -
#   Mission 4's T-62 and open Ural have none; these three take effect at the next level load.
# (Same changes as a third-party modded exe, "DesertStorm_oct_ai_voc.exe".)
COMMANDEER = [
    ("Cheats: board vehicles with enemy crew", 0x0043D039, "0F85E1000000", "909090909090", False),
    ("Cheats: every vehicle's sound bank", 0x00438598, "741C", "9090", False),
    ("Cheats: every vehicle's physics body (1)", 0x00438DE5, "741B", "9090", False),
    ("Cheats: every vehicle's physics body (2)", 0x00439385, "741B", "9090", False),
]

# Speech FUN_004366d0(sample, quiet sample, AI only, …): lines asked for "AI only" (arg 3 = 1: I'm hit, enemy killed,
# destroyed vehicle, grenade throw in single player) are skipped for the player-driven soldier (vtable +0x4C(1) at
# 0x43672E, test at 0x436731) - "test eax, eax" -> "xor eax, eax": the soldier you control says them too.
# Spotted callouts FUN_00407ff0(soldier, target) (from the target tracking FUN_00408680, which runs for the player's
# soldier too) start with the same test (0x407FFB): your soldier calls out infantry / vehicles / tanks / guns /
# helicopters, with the game's cooldowns (60 s per target, 20 s per kind).
# Incoming grenade: an enemy throwing at a player-driven soldier makes every squadmate within range of him shout it
# (team loop 0x404FBD in the throw code), skipping the target himself (0x404FBF je) - now he shouts it too.
# Man down: FUN_00423f70 (from TakeDamage 0x4243CF, a soldier going down) has the first team member within 20 m say it,
# skipping player-driven soldiers (0x423FFD jne) - now the soldier you control can be that one.
# (Watch your fire needs nothing: a squadmate says it when the player's soldier fires close past him.)
PLAYER_VOICE = [
    ("Voices: the soldier you control talks", 0x00436731, "85C0", "31C0", False),
    ("Voices: the soldier you control calls out enemies", 0x00407FFB, "85C0", "31C0", False),
    ("Voices: the soldier you control shouts incoming grenade", 0x00404FBF, "0F8499000000", "909090909090", False),
    ("Voices: the soldier you control says man down", 0x00423FFD, "7573", "9090", False),
]

# Pivotal's debug key map: FUN_00447150 (73 callers, registration from 0x4108F9 at start-up) registers a binding only
# with start-up flag M - always registered now; the per-frame dispatcher's flag check (0x4473B5) is hooked in C++.
DEBUG_KEYS = [
    ("Developer: debug key bindings registered", 0x0044715C, "7501", "EB01", False),
]

# Engine limits for big battles (limits.cpp has the hooks; research/large_battles.md). Pools sized at start-up / level
# init; Global Heap pools get their room in memory.cpp (features::LimitsHeapExtra).
# - Karma world (FUN_004fdf70): MdtWorldCreate(bodies 200, constraints 500 - joints AND contacts share them; callers
#   never check the null an exhausted pool returns -> crash) -> 2048 / 8192. McdInit models 400 -> 4096, McdSpace
#   AxisSort(axes, objects 400, pairs 400 - "Resizing not implemented" = abort) -> 4096 / 16384.
# - particles (FUN_004f0910, budget in bytes, 200 per particle, Global Heap) 50000 -> 400000 = 2000 particles.
# - event queue (FUN_004db610, 0x48 B each, Global Heap; timers, queue_call, delayed events; full = dropped) 200 -> 2048.
# - render command arena (FUN_0051f770, Global Heap, per level; full = draws skipped) 64 KB -> 1 MB.
# - script-id table (object manager +0x30): not built when the highest id is over 1000 (`cmp ebx, 0x3E8` at
#   0x4CA4F9; then every by-id lookup walks all objects and the plugin's table reads fail) -> 65534.
# - waypoint table [0x63C9B0] (600 x 0x20 B + count at +0x4B00, indexed by id without a check) -> 8192 entries: the
#   allocation (0x4102BD) and the 12 users of the count field (all in the waypoint code) move the count to +0x40000.
# - save buffer (saver ctor 0x415480, filled without an end check): 0x2AF53 base + 4 MB; limits.cpp writes only the
#   used part (never less than the original size, so ordinary saves stay byte-identical).
# - AI think scheduler [0x60EDA0] (FUN_00401590): the adaptive soldier budget was clamped to 2 per frame (200 awake
#   soldiers = each one every 1.7 s at 60 fps) -> 32 (it still only rises while a full cycle takes over 500 ms).
# - router A* searches per frame (+0x30, set at 0x509AC8) 3 -> 12.
# - positional sound voices in software mode (no hardware 3D buffers - every modern PC; 0x55F228) 6 -> 32.
# - decal store ring (footprints, bullet holes, blood; manager [0x63C96C] +0x40, push at 0x521BDC) 128 -> 512 KB.
WAYPOINT_COUNT_SITES = [0x401CAC, 0x401DDF, 0x40DA3F, 0x413B4D, 0x4D13D2, 0x4D14C0, 0x4D14F7, 0x4D1517, 0x4D1534,
                        0x4D1546, 0x4D1557, 0x4D155E]
LIMITS = [
    ("Limits: Karma constraints 500 -> 8192", 0x004FDFCE, "F4010000", "00200000", False),
    ("Limits: Karma bodies 200 -> 2048", 0x004FDFD6, "C8000000", "00080000", False),
    ("Limits: collision models 400 -> 4096", 0x004FE098, "90010000", "00100000", False),
    ("Limits: collision pairs 400 -> 16384", 0x004FE0B9, "90010000", "00400000", False),
    ("Limits: collision space objects 400 -> 4096", 0x004FE0BE, "90010000", "00100000", False),
    ("Limits: particle budget 50000 -> 400000 bytes", 0x004104E6, "50C30000", "801A0600", False),
    ("Limits: event queue 200 -> 2048", 0x00410528, "C8000000", "00080000", False),
    ("Limits: render command arena 64 KB -> 1 MB", 0x004CB2D6, "00000100", "00001000", False),
    ("Limits: script-id table up to id 65534", 0x004CA4FB, "E8030000", "FEFF0000", False),
    ("Limits: waypoint table 600 -> 8192 (allocation)", 0x004102BD, "044B0000", "04000400", False),
] + [
    (f"Limits: waypoint count field 0x{va:X}", va + 2, "004B0000", "00000400", False) for va in WAYPOINT_COUNT_SITES
] + [
    ("Limits: save buffer + 4 MB", 0x004154A7, "53AF0200", "53AF4200", False),
    ("Limits: AI think budget clamp 2 -> 32 (compare)", 0x004016BC, "02", "20", False),
    ("Limits: AI think budget clamp 2 -> 32 (store)", 0x004016C2, "02000000", "20000000", False),
    ("Limits: A* searches per frame 3 -> 12", 0x00509ACB, "03000000", "0C000000", False),
    ("Limits: positional sound voices 6 -> 32", 0x0055F22B, "06000000", "20000000", False),
    ("Limits: decal store 128 KB -> 512 KB", 0x00521BDD, "00000200", "00000800", False),
]

# Layout clamps (part of HUD): layout code computes c * max(800, W)/800 * sheetScale; with the resolution factor
# in the sheet scale that double-scaled, so max(800, W) is forced to 800 (jle -> jmp, jg -> nop nop).
# VA  original  patched
LAYOUT_CLAMPS = """
0040B0C9 7E02 EB02   0040B12E 7E02 EB02   004144E9 7E02 EB02
004144FF 7F05 9090   0044944A 7E02 EB02   00449457 7F05 9090
004495A6 7E02 EB02   004495B3 7F05 9090   0044BE1B 7F05 9090
004516DB 7F05 9090   00451735 7F05 9090   0045177D 7F05 9090
004518E1 7F05 9090   0045193B 7F05 9090   00451983 7F05 9090
00451AC4 7F05 9090   00451B1E 7F05 9090   00451B66 7F05 9090
00451D09 7F05 9090   00451D4E 7F05 9090   00451DB9 7E02 EB02
00451DDB 7E02 EB02   00451EB3 7F05 9090   00451F2C 7E02 EB02
00451F4D 7E02 EB02   00451F63 7E02 EB02   00452069 7F05 9090
0045235A 7F05 9090   004523B4 7F05 9090   004523FC 7F05 9090
004537F3 7F05 9090   0045407A 7E02 EB02   00454087 7F05 9090
00459999 7E02 EB02   004599A6 7F05 9090   00459D26 7E02 EB02
00459D39 7F05 9090   00459E55 7F05 9090   0046B4E9 7E02 EB02
0046D170 7E02 EB02   0046D18B 7E0F EB0F   0047062B 7E02 EB02
00470644 0F8EA9010000 90E9A9010000   0047065D 7F05 9090   00474F1B 7E02 EB02
00475532 7E04 EB04   004761CD 7E02 EB02   00479186 7E04 EB04
0047919A 7E02 EB02   004791AC 7E02 EB02   004791BE 7E02 EB02
004791CB 7F05 9090   0047A50D 7F05 9090   0047A555 7F05 9090
0047A59D 7F05 9090   0047D4CE 7E02 EB02   0047D4E5 7F05 9090
0047D5BF 7E02 EB02   0047D5EB 7E02 EB02   0047D6E8 7E02 EB02
0047D714 7E02 EB02   0047D837 7E02 EB02   0047D863 7E02 EB02
0047D878 7E02 EB02   0047D9E4 7E02 EB02   0047DA10 7E02 EB02
0047DA25 7E02 EB02   0047DBE0 7E02 EB02   0047DBF7 7F05 9090
0047DCC8 7E02 EB02   0047DCE3 7E02 EB02   0047DDD5 7E02 EB02
0047DDF4 7E02 EB02   0047DEDC 7E02 EB02   0047DEFB 7E02 EB02
0047E009 7E02 EB02   0047E028 7E02 EB02   0047E03D 7E02 EB02
0047E194 7E02 EB02   0047E1B3 7E02 EB02   0047E1C8 7E02 EB02
0047E779 7E02 EB02   0047E78C 7F05 9090   0047E809 7E02 EB02
0047E823 7E02 EB02   00480674 7F05 9090   004806BD 7F05 9090
00480700 7F05 9090   0048075A 7E02 EB02   004807FD 7E02 EB02
00480810 7F05 9090   0048D53F 7E04 EB04   0048D54E 7F05 9090

"""

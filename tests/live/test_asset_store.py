"""Live: the PSE asset store + pse.vfs (src/features/assetstore.cpp, docs/sdk/asset-store.md).

Starts its OWN game (dev values are read at start-up, so it can't share the session game):
  Dev\\PseStore = 1 (store first + global fallback), Dev\\PseNoBorrow = 1 (mods.cpp copies nothing from other levels),
  VehiclesSPMission9 = "US_VEH_M2A1_Bradley=IR_VEH_BMP-2" (the BMP-2 is not in mission9.dat).
Checks: the store is built / opened at start-up; in Mission 1 the game's own whole-file reader gets APACHE_AH64.EVO, which
only mission3.dat ships, byte-identical to mission3.dat's copy; Mission 3 and Mission 1 play; Mission 9 builds the
swapped-in BMP-2 from other levels' files served by the store (no Borrow copies).
PSE_STORE_CONTROL=1 adds the control run (Dev\\PseStore = 2: store without the fallback, still no Borrow copies): the
Apache read fails and the BMP-2's files are missing.
Afterwards: the store cache the run created (<game>\\PSE\\Cache) is removed, dev / settings values are restored.
"""
import hashlib, os, shutil, sys, time, winreg

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "tools"))
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)
import pse_client  # noqa: E402

try:
    import bg  # noqa: E402
    from io_scene_evo import archive  # noqa: E402
    from io_scene_evo.evo import archive_hash  # noqa: E402
except ImportError:
    pytest.skip("needs tools/bg.py (local tooling)", allow_module_level=True)

SWAP_NAME = "VehiclesSPMission9"
SWAP = "US_VEH_M2A1_Bradley=IR_VEH_BMP-2"
ONLY_IN_M3 = "APACHE_AH64.EVO"     # (pse_build --find: mission3.dat only)
VEHICLE_VTABLE = 0x5D8A58

READ_LUA = """
local name = %r
local exists = dev.call(0x4BFC80, "cdecl", name)      -- FUN_004bfc80: the game's exists check
local out = dev.alloc(16)
local ok = dev.call(0x4BFB90, "cdecl", name, out, out + 4, 0, 0)   -- whole-file reader, game allocator
local buf, size = dev.u32(out), dev.u32(out + 4)
local hex = ""
if buf ~= 0 and size > 0 then hex = dev.bytes(buf, size) end
if buf ~= 0 then dev.call(0x4B8510, "cdecl", buf) end   -- the game allocator's free
dev.free(out)
return {exists = exists, ok = ok, size = size, hex = hex}
"""

VEHICLES_LUA = """   -- model names (+0x24 -> +8) of every vehicle object
local mgr = dev.u32(0x63C9A0)
local tab = mgr ~= 0 and dev.u32(mgr + 0x30) or 0
local units = {}
if tab == 0 then return units end
for i = 0, 699 do
  pcall(function()   -- (slots of removed objects can hold stale pointers)
    local v = dev.u32(tab + i * 4)
    if v ~= 0 and dev.u32(v) == %d then
      local u = dev.u32(v + 0x24)
      units[#units + 1] = u ~= 0 and dev.cstring(u + 8, 64) or "?"
    end
  end)
end
return units
""" % VEHICLE_VTABLE


def archive_file(dat, name):
    data = open(os.path.join(bg.GAME, dat), "rb").read()
    h = archive_hash(name)
    for hh, o, s in archive.entries(data):
        if hh == h:
            return data[o:o + s]
    return None


def log_lines(c):
    return c.log(5000) or []


def since(lines, marker):
    """Lines after the last one containing marker (this level's part of the log)."""
    idx = max((i for i, l in enumerate(lines) if marker in l), default=-1)
    return lines[idx + 1:] if idx >= 0 else lines


class StoreGame:
    def __init__(self, mode):
        self.mode = mode

    def __enter__(self):
        self.cache = os.path.join(bg.GAME, "PSE", "Cache")
        self.had_cache = os.path.isdir(self.cache)
        bg.ensure_settings_migrated()
        self.old_swap = bg._reg(bg.ENH_KEY, SWAP_NAME)
        bg._reg(bg.ENH_KEY, SWAP_NAME, SWAP, winreg.REG_SZ)
        self.g = bg.Game(dev={"StartLevel": "", "SplitPlayers": 0, "PseStore": self.mode, "PseNoBorrow": 1,
                              "PseRebuild": 0}, enh={"FeatLoadout": 1})
        self.g.__enter__()
        t0 = time.time()
        pid = pse_client.wait_for_pipe(self.g.p.pid, timeout=120)
        self.c = pse_client.Client(pid)
        self.c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
        print("\n[store] game %d (PseStore=%d) at the main menu after %.1f s" % (pid, self.mode, time.time() - t0))
        return self

    def __exit__(self, *exc):
        try:
            self.c.close()
        except Exception:
            pass
        self.g.__exit__(None, None, None)
        if self.old_swap is None:
            bg._reg(bg.ENH_KEY, SWAP_NAME, delete=True)
        else:
            bg._reg(bg.ENH_KEY, SWAP_NAME, self.old_swap, winreg.REG_SZ)
        if not self.had_cache:
            for _ in range(10):   # the game's handle on store.pack goes with the process
                shutil.rmtree(self.cache, ignore_errors=True)
                if not os.path.isdir(self.cache):
                    break
                time.sleep(0.5)
            assert not os.path.isdir(self.cache), "couldn't remove the store cache " + self.cache


@pytest.fixture(scope="module")
def sg():
    with StoreGame(1) as s:
        yield s


def test_store_is_on(sg):
    lines = log_lines(sg.c)
    on = [l for l in lines if "Asset store: ON" in l]
    assert on, "no 'Asset store: ON' line - " + " / ".join(l for l in lines if "Asset store" in l)[-500:]
    print("[store]", on[-1])
    assert "global fallback" in on[-1]
    for l in lines:
        if "Asset store: asset store:" in l or "Asset store: building" in l:
            print("[store]", l)
    assert os.path.isfile(os.path.join(sg.cache, "assets", "store.pack"))
    assert os.path.isfile(os.path.join(sg.cache, "assets", "stamp.txt"))


def test_mission1_reads_a_file_only_mission3_ships(sg):
    c = sg.c
    p = c.start_level("Mission1.dll")
    assert p and p["soldier"], "Mission 1 didn't reach play"
    want = archive_file("mission3.dat", ONLY_IN_M3)
    assert want and archive_file("mission1.dat", ONLY_IN_M3) is None
    r = c.run(READ_LUA % ONLY_IN_M3, timeout=60)
    print("[store] Mission 1: exists %s, read %s, %d bytes" % (r["exists"], r["ok"], r["size"]))
    assert r["exists"] == 1
    assert r["size"] == len(want)
    got = bytes.fromhex(r["hex"])
    assert hashlib.sha256(got).hexdigest() == hashlib.sha256(want).hexdigest(), "bytes differ from mission3.dat's"
    served = [l for l in log_lines(c) if "Asset store: " + ONLY_IN_M3 in l]
    print("[store]", served[-1] if served else "(no log line)")
    assert served and "from mission3.dat" in served[-1]
    assert c.frames_advancing(1.0)


def test_mission3_plays_from_the_store(sg):
    """PseStore = 1 reads EVERY archive file from store.pack (store first), not only the fallback ones."""
    c = sg.c
    p = c.start_level("Mission3.dll")
    assert p and p["soldier"]
    time.sleep(3)
    assert c.frames_advancing(1.0)
    lines = since(log_lines(c), "Console: level load Mission3.dll") or log_lines(c)
    served = [l for l in lines if "files served from the store so far" in l and "from mission3.dat)" in l]
    print("[store]", served[-1] if served else "(no served line)")
    assert served, "Mission 3's own files didn't come from store.pack"
    errors = [l for l in lines if "[game Error]" in l or "[fail] Asset store" in l]
    assert not errors, errors[:5]


def test_mission9_vehicle_swap_needs_no_borrow_copies(sg):
    c = sg.c
    p = c.start_level("Mission9.dll", timeout=240)
    assert p and p["soldier"]
    time.sleep(2)
    lines = log_lines(c)
    swap = [l for l in lines if "Vehicles: Mission9.dll - " in l]
    borrow = [l for l in lines if "Vehicles: IR_VEH_BMP-2 for Mission9" in l]
    fetched = [l for l in lines if "for mission9.dat from" in l and "Asset store:" in l]
    print("[store]", swap[-1] if swap else "(no swap line)")
    print("[store]", borrow[-1] if borrow else "(no borrow line)")
    print("[store] %d file(s) for mission9.dat from other archives, e.g.:" % len(fetched))
    for l in fetched[:12]:
        print("   ", l.split("Asset store: ", 1)[1])
    assert swap and "1 swap(s)" in swap[-1]
    assert borrow and " 0 file(s) copied" in borrow[-1], "Borrow* copied files although PseNoBorrow is on"
    assert any("BMP2.EVO (" in l.upper() for l in fetched), "the BMP-2 model didn't come from the store"
    units = c.run(VEHICLES_LUA)
    print("[store] vehicles in Mission 9:", sorted(set(units)))
    assert "Bmp2" in units, "no BMP-2 (model Bmp2) in the level"
    errors = [l for l in since(lines, "Console: level load Mission9.dll") if "[game Error]" in l and "BMP" in l.upper()]
    assert not errors, errors[:5]
    assert c.frames_advancing(1.0)


@pytest.mark.skipif(os.environ.get("PSE_STORE_CONTROL") != "1", reason="control run: PSE_STORE_CONTROL=1")
def test_control_without_fallback(sg):
    """Store without the global fallback (and still no Borrow copies): what the fallback provides goes missing.
    Seen 2026-10-19: "[file] Bmp2.evo - NOT FOUND", "[fail] Vehicles: model Bmp2 didn't load", no BMP-2 in Mission 9."""
    sg.__exit__(None, None, None)   # this module's game first (one game at a time)
    with StoreGame(2) as s2:
        c = s2.c
        c.start_level("Mission1.dll")
        r = c.run(READ_LUA % ONLY_IN_M3, timeout=60)
        print("[control] Mission 1: exists %s, %d bytes" % (r["exists"], r["size"]))
        assert r["exists"] == 0 and r["size"] == 0
        try:
            c.start_level("Mission9.dll", timeout=240)
            alive = True
        except Exception as e:   # the game may not survive a vehicle without its files
            print("[control] Mission 9:", e)
            alive = False
        if alive:
            lines = log_lines(c)
            m9 = since(lines, "Console: level load Mission9.dll")
            missing = [l for l in m9 if "BMP2" in l.upper() and "NOT FOUND" in l]
            for l in [l for l in m9 if "BMP2" in l.upper() or "Asset store" in l][:40]:
                print("[control]  ", l)
            print("[control] BMP-2 files not found:", len(missing))
            units = c.run(VEHICLES_LUA)
            print("[control] vehicles:", sorted(set(units)))
            assert missing and "Bmp2" not in units

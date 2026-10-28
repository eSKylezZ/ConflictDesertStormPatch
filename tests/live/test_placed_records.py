"""Live: placed-reference edits from a mod's records\\*.lua reach the game (pse/records/placed.h, mods.cpp ApplyRecords;
docs/sdk/data-files.md "Changing placed references").

A test mod Mods\\zz_PlacedTest\\records\\edits.lua (made before the game starts: the mod index is built at start-up)
moves Mission 1's MEDIKIT_266 (an item that stays where it is put) 2 m along x, copies BASE_GUARD_1 (script id 4) as
GATE_EXTRA_GUARD (the next free script id, 310), takes ALCOVE_PATROL (6) out of play and gives waypoint CELL_BLOCK_GUARD_0 (1) a 7777 ms wait. In Mission 1 the
game's own tables must show it: object table [[0x63C9A0]+0x30][id] (+0x30 position in cm, +0xC bit 0 = in play) and
the waypoint table [0x63C9B0] (0x20-byte entries by id, +0x1C wait ms). The mod is removed afterwards.
Needs tools/bg.py (local tooling); uses the installed build.
"""
import os, shutil, sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import pse_client  # noqa: E402

try:
    import bg  # noqa: E402
except ImportError:
    pytest.skip("needs tools/bg.py (local tooling)", allow_module_level=True)

GUARD = (-20417.34, 753.09, 42756.30)   # BASE_GUARD_1 in the shipped MISSION1.ENV (he walks off once play starts)
MEDIKIT = (-15430.2, 156.54, 29733.1)   # MEDIKIT_266
EDITS = """return {
  { id = "ds1:mission1/object/medikit_266", pos = { x = %.2f, y = %.2f, z = %.2f } },
  { id = "ds1:mission1/object/gate_extra_guard", based_on = "ds1:mission1/object/base_guard_1" },
  { id = "ds1:mission1/object/alcove_patrol", delete = true },
  { id = "ds1:mission1/waypoint/cell_block_guard_0", wait_ms = 7777 },
}
""" % (MEDIKIT[0] + 200, MEDIKIT[1], MEDIKIT[2])
OBJ = ("local t = dev.u32(dev.u32(0x63C9A0) + 0x30) local o = dev.u32(t + %d * 4) if o == 0 then return {} end "
       "return {dev.f32(o + 0x30), dev.f32(o + 0x34), dev.f32(o + 0x38), dev.u32(o + 0xC) & 1}")


@pytest.fixture(scope="module")
def run():
    mod = os.path.join(bg.GAME, "Mods", "zz_PlacedTest")
    if os.path.exists(mod):
        shutil.rmtree(mod)
    os.makedirs(os.path.join(mod, "records"))
    with open(os.path.join(mod, "records", "edits.lua"), "w", encoding="utf-8") as f:
        f.write(EDITS)
    res = {}
    try:
        dev = {"StartLevel": "", "SplitPlayers": 0, "NoErrorBoxes": 1}
        enh = {"FeatLoadout": 0, "SkipCutscenes": 1, "FpsCap": 60, "FeatMods": 1}
        with bg.Game(dev=dev, enh=enh) as g:
            c = pse_client.Client(pse_client.wait_for_pipe(g.p.pid, timeout=120))
            c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
            c.start_level("Mission1.dll", wait=True, timeout=240)
            for sid in (266, 6, 310):
                res[sid] = c.run(OBJ % sid)
            res["wait"] = c.run("return dev.u32(dev.u32(0x63C9B0) + 1 * 0x20 + 0x1C)")
            res["log"] = c.log(8000) or []
            c.close()
    finally:
        shutil.rmtree(mod, ignore_errors=True)
    print("\n[placed]", {k: v for k, v in res.items() if k != "log"})
    return res


def test_scene_built_from_the_edits(run):
    lines = [l for l in run["log"] if "Records:" in l]
    for l in lines:
        print("[placed]", l)
    assert any("MISSION1.ENV = the game's scene + placed reference edits" in l for l in lines)
    assert any("new ds1:mission1/object/gate_extra_guard based on ds1:mission1/object/base_guard_1, script id 310" in l
               for l in lines)
    assert not any("[fail]" in l for l in lines)


def test_item_moved(run):
    x, y, z, in_play = run[266]
    assert abs(x - (MEDIKIT[0] + 200)) < 5 and abs(z - MEDIKIT[2]) < 5, run[266]
    assert in_play == 1


def test_copy_exists_where_the_guard_was(run):
    assert run[310], "no object with script id 310"
    x, y, z, in_play = run[310]
    assert abs(x - GUARD[0]) < 5 and abs(z - GUARD[2]) < 5, run[310]
    assert in_play == 1


def test_deleted_one_is_out_of_play(run):
    assert run[6] and run[6][3] == 0, run[6]


def test_waypoint_wait(run):
    assert run["wait"] == 7777

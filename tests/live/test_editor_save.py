"""Live, end to end: the Conflict Editor (ModKit, headless) edits Mission 1 and saves it with save_mod - changed /
copied objects and waypoints as record edits (records\\mission1.placed.lua), a new waypoint as MISSION1.envpatch - and
the game builds the level from both (pse/records/placed.h + scenepatch.h, mods.cpp).

Edits made with the editor's own commands: MEDIKIT_266 moved 2 m, BASE_GUARD_1 duplicated (its copy gets the next
script id and its own name), ALCOVE_PATROL out of play (set_field in_play 0), waypoint 1's wait 7777 ms, waypoint 1
duplicated (a new waypoint: the .envpatch). Read back from the game's object table [[0x63C9A0]+0x30][id] and waypoint
table [0x63C9B0] (0x20 bytes per id: +0x1C wait, +0x18 next). The mod Mods\\zz_EditorE2E is removed afterwards.
Needs tools/bg.py, the ModKit next to the repo and the editor's Desert Storm extraction (%LOCALAPPDATA%\\ConflictEditor).
"""
import os, shutil, sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
MODKIT = os.path.join(os.path.dirname(REPO), "ConflictDesertStormModKit")
DATA = os.path.join(os.environ.get("LOCALAPPDATA", ""), "ConflictEditor", "ds1", "extracted")
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, MODKIT)
sys.path.insert(0, os.path.join(MODKIT, "editor"))
import pse_client  # noqa: E402

try:
    import bg  # noqa: E402
    import commands as ed  # noqa: E402
except ImportError:
    pytest.skip("needs tools/bg.py and the ModKit", allow_module_level=True)
if not os.path.isdir(os.path.join(DATA, "mission1")):
    pytest.skip("needs the editor's Desert Storm extraction", allow_module_level=True)

MOD = "zz_EditorE2E"
OBJ = ("local t = dev.u32(dev.u32(0x63C9A0) + 0x30) local o = dev.u32(t + %d * 4) if o == 0 then return {} end "
       "return {dev.f32(o + 0x30), dev.f32(o + 0x34), dev.f32(o + 0x38), dev.u32(o + 0xC) & 1}")
WP = "local w = dev.u32(0x63C9B0) + %d * 0x20 return {dev.u32(w + 0x1C), dev.i32(w + 0x18), dev.f32(w)}"


@pytest.fixture(scope="module")
def run():
    folder = os.path.join(bg.GAME, "Mods", MOD)
    shutil.rmtree(folder, ignore_errors=True)
    s = ed.Session(DATA, bg.GAME)
    call = lambda name, **a: s.call(name, a)   # noqa: E731
    call("open_level", level="MISSION1")
    line = lambda kind, **m: s.env.find(kind, **m)[0].index + 1   # noqa: E731
    med = s.env.find("HLOI", script_id=266)[0]["pos"]
    call("move", line=line("HLOI", script_id=266), pos=[med[0] + 200, med[1], med[2]])
    copy = call("duplicate", line=line("HLOI", script_id=4))
    call("set_field", line=line("HLOI", script_id=6), field="in_play", value=0)
    call("set_field", line=line("NEWENVWAYPT", id=1), field="wait_ms", value=7777)
    wcopy = s.record(call("duplicate", line=line("NEWENVWAYPT", id=1))["line"])
    res = {"med": med, "copy_sid": copy["script_id"], "copy_stat": s.record(copy["line"])["stat"],
           "wp_new": wcopy["id"], "guard": s.env.find("HLOI", script_id=4)[0]["pos"]}
    try:   # from the save on: the mod is removed whatever fails (a left-over zz_EditorE2E changes other tests' scenes)
        res["save"] = call("save_mod", mod=MOD)
        res["records"] = open(os.path.join(folder, "records", "mission1.placed.lua"), encoding="utf-8").read()
        # wherever the mod's layout put it (new mods: levels\MISSION1.envpatch; the game finds it by name either way)
        from conflict_ds_tools import modlayout
        patch = modlayout.find(folder, "MISSION1.envpatch")
        assert patch, "save_mod wrote no MISSION1.envpatch into Mods\\%s" % MOD
        res["patch"] = open(patch, encoding="latin-1").read()
        print("\n[e2e] save", res["save"], "\n" + res["records"] + res["patch"])
        dev = {"StartLevel": "", "SplitPlayers": 0, "NoErrorBoxes": 1}
        enh = {"FeatLoadout": 0, "SkipCutscenes": 1, "FpsCap": 60, "FeatMods": 1}
        with bg.Game(dev=dev, enh=enh) as g:
            c = pse_client.Client(pse_client.wait_for_pipe(g.p.pid, timeout=120))
            c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
            c.start_level("Mission1.dll", wait=True, timeout=240)
            for sid in (266, 6, res["copy_sid"]):
                res[sid] = c.run(OBJ % sid)
            res["wp1"] = c.run(WP % 1)
            res["wpn"] = c.run(WP % res["wp_new"])
            res["log"] = c.log(8000) or []
            c.close()
    finally:
        shutil.rmtree(folder, ignore_errors=True)
    print("[e2e]", {k: v for k, v in res.items() if k not in ("log", "records", "patch", "save")})
    for l in res["log"]:
        if "Records:" in l or "MISSION1" in l:
            print("[e2e]", l)
    return res


def test_saved_as_records_and_a_patch(run):
    assert run["save"]["record_edits"] == 4, run["records"]   # medikit, copy, alcove patrol, waypoint wait
    assert "based_on" in run["records"] and "ds1:mission1/object/" + run["copy_stat"].lower() in run["records"]
    assert run["copy_stat"] != "BASE_GUARD_1"
    assert '"' + run["copy_stat"] + '"' not in run["patch"]   # the copy is a record, not a patch line
    assert "NEWENVWAYPT %d " % run["wp_new"] in run["patch"]  # the new waypoint is a patch line


def test_game_built_the_level(run):
    log = run["log"]
    assert any("MISSION1.ENV = MISSION1.ENV + MISSION1.envpatch" in l for l in log)
    assert any("MISSION1.ENV = the mods' scene + placed reference edits" in l for l in log)
    assert not any("[fail]" in l and ("Records" in l or "MISSION1" in l) for l in log)


def test_objects(run):
    x, y, z, in_play = run[266]
    assert abs(x - (run["med"][0] + 200)) < 5 and abs(z - run["med"][2]) < 5, run[266]
    assert run[6] and run[6][3] == 0, run[6]
    cx, cy, cz, cin = run[run["copy_sid"]]
    assert abs(cx - run["guard"][0]) < 5 and abs(cz - run["guard"][2]) < 5 and cin == 1, run[run["copy_sid"]]


def test_waypoints(run):
    assert run["wp1"][0] == 7777, run["wp1"]
    assert run["wpn"][0] == 7777 and run["wpn"][1] == -1, run["wpn"]   # the copy of the changed waypoint, chain end

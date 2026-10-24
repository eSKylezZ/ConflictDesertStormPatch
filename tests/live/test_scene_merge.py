"""Live: several mods changing the same level (pse/records/scenepatch.h, mods.cpp BuildReferences + ApplyRecords;
docs/sdk/data-files.md "Several mods, one level").

Three test mods for Mission 1, made before the game starts from the player's own MISSION1.ENV (with the ModKit):
  zy_PatchA  MISSION1.envpatch: MEDIKIT_257 moved 3 m, AMMO_AK47_251 moved 1 m, a copy of the medikit (script id 321)
  zy_PatchB  MISSION1.envpatch: AMMO_AK47_251 moved 2 m the other way (the same line as A's: B wins), its own copy (322)
  zz_Records records\\edits.lua: MEDIKIT_266 moved 2 m and BASE_GUARD_1 copied - on top of the merged scene (the copy
             takes the next free id, 323)
In Mission 1 every change must be in the game's object table [[0x63C9A0]+0x30][id] (+0x30 position in cm), and the
mod report must name the merge and the conflict. The mods are removed afterwards.
Needs tools/bg.py and the ModKit (../ConflictDesertStormModKit); uses the installed build.
"""
import os, shutil, sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools"))
sys.path.insert(0, os.path.join(os.path.dirname(REPO), "ConflictDesertStormModKit"))
import pse_client  # noqa: E402

try:
    import bg  # noqa: E402
    from conflict_ds_tools import archive, env as envmod, gamerefs  # noqa: E402
except ImportError:
    pytest.skip("needs tools/bg.py and the ModKit", allow_module_level=True)

OBJ = ("local t = dev.u32(dev.u32(0x63C9A0) + 0x30) local o = dev.u32(t + %d * 4) if o == 0 then return {} end "
       "return {dev.f32(o + 0x30), dev.f32(o + 0x34), dev.f32(o + 0x38), dev.u32(o + 0xC) & 1}")
MODS = ("zy_PatchA", "zy_PatchB", "zz_Records")


def hloi(env, sid):
    return next(r for r in env.find("HLOI") if r.get("script_id") == sid)


def patch(data, changes):
    base, env = envmod.EnvFile.read(data), envmod.EnvFile.read(data)
    changes(env)
    return gamerefs.make_env_patch(base.lines, env.lines, "MISSION1.ENV")


@pytest.fixture(scope="module")
def run():
    data = archive.GameFiles(bg.GAME).get("MISSION1.ENV")
    orig = {sid: hloi(envmod.EnvFile.read(data), sid)["pos"] for sid in (251, 257, 266)}

    def a(env):
        env.duplicate(hloi(env, 257), script_id=321, stat="PATCH_A_KIT")
        x, y, z = orig[257]
        hloi(env, 257)["pos"] = (x + 300, y, z)
        x, y, z = orig[251]
        hloi(env, 251)["pos"] = (x + 100, y, z)

    def b(env):
        env.duplicate(hloi(env, 257), script_id=322, stat="PATCH_B_KIT")
        x, y, z = orig[251]
        hloi(env, 251)["pos"] = (x - 200, y, z)

    x, y, z = orig[266]
    files = {
        "zy_PatchA/MISSION1.envpatch": patch(data, a),
        "zy_PatchB/MISSION1.envpatch": patch(data, b),
        "zz_Records/records/edits.lua": """return {
  { id = "ds1:mission1/object/medikit_266", pos = { x = %.2f, y = %.2f, z = %.2f } },
  { id = "ds1:mission1/object/gate_extra_guard", based_on = "ds1:mission1/object/base_guard_1" },
}
""" % (x + 200, y, z),
    }
    for m in MODS:
        shutil.rmtree(os.path.join(bg.GAME, "Mods", m), ignore_errors=True)
    for rel, text in files.items():
        p = os.path.join(bg.GAME, "Mods", rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="latin-1", newline="\n") as f:
            f.write(text)
    res = {"orig": orig}
    try:
        dev = {"StartLevel": "", "SplitPlayers": 0, "NoErrorBoxes": 1}
        enh = {"FeatLoadout": 0, "SkipCutscenes": 1, "FpsCap": 60, "FeatMods": 1}
        with bg.Game(dev=dev, enh=enh) as g:
            c = pse_client.Client(pse_client.wait_for_pipe(g.p.pid, timeout=120))
            c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
            c.start_level("Mission1.dll", wait=True, timeout=240)
            for sid in (251, 257, 266, 321, 322, 323):
                res[sid] = c.run(OBJ % sid)
            res["log"] = c.log(8000) or []
            c.close()
    finally:
        for m in MODS:
            shutil.rmtree(os.path.join(bg.GAME, "Mods", m), ignore_errors=True)
    print("\n[merge]", {k: v for k, v in res.items() if k != "log"})
    for l in res["log"]:
        if "MISSION1" in l or "conflict" in l or "Records:" in l:
            print("[merge]", l)
    return res


def near(got, want, dx=0.0):
    return got and abs(got[0] - (want[0] + dx)) < 5 and abs(got[2] - want[2]) < 5


def test_report_names_the_merge_and_the_conflict(run):
    log = run["log"]
    assert any("MISSION1.ENV = MISSION1.ENV + MISSION1.envpatch (zy_PatchA) + MISSION1.envpatch (zy_PatchB)" in l for l in log)
    assert any("conflict: MISSION1.ENV lines" in l and "zy_PatchA / zy_PatchB (zy_PatchB wins)" in l for l in log)
    assert any("MISSION1.ENV = the mods' scene + placed reference edits" in l for l in log)
    assert not any("[fail]" in l and "MISSION1" in l for l in log)


def test_both_patches_apply(run):
    assert near(run[257], run["orig"][257], 300), run[257]    # A's move
    assert near(run[321], run["orig"][257]) and near(run[322], run["orig"][257]), (run[321], run[322])   # both copies


def test_later_patch_wins_the_same_line(run):
    assert near(run[251], run["orig"][251], -200), run[251]


def test_record_edits_on_top(run):
    assert near(run[266], run["orig"][266], 200), run[266]
    assert run[323] and run[323][3] == 1, run[323]   # the guard's copy: the next id after both patches' copies

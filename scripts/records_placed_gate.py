"""Records: placed-reference edits (a level's objects / waypoints / regions) - DesertStormFix's C++ (pse/records/placed.h,
`pse_build records merged`) and the ModKit's conflict_ds_tools/placed.py must give the same scenes and the same report.

  python scripts/records_placed_gate.py [--game <dir>] [--pse-build path] [--modkit path]

Test mods are made from the player's own scenes (editor IDs picked from Mission 1, Mission 3 and Training 2): moves,
copies, take-out-of-play, waypoint chains, region shapes, scene fields, every refusal. Nothing it makes is kept.
Exit code = mismatches.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
SCHEMA = os.path.join(REPO, "pse", "records", "ds1", "schema.lua")

ap = argparse.ArgumentParser()
ap.add_argument("--game")
ap.add_argument("--pse-build")
ap.add_argument("--modkit", default=os.path.join(os.path.dirname(REPO), "ConflictDesertStormModKit"))
args = ap.parse_args()
sys.path.insert(0, args.modkit)
from conflict_ds_tools import archive, forms, games, placed, pse_data  # noqa: E402

game = args.game or games.find_game_dir(games.GAMES["ds1"])
exe = args.pse_build
if not exe:
    cands = [os.path.join(REPO, d, "pse", c, "pse_build.exe") for d in ("build-rec", "build-pse", "build")
             for c in ("RelWithDebInfo", "Release", "Debug")]
    cands = [c for c in cands if os.path.exists(c)]
    exe = max(cands, key=os.path.getmtime) if cands else sys.exit("pse_build.exe not found")
schema = forms.load_schema(SCHEMA)
gf = archive.GameFiles(game)
docs = forms.generate(game, schema, gf)


def refs(level):
    out = {"object": [], "waypoint": [], "region": []}
    for r in placed.level_refs(game, level, docs, gf).root["refs"].items:
        out[r["id"].split("/")[1]].append(r)
    return out


m1, m3, t2 = refs("mission1"), refs("mission3"), refs("training2")
o1, w1, g1 = m1["object"], m1["waypoint"], m1["region"]
o3, w3 = m3["object"], m3["waypoint"]
spheres = [r for r in g1 + m3["region"] if r["shape"] == "sphere"]
boxes = [r for r in g1 + m3["region"] if r["shape"] == "box"]
unit = next(r["id"] for r in docs["unit"].root["records"].items if isinstance(r, pse_data.Table) and r.get("id"))
lvl_of = lambda rid: rid.split(":")[1].split("/")[0]

A = '''-- moves, copies, waypoint / region edits
return {
  { id = "%(o1a)s", pos = { x = 100.5, y = -20.25, z = 3000 }, rot = { y = 90.0 } },
  { id = "%(o1b)s", delete = true },
  { id = "%(o1c)s", in_play = false },
  { id = "ds1:mission1/object/gate_extra_guard", based_on = "%(o1a)s", in_play = true, pos = { x = 1.0, y = 2.0, z = 3.0 } },
  { id = "ds1:mission1/object/gate_second_copy", based_on = "%(o1d)s" },
  { id = "ds1:mission1/object/gate_asked_id", based_on = "%(o1d)s", script_id = 400 },
  { id = "ds1:mission1/object/gate_taken_id", based_on = "%(o1d)s", script_id = 4 },
  { id = "%(w1a)s", wait_ms = 7777, next = "%(w1b)s" },
  { id = "%(w1b)s", next = -1, pos = { x = 5, y = 6, z = 7 } },
  { id = "%(w3a)s", next = 3 },
  { id = "%(sph)s", radius = 999.5, pos = { x = 1, y = 2, z = 3 } },
  { id = "%(box)s", min = { x = -10, y = -10, z = -10 }, max = { x = 10, y = 10.5, z = 10 } },
  { id = "%(o3a)s", base = "%(unit)s", scene = { u_a = 1, stat = "RENAMED_BY_GATE" } },
  { id = "ds1:weapon/us_wpn_m16a2", damage = 22 },
}'''
B = '''-- refusals and odd values
return {
  { id = "%(o1a)s", pos = { x = 7.0 } },
  { id = "%(o1e)s", pos = 5, rot = { x = "north" } },
  { id = "%(o1e)s", scene = { no_such_field = 1, kit_count = "two", [2] = 1, unit = { x = 1 }, model = 1.5, u_a = 2.5 } },
  { id = "%(o1e)s", base = "ds1:unit/no_such_unit" },
  { id = "%(o1e)s", base = 12 },
  { id = "%(o1e)s", in_play = 0, "a list item" },
  { id = "%(o1f)s", model = "TWO WORDS", zone = "with \\"quote\\"" },
  { id = "%(w1a)s", next = "ds1:mission1/waypoint/no_such_waypoint" },
  { id = "%(w1a)s", next = 1.5 },
  { id = "%(w1c)s", delete = true },
  { id = "%(w1c)s", wait_ms = true, u1 = "x" },
  { id = "%(sph)s", radius = { x = 1 } },
  { id = "ds1:mission1/object/not_in_this_scene", pos = { x = 1, y = 1, z = 1 } },
  { id = "ds1:mission1/object/copy_from_nowhere", based_on = "ds1:mission1/object/no_such_object" },
  { id = "ds1:mission1/object/copy_wrong_level", based_on = "%(o3a)s" },
  { id = "ds1:mission1/marker/m1", pos = { x = 1, y = 1, z = 1 } },
  { id = "ds1:Mission1/object/upper_case_level", pos = { x = 1, y = 1, z = 1 } },
  { id = "ds1:mission77/object/x", pos = { x = 1, y = 1, z = 1 } },
  { id = "%(t2a)s", pos = { x = -1, y = -2, z = -3 } },
}'''
C = '''-- a later mod over the same refs (later wins)
return {
  { id = "%(o1a)s", pos = { x = 200.0, y = 0, z = 0 } },
  { id = "ds1:mission1/object/gate_extra_guard", pos = { x = 9, y = 9, z = 9 } },
  { id = "%(o1b)s", in_play = true },
  { id = "%(o1c)s", in_play = false },
  { id = "%(o3a)s", scene = { stat = "RENAMED_AGAIN", u_a = 1 } },
  { id = "%(w1a)s", wait_ms = 7777 },
}'''
fill = {"o1a": o1[3]["id"], "o1b": o1[4]["id"], "o1c": o1[5]["id"], "o1d": o1[6]["id"], "o1e": o1[7]["id"],
        "o1f": o1[8]["id"], "w1a": w1[0]["id"], "w1b": w1[1]["id"], "w1c": w1[2]["id"], "w3a": w3[0]["id"],
        "sph": spheres[0]["id"], "box": boxes[0]["id"], "o3a": o3[2]["id"], "unit": unit,
        "t2a": (t2["object"] or t2["waypoint"])[0]["id"]}
MODS = {"A_Placed": A % fill, "B_Bad": B % fill, "C_Later": C % fill, "_Off": A % fill}

bad = 0
tmp = tempfile.mkdtemp(prefix="records_placed_gate_")
try:
    for label, mods in (("no mods", {}), ("placed edits", MODS)):
        mdir = os.path.join(tmp, "mods_" + label.replace(" ", "_"))
        os.makedirs(mdir)
        for name, text in mods.items():
            os.makedirs(os.path.join(mdir, name, "records"))
            open(os.path.join(mdir, name, "records", "edits.lua"), "w", encoding="utf-8").write(text)
        out = os.path.join(tmp, "out_" + label.replace(" ", "_"))
        r = subprocess.run([exe, "records", "merged", "--schema", SCHEMA, "--game", game, "--mods", mdir, "--out", out],
                           capture_output=True, text=True)
        if r.returncode:
            bad += 1
            print("pse_build records merged failed (%s): %s" % (label, r.stderr))
            continue
        edits = forms.load_mod_edits(mdir)
        merged, report = forms.merge(docs, edits, schema)
        scenes, rep2 = placed.merged_scenes(game, edits, merged, gf)
        report += rep2
        cpp_report = open(os.path.join(out, "report.txt"), encoding="utf-8").read().splitlines()
        if cpp_report != report:
            bad += 1
            print("REPORT DIFFERS (%s):" % label)
            for k in range(max(len(report), len(cpp_report))):
                a = report[k] if k < len(report) else None
                b = cpp_report[k] if k < len(cpp_report) else None
                if a != b:
                    print("  line %d\n    python: %s\n    c++   : %s" % (k, a, b))
        cpp_scenes = sorted(f for f in os.listdir(out) if f.upper().endswith(".ENV"))
        if cpp_scenes != sorted(scenes):
            bad += 1
            print("SCENES DIFFER (%s): python %s, c++ %s" % (label, sorted(scenes), cpp_scenes))
        changed = 0
        for name, data in scenes.items():
            p = os.path.join(out, name)
            cpp = open(p, "rb").read() if os.path.exists(p) else None
            if cpp != data:
                bad += 1
                pl, cl = data.split(b"\n"), (cpp or b"").split(b"\n")
                k = next((i for i in range(min(len(pl), len(cl))) if pl[i] != cl[i]), min(len(pl), len(cl)))
                print("SCENE DIFFERS (%s): %s line %d\n  python: %r\n  c++   : %r" % (
                    label, name, k + 1, pl[k] if k < len(pl) else None, cl[k] if k < len(cl) else None))
            changed += data != gf.get(name)
        print("%s: %d scene(s) compared, %d changed, %d report line(s)" % (label, len(scenes), changed, len(report)))
        if label != "no mods":
            for l in report:
                print("   ", l)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
print("MISMATCHES: %d" % bad)
sys.exit(bad)

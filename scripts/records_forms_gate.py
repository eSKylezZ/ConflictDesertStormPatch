"""Records: the C++ generator / merger (pse/records/forms.h, `pse_build records ...`) and the ModKit's
conflict_ds_tools/forms.py must give the same record files, the same merged tables and the same report.

  python scripts/records_forms_gate.py [--game <dir>] [--pse-build path] [--modkit path]

Reads the player's own catalog.dat (game folder or PSE\\Originals). Nothing it makes is kept. Exit code = mismatches.
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
from conflict_ds_tools import archive, forms, games, pse_data  # noqa: E402

game = args.game or games.find_game_dir(games.GAMES["ds1"])
exe = args.pse_build
if not exe:
    cands = [os.path.join(REPO, d, "pse", c, "pse_build.exe") for d in ("build-pse-t", "build-pse", "build")
             for c in ("RelWithDebInfo", "Release", "Debug")]
    cands = [c for c in cands if os.path.exists(c)]
    exe = max(cands, key=os.path.getmtime) if cands else sys.exit("pse_build.exe not found")
schema = forms.load_schema(SCHEMA)
bad = 0

MODS = {
    "A_Damage": '''return {
  { id = "ds1:weapon/us_wpn_m16a2", damage = 20, range = 900.0 },
  { id = "ds1:weapon/us_wpn_mp5silencedsubmg", magazine_size = 40, type = "SMG" },
  { id = "ds1:unit/bradley", experience = "ds1:experience_level/veteran" },
}''',
    "B_More": '''-- a second mod: one conflict with A, new records, deletes, bad edits
return {
  { id = "ds1:weapon/us_wpn_m16a2", damage = 25, col10 = 7 },
  { id = "ds1:weapon/my_rifle", based_on = "ds1:weapon/us_wpn_m16a2", key = "MY_RIFLE", damage = 30 },
  { id = "ds1:weapon/no_key_given", based_on = "ds1:weapon/ir_wpn_ak47" },
  { id = "ds1:unit_kit/my_squaddie", based_on = "ds1:unit_kit/iraqi_republican" },
  { id = "ds1:unit/bradley", clear = { "speech_bank", "stats" } },
  { id = "ds1:feature/nu_prop_crate5#2", delete = true },
  { id = "ds1:weapon/does_not_exist", damage = 1 },
  { id = "ds1:nosuchtype/x", damage = 1 },
  { id = "nonsense" },
  { damage = 3 },
  { id = "ds1:weapon/ir_wpn_ak47", damage = 12.5, magazines = true, reload_time = 3, [2] = "int key" },
  { id = "ds1:vehicle_weapons/bmp1", rows = { { vehicle = "BMP1", weapon = "ds1:weapon/us_wpn_m16a2", kind = "WEAPON", ammo = 99 } } },
}''',
    "C_Same": '''return { { id = "ds1:weapon/us_wpn_m16a2", damage = 25 } }''',
    "_Off": '''return { { id = "ds1:weapon/us_wpn_m16a2", damage = 1 } }''',
}

tmp = tempfile.mkdtemp(prefix="records_forms_gate_")
try:
    # 1. generated record files
    out1 = os.path.join(tmp, "gen")
    r = subprocess.run([exe, "records", "generate", "--schema", SCHEMA, "--game", game, "--out", out1],
                       capture_output=True, text=True)
    if r.returncode:
        sys.exit("pse_build records generate failed: " + r.stderr)
    docs = forms.generate(game, schema)
    for rtype, doc in docs.items():
        py = pse_data.dumps_bytes(doc)
        p = os.path.join(out1, rtype + ".lua")
        cpp = open(p, "rb").read() if os.path.exists(p) else None
        if cpp != py:
            bad += 1
            at = next((i for i in range(min(len(py), len(cpp or b""))) if py[i] != cpp[i]), None)
            print("RECORDS DIFFER: %s (byte %s)" % (rtype, at))
            if at is not None:
                print("  python:", py[max(0, at - 80):at + 80])
                print("  c++   :", cpp[max(0, at - 80):at + 80])
    print("record files: %d types compared" % len(docs))

    # limits: a schema copy with small max_len / max_rows so every limit branch runs (key too long, field too long,
    # plain-name ref too long, record id ref allowed, table full)
    lim = pse_data.loads(pse_data.dumps(pse_data.Document(schema.root if hasattr(schema, "root") else schema)))
    for t in lim.root["tables"].items:
        cols = t["columns"].items
        if t["type"] == "weapon":
            cols[0].set("max_len", 20)
            next(c for c in cols if c["name"] == "model").set("max_len", 10)
        if t["type"] == "unit":
            next(c for c in cols if c["name"] == "stats").set("max_len", 12)
        if t["type"] == "door":
            t.set("max_rows", sum(1 for r in docs["door"].root["records"].items
                                  if isinstance(r, pse_data.Table) and not r.get("_blank")))
    LIMIT_SCHEMA = os.path.join(tmp, "limits_schema.lua")
    pse_data.save(LIMIT_SCHEMA, lim)
    LIMIT_MODS = {"L_Limits": '''return {
  { id = "ds1:weapon/a_new_weapon_with_a_long_name", based_on = "ds1:weapon/us_wpn_m16a2" },
  { id = "ds1:weapon/short_one", based_on = "ds1:weapon/us_wpn_m16a2", key = "THIS_KEY_IS_FAR_TOO_LONG_X" },
  { id = "ds1:weapon/us_wpn_m16a2", model = "AN_OVERLONG_MODEL", damage = 21 },
  { id = "ds1:unit/bradley", stats = "A_PLAIN_NAME_TOO_LONG" },
  { id = "ds1:unit/foley", stats = "ds1:unit_stats/us_infantryman00" },
  { id = "ds1:door/one_door_too_many", based_on = "%s" },
}''' % next(r["id"] for r in docs["door"].root["records"].items if isinstance(r, pse_data.Table) and r.get("id"))}

    # 2. merged tables + report, with and without mods
    for label, mods, sch_path in (("no mods", {}, SCHEMA), ("test mods", MODS, SCHEMA),
                                  ("limits", LIMIT_MODS, LIMIT_SCHEMA)):
        sch = schema if sch_path == SCHEMA else forms.load_schema(sch_path)
        mdir = os.path.join(tmp, "mods_" + label.replace(" ", "_"))
        for name, text in mods.items():
            os.makedirs(os.path.join(mdir, name, "records"))
            open(os.path.join(mdir, name, "records", "edits.lua"), "w", encoding="utf-8").write(text)
        os.makedirs(mdir, exist_ok=True)
        out2 = os.path.join(tmp, "merged_" + label.replace(" ", "_"))
        r = subprocess.run([exe, "records", "merged", "--schema", sch_path, "--game", game, "--mods", mdir, "--out", out2],
                           capture_output=True, text=True)
        if r.returncode:
            bad += 1
            print("pse_build records merged failed (%s): %s" % (label, r.stderr))
            continue
        files, report = forms.merged_tables(game, sch, mdir)
        cpp_report = open(os.path.join(out2, "report.txt"), encoding="utf-8").read().splitlines()
        if cpp_report != report:
            bad += 1
            print("REPORT DIFFERS (%s):\n  python: %s\n  c++   : %s" % (label, report, cpp_report))
        gf = archive.GameFiles(game)
        changed = 0
        for f, data in files.items():
            cpp = open(os.path.join(out2, f), "rb").read()
            if cpp != data:
                bad += 1
                print("TABLE DIFFERS (%s): %s" % (label, f))
            changed += data != gf.get(f)
        if label == "no mods" and changed:
            bad += 1
            print("no mods but %d table(s) changed" % changed)
        print("%s: %d tables compared, %d changed by the edits, %d report line(s)" % (label, len(files), changed, len(report)))
        for line in report:
            print("   ", line)
finally:
    shutil.rmtree(tmp, ignore_errors=True)
print("%d mismatch(es) - %s" % (bad, exe))
sys.exit(min(bad, 255))

"""The C++ readable mission translator's gate: byte-identical readable Lua with the Python for every mission DLL.

    python scripts/m2r_gate.py [--game <install>] [--pse-build <pse_build.exe>] [--modkit <ModKit>] [--only Mission1,...]
                               [--keep <dir>]

For each Mission*.dll / Training*.dll of the install: ModKit conflict_ds_tools/mission2lua_readable.py (the oracle) and
`pse_build missions --readable` (pse/build/mission2lua/m2r_*.cpp, the C++ port) translate it - scene editor IDs read from
the archives next to the DLL by both - and every file of the mod-folder layout (mission.lua, refs.lua, scripts/events.lua,
scripts/runtime.lua, scripts/start.lua) and the bundle (<Mission>.lua) must be byte-identical. Mission patches
(docs/sdk/reference/mission-patches.md) are written against the ModKit's text and applied to the game's, so this is what
keeps them applying. Also checks that pse/build/mission2lua/m2r_data.inc is up to date (scripts/sync_m2r_data.py).

Needs Python with capstone + pefile (the ModKit's requirements). The generated Lua is game code: it is written to a temp
folder and deleted afterwards (unless --keep). Exit code 0 = gate passed.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PARTS = ("mission.lua", "refs.lua", "scripts/events.lua", "scripts/runtime.lua", "scripts/start.lua")


def find_game():
    env = os.environ.get("PSE_GAME_DIR")
    for c in ([env] if env else []) + [r"C:\Program Files (x86)\Steam\steamapps\common\conflict_desert_storm", r"C:\GOG Games\Conflict - Desert Storm"]:
        if os.path.exists(os.path.join(c, "Mission1.dll")) or os.path.exists(os.path.join(c, "PSE", "Originals")):
            return c
    return None


def find_pse_build():
    for d in ("build-pse", "build"):
        for cfg in ("RelWithDebInfo", "Release", "Debug"):
            p = os.path.join(REPO, d, "pse", cfg, "pse_build.exe")
            if os.path.exists(p):
                return p
    return None


def mission_dlls(game):
    """The game folder's DLLs, else PSE/Originals' (where the game layer moves them), like ds_mission2lua_readable."""
    files = {}
    for d in (os.path.join(game, "PSE", "Originals"), game):
        for pat in ("Mission*.dll", "Training*.dll"):
            for p in glob.glob(os.path.join(d, pat)):
                files[os.path.basename(p).lower()] = p
    return [files[k] for k in sorted(files)]


def first_diff(a, b):
    la, lb = a.split(b"\n"), b.split(b"\n")
    for n, (x, y) in enumerate(zip(la, lb)):
        if x != y:
            return n + 1, x[:160], y[:160]
    return min(len(la), len(lb)) + 1, b"<end>" if len(la) < len(lb) else b"", b"<end>" if len(lb) < len(la) else b""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=find_game())
    ap.add_argument("--pse-build", default=find_pse_build())
    ap.add_argument("--modkit", default=os.path.join(os.path.dirname(REPO), "ConflictDesertStormModKit"))
    ap.add_argument("--only", default="")
    ap.add_argument("--keep", default=None, help="keep the generated files in this folder (game code - local only)")
    a = ap.parse_args()
    if not a.game or not a.pse_build:
        print("need --game and --pse-build")
        return 2
    sync = subprocess.run([sys.executable, os.path.join(HERE, "sync_m2r_data.py"), "--modkit", a.modkit, "--check"])
    if sync.returncode:
        return 1
    sys.path.insert(0, a.modkit)
    from conflict_ds_tools import mission2lua_readable as m2r
    dlls = mission_dlls(a.game)
    if a.only:
        want = {x.strip().lower() for x in a.only.split(",") if x.strip()}
        dlls = [p for p in dlls if os.path.splitext(os.path.basename(p))[0].lower() in want]
    tmp = a.keep or tempfile.mkdtemp(prefix="m2r_gate_")
    py_dir, cpp_dir = os.path.join(tmp, "python"), os.path.join(tmp, "cpp")
    failed = 0
    t_py = t_cpp = 0.0
    try:
        for path in dlls:
            name = os.path.splitext(os.path.basename(path))[0]
            t0 = time.time()
            try:
                parts, _g = m2r.translate(path)
            except m2r.Unsupported as e:
                print("%-12s python: unsupported: %s" % (name, e))
                failed += 1
                continue
            m2r.write_folder(parts, py_dir, name)
            t1 = time.time()
            r = subprocess.run([a.pse_build, "missions", "--readable", "--dll", path, "--out", cpp_dir],
                               capture_output=True, text=True)
            t2 = time.time()
            t_py += t1 - t0
            t_cpp += t2 - t1
            if r.returncode:
                print("%-12s C++ failed: %s" % (name, (r.stdout + r.stderr).strip().splitlines()[-2:]))
                failed += 1
                continue
            bad = []
            for rel in PARTS + ("../%s.lua" % name,):
                pa = os.path.normpath(os.path.join(py_dir, name, rel))
                pb = os.path.normpath(os.path.join(cpp_dir, name, rel))
                x = open(pa, "rb").read()
                y = open(pb, "rb").read() if os.path.exists(pb) else b""
                if x != y:
                    n, lx, ly = first_diff(x, y)
                    bad.append("%s line %d\n      python: %r\n      C++:    %r" % (rel.replace("../", ""), n, lx, ly))
            if bad:
                failed += 1
                print("%-12s DIFFERENT (%.1f s / %.1f s)" % (name, t1 - t0, t2 - t1))
                for b in bad:
                    print("    " + b)
            else:
                print("%-12s identical (python %.1f s, C++ %.2f s)" % (name, t1 - t0, t2 - t1))
    finally:
        if not a.keep:
            shutil.rmtree(tmp, ignore_errors=True)
    print("%d of %d DLLs identical (python %.0f s, C++ %.1f s)%s" % (len(dlls) - failed, len(dlls), t_py, t_cpp,
                                                                     "" if not failed else " - GATE FAILED"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

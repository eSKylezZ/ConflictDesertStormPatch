"""The C++ mission translator's gate: byte-identical Lua with the Python generator for every mission DLL.

    python scripts/m2l_gate.py [--game <install>] [--pse-build <pse_build.exe>] [--modkit <ModKit>] [--only Mission1,...]
                               [--keep <dir>] [--decode]

For each Mission*.dll / Training*.dll of the install: ModKit conflict_ds_tools/mission2lua.py (the oracle,
docs/internal/mission-translation.md) and `pse_build missions` (pse/build/mission2lua, the C++ port) translate it; the two
files must be byte-identical. Also checks that pse/build/mission2lua/m2l_runtime.inc matches mission2lua.py's runtime
text (scripts/sync_m2l_runtime.py). --decode additionally compares the two decoders at every byte offset of every
DLL's code section (capstone through missionflow.Dll vs Zydis through m2l_decode.cpp).

Needs Python with capstone + pefile (the ModKit's requirements). The generated Lua is game code: it is written to a
temp folder and deleted afterwards (unless --keep). Exit code 0 = gate passed.
"""
import argparse
import glob
import hashlib
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def find_game():
    for c in (r"H:\SteamLibrary\steamapps\common\conflict_desert_storm", r"H:\GOG\Games\Conflict - Desert Storm"):
        if os.path.exists(os.path.join(c, "Mission1.dll")) or os.path.exists(os.path.join(c, "PSE", "Originals")):
            return c
    return None


def dll_folder(game):
    """Where the mission DLLs are: the game folder, or PSE/Originals once the game layer has moved them there."""
    if not mission_dlls(game) and mission_dlls(os.path.join(game, "PSE", "Originals")):
        return os.path.join(game, "PSE", "Originals")
    return game


def find_pse_build():
    for d in ("build-pse-t", "build-pse", "build"):
        for cfg in ("RelWithDebInfo", "Release", "Debug"):
            p = os.path.join(REPO, d, "pse", cfg, "pse_build.exe")
            if os.path.exists(p):
                return p
    return None


def mission_dlls(game):
    files = glob.glob(os.path.join(game, "Mission*.dll")) + glob.glob(os.path.join(game, "Training*.dll"))
    return sorted({os.path.normcase(p): p for p in files}.values(), key=os.path.normcase)


def first_diff(a, b):
    la, lb = a.split(b"\n"), b.split(b"\n")
    for n, (x, y) in enumerate(zip(la, lb)):
        if x != y:
            return n + 1, x[:160], y[:160]
    return min(len(la), len(lb)) + 1, b"<end>" if len(la) < len(lb) else b"", b"<end>" if len(lb) < len(la) else b""


def decode_check(dlls, pse_build, modkit, tmp):
    sys.path.insert(0, modkit)
    from capstone import x86
    from conflict_ds_tools import missionflow as mf

    def ops(i):
        out = []
        for o in i.operands:
            if o.type == x86.X86_OP_REG:
                out.append("r%d:%s" % (o.size, i.reg_name(o.reg)))
            elif o.type == x86.X86_OP_IMM:
                out.append("i%d:%d" % (o.size, o.imm))
            elif o.type == x86.X86_OP_MEM:
                m = o.mem
                out.append("m%d:%s:%s:%s:%d:%d" % (o.size, i.reg_name(m.segment) if m.segment else "",
                                                 i.reg_name(m.base) if m.base else "",
                                                 i.reg_name(m.index) if m.index else "", m.scale, m.disp))
            else:
                out.append("?")
        return ",".join(out)

    total = bad = 0
    for path in dlls:
        d = mf.Dll(open(path, "rb").read())
        lines = []
        for va in range(d.text_lo, d.text_hi):
            i = d.insn(va)
            if i is None:
                lines.append("%08X -" % va)
            else:
                lines.append("%08X %d|%s|%s|%s|%d|%d|%s" % (va, i.size, i.mnemonic, i.op_str, ops(i), i.imm_offset,
                                                           i.disp_offset, bytes(i.bytes).hex()))
        zy = os.path.join(tmp, "decode.txt")
        subprocess.run([pse_build, "missions", "--dump", path, zy], check=True)
        other = open(zy).read().splitlines()
        total += len(lines)
        diffs = [(x, y) for x, y in zip(lines, other) if x != y]
        if len(lines) != len(other):
            diffs.append(("length %d" % len(lines), "length %d" % len(other)))
        bad += len(diffs)
        print("decode %-14s %7d offsets, %d differ%s" % (os.path.basename(path), len(lines), len(diffs),
                                                        ("  e.g. cs %s / zy %s" % diffs[0]) if diffs else ""))
    print("decode: %d of %d offsets differ" % (bad, total))
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--game", default=find_game())
    ap.add_argument("--pse-build", default=find_pse_build())
    ap.add_argument("--modkit", default=os.path.join(os.path.dirname(REPO), "ConflictDesertStormModKit"))
    ap.add_argument("--only", default="")
    ap.add_argument("--keep", default=None, help="keep the generated files in this folder (game code - local only)")
    ap.add_argument("--decode", action="store_true")
    a = ap.parse_args()
    if not a.game or not a.pse_build:
        print("need --game and --pse-build")
        return 2
    sync = subprocess.run([sys.executable, os.path.join(HERE, "sync_m2l_runtime.py"), "--modkit", a.modkit, "--check"])
    if sync.returncode:
        print("GATE FAILED: the C++ runtime text is out of date (run scripts/sync_m2l_runtime.py, rebuild)")
        return 1
    sys.path.insert(0, a.modkit)
    from conflict_ds_tools import mission2lua

    a.game = dll_folder(a.game)
    dlls = mission_dlls(a.game)
    if a.only:
        want = {w.lower() for w in a.only.split(",")}
        dlls = [p for p in dlls if os.path.splitext(os.path.basename(p))[0].lower() in want]
    tmp = a.keep or tempfile.mkdtemp(prefix="m2l_gate_")
    py_dir, cpp_dir = os.path.join(tmp, "py"), os.path.join(tmp, "cpp")
    os.makedirs(py_dir, exist_ok=True)
    os.makedirs(cpp_dir, exist_ok=True)
    failed = 0
    try:
        t_py = 0.0
        for p in dlls:
            t0 = time.perf_counter()
            src, _ = mission2lua.translate(p)
            t_py += time.perf_counter() - t0
            stem = os.path.splitext(os.path.basename(p))[0]
            with open(os.path.join(py_dir, stem + ".lua"), "w", encoding="utf-8", newline="\n") as f:
                f.write(src)
        names = ",".join(os.path.splitext(os.path.basename(p))[0] for p in dlls)
        t0 = time.perf_counter()
        r = subprocess.run([a.pse_build, "missions", "--game", a.game, "--out", cpp_dir, "--only", names, "--force",
                            "--quiet"])
        t_cpp = time.perf_counter() - t0
        if r.returncode:
            print("pse_build missions failed (%d)" % r.returncode)
            failed += 1
        for p in dlls:
            stem = os.path.splitext(os.path.basename(p))[0]
            x = open(os.path.join(py_dir, stem + ".lua"), "rb").read()
            yp = os.path.join(cpp_dir, stem + ".lua")
            y = open(yp, "rb").read() if os.path.exists(yp) else b""
            if x == y:
                print("same  %-14s %9d bytes  sha256 %s" % (stem, len(x), hashlib.sha256(x).hexdigest()[:16]))
            else:
                failed += 1
                n, lx, ly = first_diff(x, y)
                print("DIFF  %-14s line %d\n      py:  %r\n      cpp: %r" % (stem, n, lx, ly))
        print("%d DLL(s): %d identical, %d different. Python %.2f s, C++ %.2f s (pse_build missions, all DLLs)" % (
            len(dlls), len(dlls) - failed, failed, t_py, t_cpp))
        if a.decode:
            failed += 1 if decode_check(dlls, a.pse_build, a.modkit, tmp) else 0
    finally:
        if not a.keep:
            for d in (py_dir, cpp_dir):
                for f in glob.glob(os.path.join(d, "*")):
                    os.remove(f)
                os.rmdir(d)
            for f in glob.glob(os.path.join(tmp, "*")):
                os.remove(f)
            os.rmdir(tmp)
    print("GATE %s" % ("PASSED" if not failed else "FAILED"))
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())

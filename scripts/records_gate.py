"""PSE data files: the C++ writer (pse/records, `pse_build data canon`) and the ModKit's Python one
(conflict_ds_tools/pse_data.py) must give the same canonical bytes.

  python scripts/records_gate.py [--count N] [--seed S] [--pse-build path] [--modkit path]

Random documents written in messy, non-canonical ways (spacing, quote styles, hex, comments, separators, long
strings, unicode, control bytes, deep nesting, long lines) -> both canonicalise -> compare; plus bad inputs that both
must refuse. Exit code = number of mismatches (capped at 255).
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

ap = argparse.ArgumentParser()
ap.add_argument("--count", type=int, default=400)
ap.add_argument("--seed", type=int, default=1)
ap.add_argument("--pse-build", default=None)
ap.add_argument("--modkit", default=os.path.join(os.path.dirname(REPO), "ConflictDesertStormModKit"))
args = ap.parse_args()

sys.path.insert(0, args.modkit)
from conflict_ds_tools import pse_data  # noqa: E402

exe = args.pse_build
if not exe:
    for d in ("build-pse", "build"):
        for cfg in ("RelWithDebInfo", "Release", "Debug"):
            p = os.path.join(REPO, d, "pse", cfg, "pse_build.exe")
            if os.path.exists(p) and (not exe or os.path.getmtime(p) > os.path.getmtime(exe)):
                exe = p
if not exe:
    sys.exit("pse_build.exe not found (build the pse_build target)")

rnd = random.Random(args.seed)
NAMES = ["id", "name", "model", "damage", "x", "_hidden", "ammo2", "Range", "zeta", "a", "b", "id2", "if_", "end_"]
ODD_KEYS = ["with space", "end", "123abc", "caf\u00e9", "tab\there", "quote\"d", "back\\slash", "", "line\nbreak"]


def rand_string():
    pool = ["plain", "M16A2", "ds1:weapon/m249saw", "Hello, World", "caf\u00e9 \u00fc\u00df", "\u65e5\u672c",
            "quote\"inside", "back\\slash", "tab\tand\nnewline", "\x01ctrl\x7f", "x" * rnd.randint(0, 120), "", "'single'",
            "]]", "--not a comment", "\U0001F600"]
    return rnd.choice(pool)


def rand_float():
    k = rnd.random()
    if k < 0.2:
        return rnd.choice([0.0, -0.0, 1.0, 0.1, 1e15, 1e16, 1e-4, 1e-5, 123.456, 2.5e-300, 1.7976931348623157e308])
    if k < 0.6:
        return round(rnd.uniform(-1000, 1000), rnd.randint(0, 6))
    return rnd.uniform(-1, 1) * 10 ** rnd.randint(-30, 30)


def rand_scalar():
    k = rnd.random()
    if k < 0.25:
        return rnd.choice([0, 1, -1, 42, 2 ** 31, -(2 ** 63), 2 ** 63 - 1, rnd.randint(-10 ** 6, 10 ** 6)])
    if k < 0.5:
        return rand_float()
    if k < 0.85:
        return rand_string()
    return rnd.random() < 0.5


def rand_table(depth):
    t = pse_data.Table()
    n = rnd.randint(0, 6 if depth else 10)
    used = set()
    for _ in range(rnd.randint(0, 4)):   # list part
        t.push(rand_value(depth + 1), comments=rand_comments())
    for _ in range(n):
        r = rnd.random()
        if r < 0.6:
            key = rnd.choice(NAMES)
        elif r < 0.8:
            key = rnd.choice(ODD_KEYS)
        else:
            key = rnd.randint(len(t.items) + 1, len(t.items) + 50) if rnd.random() < 0.7 else rnd.randint(-5, 0)
        if (type(key), key) in used:
            continue
        used.add((type(key), key))
        t.entries.append(pse_data.Entry(key, rand_value(depth + 1), rand_comments()))
    return t


def rand_comments():
    if rnd.random() < 0.8:
        return []
    return [rnd.choice(["note", "", "  indented", "caf\u00e9 comment", "TODO: x = 1"]) for _ in range(rnd.randint(1, 2))]


def rand_value(depth):
    if depth < 5 and rnd.random() < 0.3:
        return rand_table(depth)
    return rand_scalar()


# ---- messy writer (valid Lua, not canonical) ----

def messy_string(s, key=False):
    if not key and "\n" not in s and "]]" not in s and rnd.random() < 0.1 and not any(ord(c) < 32 for c in s) and "\r" not in s:
        return "[[" + s + "]]"   # long string (no leading newline in s here)
    q = rnd.choice(['"', "'"])
    out = []
    for ch in s:
        o = ord(ch)
        if ch == "\\":
            out.append("\\\\")
        elif ch == q:
            out.append("\\" + q)
        elif ch == "\n":
            out.append(rnd.choice(["\\n", "\\10", "\\x0A"]))
        elif o < 32 or o == 127:
            out.append(rnd.choice(["\\%d" % o, "\\x%02X" % o, "\\%03d" % o]))
        elif o > 127 and rnd.random() < 0.3:
            out.append("\\u{%X}" % o)
        else:
            out.append(ch)
    return q + "".join(out) + q


def messy_scalar(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, int):
        if v >= 0 and rnd.random() < 0.2:
            return "0x%X" % v
        if v < 0 and v != -(2 ** 63) and rnd.random() < 0.3:
            return "-" + " " * rnd.randint(0, 2) + str(-v)
        return str(v)
    if isinstance(v, float):
        r = repr(v)
        if rnd.random() < 0.3:
            r = "%.17g" % v
            if "." not in r and "e" not in r and "n" not in r:
                r += ".0"
        return r
    return messy_string(v)


def sp():
    return rnd.choice(["", " ", "  ", "\n", "\t", " \n  "])


def messy(v, depth):
    if not isinstance(v, pse_data.Table):
        return messy_scalar(v)
    parts = []
    entries = list(v.entries)
    lst = [e for e in entries if e.key is None]
    keyed = [e for e in entries if e.key is not None]
    rnd.shuffle(keyed)
    order = lst + keyed   # list items keep their order, keyed entries shuffled
    for e in order:
        cm = "".join("\n--" + (" " + c if c else "") + "\n" for c in e.comments)
        if e.key is None:
            k = ""
        elif isinstance(e.key, int):
            k = "[" + sp() + str(e.key) + sp() + "]" + sp() + "=" + sp()
        elif pse_data.is_name(e.key) and rnd.random() < 0.8:
            k = e.key + sp() + "=" + sp()
        else:
            k = "[" + messy_string(e.key, key=True) + "]" + sp() + "=" + sp()   # [[..]] after [ = invalid Lua
        parts.append(cm + k + messy(e.value, depth + 1))
    seps = [rnd.choice([",", ";", " ,", ",\n"]) for _ in parts]
    body = "".join(p + (seps[i] if i < len(parts) - 1 or rnd.random() < 0.5 else "") + sp() for i, p in enumerate(parts))
    if rnd.random() < 0.1:
        body += " -- trailing comment\n"
    return "{" + sp() + body + "}"


def messy_doc(doc):
    head = "".join(("--" + (" " + c if c else "") + "\n") for c in doc.header)
    if rnd.random() < 0.2:
        head += "--[[ block\n comment ]]\n"
    return head + "return" + rnd.choice([" ", "\n", "  "]) + messy(doc.root, 0) + rnd.choice(["", "\n", "\n\n"])


def cpp_canon(path):
    r = subprocess.run([exe, "data", "canon", path], capture_output=True)
    return r.returncode, r.stdout, r.stderr.decode("utf-8", "replace")


BAD = ["x = 1", "return { a = os.time() }", "return { a = nil }", "return { 1 + 2 }", "return { a = 1, a = 2 }",
       "return { 10, [1] = 5 }", "return { [true] = 1 }", "return { 1 } x", "return { 'open }", "return { 1",
       "return { 9223372036854775808 }", "return { a = function() end }", "return { \"\\q\" }", "return 5",
       "return { ... }", "return { [1.5] = 1 }", "return { a = -'x' }", "return { 0x }", "return { 1e }"]

mismatch = 0
both_refused = 0
compared = 0
sizes = []
tmp = tempfile.mkdtemp(prefix="records_gate_")
try:
    for i in range(args.count):
        doc = pse_data.Document(rand_table(0), rnd.choice([[], ["generated"], ["two", ""]]))
        text = messy_doc(doc)
        p = os.path.join(tmp, "doc%d.lua" % i)
        with open(p, "wb") as f:
            f.write(text.encode("utf-8", errors="surrogateescape"))
        try:
            py = pse_data.dumps_bytes(pse_data.loads(open(p, "rb").read()))
        except pse_data.DataError as e:
            py = None
            pyerr = str(e)
        rc, cpp, err = cpp_canon(p)
        if rc != 0 and py is None:
            both_refused += 1
            if both_refused <= 3: print('both refused doc %d: %s' % (i, pyerr))
            continue
        if rc != 0 or py is None or cpp != py:
            mismatch += 1
            if mismatch <= 5:
                print("MISMATCH doc %d (%s)" % (i, p))
                print(" python:", "error " + pyerr if py is None else py[:600])
                print(" c++   :", "error " + err if rc else cpp[:600])
            continue
        # canonical again = unchanged, both sides
        p2 = p + ".canon"
        with open(p2, "wb") as f:
            f.write(py)
        rc2, cpp2, _ = cpp_canon(p2)
        if rc2 != 0 or cpp2 != py or pse_data.dumps_bytes(pse_data.loads(py)) != py:
            mismatch += 1
            print("NOT STABLE doc %d" % i)
            continue
        compared += 1
        sizes.append(len(py))
        # the content survived the messy round trip
        if pse_data.loads(py).root != doc.root:
            mismatch += 1
            print("CONTENT CHANGED doc %d" % i)
    for j, bad in enumerate(BAD):
        p = os.path.join(tmp, "bad%d.lua" % j)
        with open(p, "wb") as f:
            f.write(bad.encode())
        try:
            pse_data.loads(bad)
            py_ok = True
        except pse_data.DataError:
            py_ok = False
        rc, _, _ = cpp_canon(p)
        if py_ok or rc != 2:
            mismatch += 1
            print("BAD INPUT accepted: %r (python %s, c++ rc %d)" % (bad, "ok" if py_ok else "refused", rc))
finally:
    import shutil
    shutil.rmtree(tmp, ignore_errors=True)

print("%d random documents (%d compared, %d refused by both, canonical size %d..%d bytes) + %d bad inputs, %d mismatch(es) - %s"
      % (args.count, compared, both_refused, min(sizes or [0]), max(sizes or [0]), len(BAD), mismatch, exe))
sys.exit(min(mismatch, 255))

"""PSE ID database compiler (docs/sdk/reference/ids.md).

Source of truth: pse/ids/<game>.ids.txt (ids, names, kinds, descriptions + per exe build the address and the
signature that verifies it). Published (frozen) ids: pse/ids/<game>.published.txt - an id listed there must exist
for ever with the same name and kind; it may get new builds / addresses, never a new number.

  python scripts/gen_ids.py                         # check every game + write the generated files
  python scripts/gen_ids.py --check                 # check only (exit 1 on a problem, nothing written)
  python scripts/gen_ids.py --fill-signatures ds1   # replace "?" signatures from the exe (needs pefile)
  python scripts/gen_ids.py --verify-exe ds1        # every signature of the build against the exe (needs pefile)
  python scripts/gen_ids.py --publish ds1           # freeze every id not yet published (+ database version + 1)

--exe <path> picks the exe for --fill-signatures / --verify-exe (default: Steam, then GOG install; the build is
found by its PE timestamp + image size).

Generated (do not edit): src/generated/pse_ids_<game>.h (the table the host registers), pse/include/pse/
pse_ids_<game>.h (public PSE_ID_<game>_<Name> constants), docs/sdk/reference/ids-<game>.md (the list).
"""
import argparse
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IDS_DIR = os.path.join(ROOT, "pse", "ids")
KINDS = {"function": 1, "global": 2, "site": 3, "vtable": 4, "data": 5}
NAME_RE = re.compile(r"^[A-Za-z][A-Za-z0-9_]{0,62}$")
BUILD_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,31}$")
DEFAULT_EXES = {
    "ds1": [r"H:\SteamLibrary\steamapps\common\conflict_desert_storm\DesertStorm.exe",
            r"H:\GOG\Games\Conflict - Desert Storm\DesertStorm.EXE"],
    # Desert Storm II: the disc-free CDS II.exe (the retail SecuROM exe is no build of the database yet).
    "ds2": [r"H:\SCI Games\Conflict Desert Storm II\CDS II.exe"],
}
# Byte patches of the game's feature code (reference windows for globals avoid them - a plugin may patch there).
# ds1: DesertStormFix's table; other games: none yet (their patch tables live in their own repos).
PATCH_DEFS = {
    "ds1": os.path.join(ROOT, "scripts", "patch_defs.py"),
}
SIG_TEXT_LEN = 12   # code: bytes at the entry / instruction
SIG_DATA_LEN = 8    # .rdata / .data contents
SIG_REF_LEN = 8     # globals / IAT slots: a window of code around a reference to the address (2 + 4 + 2)


class Problem(Exception):
    pass


# ---- source format ---------------------------------------------------------------------------------------------

class Entry:
    def __init__(self, id_, name, kind, desc, line):
        self.id, self.name, self.kind, self.desc, self.line = id_, name, kind, desc, line


class Address:
    def __init__(self, id_, va, sig, line):
        self.id, self.va, self.sig, self.line = id_, va, sig, line


class Build:
    def __init__(self, name, timestamp, image, sha256, desc):
        self.name, self.timestamp, self.image, self.sha256, self.desc = name, timestamp, image, sha256, desc
        self.addrs = []


class Db:
    def __init__(self, game):
        self.game, self.title, self.version = game, "", -1
        self.entries, self.builds = [], []
        self.lines = []


def parse_sig(sig):
    """-> (check_va or None, [byte or None]) ; '-' -> (None, []) ; '?' -> None."""
    if sig == "?":
        return None
    if sig == "-":
        return (None, [])
    at = None
    m = re.fullmatch(r"@0x([0-9A-Fa-f]{1,8}):(.+)", sig)
    if m:
        at, sig = int(m.group(1), 16), m.group(2)
    if len(sig) % 2 or not re.fullmatch(r"(?:[0-9A-Fa-f]{2}|\?\?)+", sig):
        raise Problem(f"bad signature {sig!r}")
    out = [None if sig[i:i + 2] == "??" else int(sig[i:i + 2], 16) for i in range(0, len(sig), 2)]
    if not 4 <= len(out) <= 16:
        raise Problem("a signature has 4..16 bytes")
    if all(b is None for b in out):
        raise Problem("a signature needs at least one fixed byte")
    return (at, out)


def load(game):
    path = os.path.join(IDS_DIR, f"{game}.ids.txt")
    db = Db(game)
    section, build = None, None
    with open(path, encoding="utf-8") as f:
        db.lines = f.read().splitlines()
    for n, raw in enumerate(db.lines, 1):
        line = raw.split("#", 1)[0].strip() if not raw.lstrip().startswith("#") else ""
        if not line:
            continue
        try:
            if line.startswith("["):
                m = re.fullmatch(r"\[(ids|addresses ([a-z0-9_.-]+))\]", line)
                if not m:
                    raise Problem(f"unknown section {line}")
                section = "ids" if m.group(1) == "ids" else "addresses"
                if section == "addresses":
                    build = next((b for b in db.builds if b.name == m.group(2)), None)
                    if not build:
                        raise Problem(f"addresses for undeclared build {m.group(2)}")
                continue
            if section is None:
                parts = line.split(None, 1)
                if parts[0] == "game":
                    g, _, title = parts[1].partition(" ")
                    if g != game:
                        raise Problem(f"file is for game {g}")
                    db.title = title.strip().strip('"')
                elif parts[0] == "database":
                    db.version = int(parts[1])
                elif parts[0] == "build":
                    p = line.split(None, 5)
                    if len(p) < 5:
                        raise Problem("build <name> <timestamp> <image size> <sha256> [description]")
                    if not BUILD_RE.match(p[1]):
                        raise Problem(f"bad build name {p[1]}")
                    if any(b.name == p[1] for b in db.builds):
                        raise Problem(f"build {p[1]} declared twice")
                    if not re.fullmatch(r"[0-9A-Fa-f]{64}|-", p[4]):
                        raise Problem("sha256 = 64 hex digits or -")
                    db.builds.append(Build(p[1], int(p[2], 16), int(p[3], 16), p[4].upper(),
                                           p[5] if len(p) > 5 else ""))
                else:
                    raise Problem(f"unknown header line {parts[0]}")
            elif section == "ids":
                p = line.split(None, 3)
                if len(p) < 3:
                    raise Problem("<id> <name> <kind> [description]")
                db.entries.append(Entry(int(p[0], 0), p[1], p[2], p[3] if len(p) > 3 else "", n))
            else:
                p = line.split()
                if len(p) != 3:
                    raise Problem("<id> <va> <signature>")
                build.addrs.append(Address(int(p[0], 0), int(p[1], 16), p[2], n))
        except (Problem, ValueError) as e:
            raise Problem(f"{path}({n}): {e}")
    return db


def load_published(game):
    path = os.path.join(IDS_DIR, f"{game}.published.txt")
    out = {}
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for n, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            p = line.split()
            if len(p) != 3:
                raise Problem(f"{path}({n}): <id> <name> <kind>")
            out[int(p[0], 0)] = (p[1], p[2])
    return out


def check(db, published, need_signatures=True):
    problems = []
    seen_ids, seen_names = {}, {}
    if not db.title or db.version < 0:
        problems.append("missing 'game <id> <title>' / 'database <n>' header")
    for e in db.entries:
        where = f"line {e.line}"
        if not 1 <= e.id <= 0xFFFFFF:
            problems.append(f"{where}: id {e.id} out of range 1..16777215")
        if e.id in seen_ids:
            problems.append(f"{where}: id {e.id} used twice (line {seen_ids[e.id]})")
        seen_ids[e.id] = e.line
        if not NAME_RE.match(e.name):
            problems.append(f"{where}: bad name {e.name!r} ([A-Za-z][A-Za-z0-9_]*, <= 63)")
        low = e.name.lower()
        if low in seen_names:
            problems.append(f"{where}: name {e.name} used twice (line {seen_names[low]}, names are case-insensitive)")
        seen_names[low] = e.line
        if e.kind not in KINDS:
            problems.append(f"{where}: kind {e.kind!r} (one of {', '.join(KINDS)})")
    by_id = {e.id: e for e in db.entries}
    for pid, (pname, pkind) in sorted(published.items()):
        e = by_id.get(pid)
        if not e:
            problems.append(f"published id {pid} ({pname}) was removed - published ids are never removed or renumbered")
        elif e.name != pname:
            problems.append(f"published id {pid} was renamed {pname} -> {e.name} - published names never change")
        elif e.kind != pkind:
            problems.append(f"published id {pid} ({pname}) changed kind {pkind} -> {e.kind}")
    for name in {n for n, _ in published.values()}:
        owners = [i for i, (n, _) in published.items() if n == name]
        if len(owners) > 1:
            problems.append(f"published name {name} has several ids {owners}")
    if not db.builds:
        problems.append("no build declared")
    for b in db.builds:
        seen = {}
        for a in b.addrs:
            where = f"line {a.line}"
            if a.id not in by_id:
                problems.append(f"{where}: build {b.name}: unknown id {a.id}")
            if a.id in seen:
                problems.append(f"{where}: build {b.name}: id {a.id} twice (line {seen[a.id]})")
            seen[a.id] = a.line
            if not 0x1000 <= a.va <= 0xFFFFFFFF:
                problems.append(f"{where}: bad va {a.va:#x}")
            try:
                s = parse_sig(a.sig)
                if s is None and need_signatures:
                    problems.append(f"{where}: signature not filled ('?' - run --fill-signatures)")
            except Problem as e:
                problems.append(f"{where}: {e}")
    return problems


# ---- the exe ---------------------------------------------------------------------------------------------------

class Exe:
    def __init__(self, path):
        import pefile  # only needed here
        self.path = path
        pe = pefile.PE(path, fast_load=False)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.timestamp = pe.FILE_HEADER.TimeDateStamp
        self.image = pe.OPTIONAL_HEADER.SizeOfImage
        self.img = pe.get_memory_mapped_image()
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\0").decode("latin-1")
            va = self.base + s.VirtualAddress
            self.sections.append((name, va, max(s.Misc_VirtualSize, s.SizeOfRawData), s.SizeOfRawData))
        self.iat = set()
        for e in getattr(pe, "DIRECTORY_ENTRY_IMPORT", []):
            for i in e.imports:
                self.iat.add(i.address)
        text = self.section(".text")
        self.text_va, self.text = text[1], self.read(text[1], text[3])

    def section(self, name):
        return next(s for s in self.sections if s[0] == name)

    def section_of(self, va):
        return next((s for s in self.sections if s[1] <= va < s[1] + s[2]), None)

    def read(self, va, n):
        off = va - self.base
        b = bytes(self.img[off:off + n])
        return b + bytes(n - len(b))


def find_exe(game, override):
    paths = [override] if override else DEFAULT_EXES.get(game, [])
    for p in paths:
        if p and os.path.exists(p):
            return Exe(p)
    raise Problem(f"no exe found ({', '.join(paths)}); pass --exe")


def build_for(db, exe):
    for b in db.builds:
        if b.timestamp == exe.timestamp and b.image == exe.image:
            return b
    raise Problem(f"{exe.path}: timestamp {exe.timestamp:#x} / image {exe.image:#x} is no build of {db.game}")


def sig_bytes(exe, va, n):
    return exe.read(va, n).hex().upper()


def avoid_set(db, b):
    """Byte ranges we'd rather not use as reference windows: hook sites / entries (and the patch tables')."""
    spans = []
    kinds = {e.id: e.kind for e in db.entries}
    for a in b.addrs:
        if kinds.get(a.id) in ("function", "site"):
            spans.append((a.va - 2, a.va + 12))
    pd = PATCH_DEFS.get(db.game)
    if pd and os.path.exists(pd):
        for m in re.finditer(r"0x([0-9A-Fa-f]{6,8})\b|\b([0-9A-F]{8}) [0-9A-F]+ [0-9A-F]+", open(pd).read()):
            v = int(m.group(1) or m.group(2), 16)
            if 0x401000 <= v < 0x5D8000:
                spans.append((v - 8, v + 16))
    return spans


def compute_sig(db, b, e, a, exe, avoid):
    sec = exe.section_of(a.va)
    if not sec:
        raise Problem(f"id {e.id} {e.name}: {a.va:#x} is in no section")
    in_raw = a.va + 4 <= sec[1] + sec[3]
    want_ref = e.kind == "global" or a.va in exe.iat or not in_raw
    if sec[0] == ".text" and not want_ref:
        n = min(SIG_TEXT_LEN, sec[1] + sec[3] - a.va)
        return sig_bytes(exe, a.va, n)
    if not want_ref:
        n = SIG_TEXT_LEN if e.kind == "vtable" else SIG_DATA_LEN
        n = min(n, sec[1] + sec[3] - a.va)
        if any(exe.read(a.va, n)):
            return sig_bytes(exe, a.va, n)
    # reference window in .text
    needle = struct.pack("<I", a.va)
    first = None
    pos = exe.text.find(needle)
    while pos >= 0:
        va = exe.text_va + pos - 2
        if first is None:
            first = va
        if not any(lo < va + SIG_REF_LEN and va < hi for lo, hi in avoid):
            return f"@0x{va:08X}:{sig_bytes(exe, va, SIG_REF_LEN)}"
        pos = exe.text.find(needle, pos + 1)
    if first is not None:
        return f"@0x{first:08X}:{sig_bytes(exe, first, SIG_REF_LEN)}"
    raise Problem(f"id {e.id} {e.name}: no code reference to {a.va:#x} (use '-' or a hand-picked @va:bytes)")


def verify(db, b, exe):
    problems = []
    by_id = {e.id: e for e in db.entries}
    for a in b.addrs:
        s = parse_sig(a.sig)
        if s is None or not s[1]:
            continue
        at, pat = s
        va = a.va if at is None else at
        got = exe.read(va, len(pat))
        if any(p is not None and p != g for p, g in zip(pat, got)):
            problems.append(f"line {a.line}: {by_id[a.id].name} {a.sig} != {got.hex().upper()} at {va:#x}")
    return problems


def fill(db, game, exe):
    b = build_for(db, exe)
    by_id = {e.id: e for e in db.entries}
    avoid = avoid_set(db, b)
    lines, filled = db.lines, 0
    for a in b.addrs:
        if a.sig != "?":
            continue
        sig = compute_sig(db, b, by_id[a.id], a, exe, avoid)
        raw = lines[a.line - 1]
        lines[a.line - 1] = re.sub(r"(\s)\?(\s*(#.*)?)$", lambda m: m.group(1) + sig + m.group(2), raw)
        filled += 1
    with open(os.path.join(IDS_DIR, f"{game}.ids.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{game}: {filled} signature(s) filled from {exe.path}")


def publish(db, game, published):
    new = [e for e in db.entries if e.id not in published]
    if not new:
        print(f"{game}: nothing new to publish")
        return
    path = os.path.join(IDS_DIR, f"{game}.published.txt")
    head = ""
    if not os.path.exists(path):
        head = (f"# Published (frozen) ids of {game} - written by scripts/gen_ids.py --publish, never edited by hand.\n"
                "# An id listed here is never removed, renumbered, renamed or given another kind.\n"
                "# <id> <name> <kind>\n")
    with open(path, "a", encoding="utf-8", newline="\n") as f:
        f.write(head)
        for e in sorted(new, key=lambda e: e.id):
            f.write(f"{e.id} {e.name} {e.kind}\n")
    # database version + 1
    lines = db.lines
    for i, raw in enumerate(lines):
        if re.match(r"^database\s+\d+", raw):
            lines[i] = re.sub(r"\d+", str(db.version + 1), raw, count=1)
            break
    with open(os.path.join(IDS_DIR, f"{game}.ids.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print(f"{game}: published {len(new)} id(s), database version {db.version + 1}")


# ---- generated files -------------------------------------------------------------------------------------------

def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def write_if_changed(path, text):
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    if old != text:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote " + os.path.relpath(path, ROOT))


def generate(db, published):
    g = db.game
    ns = f"pse_ids_{g}"
    entries = sorted(db.entries, key=lambda e: e.id)
    o = [f"// GENERATED by scripts/gen_ids.py from pse/ids/{g}.ids.txt - do not edit.",
         "// The host registers it at start-up: pse::ids::RegisterGameForProcess(" + ns + "::kGame) (docs/sdk/reference/ids.md).",
         "#pragma once", '#include "ids.h"', "", f"namespace {ns} {{", "",
         f"inline constexpr pse::ids::EntryDef kEntries[] = {{"]
    for e in entries:
        o.append(f"    {{{e.id}u, {c_str(e.name)}, {KINDS[e.kind]}u}},")
    o.append("};")
    for b in db.builds:
        o.append(f"inline constexpr pse::ids::AddressDef kAddresses_{b.name.replace('-', '_').replace('.', '_')}[] = {{")
        for a in sorted(b.addrs, key=lambda a: a.id):
            at, pat = parse_sig(a.sig) or (None, [])
            hexs = "".join("??" if x is None else f"{x:02X}" for x in pat)
            o.append(f"    {{{a.id}u, 0x{a.va:08X}u, 0x{(at if at is not None else (a.va if pat else 0)):08X}u, {c_str(hexs)}}},")
        o.append("};")
    o.append("inline constexpr pse::ids::BuildDef kBuilds[] = {")
    for b in db.builds:
        arr = f"kAddresses_{b.name.replace('-', '_').replace('.', '_')}"
        o.append(f"    {{{c_str(b.name)}, {c_str(b.desc)}, 0x{b.timestamp:08X}u, 0x{b.image:08X}u, {c_str(b.sha256)}, "
                 f"{arr}, static_cast<uint32_t>(sizeof {arr} / sizeof {arr}[0])}},")
    o.append("};")
    o.append("inline constexpr pse::ids::PublishedDef kPublished[] = {")
    for pid in sorted(published):
        o.append(f"    {{{pid}u, {c_str(published[pid][0])}}},")
    if not published:
        o.append("    {0u, nullptr},")
    o.append("};")
    o.append(f"inline constexpr pse::ids::GameDef kGame = {{sizeof(pse::ids::GameDef), {c_str(g)}, {c_str(db.title)}, "
             f"{db.version}u, kEntries, static_cast<uint32_t>(sizeof kEntries / sizeof kEntries[0]), kBuilds, "
             f"static_cast<uint32_t>(sizeof kBuilds / sizeof kBuilds[0]), kPublished, {len(published)}u}};")
    o += ["", f"}}  // namespace {ns}", ""]
    write_if_changed(os.path.join(ROOT, "src", "generated", f"pse_ids_{g}.h"), "\n".join(o))

    guard = f"PSE_IDS_{g.upper()}_H"
    p = [f"/* GENERATED by scripts/gen_ids.py from pse/ids/{g}.ids.txt - do not edit.",
         f" * PSE ID constants for {db.title} (game id \"{g}\"), database version {db.version}.",
         " * Resolve with pse.ids: ids->Resolve(\"" + g + "\", PSE_ID(" + g + ", Name)) - docs/sdk/reference/ids.md,",
         f" * the list with descriptions: docs/sdk/reference/ids-{g}.md. Published ids never change their number. */",
         f"#ifndef {guard}", f"#define {guard}", "",
         f"#define PSE_IDS_{g.upper()}_DATABASE_VERSION {db.version}u", ""]
    width = max(len(e.name) for e in entries) + len(g) + 8
    for e in entries:
        p.append(f"#define {('PSE_ID_' + g + '_' + e.name).ljust(width)} {e.id}u  /* {e.kind} */")
    p += ["", f"#endif /* {guard} */", ""]
    write_if_changed(os.path.join(ROOT, "pse", "include", "pse", f"pse_ids_{g}.h"), "\n".join(p))

    m = [f"# IDs: {db.title} (`{g}`)", "",
         f"GENERATED by `scripts/gen_ids.py` from [`pse/ids/{g}.ids.txt`](../../../pse/ids/{g}.ids.txt) - do not edit.",
         f"How to use and add ids: [ids.md](ids.md). Database version **{db.version}**, {len(entries)} ids "
         f"({len(published)} published).", "", "## Builds", "", "| Build | PE timestamp | Image size | SHA-256 | Notes |",
         "| --- | --- | --- | --- | --- |"]
    for b in db.builds:
        m.append(f"| `{b.name}` | `0x{b.timestamp:08X}` | `0x{b.image:X}` | `{b.sha256[:16]}…` | {b.desc} |")
    addr = {b.name: {a.id: a.va for a in b.addrs} for b in db.builds}
    for kind, title in (("function", "Functions"), ("site", "Code sites (calls, jumps, immediates)"),
                        ("global", "Globals"), ("vtable", "Vtables"), ("data", "Data (tables, vtable / IAT slots, constants)")):
        rows = [e for e in entries if e.kind == kind]
        if not rows:
            continue
        m += ["", f"## {title}", "", "| Id | Name | " + " | ".join(f"`{b.name}`" for b in db.builds) + " | What |",
              "| --- | --- | " + " | ".join("---" for _ in db.builds) + " | --- |"]
        for e in rows:
            vas = " | ".join(f"`0x{addr[b.name][e.id]:06X}`" if e.id in addr[b.name] else "–" for b in db.builds)
            star = "" if e.id in published else " (unpublished)"
            m.append(f"| {e.id} | `{e.name}`{star} | {vas} | {e.desc.replace('|', '/')} |")
    m.append("")
    write_if_changed(os.path.join(ROOT, "docs", "sdk", "reference", f"ids-{g}.md"), "\n".join(m))


def games():
    return sorted(f[:-8] for f in os.listdir(IDS_DIR) if f.endswith(".ids.txt"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="check only, write nothing")
    ap.add_argument("--fill-signatures", metavar="GAME")
    ap.add_argument("--verify-exe", metavar="GAME")
    ap.add_argument("--publish", metavar="GAME")
    ap.add_argument("--exe")
    a = ap.parse_args()
    try:
        if a.fill_signatures:
            g = a.fill_signatures
            db = load(g)
            probs = check(db, load_published(g), need_signatures=False)
            if probs:
                raise Problem("\n".join(probs))
            fill(db, g, find_exe(g, a.exe))
        if a.verify_exe:
            g = a.verify_exe
            db = load(g)
            exe = find_exe(g, a.exe)
            probs = verify(db, build_for(db, exe), exe)
            print(f"{g}: {len(build_for(db, exe).addrs)} signature(s) checked against {exe.path}: "
                  f"{len(probs)} mismatch(es)")
            if probs:
                raise Problem("\n".join(probs))
        if a.publish:
            g = a.publish
            db = load(g)
            probs = check(db, load_published(g))
            if probs:
                raise Problem("\n".join(probs))
            publish(db, g, load_published(g))
        failed = False
        for g in games():
            db = load(g)
            published = load_published(g)
            probs = check(db, published)
            for p in probs:
                print(f"{g}: {p}")
            if probs:
                failed = True
                continue
            if not a.check:
                generate(db, published)
            print(f"{g}: {len(db.entries)} ids, {len(published)} published, "
                  f"builds {', '.join(b.name for b in db.builds)} - ok")
        return 1 if failed else 0
    except Problem as e:
        print(f"error: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

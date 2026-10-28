"""Per DesertStormFix plugin: the dsfix.host calls (src/core/dsfix_host.spec) its sources make, the dsfix.services
members it uses, the interface it publishes and how many functions dsfix.dll calls in it - the table in
docs/sdk/ds1-host-layer.md. usage: python games/ds1/scripts/host_usage.py > table.md
Names are matched in the sources (comments left out), so a name that is also the plugin's own local helper can show up;
the generated tables (plugins/<g>/generated, src/generated) are exact."""
import glob, os, re
DS1 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # games/ds1

def strip(s):
    return re.sub(r"/\*.*?\*/", "", re.sub(r"//[^\n]*", "", s), flags=re.S)

host = []
for line in open(os.path.join(DS1, "src", "core", "dsfix_host.spec"), encoding="utf-8"):
    line = line.strip()
    if not line or line.startswith(("#", "interface", "side", "pod", "enum")):
        continue
    m = re.search(r"\b((?:\w+::)+)(\w+)\s*\(", line)
    if m:
        host.append((m.group(1) + m.group(2), m.group(2)))

services = re.findall(r"\(PSE_CALL\*\s*(\w+)\)", open(os.path.join(DS1, "src", "core", "dsfix_services.h"),
                                                    encoding="utf-8").read())
rows = []
for d in sorted(glob.glob(os.path.join(DS1, "plugins", "*"))):
    name = os.path.basename(d)
    if name in ("kit",) or not os.path.isdir(d):
        continue
    pse = glob.glob(os.path.join(d, "*.pse"))
    plugin = os.path.basename(pse[0])[:-4] if pse else name
    src = "".join(strip(open(f, encoding="utf-8", errors="replace").read()) for f in glob.glob(os.path.join(d, "*.cpp")))
    own = set()
    spec = glob.glob(os.path.join(d, "*.spec"))
    exported = 0
    iface = ""
    if spec:
        for line in open(spec[0], encoding="utf-8"):
            line = line.strip()
            if line.startswith("interface "):
                iface = line.split()[1]
            elif line and not line.startswith(("#", "side", "pod", "enum", "interface")):
                exported += 1
    # functions the plugin defines itself (not host calls)
    for m in re.finditer(r"^[\w:<>\*&,][\w:<>\*&\s,]*?\b((?:\w+::)+\w+)\s*\([^;{]*?\)\s*(?:const\s*)?\{", src, re.M | re.S):
        own.add(m.group(1))
    used = sorted({full for full, short in host if full not in own and re.search(r"\b%s\s*\(" % short, src)})
    svc = sorted({s for s in services if re.search(r"(Services\(\)|services|svc)\s*->\s*%s\b" % s, src)})
    ext = "external" if "DSFIX_KIT_PLUGIN" not in src and "kit::" not in src else "kit"
    if not iface:   # interfaces registered by hand (v1 headers next to the plugin): DSFIX_<NAME>_INTERFACE
        m = re.search(r"RegisterInterface\(kit::Self\(\), DSFIX_(\w+)_INTERFACE", src)
        if m:
            iface = "dsfix." + m.group(1).lower() + " (by hand)"
    if ext == "external":
        used, svc = [], []
    rows.append((plugin, iface, exported, used, svc, ext))

print("| Plugin | Publishes | dsfix.dll calls in it | dsfix.host calls it makes | dsfix.services members |")
print("| --- | --- | --- | --- | --- |")
for plugin, iface, exported, used, svc, ext in rows:
    short = ", ".join("`%s`" % u.split("::")[-1] for u in used) or "-"
    if ext == "external":
        print("| `%s` | - | - | (external: the public SDK + pse.game.ds1 only) | - |" % plugin)
        continue
    print("| `%s` | %s | %s | %d: %s | %s |" % (plugin, "`%s`" % iface if iface else "-", exported or "-", len(used), short,
                                             ", ".join("`%s`" % s for s in svc) or "-"))

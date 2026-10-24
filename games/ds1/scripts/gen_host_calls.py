"""Cross-DLL calls between dsfix.dll and DesertStormFix's own plugin DLLs, generated from spec files (plain C tables).

Every DLL has its own static C runtime, so calls cross as plain C: bool -> int, std::string -> const char*, a
std::vector<std::string> -> one '\\x1E'-separated const char*, a reference to a plain struct -> a pointer. Anything
else is refused (write that call by hand).

Spec files (one declaration per line, exactly as in its header but without default arguments):

    interface dsfix.host          the PSE interface id (version 1; the layout hash below guards mismatches)
    side host                     host = dsfix.dll implements them (plugins call them through the kit's client)
                                  plugin <name> = the plugin implements them (dsfix.dll's wrappers call it)
    pod padio::JoyState           a plain struct that may be passed by pointer / reference
    enum features::DisplayMode    an enum class passed as int32_t
    bool features::GameFocused()
    const char* features::GameText(uint32_t hash)  @GameTextHash   (@ = the member name, for overloads)
    bool features::PhotoModeOpen() = false        (side plugin: what dsfix.dll's wrapper answers without the plugin)

Outputs (all in src/generated/ unless noted, never edited by hand):
    side host:   dsfix_host.h (the table), dsfix_host_impl.inc (dsfix.dll: thunks + table + Register),
                 dsfix_host_client.inc (the kit's client stubs; a plugin compiles it with DSFIX_OWN_<member> defined
                 for the functions it implements itself, see plugin specs)
    side plugin: plugins/<name>/generated/<base>_iface.h (the table), plugins/<name>/generated/<base>_export.inc
                 (the plugin: thunks + table + Register<Base>Interface()), plugins/<name>/generated/own_functions.h
                 (DSFIX_OWN_* for the host client), src/generated/<base>_wrappers.inc (dsfix.dll: features::X calling
                 the plugin, or the default)

usage: gen_host_calls.py [--check]   (all *.spec under src/core and plugins/*)"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DS1 = os.path.dirname(HERE)
SRC = os.path.join(DS1, "src")
GEN = os.path.join(SRC, "generated")
SCALARS = {"void", "bool", "char", "int", "unsigned", "float", "double", "size_t", "DWORD", "HWND", "HMODULE",
           "int8_t", "uint8_t", "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t", "uintptr_t",
           "LONG", "UINT", "BOOL", "WORD", "BYTE"}
SEP = "\\x1E"


class SpecError(Exception):
    pass


def fnv(text):
    h = 0x811C9DC5
    for b in text.encode("utf-8"):
        h = ((h ^ b) * 0x01000193) & 0xFFFFFFFF
    return h


def split_params(s):
    s = s.strip()
    if not s or s == "void":
        return []
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "<(":
            depth += 1
        elif ch in ">)":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += ch
    out.append(cur.strip())
    return out


STRING_OUT = 1024   # buffer size for a std::string& out parameter


class Type:
    """One parameter / return type and how it crosses: its C type, and per side the C expression for the call plus
    statements before / after it (out parameters copied back)."""

    def __init__(self, text, pods, enums, is_return):
        t = " ".join(text.replace("*", " * ").replace("&", " & ").split())
        t = re.sub(r"\s*([<>])\s*", r"\1", t)
        self.cpp = t.replace(" *", "*").replace(" &", "&")
        base = t.replace("const ", "").replace("*", "").replace("&", "").strip()
        self.base = base
        if t == "std::string" and is_return:
            self.kind, self.c = "string_ret", "const char*"
        elif t == "const std::string &" and not is_return:
            self.kind, self.c = "string_in", "const char*"
        elif t == "std::string &" and not is_return:
            self.kind, self.c = "string_out", "char*"
        elif t == "std::vector<std::string>" and is_return:
            self.kind, self.c = "strings_ret", "const char*"
        elif t in ("const std::vector<std::string> &", "const std::vector<std::string>&") and is_return:
            self.kind, self.c = "strings_cref", "const char*"
        elif t == "bool":
            self.kind, self.c = "bool", "int"
        elif t == "bool &" and not is_return:
            self.kind, self.c = "boolref", "int*"
        elif base in enums and t.endswith("&") and not t.startswith("const") and not is_return:
            self.kind, self.c = "enumref", "int32_t*"
        elif t.endswith("&") and not is_return:
            if base not in SCALARS and base not in pods:
                raise SpecError("reference to '%s' (not a plain type - declare it 'pod' if it is one)" % base)
            self.kind, self.c = "ref", t[:-1].strip().replace(" *", "*") + "*"
        elif base in enums and "*" not in t and "&" not in t:
            self.kind, self.c = "enum", "int32_t"
        elif "*" in t and "&" not in t:
            if base not in SCALARS and base not in pods:
                raise SpecError("pointer to '%s' (not a plain type - declare it 'pod' if it is one)" % base)
            self.kind, self.c = "plain", t.replace(" *", "*")
        elif (base in SCALARS or base in pods) and t == base:
            self.kind, self.c = "plain", t
        else:
            raise SpecError("type '%s' can't cross as plain C (write this call by hand)" % text)

    # client side (C++ value `name` -> C argument): (statements before, argument, statements after)
    def client_arg(self, name, i):
        k = self.kind
        if k == "bool":
            return [], "(%s) ? 1 : 0" % name, []
        if k == "string_in":
            return [], "(%s).c_str()" % name, []
        if k == "ref":
            return [], "&(%s)" % name, []
        if k == "enum":
            return [], "static_cast<int32_t>(%s)" % name, []
        if k == "boolref":
            return ["int b%d = %s ? 1 : 0;" % (i, name)], "&b%d" % i, ["%s = b%d != 0;" % (name, i)]
        if k == "enumref":
            return (["int32_t e%d = static_cast<int32_t>(%s);" % (i, name)], "&e%d" % i,
                    ["%s = static_cast<%s>(e%d);" % (name, self.base, i)])
        if k == "string_out":
            return ["char s%d[%d] = {};" % (i, STRING_OUT)], "s%d" % i, ["%s = s%d;" % (name, i)]
        return [], name, []

    # thunk side (C argument `a<i>` -> C++ value): (statements before, argument, statements after)
    def thunk_arg(self, i):
        a, k = "a%d" % i, self.kind
        if k == "bool":
            return [], "%s != 0" % a, []
        if k == "string_in":
            return [], "std::string(%s ? %s : \"\")" % (a, a), []
        if k == "ref":
            return [], "*%s" % a, []
        if k == "enum":
            return [], "static_cast<%s>(%s)" % (self.base, a), []
        if k == "boolref":
            return ["bool b%d = %s && *%s != 0;" % (i, a, a)], "b%d" % i, ["if (%s) *%s = b%d ? 1 : 0;" % (a, a, i)]
        if k == "enumref":
            return (["%s e%d = static_cast<%s>(%s ? *%s : 0);" % (self.base, i, self.base, a, a)], "e%d" % i,
                    ["if (%s) *%s = static_cast<int32_t>(e%d);" % (a, a, i)])
        if k == "string_out":
            return (["std::string s%d;" % i], "s%d" % i,
                    ["if (%s) strncpy_s(%s, %d, s%d.c_str(), _TRUNCATE);" % (a, a, STRING_OUT, i)])
        return [], a, []

    def ret_to_c(self, expr):    # thunk: C++ result -> C
        return {"bool": "(%s) ? 1 : 0", "enum": "static_cast<int32_t>(%s)"}.get(self.kind, "%s") % expr

    def ret_from_c(self, expr):  # client: C result -> C++
        if self.kind == "bool":
            return "(%s) != 0" % expr
        if self.kind == "enum":
            return "static_cast<%s>(%s)" % (self.base, expr)
        return expr


class Func:
    def __init__(self, line, pods, enums):
        m = re.match(r"^(.+?)\s*\b([\w:]+)\s*\((.*)\)\s*(?:@(\w+))?\s*(?:=\s*(.+?))?\s*$", line)
        if not m:
            raise SpecError("not a declaration")
        ret, self.qual, params, alias, self.default = m.groups()
        self.ns, _, self.name = self.qual.rpartition("::")
        self.member = alias or self.name
        self.ret = Type(ret, pods, enums, True)
        self.params = []
        for i, p in enumerate(split_params(params)):
            pm = re.match(r"^(.*?)\s*\b(\w+)\s*(\[\d*\])?$", p)
            if not pm or not pm.group(1).strip():
                raise SpecError("parameter '%s' needs a type and a name" % p)
            ptype = pm.group(1) + ("*" if pm.group(3) else "")
            self.params.append((Type(ptype, pods, enums, False), pm.group(2)))
        self.norm = "%s %s(%s)" % (self.ret.cpp, self.qual, ", ".join(t.cpp for t, _ in self.params))

    def c_proto(self, name):
        return "%s (PSE_CALL* %s)(%s)" % (self.ret.c, name, ", ".join(t.c for t, _ in self.params) or "void")

    def cpp_params(self):
        return ", ".join("%s %s" % (t.cpp, n) for t, n in self.params)


def read_spec(path):
    iface, side, pods, enums, funcs = None, None, set(), set(), []
    for no, raw in enumerate(open(path, encoding="utf-8"), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            if line.startswith("interface "):
                iface = line.split()[1]
            elif line.startswith("side "):
                side = line.split()[1:]
            elif line.startswith("pod "):
                pods.add(line.split()[1])
            elif line.startswith("enum "):
                enums.add(line.split()[1])
            else:
                funcs.append(Func(line, pods, enums))
        except SpecError as e:
            raise SystemExit("%s:%d: %s\n    %s" % (path, no, e, raw.rstrip()))
    if not iface or not side:
        raise SystemExit("%s: needs 'interface' and 'side' lines" % path)
    members = [f.member for f in funcs]
    dup = {m for m in members if members.count(m) > 1}
    if dup:
        raise SystemExit("%s: duplicate member names %s (use @Name for overloads)" % (path, ", ".join(sorted(dup))))
    return iface, side, funcs


def ident(iface):
    return re.sub(r"\W", "_", iface).upper()


def struct_name(iface):
    return "".join(p.capitalize() for p in re.split(r"[^a-z0-9]", iface.lower()) if p) + "V1"


def header(iface, funcs, layout, guard):
    s = "// Generated by games/ds1/scripts/gen_host_calls.py from its .spec - don't edit. DesertStormFix-internal: the\n"
    s += "// layout changes with every DesertStormFix build (checked by the layout hash); not an SDK interface.\n"
    s += "#pragma once\n#include <stddef.h>\n#include <stdint.h>\n\n#include \"pse/pse_api.h\"\n\n"
    s += "#define %s_INTERFACE \"%s\"\n#define %s_LAYOUT 0x%08Xu\n\n" % (guard, iface, guard, layout)
    s += "typedef struct %s {\n    uint32_t size;\n    uint32_t version;   // 1\n    uint32_t layout;    // %s_LAYOUT\n" % (
        struct_name(iface), guard)
    for f in funcs:
        s += "    %s;\n" % f.c_proto(f.member)
    s += "} %s;\n" % struct_name(iface)
    return s


JOIN = ("    static thread_local std::string s;\n    s.clear();\n    for (const std::string& v : r) {\n"
        "        if (!s.empty()) s += '%s';\n        s += v;\n    }\n" % SEP)
SPLIT = ("    if (j && *j)\n        for (const char* p = j;; ++p) {\n            const char* e = p;\n"
         "            while (*e && *e != '%s') ++e;\n            out.emplace_back(p, e);\n            if (!*e) break;\n"
         "            p = e;\n        }\n" % SEP)


def lines(stmts, indent):
    return "".join("%s%s\n" % (indent, x) for x in stmts)


def thunks(funcs, call_target):
    """C thunks: C arguments -> the real C++ function -> C result."""
    out = ""
    for f in funcs:
        args = ", ".join("%s a%d" % (t.c, i) for i, (t, _) in enumerate(f.params)) or "void"
        pre, call_args, post = [], [], []
        for i, (t, _) in enumerate(f.params):
            a, b, c = t.thunk_arg(i)
            pre += a
            call_args.append(b)
            post += c
        call = "%s(%s)" % (call_target(f), ", ".join(call_args))
        out += "%s PSE_CALL t_%s(%s) {\n" % (f.ret.c, f.member, args)
        out += lines(pre, "    ")
        k = f.ret.kind
        if f.ret.cpp == "void":
            out += "    %s;\n" % call + lines(post, "    ")
        elif k == "string_ret":
            out += "    static thread_local std::string s;\n    s = %s;\n" % call
            out += lines(post, "    ") + "    return s.c_str();\n"
        elif k in ("strings_ret", "strings_cref"):
            out += "    const std::vector<std::string>& r = %s;\n" % call + JOIN
            out += lines(post, "    ") + "    return s.c_str();\n"
        else:
            out += "    auto r = %s;\n" % call + lines(post, "    ")
            out += "    return %s;\n" % f.ret.ret_to_c("r")
        out += "}\n"
    return out


def client_body(f, getter, default):
    """features::X() calling the C table (or the default when there is none)."""
    pre, call_args, post = [], [], []
    for i, (t, n) in enumerate(f.params):
        a, b, c = t.client_arg(n, i)
        pre += a
        call_args.append(b)
        post += c
    call = "t->%s(%s)" % (f.member, ", ".join(call_args))
    k, cpp = f.ret.kind, f.ret.cpp
    b = "    const auto* t = %s();\n" % getter + lines(pre, "    ")
    P = lines(post, "        ")
    if cpp == "void":
        return b + "    if (t) {\n        %s;\n%s    }\n" % (call, P)
    if k == "string_ret":
        d = default or "\"\""
        return (b + "    const char* r = nullptr;\n    if (t) {\n        r = %s;\n%s    }\n" % (call, P) +
                "    return r ? std::string(r) : std::string(%s);\n" % d)
    if k in ("strings_ret", "strings_cref"):
        v = ("    static thread_local std::vector<std::string> out;\n    out.clear();\n" if k == "strings_cref"
             else "    std::vector<std::string> out;\n")
        return b + v + "    const char* j = nullptr;\n    if (t) {\n        j = %s;\n%s    }\n" % (call, P) + SPLIT + "    return out;\n"
    d = default if default is not None else ("false" if k == "bool" else
                                             ("nullptr" if "*" in cpp else cpp.replace("const ", "") + "{}"))
    if not post:
        return b + "    return t ? %s : %s;\n" % (f.ret.ret_from_c(call), d)
    return b + "    if (!t) return %s;\n    auto r = %s;\n%s    return %s;\n" % (d, call, lines(post, "    "), f.ret.ret_from_c("r"))


def definition(f, body):
    return "%s %s(%s) {\n%s}\n" % (f.ret.cpp, f.qual, f.cpp_params(), body)


GETTER_HOST = """namespace {
const %(S)s* Table() {
    static const %(S)s* t = nullptr;
    static bool asked = false;
    if (!asked && kit::Messaging()) {
        asked = true;
        t = static_cast<const %(S)s*>(kit::Query(%(G)s_INTERFACE, 1));
        if (t && (t->size < sizeof(%(S)s) || t->layout != %(G)s_LAYOUT)) {
            if (kit::Log())
                kit::Log()->Write(kit::Self(), PSE_LOG_ERROR, "%%s: dsfix.dll's " %(G)s_INTERFACE " is from another "
                                  "DesertStormFix build - its calls answer nothing (install the plugins together)",
                                  kit::Name());
            t = nullptr;
        }
    }
    return t;
}
}  // namespace
"""

GETTER_PLUGIN = """namespace {
// Resolved once the plugins are loaded (features::PluginsLoaded); asked again before that.
const %(S)s* Table() {
    static const %(S)s* t = nullptr;
    static bool settled = false;
    if (t || settled) return t;
    const PSEMessagingInterface* m = pse::MessagingInterface();
    if (!m) return nullptr;
    const bool loaded = features::PluginsLoaded();
    t = static_cast<const %(S)s*>(m->QueryInterface(pse_host::Core(), %(G)s_INTERFACE, 1));
    if (t && (t->size < sizeof(%(S)s) || t->layout != %(G)s_LAYOUT)) {
        dslog::Write("[fail] %%s is from another DesertStormFix build - not used", %(G)s_INTERFACE);
        t = nullptr;
    }
    settled = loaded;
    return t;
}
}  // namespace
"""


def generate(path):
    iface, side, funcs = read_spec(path)
    layout = fnv(iface + "\n" + "\n".join(f.norm + "@" + f.member for f in funcs))
    G, S = ident(iface), struct_name(iface)
    base = os.path.splitext(os.path.basename(path))[0]
    files = {}
    banner = "// Generated by games/ds1/scripts/gen_host_calls.py from %s - don't edit.\n" % os.path.relpath(path, DS1).replace("\\", "/")
    if side[0] == "host":
        files[os.path.join(GEN, base + ".h")] = header(iface, funcs, layout, G)
        impl = banner + "// dsfix.dll: the C thunks and the table (included by features/host_iface.cpp).\nnamespace {\n"
        impl += thunks(funcs, lambda f: f.qual)
        impl += "const %s kTable = {sizeof(%s), 1, %s_LAYOUT,\n%s};\n}  // namespace\n" % (
            S, S, G, ",\n".join("    &t_%s" % f.member for f in funcs))
        files[os.path.join(GEN, base + "_impl.inc")] = impl
        cl = banner + "// The dsfix kit's client (plugins/kit/host_client.cpp, compiled into each plugin): each call goes to\n"
        cl += "// dsfix.dll; a plugin's own functions are left out (DSFIX_OWN_<member> from its own_functions.h).\n"
        cl += GETTER_HOST % {"S": S, "G": G}
        for f in funcs:
            cl += "#ifndef DSFIX_OWN_%s\n%s#endif\n" % (f.member, definition(f, client_body(f, "Table", None)))
        files[os.path.join(GEN, base + "_client.inc")] = cl
    else:
        plugin = side[1]
        pdir = os.path.join(DS1, "plugins", plugin, "generated")
        files[os.path.join(pdir, base + "_iface.h")] = header(iface, funcs, layout, G)
        ex = banner + "// The plugin: C thunks over its own functions + the table it publishes.\nnamespace {\n"
        ex += thunks(funcs, lambda f: f.qual)
        ex += "const %s kTable = {sizeof(%s), 1, %s_LAYOUT,\n%s};\n}  // namespace\n" % (
            S, S, G, ",\n".join("    &t_%s" % f.member for f in funcs))
        ex += "bool Register%sInterface() {\n" % "".join(p.capitalize() for p in base.split("_"))
        ex += "    return kit::Messaging() && kit::Messaging()->RegisterInterface(kit::Self(), %s_INTERFACE, 1, &kTable) == PSE_OK;\n}\n" % G
        files[os.path.join(pdir, base + "_export.inc")] = ex
        files[os.path.join(pdir, "own_functions.h")] = banner + "#pragma once\n" + "".join(
            "#define DSFIX_OWN_%s\n" % f.member for f in funcs)
        wr = banner + "// dsfix.dll: features::X calls the plugin %s, or answers the default without it.\n" % plugin
        wr += GETTER_PLUGIN % {"S": S, "G": G}
        for f in funcs:
            if f.default is None and f.ret.cpp != "void":
                raise SystemExit("%s: %s needs a default ('= value') for dsfix.dll without the plugin" % (path, f.member))
            wr += definition(f, client_body(f, "Table", f.default))
        files[os.path.join(GEN, base + "_wrappers.inc")] = wr
    return files


def main():
    check = "--check" in sys.argv
    specs = [os.path.join(SRC, "core", f) for f in sorted(os.listdir(os.path.join(SRC, "core"))) if f.endswith(".spec")]
    for p in sorted(os.listdir(os.path.join(DS1, "plugins"))):
        d = os.path.join(DS1, "plugins", p)
        if os.path.isdir(d):
            specs += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(".spec")]
    stale = []
    for spec in specs:
        for out, text in generate(spec).items():
            old = open(out, encoding="utf-8").read() if os.path.exists(out) else None
            if old != text:
                stale.append(out)
                if not check:
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    open(out, "w", encoding="utf-8", newline="\n").write(text)
    for s in stale:
        print(("stale: " if check else "wrote: ") + os.path.relpath(s, DS1))
    if check and stale:
        sys.exit(1)
    print("%d spec(s), %s" % (len(specs), "up to date" if not stale else ("%d file(s) %s" % (len(stale), "stale" if check else "written"))))


if __name__ == "__main__":
    main()

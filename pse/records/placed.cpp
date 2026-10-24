// Placed references (placed.h). Mirrors the ModKit rule for rule: conflict_ds_tools/placed.py apply_edits, env.py's
// tokenizer / record layouts / token formatting (Desert Storm's HLOI / NEWENVWAYPT / REGN) and
// mission2lua_readable.refs_from_env (editor IDs). Python's error texts included - the gate compares the report.
#include "placed.h"
#include "scenepatch.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>

namespace pse::records {
namespace {

using pse::data::Key;
using pse::data::Kind;
using pse::data::Value;

constexpr const char* kPrefix = "ds1:";

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}
std::string Upper(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
    return s;
}

// UTF-8 (PSE data strings) -> latin-1 (the scene); a code point beyond latin-1 (or a bad sequence) -> "?"
std::string Utf8ToLatin1(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) {
            o.push_back('?');
            ++i;
            continue;
        }
        uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
        bool ok = true;
        for (size_t k = 1; k < n; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) ok = false;
            cp = (cp << 6) | (d & 0x3F);
        }
        o.push_back(ok && cp < 0x100 ? static_cast<char>(cp) : '?');
        i += ok ? n : 1;
    }
    return o;
}

// Python's str.isspace for one latin-1 character (the regex \s of env.py's tokenizer)
bool IsSpace(unsigned char c) { return (c >= 9 && c <= 13) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsInt(const std::string& t) {   // -?\d+
    size_t i = !t.empty() && t[0] == '-';
    if (i >= t.size()) return false;
    for (; i < t.size(); ++i)
        if (!IsDigit(t[i])) return false;
    return true;
}
bool IsNumber(const std::string& t) {   // -?\d+(\.\d*)?
    size_t i = !t.empty() && t[0] == '-', digits = 0;
    while (i < t.size() && IsDigit(t[i])) ++i, ++digits;
    if (!digits) return false;
    if (i < t.size() && t[i] == '.')
        for (++i; i < t.size(); ++i)
            if (!IsDigit(t[i])) return false;
    return i == t.size();
}
bool IsKeyword(const std::string& t) {   // [A-Z<][A-Z0-9_/>]* | Version
    if (t == "Version") return true;
    if (t.empty() || !((t[0] >= 'A' && t[0] <= 'Z') || t[0] == '<')) return false;
    for (char c : t)
        if (!((c >= 'A' && c <= 'Z') || IsDigit(c) || c == '_' || c == '/' || c == '>' || c == '<')) return false;
    return t.find('<', 1) == std::string::npos;
}

// Python texts (the report lines are compared with the ModKit's)
std::string ReprStr(const std::string& s) {   // repr() of a str
    const char q = s.find('\'') != std::string::npos && s.find('"') == std::string::npos ? '"' : '\'';
    std::string o(1, q);
    for (unsigned char c : s) {
        if (c == '\\') o += "\\\\";
        else if (c == q) o += std::string("\\") + static_cast<char>(q);
        else if (c == '\n') o += "\\n";
        else if (c == '\r') o += "\\r";
        else if (c == '\t') o += "\\t";
        else if (c < 0x20 || c == 0x7F) {
            char b[8];
            std::snprintf(b, sizeof b, "\\x%02x", c);
            o += b;
        } else o.push_back(static_cast<char>(c));
    }
    return o + q;
}
std::string PyStr(const Value& v) {   // str() of a pse_data value
    switch (v.kind) {
    case Kind::Bool: return v.b ? "True" : "False";
    case Kind::Int: return std::to_string(v.i);
    case Kind::Float: return pse::data::FormatFloat(v.f);
    case Kind::String: return v.s;
    default: return "a table";
    }
}
std::string PyRepr(const Value& v) { return v.kind == Kind::String ? ReprStr(v.s) : PyStr(v); }
std::string Show(const Value& v) {    // placed.py _show
    if (v.kind == Kind::Bool) return v.b ? "true" : "false";
    return PyStr(v);
}
bool Truthy(const Value& v) {
    switch (v.kind) {
    case Kind::Bool: return v.b;
    case Kind::Int: return v.i != 0;
    case Kind::Float: return v.f != 0;
    case Kind::String: return !v.s.empty();
    default: return true;   // a Table has no length in pse_data.py: always true
    }
}

// a field key as Python sees it: None (a list item), an int or a str
struct FieldKey {
    Key::Type type = Key::Type::None;
    std::string s;
    int64_t i = 0;
    std::string Str() const { return type == Key::Type::Name ? s : type == Key::Type::Int ? std::to_string(i) : "None"; }
    std::string Repr() const { return type == Key::Type::Name ? ReprStr(s) : Str(); }
};

// Python's KeyError / ValueError: str(KeyError(m)) = repr(m)
struct PyError {
    bool key;
    std::string msg;
    std::string Text() const { return key ? ReprStr(msg) : msg; }
};
struct LayoutStop {};

// ---- the scene (env.py EnvFile, the parts placed.py uses) ----
struct Tok {
    size_t start, end;
};
struct Rec {
    size_t line;
    std::string kind;
    std::vector<Tok> toks;
    std::map<std::string, size_t> fields;   // name -> token index (vectors: name.x / .y / .z)
};

std::vector<Tok> Tokenize(const std::string& line) {   // "[^"\r\n]*"|[^\s"]+
    std::vector<Tok> out;
    const size_t n = line.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(line[i]);
        if (c == '"') {
            size_t j = i + 1;
            while (j < n && line[j] != '"' && line[j] != '\r' && line[j] != '\n') ++j;
            if (j < n && line[j] == '"') {
                out.push_back({i, j + 1});
                i = j + 1;
            } else {
                ++i;   // an unclosed quote is no token
            }
        } else if (IsSpace(c)) {
            ++i;
        } else {
            size_t j = i;
            while (j < n && !IsSpace(static_cast<unsigned char>(line[j])) && line[j] != '"') ++j;
            out.push_back({i, j});
            i = j;
        }
    }
    return out;
}

class Scene {
public:
    std::vector<std::string> lines;
    std::string eol;
    int version = 0;
    std::vector<Rec> recs;

    explicit Scene(const std::string& data) {
        std::string text = data;
        const bool crlf = text.find("\r\n") != std::string::npos;
        if (crlf) {   // mixed line ends: one style (EnvFile.read)
            std::string t;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) t += '\r';
                t += text[i];
            }
            text.swap(t);
        }
        eol = crlf ? "\r\n" : "\n";
        for (size_t a = 0;;) {
            const size_t b = text.find(eol, a);
            lines.push_back(text.substr(a, b == std::string::npos ? std::string::npos : b - a));
            if (b == std::string::npos) break;
            a = b + eol.size();
        }
        Parse();
    }

    std::string Bytes() const {
        std::string o;
        for (size_t i = 0; i < lines.size(); ++i) o += (i ? eol : std::string()) + lines[i];
        return o;
    }

    std::string Text(const Rec& r, size_t i) const { return lines[r.line].substr(r.toks[i].start, r.toks[i].end - r.toks[i].start); }
    bool Has(const Rec& r, const std::string& f) const { return r.fields.count(f) != 0; }
    // the token's value as env.py's Token.value: a quoted string, else an int (or None for anything else here)
    bool Str(const Rec& r, const std::string& f, std::string* out) const {
        auto it = r.fields.find(f);
        if (it == r.fields.end()) return false;
        const std::string t = Text(r, it->second);
        if (t.empty() || t[0] != '"') return false;
        *out = t.substr(1, t.size() - 2);
        return true;
    }
    bool Int(const Rec& r, const std::string& f, int64_t* out) const {
        auto it = r.fields.find(f);
        if (it == r.fields.end()) return false;
        const std::string t = Text(r, it->second);
        if (!IsInt(t)) return false;
        *out = std::strtoll(t.c_str(), nullptr, 10);
        return true;
    }

    void Parse() {
        recs.clear();
        for (size_t i = 0; i < lines.size(); ++i) {
            Rec r{i, "", Tokenize(lines[i]), {}};
            if (r.toks.empty()) continue;
            const std::string first = Text(r, 0);
            r.kind = IsKeyword(first) ? first : "";
            if (r.kind == "HLOU") r.kind = "HLOI";   // DS2 squad units: an HLOI with one more name
            if (r.kind == "Version" && r.toks.size() > 1) {
                const std::string v = Text(r, 1);
                if (IsInt(v)) version = std::atoi(v.c_str());
            }
            Layout(r);
            recs.push_back(std::move(r));
        }
    }

    void Replace(Rec& r, size_t i, const std::string& text) {
        std::string& line = lines[r.line];
        line = line.substr(0, r.toks[i].start) + text + line.substr(r.toks[i].end);
        r.toks = Tokenize(line);
        Layout(r);
    }

    // the line index of the first record of `kind` after `after`
    bool FindAfter(const std::string& kind, size_t after, size_t* line) const {
        for (const Rec& r : recs)
            if (r.kind == kind && r.line > after) return *line = r.line, true;
        return false;
    }

private:
    struct Cursor {
        Scene& s;
        Rec& r;
        int v;
        size_t i = 1;   // token 0 = keyword
        std::string Take(const std::string& name, char type) {
            if (i >= r.toks.size()) throw LayoutStop{};
            const std::string t = s.Text(r, i);
            const bool quoted = !t.empty() && t[0] == '"';
            const bool ok = type == 'd' ? !quoted && IsInt(t) : type == 'f' ? !quoted && IsNumber(t) :
                            type == 'q' ? quoted : type == 's' ? !quoted : true;
            if (!ok) throw LayoutStop{};
            if (!name.empty()) r.fields[name] = i;
            ++i;
            return t;
        }
        int64_t d(const std::string& name) { return std::strtoll(Take(name, 'd').c_str(), nullptr, 10); }
        void f(const std::string& name) { Take(name, 'f'); }
        void q(const std::string& name) { Take(name, 'q'); }
        void v3(const std::string& name) {
            for (const char* c : {".x", ".y", ".z"}) f(name + c);
        }
    };

    static std::string N(const char* prefix, int64_t k, const char* suffix = "") {
        return prefix + std::to_string(k) + suffix;
    }

    // env.py _hloi, the Conflict branch (scene versions below 190)
    void Hloi(Cursor& c) {
        const int v = c.v;
        if (v > 9) c.d("flags");
        if (v >= 95) c.d("u_z");
        if (Text(c.r, 0) == "HLOU") c.q("profile");
        c.q("model"); c.q("zone");
        if (v > 48) c.q("texture");
        c.v3("pos"); c.v3("rot");
        c.d("u_a"); c.d("u_b"); c.d("u_c");
        c.q("unit"); c.d("u_d"); c.d("script_id"); c.q("stat"); c.d("u_e"); c.d("in_play");
        if (v > 8) { c.d("u_g"); c.d("u_h"); c.d("u_i"); }
        if (v > 26) c.d("u_j");
        if (v >= 69) c.d("u_k");
        c.d("u_l"); c.d("u_m");
        if (v > 18) {
            for (int k = 0; k < 5; ++k) c.f(N("ai_f", k));
            c.d("ai_d0");
            if (v > 24) { c.f("ai_f5"); c.d("ai_d1"); }
            if (v > 35) c.d("ai_d2");
            if (v > 38) c.d("ai_d3");
            if (v > 41) { c.d("ai_d4"); c.d("ai_d5"); }
            if (v >= 94) c.q("ai_instance");
            if (v >= 40) c.q("ai_name");
            const int64_t bits = c.d("ai_bits");
            for (int b = 0; b < 6; ++b)
                if (bits & (int64_t{1} << b)) c.f(N("ai_bit", b));
            if (bits & 0x40) {
                c.d("ai_bit6");
                if (v > 25) c.f("ai_bit6f");
            }
            if (bits & 0x100) {
                const int64_t t = c.d("ai_shape");
                if (v > 21) {
                    const int n = t == 0 ? 1 : t == 1 ? 4 : t == 2 ? 8 : 0;
                    for (int k = 0; k < n; ++k) c.f(N("ai_shape", k));
                } else if (t == 2) {
                    for (int k = 0; k < 4; ++k) c.f(N("ai_shape", k));
                    c.d("ai_shape4");
                }
                if (20 < v && v < 36) c.d("ai_shape_d");
            }
            const int64_t n = c.d("ai_list_count");
            for (int64_t k = 0; k < n; ++k) c.d(N("ai_list", k));
        }
        if (v > 19) { c.f("u_n"); c.f("u_o"); }
        const int64_t kits = c.d("kit_count");
        for (int64_t k = 0; k < kits; ++k) {
            c.q(N("kit", k, ".item")); c.d(N("kit", k, ".category")); c.d(N("kit", k, ".amount"));
        }
        if (v >= 45) c.d("u_p");
        if (v >= 50) { c.f("u_q"); c.f("u_r"); }
        if (v >= 23) {
            c.v3("v1"); c.v3("v2"); c.d("u_s");
            if (v < 63) { c.v3("v3"); c.v3("v4"); c.d("u_t"); }
        }
        if (v >= 30) {
            c.d("u_u");
            const int64_t n = c.d("group_count");
            for (int64_t k = 0; k < n; ++k) c.d(N("group", k));
        }
        if (v >= 86) c.d("u_v");
        const int64_t links = c.d("link_count");
        for (int64_t k = 0; k < links; ++k) {
            c.q(N("link", k, ".a")); c.d(N("link", k, ".n")); c.q(N("link", k, ".b"));
        }
    }
    void Waypoint(Cursor& c) {
        c.d("id");
        if (c.v > 10) c.d("u1");
        c.q("name"); c.v3("pos");
        if (c.v > 14) c.v3("rot");
        c.d("wait_ms");
        if (c.v >= 107) c.f("u6");
        c.d("next");
        if (c.v >= 79) { c.d("u2"); c.d("u3"); }
        if (c.v >= 82) c.d("u4");
        if (c.v >= 88) c.d("u5");
    }
    void Region(Cursor& c) {
        c.q("name");
        const int64_t t = c.d("type");
        c.v3("pos");
        if (t == 1) {
            c.f("radius");
        } else {
            c.v3("rot"); c.v3("min"); c.v3("max");
        }
    }
    // env.py _layout: the fields read before a layout problem stay (the problem only goes to EnvFile.problems)
    void Layout(Rec& r) {
        r.fields.clear();
        if (version >= 190) return;   // only Desert Storm's layouts here
        Cursor c{*this, r, version};
        try {
            if (r.kind == "HLOI") Hloi(c);
            else if (r.kind == "NEWENVWAYPT") Waypoint(c);
            else if (r.kind == "REGN") Region(c);
        } catch (const LayoutStop&) {
        }
    }
};

// ---- editor IDs (mission2lua_readable.refs_from_env) ----
const std::set<std::string>& LuaKeywords() {
    static const std::set<std::string> k = {"and", "break", "do", "else", "elseif", "end", "false", "for", "function",
        "goto", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while"};
    return k;
}
std::string Ident(const std::string& text, std::set<std::string>* used) {   // _ref_ident
    std::string t;
    for (char ch : text) {
        const bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || IsDigit(ch) || ch == '_';
        t.push_back(ok ? ch : '_');
    }
    const size_t a = t.find_first_not_of('_');
    t = a == std::string::npos ? "" : t.substr(a, t.find_last_not_of('_') - a + 1);
    std::string c;
    for (char ch : t)
        if (!(ch == '_' && !c.empty() && c.back() == '_')) c.push_back(ch);
    if (c.empty()) c = "ref";
    if (IsDigit(c[0])) c = "_" + c;
    std::string out = c;
    for (int k = 2; used->count(out) || LuaKeywords().count(out); ++k) out = c + "_" + std::to_string(k);
    used->insert(out);
    return out;
}

struct Names {
    std::map<int64_t, std::string> objects, waypoints;
    std::vector<std::string> regions;
};

Names RefNames(const Scene& s) {
    Names out;
    std::vector<const Rec*> hloi;
    for (const Rec& r : s.recs)
        if (r.kind == "HLOI") hloi.push_back(&r);
    std::stable_sort(hloi.begin(), hloi.end(), [&](const Rec* a, const Rec* b) {
        int64_t x = 0, y = 0;
        s.Int(*a, "script_id", &x);
        s.Int(*b, "script_id", &y);
        return x < y;
    });
    std::set<std::string> used;
    for (const Rec* r : hloi) {
        int64_t sid = 0;
        if (!s.Int(*r, "script_id", &sid) || sid < 0 || out.objects.count(sid)) continue;
        std::string stat, unit;
        s.Str(*r, "stat", &stat);
        s.Str(*r, "unit", &unit);
        bool letter = false;
        for (char ch : stat) letter |= (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
        std::string name;
        if (!stat.empty() && Upper(stat) != "NONE" && letter) {
            name = stat;
        } else {
            std::string u = unit;   // ^(IR|US|UK|NU|RU)_(VEH|PROP|STRCT|DOOR|WPN)_ dropped
            for (const char* side : {"IR_", "US_", "UK_", "NU_", "RU_"})
                if (u.compare(0, 3, side) == 0)
                    for (const char* what : {"VEH_", "PROP_", "STRCT_", "DOOR_", "WPN_"})
                        if (u.compare(3, std::strlen(what), what) == 0) {
                            u = u.substr(3 + std::strlen(what));
                            break;
                        }
            name = u + "_" + std::to_string(sid);
        }
        out.objects[sid] = Ident(name, &used);
    }
    std::set<std::string> usedW;
    for (const Rec& r : s.recs) {
        int64_t id = 0;
        if (r.kind != "NEWENVWAYPT" || !s.Int(r, "id", &id) || out.waypoints.count(id)) continue;
        std::string name;
        s.Str(r, "name", &name);
        out.waypoints[id] = Ident(name.empty() ? "WP_" + std::to_string(id) : name, &usedW);
    }
    std::set<std::string> usedR;
    for (const Rec& r : s.recs) {
        if (r.kind != "REGN") continue;
        std::string name;
        s.Str(r, "name", &name);
        out.regions.push_back(Ident(name.empty() ? "REGION_" + std::to_string(out.regions.size()) : name, &usedR));
    }
    return out;
}

// placed.py lookup(): {kind: {editor id: record}}, HLOI by script id (the last line of an id wins), waypoint numbers
struct Lookup {
    std::map<std::string, std::map<std::string, size_t>> by;   // kind -> lower editor id -> index into Scene::recs
    std::map<int64_t, size_t> hloi;
    std::map<std::string, int64_t> numbers;
};
Lookup Look(const Scene& s) {
    Lookup l;
    l.by["object"];
    l.by["waypoint"];
    l.by["region"];
    const Names names = RefNames(s);
    std::map<int64_t, size_t> wps;
    std::vector<size_t> regions;
    for (size_t k = 0; k < s.recs.size(); ++k) {
        int64_t n = 0;
        if (s.recs[k].kind == "HLOI" && s.Int(s.recs[k], "script_id", &n)) l.hloi[n] = k;
        if (s.recs[k].kind == "NEWENVWAYPT" && s.Int(s.recs[k], "id", &n)) wps[n] = k;
        if (s.recs[k].kind == "REGN") regions.push_back(k);
    }
    for (const auto& [sid, editor] : names.objects)
        if (auto it = l.hloi.find(sid); it != l.hloi.end()) l.by["object"][Lower(editor)] = it->second;
    for (const auto& [n, editor] : names.waypoints)
        if (auto it = wps.find(n); it != wps.end()) {
            l.by["waypoint"][Lower(editor)] = it->second;
            l.numbers[Lower(editor)] = n;
        }
    for (size_t k = 0; k < regions.size(); ++k) l.by["region"][Lower(names.regions[k])] = regions[k];
    return l;
}

// env.py _format_like: the value written the way the old token was
std::string FormatLike(const std::string& old, const Value& v) {
    if (!old.empty() && old[0] == '"') {
        const std::string s = PyStr(v);
        if (s.find('"') != std::string::npos || s.find('\n') != std::string::npos)
            throw PyError{false, "string can't contain quotes or newlines: " + ReprStr(s)};
        return "\"" + Utf8ToLatin1(s) + "\"";
    }
    if (!IsNumber(old)) {
        const std::string s = Utf8ToLatin1(PyStr(v));
        bool bad = s.empty();
        for (unsigned char c : s) bad |= IsSpace(c) || c == '"';
        if (bad) throw PyError{false, "a bare name can't be empty or contain spaces / quotes: " + ReprStr(PyStr(v))};
        return s;
    }
    if (v.kind == Kind::String || v.kind == Kind::Table)
        throw PyError{false, "expected a number for token " + ReprStr(old) + ", got " + PyRepr(v)};
    const double f = v.kind == Kind::Float ? v.f : v.kind == Kind::Int ? static_cast<double>(v.i) : v.b ? 1.0 : 0.0;
    if (const size_t dot = old.find('.'); dot != std::string::npos) {
        char b[512];
        std::snprintf(b, sizeof b, "%.*f", static_cast<int>(old.size() - dot - 1), f);
        return b;
    }
    if (v.kind == Kind::Float) {
        if (!std::isfinite(v.f) || std::floor(v.f) != v.f)
            throw PyError{false, "token " + ReprStr(old) + " is an integer, got " + PyRepr(v)};
        if (v.f == 0) return "0";
        char b[512];
        std::snprintf(b, sizeof b, "%.0f", v.f);
        return b;
    }
    return v.kind == Kind::Int ? std::to_string(v.i) : v.b ? "1" : "0";
}

void SetScalar(Scene& s, Rec& r, const FieldKey& f, const Value& v) {
    auto it = f.type == Key::Type::Name ? r.fields.find(f.s) : r.fields.end();
    if (it == r.fields.end()) throw PyError{true, r.kind + " has no field " + f.Repr()};
    s.Replace(r, it->second, FormatLike(s.Text(r, it->second), v));
}
void SetVector(Scene& s, Rec& r, const std::string& name, const Value& t) {   // { x, y, z } (missing = 0.0)
    for (const char* c : {"x", "y", "z"}) {
        const Value* comp = t.Get(c);
        FieldKey k{Key::Type::Name, name + "." + c};
        SetScalar(s, r, k, comp ? *comp : Value::Float(0.0));
    }
}
// placed.py _set
void Set(Scene& s, Rec& r, const FieldKey& f, const Value& v) {
    const bool named = f.type == Key::Type::Name;
    const bool vec = named && r.fields.count(f.s + ".x") && !r.fields.count(f.s);
    if (v.kind == Kind::Table) {
        if (!vec) {
            if (named && r.fields.count(f.s)) throw PyError{false, f.s + " takes a value, not a table"};
            throw PyError{true, r.kind + " has no field " + f.Repr()};
        }
        SetVector(s, r, f.s, v);
    } else if (vec) {
        throw PyError{false, f.s + " takes { x = ..., y = ..., z = ... }"};
    } else {
        SetScalar(s, r, f, v);
    }
}

const std::string* StrOf(const Value* v) { return v && v->kind == Kind::String ? &v->s : nullptr; }

}  // namespace

bool PlacedIdParts(const std::string& id, std::string* level, std::string* kind, std::string* name) {
    if (id.compare(0, 4, kPrefix) != 0) return false;
    const std::string rest = id.substr(4);
    const size_t a = rest.find('/');
    const size_t b = a == std::string::npos ? a : rest.find('/', a + 1);
    if (b == std::string::npos || rest.find('/', b + 1) != std::string::npos) return false;
    *level = rest.substr(0, a);
    *kind = rest.substr(a + 1, b - a - 1);
    *name = rest.substr(b + 1);
    return true;
}

std::vector<int64_t> SceneScriptIds(const std::string& sceneBytes) {
    const Scene scene(sceneBytes);
    std::vector<int64_t> ids;
    for (const Rec& r : scene.recs) {
        int64_t id = 0;
        if (r.kind == "HLOI" && scene.Int(r, "script_id", &id)) ids.push_back(id);
    }
    return ids;
}

std::vector<std::string> PlacedLevels(const std::vector<Edits>& edits) {
    std::set<std::string> levels;
    for (const Edits& ed : edits)
        for (const pse::data::Entry& item : ed.doc.root.entries) {
            if (item.key.type != Key::Type::None || item.value->kind != Kind::Table) continue;
            std::string level, kind, name;
            if (const std::string* id = StrOf(item.value->Get("id")); id && PlacedIdParts(*id, &level, &kind, &name))
                levels.insert(Lower(level));   // the level part ignores case: ds1:Mission1/... = ds1:mission1/...
        }
    return {levels.begin(), levels.end()};
}

std::string ApplyPlaced(const std::string& sceneBytes, const std::string& level, const std::vector<Edits>& edits,
                        const Forms& forms, std::vector<std::string>* report) {
    std::map<std::string, Value> keys;   // record id -> its key (the name the game's tables use)
    for (const auto& [type, doc] : forms)
        if (const Value* recs = doc.root.Get("records"))
            for (const pse::data::Entry& e : recs->entries) {
                if (e.key.type != Key::Type::None || e.value->kind != Kind::Table) continue;
                const std::string* id = StrOf(e.value->Get("id"));
                const Value* key = e.value->Get("key");
                if (id && !id->empty() && key) keys[*id] = key->Clone();
            }
    const std::string lvl = Lower(level);
    Scene scene(sceneBytes);
    auto fail = [&](const Edits& ed, const std::string& msg) { report->push_back("[fail] " + ed.mod + "/" + ed.file + ": " + msg); };

    struct Todo {
        const Edits* ed;
        const Value* e;
        std::string id, kind, editor;
    };
    std::vector<Todo> todo;
    for (const Edits& ed : edits)
        for (const pse::data::Entry& item : ed.doc.root.entries) {
            if (item.key.type != Key::Type::None || item.value->kind != Kind::Table) continue;
            const std::string* id = StrOf(item.value->Get("id"));
            std::string l, kind, name;
            if (id && PlacedIdParts(*id, &l, &kind, &name) && Lower(l) == lvl) todo.push_back({&ed, item.value.get(), *id, kind, name});
        }

    // pass 1: new objects (copies go to the end of the HLOI section - lines are added)
    Lookup look = Look(scene);
    for (const Todo& t : todo) {
        auto kindIt = look.by.find(t.kind);
        if (kindIt == look.by.end() || kindIt->second.count(t.editor)) continue;
        const std::string* basedOn = StrOf(t.e->Get("based_on"));
        std::string bl, bk, bn;
        size_t src = SIZE_MAX;
        if (basedOn && PlacedIdParts(*basedOn, &bl, &bk, &bn) && Lower(bl) == lvl && bk == t.kind)
            if (auto it = kindIt->second.find(bn); it != kindIt->second.end()) src = it->second;
        if (t.kind != "object" || src == SIZE_MAX) continue;   // reported in pass 2
        int64_t sid = -1;
        for (const auto& [s, rec] : look.hloi) sid = std::max(sid, s);
        ++sid;
        if (const Value* want = t.e->Get("script_id"); want && want->kind == Kind::Int && want->i >= 0) {
            // a copy may ask for its script id (mission logic refers to objects by it)
            if (look.hloi.count(want->i))
                fail(*t.ed, t.id + " script id " + std::to_string(want->i) + " is taken - it gets " +
                                std::to_string(sid));
            else
                sid = want->i;
        }
        const Rec& from = scene.recs[src];
        size_t at = from.line + 1;
        scene.FindAfter(scene.Text(from, 0) == "HLOU" ? "HLOUX" : "HLOX", from.line, &at);
        const std::string copy = scene.lines[from.line];
        scene.lines.insert(scene.lines.begin() + static_cast<std::ptrdiff_t>(at), copy);
        scene.Parse();
        for (Rec& r : scene.recs)
            if (r.line == at) {
                try {
                    SetScalar(scene, r, {Key::Type::Name, "script_id"}, Value::Int(sid));
                    SetScalar(scene, r, {Key::Type::Name, "stat"}, Value::String(Upper(t.editor)));
                } catch (const PyError& err) {
                    fail(*t.ed, t.id + " can't be copied: " + err.Text());
                }
                break;
            }
        report->push_back("new " + t.id + " based on " + *basedOn + ", script id " + std::to_string(sid) + " (" + t.ed->mod + ")");
        look = Look(scene);
    }

    // pass 2: fields (no lines added or removed). Two mods giving one field different values = a conflict line (the
    // later mod wins), as forms.h Merge: noted before the value is set; a scene = { ... } part is "scene.<field>", a
    // delete is an in_play value.
    std::map<std::string, std::pair<std::string, std::string>> setby;   // kind/editor/field -> (mod, value text)
    auto note = [&](const Todo& t, const std::string& field, const Value* value) {
        std::string text = "delete";
        if (value) {
            pse::data::Document d;
            d.root.Set("v", value->Clone());
            std::string e;
            text.clear();
            pse::data::Write(d, &text, &e);
        }
        const std::string key = t.kind + "/" + t.editor + "/" + field;
        auto prev = setby.find(key);
        if (prev != setby.end() && prev->second.first != t.ed->mod && prev->second.second != text)
            report->push_back("conflict: " + t.id + " " + field + ": " + prev->second.first + " / " + t.ed->mod + " (" +
                              t.ed->mod + " wins)");
        setby[key] = {t.ed->mod, text};
    };
    static const std::set<std::string> kSkip = {"id", "based_on", "delete", "editor_id", "script_id", "number", "index",
                                                "shape"};
    for (const Todo& t : todo) {
        auto kindIt = look.by.find(t.kind);
        if (kindIt == look.by.end()) {
            fail(*t.ed, t.id + " - no placed kind " + t.kind);
            continue;
        }
        auto recIt = kindIt->second.find(t.editor);
        if (recIt == kindIt->second.end()) {
            fail(*t.ed, t.id + " isn't in " + lvl + "'s scene - a new one needs based_on = <an object of this level>");
            continue;
        }
        Rec& rec = scene.recs[recIt->second];
        if (const Value* del = t.e->Get("delete"); del && del->kind == Kind::Bool && del->b) {
            if (t.kind == "object") {
                note(t, "in_play", nullptr);
                try {
                    SetScalar(scene, rec, {Key::Type::Name, "in_play"}, Value::Int(0));
                } catch (const PyError&) {
                }
                report->push_back(t.id + " taken out of play (" + t.ed->mod + ")");
            } else {
                fail(*t.ed, t.id + " - only objects can be deleted (set it elsewhere instead)");
            }
            continue;
        }
        for (const pse::data::Entry& ent : t.e->entries) {
            const FieldKey f{ent.key.type, ent.key.name, ent.key.i};
            if (f.type == Key::Type::Name && kSkip.count(f.s)) continue;
            const Value& v = *ent.value;
            const bool object = t.kind == "object";
            if (f.type == Key::Type::Name && f.s == "scene" && object && v.kind == Kind::Table) {
                for (const pse::data::Entry& sub : v.entries)
                    if (sub.key.type == Key::Type::Name) note(t, "scene." + sub.key.name, sub.value.get());
            } else if (f.type == Key::Type::Name) {
                note(t, f.s, &v);
            }
            try {
                if (f.type == Key::Type::Name && f.s == "in_play" && object) {
                    SetScalar(scene, rec, f, Value::Int(Truthy(v) ? 1 : 0));
                } else if (f.type == Key::Type::Name && f.s == "base" && object) {
                    auto key = v.kind == Kind::String ? keys.find(v.s) : keys.end();
                    if (key == keys.end()) {
                        fail(*t.ed, t.id + " base " + Show(v) + " isn't a record");
                        continue;
                    }
                    SetScalar(scene, rec, {Key::Type::Name, "unit"}, key->second);
                } else if (f.type == Key::Type::Name && f.s == "scene" && object && v.kind == Kind::Table) {
                    for (const pse::data::Entry& sub : v.entries)
                        Set(scene, rec, FieldKey{sub.key.type, sub.key.name, sub.key.i}, *sub.value);
                } else if (f.type == Key::Type::Name && f.s == "next" && t.kind == "waypoint") {
                    if (v.kind == Kind::Int || v.kind == Kind::Bool) {
                        SetScalar(scene, rec, f, v);
                    } else {
                        std::string pl, pk, pn;
                        const bool parts = v.kind == Kind::String && PlacedIdParts(v.s, &pl, &pk, &pn);
                        auto n = parts ? look.numbers.find(pn) : look.numbers.end();
                        if (n == look.numbers.end()) {
                            fail(*t.ed, t.id + " next " + Show(v) + " isn't a waypoint of " + lvl);
                            continue;
                        }
                        SetScalar(scene, rec, f, Value::Int(n->second));
                    }
                } else {
                    Set(scene, rec, f, v);
                }
            } catch (const PyError& err) {
                fail(*t.ed, t.id + " " + f.Str() + " can't be set: " + err.Text());
            }
        }
    }
    return scene.Bytes();
}

}  // namespace pse::records

// Base forms: generate / tables / merge (forms.h). Mirrors conflict_ds_tools/forms.py rule for rule.
#include "forms.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <set>

namespace pse::records {
namespace {

using pse::data::Document;
using pse::data::Entry;
using pse::data::Key;
using pse::data::Kind;
using pse::data::Value;

constexpr const char* kPrefix = "ds1";

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

// latin-1 bytes (the tables) <-> UTF-8 (PSE data strings)
std::string Latin1ToUtf8(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c < 0x80) {
            o.push_back(static_cast<char>(c));
        } else {
            o.push_back(static_cast<char>(0xC0 | (c >> 6)));
            o.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return o;
}
std::string Utf8ToLatin1(const std::string& s) {   // a code point beyond latin-1 (or a bad sequence) -> "?"
    std::string o;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
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
        if (!ok) {
            o.push_back('?');
            ++i;
            continue;
        }
        o.push_back(cp <= 0xFF ? static_cast<char>(cp) : '?');
        i += n;
    }
    return o;
}

const std::string* Str(const Value* v) { return v && v->kind == Kind::String ? &v->s : nullptr; }
std::string StrOr(const Value* v, const std::string& d = "") { return v && v->kind == Kind::String ? v->s : d; }
bool Truthy(const Value* v) {   // Python truthiness of a field value (what forms.py's `if x.get(...)` tests)
    if (!v) return false;
    switch (v->kind) {
        case Kind::Bool: return v->b;
        case Kind::Int: return v->i != 0;
        case Kind::Float: return v->f != 0;
        case Kind::String: return !v->s.empty();
        case Kind::Table: return !v->entries.empty();
    }
    return false;
}
std::vector<const Value*> Items(const Value* t) {
    std::vector<const Value*> out;
    if (t && t->kind == Kind::Table)
        for (const Entry& e : t->entries)
            if (e.key.type == Key::Type::None) out.push_back(e.value.get());
    return out;
}
std::vector<Value*> ItemsMut(Value* t) {
    std::vector<Value*> out;
    if (t && t->kind == Kind::Table)
        for (Entry& e : t->entries)
            if (e.key.type == Key::Type::None) out.push_back(e.value.get());
    return out;
}

std::vector<std::string> Split(const std::string& s, const std::string& sep) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
        const size_t b = s.find(sep, a);
        out.push_back(s.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) break;
        a = b + sep.size();
    }
    return out;
}

struct Split3 {
    std::vector<std::string> lines;
    std::string eol;
    bool final = false;
};
Split3 SplitLines(const std::string& text) {
    Split3 r;
    r.eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    r.final = text.size() >= r.eol.size() && text.compare(text.size() - r.eol.size(), r.eol.size(), r.eol) == 0;
    const std::string body = r.final ? text.substr(0, text.size() - r.eol.size()) : text;
    if (!body.empty()) r.lines = Split(body, r.eol);
    return r;
}

bool Digits(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i)
        if (s[i] < '0' || s[i] > '9') return false;
    return true;
}
bool IsInt(const std::string& c) { return Digits(c, !c.empty() && c[0] == '-' ? 1 : 0); }
bool IsFixed(const std::string& c) {
    const size_t dot = c.find('.');
    if (dot == std::string::npos) return false;
    return IsInt(c.substr(0, dot)) && Digits(c, dot + 1) && c.find('.', dot + 1) == std::string::npos;
}

std::string Fixed2(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

// a field's value as cell text (forms.py _fmt)
std::string Fmt(const Value& v, const std::string& kind) {
    const bool num = v.kind == Kind::Int || v.kind == Kind::Float;
    if (kind == "int" && v.kind == Kind::Int) return std::to_string(v.i);
    if (kind == "fixed2" && num) return Fixed2(v.kind == Kind::Int ? static_cast<double>(v.i) : v.f);
    switch (v.kind) {
        case Kind::Bool: return v.b ? "YES" : "NO";
        case Kind::Int: return std::to_string(v.i);
        case Kind::Float: return pse::data::FormatFloat(v.f);
        case Kind::String: return v.s;
        default: return "";
    }
}

// the typed value of a cell, or the cell itself when the kind wouldn't write it back the same (forms.py _parse)
Value Parse(const std::string& cell, const std::string& kind) {
    if (kind == "int") {
        if (!IsInt(cell)) return Value::String(cell);
        errno = 0;
        char* end = nullptr;
        const long long v = std::strtoll(cell.c_str(), &end, 10);
        if (errno == ERANGE) return Value::String(cell);
        return std::to_string(v) == cell ? Value::Int(v) : Value::String(cell);
    }
    if (kind == "fixed2") {
        if (!IsFixed(cell)) return Value::String(cell);
        const double v = std::strtod(cell.c_str(), nullptr);
        return Fixed2(v) == cell ? Value::Float(v) : Value::String(cell);
    }
    if (kind == "yesno") {
        if (cell == "YES") return Value::Bool(true);
        if (cell == "NO") return Value::Bool(false);
    }
    return Value::String(cell);
}

struct Column {
    std::string name, kind = "text";
    bool hasNone = false;
    std::string none;
    std::vector<std::string> refs;
    long long maxLen = 0;   // schema max_len: the most characters the game's loader takes (0 = not known)
};
struct TableSpec {
    std::string file, type, owner;
    bool perKey = false;
    long long maxRows = 0;  // schema max_rows: the most records the game keeps (0 = not known)
    std::vector<Column> cols;
};

std::vector<TableSpec> Specs(const Value& schema) {
    std::vector<TableSpec> out;
    for (const Value* t : Items(schema.Get("tables"))) {
        TableSpec s;
        s.file = StrOr(t->Get("file"));
        s.type = StrOr(t->Get("type"));
        s.owner = StrOr(t->Get("owner"));
        s.perKey = Truthy(t->Get("rows_per_key"));
        if (const Value* m = t->Get("max_rows"); m && m->kind == Kind::Int) s.maxRows = m->i;
        for (const Value* c : Items(t->Get("columns"))) {
            Column col;
            col.name = StrOr(c->Get("name"));
            col.kind = StrOr(c->Get("kind"), "text");
            if (const Value* m = c->Get("max_len"); m && m->kind == Kind::Int) col.maxLen = m->i;
            if (const Value* n = c->Get("none")) {
                col.hasNone = true;
                col.none = Fmt(*n, "text");
            }
            if (const Value* r = c->Get("ref")) {
                if (r->kind == Kind::Table)
                    for (const Value* x : Items(r)) col.refs.push_back(StrOr(x));
                else
                    col.refs.push_back(StrOr(r));
            }
            s.cols.push_back(col);
        }
        out.push_back(std::move(s));
    }
    return out;
}

const Column* ColAt(const TableSpec& t, size_t i) { return i < t.cols.size() ? &t.cols[i] : nullptr; }
std::string ColName(const Column* c, size_t i) { return c ? c->name : "col" + std::to_string(i + 1); }

using KeyIndex = std::map<std::string, std::map<std::string, std::string>>;   // type -> lower key -> key

KeyIndex BuildKeyIndex(const std::vector<TableSpec>& specs, const TableFiles& tables) {
    KeyIndex out;
    for (const TableSpec& t : specs) {
        if (t.perKey) continue;
        auto it = tables.find(t.file);
        if (it == tables.end()) continue;
        auto& keys = out[t.type];
        for (const std::string& line : SplitLines(it->second).lines) {
            if (line.find_first_not_of(" \t\r\n\f\v") == std::string::npos) continue;
            const std::string k = Latin1ToUtf8(line.substr(0, line.find(',')));
            keys.emplace(Lower(k), k);
        }
    }
    return out;
}

}  // namespace

std::string RecordId(const std::string& type, const std::string& key) {
    return std::string(kPrefix) + ":" + type + "/" + Lower(key);
}

bool Generate(const TableFiles& tables, const Value& schema, Forms* out, std::string* error) {
    const std::vector<TableSpec> specs = Specs(schema);
    const KeyIndex keyidx = BuildKeyIndex(specs, tables);
    out->clear();
    for (const TableSpec& t : specs) {
        auto it = tables.find(t.file);
        if (it == tables.end()) continue;
        const Split3 sp = SplitLines(it->second);
        size_t width = 0;
        for (const std::string& l : sp.lines) width = std::max(width, Split(l, ",").size());
        Value recs = Value::Table();
        Value info = Value::Table();
        info.Set("file", Value::String(t.file));
        info.Set("eol", Value::String(sp.eol == "\r\n" ? "crlf" : "lf"));
        info.Set("final_eol", Value::Bool(sp.final));
        info.Set("width", Value::Int(static_cast<int64_t>(width)));
        bool grouped = true;
        if (t.perKey) {
            std::set<std::string> seen;
            std::vector<std::string> keys;
            for (const std::string& l : sp.lines) keys.push_back(Lower(Latin1ToUtf8(l.substr(0, l.find(',')))));
            for (size_t i = 0; i < keys.size(); ++i) {
                if (seen.count(keys[i]) && keys[i - 1] != keys[i]) grouped = false;
                seen.insert(keys[i]);
            }
            if (!grouped) info.Set("by_line", Value::Bool(true));
        }
        std::map<std::string, Value*> groups;
        std::set<std::string> idsSeen;
        for (size_t n = 0; n < sp.lines.size(); ++n) {
            const std::string& line = sp.lines[n];
            const std::vector<std::string> cells = Split(line, ",");
            Value row = Value::Table();
            if (line.empty()) {
                row.Set("_blank", Value::Bool(true));
            } else {
                for (size_t i = 0; i < cells.size(); ++i) {
                    const Column* col = ColAt(t, i);
                    const std::string name = ColName(col, i);
                    const std::string kind = col ? col->kind : "text";
                    if (i == 0 && !t.perKey) continue;
                    const std::string cell = Latin1ToUtf8(cells[i]);
                    if (col && col->hasNone && cell == col->none && (kind == "text" || kind == "ref")) continue;
                    if (kind == "ref") {
                        Value v = Value::String(cell);
                        for (const std::string& tt : col->refs) {
                            auto ki = keyidx.find(tt);
                            if (ki == keyidx.end()) continue;
                            auto kk = ki->second.find(Lower(cell));
                            if (kk != ki->second.end() && kk->second == cell) {
                                v = Value::String(RecordId(tt, cell));
                                break;
                            }
                        }
                        row.Set(name, std::move(v));
                    } else {
                        row.Set(name, Parse(cell, kind));
                    }
                }
                if (cells.size() != width) row.Set("_width", Value::Int(static_cast<int64_t>(cells.size())));
            }
            const std::string key = line.empty() ? "" : Latin1ToUtf8(cells[0]);
            if (t.perKey) {
                if (!grouped) row.Set("_line", Value::Int(static_cast<int64_t>(n)));
                const std::string gk = Lower(key);
                auto g = groups.find(gk);
                if (g == groups.end()) {
                    Value rec = Value::Table();
                    rec.Set("id", Value::String(RecordId(t.type, key)));
                    rec.Set("key", Value::String(key));
                    if (!t.owner.empty()) rec.Set("owner", Value::String(RecordId(t.owner, key)));
                    rec.Set("rows", Value::Table());
                    g = groups.emplace(gk, &recs.Push(std::move(rec))).first;
                }
                g->second->Get("rows")->Push(std::move(row));
            } else {
                if (!line.empty()) {
                    std::string rid = RecordId(t.type, key);
                    if (idsSeen.count(rid)) {
                        int k = 2;
                        while (idsSeen.count(rid + "#" + std::to_string(k))) ++k;
                        rid += "#" + std::to_string(k);
                        row.Set("_duplicate", Value::Bool(true));
                    }
                    idsSeen.insert(rid);
                    row.Set("id", Value::String(rid));
                    row.Set("key", Value::String(key));
                }
                recs.Push(std::move(row));
            }
        }
        // Push may reallocate entries: group pointers were into Values owned by shared_ptr (stable) - fine
        Document doc;
        doc.header = {std::string(kPrefix) + " " + t.type +
                          " records - made from your own game files by the records generator; not shipped, not for editing",
                      "(mods change records in their own files; this one is rebuilt from the game)"};
        doc.root.Set("table", std::move(info));
        doc.root.Set("records", std::move(recs));
        (*out)[t.type] = std::move(doc);
    }
    (void)error;
    return true;
}

namespace {
std::string CellText(const Value& v, const Column* col, const std::map<std::string, std::string>& ids) {
    const std::string kind = col ? col->kind : "text";
    if (v.kind == Kind::String) {
        if (kind == "ref" && v.s.rfind(std::string(kPrefix) + ":", 0) == 0) {
            auto it = ids.find(v.s);
            return it != ids.end() ? it->second : v.s;
        }
        return v.s;
    }
    return Fmt(v, kind);
}

std::string RowText(const Value& row, const TableSpec& t, int64_t width, const std::map<std::string, std::string>& ids,
                    const std::string* key) {
    if (Truthy(row.Get("_blank"))) return "";
    const Value* w = row.Get("_width");
    const int64_t n = w && w->kind == Kind::Int ? w->i : width;
    std::string out;
    for (int64_t i = 0; i < n; ++i) {
        if (i) out += ",";
        const Column* col = ColAt(t, static_cast<size_t>(i));
        if (i == 0 && key) {
            out += *key;
            continue;
        }
        const Value* v = row.Get(ColName(col, static_cast<size_t>(i)));
        if (!v) out += col && col->hasNone ? col->none : "";
        else out += CellText(*v, col, ids);
    }
    return out;
}
}  // namespace

TableFiles Tables(const Forms& forms, const Value& schema) {
    const std::vector<TableSpec> specs = Specs(schema);
    std::map<std::string, std::string> ids;
    for (const TableSpec& t : specs) {   // schema order, as forms.py's dict order
        auto it = forms.find(t.type);
        if (it == forms.end()) continue;
        for (const Value* r : Items(it->second.root.Get("records")))
            if (r->kind == Kind::Table && Truthy(r->Get("id")) && r->Get("key"))
                if (const std::string* id = Str(r->Get("id"))) ids.emplace(*id, Fmt(*r->Get("key"), "text"));
    }
    TableFiles out;
    for (const TableSpec& t : specs) {
        auto it = forms.find(t.type);
        if (it == forms.end()) continue;
        const Value& root = it->second.root;
        const Value* info = root.Get("table");
        const int64_t width = info->Get("width")->i;
        std::vector<std::string> lines;
        if (t.perKey) {
            const std::string first = t.cols.empty() ? "col1" : t.cols[0].name;
            struct R {
                const Value* row;
                std::string key;
                bool keyed;
            };
            std::vector<R> rows;
            for (const Value* rec : Items(root.Get("records"))) {
                const std::string rk = StrOr(rec->Get("key"));
                for (const Value* row : Items(rec->Get("rows"))) {
                    const std::string* own = Str(row->Get(first));
                    rows.push_back({row, rk, !own || Lower(*own) != Lower(rk)});
                }
            }
            if (Truthy(info->Get("by_line")))
                std::stable_sort(rows.begin(), rows.end(), [](const R& a, const R& b) {
                    const Value* x = a.row->Get("_line");
                    const Value* y = b.row->Get("_line");
                    const int64_t xa = x && x->kind == Kind::Int ? x->i : (int64_t(1) << 30);
                    const int64_t yb = y && y->kind == Kind::Int ? y->i : (int64_t(1) << 30);
                    return xa < yb;
                });
            for (const R& r : rows) lines.push_back(RowText(*r.row, t, width, ids, r.keyed ? &r.key : nullptr));
        } else {
            for (const Value* rec : Items(root.Get("records"))) {
                if (Truthy(rec->Get("_blank"))) {
                    lines.push_back(RowText(*rec, t, width, ids, nullptr));
                } else {
                    const std::string k = rec->Get("key") ? Fmt(*rec->Get("key"), "text") : "";
                    lines.push_back(RowText(*rec, t, width, ids, &k));
                }
            }
        }
        const std::string eol = StrOr(info->Get("eol")) == "crlf" ? "\r\n" : "\n";
        std::string text;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i) text += eol;
            text += lines[i];
        }
        if (Truthy(info->Get("final_eol"))) text += eol;
        out[t.file] = Utf8ToLatin1(text);
    }
    return out;
}

namespace {
std::string TypeOf(const std::string& rid) {
    const size_t c = rid.find(':');
    if (c == std::string::npos || rid.find('/') == std::string::npos) return "";
    const std::string rest = rid.substr(c + 1);
    return rest.substr(0, rest.find('/'));
}
std::string KeyText(const Key& k) { return k.type == Key::Type::Int ? std::to_string(k.i) : k.name; }
std::string ValueText(const Value& v) {   // forms.py: dumps({"v": value}) - for comparing two mods' values
    Document d;
    d.root.Set("v", v.Clone());
    std::string s, e;
    pse::data::Write(d, &s, &e);
    return s;
}
long long Chars(const std::string& utf8) {   // forms.py: len(str) - code points
    long long n = 0;
    for (unsigned char c : utf8)
        if ((c & 0xC0) != 0x80) ++n;
    return n;
}
// The column a field writes (the record's "key" is column 1); null when the schema doesn't name it.
const Column* FieldColumn(const TableSpec* t, const std::string& field) {
    if (!t) return nullptr;
    if (field == "key") return t->cols.empty() ? nullptr : &t->cols[0];
    for (const Column& c : t->cols)
        if (c.name == field) return &c;
    return nullptr;
}
// A text value longer than the column's max_len (a ref given as a record id is that record's key: checked there).
bool TooLong(const Column* col, const Value& v, long long* n) {
    if (!col || col->maxLen <= 0 || v.kind != Kind::String) return false;
    if (col->kind == "ref" && v.s.rfind("ds1:", 0) == 0) return false;
    *n = Chars(v.s);
    return *n > col->maxLen;
}
std::string ScalarText(const Value& v) {   // Python "%s" of a clear-list entry (strings / integers)
    if (v.kind == Kind::String) return v.s;
    if (v.kind == Kind::Int) return std::to_string(v.i);
    if (v.kind == Kind::Bool) return v.b ? "True" : "False";
    return "?";
}
}  // namespace

void Merge(Forms* forms, const std::vector<Edits>& edits, const Value& schema, std::vector<std::string>* report) {
    const std::vector<TableSpec> specs = Specs(schema);
    struct Where {
        std::string type;
        Value* rec;
    };
    std::map<std::string, Where> index;
    for (auto& [type, doc] : *forms)
        for (Value* r : ItemsMut(doc.root.Get("records")))
            if (r->kind == Kind::Table && Truthy(r->Get("id")))
                if (const std::string* id = Str(r->Get("id"))) index[*id] = {type, r};
    std::map<std::pair<std::string, std::string>, std::pair<std::string, std::string>> setby;
    static const std::set<std::string> kEditKeys = {"id", "based_on", "delete", "clear"};
    for (const Edits& ed : edits) {
        for (const Value* e : Items(&ed.doc.root)) {
            if (e->kind != Kind::Table) continue;
            const std::string* ridp = Str(e->Get("id"));
            if (!ridp || TypeOf(*ridp).empty()) {
                report->push_back("[fail] " + ed.mod + "/" + ed.file + ": an edit without a valid id (ds1:<type>/<name>)");
                continue;
            }
            const std::string rid = *ridp, rtype = TypeOf(rid);
            auto docIt = forms->find(rtype);
            if (docIt == forms->end()) {
                report->push_back("[fail] " + ed.mod + "/" + ed.file + ": " + rid + " - no record type " + rtype);
                continue;
            }
            const Value* del = e->Get("delete");
            if (del && del->kind == Kind::Bool && del->b) {
                auto at = index.find(rid);
                if (at != index.end()) {
                    Value* recs = docIt->second.root.Get("records");
                    Value* target = at->second.rec;
                    recs->entries.erase(std::remove_if(recs->entries.begin(), recs->entries.end(),
                                                       [&](const Entry& x) { return x.value.get() == target; }),
                                        recs->entries.end());
                    index.erase(at);
                    report->push_back("deleted " + rid + " (" + ed.mod + ")");
                }
                continue;
            }
            const TableSpec* spec = nullptr;
            for (const TableSpec& t : specs)
                if (t.type == rtype) {
                    spec = &t;
                    break;
                }
            Value* rec = nullptr;
            bool created = false;
            std::string newLine;
            auto at = index.find(rid);
            if (at != index.end()) rec = at->second.rec;
            if (!rec) {
                const std::string* base = Str(e->Get("based_on"));
                if (!base || !index.count(*base)) {
                    report->push_back("[fail] " + ed.mod + "/" + ed.file + ": " + rid +
                                      " isn't a game record - a new one needs based_on = <a record id>");
                    continue;
                }
                if (spec && spec->maxRows > 0) {
                    long long rows = 0;
                    for (const Value* r : Items(docIt->second.root.Get("records")))
                        if (r->kind == Kind::Table && !Truthy(r->Get("_blank"))) ++rows;
                    if (rows >= spec->maxRows) {
                        report->push_back("[fail] " + ed.mod + "/" + ed.file + ": " + rid + " not added - " + spec->file +
                                          " holds at most " + std::to_string(spec->maxRows) + " records");
                        continue;
                    }
                }
                Value copy = index[*base].rec->Clone();
                copy.Set("id", Value::String(rid));
                copy.Remove("_duplicate");
                copy.Remove("_line");
                if (!e->Get("key")) copy.Set("key", Value::String(Upper(rid.substr(rid.find('/') + 1))));
                rec = &docIt->second.root.Get("records")->Push(std::move(copy));
                index[rid] = {rtype, rec};
                created = true;
                newLine = "new " + rid + " based on " + *base + " (" + ed.mod + ")";
            }
            const size_t firstLine = report->size();   // the "new" line goes before this entry's other lines
            long long keyLen = 0;                        // a new record's key that doesn't fit
            if (const Value* clear = e->Get("clear"); clear && clear->kind == Kind::Table) {
                for (const Value* f : Items(clear)) {
                    const Column* col = nullptr;
                    for (const TableSpec& t : specs)
                        if (t.type == rtype) {
                            for (const Column& c : t.cols)
                                if (f->kind == Kind::String && c.name == f->s) {
                                    col = &c;
                                    break;
                                }
                            break;
                        }
                    if (!col || !col->hasNone) {
                        report->push_back("[fail] " + ed.mod + "/" + ed.file + ": " + rid + " " + ScalarText(*f) +
                                          " can't be cleared - the game has no \"none\" for that field");
                        continue;
                    }
                    rec->Remove(f->s);
                    setby[{rid, f->s}] = {ed.mod, "none"};
                }
            }
            for (const Entry& ent : e->entries) {
                if (ent.key.type == Key::Type::None) continue;
                if (ent.key.type == Key::Type::Name && kEditKeys.count(ent.key.name)) continue;
                const std::string kt = (ent.key.type == Key::Type::Int ? "#" : "") + KeyText(ent.key);
                long long n = 0;
                const Column* col = ent.key.type == Key::Type::Name ? FieldColumn(spec, ent.key.name) : nullptr;
                if (TooLong(col, *ent.value, &n)) {
                    if (created && ent.key.name == "key") {   // reported once below: the record isn't added
                        keyLen = n;
                        continue;
                    }
                    report->push_back("[fail] " + ed.mod + "/" + ed.file + ": " + rid + " " + ent.key.name + " is " +
                                      std::to_string(n) + " characters - the game reads at most " +
                                      std::to_string(col->maxLen) + "; not changed");
                    continue;
                }
                const std::string text = ValueText(*ent.value);
                auto prev = setby.find({rid, kt});
                if (prev != setby.end() && prev->second.first != ed.mod && prev->second.second != text)
                    report->push_back("conflict: " + rid + " " + KeyText(ent.key) + ": " + prev->second.first + " / " +
                                      ed.mod + " (" + ed.mod + " wins)");
                setby[{rid, kt}] = {ed.mod, text};
                rec->SetKey(ent.key, ent.value->Clone());
            }
            long long n = keyLen;
            const Column* keyCol = FieldColumn(spec, "key");
            if (created && (keyLen > 0 || (rec->Get("key") && TooLong(keyCol, *rec->Get("key"), &n)))) {
                report->erase(report->begin() + static_cast<std::ptrdiff_t>(firstLine), report->end());
                Value* recs = docIt->second.root.Get("records");
                recs->entries.erase(std::remove_if(recs->entries.begin(), recs->entries.end(),
                                                   [&](const Entry& x) { return x.value.get() == rec; }),
                                    recs->entries.end());
                index.erase(rid);
                report->push_back("[fail] " + ed.mod + "/" + ed.file + ": " + rid + " not added - its key is " +
                                  std::to_string(n) + " characters, the game reads at most " +
                                  std::to_string(keyCol->maxLen));
            } else if (created) {
                report->insert(report->begin() + static_cast<std::ptrdiff_t>(firstLine), newLine);
            }
        }
    }
}

}  // namespace pse::records

#pragma once
// PSE data files (DESIGN.md §13.4): Lua tables, literals only. The records layer's base forms, placed references and
// mod edits are written in this format; the ModKit's Python reader / writer (conflict_ds_tools/pse_data.py) produces
// byte-identical output (scripts/records_gate.py checks both on the same inputs).
//
// A file:   [-- header comment lines]  return <table>
// Values:   tables, strings ("..." / '...' / [[...]]), integers (decimal / hex, 64-bit), floats, true / false, and
//           '-' before a number. Nothing else - no names, calls, operators, nil, varargs; refused with line:column.
// Keys:     name = v, ["string"] = v, [integer] = v; plain items = the list part. Duplicate keys are refused.
// Comments: full-line comments ("--", "--[[ ]]") before an entry belong to that entry; comments elsewhere (after a
//           value on the same line, after the last entry) are dropped. Block comments become "--" lines.
//
// Canonical output (Write): what the editor saves, so a re-saved file only differs where something changed -
//   - header comments, "return " + the table, "\n" line endings, a final "\n";
//   - list items first, in order; then keyed entries: "id" first, integer keys ascending, then string keys in byte
//     order; keys that are Lua names are written bare, others as ["..."];
//   - a table with no comments, only scalars and a one-line form that fits in 100 columns (indent included) is written
//     on one line: { 1, 2, 3 } / { x = 1, y = 2 } / {}; otherwise one entry per line, 2 spaces per level, every
//     entry followed by ",";
//   - strings in "...": \\ \" \n \r \t escaped, other bytes below 0x20 and 0x7F as \ddd, everything else (UTF-8)
//     as is;
//   - integers in decimal; floats the shortest text that reads back to the same double, Python repr style
//     (scientific "1e+16" / "1.5e-05" when the exponent is < -4 or >= 16, else fixed with ".0" when whole);
//     inf / nan can't be written.
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pse::data {

struct Value;
struct Entry;
struct Key;

enum class Kind { Bool, Int, Float, String, Table };

struct Value {
    Kind kind = Kind::Table;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::string s;
    std::vector<Entry> entries;   // a table: list items (keyless) and keyed entries, in file order

    static Value Bool(bool v) { Value x; x.kind = Kind::Bool; x.b = v; return x; }
    static Value Int(int64_t v) { Value x; x.kind = Kind::Int; x.i = v; return x; }
    static Value Float(double v) { Value x; x.kind = Kind::Float; x.f = v; return x; }
    static Value String(std::string v) { Value x; x.kind = Kind::String; x.s = std::move(v); return x; }
    static Value Table() { return Value(); }

    // keyed access (string keys); null = no such key
    const Value* Get(std::string_view key) const;
    Value* Get(std::string_view key);
    // sets / replaces a string-keyed entry (comments of an existing entry kept)
    Value& Set(std::string key, Value v);
    // sets / replaces an entry by key (string or integer key; comments of an existing entry kept)
    Value& SetKey(const Key& key, Value v);
    // removes keyed entries with that string key; false = none
    bool Remove(std::string_view key);
    // appends a list item
    Value& Push(Value v);
    // a deep copy (entries share nothing with this one; comments dropped)
    Value Clone() const;
    bool Equals(const Value& o) const;   // same content (comments and entry order of keyed entries ignored)
};

struct Key {
    enum class Type { None, Name, Int } type = Type::None;   // None = a list item
    std::string name;
    int64_t i = 0;
};

struct Entry {
    Key key;
    std::shared_ptr<Value> value;   // shared_ptr: Value holds a vector of Entry
    std::vector<std::string> comments;   // full-line comments before it, without the "--" and one following space
};

struct Document {
    std::vector<std::string> header;   // comments before "return"
    Value root;
};

// false = not a PSE data file: *error = "line:column: what".
bool Parse(std::string_view text, Document* out, std::string* error);
// Canonical text. false (+ *error) only for a float that is inf / nan or a table nested deeper than 200 levels.
bool Write(const Document& doc, std::string* out, std::string* error);

// The float rule above, alone (shared with the tests).
std::string FormatFloat(double v);

}  // namespace pse::data

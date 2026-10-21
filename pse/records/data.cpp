// PSE data files - reader / canonical writer (data.h).
#include "data.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace pse::data {
namespace {

constexpr size_t kWidth = 100;   // one-line tables: the whole line (indent included, bytes) at most this long
constexpr int kMaxDepth = 200;

const char* const kKeywords[] = {"and",   "break", "do",  "else", "elseif", "end",    "false", "for",
                                 "function", "goto", "if", "in",   "local",  "nil",    "not",   "or",
                                 "repeat", "return", "then", "true", "until",  "while"};

bool IsNameStart(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool IsNameChar(char c) { return IsNameStart(c) || (c >= '0' && c <= '9'); }

bool IsName(std::string_view s) {
    if (s.empty() || !IsNameStart(s[0])) return false;
    for (char c : s)
        if (!IsNameChar(c)) return false;
    for (const char* k : kKeywords)
        if (s == k) return false;
    return true;
}

// ---- reader ---------------------------------------------------------------------------------------------------------

class Parser {
public:
    explicit Parser(std::string_view t) : t_(t) {}

    bool Run(Document* doc, std::string* error) {
        pending_.clear();
        SkipSpace();
        doc->header = std::move(pending_);
        pending_.clear();
        if (!Word("return")) return Fail(error, "expected 'return' followed by a table");
        SkipSpace();
        pending_.clear();
        if (!Peek('{')) return Fail(error, "expected '{' after 'return'");
        if (!Table(&doc->root, 0)) return Fail(error, why_);
        SkipSpace();
        if (p_ < t_.size()) return Fail(error, "unexpected text after the table");
        return true;
    }

private:
    bool Fail(std::string* error, const std::string& what) {
        if (error) {
            size_t line = 1, col = 1;
            for (size_t k = 0; k < p_ && k < t_.size(); ++k) {
                if (t_[k] == '\n') ++line, col = 1;
                else ++col;
            }
            *error = std::to_string(line) + ":" + std::to_string(col) + ": " + what;
        }
        return false;
    }
    bool Err(const char* what) {
        why_ = what;
        return false;
    }

    bool Peek(char c) const { return p_ < t_.size() && t_[p_] == c; }

    bool Word(const char* w) {
        const size_t n = strlen(w);
        if (t_.substr(p_, n) != w || (p_ + n < t_.size() && IsNameChar(t_[p_ + n]))) return false;
        p_ += n;
        return true;
    }

    // Long bracket at p_ ("[[", "[=[" ...): its level, or -1.
    int LongLevel(size_t at) const {
        if (at >= t_.size() || t_[at] != '[') return -1;
        size_t k = at + 1;
        int level = 0;
        while (k < t_.size() && t_[k] == '=') ++k, ++level;
        return k < t_.size() && t_[k] == '[' ? level : -1;
    }
    // Reads a long bracket's content (p_ at '['). false = not closed.
    bool LongBody(int level, std::string* out) {
        p_ += 2 + level;
        if (p_ < t_.size() && t_[p_] == '\r') ++p_;   // a newline right after the opening bracket is skipped
        if (p_ < t_.size() && t_[p_] == '\n') ++p_;
        const std::string close = "]" + std::string(level, '=') + "]";
        const size_t end = t_.find(close, p_);
        if (end == std::string_view::npos) return false;
        out->assign(t_.substr(p_, end - p_));
        p_ = end + close.size();
        return true;
    }

    // Spaces and comments. A comment starting a line (only spaces before it) is kept in pending_; others are dropped.
    void SkipSpace() {
        bool lineStart = p_ == 0 || t_[p_ - 1] == '\n';
        while (p_ < t_.size()) {
            const char c = t_[p_];
            if (c == '\n') {
                lineStart = true;
                ++p_;
            } else if (c == ' ' || c == '\t' || c == '\r') {
                ++p_;
            } else if (c == '-' && p_ + 1 < t_.size() && t_[p_ + 1] == '-') {
                p_ += 2;
                const int level = LongLevel(p_);
                std::string text;
                if (level >= 0) {
                    if (!LongBody(level, &text)) {   // unclosed block comment: the rest of the file
                        text.assign(t_.substr(p_));
                        p_ = t_.size();
                    }
                    if (lineStart) {
                        size_t s = 0;
                        while (true) {
                            size_t e = text.find('\n', s);
                            std::string l(text.substr(s, e == std::string::npos ? std::string::npos : e - s));
                            while (!l.empty() && (l.back() == '\r' || l.back() == ' ' || l.back() == '\t')) l.pop_back();
                            if (!l.empty() && l[0] == ' ') l.erase(0, 1);   // as for "-- text"
                            pending_.push_back(l);
                            if (e == std::string::npos) break;
                            s = e + 1;
                        }
                    }
                } else {
                    const size_t e = t_.find('\n', p_);
                    text.assign(t_.substr(p_, e == std::string_view::npos ? std::string_view::npos : e - p_));
                    p_ = e == std::string_view::npos ? t_.size() : e;
                    while (!text.empty() && (text.back() == '\r' || text.back() == ' ' || text.back() == '\t'))
                        text.pop_back();
                    if (!text.empty() && text[0] == ' ') text.erase(0, 1);
                    if (lineStart) pending_.push_back(text);
                }
            } else {
                break;
            }
        }
    }

    bool Table(Value* out, int depth) {
        if (depth > kMaxDepth) return Err("tables nested too deeply");
        *out = Value::Table();
        ++p_;   // '{'
        size_t listCount = 0;
        while (true) {
            SkipSpace();
            if (p_ >= t_.size()) return Err("table not closed ('}' missing)");
            if (Peek('}')) {
                ++p_;
                pending_.clear();   // comments after the last entry are dropped
                break;
            }
            Entry e;
            e.comments = std::move(pending_);
            pending_.clear();
            const size_t keyAt = p_;
            if (Peek('[') && LongLevel(p_) < 0) {   // [key] =
                ++p_;
                SkipSpace();
                Value k;
                if (!Scalar(&k)) return false;
                if (k.kind == Kind::String) {
                    e.key.type = Key::Type::Name;
                    e.key.name = k.s;
                } else if (k.kind == Kind::Int) {
                    e.key.type = Key::Type::Int;
                    e.key.i = k.i;
                } else {
                    p_ = keyAt;
                    return Err("a key must be a string or an integer");
                }
                SkipSpace();
                if (!Peek(']')) return Err("']' expected after the key");
                ++p_;
                SkipSpace();
                if (!Peek('=')) return Err("'=' expected after the key");
                ++p_;
            } else if (p_ < t_.size() && IsNameStart(t_[p_])) {
                size_t k = p_;
                while (k < t_.size() && IsNameChar(t_[k])) ++k;
                const std::string_view name = t_.substr(p_, k - p_);
                size_t q = k;
                while (q < t_.size() && (t_[q] == ' ' || t_[q] == '\t' || t_[q] == '\r' || t_[q] == '\n')) ++q;
                if (q < t_.size() && t_[q] == '=' && !(q + 1 < t_.size() && t_[q + 1] == '=') && IsName(name)) {
                    e.key.type = Key::Type::Name;
                    e.key.name.assign(name);
                    p_ = q + 1;
                }
            }
            SkipSpace();
            e.value = std::make_shared<Value>();
            if (Peek('{')) {
                if (!Table(e.value.get(), depth + 1)) return false;
            } else if (!Scalar(e.value.get())) {
                return false;
            }
            if (e.key.type == Key::Type::None) ++listCount;
            for (const Entry& o : out->entries) {
                if (o.key.type != e.key.type || e.key.type == Key::Type::None) continue;
                if ((e.key.type == Key::Type::Name && o.key.name == e.key.name) ||
                    (e.key.type == Key::Type::Int && o.key.i == e.key.i)) {
                    p_ = keyAt;
                    return Err("duplicate key");
                }
            }
            out->entries.push_back(std::move(e));
            SkipSpace();
            if (Peek(',') || Peek(';')) {
                ++p_;
                continue;
            }
            SkipSpace();
            if (Peek('}')) continue;
            return Err("',' or '}' expected");
        }
        for (const Entry& e : out->entries)
            if (e.key.type == Key::Type::Int && e.key.i >= 1 && static_cast<uint64_t>(e.key.i) <= listCount)
                return Err("an integer key overlaps the list items");
        return true;
    }

    bool Scalar(Value* out) {
        if (p_ >= t_.size()) return Err("value expected");
        const char c = t_[p_];
        if (c == '"' || c == '\'') return QuotedString(out);
        const int level = LongLevel(p_);
        if (level >= 0) {
            std::string s;
            if (!LongBody(level, &s)) return Err("long string not closed");
            *out = Value::String(std::move(s));
            return true;
        }
        if (Word("true")) return *out = Value::Bool(true), true;
        if (Word("false")) return *out = Value::Bool(false), true;
        if (c == '-' || (c >= '0' && c <= '9') || c == '.') return Number(out);
        if (Word("nil")) return Err("nil is not allowed (leave the entry out)");
        return Err("only literals are allowed: tables, strings, numbers, true / false");
    }

    bool Number(Value* out) {
        bool neg = false;
        if (Peek('-')) {
            neg = true;
            ++p_;
            SkipSpace();
        }
        const size_t s = p_;
        if (t_.substr(p_, 2) == "0x" || t_.substr(p_, 2) == "0X") {
            p_ += 2;
            uint64_t v = 0;
            size_t n = 0;
            while (p_ < t_.size() && isxdigit(static_cast<unsigned char>(t_[p_]))) {
                const char d = t_[p_];
                v = v * 16 + static_cast<uint64_t>(d <= '9' ? d - '0' : (d | 0x20) - 'a' + 10);   // wraps like Lua
                ++p_, ++n;
            }
            if (!n || (p_ < t_.size() && (IsNameChar(t_[p_]) || t_[p_] == '.'))) return Err("bad hexadecimal number");
            int64_t i = static_cast<int64_t>(v);
            *out = Value::Int(neg ? static_cast<int64_t>(0 - static_cast<uint64_t>(i)) : i);
            return true;
        }
        bool isFloat = false;
        while (p_ < t_.size()) {
            const char d = t_[p_];
            if (d >= '0' && d <= '9') {
                ++p_;
            } else if (d == '.' || d == 'e' || d == 'E') {
                isFloat = true;
                ++p_;
                if ((d == 'e' || d == 'E') && p_ < t_.size() && (t_[p_] == '+' || t_[p_] == '-')) ++p_;
            } else {
                break;
            }
        }
        if (p_ < t_.size() && IsNameChar(t_[p_])) return Err("bad number");
        const std::string text(t_.substr(s, p_ - s));
        if (text.empty() || text == ".") return Err("bad number");
        if (!isFloat) {
            uint64_t v = 0;
            const auto r = std::from_chars(text.data(), text.data() + text.size(), v);
            if (r.ec != std::errc() || r.ptr != text.data() + text.size() ||
                v > (neg ? 0x8000000000000000ull : 0x7FFFFFFFFFFFFFFFull))
                return Err("integer out of range (64-bit)");
            *out = Value::Int(neg ? static_cast<int64_t>(0 - v) : static_cast<int64_t>(v));
            return true;
        }
        double d = 0;
        const auto r = std::from_chars(text.data(), text.data() + text.size(), d);
        if (r.ec != std::errc() || r.ptr != text.data() + text.size()) return Err("bad number");
        *out = Value::Float(neg ? -d : d);
        return true;
    }

    static void PutUtf8(std::string* s, uint32_t cp) {
        if (cp < 0x80) {
            s->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s->push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            s->push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            s->push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool QuotedString(Value* out) {
        const char q = t_[p_++];
        std::string s;
        while (true) {
            if (p_ >= t_.size() || t_[p_] == '\n') return Err("string not closed");
            const char c = t_[p_++];
            if (c == q) break;
            if (c != '\\') {
                s.push_back(c);
                continue;
            }
            if (p_ >= t_.size()) return Err("string not closed");
            const char e = t_[p_++];
            switch (e) {
                case 'a': s.push_back('\a'); break;
                case 'b': s.push_back('\b'); break;
                case 'f': s.push_back('\f'); break;
                case 'n': s.push_back('\n'); break;
                case 'r': s.push_back('\r'); break;
                case 't': s.push_back('\t'); break;
                case 'v': s.push_back('\v'); break;
                case '\\': s.push_back('\\'); break;
                case '"': s.push_back('"'); break;
                case '\'': s.push_back('\''); break;
                case '\n': s.push_back('\n'); break;
                case 'z':
                    while (p_ < t_.size() && isspace(static_cast<unsigned char>(t_[p_]))) ++p_;
                    break;
                case 'x': {
                    if (p_ + 2 > t_.size() || !isxdigit(static_cast<unsigned char>(t_[p_])) ||
                        !isxdigit(static_cast<unsigned char>(t_[p_ + 1])))
                        return Err("bad \\x escape");
                    s.push_back(static_cast<char>(std::stoi(std::string(t_.substr(p_, 2)), nullptr, 16)));
                    p_ += 2;
                    break;
                }
                case 'u': {
                    if (!Peek('{')) return Err("bad \\u escape");
                    ++p_;
                    uint32_t cp = 0;
                    size_t n = 0;
                    while (p_ < t_.size() && isxdigit(static_cast<unsigned char>(t_[p_]))) {
                        const char d = t_[p_++];
                        cp = cp * 16 + static_cast<uint32_t>(d <= '9' ? d - '0' : (d | 0x20) - 'a' + 10);
                        if (cp > 0x7FFFFFFF) return Err("bad \\u escape");
                        ++n;
                    }
                    if (!n || !Peek('}') || cp > 0x10FFFF) return Err("bad \\u escape");
                    ++p_;
                    PutUtf8(&s, cp);
                    break;
                }
                default:
                    if (e >= '0' && e <= '9') {
                        int v = e - '0';
                        for (int k = 0; k < 2 && p_ < t_.size() && t_[p_] >= '0' && t_[p_] <= '9'; ++k)
                            v = v * 10 + (t_[p_++] - '0');
                        if (v > 255) return Err("bad \\ddd escape");
                        s.push_back(static_cast<char>(v));
                        break;
                    }
                    return Err("bad escape in a string");
            }
        }
        *out = Value::String(std::move(s));
        return true;
    }

    std::string_view t_;
    size_t p_ = 0;
    std::vector<std::string> pending_;
    std::string why_;
};

// ---- writer ---------------------------------------------------------------------------------------------------------

std::string Quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20 || c == 0x7F) {
                    char b[8];   // always 3 digits: a digit right after a shorter \dd would be read as part of it
                    std::snprintf(b, sizeof b, "\\%03u", static_cast<unsigned>(c));
                    out += b;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out + "\"";
}

std::string KeyText(const Key& k) {
    if (k.type == Key::Type::None) return {};
    if (k.type == Key::Type::Int) return "[" + std::to_string(k.i) + "] = ";
    return (IsName(k.name) ? k.name : "[" + Quote(k.name) + "]") + " = ";
}

// Canonical entry order: list items (in order), then "id", integer keys ascending, string keys in byte order.
std::vector<const Entry*> Ordered(const Value& t) {
    std::vector<const Entry*> list, keyed;
    for (const Entry& e : t.entries) (e.key.type == Key::Type::None ? list : keyed).push_back(&e);
    std::stable_sort(keyed.begin(), keyed.end(), [](const Entry* a, const Entry* b) {
        auto rank = [](const Entry* e) {
            if (e->key.type == Key::Type::Name && e->key.name == "id") return 0;
            return e->key.type == Key::Type::Int ? 1 : 2;
        };
        const int ra = rank(a), rb = rank(b);
        if (ra != rb) return ra < rb;
        if (ra == 1) return a->key.i < b->key.i;
        return a->key.name < b->key.name;   // char_traits<char> compares as unsigned bytes
    });
    list.insert(list.end(), keyed.begin(), keyed.end());
    return list;
}

class Writer {
public:
    bool Run(const Document& doc, std::string* out, std::string* error) {
        for (const std::string& c : doc.header) out_ += Comment(c, 0);
        if (!Emit(doc.root, 0, "return ", "")) {
            if (error) *error = why_;
            return false;
        }
        *out = std::move(out_);
        return true;
    }

private:
    static std::string Comment(const std::string& text, int depth) {
        return std::string(static_cast<size_t>(depth) * 2, ' ') + (text.empty() ? "--" : "-- " + text) + "\n";
    }

    bool ScalarText(const Value& v, std::string* s) {
        switch (v.kind) {
            case Kind::Bool: *s = v.b ? "true" : "false"; return true;
            case Kind::Int: *s = std::to_string(v.i); return true;
            case Kind::Float:
                if (!std::isfinite(v.f)) {
                    why_ = "inf / nan can't be written";
                    return false;
                }
                *s = FormatFloat(v.f);
                return true;
            case Kind::String: *s = Quote(v.s); return true;
            default: return false;
        }
    }

    // One-line form, or "" when the table must be multi-line (comments, nested tables).
    bool Inline(const Value& t, std::string* s) {
        if (t.entries.empty()) return *s = "{}", true;
        std::string body;
        for (const Entry* e : Ordered(t)) {
            if (!e->comments.empty() || e->value->kind == Kind::Table) return *s = "", true;
            std::string v;
            if (!ScalarText(*e->value, &v)) return false;
            if (!body.empty()) body += ", ";
            body += KeyText(e->key) + v;
        }
        *s = "{ " + body + " }";
        return true;
    }

    bool Emit(const Value& v, int depth, const std::string& prefix, const std::string& suffix) {
        const std::string indent(static_cast<size_t>(depth) * 2, ' ');
        if (depth > kMaxDepth) {
            why_ = "tables nested too deeply";
            return false;
        }
        if (v.kind != Kind::Table) {
            std::string s;
            if (!ScalarText(v, &s)) return false;
            out_ += indent + prefix + s + suffix + "\n";
            return true;
        }
        std::string one;
        if (!Inline(v, &one)) return false;
        if (!one.empty() && indent.size() + prefix.size() + one.size() + suffix.size() <= kWidth) {
            out_ += indent + prefix + one + suffix + "\n";
            return true;
        }
        out_ += indent + prefix + "{\n";
        for (const Entry* e : Ordered(v)) {
            for (const std::string& c : e->comments) out_ += Comment(c, depth + 1);
            if (!Emit(*e->value, depth + 1, KeyText(e->key), ",")) return false;
        }
        out_ += indent + "}" + suffix + "\n";
        return true;
    }

    std::string out_, why_;
};

}  // namespace

std::string FormatFloat(double v) {
    if (v == 0) return std::signbit(v) ? "-0.0" : "0.0";
    char buf[64];
    const auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);   // shortest round trip
    std::string sci(buf, r.ptr);
    const bool neg = sci[0] == '-';
    if (neg) sci.erase(0, 1);
    const size_t epos = sci.find('e');
    std::string digits = sci.substr(0, epos);
    digits.erase(std::remove(digits.begin(), digits.end(), '.'), digits.end());
    const int exp = std::stoi(sci.substr(epos + 1));
    std::string out;
    if (exp < -4 || exp >= 16) {   // scientific, Python repr style: d[.ddd]e+XX (at least 2 exponent digits)
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        char e[16];
        std::snprintf(e, sizeof e, "e%c%02d", exp < 0 ? '-' : '+', exp < 0 ? -exp : exp);
        out += e;
    } else if (exp < 0) {
        out = "0." + std::string(static_cast<size_t>(-exp - 1), '0') + digits;
    } else {
        const size_t intLen = static_cast<size_t>(exp) + 1;
        if (digits.size() <= intLen) out = digits + std::string(intLen - digits.size(), '0') + ".0";
        else out = digits.substr(0, intLen) + "." + digits.substr(intLen);
    }
    return neg ? "-" + out : out;
}

const Value* Value::Get(std::string_view key) const {
    for (const Entry& e : entries)
        if (e.key.type == Key::Type::Name && e.key.name == key) return e.value.get();
    return nullptr;
}
Value* Value::Get(std::string_view key) {
    return const_cast<Value*>(static_cast<const Value*>(this)->Get(key));
}
Value& Value::Set(std::string key, Value v) {
    for (Entry& e : entries)
        if (e.key.type == Key::Type::Name && e.key.name == key) return *e.value = std::move(v);
    Entry e;
    e.key.type = Key::Type::Name;
    e.key.name = std::move(key);
    e.value = std::make_shared<Value>(std::move(v));
    entries.push_back(std::move(e));
    return *entries.back().value;
}
Value& Value::SetKey(const Key& key, Value v) {
    if (key.type == Key::Type::None) return Push(std::move(v));
    for (Entry& e : entries)
        if (e.key.type == key.type && (key.type == Key::Type::Name ? e.key.name == key.name : e.key.i == key.i))
            return *e.value = std::move(v);
    Entry e;
    e.key = key;
    e.value = std::make_shared<Value>(std::move(v));
    entries.push_back(std::move(e));
    return *entries.back().value;
}
bool Value::Remove(std::string_view key) {
    const size_t before = entries.size();
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const Entry& e) { return e.key.type == Key::Type::Name && e.key.name == key; }),
                  entries.end());
    return entries.size() != before;
}
Value Value::Clone() const {
    Value v = *this;
    for (Entry& e : v.entries) {
        e.value = std::make_shared<Value>(e.value->Clone());
        e.comments.clear();
    }
    return v;
}
Value& Value::Push(Value v) {
    Entry e;
    e.value = std::make_shared<Value>(std::move(v));
    entries.push_back(std::move(e));
    return *entries.back().value;
}
bool Value::Equals(const Value& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
        case Kind::Bool: return b == o.b;
        case Kind::Int: return i == o.i;
        case Kind::Float: return f == o.f || (std::isnan(f) && std::isnan(o.f));
        case Kind::String: return s == o.s;
        case Kind::Table: break;
    }
    const auto a = Ordered(*this), c = Ordered(o);
    if (a.size() != c.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) {
        const Key &x = a[k]->key, &y = c[k]->key;
        if (x.type != y.type || x.name != y.name || x.i != y.i || !a[k]->value->Equals(*c[k]->value)) return false;
    }
    return true;
}

bool Parse(std::string_view text, Document* out, std::string* error) {
    if (text.substr(0, 3) == "\xEF\xBB\xBF") text.remove_prefix(3);   // UTF-8 BOM
    Document doc;
    if (!Parser(text).Run(&doc, error)) return false;
    *out = std::move(doc);
    return true;
}

bool Write(const Document& doc, std::string* out, std::string* error) { return Writer().Run(doc, out, error); }

}  // namespace pse::data

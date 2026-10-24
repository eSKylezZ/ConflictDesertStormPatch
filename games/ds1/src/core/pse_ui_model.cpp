// pse.ui's pure parts - see pse_ui_model.h. No game memory, no Windows calls: unit-tested in pse_tests.
#include "core/pse_ui_model.h"

#include <cstdio>
#include <cstring>

namespace pse_ui {
namespace {

// Windows-1252 0x80..0x9F (0 = unused byte).
constexpr uint16_t kCp1252High[32] = {0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                      0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D,
                                      0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E,
                                      0x0178};

// Unicode code point -> its 1252 byte, or -1.
int Cp1252(uint32_t cp) {
    if (cp < 0x80 || (cp >= 0xA0 && cp <= 0xFF)) return static_cast<int>(cp);
    for (int i = 0; i < 32; ++i)
        if (kCp1252High[i] && kCp1252High[i] == cp) return 0x80 + i;
    return -1;
}

// Latin Extended-A (U+0100..U+017F) without accents.
constexpr char kLatinExtA[] =
    "AaAaAa" "CcCcCcCc" "DdDd" "EeEeEeEeEe" "GgGgGgGg" "HhHh" "IiIiIiIiIi" "Ii" "Jj" "Kkk" "LlLlLlLlLl" "NnNnNnnNn"
    "OoOoOo" "Oo" "RrRrRr" "SsSsSsSs" "TtTtTt" "UuUuUuUuUuUu" "Ww" "YyY" "ZzZzZz" "s";
static_assert(sizeof kLatinExtA == 129, "one letter per code point U+0100..U+017F");

// The plain (ASCII) form of a character, or null.
const char* Plain(uint32_t cp) {
    static char one[2];
    if (cp >= 0xC0 && cp <= 0xFF) {
        static const char* const kLatin1[64] = {
            "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
            "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "TH", "ss",
            "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
            "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"};
        return kLatin1[cp - 0xC0];
    }
    switch (cp) {
        case 0x152: return "OE";
        case 0x153: return "oe";
        case 0xA0: return " ";
        case 0xA9: return "(C)";
        case 0xAE: return "(R)";
        case 0xAB: return "<<";
        case 0xBB: return ">>";
        case 0xB4: case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return "\"";
        case 0x2039: return "<";
        case 0x203A: return ">";
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
        case 0x2026: return "...";
        case 0x2022: case 0xB7: return "*";
        case 0x20AC: return "EUR";
        case 0x2122: return "TM";
        case 0x192: return "f";
        case 0x2C6: return "^";
        case 0x2DC: return "~";
        default: break;
    }
    if (cp >= 0x100 && cp <= 0x17F) {
        one[0] = kLatinExtA[cp - 0x100];
        one[1] = 0;
        return one;
    }
    if (cp >= 0x2000 && cp <= 0x200A) return " ";   // typographic spaces
    return nullptr;
}

uint32_t Upper(uint32_t cp) {
    if (cp >= 'a' && cp <= 'z') return cp - 0x20;
    if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 0x20;
    if (cp == 0xFF) return 0x178;
    if (cp >= 0x100 && cp <= 0x17F && kLatinExtA[cp - 0x100] >= 'a' && kLatinExtA[cp - 0x100] <= 'z' && cp != 0x138 &&
        cp != 0x149 && cp != 0x17F && cp != 0x131) {
        // pairs: upper case is the code point before when it is a capital
        if (cp > 0x100 && kLatinExtA[cp - 0x101] >= 'A' && kLatinExtA[cp - 0x101] <= 'Z') return cp - 1;
    }
    return cp;
}

char UpperAscii(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 0x20) : c; }

// Next code point; false at the end. A malformed sequence = U+FFFD, one byte consumed.
bool Decode(const unsigned char*& p, uint32_t& cp) {
    if (!*p) return false;
    const unsigned char c = *p;
    int n = 0;
    if (c < 0x80) {
        cp = c;
        ++p;
        return true;
    }
    if ((c & 0xE0) == 0xC0) cp = c & 0x1F, n = 1;
    else if ((c & 0xF0) == 0xE0) cp = c & 0x0F, n = 2;
    else if ((c & 0xF8) == 0xF0) cp = c & 0x07, n = 3;
    else {
        cp = 0xFFFD;
        ++p;
        return true;
    }
    for (int i = 1; i <= n; ++i)
        if ((p[i] & 0xC0) != 0x80) {
            cp = 0xFFFD;
            ++p;
            return true;
        }
    for (int i = 1; i <= n; ++i) cp = cp << 6 | (p[i] & 0x3F);
    static const uint32_t kMin[4] = {0, 0x80, 0x800, 0x10000};
    if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    p += n + 1;
    return true;
}
}  // namespace

std::string ToGameText(const char* utf8, bool upper, HasGlyph has, void* ctx, size_t maxBytes) {
    std::string out;
    if (!utf8) return out;
    auto drawable = [&](unsigned char b) { return b == ' ' || !has || has(b, ctx); };
    auto put = [&](char c) {
        if (maxBytes && out.size() >= maxBytes) return false;
        out += c;
        return true;
    };
    const auto* p = reinterpret_cast<const unsigned char*>(utf8);
    uint32_t cp;
    while (Decode(p, cp)) {
        if (cp == '\n') break;   // one line
        if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp < 0xA0)) cp = ' ';   // icon slots / C1 controls
        if (upper) cp = Upper(cp);
        const int b = Cp1252(cp);
        if (b >= 0 && drawable(static_cast<unsigned char>(b))) {
            if (!put(static_cast<char>(b))) break;
            continue;
        }
        bool ok = true;
        if (const char* plain = Plain(cp)) {
            std::string alt;
            for (const char* s = plain; *s; ++s) alt += upper ? UpperAscii(*s) : *s;
            for (char c : alt)
                if (!drawable(static_cast<unsigned char>(c))) ok = false;
            if (ok) {
                for (char c : alt)
                    if (!put(c)) return out;
                continue;
            }
        }
        if (!put('?')) break;
    }
    return out;
}

// ---- settings page ----
namespace {
bool Shown(const PSESettingDesc* d) {
    return d && !(d->flags & PSE_SETTING_HIDDEN) && d->type >= PSE_SETTING_BOOL && d->type <= PSE_SETTING_STRING;
}
}  // namespace

std::vector<Row> BuildPage(const PSESettingsInterface* settings, PSEPluginHandle owner) {
    std::vector<Row> rows;
    if (!settings || !owner) return rows;
    std::string group;
    const PSESettingDesc* d = nullptr;
    for (uint32_t i = 0; settings->Describe(owner, i, &d, nullptr) == PSE_OK; ++i) {
        if (!Shown(d)) continue;
        const std::string g = d->group ? d->group : "";
        if (!g.empty() && g != group) {
            Row h;
            h.kind = RowKind::Heading;
            h.owner = owner;
            h.label = g;
            rows.push_back(h);
        }
        group = g;
        Row r;
        r.owner = owner;
        r.key = d->key ? d->key : "";
        r.label = d->label && *d->label ? d->label : r.key;
        r.help = d->help ? d->help : "";
        r.flags = d->flags;
        r.def = d->defaultInt;
        switch (d->type) {
            case PSE_SETTING_BOOL: r.kind = RowKind::Bool, r.min = 0, r.max = 1; break;
            case PSE_SETTING_INT:
                r.kind = RowKind::Int, r.min = d->minInt, r.max = d->maxInt, r.step = d->step > 0 ? d->step : 1;
                break;
            case PSE_SETTING_CHOICE:
                r.kind = RowKind::Choice;
                for (const char* const* c = d->choices; c && *c; ++c) r.choices.push_back(*c);
                r.min = 0, r.max = r.choices.empty() ? 0 : static_cast<int32_t>(r.choices.size()) - 1;
                break;
            case PSE_SETTING_COLOUR: r.kind = RowKind::Colour, r.min = 0, r.max = 0xFFFFFF; break;
            default: r.kind = RowKind::String; break;
        }
        rows.push_back(std::move(r));
    }
    return rows;
}

size_t VisibleSettings(const PSESettingsInterface* settings, PSEPluginHandle owner) {
    size_t n = 0;
    if (!settings || !owner) return 0;
    const PSESettingDesc* d = nullptr;
    for (uint32_t i = 0; settings->Describe(owner, i, &d, nullptr) == PSE_OK; ++i)
        if (Shown(d)) ++n;
    return n;
}

bool Editable(const Row& row) {
    return row.kind == RowKind::Bool || row.kind == RowKind::Int || row.kind == RowKind::Choice ||
           row.kind == RowKind::Colour;
}

bool Spinner(const Row& row) {
    return (row.kind == RowKind::Bool || row.kind == RowKind::Int || row.kind == RowKind::Choice) && row.max > row.min;
}

int32_t StepValue(const Row& row, int32_t value, int dir) {
    if (!dir) return value;
    switch (row.kind) {
        case RowKind::Bool: return value ? 0 : 1;
        case RowKind::Int: {
            const int64_t v = static_cast<int64_t>(value) + static_cast<int64_t>(dir < 0 ? -row.step : row.step);
            return static_cast<int32_t>(v < row.min ? row.min : v > row.max ? row.max : v);
        }
        case RowKind::Choice: {
            const int32_t n = row.max - row.min + 1;
            if (n <= 1) return row.min;
            int32_t v = value - row.min + (dir < 0 ? -1 : 1);
            v = ((v % n) + n) % n;
            return row.min + v;
        }
        default: return value;
    }
}

std::string ValueText(const Row& row, int32_t value, const std::string& text) {
    char b[32];
    switch (row.kind) {
        case RowKind::Bool: return value ? "ON" : "OFF";
        case RowKind::Int: std::snprintf(b, sizeof b, "%d", value); return b;
        case RowKind::Choice:
            return value >= 0 && value < static_cast<int32_t>(row.choices.size()) ? row.choices[value] : std::string("?");
        case RowKind::Colour: std::snprintf(b, sizeof b, "#%06X", static_cast<unsigned>(value) & 0xFFFFFFu); return b;
        case RowKind::String: return text;
        default: return std::string();
    }
}

const char* WhenMarker(uint32_t flags) {
    if (flags & PSE_SETTING_LIVE) return "";
    if (flags & PSE_SETTING_NEXT_LEVEL) return " +";
    return " *";
}

bool NeedsRestart(uint32_t flags) { return !(flags & (PSE_SETTING_LIVE | PSE_SETTING_NEXT_LEVEL)); }

// ---- PLUGINS list ----
std::vector<PluginEntry> BuildPluginList(const std::vector<pse::PluginLoadResult>& results,
                                         const std::vector<std::string>& hostNames,
                                         size_t (*settingsOf)(PSEPluginHandle, void*), void* ctx) {
    std::vector<PluginEntry> out;
    for (const pse::PluginLoadResult& r : results) {
        PluginEntry e;
        e.name = r.name;
        e.displayName = r.displayName.empty() ? r.name : r.displayName;
        char v[32];
        std::snprintf(v, sizeof v, "%u.%u.%u", PSE_VERSION_MAJOR(r.version), PSE_VERSION_MINOR(r.version),
                      PSE_VERSION_PATCH(r.version));
        e.version = r.version ? v : "";
        e.state = r.state;
        e.reason = r.reason;
        e.handle = r.handle;
        for (const std::string& h : hostNames)
            if (h == r.name) e.host = true;
        if (r.state == pse::PluginState::Loaded && !e.host && r.handle && settingsOf) e.settings = settingsOf(r.handle, ctx);
        out.push_back(std::move(e));
    }
    return out;
}

const char* StateText(pse::PluginState state) {
    switch (state) {
        case pse::PluginState::Loaded: return "LOADED";
        case pse::PluginState::Refused: return "REFUSED";
        case pse::PluginState::Failed: return "FAILED";
        case pse::PluginState::Unloaded: return "UNLOADED";
    }
    return "?";
}

bool LauncherSetting(const PSESettingDesc& d) {
    return (d.flags & PSE_SETTING_LAUNCHER) && !(d.flags & PSE_SETTING_HIDDEN) &&
           (d.type == PSE_SETTING_BOOL || d.type == PSE_SETTING_INT || d.type == PSE_SETTING_CHOICE);
}

// ---- text hashes ----
uint32_t TextTable::Register(PSEPluginHandle plugin, const std::string& key, const std::string& text) {
    if (!plugin || key.empty()) return 0;
    const auto k = std::make_pair(plugin, key);
    auto it = keys_.find(k);
    if (it != keys_.end()) {
        texts_[it->second] = text;
        return it->second;
    }
    uint32_t& n = next_[plugin];
    if (n >= kPerPlugin) return 0;
    ++n;
    const uint32_t hash = kBase | (plugin & 0xFFFu) << 12 | (n & 0xFFFu);
    keys_.emplace(k, hash);
    texts_[hash] = text;
    return hash;
}

uint32_t TextTable::Hash(PSEPluginHandle plugin, const std::string& key) const {
    auto it = keys_.find(std::make_pair(plugin, key));
    return it == keys_.end() ? 0 : it->second;
}

const std::string* TextTable::Find(uint32_t hash) const {
    auto it = texts_.find(hash);
    return it == texts_.end() ? nullptr : &it->second;
}

}  // namespace pse_ui

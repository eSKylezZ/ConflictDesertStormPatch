// Mission patches (docs/sdk/reference/mission-patches.md): parsing a <Stem>.patch.lua and applying its edit blocks to the
// readable mission (TranslateReadable's bundle). Used by DesertStormFix's dsfix.missions (missionsource.cpp) and by
// `pse_build missions --readable --patch`.
#include <cstdlib>
#include <cstring>

#include "mission2lua.h"

namespace pse::m2l {

namespace {

std::vector<std::string> SplitLines(const std::string& t) {
    std::vector<std::string> out;
    size_t at = 0;
    for (;;) {
        const size_t e = t.find('\n', at);
        if (e == std::string::npos) {
            out.push_back(t.substr(at));
            return out;
        }
        out.push_back(t.substr(at, e - at));
        at = e + 1;
    }
}

std::string JoinLines(const std::vector<std::string>& v) {
    std::string s;
    for (size_t k = 0; k < v.size(); ++k) {
        if (k) s += '\n';
        s += v[k];
    }
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

}  // namespace

std::string NormalizePatchText(const std::vector<uint8_t>& raw) {
    std::string t;
    t.reserve(raw.size());
    for (size_t k = 0; k < raw.size(); ++k)
        if (raw[k] != '\r' || k + 1 >= raw.size() || raw[k + 1] != '\n') t += static_cast<char>(raw[k]);
    if (t.compare(0, 3, "\xEF\xBB\xBF") == 0) t.erase(0, 3);   // a UTF-8 BOM
    return t;
}

bool ParseMissionPatch(const std::string& dllName, MissionPatch* p, std::string* why) {
    p->edits.clear();
    const std::vector<std::string> lines = SplitLines(p->text);
    bool forThis = false, haveVersion = false;
    int version = -1;
    for (const std::string& raw : lines) {
        const std::string l = Trim(raw);
        if (l.compare(0, 2, "--") != 0) break;
        if (l.compare(0, 11, "--@patches ") == 0) forThis = _stricmp(Trim(l.substr(11)).c_str(), dllName.c_str()) == 0;
        if (l.compare(0, 11, "--@readable") == 0) {
            haveVersion = true;
            version = std::atoi(l.c_str() + 11);
        }
    }
    if (!forThis) {
        *why = "its header has no \"--@patches " + dllName + "\" line";
        return false;
    }
    if (!haveVersion || version != ReadableVersion()) {
        *why = "written for readable missions version " + (haveVersion ? std::to_string(version) : std::string("?")) +
               ", this one makes version " + std::to_string(ReadableVersion()) + " (header line --@readable)";
        return false;
    }
    for (size_t k = 0; k < lines.size(); ++k) {
        const std::string l = Trim(lines[k]);
        if (l.compare(0, 3, "--[") != 0) continue;
        size_t eq = 3;
        while (eq < l.size() && l[eq] == '=') ++eq;
        if (l.compare(eq, 6, "[@edit") != 0) continue;
        const std::string close = "]" + std::string(eq - 3, '=') + "]";
        PatchEdit e;
        e.line = static_cast<int>(k + 1);
        bool to = false, closed = false;
        for (++k; k < lines.size(); ++k) {
            const std::string& c = lines[k];
            const size_t at = c.find(close);
            const std::string content = at == std::string::npos ? c : c.substr(0, at);
            if (Trim(content) == "--@to") {
                to = true;
            } else if (at == std::string::npos || !Trim(content).empty()) {
                (to ? e.to : e.from).push_back(content);
            }
            if (at != std::string::npos) {
                closed = true;
                break;
            }
        }
        if (!closed || !to || e.from.empty()) {
            *why = "the edit block at line " + std::to_string(e.line) +
                   (!closed ? " isn't closed" : !to ? " has no --@to line" : " has nothing before --@to");
            return false;
        }
        p->edits.push_back(e);
    }
    return true;
}

bool ApplyMissionPatchEdits(std::string* bundle, const MissionPatch& p, std::string* why) {
    std::vector<std::string> work = SplitLines(*bundle);
    for (size_t n = 0; n < p.edits.size(); ++n) {
        const PatchEdit& e = p.edits[n];
        std::vector<size_t> hits;
        for (size_t i = 0; i + e.from.size() <= work.size(); ++i) {
            bool same = true;
            for (size_t j = 0; j < e.from.size() && same; ++j) same = work[i + j] == e.from[j];
            if (same) hits.push_back(i);
        }
        if (hits.size() != 1) {
            *why = "the edit block at line " + std::to_string(e.line) + " (\"" + Trim(e.from[0]) + "\" ...) matches " +
                   std::to_string(hits.size()) +
                   " places in the mission, not one - written for another game version, or another patch changed those lines";
            return false;
        }
        work.erase(work.begin() + static_cast<ptrdiff_t>(hits[0]),
                   work.begin() + static_cast<ptrdiff_t>(hits[0] + e.from.size()));
        work.insert(work.begin() + static_cast<ptrdiff_t>(hits[0]), e.to.begin(), e.to.end());
    }
    *bundle = JoinLines(work);
    return true;
}

bool HandOutMissionEnv(std::string* bundle) {
    static const char* const kStart = "-- ======================== scripts/start.lua ========================";
    std::vector<std::string> lines = SplitLines(*bundle);
    for (size_t k = 0; k < lines.size(); ++k)
        if (lines[k] == kStart) {
            lines.insert(lines.begin() + static_cast<ptrdiff_t>(k),
                         "_G.__pse_mission_env = _ENV   -- (DesertStormFix: mission patches run in here)");
            *bundle = JoinLines(lines);
            return true;
        }
    return false;
}

const char* MissionPatchPrelude() {
    return R"lua(
local function hash(s)
  local x = 1
  for i = 1, #s do
    local c = s:byte(i)
    for b = 0, 7 do
      local bit = ((c >> b) ~ x ~ (x >> 1) ~ (x >> 21) ~ (x >> 31)) & 1
      x = ((x << 1) | bit) & 0xFFFFFFFF
    end
  end
  return x
end
text_hash = function(key) return hash(string.lower(key)) end
movie_hash = function(key) return hash(string.upper(key)) end
TEXT = setmetatable({}, {__index = function(_, k) return text_hash(k) end})
MOVIE = setmetatable({}, {__index = function(_, k) return movie_hash(k) end})
X = X86RT
)lua";
}

}  // namespace pse::m2l

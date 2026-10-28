// Mods' .envpatch / .rmdpatch files of one output merged (scenepatch.h). SceneScriptIds lives in placed.cpp (the
// scene reader).
#include "scenepatch.h"

#include <algorithm>
#include <cstdio>
#include <map>

namespace pse::records {
namespace {

std::vector<std::string> Lines(const std::string& text) {   // as mods.cpp SplitLines: "\n", a trailing "\r" dropped
    std::vector<std::string> out;
    for (size_t a = 0; a < text.size();) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string line = text.substr(a, b - a);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        a = b + 1;
    }
    return out;
}

struct Hunk {
    unsigned start = 0, del = 0;
    std::vector<std::string> lines;
    size_t patch = 0;   // index into the patches (mod order)
};

bool ParseEnv(const std::string& text, std::string* base, std::vector<Hunk>* hunks, std::string* error) {
    const std::vector<std::string> p = Lines(text);
    if (p.empty() || p[0].compare(0, 5, "base ") != 0) return *error = "no \"base\" line", false;
    *base = p[0].substr(5);
    unsigned at = 0;   // the end of the previous hunk: hunks come in order and don't overlap
    for (size_t i = 1; i < p.size(); ++i) {
        if (p[i].empty() || p[i][0] != '@') continue;
        Hunk h;
        unsigned ins = 0;
        if (sscanf_s(p[i].c_str(), "@ %u %u %u", &h.start, &h.del, &ins) != 3) return *error = "bad hunk " + p[i], false;
        if (h.start < at) return *error = "hunks out of order at " + p[i], false;
        if (i + ins >= p.size() + 1) return *error = "hunk " + p[i] + " is cut short", false;
        for (unsigned k = 0; k < ins; ++k) h.lines.push_back(p[++i]);
        at = h.start + h.del;
        hunks->push_back(std::move(h));
    }
    return true;
}

bool Overlap(const Hunk& a, const Hunk& b) {
    if (a.del && b.del) return a.start < b.start + b.del && b.start < a.start + a.del;
    if (!a.del && b.del) return b.start < a.start && a.start < b.start + b.del;   // an insert inside b's change
    if (a.del && !b.del) return a.start < b.start && b.start < a.start + a.del;
    return false;   // two inserts: both kept
}
bool Same(const Hunk& a, const Hunk& b) { return a.start == b.start && a.del == b.del && a.lines == b.lines; }
std::string Where(const Hunk& h) {
    return h.del ? "lines " + std::to_string(h.start + 1) + "-" + std::to_string(h.start + h.del)
                 : "at line " + std::to_string(h.start + 1);
}

// keeps the patches whose base is the last patch's (the later mod wins a different base)
template <class T>
void SameBase(const std::string& name, const std::vector<PatchText>& patches, std::vector<T>* parsed,
              std::vector<std::string>* report) {
    if (parsed->empty()) return;
    const std::string last = parsed->back().base;
    std::vector<T> keep;
    for (T& p : *parsed) {
        if (p.base == last) {
            keep.push_back(std::move(p));
            continue;
        }
        report->push_back("conflict: " + name + ": " + patches[p.index].mod + "'s patch is of " + p.base + ", " +
                          patches[parsed->back().index].mod + "'s of " + last + " (" + patches[parsed->back().index].mod +
                          " wins)");
    }
    parsed->swap(keep);
}

}  // namespace

bool MergeEnvPatches(const std::string& name, const std::vector<PatchText>& patches, std::string* merged,
                     std::vector<std::string>* report) {
    struct Parsed {
        size_t index;
        std::string base;
        std::vector<Hunk> hunks;
    };
    std::vector<Parsed> parsed;
    for (size_t i = 0; i < patches.size(); ++i) {
        Parsed p{i, {}, {}};
        std::string error;
        if (!ParseEnv(patches[i].text, &p.base, &p.hunks, &error)) {
            report->push_back("[fail] " + patches[i].mod + ": " + name + " patch not used - " + error);
            continue;
        }
        for (Hunk& h : p.hunks) h.patch = i;
        parsed.push_back(std::move(p));
    }
    merged->clear();
    if (parsed.empty()) return false;
    SameBase(name, patches, &parsed, report);
    std::vector<Hunk> kept;   // in the order they were accepted (mod order)
    for (const Parsed& p : parsed)
        for (const Hunk& h : p.hunks) {
            if (std::any_of(kept.begin(), kept.end(), [&](const Hunk& k) { return Same(k, h); })) continue;
            for (auto it = kept.begin(); it != kept.end();) {
                if (Overlap(*it, h)) {
                    report->push_back("conflict: " + name + " " + Where(h) + ": " + patches[it->patch].mod + " / " +
                                      patches[h.patch].mod + " (" + patches[h.patch].mod + " wins)");
                    it = kept.erase(it);
                } else {
                    ++it;
                }
            }
            kept.push_back(h);
        }
    // at one start line the inserts come first (mod order), then the change
    std::stable_sort(kept.begin(), kept.end(), [](const Hunk& a, const Hunk& b) {
        return a.start != b.start ? a.start < b.start : (a.del != 0) < (b.del != 0);
    });
    *merged = "base " + parsed.back().base + "\n";
    for (const Hunk& h : kept) {
        *merged += "@ " + std::to_string(h.start) + " " + std::to_string(h.del) + " " + std::to_string(h.lines.size()) + "\n";
        for (const std::string& l : h.lines) *merged += l + "\n";
    }
    return true;
}

bool MergeRmdPatches(const std::string& name, const std::vector<PatchText>& patches, std::string* merged,
                     std::vector<std::string>* report) {
    struct Parsed {
        size_t index;
        std::string base;
        unsigned count = 0;
        std::vector<std::pair<unsigned, std::string>> rows;
    };
    std::vector<Parsed> parsed;
    for (size_t i = 0; i < patches.size(); ++i) {
        const std::vector<std::string> p = Lines(patches[i].text);
        Parsed r{i, {}, 0, {}};
        if (p.size() < 2 || p[0].compare(0, 5, "base ") != 0 || sscanf_s(p[1].c_str(), "count %u", &r.count) != 1) {
            report->push_back("[fail] " + patches[i].mod + ": " + name + " patch not used - no \"base\" / \"count\" line");
            continue;
        }
        r.base = p[0].substr(5);
        for (size_t k = 2; k < p.size(); ++k) {
            unsigned idx = 0;
            if (sscanf_s(p[k].c_str(), "%u", &idx) == 1) r.rows.push_back({idx, p[k]});
        }
        parsed.push_back(std::move(r));
    }
    merged->clear();
    if (parsed.empty()) return false;
    SameBase(name, patches, &parsed, report);
    unsigned count = 0;
    std::map<unsigned, std::pair<size_t, std::string>> rows;   // marker -> (patch, row)
    for (const Parsed& p : parsed) {
        count = std::max(count, p.count);
        for (const auto& [idx, row] : p.rows) {
            auto it = rows.find(idx);
            if (it != rows.end() && it->second.first != p.index && it->second.second != row)
                report->push_back("conflict: " + name + " marker " + std::to_string(idx) + ": " +
                                  patches[it->second.first].mod + " / " + patches[p.index].mod + " (" +
                                  patches[p.index].mod + " wins)");
            rows[idx] = {p.index, row};
        }
    }
    *merged = "base " + parsed.back().base + "\ncount " + std::to_string(count) + "\n";
    for (const auto& [idx, row] : rows) *merged += row.second + "\n";
    return true;
}

}  // namespace pse::records

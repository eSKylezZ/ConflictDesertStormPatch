// See pse_saves_format.h.
#include "core/pse_saves_format.h"

#include <cstdio>
#include <cstring>

namespace pse_saves {
namespace {

struct Out {
    std::vector<uint8_t> b;
    void U8(uint8_t v) { b.push_back(v); }
    void U32(uint32_t v) {
        uint8_t x[4];
        std::memcpy(x, &v, 4);
        b.insert(b.end(), x, x + 4);
    }
    void I32(int v) { U32(static_cast<uint32_t>(v)); }
    void Str(const std::string& s) {
        const size_t n = s.size() < 0xFFFF ? s.size() : 0xFFFF;
        b.push_back(static_cast<uint8_t>(n & 0xFF));
        b.push_back(static_cast<uint8_t>(n >> 8));
        b.insert(b.end(), s.begin(), s.begin() + static_cast<ptrdiff_t>(n));
    }
};

struct In {
    const uint8_t* p;
    size_t left;
    bool ok = true;
    bool Take(void* out, size_t n) {
        if (!ok || left < n) return ok = false;
        std::memcpy(out, p, n);
        p += n;
        left -= n;
        return true;
    }
    uint8_t U8() {
        uint8_t v = 0;
        Take(&v, 1);
        return v;
    }
    uint32_t U32() {
        uint32_t v = 0;
        Take(&v, 4);
        return v;
    }
    int I32() { return static_cast<int>(U32()); }
    std::string Str() {
        uint8_t n[2] = {};
        if (!Take(n, 2)) return {};
        const size_t len = n[0] | (n[1] << 8);
        if (left < len) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(p), len);
        p += len;
        left -= len;
        return s;
    }
    // A count that can't possibly fit in what is left (each element >= min bytes) = damage.
    bool Count(uint32_t n, size_t min) { return ok = ok && n <= left / (min ? min : 1); }
};

}  // namespace

std::vector<uint8_t> EncodeKits(const std::vector<SavedWeapon>& kits) {
    Out o;
    o.U32(static_cast<uint32_t>(kits.size()));
    for (const SavedWeapon& w : kits) {
        o.I32(w.role);
        o.Str(w.id);
        o.I32(w.reserve);
        o.I32(w.magazine);
        o.U8(w.held ? 1 : 0);
    }
    return o.b;
}

std::vector<uint8_t> EncodeGear(const SavedGear& gear) {
    Out o;
    o.U32(static_cast<uint32_t>(gear.size()));
    for (const auto& [role, pieces] : gear) {
        o.I32(role);
        o.U32(static_cast<uint32_t>(pieces.size()));
        for (const SavedGearPiece& g : pieces) {
            o.Str(g.connector);
            o.Str(g.model);
        }
    }
    return o.b;
}

std::vector<uint8_t> EncodeVehicles(const SavedVehicles& swaps) {
    Out o;
    o.U32(static_cast<uint32_t>(swaps.size()));
    for (const auto& [from, to] : swaps) {
        o.Str(from);
        o.Str(to);
    }
    return o.b;
}

bool DecodeKits(const uint8_t* data, size_t len, std::vector<SavedWeapon>* out) {
    In in{data, data ? len : 0};
    const uint32_t n = in.U32();
    if (!in.Count(n, 15)) return false;
    for (uint32_t i = 0; i < n && in.ok; ++i) {
        SavedWeapon w;
        w.role = in.I32();
        w.id = in.Str();
        w.reserve = in.I32();
        w.magazine = in.I32();
        w.held = in.U8() != 0;
        if (in.ok) out->push_back(std::move(w));
    }
    return in.ok;
}

bool DecodeGear(const uint8_t* data, size_t len, SavedGear* out) {
    In in{data, data ? len : 0};
    const uint32_t roles = in.U32();
    if (!in.Count(roles, 8)) return false;
    for (uint32_t r = 0; r < roles && in.ok; ++r) {
        const int role = in.I32();
        const uint32_t n = in.U32();
        if (!in.Count(n, 4)) break;
        std::vector<SavedGearPiece> pieces;
        for (uint32_t i = 0; i < n && in.ok; ++i) {
            SavedGearPiece g;
            g.connector = in.Str();
            g.model = in.Str();
            if (in.ok) pieces.push_back(std::move(g));
        }
        if (in.ok) (*out)[role] = std::move(pieces);
    }
    return in.ok;
}

bool DecodeVehicles(const uint8_t* data, size_t len, SavedVehicles* out) {
    In in{data, data ? len : 0};
    const uint32_t n = in.U32();
    if (!in.Count(n, 4)) return false;
    for (uint32_t i = 0; i < n && in.ok; ++i) {
        std::string from = in.Str(), to = in.Str();
        if (in.ok) (*out)[from] = to;
    }
    return in.ok;
}

Sidecar ParseSidecar(const std::string& text) {
    Sidecar s;
    size_t a = 0;
    while (a < text.size()) {
        size_t e = text.find('\n', a);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(a, e - a);
        a = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.size() > 256) continue;   // (the old reader's line buffer)
        char x[128] = {}, y[128] = {};
        int role = -1;
        if (line.size() > 2 && line[0] == 'V' && line[1] == '|') {
            if (std::sscanf(line.c_str(), "V|%63[^|]|%63[^\r\n]", x, y) == 2) s.vehicles[x] = y;
            continue;
        }
        if (line.size() > 2 && line[0] == 'G' && line[1] == '|') {
            if (std::sscanf(line.c_str(), "G|%d|%63[^|]|%63[^\r\n]", &role, x, y) == 3)
                s.gear[role].push_back({x, std::strcmp(y, "-") ? y : ""});
            continue;
        }
        SavedWeapon w;
        int held = 0;
        if (std::sscanf(line.c_str(), "%d|%127[^|]|%d|%d|%d", &w.role, x, &w.reserve, &w.magazine, &held) == 5) {
            w.id = x;
            w.held = held != 0;
            s.kits.push_back(std::move(w));
        }
    }
    return s;
}

std::vector<uint8_t> SidecarToCosave(const Sidecar& s) {
    struct Chunk {
        const char* plugin;
        uint32_t tag;
        std::vector<uint8_t> data;
    };
    std::vector<Chunk> chunks;
    if (!s.kits.empty()) chunks.push_back({"dsfix.loadout", kKitsTag, EncodeKits(s.kits)});
    if (!s.gear.empty()) chunks.push_back({"dsfix.gear", kGearTag, EncodeGear(s.gear)});
    if (!s.vehicles.empty()) chunks.push_back({"dsfix.vehicles", kVehiclesTag, EncodeVehicles(s.vehicles)});
    Out o;
    o.b = {'P', 'S', 'E', 1};
    o.U32(static_cast<uint32_t>(chunks.size()));
    for (const Chunk& c : chunks) {
        const size_t n = std::strlen(c.plugin);
        o.U8(static_cast<uint8_t>(n));
        o.b.insert(o.b.end(), c.plugin, c.plugin + n);
        o.U32(c.tag);
        o.U32(kChunkVersion);
        o.U32(static_cast<uint32_t>(c.data.size()));
        o.b.insert(o.b.end(), c.data.begin(), c.data.end());
    }
    return o.b;
}

namespace {
bool EndsWithSav(const std::string& p) {
    return p.size() >= 4 && _stricmp(p.c_str() + p.size() - 4, ".sav") == 0;
}
}  // namespace

std::string CoSavePathFor(const std::string& savePath) {
    if (EndsWithSav(savePath)) return savePath.substr(0, savePath.size() - 4) + ".pse";
    return savePath + ".pse";
}

std::string SidecarPathFor(const std::string& savePath) { return savePath + ".dsfix"; }

}  // namespace pse_saves

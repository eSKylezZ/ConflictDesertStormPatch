// Online plugin-set check (docs/sdk/online-compat.md, DESIGN.md 8): what a PC sends about itself when an online peer
// connects, the text encoding (lines that fit the game-command channel), and the comparison. The comparison is a pure
// function (pse_tests: test_compat.cpp); the host (DesertStormFix online.cpp / onlinecompat.cpp) does the sending,
// shows the differences and refuses the join. Pure (no loader state): DesertStormFix's online plugin compiles this
// file too; LocalPlugins() - the running loader's list - is compat_local.cpp.
#include <windows.h>

#include <bcrypt.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>

#include "loader.h"

namespace pse::compat {
namespace {

std::string Hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%X", v);
    return b;
}

bool ParseHex32(const std::string& s, uint32_t* out) {
    if (s.empty() || s.size() > 8) return false;
    uint32_t v = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return false;
        v = v << 4 | static_cast<uint32_t>(d);
    }
    *out = v;
    return true;
}

bool Plain(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::strchr("._-+()!~#,'&[]", c);
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t p = s.find(sep, start);
        out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string Short(const std::string& hash) { return hash.size() > 8 ? hash.substr(0, 8) : hash; }

}  // namespace

bool Simulation(uint32_t flags) {
    // DESIGN.md 4.1: neither flag = SIMULATION (the safe default online); SIMULATION wins over CLIENT_ONLY.
    return (flags & PSE_PLUGIN_SIMULATION) || !(flags & PSE_PLUGIN_CLIENT_ONLY);
}

std::string Escape(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if (Plain(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", c);
            out += b;
        }
    }
    return out.empty() ? "%" : out;   // "%" alone = the empty string (tokens can't be empty)
}

bool Unescape(const std::string& s, std::string* out) {
    out->clear();
    if (s == "%") return true;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '%') {
            out->push_back(s[i]);
            continue;
        }
        uint32_t v = 0;
        if (i + 2 >= s.size() || !ParseHex32(s.substr(i + 1, 2), &v)) return false;
        out->push_back(static_cast<char>(v));
        i += 2;
    }
    return true;
}

std::vector<std::string> Encode(const Set& s, uint32_t id, size_t maxLine) {
    if (maxLine < 80) maxLine = 80;
    std::vector<std::string> lines;
    const std::string head = "PSEC " + Hex32(id) + " ";
    lines.push_back(head + "B " + Hex32(s.protocol) + " " + Hex32(s.apiVersion) + " " + Escape(s.gameId) + " " +
                    Escape(s.host) + " " + Escape(s.hostFingerprint));
    auto pack = [&](char kind, const std::vector<std::string>& entries) {
        std::string line;
        for (const std::string& e : entries) {
            if (!line.empty() && line.size() + 1 + e.size() > maxLine) {
                lines.push_back(line);
                line.clear();
            }
            line += line.empty() ? head + kind + " " + e : " " + e;
        }
        if (!line.empty()) lines.push_back(line);
    };
    std::vector<std::string> entries;
    for (const PluginEntry& p : s.plugins)
        entries.push_back(Escape(p.name) + ":" + Hex32(p.version) + ":" + Hex32(p.flags) + ":" +
                          (p.hash.empty() ? std::string("-") : Escape(p.hash)));
    pack('P', entries);
    entries.clear();
    for (const ModEntry& m : s.mods) entries.push_back(Escape(m.name) + ":" + Escape(m.hash));
    pack('M', entries);
    lines.push_back(head + "E " + std::to_string(s.plugins.size()) + " " + std::to_string(s.mods.size()));
    return lines;
}

Assembler::Result Assembler::Feed(const std::string& line) {
    if (line.compare(0, 5, "PSEC ") != 0) return NotOurs;
    std::vector<std::string> t = Split(line, ' ');
    auto bad = [&](const char* why) {
        error = why;
        building_ = false;
        return Bad;
    };
    if (t.size() < 3 || t[2].size() != 1) return bad("malformed line");
    uint32_t id;
    if (!ParseHex32(t[1], &id)) return bad("malformed id");
    const char kind = t[2][0];
    if (kind == 'B') {
        if (t.size() != 8) return bad("malformed B line");
        Set s;
        std::string fp;
        if (!ParseHex32(t[3], &s.protocol) || !ParseHex32(t[4], &s.apiVersion) || !Unescape(t[5], &s.gameId) ||
            !Unescape(t[6], &s.host) || !Unescape(t[7], &s.hostFingerprint))
            return bad("malformed B line");
        partial_ = std::move(s);
        id_ = id;
        building_ = true;
        return Pending;
    }
    if (!building_ || id != id_) return Pending;   // the rest of an older / unstarted list: ignored
    if (kind == 'P') {
        for (size_t i = 3; i < t.size(); ++i) {
            std::vector<std::string> f = Split(t[i], ':');
            PluginEntry e;
            if (f.size() != 4 || !Unescape(f[0], &e.name) || !ParseHex32(f[1], &e.version) || !ParseHex32(f[2], &e.flags))
                return bad("malformed plugin entry");
            if (f[3] != "-" && !Unescape(f[3], &e.hash)) return bad("malformed plugin hash");
            partial_.plugins.push_back(std::move(e));
        }
        return Pending;
    }
    if (kind == 'M') {
        for (size_t i = 3; i < t.size(); ++i) {
            std::vector<std::string> f = Split(t[i], ':');
            ModEntry m;
            if (f.size() != 2 || !Unescape(f[0], &m.name) || !Unescape(f[1], &m.hash)) return bad("malformed mod entry");
            partial_.mods.push_back(std::move(m));
        }
        return Pending;
    }
    if (kind == 'E') {
        if (t.size() != 5) return bad("malformed E line");
        if (t[3] != std::to_string(partial_.plugins.size()) || t[4] != std::to_string(partial_.mods.size()))
            return bad("incomplete list (entries missing)");
        set = std::move(partial_);
        partial_ = Set{};
        building_ = false;
        error.clear();
        return Done;
    }
    return bad("unknown line kind");
}

std::vector<std::string> Compare(const Set& local, const Set& remote, const std::string& who) {
    std::vector<std::string> out;
    if (local.protocol != remote.protocol) {
        out.push_back("online check version " + std::to_string(local.protocol) + " here vs " +
                      std::to_string(remote.protocol) + " on " + who + " - update both to the same release");
        return out;   // the rest may not even mean the same
    }
    if (local.gameId != remote.gameId) {
        out.push_back("a different game: " + local.gameId + " here, " + remote.gameId + " on " + who);
        return out;
    }
    if (local.apiVersion != remote.apiVersion)
        out.push_back("PSE loader API " + std::to_string(local.apiVersion) + " here vs " +
                      std::to_string(remote.apiVersion) + " on " + who);
    const bool sameHost = local.host == remote.host;
    if (!sameHost)
        out.push_back(local.host + " here vs " + remote.host + " on " + who);
    else if (local.hostFingerprint != remote.hostFingerprint)
        out.push_back(local.host + ": a different build here than on " + who + " (simulation fingerprint " +
                      Short(local.hostFingerprint) + " vs " + Short(remote.hostFingerprint) + ")");

    std::map<std::string, const PluginEntry*> mine, theirs;
    for (const PluginEntry& p : local.plugins) mine[p.name] = &p;
    for (const PluginEntry& p : remote.plugins) theirs[p.name] = &p;
    std::map<std::string, int> names;
    for (auto& [n, p] : mine) names[n] |= 1;
    for (auto& [n, p] : theirs) names[n] |= 2;
    for (auto& [name, where] : names) {
        const PluginEntry* a = where & 1 ? mine[name] : nullptr;
        const PluginEntry* b = where & 2 ? theirs[name] : nullptr;
        const bool simA = a && Simulation(a->flags), simB = b && Simulation(b->flags);
        if (!simA && !simB) continue;   // client-only on every side that has it: may differ
        if (!a) {
            out.push_back("missing " + name + " " + VersionString(b->version) + " (" + who + " has it)");
            continue;
        }
        if (!b) {
            out.push_back(who + " is missing " + name + " " + VersionString(a->version));
            continue;
        }
        if (a->version != b->version) {
            out.push_back(name + " " + VersionString(a->version) + " here vs " + VersionString(b->version) + " on " + who);
            continue;
        }
        if (simA != simB) {
            out.push_back(name + " " + VersionString(a->version) + ": " + (simA ? "simulation" : "client-only") +
                          " here, " + (simB ? "simulation" : "client-only") + " on " + who);
            continue;
        }
        const bool builtA = a->hash == kBuiltIn, builtB = b->hash == kBuiltIn;
        if (builtA && builtB) continue;   // the host build fingerprint above covers built-in plugins
        if (builtA != builtB) {
            out.push_back(name + " " + VersionString(a->version) + ": " + (builtA ? "built in" : "a plugin DLL") +
                          " here, " + (builtB ? "built in" : "a plugin DLL") + " on " + who);
            continue;
        }
        const bool fpA = a->hash.rfind(kFingerprintPrefix, 0) == 0, fpB = b->hash.rfind(kFingerprintPrefix, 0) == 0;
        if (a->hash != b->hash && (fpA || fpB))
            out.push_back(name + " " + VersionString(a->version) + ": different simulation code here than on " + who +
                          " (fingerprint " + (fpA ? Short(a->hash.substr(3)) : "none") + " vs " +
                          (fpB ? Short(b->hash.substr(3)) : "none") + ")");
        else if (a->hash != b->hash)
            out.push_back(name + " " + VersionString(a->version) + ": a different file here than on " + who +
                          " (SHA-256 " + Short(a->hash) + " vs " + Short(b->hash) + ")");
    }

    std::map<std::string, const ModEntry*> myMods, theirMods;
    for (const ModEntry& m : local.mods) myMods[Lower(m.name)] = &m;
    for (const ModEntry& m : remote.mods) theirMods[Lower(m.name)] = &m;
    std::map<std::string, int> mods;
    for (auto& [n, m] : myMods) mods[n] |= 1;
    for (auto& [n, m] : theirMods) mods[n] |= 2;
    for (auto& [key, where] : mods) {
        const ModEntry* a = where & 1 ? myMods[key] : nullptr;
        const ModEntry* b = where & 2 ? theirMods[key] : nullptr;
        if (!a) out.push_back("missing mod " + b->name + " (" + who + " has it switched on)");
        else if (!b) out.push_back("mod " + a->name + " is switched on here, not on " + who);
        else if (a->hash != b->hash) out.push_back("mod " + a->name + " differs (its files aren't the same)");
    }
    // load order (mod priority list - the mod loaded last wins a file name): the lists come in load order, so the
    // mods both PCs use must come in the same relative order, or the PCs load different files
    std::vector<std::string> myOrder, theirOrder;
    for (const ModEntry& m : local.mods)
        if (theirMods.count(Lower(m.name))) myOrder.push_back(m.name);
    for (const ModEntry& m : remote.mods)
        if (myMods.count(Lower(m.name))) theirOrder.push_back(m.name);
    bool sameOrder = myOrder.size() == theirOrder.size();
    for (size_t i = 0; sameOrder && i < myOrder.size(); ++i) sameOrder = Lower(myOrder[i]) == Lower(theirOrder[i]);
    if (!sameOrder) {
        auto list = [](const std::vector<std::string>& v) {
            std::string s;
            for (const std::string& n : v) s += (s.empty() ? "" : ", ") + n;
            return s;
        };
        out.push_back("mod load order differs (here " + list(myOrder) + "; on " + who + " " + list(theirOrder) + ")");
    }
    return out;
}

// ---- SHA-256 (Windows CNG) -----------------------------------------------------------------------------------------

struct Sha256::Impl {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
};

Sha256::Sha256() : impl_(new Impl) {
    if (BCryptOpenAlgorithmProvider(&impl_->alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) impl_->alg = nullptr;
    if (impl_->alg && BCryptCreateHash(impl_->alg, &impl_->hash, nullptr, 0, nullptr, 0, 0) != 0) impl_->hash = nullptr;
}

Sha256::~Sha256() {
    if (impl_->hash) BCryptDestroyHash(impl_->hash);
    if (impl_->alg) BCryptCloseAlgorithmProvider(impl_->alg, 0);
    delete impl_;
}

void Sha256::Add(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (impl_->hash && len) {
        ULONG n = static_cast<ULONG>(std::min<size_t>(len, 1u << 30));
        BCryptHashData(impl_->hash, const_cast<PUCHAR>(p), n, 0);
        p += n;
        len -= n;
    }
}

std::string Sha256::Hex() {
    uint8_t digest[32] = {};
    if (!impl_->hash || BCryptFinishHash(impl_->hash, digest, sizeof digest, 0) != 0) return {};
    BCryptDestroyHash(impl_->hash);
    impl_->hash = nullptr;
    std::string out;
    char b[3];
    for (uint8_t c : digest) {
        std::snprintf(b, sizeof b, "%02x", c);
        out += b;
    }
    return out;
}

std::string Sha256Hex(const void* data, size_t len) {
    Sha256 h;
    h.Add(data, len);
    return h.Hex();
}

bool Sha256File(const std::wstring& path, std::string* hex) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    Sha256 h;
    std::vector<uint8_t> buf(1 << 16);
    DWORD got = 0;
    bool ok = true;
    while (true) {
        if (!ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr)) {
            ok = false;
            break;
        }
        if (!got) break;
        h.Add(buf.data(), got);
    }
    CloseHandle(f);
    if (!ok) return false;
    *hex = h.Hex();
    return !hex->empty();
}

}  // namespace pse::compat

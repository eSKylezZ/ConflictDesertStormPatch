// Online plugin-set check, DesertStormFix side (onlinecompat.h, docs/sdk/online-compat.md).
//
// What this PC sends (pse::compat::Set):
//   - loader API version, game id "ds1", host "DesertStormFix <DS_VERSION>";
//   - the host's SIMULATION FINGERPRINT: SHA-256 over DS_VERSION, kSimRevision and every byte patch of the generated
//     patch tables (address, length, original + patched bytes). It stands for all built-in plugins (dsfix.*): they
//     are compared by name + version + this fingerprint, NOT by the dinput8.dll file hash - the dev build (Steam,
//     logging, dev hooks) and the release package (GOG) of the same source are different files but simulate the
//     same, and must be able to play together. Code changes outside the patch tables that change the simulation
//     need a version bump or kSimRevision + 1;
//   - every loaded plugin (pse::compat::LocalPlugins: external SIMULATION ones with the SHA-256 of their DLL);
//   - every switched-on mod (features::LoadedMods): SHA-256 over the sorted list of its files (path relative to the
//     mod folder, lower case, '/' separators + each file's own SHA-256). Whole folder contents - simpler and safer
//     than guessing which files feed the simulation; a mod's readme counts too. Hashed on a worker thread (once per
//     file per run: size + write time cache), so the menus never stall.
#include "features/onlinecompat.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>

#include "core/log.h"
#include "core/simrevision.h"
#include "features/features.h"
#include "generated/patch_tables.h"
#include "loader.h"

namespace {
using pse::compat::Set;
using dsfix::kSimRevision;   // core/simrevision.h - bump there

constexpr DWORD kNoListMs = 30000;     // a peer that sends no list within this = an older build / broken: refused
constexpr DWORD kLeaveAfterMs = 4000;  // joiner, after a refusal: time for its own list to reach the host
constexpr DWORD kDropAfterMs = 6000;   // host, after a refusal: the joiner normally left by then

std::string HostName() { return std::string("DesertStormFix ") + DS_VERSION; }

std::string ComputeFingerprint() {
    pse::compat::Sha256 h;
    const std::string head = "dsfix-sim " + HostName() + " " + std::to_string(kSimRevision) + "\n";
    h.Add(head.data(), head.size());
    for (const gen::PatchGroup& g : gen::kAllGroups) {
        h.Add(g.name, strlen(g.name) + 1);
        for (size_t i = 0; i < g.count; ++i) {
            const BytePatch& p = g.patches[i];
            const uint32_t fields[3] = {p.va, p.len, p.anyOriginal ? 1u : 0u};
            h.Add(fields, sizeof fields);
            h.Add(p.original, p.len);
            h.Add(p.patched, p.len);
        }
    }
    return h.Hex();
}

// ---- mod hashing (worker thread) ----
struct FileKey {
    std::string path;
    uint64_t size, time;
    bool operator<(const FileKey& o) const {
        return std::tie(path, size, time) < std::tie(o.path, o.size, o.time);
    }
};
std::mutex g_cacheMx;
std::map<FileKey, std::string> g_fileHashes;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

void ListFiles(const std::string& dir, const std::string& rel, std::vector<std::pair<std::string, WIN32_FIND_DATAA>>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string name = fd.cFileName;
        if (name == "." || name == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(dir + "\\" + name, rel + name + "/", out);
        else out.push_back({rel + name, fd});
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

std::string ModHash(const std::string& folder) {
    std::vector<std::pair<std::string, WIN32_FIND_DATAA>> files;
    ListFiles(folder, "", files);
    for (auto& f : files) f.first = Lower(f.first);
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    pse::compat::Sha256 mod;
    for (const auto& [rel, fd] : files) {
        std::string full = folder + "\\" + rel;
        std::replace(full.begin(), full.end(), '/', '\\');
        const FileKey key{full, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow,
                          (uint64_t(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime};
        std::string hex;
        {
            std::lock_guard<std::mutex> lk(g_cacheMx);
            auto it = g_fileHashes.find(key);
            if (it != g_fileHashes.end()) hex = it->second;
        }
        if (hex.empty()) {
            if (!pse::compat::Sha256File(std::wstring(full.begin(), full.end()), &hex)) hex = "unreadable";
            std::lock_guard<std::mutex> lk(g_cacheMx);
            g_fileHashes[key] = hex;
        }
        mod.Add(rel.c_str(), rel.size() + 1);
        mod.Add(hex.data(), hex.size());
    }
    return mod.Hex();
}

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}

// ---- this PC's list ----
std::mutex g_localMx;
Set g_local;
std::atomic<bool> g_localReady{false}, g_localBusy{false};
std::vector<std::string> g_localMods;   // the mod names the list was built for
DWORD g_localStarted = 0;
std::atomic<uint32_t> g_localVersion{0};   // +1 per finished list: peers get it again and re-check (mods switched)

void StartLocal() {
    const std::vector<std::string> mods = features::LoadedMods();
    if (g_localBusy || (g_localReady && mods == g_localMods)) return;
    g_localReady = false;
    g_localBusy = true;
    g_localMods = mods;
    g_localStarted = GetTickCount();
    std::thread([mods, ports = features::LoadedPorts()] {
        Set s;
        s.apiVersion = PSE_API_VERSION;
        s.gameId = "ds1";
        s.host = HostName();
        s.hostFingerprint = onlinecompat::Fingerprint();
        s.plugins = pse::compat::LocalPlugins();
        const std::string root = GameDir() + "\\Mods\\";
        for (const std::string& m : mods) s.mods.push_back({m, ModHash(root + m)});
        // converted campaigns (mods.cpp BuildIndex, <game>\PSE\Ports\<id>): compared like simulation plugins - a
        // player without the same conversion is refused ("missing port:ds2"), never sent it (they convert it from
        // their own copy of that game: ModKit, installer or editor)
        for (const std::string& id : ports)
            s.plugins.push_back({"port:" + id, 1, PSE_PLUGIN_SIMULATION, ModHash(GameDir() + "\\PSE\\Ports\\" + id)});
        {
            std::lock_guard<std::mutex> lk(g_localMx);
            g_local = std::move(s);
        }
        ++g_localVersion;
        g_localReady = true;
        g_localBusy = false;
    }).detach();
}

// ---- peers ----
struct Peer {
    uint32_t id = 0;
    onlinecompat::State state = onlinecompat::State::Waiting;
    DWORD since = 0, refusedAt = 0;
    bool sent = false, gone = false, haveRemote = false;
    uint32_t sentVersion = 0;   // g_localVersion of the list it got
    pse::compat::Assembler assembler;
    Set remote;
    std::vector<std::string> diffs;
};
std::map<int, Peer> g_peers;              // by player number (game thread only)
std::vector<std::string> g_notice;        // the last refusal, kept for the screen after the connection is gone
bool g_wasHost = false;

std::string Who(int player) { return player == 1 ? "the host" : "player " + std::to_string(player); }

void Log(const std::string& line) { features::OnlineLogLine(line.c_str()); }   // + DesertStormFix.log (lockstep.cpp)

void Evaluate(int player, Peer& p) {
    Set local;
    {
        std::lock_guard<std::mutex> lk(g_localMx);
        local = g_local;
    }
    p.diffs = pse::compat::Compare(local, p.remote, Who(player));
    // Only the mods differ: not a refusal - the joiner gets the host's mods (modsync.cpp: downloads / switches them on
    // or off, then sends its list again); START waits meanwhile.
    Set a = local, b = p.remote;
    a.mods.clear(), b.mods.clear();
    if (!p.diffs.empty() && pse::compat::Compare(a, b, Who(player)).empty()) {
        p.state = onlinecompat::State::ModsDiffer;
        Log("Online check: " + Who(player) + " - same plugins, different mods (" + std::to_string(p.diffs.size()) +
            ") - the joiner syncs them");
        for (const std::string& d : p.diffs) Log("  " + d);
        return;
    }
    if (p.diffs.empty()) {
        p.state = onlinecompat::State::Ok;
        Log("Online check: " + Who(player) + " - plugins and mods match (" + std::to_string(p.remote.plugins.size()) +
            " plugins, " + std::to_string(p.remote.mods.size()) + " mods)");
        return;
    }
    p.state = onlinecompat::State::Refused;
    p.refusedAt = GetTickCount();
    Log("[fail] Online check: " + Who(player) + " refused - plugins / mods differ (" + std::to_string(p.diffs.size()) +
        "):");
    for (const std::string& d : p.diffs) Log("  " + d);
    features::ModLog("[fail] Online: %s - plugins / mods differ, see DesertStormFix-online.log", Who(player).c_str());
    g_notice.clear();
    g_notice.push_back(player == 1 ? "JOIN REFUSED - plugins / mods differ from the host's:"
                                   : "PLAYER " + std::to_string(player) + " REFUSED - plugins / mods differ:");
    for (const std::string& d : p.diffs) g_notice.push_back("  " + d);
}

void Refuse(int player, Peer& p, const std::string& why) {
    p.diffs = {why};
    p.state = onlinecompat::State::Refused;
    p.refusedAt = GetTickCount();
    Log("[fail] Online check: " + Who(player) + " refused - " + why);
    g_notice = {player == 1 ? "JOIN REFUSED:" : "PLAYER " + std::to_string(player) + " REFUSED:", "  " + why};
}

void Send(int player, Peer& p) {
    Set local;
    {
        std::lock_guard<std::mutex> lk(g_localMx);
        local = g_local;
    }
    const std::vector<std::string> lines = pse::compat::Encode(local, GetTickCount() ^ (player << 24), 240);
    for (const std::string& l : lines) net::SendCommandTo(player, l);
    p.sent = true;
    p.sentVersion = g_localVersion;
    Log("Online check: sent this PC's list to " + Who(player) + " (" + std::to_string(local.plugins.size()) +
        " plugins, " + std::to_string(local.mods.size()) + " mods, fingerprint " + local.hostFingerprint.substr(0, 8) +
        ", " + std::to_string(lines.size()) + " lines)");
}
}  // namespace

std::string onlinecompat::Fingerprint() {
    static const std::string fp = ComputeFingerprint();
    return fp;
}

void onlinecompat::Frame(const net::Snapshot& snap) {
    if (snap.role == net::Snapshot::Role::None) {
        g_peers.clear();
        return;
    }
    StartLocal();
    const bool host = snap.role == net::Snapshot::Role::Host;
    if (host != g_wasHost) g_peers.clear();
    g_wasHost = host;
    const DWORD now = GetTickCount();
    // connections now: player -> id
    std::map<int, uint32_t> live;
    for (size_t i = 0; i < snap.peers.size(); ++i)
        if (snap.peers[i].connected) live[host ? snap.peers[i].pc : 1] = snap.peers[i].id;
    for (auto it = g_peers.begin(); it != g_peers.end();) {
        auto l = live.find(it->first);
        if (l == live.end() && it->second.id) it = g_peers.erase(it);   // gone (the refusal text stays in g_notice)
        else ++it;
    }
    for (const auto& [player, id] : live) {
        Peer& p = g_peers[player];
        if (p.id && p.id != id) p = Peer{};   // a new connection at that number
        if (!p.id) {
            p.id = id;
            if (!p.since) p.since = now;
            Log("Online check: " + Who(player) + " connected - comparing plugins and mods");
        }
        if (!g_localReady) continue;
        if (p.sent && p.sentVersion != g_localVersion && p.state != State::Refused) {   // this PC's mods changed
            p.sent = false;
            if (p.haveRemote) p.state = State::Waiting;
        }
        if (!p.sent) Send(player, p);
        if (p.haveRemote && p.state == State::Waiting) Evaluate(player, p);
        if (p.state == State::Waiting && now - p.since > kNoListMs)
            Refuse(player, p, Who(player) + " sent no plugin / mod list (an older DesertStormFix? update both PCs)");
        // The joiner leaves first (once its own list was acknowledged, 1.5-4 s); the host drops a refused player only
        // later (6 s) - dropped earlier, a joiner still sending hellos came straight back as a new player.
        const bool leave = host ? now - p.refusedAt > kDropAfterMs
                                : now - p.refusedAt > 1500 && (net::Unacked(player) == 0 || now - p.refusedAt > kLeaveAfterMs);
        if (p.state == State::Refused && !p.gone && leave) {
            p.gone = true;
            if (host) {
                Log("Online check: dropping " + Who(player));
                net::Drop(player);
            } else {
                Log("Online check: leaving the host's session");
                net::Stop();
                g_peers.clear();
                return;
            }
        }
    }
}

bool onlinecompat::Command(const net::Command& c) {
    if (c.text.compare(0, 5, "PSEC ") != 0) return false;
    Peer& p = g_peers[c.from];
    if (!p.since) p.since = GetTickCount();
    switch (p.assembler.Feed(c.text)) {
        case pse::compat::Assembler::Done:
            p.remote = p.assembler.set;
            p.haveRemote = true;
            Log("Online check: list from " + Who(c.from) + " received (" + p.remote.host + ", " +
                std::to_string(p.remote.plugins.size()) + " plugins, " + std::to_string(p.remote.mods.size()) + " mods)");
            p.state = State::Waiting;
            if (g_localReady) Evaluate(c.from, p);
            break;
        case pse::compat::Assembler::Bad:
            Log("[fail] Online check: a bad line from " + Who(c.from) + " (" + p.assembler.error + ")");
            break;
        default: break;
    }
    return true;
}

onlinecompat::State onlinecompat::PeerState(int player) {
    auto it = g_peers.find(player);
    return it == g_peers.end() ? State::None : it->second.state;
}

bool onlinecompat::AllPassed(const net::Snapshot& snap, std::string* why) {
    for (size_t i = 0; i < snap.peers.size(); ++i) {
        if (!snap.peers[i].connected) continue;
        const int player = snap.peers[i].pc;
        const State s = PeerState(player);
        if (s == State::Ok) continue;
        if (why)
            *why = s == State::Refused      ? "PC " + std::to_string(player) + " was refused - plugins / mods differ"
                   : s == State::ModsDiffer ? "PC " + std::to_string(player) + " is getting the host's mods ..."
                                            : "PC " + std::to_string(player) + ": still comparing plugins and mods ...";
        return false;
    }
    return true;
}

std::vector<std::string> onlinecompat::StatusLines(bool host, size_t maxDiffs) {
    std::vector<std::string> out;
    auto add = [&](const std::vector<std::string>& block) {
        for (size_t i = 0; i < block.size(); ++i) {
            if (i > maxDiffs) {
                out.push_back("  ... " + std::to_string(block.size() - i) + " more (DesertStormFix-online.log)");
                break;
            }
            out.push_back(block[i]);
        }
    };
    bool refusedShown = false;
    for (const auto& [player, p] : g_peers) {
        if (!p.id) continue;
        const std::string who = player == 1 ? "Host" : "PC " + std::to_string(player);
        switch (p.state) {
            case State::Waiting: out.push_back(who + ": comparing plugins and mods ..."); break;
            case State::Ok: out.push_back(who + ": same plugins and mods - ok"); break;
            case State::ModsDiffer: out.push_back(who + ": same plugins, different mods - syncing the host's mods ..."); break;
            case State::Refused:
                if (!refusedShown && !g_notice.empty()) add(g_notice), refusedShown = true;
                break;
            default: break;
        }
    }
    if (!refusedShown && !g_notice.empty()) add(g_notice);
    (void)host;
    return out;
}

void onlinecompat::ClearNotice() { g_notice.clear(); }

std::vector<pse::compat::ModEntry> onlinecompat::RemoteMods(int player) {
    auto it = g_peers.find(player);
    return it == g_peers.end() || !it->second.haveRemote ? std::vector<pse::compat::ModEntry>{} : it->second.remote.mods;
}
bool onlinecompat::LocalMods(std::vector<pse::compat::ModEntry>& out) {
    if (!g_localReady) return false;
    std::lock_guard<std::mutex> lk(g_localMx);
    out = g_local.mods;
    return true;
}
std::string onlinecompat::HashModFolder(const std::string& folder) { return ModHash(folder); }

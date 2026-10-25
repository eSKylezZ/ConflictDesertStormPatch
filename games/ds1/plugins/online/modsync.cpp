// Online: a joiner gets the host's mods (user, 2026-10-22: "upload/download mods from the host if other players are
// missing it, a status screen ... then enable it"). The plugin / mod check (onlinecompat.cpp) reports "same plugins,
// different mods" as ModsDiffer instead of refusing; then, on the joiner:
//   plan      the host's switched-on mods vs this PC's: same name + content hash on -> nothing; the same content in a
//             switched-off folder ("_Name", ".Name", "_Name.yours") -> switch that one on; missing / another version ->
//             download it; switched on here but not at the host -> switch it off. Nothing is deleted: a replaced
//             version is renamed "_Name.yours" (off).
//   consent   the lobby shows the plan (sizes, time, a warning when a mod has scripts / DLLs - code that runs here);
//             Cross / A / Enter accepts, Esc leaves the session (lobbymenu.cpp).
//   apply     switches first, then each download, then the mods reload in game (features::ReloadMods - no restart);
//             the check sends this PC's list again and the host lets START go.
// Transfer: game commands (reliable, <= 249 chars) for the request / manifest, raw packets for the data:
//   "MS Q <xid> <name hex>"                        joiner -> host: send me that mod
//   "MS M <xid> <total bytes> <files> <hash>"      host: the manifest, then "MS F <xid> <index> <size> <path hex>"
//   "MS E <xid>"                                   per file, "MS E" at the end ("MS X <xid> <why hex>" = refused)
//   "MS G <xid>" / "MS D <xid>" / "MS C <xid>"     joiner: go / got it / cancel
//   raw 'M' 'D' xid u16, chunk u32, data           host -> joiner, 1200-byte chunks of the mod's files end to end
//   raw 'M' 'A' xid u16, next u32, have u32, bits  joiner -> host: first missing chunk + the 256 after it
// The host keeps a window of chunks in flight (grows while acks come, halves on a loss) and resends what isn't
// acknowledged in time; the joiner writes chunks into one file at their offset, splits it into the mod's files at the
// end and checks the folder's content hash against the host's before it goes in. Only in the front end (raw
// packets belong to lockstep.cpp in a mission). Host: only its own switched-on mods, only files under that folder.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "features/features.h"
#include "features/lobby.h"
#include "features/onlinecompat.h"

namespace {
constexpr uint32_t kChunk = 1200;
constexpr uint32_t kDispatchMode = 0x60EC8C;
constexpr uint64_t kMaxModBytes = 2ull << 30;   // 2 GB: refused above (one chunk index space is plenty)

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    return dir.substr(0, dir.find_last_of("\\/"));
}
std::string ModsDir() { return GameDir() + "\\Mods"; }
std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
bool SafeName(const std::string& n) {   // a mod folder name from the other PC
    if (n.empty() || n.size() > 100 || n == "." || n == ".." || n[0] == '_' || n[0] == '.') return false;
    for (char c : n)
        if (c < 0x20 || strchr("\\/:*?\"<>|", c)) return false;
    return true;
}
// A file path inside a mod, from the host ("textures/x.dds"): sent as hex in a command of <= 249 characters.
bool SafePath(const std::string& p) {
    if (p.empty() || p.size() > 100 || p[0] == '/' || p[0] == '\\') return false;
    for (char c : p)
        if (c < 0x20 || strchr(":*?\"<>|", c)) return false;
    size_t a = 0;
    while (a <= p.size()) {
        size_t b = p.find_first_of("/\\", a);
        if (b == std::string::npos) b = p.size();
        const std::string part = p.substr(a, b - a);
        if (part.empty() || part == "." || part == "..") return false;
        a = b + 1;
    }
    return true;
}
bool Runs(const std::string& path) {   // a file type that runs code here
    const std::string e = Lower(path.substr(path.find_last_of('.') + 1));
    return e == "dll" || e == "exe" || e == "asi" || e == "lua" || e == "bat" || e == "cmd" || e == "ps1" || e == "vbs" ||
           e == "js" || e == "scr";
}
std::string Size(uint64_t b) {
    char t[32];
    if (b >= 1024 * 1024) snprintf(t, sizeof t, "%.1f MB", b / (1024.0 * 1024.0));
    else snprintf(t, sizeof t, "%u KB", static_cast<unsigned>((b + 1023) / 1024));
    return t;
}
std::string Time(double seconds) {
    char t[48];
    if (seconds < 60) snprintf(t, sizeof t, "about %d s", std::max(1, static_cast<int>(seconds + 0.5)));
    else snprintf(t, sizeof t, "about %d min", static_cast<int>(seconds / 60 + 0.5));
    return t;
}

struct FileEntry {
    std::string rel;
    uint64_t size = 0, offset = 0;
};

void ListFiles(const std::string& dir, const std::string& rel, std::vector<FileEntry>& out) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(dir + "\\" + n, rel + n + "/", out);
        else out.push_back({rel + n, (uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow, 0});
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ================================================= host =================================================
struct Outgoing {
    int player = 0;
    uint16_t xid = 0;
    std::string mod, folder;
    std::vector<FileEntry> files;
    uint64_t total = 0;
    uint32_t chunks = 0, acked = 0, next = 0, ackBase = 0;   // ackBase: every chunk below it acknowledged
    std::vector<uint8_t> done;
    std::map<uint32_t, DWORD> inflight;   // chunk -> when sent
    double window = 32;
    DWORD lastLoss = 0, started = 0;
    bool go = false;
    FILE* open = nullptr;
    size_t openIndex = SIZE_MAX;
};
std::vector<std::unique_ptr<Outgoing>> g_out;

Outgoing* FindOut(int player, uint16_t xid) {
    for (auto& o : g_out)
        if (o->player == player && o->xid == xid) return o.get();
    return nullptr;
}
void CloseOut(Outgoing& o) {
    if (o.open) fclose(o.open), o.open = nullptr;
}

// One chunk's bytes from the mod's files (it may span several).
bool ReadChunk(Outgoing& o, uint32_t chunk, uint8_t* buf, uint32_t& len) {
    const uint64_t start = uint64_t(chunk) * kChunk;
    len = static_cast<uint32_t>(std::min<uint64_t>(kChunk, o.total - start));
    uint32_t got = 0;
    for (size_t i = 0; i < o.files.size() && got < len; ++i) {
        const FileEntry& f = o.files[i];
        const uint64_t at = start + got;
        if (at < f.offset || at >= f.offset + f.size) continue;
        if (o.openIndex != i) {
            CloseOut(o);
            std::string path = o.folder + "\\" + f.rel;
            std::replace(path.begin(), path.end(), '/', '\\');
            if (fopen_s(&o.open, path.c_str(), "rb") || !o.open) return false;
            o.openIndex = i;
        }
        const uint32_t n = static_cast<uint32_t>(std::min<uint64_t>(len - got, f.offset + f.size - at));
        _fseeki64(o.open, static_cast<__int64>(at - f.offset), SEEK_SET);
        if (fread(buf + got, 1, n, o.open) != n) return false;
        got += n;
    }
    return got == len;
}

void SendChunk(Outgoing& o, uint32_t chunk) {
    uint8_t p[8 + kChunk];
    uint32_t len = 0;
    if (!ReadChunk(o, chunk, p + 8, len)) return;
    p[0] = 'M', p[1] = 'D';
    memcpy(p + 2, &o.xid, 2);
    memcpy(p + 4, &chunk, 4);
    net::SendRawTo(o.player, p, static_cast<int>(8 + len));
    o.inflight[chunk] = GetTickCount();
}

void HostRequest(int player, uint16_t xid, const std::string& name) {
    auto refuse = [&](const char* why) {
        net::SendCommandTo(player, "MS X " + std::to_string(xid) + " " + lobby::Hex(why));
        dslog::Write("Mod sync: PC %d asked for %s - refused (%s)", player, name.c_str(), why);
    };
    std::vector<pse::compat::ModEntry> mine;
    if (!onlinecompat::LocalMods(mine)) return refuse("the host is still hashing its mods - ask again");
    const pse::compat::ModEntry* m = nullptr;
    for (const auto& e : mine)
        if (_stricmp(e.name.c_str(), name.c_str()) == 0) m = &e;
    if (!m || !SafeName(name)) return refuse("not one of the host's switched-on mods");
    auto o = std::make_unique<Outgoing>();
    o->player = player, o->xid = xid, o->mod = m->name, o->folder = ModsDir() + "\\" + m->name;
    ListFiles(o->folder, "", o->files);
    std::sort(o->files.begin(), o->files.end(), [](const FileEntry& a, const FileEntry& b) { return Lower(a.rel) < Lower(b.rel); });
    for (FileEntry& f : o->files) f.offset = o->total, o->total += f.size;
    for (const FileEntry& f : o->files)
        if (!SafePath(f.rel)) return refuse("a file path the transfer can't carry");
    if (o->total > kMaxModBytes) return refuse("bigger than 2 GB");
    o->chunks = static_cast<uint32_t>((o->total + kChunk - 1) / kChunk);
    o->done.assign(o->chunks, 0);
    net::SendCommandTo(player, "MS M " + std::to_string(xid) + " " + std::to_string(o->total) + " " +
                                   std::to_string(o->files.size()) + " " + m->hash);
    for (size_t i = 0; i < o->files.size(); ++i)
        net::SendCommandTo(player, "MS F " + std::to_string(xid) + " " + std::to_string(i) + " " +
                                       std::to_string(o->files[i].size) + " " + lobby::Hex(o->files[i].rel));
    net::SendCommandTo(player, "MS E " + std::to_string(xid));
    dslog::Write("Mod sync: PC %d asked for %s - manifest sent (%zu files, %s)", player, o->mod.c_str(), o->files.size(),
                 Size(o->total).c_str());
    for (auto it = g_out.begin(); it != g_out.end();)   // that PC's older request for the same mod goes
        if ((*it)->player == player && _stricmp((*it)->mod.c_str(), o->mod.c_str()) == 0) CloseOut(**it), it = g_out.erase(it);
        else ++it;
    g_out.push_back(std::move(o));
}

void HostAck(int player, const std::vector<uint8_t>& d) {
    if (d.size() < 12) return;
    uint16_t xid;
    uint32_t base, have;
    memcpy(&xid, &d[2], 2), memcpy(&base, &d[4], 4), memcpy(&have, &d[8], 4);
    Outgoing* o = FindOut(player, xid);
    if (!o || !o->go) return;
    uint32_t fresh = 0;
    auto mark = [&](uint32_t c) {
        if (c >= o->chunks || o->done[c]) return;
        o->done[c] = 1, ++o->acked, ++fresh;
        o->inflight.erase(c);
    };
    for (uint32_t c = o->ackBase; c < base && c < o->chunks; ++c) mark(c);
    o->ackBase = std::max(o->ackBase, std::min(base, o->chunks));
    for (size_t i = 12; i < d.size(); ++i)
        for (int b = 0; b < 8; ++b)
            if (d[i] >> b & 1) mark(base + 1 + static_cast<uint32_t>((i - 12) * 8 + b));
    o->window = std::min(2048.0, o->window + fresh / std::max(1.0, o->window) * 2);
    (void)have;
}

void HostFrame(const net::Snapshot& snap) {
    const DWORD now = GetTickCount();
    for (auto it = g_out.begin(); it != g_out.end();) {   // PCs that left
        bool live = false;
        for (const auto& p : snap.peers) live |= p.connected && p.pc == (*it)->player;
        if (!live) CloseOut(**it), it = g_out.erase(it);
        else ++it;
    }
    for (auto& up : g_out) {
        Outgoing& o = *up;
        if (!o.go || o.acked >= o.chunks) continue;
        int ping = 100;
        for (const auto& p : snap.peers)
            if (p.pc == o.player && p.ping > 0) ping = p.ping;
        const DWORD rto = static_cast<DWORD>(std::max(250, ping * 3));
        int budget = 600;   // packets this frame
        for (auto& [chunk, at] : o.inflight)   // lost: again, and a smaller window (once per round trip)
            if (now - at > rto && budget-- > 0) {
                if (now - o.lastLoss > rto) o.window = std::max(16.0, o.window * 0.5), o.lastLoss = now;
                uint8_t p[8 + kChunk];
                uint32_t len = 0;
                if (ReadChunk(o, chunk, p + 8, len)) {
                    p[0] = 'M', p[1] = 'D';
                    memcpy(p + 2, &o.xid, 2), memcpy(p + 4, &chunk, 4);
                    net::SendRawTo(o.player, p, static_cast<int>(8 + len));
                }
                at = now;
            }
        while (o.inflight.size() < static_cast<size_t>(o.window) && o.next < o.chunks && budget-- > 0) {
            if (!o.done[o.next]) SendChunk(o, o.next);
            ++o.next;
        }
    }
}

// ================================================= joiner =================================================
enum class Kind { Download, Enable, Disable };
enum class Step { Waiting, Manifest, Ready, Running, Done, Failed };
struct Item {
    Kind kind;
    std::string mod, hash, from;   // from: the folder switched on (Enable) / off (Disable)
    Step step = Step::Waiting;
    std::string error;
    // download
    uint16_t xid = 0;
    std::vector<FileEntry> files;
    uint64_t total = 0;
    uint32_t chunks = 0, have = 0;
    std::vector<uint8_t> got;
    size_t fileCount = 0;
    bool runs = false;
    FILE* stream = nullptr;
    std::string tmp;
    DWORD lastAck = 0, started = 0;
    uint64_t rateBytes = 0;
    DWORD rateAt = 0;
    double rate = 0;   // bytes / s
};
enum class Phase { None, Planning, Manifests, Consent, Applying, Reload, Done, Failed };
Phase g_phase = Phase::None;
std::vector<Item> g_items;
std::string g_failed;
uint16_t g_nextXid = 1;
std::atomic<bool> g_planReady{false};
std::vector<Item> g_planned;   // built on a worker thread
std::mutex g_planMx;
std::string g_planFor;         // the host mod list the plan was made for
DWORD g_doneAt = 0;

std::string ListKey(const std::vector<pse::compat::ModEntry>& m) {
    std::string k;
    for (const auto& e : m) k += Lower(e.name) + ":" + e.hash + ";";
    return k;
}

// The worker: which local folder (any switch state) holds each host mod's content.
void BuildPlan(std::vector<pse::compat::ModEntry> host, std::vector<pse::compat::ModEntry> local) {
    std::vector<Item> plan;
    const std::string root = ModsDir();
    std::vector<std::string> folders;   // every folder under Mods
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::string n = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n != "." && n != "..") folders.push_back(n);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    auto bare = [](std::string n) {   // "_Name.yours" / ".Name" -> "name"
        while (!n.empty() && (n[0] == '_' || n[0] == '.')) n.erase(0, 1);
        const size_t y = Lower(n).rfind(".yours");
        if (y != std::string::npos) n.resize(y);
        return Lower(n);
    };
    for (const auto& m : host) {
        bool same = false;
        for (const auto& l : local) same |= _stricmp(l.name.c_str(), m.name.c_str()) == 0 && l.hash == m.hash;
        if (same) continue;
        std::string found;
        for (const std::string& f : folders) {
            if (bare(f) != Lower(m.name) || _stricmp(f.c_str(), m.name.c_str()) == 0) continue;   // (the on one differs)
            if (onlinecompat::HashModFolder(root + "\\" + f) == m.hash) {
                found = f;
                break;
            }
        }
        Item it;
        it.kind = found.empty() ? Kind::Download : Kind::Enable;
        it.mod = m.name, it.hash = m.hash, it.from = found;
        plan.push_back(std::move(it));
    }
    for (const auto& l : local) {
        bool used = false;
        for (const auto& m : host) used |= _stricmp(l.name.c_str(), m.name.c_str()) == 0;
        if (used) continue;
        Item it;
        it.kind = Kind::Disable;
        it.mod = l.name, it.from = l.name;
        plan.push_back(std::move(it));
    }
    std::lock_guard<std::mutex> lk(g_planMx);
    g_planned = std::move(plan);
    g_planReady = true;
}

std::string FreeName(const std::string& want) {   // "_Name.yours", "_Name.yours2" ...
    std::string n = want;
    for (int k = 2; GetFileAttributesA((ModsDir() + "\\" + n).c_str()) != INVALID_FILE_ATTRIBUTES; ++k) n = want + std::to_string(k);
    return n;
}
bool Move(const std::string& from, const std::string& to, std::string& error) {
    if (MoveFileExA((ModsDir() + "\\" + from).c_str(), (ModsDir() + "\\" + to).c_str(), 0)) return true;
    error = "couldn't rename " + from + " (error " + std::to_string(GetLastError()) + ")";
    return false;
}
// The host's version goes in as Mods\<mod>; one already there (another version) stays, switched off.
bool PutInPlace(const std::string& from, const std::string& mod, std::string& error) {
    if (GetFileAttributesA((ModsDir() + "\\" + mod).c_str()) != INVALID_FILE_ATTRIBUTES &&
        !Move(mod, FreeName("_" + mod + ".yours"), error))
        return false;
    return Move(from, mod, error);
}

void Fail(const std::string& why) {
    g_failed = why;
    g_phase = Phase::Failed;
    for (Item& it : g_items)
        if (it.stream) fclose(it.stream), it.stream = nullptr;
    dslog::Write("[fail] Mod sync: %s", why.c_str());
    features::OnlineLogLine(("[fail] Mod sync: " + why).c_str());
}

void Request(Item& it) {
    it.xid = g_nextXid++;
    it.step = Step::Manifest;
    net::SendCommandTo(1, "MS Q " + std::to_string(it.xid) + " " + lobby::Hex(it.mod));
}

void SendAck(Item& it) {
    uint8_t p[12 + 32] = {'M', 'A'};
    uint32_t base = 0;
    while (base < it.chunks && it.got[base]) ++base;
    memcpy(p + 2, &it.xid, 2), memcpy(p + 4, &base, 4), memcpy(p + 8, &it.have, 4);
    for (uint32_t k = 0; k < 256; ++k) {
        const uint32_t c = base + 1 + k;
        if (c < it.chunks && it.got[c]) p[12 + k / 8] |= static_cast<uint8_t>(1u << (k % 8));
    }
    net::SendRawTo(1, p, sizeof p);
    it.lastAck = GetTickCount();
}

// All chunks here: the stream split into the mod's files, the content checked, the folder put in place.
bool Finish(Item& it) {
    fclose(it.stream), it.stream = nullptr;
    const std::string stream = it.tmp + "\\.stream";
    FILE* in = nullptr;
    if (fopen_s(&in, stream.c_str(), "rb") || !in) return Fail("can't read the download of " + it.mod), false;
    std::vector<char> buf(1 << 20);
    for (const FileEntry& f : it.files) {
        std::string path = it.tmp + "\\" + f.rel;
        std::replace(path.begin(), path.end(), '/', '\\');
        for (size_t s = path.find('\\', it.tmp.size() + 1); s != std::string::npos; s = path.find('\\', s + 1))
            CreateDirectoryA(path.substr(0, s).c_str(), nullptr);
        FILE* out = nullptr;
        if (fopen_s(&out, path.c_str(), "wb") || !out) return fclose(in), Fail("can't write " + f.rel), false;
        uint64_t left = f.size;
        while (left) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(left, buf.size()));
            if (fread(buf.data(), 1, n, in) != n || fwrite(buf.data(), 1, n, out) != n) {
                fclose(out), fclose(in);
                return Fail("the download of " + it.mod + " is short"), false;
            }
            left -= n;
        }
        fclose(out);
    }
    fclose(in);
    DeleteFileA(stream.c_str());
    const std::string hash = onlinecompat::HashModFolder(it.tmp);
    if (hash != it.hash) return Fail(it.mod + " arrived different from the host's (content hash) - not used"), false;
    std::string error;
    const std::string tmpName = it.tmp.substr(it.tmp.find_last_of('\\') + 1);
    if (!PutInPlace(tmpName, it.mod, error)) return Fail(error), false;
    net::SendCommandTo(1, "MS D " + std::to_string(it.xid));
    dslog::Write("Mod sync: %s downloaded (%s) and in place", it.mod.c_str(), Size(it.total).c_str());
    return true;
}

void JoinerData(const std::vector<uint8_t>& d) {
    if (d.size() < 8) return;
    uint16_t xid;
    uint32_t chunk;
    memcpy(&xid, &d[2], 2), memcpy(&chunk, &d[4], 4);
    for (Item& it : g_items) {
        if (it.kind != Kind::Download || it.xid != xid || it.step != Step::Running || !it.stream) continue;
        if (chunk >= it.chunks || it.got[chunk]) return;
        const uint32_t len = static_cast<uint32_t>(d.size() - 8);
        const uint32_t want = static_cast<uint32_t>(std::min<uint64_t>(kChunk, it.total - uint64_t(chunk) * kChunk));
        if (len != want) return;
        _fseeki64(it.stream, static_cast<__int64>(uint64_t(chunk) * kChunk), SEEK_SET);
        fwrite(&d[8], 1, len, it.stream);
        it.got[chunk] = 1, ++it.have;
        it.rateBytes += len;
        return;
    }
}

void StartDownload(Item& it) {
    it.tmp = ModsDir() + "\\." + it.mod + ".download";
    CreateDirectoryA(ModsDir().c_str(), nullptr);
    CreateDirectoryA(it.tmp.c_str(), nullptr);
    if (fopen_s(&it.stream, (it.tmp + "\\.stream").c_str(), "wb+") || !it.stream)
        return Fail("can't write into " + it.tmp);
    if (it.total) {   // the stream at its full size (chunks land anywhere)
        _fseeki64(it.stream, static_cast<__int64>(it.total - 1), SEEK_SET);
        fputc(0, it.stream);
    }
    it.got.assign(it.chunks, 0);
    it.have = 0;
    it.step = Step::Running;
    it.started = it.rateAt = GetTickCount();
    net::SendCommandTo(1, "MS G " + std::to_string(it.xid));
    dslog::Write("Mod sync: downloading %s (%s, %zu files)", it.mod.c_str(), Size(it.total).c_str(), it.files.size());
}

void Apply() {   // switches, then the downloads one at a time, then the reload
    for (Item& it : g_items) {
        if (it.step == Step::Done || it.kind == Kind::Download) continue;
        std::string error;
        const bool ok = it.kind == Kind::Enable ? PutInPlace(it.from, it.mod, error) : Move(it.from, FreeName("_" + it.from), error);
        if (!ok) return Fail(error);
        it.step = Step::Done;
        dslog::Write("Mod sync: %s switched %s", it.mod.c_str(), it.kind == Kind::Enable ? "on" : "off");
    }
    for (Item& it : g_items) {
        if (it.kind != Kind::Download || it.step == Step::Done) continue;
        if (it.step == Step::Ready) StartDownload(it);
        if (g_phase == Phase::Failed) return;
        if (it.step == Step::Running) {
            const DWORD now = GetTickCount();
            if (now - it.rateAt >= 1000) {   // bytes per second, smoothed
                const double r = it.rateBytes * 1000.0 / (now - it.rateAt);
                it.rate = it.rate > 0 ? it.rate * 0.6 + r * 0.4 : r;
                it.rateBytes = 0, it.rateAt = now;
            }
            if (now - it.lastAck >= 25) SendAck(it);
            if (it.have == it.chunks) {
                SendAck(it);
                if (Finish(it)) it.step = Step::Done;
            }
            return;   // one at a time
        }
    }
    g_phase = Phase::Reload;
}

void JoinerFrame(const net::Snapshot& snap) {
    const bool connected = snap.role == net::Snapshot::Role::Join && !snap.peers.empty() && snap.peers[0].connected;
    const onlinecompat::State state = onlinecompat::PeerState(1);
    if (!connected) {
        if (g_phase != Phase::None) {
            for (Item& it : g_items)
                if (it.stream) fclose(it.stream), it.stream = nullptr;
            g_items.clear();
            g_phase = Phase::None;
        }
        return;
    }
    if (state == onlinecompat::State::Ok && g_phase != Phase::Failed) {
        if (g_phase != Phase::None && g_phase != Phase::Done) dslog::Write("Mod sync: the host's mods match now");
        if (g_phase == Phase::Reload || g_phase == Phase::Applying) g_phase = Phase::Done, g_doneAt = GetTickCount();
        if (g_phase == Phase::Done && GetTickCount() - g_doneAt > 4000) g_phase = Phase::None, g_items.clear();
        return;
    }
    switch (g_phase) {
        case Phase::None: {
            if (state != onlinecompat::State::ModsDiffer) return;
            std::vector<pse::compat::ModEntry> local;
            const auto host = onlinecompat::RemoteMods(1);
            if (!onlinecompat::LocalMods(local)) return;
            if (!features::ModsFolderEnabled())
                return Fail("the host uses mods, and the Mods folder is off here (OPTIONS > DESERTSTORMFIX > MODS, then restart)");
            g_planFor = ListKey(host);
            g_planReady = false;
            g_phase = Phase::Planning;
            std::thread(BuildPlan, host, local).detach();
            return;
        }
        case Phase::Planning: {
            if (!g_planReady) return;
            {
                std::lock_guard<std::mutex> lk(g_planMx);
                g_items = std::move(g_planned);
            }
            dslog::Write("Mod sync: %zu change(s) to match the host's mods", g_items.size());
            g_phase = Phase::Manifests;
            for (Item& it : g_items)
                if (it.kind == Kind::Download) Request(it);
            return;
        }
        case Phase::Manifests: {
            bool all = true;
            for (const Item& it : g_items) all &= it.kind != Kind::Download || it.step == Step::Ready;
            if (all) g_phase = Phase::Consent;
            return;
        }
        case Phase::Applying: return Apply();
        case Phase::Done:   // reloaded: the host's check should pass within moments
            if (state == onlinecompat::State::ModsDiffer && GetTickCount() - g_doneAt > 20000)
                Fail("the mods still differ from the host's after the switch - see DesertStormFix-online.log");
            return;
        case Phase::Reload:
            features::ReloadMods();   // the check notices the new mod list and sends it again (onlinecompat.cpp)
            g_phase = Phase::Done;
            g_doneAt = GetTickCount();
            dslog::Write("Mod sync: mods reloaded - waiting for the host's check");
            return;
        default: return;
    }
}

void JoinerCommand(const std::string& t) {
    char kind = 0;
    unsigned xid = 0;
    if (sscanf_s(t.c_str(), "MS %c %u", &kind, 1, &xid) < 2) return;
    for (Item& it : g_items) {
        if (it.kind != Kind::Download || it.xid != xid) continue;
        if (kind == 'M') {
            unsigned long long total = 0;
            size_t count = 0;
            char hash[80] = {};
            if (sscanf_s(t.c_str(), "MS M %*u %llu %zu %79s", &total, &count, hash, 80u) < 3 || total > kMaxModBytes) return Fail("a bad manifest for " + it.mod);
            if (hash[0] && it.hash != hash) return Fail(it.mod + " changed at the host - join again");
            it.total = total, it.fileCount = count;
            it.chunks = static_cast<uint32_t>((total + kChunk - 1) / kChunk);
            it.files.clear();
        } else if (kind == 'F') {
            unsigned idx = 0;
            unsigned long long size = 0;
            char path[600] = {};
            if (sscanf_s(t.c_str(), "MS F %*u %u %llu %599s", &idx, &size, path, 600u) < 3) return Fail("a bad file entry for " + it.mod);
            const std::string rel = lobby::Unhex(path);
            if (!SafePath(rel)) return Fail(it.mod + " has a file path that isn't allowed: " + rel);
            FileEntry f{rel, size, it.files.empty() ? 0 : it.files.back().offset + it.files.back().size};
            it.files.push_back(f);
            it.runs |= Runs(rel);
        } else if (kind == 'E') {
            uint64_t sum = 0;
            for (const FileEntry& f : it.files) sum += f.size;
            if (it.files.size() != it.fileCount || sum != it.total) return Fail("the manifest of " + it.mod + " doesn't add up");
            it.step = Step::Ready;
        } else if (kind == 'X') {
            char why[300] = {};
            sscanf_s(t.c_str(), "MS X %*u %299s", why, 300u);
            return Fail("the host can't send " + it.mod + ": " + lobby::Unhex(why));
        }
        return;
    }
}
}  // namespace

// ================================================= interface =================================================
void features::OnFrameModSync() {
    const net::Snapshot snap = net::Snap();
    if (snap.role == net::Snapshot::Role::None) {
        for (auto& o : g_out) CloseOut(*o);
        g_out.clear();
        if (g_phase != Phase::None) g_items.clear(), g_phase = Phase::None;
        return;
    }
    // raw packets belong to lockstep.cpp in a mission; transfers happen in the front end only
    if (!features::LockstepActive() && *reinterpret_cast<int8_t*>(kDispatchMode) == 0)
        for (const net::Raw& r : net::TakeRaw()) {
            if (r.data.size() < 2 || r.data[0] != 'M') continue;
            if (r.data[1] == 'D' && snap.role == net::Snapshot::Role::Join) JoinerData(r.data);
            else if (r.data[1] == 'A' && snap.role == net::Snapshot::Role::Host) HostAck(r.from, r.data);
        }
    if (snap.role == net::Snapshot::Role::Host) HostFrame(snap);
    else JoinerFrame(snap);
}

bool features::ModSyncCommand(int from, const std::string& text) {
    if (text.compare(0, 3, "MS ") != 0) return false;
    if (net::LocalPlayer() == 1) {   // host: requests from a joiner
        char kind = 0;
        unsigned xid = 0;
        char name[300] = {};
        if (sscanf_s(text.c_str(), "MS %c %u %299s", &kind, 1, &xid, name, 300u) < 2) return true;
        if (kind == 'Q') HostRequest(from, static_cast<uint16_t>(xid), lobby::Unhex(name));
        else if (Outgoing* o = FindOut(from, static_cast<uint16_t>(xid))) {
            if (kind == 'G') o->go = true, o->started = GetTickCount(), dslog::Write("Mod sync: sending %s to PC %d", o->mod.c_str(), from);
            if (kind == 'D' || kind == 'C') {
                dslog::Write("Mod sync: PC %d %s %s", from, kind == 'D' ? "got" : "cancelled", o->mod.c_str());
                CloseOut(*o);
                g_out.erase(std::remove_if(g_out.begin(), g_out.end(), [&](const auto& p) { return p.get() == o; }), g_out.end());
            }
        }
        return true;
    }
    if (from == 1) JoinerCommand(text);
    return true;
}

bool features::ModSyncActive() { return g_phase != Phase::None; }
bool features::ModSyncWaitingForYes() { return g_phase == Phase::Consent; }
void features::ModSyncAccept() {
    if (g_phase != Phase::Consent) return;
    dslog::Write("Mod sync: the player accepted - %zu change(s)", g_items.size());
    g_phase = Phase::Applying;
}
void features::ModSyncCancel() {
    for (Item& it : g_items) {
        if (it.step == Step::Running) net::SendCommandTo(1, "MS C " + std::to_string(it.xid));
        if (it.stream) fclose(it.stream), it.stream = nullptr;
    }
    g_items.clear();
    g_phase = Phase::None;
}

// The joiner's screen: a title, one row per change (text, progress 0..1 or -1), the prompt.
std::vector<features::ModSyncRow> features::ModSyncRows(std::string& title, std::string& footer) {
    std::vector<ModSyncRow> rows;
    uint64_t bytes = 0;
    bool runs = false;
    double rate = 0;
    for (const Item& it : g_items) {
        ModSyncRow r;
        r.progress = -1;
        switch (it.kind) {
            case Kind::Enable: r.text = "SWITCH ON   " + it.mod + "   (you have the host's version)"; break;
            case Kind::Disable: r.text = "SWITCH OFF  " + it.mod + "   (the host doesn't use it)"; break;
            case Kind::Download:
                r.text = "DOWNLOAD    " + it.mod + "   " + (it.step == Step::Manifest ? "asking the host ..." : Size(it.total));
                if (it.runs) r.text += "   - has scripts / DLLs";
                bytes += it.step == Step::Done ? 0 : it.total - uint64_t(it.have) * kChunk;
                runs |= it.runs;
                if (it.step == Step::Running) {
                    r.progress = it.chunks ? static_cast<float>(it.have) / it.chunks : 1.0f;
                    rate = it.rate;
                    if (it.rate > 1) r.text += "   " + Time((it.total - std::min<uint64_t>(it.total, uint64_t(it.have) * kChunk)) / it.rate) + " left";
                }
                if (it.step == Step::Done) r.progress = 1;
                break;
        }
        if (it.step == Step::Done && it.kind != Kind::Download) r.progress = 1;
        rows.push_back(r);
    }
    switch (g_phase) {
        case Phase::Planning: title = "COMPARING YOUR MODS WITH THE HOST'S ..."; footer = ""; break;
        case Phase::Manifests: title = "ASKING THE HOST FOR ITS MODS ..."; footer = ""; break;
        case Phase::Consent:
            title = "THE HOST PLAYS WITH OTHER MODS";
            footer = bytes ? "DOWNLOAD " + Size(bytes) + " FROM THE HOST AND SWITCH THEM ON?" : "SWITCH THEM AS THE HOST HAS THEM?";
            if (runs) footer += "   (SCRIPTS / DLLS RUN ON THIS PC - ONLY FOR A HOST YOU TRUST)";
            break;
        case Phase::Applying:
            title = "GETTING THE HOST'S MODS";
            footer = rate > 1 ? "DOWNLOADING - " + Time(bytes / rate) + " LEFT" : "WORKING ...";
            break;
        case Phase::Reload: title = "SWITCHING THE MODS ON ..."; footer = ""; break;
        case Phase::Done: title = "MODS READY"; footer = "WAITING FOR THE HOST'S CHECK ..."; break;
        case Phase::Failed: title = "COULDN'T GET THE HOST'S MODS"; footer = g_failed; break;
        default: title.clear(); footer.clear(); break;
    }
    return rows;
}

// The host's lobby status: what each PC is getting.
std::vector<std::string> features::ModSyncHostLines() {
    std::vector<std::string> out;
    for (const auto& o : g_out) {
        if (!o->go) {
            out.push_back("PC " + std::to_string(o->player) + ": deciding whether to get " + o->mod);
            continue;
        }
        if (o->acked >= o->chunks) continue;
        const double secs = std::max(1.0, (GetTickCount() - o->started) / 1000.0);
        const double rate = uint64_t(o->acked) * kChunk / secs;
        char t[160];
        snprintf(t, sizeof t, "PC %d: getting %s - %d%%%s", o->player, o->mod.c_str(),
                 o->chunks ? static_cast<int>(100.0 * o->acked / o->chunks) : 100,
                 rate > 1 ? (", " + Time((o->total - std::min<uint64_t>(o->total, uint64_t(o->acked) * kChunk)) / rate) + " left").c_str() : "");
        out.push_back(t);
    }
    return out;
}

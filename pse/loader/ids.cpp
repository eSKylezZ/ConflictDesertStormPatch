// pse.ids: per-game ID databases -> addresses for the running exe (docs/sdk/reference/ids.md).
#include "ids.h"

#include <windows.h>

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "loader.h"

namespace pse::ids {
namespace {

struct Game {
    const GameDef* def = nullptr;
    const BuildDef* build = nullptr;   // nullptr = unknown exe
    uint32_t timestamp = 0, sizeOfImage = 0;
    std::vector<uint32_t> va;          // per entry (def->entries order)
    std::vector<uint8_t> status;       // PSEIdStatus per entry
    std::unordered_map<std::string, uint32_t> byName;   // name -> entry index
    uint32_t resolved = 0, mismatched = 0;
};

std::mutex g_mutex;
std::vector<Game*> g_games;                    // current database per game id
std::deque<std::unique_ptr<Game>> g_all;       // every Game ever made (a replaced one stays valid until reset)

// Lines go to the owner's log (pse::LogLine: the loader in phase 1, pse.host from a game layer DLL). No owner = quiet.
PSEPluginHandle g_logOwner = 0;
void Log(PSELogLevel level, const char* fmt, ...) {
    if (!g_logOwner) return;
    char text[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    LogLine(g_logOwner, level, text);
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "8B4424??" -> bytes + mask (mask 0 = any). false = malformed / empty / > 16 bytes.
bool Parse(const char* sig, uint8_t* bytes, uint8_t* mask, uint32_t* len) {
    if (!sig) return false;
    const size_t n = std::strlen(sig);
    if (n == 0 || n % 2 || n / 2 > 16) return false;
    bool anyFixed = false;
    for (size_t i = 0; i < n; i += 2) {
        if (sig[i] == '?' && sig[i + 1] == '?') {
            bytes[i / 2] = 0;
            mask[i / 2] = 0;
            continue;
        }
        const int hi = HexValue(sig[i]), lo = HexValue(sig[i + 1]);
        if (hi < 0 || lo < 0) return false;
        bytes[i / 2] = static_cast<uint8_t>(hi << 4 | lo);
        mask[i / 2] = 0xFF;
        anyFixed = true;
    }
    *len = static_cast<uint32_t>(n / 2);
    return anyFixed;
}

// NULL / "" = the running game: the first database registered (a process runs one game; its layer registers first).
const Game* FindGameLocked(const char* gameId) {
    if (!gameId || !*gameId) return g_games.empty() ? nullptr : g_games.front();
    for (const Game* g : g_games)
        if (std::strcmp(g->def->gameId, gameId) == 0) return g;
    return nullptr;
}

// Entry index of an id (entries are sorted), -1 = none.
int IndexOf(const Game* g, PSEId id) {
    uint32_t lo = 0, hi = g->def->entryCount;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        const uint32_t v = g->def->entries[mid].id;
        if (v == id) return static_cast<int>(mid);
        if (v < id) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

void Fill(const Game* g, int index, PSEIdInfo* out, PSEId id) {
    const uint32_t size = out->size;
    std::memset(out, 0, size < sizeof(PSEIdInfo) ? size : sizeof(PSEIdInfo));
    PSEIdInfo info{};
    info.size = size;
    info.id = id;
    if (g && index >= 0) {
        const EntryDef& e = g->def->entries[index];
        info.id = e.id;
        info.name = e.name;
        info.kind = static_cast<PSEIdKind>(e.kind);
        info.status = static_cast<PSEIdStatus>(g->status[static_cast<size_t>(index)]);
        info.va = g->va[static_cast<size_t>(index)];
    }
    std::memcpy(out, &info, size < sizeof(PSEIdInfo) ? size : sizeof(PSEIdInfo));
    out->size = size;
}

// ---- interface -------------------------------------------------------------------------------------------------

uint32_t PSE_CALL Resolve(const char* gameId, PSEId id) {
    std::lock_guard lock(g_mutex);
    const Game* g = FindGameLocked(gameId);
    if (!g) return 0;
    const int i = IndexOf(g, id);
    return i < 0 ? 0 : g->va[static_cast<size_t>(i)];
}

uint32_t PSE_CALL ResolveByName(const char* gameId, const char* name) {
    if (!name) return 0;
    std::lock_guard lock(g_mutex);
    const Game* g = FindGameLocked(gameId);
    if (!g) return 0;
    auto it = g->byName.find(name);
    return it == g->byName.end() ? 0 : g->va[it->second];
}

const char* PSE_CALL Name(const char* gameId, PSEId id) {
    std::lock_guard lock(g_mutex);
    const Game* g = FindGameLocked(gameId);
    if (!g) return nullptr;
    const int i = IndexOf(g, id);
    return i < 0 ? nullptr : g->def->entries[i].name;
}

PSEId PSE_CALL IdOf(const char* gameId, const char* name) {
    if (!name) return 0;
    std::lock_guard lock(g_mutex);
    const Game* g = FindGameLocked(gameId);
    if (!g) return 0;
    auto it = g->byName.find(name);
    return it == g->byName.end() ? 0 : g->def->entries[it->second].id;
}

PSEResult PSE_CALL Info(const char* gameId, PSEId id, PSEIdInfo* out) {
    if (!out || out->size < offsetof(PSEIdInfo, va) + sizeof(uint32_t)) return PSE_E_INVALID_ARG;
    std::lock_guard lock(g_mutex);
    const Game* g = FindGameLocked(gameId);
    const int i = g ? IndexOf(g, id) : -1;
    Fill(g, i, out, id);
    return i < 0 ? PSE_E_NOT_FOUND : PSE_OK;
}

PSEResult PSE_CALL InfoAt(const char* gameId, uint32_t index, PSEIdInfo* out) {
    if (!out || out->size < offsetof(PSEIdInfo, va) + sizeof(uint32_t)) return PSE_E_INVALID_ARG;
    std::lock_guard lock(g_mutex);
    const Game* g = FindGameLocked(gameId);
    if (!g || index >= g->def->entryCount) {
        Fill(nullptr, -1, out, 0);
        return PSE_E_NOT_FOUND;
    }
    Fill(g, static_cast<int>(index), out, 0);
    return PSE_OK;
}

PSEResult PSE_CALL BuildInfo(const char* gameId, PSEIdBuildInfo* out) {
    if (!out || out->size < offsetof(PSEIdBuildInfo, buildCount) + sizeof(uint32_t)) return PSE_E_INVALID_ARG;
    std::lock_guard lock(g_mutex);
    const Game* g = FindGameLocked(gameId);
    const uint32_t size = out->size;
    PSEIdBuildInfo b{};
    b.size = size;
    if (g) {
        b.gameId = g->def->gameId;
        b.gameName = g->def->gameName;
        b.build = g->build ? g->build->name : "";
        b.buildNotes = g->build ? g->build->notes : "";
        b.sha256 = g->build ? g->build->sha256 : "";
        b.timestamp = g->timestamp;
        b.sizeOfImage = g->sizeOfImage;
        b.databaseVersion = g->def->databaseVersion;
        b.idCount = g->def->entryCount;
        b.resolvedCount = g->resolved;
        b.mismatchCount = g->mismatched;
        b.buildCount = g->def->buildCount;
    }
    std::memcpy(out, &b, size < sizeof b ? size : sizeof b);
    out->size = size;
    return g ? PSE_OK : PSE_E_NOT_FOUND;
}

const PSEIdsInterface kInterface = {sizeof(PSEIdsInterface), 1, &Resolve, &ResolveByName, &Name, &IdOf,
                                    &Info, &InfoAt, &BuildInfo};

// ---- the live process --------------------------------------------------------------------------------------------

bool GuardedCopy(void* out, const void* src, uint32_t len) {
    __try {
        std::memcpy(out, src, len);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadProcess(uint32_t va, void* out, uint32_t len, void*) {
    const auto* p = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(va));
    // Every page of the range must be committed and readable (VirtualQuery first: no first-chance faults normally).
    for (uintptr_t a = reinterpret_cast<uintptr_t>(p); a < reinterpret_cast<uintptr_t>(p) + len;) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return GuardedCopy(out, p, len);
}

uint32_t Score(const BuildDef& b, const ImageView& image) {
    uint32_t ok = 0;
    for (uint32_t i = 0; i < b.addressCount; ++i)
        if (b.addresses[i].checkVa && SignatureMatches(image, b.addresses[i].checkVa, b.addresses[i].signature)) ++ok;
    return ok;
}

}  // namespace

bool ValidSignature(const char* signature) {
    uint8_t bytes[16], mask[16];
    uint32_t len = 0;
    return Parse(signature, bytes, mask, &len);
}

bool SignatureMatches(const ImageView& image, uint32_t va, const char* signature) {
    uint8_t bytes[16], mask[16], got[16];
    uint32_t len = 0;
    if (!image.read || !Parse(signature, bytes, mask, &len)) return false;
    if (!image.read(va, got, len, image.ctx)) return false;
    for (uint32_t i = 0; i < len; ++i)
        if ((got[i] & mask[i]) != bytes[i]) return false;
    return true;
}

bool ProcessImage(ImageView* out) {
    if (!out) return false;
    *out = ImageView{};
    const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    IMAGE_DOS_HEADER dos;
    if (!ReadProcess(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base)), &dos, sizeof dos, nullptr) ||
        dos.e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    IMAGE_NT_HEADERS32 nt;
    if (!ReadProcess(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base) + dos.e_lfanew), &nt, sizeof nt, nullptr) ||
        nt.Signature != IMAGE_NT_SIGNATURE)
        return false;
    out->timestamp = nt.FileHeader.TimeDateStamp;
    // Database addresses are absolute VAs for the exe at its preferred base (the supported exes have no
    // relocations). A module somewhere else would need rebasing - report it as an unknown build instead.
    out->sizeOfImage = reinterpret_cast<uintptr_t>(base) == nt.OptionalHeader.ImageBase ? nt.OptionalHeader.SizeOfImage : 0;
    out->read = &ReadProcess;
    return true;
}

PSEResult RegisterGame(const GameDef& def, const ImageView& image, PSEPluginHandle owner, RegisterSummary* summary) {
    if (def.size < sizeof(GameDef) || !def.gameId || !*def.gameId || (!def.entries && def.entryCount) ||
        (!def.builds && def.buildCount))
        return PSE_E_INVALID_ARG;
    for (uint32_t i = 0; i < def.entryCount; ++i) {
        if (!def.entries[i].id || !def.entries[i].name || (i && def.entries[i].id <= def.entries[i - 1].id))
            return PSE_E_INVALID_ARG;   // ids must be non-zero, sorted, unique
    }

    g_logOwner = owner;
    auto game = std::make_unique<Game>();
    Game& g = *game;
    g.def = &def;
    g.timestamp = image.timestamp;
    g.sizeOfImage = image.sizeOfImage;
    g.va.assign(def.entryCount, 0);
    g.status.assign(def.entryCount, PSE_ID_UNKNOWN_BUILD);
    for (uint32_t i = 0; i < def.entryCount; ++i) g.byName.emplace(def.entries[i].name, i);

    // Build: PE timestamp + image size; when several builds share them (a cracked exe with the same header), the
    // one whose signatures match best wins.
    uint32_t bestScore = 0;
    for (uint32_t b = 0; b < def.buildCount; ++b) {
        const BuildDef& bd = def.builds[b];
        if (bd.timestamp != image.timestamp || bd.sizeOfImage != image.sizeOfImage || !image.sizeOfImage) continue;
        const uint32_t s = Score(bd, image);
        if (!g.build || s > bestScore) {
            g.build = &bd;
            bestScore = s;
        }
    }

    RegisterSummary sum;
    sum.ids = def.entryCount;
    if (!g.build) {
        Log(PSE_LOG_WARN, "pse.ids: %s: unknown exe (PE timestamp 0x%08X, image size 0x%X) - none of the %u ids "
            "resolve; plugins using them must switch off", def.gameId, image.timestamp, image.sizeOfImage,
            def.entryCount);
    } else {
        sum.build = g.build->name;
        g.status.assign(def.entryCount, PSE_ID_NOT_IN_BUILD);
        for (uint32_t a = 0; a < g.build->addressCount; ++a) {
            const AddressDef& ad = g.build->addresses[a];
            const int i = IndexOf(&g, ad.id);
            if (i < 0) continue;   // the generator never writes these; ignore rather than fail
            const bool unchecked = !ad.checkVa || !ad.signature || !*ad.signature;
            if (unchecked || SignatureMatches(image, ad.checkVa, ad.signature)) {
                g.va[static_cast<size_t>(i)] = ad.va;
                g.status[static_cast<size_t>(i)] = PSE_ID_OK;
                ++g.resolved;
            } else {
                g.status[static_cast<size_t>(i)] = PSE_ID_MISMATCH;
                ++g.mismatched;
                Log(PSE_LOG_WARN, "pse.ids: %s id %u %s: bytes at 0x%08X don't match the %s signature - resolves to 0",
                    def.gameId, def.entries[i].id, def.entries[i].name, ad.checkVa, g.build->name);
            }
        }
        sum.resolved = g.resolved;
        sum.mismatched = g.mismatched;
        sum.missing = def.entryCount - g.resolved - g.mismatched;
        Log(g.mismatched ? PSE_LOG_WARN : PSE_LOG_INFO,
            "pse.ids: %s database %u, build %s: %u ids, %u resolved, %u mismatched, %u not in this build", def.gameId,
            def.databaseVersion, g.build->name, def.entryCount, g.resolved, g.mismatched, sum.missing);
    }

    {
        std::lock_guard lock(g_mutex);
        bool replaced = false;
        for (Game*& slot : g_games) {
            if (std::strcmp(slot->def->gameId, def.gameId) == 0) {
                slot = game.get();
                replaced = true;
            }
        }
        if (!replaced) g_games.push_back(game.get());
        g_all.push_back(std::move(game));
    }
    // Served like every other interface: Query(PSE_IDS_INTERFACE, 1). Already there = fine.
    if (owner) {
        const PSEMessagingInterface* m = MessagingInterface();
        if (m) m->RegisterInterface(owner, PSE_IDS_INTERFACE, kInterface.version, &kInterface);
    }
    if (summary) *summary = sum;
    return PSE_OK;
}

PSEResult RegisterGameForProcess(const GameDef& def, PSEPluginHandle owner, RegisterSummary* out) {
    ImageView image;
    if (!ProcessImage(&image)) image = ImageView{};   // unreadable header: registered as an unknown build
    return RegisterGame(def, image, owner, out);
}

const PSEIdsInterface* Interface() { return &kInterface; }

void ResetForTests() {
    std::lock_guard lock(g_mutex);
    g_games.clear();
    g_all.clear();
    g_logOwner = 0;
}

}  // namespace pse::ids

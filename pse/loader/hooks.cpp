// pse.hooks - the hooks broker (docs/internal/DESIGN.md 5.2, reference docs/sdk/reference/hooks.md).
//
// Every byte the broker writes has an owner: a byte group (PatchGroup) or a hook site. Hook sites are shared - each
// subscriber is a link in a chain: site -> subscriber 0 -> subscriber 1 -> ... -> original. The chain lives in the
// subscribers' own `next` variables, so adding or removing one only rewrites its neighbours. Independent of the
// other loader services: names and log lines come through SetEnvironment's callbacks.
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "loader.h"
#include "x86len.h"

namespace {

// ---- environment ---------------------------------------------------------------------------------------------------

const char* (*g_nameFn)(PSEPluginHandle) = nullptr;
void (*g_logFn)(PSEPluginHandle, PSELogLevel, const char*) = nullptr;

const char* NameOf(PSEPluginHandle plugin) {
    if (g_nameFn) {
        const char* n = g_nameFn(plugin);
        if (n && *n) return n;
    }
    // Default names live for the whole run (ListHooks hands the pointers out).
    static std::map<PSEPluginHandle, std::string> names;
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    auto& s = names[plugin];
    if (s.empty()) s = "plugin#" + std::to_string(plugin);
    return s.c_str();
}

void Emit(PSEPluginHandle plugin, PSELogLevel level, const char* text) {
    if (g_logFn) {
        g_logFn(plugin, level, text);
        return;
    }
    std::string line = std::string("[pse.hooks] ") + NameOf(plugin) + ": " + text + "\n";
    OutputDebugStringA(line.c_str());
}

// ---- records -------------------------------------------------------------------------------------------------------

struct Patch {
    uint32_t va = 0, len = 0;
    std::vector<uint8_t> saved;    // restored by RevertGroup
    std::vector<uint8_t> patched;
    bool anyOriginal = false;
    std::string name;
};

struct Group {
    PSEPluginHandle plugin = 0;
    std::string name;
    std::vector<Patch> patches;
};

struct Sub {
    PSEHookId id = 0;
    PSEPluginHandle plugin = 0;
    void* fn = nullptr;
    void** next = nullptr;
    int32_t priority = 0;
    uint64_t seq = 0;
};

struct Site {
    PSEHookKind kind = PSE_HOOK_CALL_SITE;
    uint32_t va = 0, len = 0;
    std::vector<uint8_t> original;  // site bytes before the first subscriber
    uint32_t end = 0;               // where the last subscriber's next points: original target / trampoline / stub
    uint32_t extra = 0;             // call site: original target, indirect call: IAT slot
    std::vector<Sub> subs;          // chain order
};

struct Owner {
    Group* group = nullptr;
    Site* site = nullptr;
};

struct LogLine {
    PSEPluginHandle plugin;
    PSELogLevel level;
    std::string text;
};

struct State {
    std::mutex mutex;
    std::list<Group> groups;  // lists: Owner keeps raw pointers
    std::list<Site> sites;
    std::map<uint32_t, Owner> owned;  // per byte
    PSEHookId nextId = 1;
    uint64_t seq = 0;
    // Code arena: bump allocation over VirtualAlloc'd chunks, never freed while running.
    std::vector<void*> chunks;
    uint8_t* cur = nullptr;
    uint32_t left = 0;
};

State& S() {
    static State s;
    return s;
}

// Holds the broker lock; log lines are emitted after unlocking so a log callback may call back into the broker.
class Op {
public:
    Op() : lock_(S().mutex) {}
    ~Op() {
        lock_.unlock();
        for (auto& l : lines_) Emit(l.plugin, l.level, l.text.c_str());
    }
    void Log(PSEPluginHandle plugin, PSELogLevel level, const char* fmt, ...) {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, ap);
        va_end(ap);
        lines_.push_back({plugin, level, buf});
    }

private:
    std::unique_lock<std::mutex> lock_;
    std::vector<LogLine> lines_;
};

// ---- memory --------------------------------------------------------------------------------------------------------

DWORD ExecutableVersion(DWORD protect) {
    switch (protect & 0xFF) {
        case PAGE_READONLY: return PAGE_EXECUTE_READ;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY: return PAGE_EXECUTE_READWRITE;
        default: return protect;
    }
}

bool Readable(uint32_t va, uint32_t len) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!va || !VirtualQuery(reinterpret_cast<void*>(va), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<uint64_t>(va) + len <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool Matches(uint32_t va, const void* bytes, uint32_t len) {
    return Readable(va, len) && std::memcmp(reinterpret_cast<void*>(va), bytes, len) == 0;
}

// Like patch::Write: pages stay executable (caves also live in .rdata padding). Aligned dwords are stored in one go
// so a pointer slot never holds half an address.
bool Write(uint32_t va, const void* bytes, uint32_t len) {
    void* dst = reinterpret_cast<void*>(va);
    DWORD old;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old)) return false;
    if (len == 4 && (va & 3) == 0) {
        LONG v;
        std::memcpy(&v, bytes, 4);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(dst), v);
    } else {
        std::memcpy(dst, bytes, len);
    }
    VirtualProtect(dst, len, ExecutableVersion(old), &old);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return true;
}

void* Alloc(uint32_t size) {
    State& s = S();
    size = (size + 15) & ~15u;
    if (size > s.left) {
        const uint32_t chunk = size > 0x10000 ? (size + 0xFFFF) & ~0xFFFFu : 0x10000;
        void* p = VirtualAlloc(nullptr, chunk, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (!p) return nullptr;
        s.chunks.push_back(p);
        s.cur = static_cast<uint8_t*>(p);
        s.left = chunk;
        std::memset(p, 0xCC, chunk);  // int3 filler: a stray jump into unused arena traps at once
    }
    void* r = s.cur;
    s.cur += size;
    s.left -= size;
    return r;
}

void EncodeRel(uint8_t* code, uint8_t op, uint32_t at, uint32_t target) {
    code[0] = op;
    int32_t rel = static_cast<int32_t>(target - (at + 5));
    std::memcpy(code + 1, &rel, 4);
}

uint32_t RelTarget(uint32_t at) {  // target of the rel32 instruction (5 bytes) at `at`
    int32_t rel;
    std::memcpy(&rel, reinterpret_cast<void*>(at + 1), 4);
    return at + 5 + static_cast<uint32_t>(rel);
}

// ---- ownership -----------------------------------------------------------------------------------------------------

const char* KindName(PSEHookKind k) {
    switch (k) {
        case PSE_HOOK_BYTES: return "byte patch";
        case PSE_HOOK_CALL_SITE: return "call hook";
        case PSE_HOOK_FUNCTION: return "function hook";
        case PSE_HOOK_INDIRECT_CALL: return "indirect call hook";
        case PSE_HOOK_POINTER: return "pointer hook";
    }
    return "?";
}

PSEPluginHandle OwnerPlugin(const Owner& o) {
    if (o.group) return o.group->plugin;
    if (o.site && !o.site->subs.empty()) return o.site->subs.front().plugin;
    return 0;
}

// First owned byte in [va, va+len), or nullptr.
const Owner* FindOwned(uint32_t va, uint32_t len, uint32_t* at) {
    auto& owned = S().owned;
    auto it = owned.lower_bound(va);
    if (it == owned.end() || it->first >= va + len) return nullptr;
    if (at) *at = it->first;
    return &it->second;
}

void Claim(uint32_t va, uint32_t len, Owner o) {
    for (uint32_t i = 0; i < len; ++i) S().owned[va + i] = o;
}

void Release(uint32_t va, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) S().owned.erase(va + i);
}

// Logs "who wants what, and who has it" for a refused request. One line names both plugins.
void LogConflict(Op& op, PSEPluginHandle plugin, const char* what, uint32_t va, const Owner& o, uint32_t at) {
    std::string held;
    if (o.group) held = std::string("byte group '") + o.group->name + "'";
    else held = std::string(KindName(o.site->kind)) + " at 0x" + [&] {
        char b[16];
        std::snprintf(b, sizeof b, "%08X", o.site->va);
        return std::string(b);
    }();
    op.Log(plugin, PSE_LOG_ERROR, "[conflict] %s (%s) at 0x%08X: byte 0x%08X is owned by %s (%s)", what,
           NameOf(plugin), va, at, NameOf(OwnerPlugin(o)), held.c_str());
}

// ---- byte groups ---------------------------------------------------------------------------------------------------

Group* FindGroup(PSEPluginHandle plugin, const char* name) {
    for (auto& g : S().groups)
        if (g.plugin == plugin && g.name == name) return &g;
    return nullptr;
}

PSEResult RevertLocked(Op& op, Group& g) {
    for (const Patch& p : g.patches) {
        if (!Readable(p.va, p.len)) return PSE_E_ACCESS;
        if (!p.anyOriginal && !Matches(p.va, p.patched.data(), p.len) && !Matches(p.va, p.saved.data(), p.len)) {
            op.Log(g.plugin, PSE_LOG_ERROR, "[fail] %s: unexpected bytes at 0x%08X (%s) - not reverted", g.name.c_str(),
                   p.va, p.name.c_str());
            return PSE_E_MISMATCH;
        }
    }
    for (const Patch& p : g.patches) {
        if (!Write(p.va, p.saved.data(), p.len)) return PSE_E_ACCESS;
        Release(p.va, p.len);
    }
    op.Log(g.plugin, PSE_LOG_INFO, "[off]  %s: reverted", g.name.c_str());
    for (auto it = S().groups.begin(); it != S().groups.end(); ++it)
        if (&*it == &g) {
            S().groups.erase(it);
            break;
        }
    return PSE_OK;
}

PSEResult ApplyLocked(Op& op, PSEPluginHandle plugin, const char* group, const PSEBytePatch* patches, uint32_t count) {
    // Ownership first: nothing is touched when any byte belongs to someone else.
    for (uint32_t i = 0; i < count; ++i) {
        const PSEBytePatch& p = patches[i];
        uint32_t at;
        if (const Owner* o = FindOwned(p.va, p.len, &at)) {
            LogConflict(op, plugin, group, p.va, *o, at);
            return PSE_E_CONFLICT;
        }
        for (uint32_t j = 0; j < i; ++j)
            if (p.va < patches[j].va + patches[j].len && patches[j].va < p.va + p.len) {
                op.Log(plugin, PSE_LOG_ERROR, "[fail] %s: patches at 0x%08X and 0x%08X overlap", group, patches[j].va,
                       p.va);
                return PSE_E_INVALID_ARG;
            }
    }
    Group g;
    g.plugin = plugin;
    g.name = group;
    unsigned already = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const PSEBytePatch& p = patches[i];
        if (!Readable(p.va, p.len)) {
            op.Log(plugin, PSE_LOG_ERROR, "[fail] %s: 0x%08X is not mapped", group, p.va);
            return PSE_E_ACCESS;
        }
        const uint8_t* cur = reinterpret_cast<const uint8_t*>(p.va);
        Patch rec;
        rec.va = p.va;
        rec.len = p.len;
        rec.anyOriginal = p.anyOriginal != 0;
        rec.name = p.name ? p.name : "";
        rec.patched.assign(p.patched, p.patched + p.len);
        if (std::memcmp(cur, p.patched, p.len) == 0) {
            ++already;  // e.g. the exe file already carries it: revert still writes the original
        } else if (!p.anyOriginal && std::memcmp(cur, p.original, p.len) != 0) {
            op.Log(plugin, PSE_LOG_ERROR,
                   "[fail] %s: unexpected bytes at 0x%08X (%s) - different exe version? Group not applied.", group,
                   p.va, p.name ? p.name : "");
            return PSE_E_MISMATCH;
        }
        if (p.anyOriginal) rec.saved.assign(cur, cur + p.len);
        else rec.saved.assign(p.original, p.original + p.len);
        g.patches.push_back(std::move(rec));
    }
    for (size_t i = 0; i < g.patches.size(); ++i) {
        if (!Write(g.patches[i].va, g.patches[i].patched.data(), g.patches[i].len)) {
            for (size_t j = 0; j < i; ++j) Write(g.patches[j].va, g.patches[j].saved.data(), g.patches[j].len);
            op.Log(plugin, PSE_LOG_ERROR, "[fail] %s: VirtualProtect failed at 0x%08X (error %lu)", group,
                   g.patches[i].va, GetLastError());
            return PSE_E_ACCESS;
        }
    }
    Group& stored = S().groups.emplace_back(std::move(g));
    for (const Patch& p : stored.patches) Claim(p.va, p.len, Owner{&stored, nullptr});
    op.Log(plugin, PSE_LOG_INFO, "[ok]   %s: %u patches (%u already in place)", group, count, already);
    return PSE_OK;
}

bool SameSet(const Group& g, const PSEBytePatch* patches, uint32_t count) {
    if (g.patches.size() != count) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const Patch& a = g.patches[i];
        const PSEBytePatch& b = patches[i];
        if (a.va != b.va || a.len != b.len || std::memcmp(a.patched.data(), b.patched, b.len) != 0) return false;
    }
    return true;
}

// ---- hook chains ---------------------------------------------------------------------------------------------------

// Site bytes that send execution to `fn`.
std::vector<uint8_t> HeadBytes(const Site& s, uint32_t fn) {
    std::vector<uint8_t> b(s.len, 0xCC);
    switch (s.kind) {
        case PSE_HOOK_CALL_SITE: EncodeRel(b.data(), 0xE8, s.va, fn); break;
        case PSE_HOOK_FUNCTION: EncodeRel(b.data(), 0xE9, s.va, fn); break;  // rest stays int3
        case PSE_HOOK_INDIRECT_CALL:
            EncodeRel(b.data(), 0xE8, s.va, fn);
            b[5] = 0x90;
            break;
        case PSE_HOOK_POINTER: std::memcpy(b.data(), &fn, 4); break;
        default: break;
    }
    return b;
}

void StoreNext(void** next, void* value) {
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(next), value);
}

// Brings every next variable and the site in line with subs. Back to front: a link is rewritten only after the one it
// points to is already right, so a thread running the chain always reaches a live subscriber or the original.
bool Rewire(Site& s) {
    for (size_t i = s.subs.size(); i-- > 0;) {
        void* want = i + 1 < s.subs.size() ? s.subs[i + 1].fn : reinterpret_cast<void*>(s.end);
        if (*s.subs[i].next != want) StoreNext(s.subs[i].next, want);
    }
    const std::vector<uint8_t> head =
        s.subs.empty() ? s.original : HeadBytes(s, reinterpret_cast<uint32_t>(s.subs.front().fn));
    if (Matches(s.va, head.data(), s.len)) return true;
    return Write(s.va, head.data(), s.len);
}

Site* FindSite(PSEHookKind kind, uint32_t va) {
    for (auto& s : S().sites)
        if (s.kind == kind && s.va == va) return &s;
    return nullptr;
}

bool NextInUse(void** next) {
    for (auto& s : S().sites)
        for (auto& sub : s.subs)
            if (sub.next == next) return true;
    return false;
}

struct HookArgs {
    PSEPluginHandle plugin;
    void* fn;
    int32_t priority;
    void** next;
    PSEHookId* outId;
};

PSEResult CheckHookArgs(Op& op, const HookArgs& a, const char* what) {
    if (!a.plugin || !a.fn || !a.next || (reinterpret_cast<uintptr_t>(a.next) & 3)) {
        op.Log(a.plugin, PSE_LOG_ERROR, "[fail] %s: plugin, fn and an aligned next variable are required", what);
        return PSE_E_INVALID_ARG;
    }
    if (NextInUse(a.next)) {
        op.Log(a.plugin, PSE_LOG_ERROR, "[fail] %s: this next variable already belongs to another hook", what);
        return PSE_E_EXISTS;
    }
    return PSE_OK;
}

// The site's bytes must be free, or owned by exactly this site (then the new subscriber joins its chain).
PSEResult CheckSiteBytes(Op& op, const HookArgs& a, const char* what, uint32_t va, uint32_t len, Site* existing) {
    for (uint32_t i = 0; i < len; ++i) {
        auto it = S().owned.find(va + i);
        if (it == S().owned.end()) continue;
        if (existing && it->second.site == existing) continue;
        LogConflict(op, a.plugin, what, va, it->second, va + i);
        return PSE_E_CONFLICT;
    }
    return PSE_OK;
}

// Adds a subscriber in priority order (higher first, ties by registration) and installs the site if new.
PSEResult Subscribe(Op& op, Site* site, bool isNew, Site&& fresh, const HookArgs& a) {
    State& st = S();
    if (isNew) {
        st.sites.push_back(std::move(fresh));
        site = &st.sites.back();
    }
    Sub sub;
    sub.id = st.nextId++;
    sub.plugin = a.plugin;
    sub.fn = a.fn;
    sub.next = a.next;
    sub.priority = a.priority;
    sub.seq = ++st.seq;
    size_t pos = 0;
    while (pos < site->subs.size() && site->subs[pos].priority >= a.priority) ++pos;
    site->subs.insert(site->subs.begin() + pos, sub);
    if (!Rewire(*site)) {
        site->subs.erase(site->subs.begin() + pos);
        Rewire(*site);
        if (isNew) st.sites.pop_back();
        op.Log(a.plugin, PSE_LOG_ERROR, "[fail] %s at 0x%08X: VirtualProtect failed (error %lu)", KindName(site->kind),
               site->va, GetLastError());
        return PSE_E_ACCESS;
    }
    if (isNew) Claim(site->va, site->len, Owner{nullptr, site});
    if (a.outId) *a.outId = sub.id;
    op.Log(a.plugin, PSE_LOG_INFO, "[hook] %s at 0x%08X: id %u, priority %d, position %u of %u", KindName(site->kind),
           site->va, sub.id, a.priority, static_cast<unsigned>(pos), static_cast<unsigned>(site->subs.size()));
    return PSE_OK;
}

// ---- function detours ----------------------------------------------------------------------------------------------

// Copies the instructions of [entry, entry+len) (original bytes `orig`) to `out` as if placed at `at`, relocating
// relative branches, and appends jmp entry+len. Returns the size, or 0 with *err set. `out` may be null (sizing).
uint32_t BuildTrampoline(const uint8_t* orig, uint32_t len, uint32_t entry, uint32_t at, uint8_t* out,
                         PSEResult* err) {
    uint32_t o = 0;
    auto put = [&](uint8_t b) {
        if (out) out[o] = b;
        ++o;
    };
    auto putRel = [&](uint32_t target) {  // rel32 at the end of an instruction
        int32_t rel = static_cast<int32_t>(target - (at + o + 4));
        for (int k = 0; k < 4; ++k) put(static_cast<uint8_t>(static_cast<uint32_t>(rel) >> (8 * k)));
    };
    for (uint32_t off = 0; off < len;) {
        pse::x86::Insn in;
        const uint32_t n = pse::x86::Decode(orig + off, len - off, &in);
        if (!n) {
            *err = PSE_E_UNSUPPORTED;
            return 0;
        }
        if (in.relLen) {
            const uint8_t* rel = orig + off + n - in.relLen;
            int32_t d = 0;
            if (in.relLen == 1) d = static_cast<int8_t>(rel[0]);
            else if (in.relLen == 4) std::memcpy(&d, rel, 4);
            const uint32_t target = entry + off + n + static_cast<uint32_t>(d);
            const bool loopLike = in.escape == 0 && in.opcode >= 0xE0 && in.opcode <= 0xE3;
            // rel16 (66 prefix) and loop / jecxz have no rel32 form; a target inside the moved bytes would land in
            // the detour jump.
            if (in.relLen == 2 || loopLike || (target >= entry && target < entry + len)) {
                *err = PSE_E_UNSUPPORTED;
                return 0;
            }
            for (uint32_t k = 0; k < in.prefixLen; ++k) put(orig[off + k]);  // e.g. branch hints
            if (in.escape == 0x0F) {
                put(0x0F);
                put(in.opcode);
            } else if (in.opcode >= 0x70 && in.opcode <= 0x7F) {
                put(0x0F);
                put(static_cast<uint8_t>(0x80 + (in.opcode - 0x70)));
            } else if (in.opcode == 0xEB) {
                put(0xE9);
            } else {
                put(in.opcode);  // E8 / E9
            }
            putRel(target);
        } else {
            for (uint32_t k = 0; k < n; ++k) put(orig[off + k]);
        }
        off += n;
    }
    put(0xE9);
    putRel(entry + len);
    return o;
}

// Prologue length must cover whole instructions of the original bytes.
bool OnBoundary(const uint8_t* orig, uint32_t avail, uint32_t len, bool* undecodable) {
    *undecodable = false;
    uint32_t off = 0;
    while (off < len) {
        uint32_t n = pse::x86::Length(orig + off, avail - off);
        if (!n) {
            *undecodable = true;
            return false;
        }
        off += n;
    }
    return off == len;
}

// ---- interface functions -------------------------------------------------------------------------------------------

PSEResult PSE_CALL PatchGroup(PSEPluginHandle plugin, const char* group, const PSEBytePatch* patches, uint32_t count) {
    Op op;
    if (!plugin || !group || (count && !patches)) return PSE_E_INVALID_ARG;
    for (uint32_t i = 0; i < count; ++i) {
        const PSEBytePatch& p = patches[i];
        if (!p.va || !p.len || !p.patched || (!p.anyOriginal && !p.original)) {
            op.Log(plugin, PSE_LOG_ERROR, "[fail] %s: patch %u is incomplete", group, i);
            return PSE_E_INVALID_ARG;
        }
    }
    if (Group* old = FindGroup(plugin, group)) {
        // Same group again: nothing to do when it is the same set and still in place (Apply* functions are
        // idempotent); a changed set (tunable values) replaces the old one, which comes back if the new one fails.
        bool inPlace = SameSet(*old, patches, count);
        for (const Patch& p : old->patches) inPlace = inPlace && Matches(p.va, p.patched.data(), p.len);
        if (inPlace) return PSE_OK;
        std::vector<PSEBytePatch> undo;
        std::vector<Patch> keep = old->patches;
        PSEResult r = RevertLocked(op, *old);
        if (r != PSE_OK) return r;
        r = ApplyLocked(op, plugin, group, patches, count);
        if (r != PSE_OK) {
            for (const Patch& p : keep)
                undo.push_back({p.va, p.saved.data(), p.patched.data(), p.len, 0, p.name.c_str()});
            ApplyLocked(op, plugin, group, undo.data(), static_cast<uint32_t>(undo.size()));
        }
        return r;
    }
    return ApplyLocked(op, plugin, group, patches, count);
}

PSEResult PSE_CALL RevertGroup(PSEPluginHandle plugin, const char* group) {
    Op op;
    if (!plugin || !group) return PSE_E_INVALID_ARG;
    Group* g = FindGroup(plugin, group);
    if (!g) return PSE_E_NOT_FOUND;
    return RevertLocked(op, *g);
}

PSEResult PSE_CALL HookCallSite(PSEPluginHandle plugin, uint32_t site, uint32_t expectedTarget, void* fn,
                                int32_t priority, void** next, PSEHookId* outId) {
    Op op;
    const HookArgs a{plugin, fn, priority, next, outId};
    if (PSEResult r = CheckHookArgs(op, a, "call hook"); r != PSE_OK) return r;
    if (!site) return PSE_E_INVALID_ARG;
    Site* existing = FindSite(PSE_HOOK_CALL_SITE, site);
    if (PSEResult r = CheckSiteBytes(op, a, "call hook", site, 5, existing); r != PSE_OK) return r;
    if (existing) {
        if (existing->extra != expectedTarget) {
            op.Log(plugin, PSE_LOG_ERROR, "[fail] call hook at 0x%08X: originally calls 0x%08X, not 0x%08X", site,
                   existing->extra, expectedTarget);
            return PSE_E_MISMATCH;
        }
        return Subscribe(op, existing, false, Site{}, a);
    }
    const uint8_t* p = reinterpret_cast<const uint8_t*>(site);
    if (!Readable(site, 5) || p[0] != 0xE8 || RelTarget(site) != expectedTarget) {
        op.Log(plugin, PSE_LOG_ERROR, "[fail] call hook at 0x%08X: does not call 0x%08X", site, expectedTarget);
        return PSE_E_MISMATCH;
    }
    Site s;
    s.kind = PSE_HOOK_CALL_SITE;
    s.va = site;
    s.len = 5;
    s.original.assign(p, p + 5);
    s.end = s.extra = expectedTarget;
    return Subscribe(op, nullptr, true, std::move(s), a);
}

PSEResult PSE_CALL HookFunction(PSEPluginHandle plugin, uint32_t entry, uint32_t prologueLen,
                                const uint8_t* expectedPrologue, void* fn, int32_t priority, void** next,
                                PSEHookId* outId) {
    Op op;
    const HookArgs a{plugin, fn, priority, next, outId};
    if (PSEResult r = CheckHookArgs(op, a, "function hook"); r != PSE_OK) return r;
    if (!entry || prologueLen < 5 || prologueLen > 64) {
        op.Log(plugin, PSE_LOG_ERROR, "[fail] function hook at 0x%08X: prologue length %u (5..64)", entry,
               prologueLen);
        return PSE_E_INVALID_ARG;
    }
    Site* existing = FindSite(PSE_HOOK_FUNCTION, entry);
    if (existing) {
        // A later subscriber joins the chain: its view of the prologue is checked against the saved original.
        if (prologueLen > existing->len) {
            op.Log(plugin, PSE_LOG_ERROR,
                   "[fail] function hook at 0x%08X: already detoured over %u bytes, %u requested", entry,
                   existing->len, prologueLen);
            return PSE_E_UNSUPPORTED;
        }
        bool undecodable;
        if (!OnBoundary(existing->original.data(), existing->len, prologueLen, &undecodable)) {
            op.Log(plugin, PSE_LOG_ERROR, "[fail] function hook at 0x%08X: %u bytes is not an instruction boundary",
                   entry, prologueLen);
            return undecodable ? PSE_E_UNSUPPORTED : PSE_E_INVALID_ARG;
        }
        if (expectedPrologue && std::memcmp(existing->original.data(), expectedPrologue, prologueLen) != 0) {
            op.Log(plugin, PSE_LOG_ERROR, "[fail] function hook at 0x%08X: prologue differs", entry);
            return PSE_E_MISMATCH;
        }
        if (PSEResult r = CheckSiteBytes(op, a, "function hook", entry, existing->len, existing); r != PSE_OK)
            return r;
        return Subscribe(op, existing, false, Site{}, a);
    }
    if (PSEResult r = CheckSiteBytes(op, a, "function hook", entry, prologueLen, nullptr); r != PSE_OK) return r;
    if (!Readable(entry, prologueLen)) return PSE_E_ACCESS;
    // Decode with up to 15 bytes of look-ahead so a too-short length shows up as "not on a boundary".
    uint8_t code[64 + 15];
    uint32_t avail = Readable(entry, prologueLen + 15) ? prologueLen + 15 : prologueLen;
    std::memcpy(code, reinterpret_cast<const void*>(entry), avail);
    if (expectedPrologue && std::memcmp(code, expectedPrologue, prologueLen) != 0) {
        op.Log(plugin, PSE_LOG_ERROR, "[fail] function hook at 0x%08X: prologue differs - different exe version?",
               entry);
        return PSE_E_MISMATCH;
    }
    bool undecodable;
    if (!OnBoundary(code, avail, prologueLen, &undecodable)) {
        op.Log(plugin, PSE_LOG_ERROR, "[fail] function hook at 0x%08X: %s", entry,
               undecodable ? "undecodable instruction in the prologue" : "length is not an instruction boundary");
        return undecodable ? PSE_E_UNSUPPORTED : PSE_E_INVALID_ARG;
    }
    PSEResult err = PSE_OK;
    const uint32_t size = BuildTrampoline(code, prologueLen, entry, 0, nullptr, &err);
    if (!size) {
        op.Log(plugin, PSE_LOG_ERROR,
               "[fail] function hook at 0x%08X: prologue can't be moved (loop/jecxz, rel16 or a branch into it)",
               entry);
        return err;
    }
    auto* tramp = static_cast<uint8_t*>(Alloc(size));
    if (!tramp) return PSE_E_NO_MEMORY;
    BuildTrampoline(code, prologueLen, entry, reinterpret_cast<uint32_t>(tramp), tramp, &err);
    FlushInstructionCache(GetCurrentProcess(), tramp, size);
    Site s;
    s.kind = PSE_HOOK_FUNCTION;
    s.va = entry;
    s.len = prologueLen;
    s.original.assign(code, code + prologueLen);
    s.end = reinterpret_cast<uint32_t>(tramp);
    return Subscribe(op, nullptr, true, std::move(s), a);
}

PSEResult PSE_CALL HookIndirectCall(PSEPluginHandle plugin, uint32_t site, uint32_t iatSlot, void* fn,
                                    int32_t priority, void** next, PSEHookId* outId) {
    Op op;
    const HookArgs a{plugin, fn, priority, next, outId};
    if (PSEResult r = CheckHookArgs(op, a, "indirect call hook"); r != PSE_OK) return r;
    if (!site || !iatSlot) return PSE_E_INVALID_ARG;
    Site* existing = FindSite(PSE_HOOK_INDIRECT_CALL, site);
    if (PSEResult r = CheckSiteBytes(op, a, "indirect call hook", site, 6, existing); r != PSE_OK) return r;
    if (existing) {
        if (existing->extra != iatSlot) {
            op.Log(plugin, PSE_LOG_ERROR, "[fail] indirect call hook at 0x%08X: calls [0x%08X], not [0x%08X]", site,
                   existing->extra, iatSlot);
            return PSE_E_MISMATCH;
        }
        return Subscribe(op, existing, false, Site{}, a);
    }
    uint8_t code[6] = {0xFF, 0x15};
    std::memcpy(code + 2, &iatSlot, 4);
    if (!Matches(site, code, 6)) {
        op.Log(plugin, PSE_LOG_ERROR, "[fail] indirect call hook at 0x%08X: does not call [0x%08X]", site, iatSlot);
        return PSE_E_MISMATCH;
    }
    // The chain ends in `jmp [iatSlot]`, so it follows whatever the slot holds at call time.
    auto* stub = static_cast<uint8_t*>(Alloc(6));
    if (!stub) return PSE_E_NO_MEMORY;
    stub[0] = 0xFF;
    stub[1] = 0x25;
    std::memcpy(stub + 2, &iatSlot, 4);
    FlushInstructionCache(GetCurrentProcess(), stub, 6);
    Site s;
    s.kind = PSE_HOOK_INDIRECT_CALL;
    s.va = site;
    s.len = 6;
    s.original.assign(code, code + 6);
    s.end = reinterpret_cast<uint32_t>(stub);
    s.extra = iatSlot;
    return Subscribe(op, nullptr, true, std::move(s), a);
}

PSEResult PSE_CALL HookPointer(PSEPluginHandle plugin, uint32_t slot, void* fn, int32_t priority, void** next,
                               PSEHookId* outId) {
    Op op;
    const HookArgs a{plugin, fn, priority, next, outId};
    if (PSEResult r = CheckHookArgs(op, a, "pointer hook"); r != PSE_OK) return r;
    if (!slot) return PSE_E_INVALID_ARG;
    Site* existing = FindSite(PSE_HOOK_POINTER, slot);
    if (PSEResult r = CheckSiteBytes(op, a, "pointer hook", slot, 4, existing); r != PSE_OK) return r;
    if (existing) return Subscribe(op, existing, false, Site{}, a);
    if (!Readable(slot, 4)) return PSE_E_ACCESS;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(slot);
    Site s;
    s.kind = PSE_HOOK_POINTER;
    s.va = slot;
    s.len = 4;
    s.original.assign(p, p + 4);
    std::memcpy(&s.end, p, 4);
    return Subscribe(op, nullptr, true, std::move(s), a);
}

PSEResult PSE_CALL Unhook(PSEPluginHandle plugin, PSEHookId id) {
    Op op;
    if (!plugin || !id) return PSE_E_INVALID_ARG;
    for (auto it = S().sites.begin(); it != S().sites.end(); ++it) {
        Site& s = *it;
        for (size_t i = 0; i < s.subs.size(); ++i) {
            if (s.subs[i].id != id) continue;
            if (s.subs[i].plugin != plugin) {
                op.Log(plugin, PSE_LOG_ERROR, "[fail] unhook %u: the hook belongs to %s", id,
                       NameOf(s.subs[i].plugin));
                return PSE_E_CONFLICT;
            }
            Sub removed = s.subs[i];
            s.subs.erase(s.subs.begin() + i);
            if (s.subs.empty() && !Matches(s.va, HeadBytes(s, reinterpret_cast<uint32_t>(removed.fn)).data(), s.len))
                op.Log(plugin, PSE_LOG_WARN, "[warn] %s at 0x%08X was changed outside the broker; restoring anyway",
                       KindName(s.kind), s.va);
            if (!Rewire(s)) {
                s.subs.insert(s.subs.begin() + i, removed);
                Rewire(s);
                return PSE_E_ACCESS;
            }
            // The removed hook's own next keeps pointing at its old successor: a thread still inside it finishes
            // through the chain.
            op.Log(plugin, PSE_LOG_INFO, "[unhook] %s at 0x%08X: id %u%s", KindName(s.kind), s.va, id,
                   s.subs.empty() ? " (site restored)" : "");
            if (s.subs.empty()) {
                Release(s.va, s.len);
                S().sites.erase(it);  // a function hook's trampoline stays in the arena (a thread may be in it)
            }
            return PSE_OK;
        }
    }
    return PSE_E_NOT_FOUND;
}

void* PSE_CALL AllocCode(PSEPluginHandle plugin, uint32_t size) {
    if (!plugin || !size || size > 0x1000000) return nullptr;
    Op op;
    return Alloc(size);
}

uint32_t PSE_CALL ListHooks(PSEHookInfo* out, uint32_t max) {
    Op op;
    uint32_t n = 0;
    auto add = [&](const PSEHookInfo& info) {
        if (out && n < max) out[n] = info;
        ++n;
    };
    for (const Group& g : S().groups)
        for (const Patch& p : g.patches) {
            PSEHookInfo info{};
            info.size = sizeof info;
            info.kind = PSE_HOOK_BYTES;
            info.owner = g.plugin;
            info.ownerName = NameOf(g.plugin);
            info.name = g.name.c_str();
            info.va = p.va;
            info.len = p.len;
            add(info);
        }
    for (const Site& s : S().sites)
        for (size_t i = 0; i < s.subs.size(); ++i) {
            PSEHookInfo info{};
            info.size = sizeof info;
            info.kind = s.kind;
            info.id = s.subs[i].id;
            info.owner = s.subs[i].plugin;
            info.ownerName = NameOf(s.subs[i].plugin);
            info.va = s.va;
            info.len = s.len;
            info.priority = s.subs[i].priority;
            info.chainPosition = static_cast<uint32_t>(i);
            add(info);
        }
    return n;
}

PSEPluginHandle PSE_CALL OwnerOf(uint32_t va) {
    Op op;
    auto it = S().owned.find(va);
    return it == S().owned.end() ? 0 : OwnerPlugin(it->second);
}

}  // namespace

void pse::hooks::SetEnvironment(const char* (*pluginName)(PSEPluginHandle),
                                void (*log)(PSEPluginHandle, PSELogLevel, const char*)) {
    std::lock_guard<std::mutex> lock(S().mutex);
    g_nameFn = pluginName;
    g_logFn = log;
}

void pse::hooks::ResetForTests() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.groups.clear();
    s.sites.clear();
    s.owned.clear();
    s.nextId = 1;
    s.seq = 0;
    for (void* c : s.chunks) VirtualFree(c, 0, MEM_RELEASE);
    s.chunks.clear();
    s.cur = nullptr;
    s.left = 0;
}

const PSEHooksInterface* pse::HooksInterface() {
    static const PSEHooksInterface table = {
        sizeof(PSEHooksInterface), 1, &PatchGroup, &RevertGroup, &HookCallSite, &HookFunction, &HookIndirectCall,
        &HookPointer, &Unhook, &AllocCode, &ListHooks, &OwnerOf,
    };
    return &table;
}

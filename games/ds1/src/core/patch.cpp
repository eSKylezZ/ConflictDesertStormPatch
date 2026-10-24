#include "core/patch.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/pse_host.h"
#include "loader.h"
#include "x86len.h"

// Every patch made here goes through the PSE hooks broker (pse.hooks): byte groups, call hooks, indirect call hooks
// and jumps get an owner (the active OwnerScope's plugin, else "dsfix"), an overlap with bytes someone else owns is
// noticed instead of silently overwriting them, and the dev console / test server can list them. The functions keep
// the semantics they had before the broker (see patch.h); docs/sdk/reference/hooks.md "DS1 host" has the rules.
namespace {
DWORD ExecutableVersion(DWORD protect) {
    switch (protect & 0xFF) {
        case PAGE_READONLY: return PAGE_EXECUTE_READ;
        case PAGE_READWRITE:
        case PAGE_WRITECOPY: return PAGE_EXECUTE_READWRITE;
        default: return protect;
    }
}

bool Readable(uint32_t va, size_t len) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<void*>(va), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return va + len <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

void EncodeRel(uint8_t (&code)[5], uint8_t op, uint32_t site, uint32_t target) {
    code[0] = op;
    int32_t rel = static_cast<int32_t>(target - (site + 5));
    std::memcpy(code + 1, &rel, 4);
}

// Target of the `call rel32` at site, or 0 when the site holds something else.
uint32_t CallTarget(uint32_t site) {
    if (!Readable(site, 5)) return 0;
    const auto* p = reinterpret_cast<const uint8_t*>(site);
    if (p[0] != 0xE8) return 0;
    int32_t rel;
    std::memcpy(&rel, p + 1, 4);
    return site + 5 + static_cast<uint32_t>(rel);
}

bool RawWrite(uint32_t va, const void* bytes, size_t len) {
    void* dst = reinterpret_cast<void*>(va);
    DWORD old;
    if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &old)) {
        dslog::Write("  VirtualProtect failed at 0x%08X (error %lu)", va, GetLastError());
        return false;
    }
    std::memcpy(dst, bytes, len);
    VirtualProtect(dst, len, ExecutableVersion(old), &old);
    FlushInstructionCache(GetCurrentProcess(), dst, len);
    return true;
}

const PSEHooksInterface& H() { return *pse::HooksInterface(); }

const char* Name(PSEPluginHandle p) {
    const char* n = pse::PluginName(p);
    return n && *n ? n : "?";
}

// ---- records of what this file put into the broker --------------------------------------------------------------

struct Sub {
    PSEHookId id = 0;
    PSEPluginHandle owner = 0;
    uint32_t fn = 0;
    int32_t priority = 0;
    void** next = nullptr;
    bool ownNext = false;  // next slot handed out by this file (returned to the pool on unhook)
};
struct HookSite {
    PSEHookKind kind = PSE_HOOK_CALL_SITE;
    uint32_t len = 5;
    // call site: original target; indirect call: IAT slot; pointer: the slot's original value; function: 0
    uint32_t original = 0;
    std::vector<Sub> subs;  // registration order (the broker keeps the chain order)
};
struct Jump {  // a jmp@ / call@ code group made by WriteJump / WriteCall
    PSEPluginHandle owner = 0;
    std::string group;
    uint32_t len = 5;
};

struct State {
    std::recursive_mutex m;
    std::map<uint32_t, HookSite> hooks;              // by site
    std::map<uint32_t, Jump> jumps;                  // by site (5 bytes)
    std::map<std::string, PSEPluginHandle> groups;   // byte group name -> owner (kept after a revert)
    std::map<uint32_t, PSEPluginHandle> siteOwners;  // hook / jump site -> last owner (kept after an unhook)
    std::deque<void*> slots;                         // internal next variables (stable addresses)
    std::vector<void**> freeSlots;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> rawSlots;  // HookSlot written outside the broker: {fn, old}
};
State& S() {
    static State s;
    return s;
}

thread_local PSEPluginHandle t_owner = 0;

// Owner for a new hook / jump at `site`: the active scope; outside any scope the site's previous owner (a feature
// re-hooking at run time, e.g. after a live toggle), else "dsfix".
PSEPluginHandle OwnerForSite(uint32_t site) {
    if (t_owner) return t_owner;
    auto it = S().siteOwners.find(site);
    return it != S().siteOwners.end() ? it->second : patch::CurrentOwner();
}

void** NewSlot() {
    State& s = S();
    if (!s.freeSlots.empty()) {
        void** p = s.freeSlots.back();
        s.freeSlots.pop_back();
        *p = nullptr;
        return p;
    }
    s.slots.push_back(nullptr);
    return &s.slots.back();
}
void FreeSlot(void** p) { S().freeSlots.push_back(p); }

// The broker's view of one hook site: subscriber id -> chain position (0 runs first).
std::map<PSEHookId, uint32_t> ChainOf(uint32_t site, PSEHookKind kind) {
    std::map<PSEHookId, uint32_t> out;
    const uint32_t n = H().ListHooks(nullptr, 0);
    std::vector<PSEHookInfo> list(n);
    const uint32_t got = H().ListHooks(list.data(), n);
    for (uint32_t i = 0; i < got && i < n; ++i)
        if (list[i].kind == kind && list[i].va == site) out[list[i].id] = list[i].chainPosition;
    return out;
}

// Subscribers of our record in chain order.
std::vector<Sub*> Ordered(uint32_t site, HookSite& h) {
    const auto chain = ChainOf(site, h.kind);
    std::vector<Sub*> out(h.subs.size(), nullptr);
    std::vector<Sub*> extra;
    for (Sub& s : h.subs) {
        auto it = chain.find(s.id);
        if (it != chain.end() && it->second < out.size() && !out[it->second]) out[it->second] = &s;
        else extra.push_back(&s);
    }
    std::vector<Sub*> r;
    for (Sub* s : out)
        if (s) r.push_back(s);
    r.insert(r.end(), extra.begin(), extra.end());
    return r;
}

bool UnhookSub(uint32_t site, HookSite& h, PSEHookId id) {
    for (size_t i = 0; i < h.subs.size(); ++i) {
        if (h.subs[i].id != id) continue;
        if (H().Unhook(h.subs[i].owner, id) != PSE_OK) return false;
        if (h.subs[i].ownNext) FreeSlot(h.subs[i].next);
        h.subs.erase(h.subs.begin() + i);
        (void)site;
        return true;
    }
    return false;
}

// Removes every hook of ours at the site (the broker restores the original bytes when the last one goes).
bool UnhookAll(uint32_t site) {
    State& s = S();
    auto it = s.hooks.find(site);
    if (it == s.hooks.end()) return true;
    bool ok = true;
    std::vector<PSEHookId> ids;  // front to back: the site always reaches a live hook
    for (Sub* sub : Ordered(site, it->second)) ids.push_back(sub->id);
    for (PSEHookId id : ids) ok = UnhookSub(site, it->second, id) && ok;
    if (it->second.subs.empty()) s.hooks.erase(it);
    return ok;
}

bool RevertJump(uint32_t site) {
    State& s = S();
    auto it = s.jumps.find(site);
    if (it == s.jumps.end()) return true;
    const PSEResult r = H().RevertGroup(it->second.owner, it->second.group.c_str());
    if (r != PSE_OK && r != PSE_E_NOT_FOUND) return false;
    s.jumps.erase(it);
    return true;
}

// Owner of the first byte in [va, va+len) the broker knows, or 0.
PSEPluginHandle FirstOwned(uint32_t va, size_t len, uint32_t* at) {
    for (size_t i = 0; i < len; ++i)
        if (PSEPluginHandle o = H().OwnerOf(va + static_cast<uint32_t>(i))) {
            if (at) *at = va + static_cast<uint32_t>(i);
            return o;
        }
    return 0;
}

// Name of the broker record holding byte `at` (for log lines).
std::string HolderOf(uint32_t at) {
    const uint32_t n = H().ListHooks(nullptr, 0);
    std::vector<PSEHookInfo> list(n);
    const uint32_t got = H().ListHooks(list.data(), n);
    for (uint32_t i = 0; i < got && i < n; ++i)
        if (at >= list[i].va && at < list[i].va + list[i].len) {
            char b[160];
            if (list[i].kind == PSE_HOOK_BYTES)
                std::snprintf(b, sizeof b, "byte group '%s' of %s", list[i].name ? list[i].name : "?", list[i].ownerName);
            else
                std::snprintf(b, sizeof b, "hook at 0x%08X of %s", list[i].va, list[i].ownerName);
            return b;
        }
    return "?";
}

// Prologue length of a function hook another plugin (another copy of this file: dsfix.dll / a dsfix kit plugin) holds
// at `entry`, else 0 - a hook there is chained onto, not refused for its jump bytes.
uint32_t BrokerFunctionLen(uint32_t entry) {
    const uint32_t n = H().ListHooks(nullptr, 0);
    std::vector<PSEHookInfo> list(n);
    for (auto& i : list) i.size = sizeof i;
    const uint32_t got = H().ListHooks(list.data(), n);
    for (uint32_t i = 0; i < got && i < n; ++i)
        if (list[i].kind == PSE_HOOK_FUNCTION && list[i].va == entry) return list[i].len;
    return 0;
}

PSEPluginHandle GroupOwner(const char* group) {
    auto& groups = S().groups;
    auto it = groups.find(group);
    if (it != groups.end()) return it->second;
    const uint32_t n = H().ListHooks(nullptr, 0);
    std::vector<PSEHookInfo> list(n);
    const uint32_t got = H().ListHooks(list.data(), n);
    for (uint32_t i = 0; i < got && i < n; ++i)
        if (list[i].kind == PSE_HOOK_BYTES && list[i].name && std::strcmp(list[i].name, group) == 0) return list[i].owner;
    return 0;
}

std::vector<PSEBytePatch> ToBroker(const BytePatch* patches, size_t count) {
    std::vector<PSEBytePatch> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const BytePatch& p = patches[i];
        out.push_back({p.va, p.original, p.patched, p.len, p.anyOriginal ? 1u : 0u, p.name});
    }
    return out;
}

// Registers fn on a hook site through the broker and records it. `len` = bytes the site owns (function hooks: the
// prologue length, also passed to the broker).
bool Subscribe(PSEHookKind kind, uint32_t site, uint32_t original, const void* fn, int32_t priority, void** next,
               uint32_t len = 0, PSEResult* result = nullptr) {
    State& s = S();
    const PSEPluginHandle owner = OwnerForSite(site);
    const bool ownNext = next == nullptr;
    if (ownNext) next = NewSlot();
    PSEHookId id = 0;
    void* f = const_cast<void*>(fn);
    PSEResult r = PSE_E_INVALID_ARG;
    switch (kind) {
        case PSE_HOOK_CALL_SITE: r = H().HookCallSite(owner, site, original, f, priority, next, &id); len = 5; break;
        case PSE_HOOK_INDIRECT_CALL: r = H().HookIndirectCall(owner, site, original, f, priority, next, &id); len = 6; break;
        case PSE_HOOK_POINTER: r = H().HookPointer(owner, site, f, priority, next, &id); len = 4; break;
        case PSE_HOOK_FUNCTION: r = H().HookFunction(owner, site, len, nullptr, f, priority, next, &id); break;
        default: break;
    }
    if (result) *result = r;
    if (r != PSE_OK) {
        if (ownNext) FreeSlot(next);
        return false;
    }
    HookSite& h = s.hooks[site];
    h.kind = kind;
    h.len = len;
    h.original = original;
    h.subs.push_back({id, owner, reinterpret_cast<uint32_t>(fn), priority, next, ownNext});
    s.siteOwners[site] = owner;
    return true;
}
}  // namespace

// ---- owner scope -------------------------------------------------------------------------------------------------

patch::OwnerScope::OwnerScope(PSEPluginHandle plugin) : prev_(t_owner) { t_owner = plugin; }
patch::OwnerScope::~OwnerScope() { t_owner = prev_; }
PSEPluginHandle patch::CurrentOwner() { return t_owner ? t_owner : pse_host::Core(); }

// ---- bytes -------------------------------------------------------------------------------------------------------

bool patch::Matches(uint32_t va, const void* bytes, size_t len) {
    return Readable(va, len) && std::memcmp(reinterpret_cast<void*>(va), bytes, len) == 0;
}

bool patch::Write(uint32_t va, const void* bytes, size_t len) {
    State& s = S();
    std::lock_guard lock(s.m);
    const uint32_t end = va + static_cast<uint32_t>(len);
    // Our hook sites under the written bytes go back to the broker first.
    for (auto it = s.hooks.begin(); it != s.hooks.end();) {
        const uint32_t site = it->first;
        HookSite& h = it->second;
        if (site >= end || site + h.len <= va) {
            ++it;
            continue;
        }
        ++it;  // UnhookAll may erase the record
        // `call X` written exactly over a call site whose chain contains X: only the hooks in front of X go
        // (a feature restoring "its" call over a hand-chained hook keeps the hooks behind it).
        uint32_t keepFrom = 0;
        if (h.kind == PSE_HOOK_CALL_SITE && va == site && len == 5 && static_cast<const uint8_t*>(bytes)[0] == 0xE8) {
            int32_t rel;
            std::memcpy(&rel, static_cast<const uint8_t*>(bytes) + 1, 4);
            keepFrom = site + 5 + static_cast<uint32_t>(rel);
        }
        // Same for a pointer slot: writing a subscriber's address keeps it and the hooks behind it.
        if (h.kind == PSE_HOOK_POINTER && va == site && len == 4) std::memcpy(&keepFrom, bytes, 4);
        bool partial = false;
        if (keepFrom && keepFrom != h.original) {
            auto order = Ordered(site, h);
            size_t k = 0;
            while (k < order.size() && order[k]->fn != keepFrom) ++k;
            if (k < order.size()) {
                partial = true;
                std::vector<PSEHookId> ids;
                for (size_t i = 0; i < k; ++i) ids.push_back(order[i]->id);
                for (PSEHookId id : ids) UnhookSub(site, h, id);
            }
        }
        if (!partial) UnhookAll(site);
    }
    for (auto it = s.rawSlots.lower_bound(va >= 3 ? va - 3 : 0); it != s.rawSlots.end() && it->first < end;)
        it = it->first + 4 > va ? s.rawSlots.erase(it) : std::next(it);
    for (auto it = s.jumps.lower_bound(va >= 63 ? va - 63 : 0); it != s.jumps.end() && it->first < end;) {
        const uint32_t site = it->first, n = it->second.len;
        ++it;
        if (site + n > va) RevertJump(site);
    }
#ifndef DS_DIST
    // Anything else the broker owns there (a byte group, someone else's hook) is written over as before; noted once.
    uint32_t at = 0;
    if (FirstOwned(va, len, &at)) {
        static std::map<uint32_t, bool> noted;
        if (!noted[va]) {
            noted[va] = true;
            dslog::Write("[note] patch::Write at 0x%08X (%u bytes) over bytes of %s", va, static_cast<unsigned>(len),
                         HolderOf(at).c_str());
        }
    }
#endif
    return RawWrite(va, bytes, len);
}

// ---- byte groups -------------------------------------------------------------------------------------------------

bool patch::ApplyGroup(const char* group, const BytePatch* patches, size_t count) {
    State& s = S();
    std::lock_guard lock(s.m);
    unsigned already = 0;
    for (size_t i = 0; i < count; ++i)
        if (Matches(patches[i].va, patches[i].patched, patches[i].len)) ++already;
    PSEPluginHandle owner = GroupOwner(group);   // re-applied by its owner, whatever scope is active
    if (!owner) owner = CurrentOwner();
    auto list = ToBroker(patches, count);
    const PSEResult r = H().PatchGroup(owner, group, list.data(), static_cast<uint32_t>(list.size()));
    if (r != PSE_OK) {
        dslog::Write("[fail] %s: not applied (%s) - different exe version, or another patch owns these bytes",
                     group, r == PSE_E_MISMATCH ? "unexpected bytes" : r == PSE_E_CONFLICT ? "conflict" : "error");
        return false;
    }
    s.groups[group] = owner;
    dslog::Write("[ok]   %s: %u patches (%u already in the exe file)", group, static_cast<unsigned>(count), already);
    return true;
}

bool patch::RevertGroup(const char* group, const BytePatch*, size_t) {
    State& s = S();
    std::lock_guard lock(s.m);
    const PSEPluginHandle owner = GroupOwner(group);
    if (!owner) return true;   // never applied
    const PSEResult r = H().RevertGroup(owner, group);
    if (r == PSE_E_NOT_FOUND) return true;   // not applied now (the owner is remembered for the next apply)
    if (r != PSE_OK) {
        dslog::Write("[fail] %s: unexpected bytes - not reverted.", group);
        return false;
    }
    dslog::Write("[off]  %s: reverted", group);
    return true;
}

// ---- call hooks --------------------------------------------------------------------------------------------------

bool patch::HookCall(uint32_t site, const void* fn, uint32_t expectedTarget) {
    State& s = S();
    std::lock_guard lock(s.m);
    const uint32_t f = reinterpret_cast<uint32_t>(fn);
    const uint32_t head = CallTarget(site);
    auto it = s.hooks.find(site);
    if (it != s.hooks.end() && it->second.kind == PSE_HOOK_CALL_SITE && !it->second.subs.empty()) {
        HookSite& h = it->second;
        if (f == head) return true;  // already runs first there
        if (head != expectedTarget) {
            if (expectedTarget == h.original)
                dslog::Write("[fail] call hook at 0x%08X: already hooked (calls 0x%08X, %s) - a second hook on the "
                             "original 0x%08X would never run",
                             site, head, Name(Ordered(site, h).front()->owner), expectedTarget);
            else
                dslog::Write("[fail] call hook at 0x%08X: does not call 0x%08X", site, expectedTarget);
            return false;
        }
        if (f == h.original) return UnhookAll(site);  // a feature puts the original call back
        for (const Sub& sub : h.subs)
            if (sub.fn == f) {
                dslog::Write("[fail] call hook at 0x%08X: 0x%08X is already in its chain", site, f);
                return false;
            }
        // Hand-chaining: fn calls the current first hook itself - it joins in front of it.
        const Sub* first = Ordered(site, h).front();
        if (!Subscribe(PSE_HOOK_CALL_SITE, site, h.original, fn, first->priority + 1, nullptr)) {
            dslog::Write("[fail] call hook at 0x%08X: the broker refused the chained hook", site);
            return false;
        }
        return true;
    }
    if (!head || head != expectedTarget) {
        dslog::Write("[fail] call hook at 0x%08X: does not call 0x%08X", site, expectedTarget);
        return false;
    }
    uint32_t at = 0;
    if (FirstOwned(site, 5, &at)) {
        // Bytes the broker gives to someone else (a byte group, a jump, another plugin's hook): written outside the
        // broker as before this file used it, and logged.
        dslog::Write("[warn] call hook at 0x%08X: byte 0x%08X belongs to %s - hooked outside the broker", site, at,
                     HolderOf(at).c_str());
        uint8_t code[5];
        EncodeRel(code, 0xE8, site, f);
        return RawWrite(site, code, 5);
    }
    if (!Subscribe(PSE_HOOK_CALL_SITE, site, expectedTarget, fn, 0, nullptr)) {
        dslog::Write("[fail] call hook at 0x%08X: the broker refused it", site);
        return false;
    }
    return true;
}

bool patch::HookIndirectCall(uint32_t site, const void* fn, uint32_t iatSlot) {
    State& s = S();
    std::lock_guard lock(s.m);
    const uint32_t f = reinterpret_cast<uint32_t>(fn);
    auto it = s.hooks.find(site);
    if (it != s.hooks.end() && it->second.kind == PSE_HOOK_INDIRECT_CALL && !it->second.subs.empty()) {
        if (CallTarget(site) == f) return true;
        dslog::Write("[fail] call hook at 0x%08X: does not call [0x%08X] (already hooked by %s)", site, iatSlot,
                     Name(Ordered(site, it->second).front()->owner));
        return false;
    }
    uint8_t code[6] = {0xFF, 0x15};
    std::memcpy(code + 2, &iatSlot, 4);
    if (!Matches(site, code, 6)) {
        dslog::Write("[fail] call hook at 0x%08X: does not call [0x%08X]", site, iatSlot);
        return false;
    }
    uint32_t at = 0;
    if (FirstOwned(site, 6, &at)) {
        dslog::Write("[warn] call hook at 0x%08X: byte 0x%08X belongs to %s - hooked outside the broker", site, at,
                     HolderOf(at).c_str());
        uint8_t rel[5];
        EncodeRel(rel, 0xE8, site, f);
        std::memcpy(code, rel, 5);
        code[5] = 0x90;
        return RawWrite(site, code, 6);
    }
    if (!Subscribe(PSE_HOOK_INDIRECT_CALL, site, iatSlot, fn, 0, nullptr)) {
        dslog::Write("[fail] call hook at 0x%08X: the broker refused it", site);
        return false;
    }
    return true;
}

bool patch::SubscribeCall(uint32_t site, const void* fn, uint32_t originalTarget, int32_t priority, void** next) {
    State& s = S();
    std::lock_guard lock(s.m);
    auto it = s.hooks.find(site);
    if (it != s.hooks.end())
        for (const Sub& sub : it->second.subs)
            if (sub.fn == reinterpret_cast<uint32_t>(fn)) return true;
    if (!next) return false;
    if (!Subscribe(PSE_HOOK_CALL_SITE, site, originalTarget, fn, priority, next)) {
        dslog::Write("[fail] call hook at 0x%08X: subscriber refused (see the [pse] lines above)", site);
        return false;
    }
    return true;
}

// ---- jumps -------------------------------------------------------------------------------------------------------

namespace {
// `jmp` / `call rel32` to fn at site, the rest of len bytes nop: byte group "jmp@0x<site>" / "call@0x<site>".
bool WriteCode(uint8_t op, uint32_t site, const void* fn, size_t len, const void* original) {
    State& s = S();
    std::lock_guard lock(s.m);
    const char* what = op == 0xE9 ? "jump" : "call";
    if (len < 5 || len > 64) {
        dslog::Write("[fail] %s at 0x%08X: length %u (5..64)", what, site, static_cast<unsigned>(len));
        return false;
    }
    uint8_t code[64];
    std::memset(code, 0x90, sizeof code);
    EncodeRel(*reinterpret_cast<uint8_t(*)[5]>(code), op, site, reinterpret_cast<uint32_t>(fn));
    const uint32_t n = static_cast<uint32_t>(len);
    auto it = s.jumps.find(site);
    if (it != s.jumps.end()) {
        if (it->second.len == n && patch::Matches(site, code, n)) return true;
        RevertJump(site);  // re-jumped: the old one goes, the new one belongs to the active scope
    }
    if (original && !patch::Matches(site, original, n) && !patch::Matches(site, code, n)) {
        dslog::Write("[fail] %s at 0x%08X: unexpected bytes - different exe version? Not written.", what, site);
        return false;
    }
    uint32_t at = 0;
    if (FirstOwned(site, n, &at)) {
        dslog::Write("[warn] %s at 0x%08X: byte 0x%08X belongs to %s - written outside the broker", what, site, at,
                     HolderOf(at).c_str());
        return RawWrite(site, code, n);
    }
    char name[24];
    std::snprintf(name, sizeof name, op == 0xE9 ? "jmp@0x%08X" : "call@0x%08X", site);
    const PSEBytePatch p{site, static_cast<const uint8_t*>(original), code, n, original ? 0u : 1u, name};
    const PSEPluginHandle owner = OwnerForSite(site);
    const PSEResult r = H().PatchGroup(owner, name, &p, 1);
    if (r != PSE_OK) {
        dslog::Write("[warn] %s at 0x%08X: the broker refused it (%d) - written outside the broker", what, site,
                     static_cast<int>(r));
        return RawWrite(site, code, n);
    }
    s.jumps[site] = {owner, name, n};
    s.siteOwners[site] = owner;
    return true;
}
}  // namespace

bool patch::WriteJump(uint32_t site, const void* fn) { return WriteCode(0xE9, site, fn, 5, nullptr); }
bool patch::WriteJump(uint32_t site, const void* fn, size_t len, const void* original) {
    return WriteCode(0xE9, site, fn, len, original);
}
bool patch::WriteCall(uint32_t site, const void* fn, size_t len, const void* original) {
    return WriteCode(0xE8, site, fn, len, original);
}
bool patch::RevertCode(uint32_t site) {
    State& s = S();
    std::lock_guard lock(s.m);
    return RevertJump(site);
}

// ---- pointer slots -----------------------------------------------------------------------------------------------

namespace {
// The broker has a pointer hook site exactly at `slot` (e.g. an external plugin's), not ours.
bool BrokerPointerSite(uint32_t slot) {
    const uint32_t n = H().ListHooks(nullptr, 0);
    std::vector<PSEHookInfo> list(n);
    const uint32_t got = H().ListHooks(list.data(), n);
    for (uint32_t i = 0; i < got && i < n; ++i)
        if (list[i].kind == PSE_HOOK_POINTER && list[i].va == slot) return true;
    return false;
}

// Removes fn from our record at `site` (kind `kind`); true when it isn't there.
bool UnhookFn(uint32_t site, PSEHookKind kind, const void* fn) {
    State& s = S();
    auto it = s.hooks.find(site);
    if (it == s.hooks.end() || it->second.kind != kind) return true;
    PSEHookId id = 0;
    for (const Sub& sub : it->second.subs)
        if (sub.fn == reinterpret_cast<uint32_t>(fn)) id = sub.id;
    if (!id) return true;
    const bool ok = UnhookSub(site, it->second, id);
    if (it->second.subs.empty()) s.hooks.erase(it);
    return ok;
}
}  // namespace

bool patch::HookSlot(uint32_t slot, const void* fn, void** next, uint32_t expected, int32_t priority) {
    State& s = S();
    std::lock_guard lock(s.m);
    const uint32_t f = reinterpret_cast<uint32_t>(fn);
    if (!fn || !Readable(slot, 4)) {
        dslog::Write("[fail] pointer hook at 0x%08X: not mapped", slot);
        return false;
    }
    uint32_t cur;
    std::memcpy(&cur, reinterpret_cast<const void*>(slot), 4);
    auto it = s.hooks.find(slot);
    if (it != s.hooks.end() && it->second.kind == PSE_HOOK_POINTER && !it->second.subs.empty()) {
        HookSite& h = it->second;
        for (const Sub& sub : h.subs)
            if (sub.fn == f) return true;
        if (expected && expected != h.original) {
            dslog::Write("[fail] pointer hook at 0x%08X: originally holds 0x%08X, not 0x%08X", slot, h.original,
                         expected);
            return false;
        }
        if (!Subscribe(PSE_HOOK_POINTER, slot, h.original, fn, priority, next)) {
            dslog::Write("[fail] pointer hook at 0x%08X: the broker refused the chained hook", slot);
            return false;
        }
        return true;
    }
    auto raw = s.rawSlots.find(slot);
    if (raw != s.rawSlots.end() && raw->second.first == f && cur == f) return true;
    if (cur == f && raw == s.rawSlots.end()) return true;  // already there (the exe or a Write put it there)
    if (expected && cur != expected && !BrokerPointerSite(slot)) {
        dslog::Write("[fail] pointer hook at 0x%08X: holds 0x%08X, not 0x%08X", slot, cur, expected);
        return false;
    }
    PSEResult r = PSE_OK;
    if (Subscribe(PSE_HOOK_POINTER, slot, cur, fn, priority, next, 4, &r)) return true;
    if (r != PSE_E_CONFLICT) {
        dslog::Write("[fail] pointer hook at 0x%08X: the broker refused it (%d)", slot, static_cast<int>(r));
        return false;
    }
    // Bytes the broker gives to someone else (a byte group, a jump, another hook kind): written outside the broker.
    uint32_t at = 0;
    FirstOwned(slot, 4, &at);
    dslog::Write("[warn] pointer hook at 0x%08X: byte 0x%08X belongs to %s - hooked outside the broker", slot, at,
                 HolderOf(at).c_str());
    if (!RawWrite(slot, &f, 4)) return false;
    if (next) *next = reinterpret_cast<void*>(cur);
    s.rawSlots[slot] = {f, cur};
    return true;
}

bool patch::UnhookSlot(uint32_t slot, const void* fn) {
    State& s = S();
    std::lock_guard lock(s.m);
    auto raw = s.rawSlots.find(slot);
    if (raw != s.rawSlots.end() && raw->second.first == reinterpret_cast<uint32_t>(fn)) {
        const uint32_t old = raw->second.second;
        s.rawSlots.erase(raw);
        return RawWrite(slot, &old, 4);
    }
    return UnhookFn(slot, PSE_HOOK_POINTER, fn);
}

// ---- function entries --------------------------------------------------------------------------------------------

uint32_t patch::PrologueLength(uint32_t entry) {
    State& s = S();
    std::lock_guard lock(s.m);
    auto it = s.hooks.find(entry);
    if (it != s.hooks.end() && it->second.kind == PSE_HOOK_FUNCTION) return it->second.len;
    if (!Readable(entry, 5)) return 0;
    uint32_t len = 0;
    while (len < 5) {
        const uint32_t avail = Readable(entry + len, 15) ? 15 : (Readable(entry + len, 1) ? 1 : 0);
        if (!avail) return 0;
        const uint32_t n = pse::x86::Length(reinterpret_cast<const uint8_t*>(entry + len), avail);
        if (!n) return 0;
        len += n;
    }
    return len;
}

bool patch::HookFunction(uint32_t entry, const void* fn, void** next, const void* expected, size_t expectedLen,
                         uint32_t prologueLen, int32_t priority) {
    State& s = S();
    std::lock_guard lock(s.m);
    const uint32_t f = reinterpret_cast<uint32_t>(fn);
    if (!fn) return false;
    auto it = s.hooks.find(entry);
    if (it != s.hooks.end() && it->second.kind == PSE_HOOK_FUNCTION && !it->second.subs.empty()) {
        HookSite& h = it->second;
        for (const Sub& sub : h.subs)
            if (sub.fn == f) return true;
        const uint32_t len = prologueLen && prologueLen <= h.len ? prologueLen : h.len;
        if (!Subscribe(PSE_HOOK_FUNCTION, entry, 0, fn, priority, next, len)) {
            dslog::Write("[fail] function hook at 0x%08X: the broker refused the chained hook", entry);
            return false;
        }
        return true;
    }
    // Hooked by another plugin's copy of this file: chain onto it (its jump replaced the bytes we would check).
    if (const uint32_t held = BrokerFunctionLen(entry)) {
        const uint32_t len = prologueLen && prologueLen <= held ? prologueLen : held;
        if (!Subscribe(PSE_HOOK_FUNCTION, entry, 0, fn, priority, next, len)) {
            dslog::Write("[fail] function hook at 0x%08X: the broker refused the chained hook", entry);
            return false;
        }
        return true;
    }
    if (expected && expectedLen && !Matches(entry, expected, expectedLen)) {
        dslog::Write("[fail] function hook at 0x%08X: unexpected bytes - different exe version, or patched elsewhere",
                     entry);
        return false;
    }
    if (s.jumps.count(entry)) RevertJump(entry);  // our own old jump at that entry gives way
    const uint32_t len = prologueLen ? prologueLen : PrologueLength(entry);
    if (len < 5) {
        dslog::Write("[fail] function hook at 0x%08X: undecodable prologue", entry);
        return false;
    }
    PSEResult r = PSE_OK;
    if (!Subscribe(PSE_HOOK_FUNCTION, entry, 0, fn, priority, next, len, &r)) {
        uint32_t at = 0;
        if (r == PSE_E_CONFLICT && FirstOwned(entry, len, &at))
            dslog::Write("[fail] function hook at 0x%08X (%u bytes): byte 0x%08X belongs to %s", entry, len, at,
                         HolderOf(at).c_str());
        else
            dslog::Write("[fail] function hook at 0x%08X (%u bytes): the broker refused it (%d)", entry, len,
                         static_cast<int>(r));
        return false;
    }
    return true;
}

bool patch::UnhookFunction(uint32_t entry, const void* fn) {
    State& s = S();
    std::lock_guard lock(s.m);
    return UnhookFn(entry, PSE_HOOK_FUNCTION, fn);
}

void patch::ResetForTests() {
    State& s = S();
    std::lock_guard lock(s.m);
    s.rawSlots.clear();
    s.hooks.clear();
    s.jumps.clear();
    s.groups.clear();
    s.siteOwners.clear();
    s.freeSlots.clear();
    s.slots.clear();
}

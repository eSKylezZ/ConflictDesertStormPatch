// patch:: (shim/core/patch.h) for the ds2fix.widescreen plugin: every write / hook goes through the pse.hooks broker,
// owned by this plugin (dropped by the loader when it is unloaded or crashes).
#include "core/patch.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "core/log.h"
#include "pse/pse_api.h"
#include "x86len.h"

namespace {
const PSEHooksInterface* g_hooks = nullptr;
PSEPluginHandle g_plugin = 0;
std::deque<std::string> g_names;   // group names stay valid for the broker
std::deque<void*> g_next;          // `next` variables of hooks whose callers don't chain (the original is called)
std::vector<std::pair<uint32_t, uint32_t>> g_detours;   // [entry, entry + len) moved by WriteJump
std::vector<uint32_t> g_writes;    // addresses with a Write group
std::deque<std::vector<uint8_t>> g_bytes;   // the bytes handed to the broker stay valid

const char* Keep(const char* s) {
    g_names.emplace_back(s);
    return g_names.back().c_str();
}
}  // namespace

void patch::Attach(const PSEHooksInterface* hooks, PSEPluginHandle plugin) {
    g_hooks = hooks;
    g_plugin = plugin;
}

bool patch::Matches(uint32_t va, const void* bytes, size_t len) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(va), bytes, len) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool patch::ApplyGroup(const char* group, const BytePatch* patches, size_t count) {
    if (!g_hooks) return false;
    std::vector<PSEBytePatch> p(count);
    for (size_t i = 0; i < count; ++i)
        p[i] = {patches[i].va, patches[i].original, patches[i].patched, patches[i].len, patches[i].anyOriginal ? 1u : 0u,
                patches[i].name};
    const PSEResult r = g_hooks->PatchGroup(g_plugin, Keep(group), p.data(), static_cast<uint32_t>(count));
    if (r != PSE_OK) dslog::Write("[fail] %s: not patched (pse.hooks %d)", group, r);
    return r == PSE_OK;
}

// A run-time value (clamp jumps / immediates following the HUD scale ...): one byte group per address, "w@<va>",
// written again by revert + patch. Bytes inside an entry this plugin detoured are its own already (left alone).
bool patch::Write(uint32_t va, const void* bytes, size_t len) {
    if (!g_hooks) return false;
    for (const auto& d : g_detours)
        if (va >= d.first && va < d.second) return true;
    char name[24];
    std::snprintf(name, sizeof name, "w@%08X", va);
    bool known = false;
    for (uint32_t w : g_writes) known = known || w == va;
    if (known) g_hooks->RevertGroup(g_plugin, name);
    else g_writes.push_back(va);
    g_bytes.emplace_back(static_cast<const uint8_t*>(bytes), static_cast<const uint8_t*>(bytes) + len);
    const PSEBytePatch p = {va, nullptr, g_bytes.back().data(), static_cast<uint32_t>(len), 1, nullptr};
    const PSEResult r = g_hooks->PatchGroup(g_plugin, known ? name : Keep(name), &p, 1);
    if (r != PSE_OK) dslog::Write("[fail] write at 0x%08X refused (pse.hooks %d)", va, r);
    return r == PSE_OK;
}

bool patch::SubscribeCall(uint32_t site, const void* fn, uint32_t originalTarget, int32_t priority, void** next) {
    if (!g_hooks) return false;
    const PSEResult r = g_hooks->HookCallSite(g_plugin, site, originalTarget, const_cast<void*>(fn), priority, next, nullptr);
    if (r != PSE_OK) dslog::Write("[fail] call hook at 0x%08X (pse.hooks %d)", site, r);
    return r == PSE_OK;
}

bool patch::HookCall(uint32_t site, const void* fn, uint32_t expectedTarget) {
    g_next.push_back(nullptr);
    return SubscribeCall(site, fn, expectedTarget, 0, &g_next.back());
}

bool patch::HookIndirectCall(uint32_t site, const void* fn, uint32_t iatSlot) {
    if (!g_hooks) return false;
    g_next.push_back(nullptr);
    const PSEResult r = g_hooks->HookIndirectCall(g_plugin, site, iatSlot, const_cast<void*>(fn), 0, &g_next.back(), nullptr);
    if (r != PSE_OK) dslog::Write("[fail] indirect call hook at 0x%08X (pse.hooks %d)", site, r);
    return r == PSE_OK;
}

// The standalone build's `jmp fn` at a function entry = a broker entry detour whose trampoline nobody calls (the
// feature replaces the function; stubs that run the moved instructions themselves jump back past them).
bool patch::WriteJump(uint32_t site, const void* fn) {
    if (!g_hooks) return false;
    uint32_t len = 0;
    while (len < 5) {
        const uint32_t n = pse::x86::Length(reinterpret_cast<const uint8_t*>(site + len));
        if (!n) return false;
        len += n;
    }
    g_next.push_back(nullptr);
    const PSEResult r = g_hooks->HookFunction(g_plugin, site, len, nullptr, const_cast<void*>(fn), 0, &g_next.back(), nullptr);
    if (r != PSE_OK) {
        dslog::Write("[fail] entry hook at 0x%08X (pse.hooks %d)", site, r);
        return false;
    }
    g_detours.emplace_back(site, site + len);
    return true;
}

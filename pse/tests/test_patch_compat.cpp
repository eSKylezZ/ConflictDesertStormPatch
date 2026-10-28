// src/core/patch.cpp on top of the hooks broker (docs/sdk/reference/hooks.md "DS1 host"): owner scopes, byte groups
// found by name whatever the scope, HookCall's compatibility rules, Write handing hook bytes back, WriteJump records,
// shared call sites. patch.cpp is compiled into this file with stubs for the two things it needs from the game DLL
// (dslog::Write, pse_host::Core).
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "core/patch.cpp"  // NOLINT: the unit under test
#include "exe_image.h"
#include "core/comhooks.h"
#include "features/features.h"
#include "generated/patch_tables.h"
#include "loader_fixture.h"
#include "pse_test.h"

namespace {
std::vector<std::string> g_dslog;
PSEPluginHandle g_coreHandle = 0;
}  // namespace

void dslog::Write(const char* fmt, ...) {
    char b[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    g_dslog.emplace_back(b);
}
PSEPluginHandle pse_host::Core() { return g_coreHandle; }

namespace {
using Fn0 = int(__cdecl*)();

bool DsLogHas(const char* a, const char* b = nullptr) {
    for (auto& l : g_dslog)
        if (l.find(a) != std::string::npos && (!b || l.find(b) != std::string::npos)) return true;
    return false;
}

struct Env {
    PSEPluginHandle core, alpha, beta, gamma;
    Env() {
        pse_fixture::Fresh();
        patch::ResetForTests();
        g_dslog.clear();
        core = g_coreHandle = pse_fixture::AddPlugin("dsfix");
        alpha = pse_fixture::AddPlugin("dsfix.alpha");
        beta = pse_fixture::AddPlugin("dsfix.beta");
        gamma = pse_fixture::AddPlugin("dsfix.gamma");
    }
    ~Env() {
        patch::ResetForTests();
        pse::hooks::ResetForTests();
    }
};

// Code buffer: a "caller" at kSite (call kOrig; ret), the original function at kOrig (mov eax, 7; ret).
constexpr uint32_t kSite = 0x10, kOrig = 0x100, kIndirect = 0x40, kSlot = 0x200, kJump = 0x300, kData = 0x400;
struct Code {
    uint8_t* p;
    Code() {
        p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        std::memset(p, 0xCC, 0x1000);
        rel(kSite, 0xE8, va(kOrig));
        p[kSite + 5] = 0xC3;
        const uint8_t orig[] = {0xB8, 7, 0, 0, 0, 0xC3};
        std::memcpy(p + kOrig, orig, sizeof orig);
        // indirect: call [slot]; ret - slot holds kOrig
        p[kIndirect] = 0xFF;
        p[kIndirect + 1] = 0x15;
        const uint32_t slot = va(kSlot), target = va(kOrig);
        std::memcpy(p + kIndirect + 2, &slot, 4);
        p[kIndirect + 6] = 0xC3;
        std::memcpy(p + kSlot, &target, 4);
        // a function entry for WriteJump: mov eax, 3; ret
        const uint8_t entry[] = {0xB8, 3, 0, 0, 0, 0xC3};
        std::memcpy(p + kJump, entry, sizeof entry);
    }
    ~Code() { VirtualFree(p, 0, MEM_RELEASE); }
    uint32_t va(uint32_t off) const { return reinterpret_cast<uint32_t>(p) + off; }
    void rel(uint32_t off, uint8_t op, uint32_t target) {
        p[off] = op;
        int32_t r = static_cast<int32_t>(target - (va(off) + 5));
        std::memcpy(p + off + 1, &r, 4);
    }
    uint32_t callTarget(uint32_t off) const {
        if (p[off] != 0xE8) return 0;
        int32_t r;
        std::memcpy(&r, p + off + 1, 4);
        return va(off) + 5 + static_cast<uint32_t>(r);
    }
    int run(uint32_t off) const { return reinterpret_cast<Fn0>(va(off))(); }
};

std::vector<PSEHookInfo> Hooks() {
    const uint32_t n = pse::HooksInterface()->ListHooks(nullptr, 0);
    std::vector<PSEHookInfo> v(n);
    pse::HooksInterface()->ListHooks(v.data(), n);
    return v;
}
std::vector<PSEHookInfo> HooksAt(uint32_t va) {
    std::vector<PSEHookInfo> out;
    for (auto& h : Hooks())
        if (h.va == va) out.push_back(h);
    return out;
}

// Hooks for the chain: each adds its weight and calls the previous (hand-chained) hook or its broker `next`.
Fn0 g_n1, g_n2, g_n3;
std::string g_order;
int __cdecl H1() { g_order += '1'; return g_n1() + 100; }
int __cdecl H2() { g_order += '2'; return g_n2() + 1000; }
int __cdecl H3() { g_order += '3'; return g_n3() + 10000; }
int __cdecl Plain() { g_order += 'P'; return 42; }
// Hand-chained hook: calls H1 itself (like a feature hooking over another feature's hook).
int __cdecl OverH1() { g_order += 'O'; return H1() + 5; }
int __cdecl OverH1b() { g_order += 'Q'; return H1() + 6; }
}  // namespace

PSE_TEST("patch compat: owner scope nests and defaults to dsfix") {
    Env e;
    PSE_CHECK_EQ(patch::CurrentOwner(), e.core);
    {
        patch::OwnerScope a(e.alpha);
        PSE_CHECK_EQ(patch::CurrentOwner(), e.alpha);
        {
            patch::OwnerScope b(e.beta);
            PSE_CHECK_EQ(patch::CurrentOwner(), e.beta);
        }
        PSE_CHECK_EQ(patch::CurrentOwner(), e.alpha);
    }
    PSE_CHECK_EQ(patch::CurrentOwner(), e.core);
}

PSE_TEST("patch compat (a): HookCall needs the expected target and records the scope's owner") {
    Env e;
    Code c;
    g_n1 = reinterpret_cast<Fn0>(c.va(kOrig));
    PSE_CHECK(!patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), c.va(kOrig) + 1));
    PSE_CHECK(DsLogHas("[fail] call hook", "does not call"));
    PSE_CHECK_EQ(c.callTarget(kSite), c.va(kOrig));
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), c.va(kOrig)));
    }
    PSE_CHECK_EQ(c.callTarget(kSite), reinterpret_cast<uint32_t>(&Plain));
    auto h = HooksAt(c.va(kSite));
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].kind, PSE_HOOK_CALL_SITE);
    PSE_CHECK_EQ(h[0].owner, e.alpha);
    PSE_CHECK_STR(h[0].ownerName, "dsfix.alpha");
    PSE_CHECK_EQ(c.run(kSite), 42);
}

PSE_TEST("patch compat (d): the same fn again is true and adds nothing") {
    Env e;
    Code c;
    PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), c.va(kOrig)));
    PSE_CHECK(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), c.va(kOrig)));
    PSE_CHECK(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), reinterpret_cast<uint32_t>(&Plain)));
    PSE_CHECK_EQ(HooksAt(c.va(kSite)).size(), size_t{1});
}

PSE_TEST("patch compat (b): hand-chaining onto the current head joins in front of it") {
    Env e;
    Code c;
    {
        patch::OwnerScope s(e.alpha);
        // H1 is a "broker-style" hook here only for its call; it calls the original through g_n1.
        g_n1 = reinterpret_cast<Fn0>(c.va(kOrig));
        PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&H1), c.va(kOrig)));
    }
    {
        patch::OwnerScope s(e.beta);
        PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&OverH1), reinterpret_cast<uint32_t>(&H1)));
    }
    PSE_CHECK_EQ(c.callTarget(kSite), reinterpret_cast<uint32_t>(&OverH1));
    g_order.clear();
    PSE_CHECK_EQ(c.run(kSite), 7 + 100 + 5);
    PSE_CHECK_STR(g_order.c_str(), "O1");
    auto h = HooksAt(c.va(kSite));
    PSE_REQUIRE(h.size() == 2);
    for (auto& i : h) {
        if (i.owner == e.beta) {
            PSE_CHECK_EQ(i.chainPosition, 0u);
            PSE_CHECK_EQ(i.priority, 1);
        } else {
            PSE_CHECK_EQ(i.owner, e.alpha);
            PSE_CHECK_EQ(i.chainPosition, 1u);
        }
    }
    // A third one on the new head goes in front again.
    PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&OverH1b), reinterpret_cast<uint32_t>(&OverH1)));
    PSE_CHECK_EQ(c.callTarget(kSite), reinterpret_cast<uint32_t>(&OverH1b));
    // Expecting the old (no longer first) hook fails like a plain mismatch.
    PSE_CHECK(!patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), reinterpret_cast<uint32_t>(&H1)));
}

PSE_TEST("patch compat (c): a second hook on the original target fails instead of never running") {
    Env e;
    Code c;
    g_n1 = reinterpret_cast<Fn0>(c.va(kOrig));
    PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&H1), c.va(kOrig)));
    g_dslog.clear();
    PSE_CHECK(!patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), c.va(kOrig)));
    PSE_CHECK(DsLogHas("[fail] call hook", "already hooked"));
    PSE_CHECK_EQ(c.callTarget(kSite), reinterpret_cast<uint32_t>(&H1));
    PSE_CHECK_EQ(HooksAt(c.va(kSite)).size(), size_t{1});
}

PSE_TEST("patch compat: HookCall back to the original target removes the hooks (hud.cpp toggles)") {
    Env e;
    Code c;
    g_n1 = reinterpret_cast<Fn0>(c.va(kOrig));
    PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&H1), c.va(kOrig)));
    PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(c.va(kOrig)), reinterpret_cast<uint32_t>(&H1)));
    PSE_CHECK_EQ(c.callTarget(kSite), c.va(kOrig));
    PSE_CHECK(HooksAt(c.va(kSite)).empty());
    // and on again
    PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&H1), c.va(kOrig)));
    PSE_CHECK_EQ(c.run(kSite), 107);
}

PSE_TEST("patch compat: Write over a hook site hands the bytes back to the broker first") {
    Env e;
    Code c;
    g_n1 = reinterpret_cast<Fn0>(c.va(kOrig));
    PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&H1), c.va(kOrig)));
    uint8_t orig[5] = {0xE8};  // the original call, as a feature restoring it writes it
    const int32_t r = static_cast<int32_t>(c.va(kOrig) - (c.va(kSite) + 5));
    std::memcpy(orig + 1, &r, 4);
    PSE_REQUIRE(patch::Write(c.va(kSite), orig, 5));
    PSE_CHECK(HooksAt(c.va(kSite)).empty());
    PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(c.va(kSite)), PSEPluginHandle{0});
    PSE_CHECK_EQ(c.run(kSite), 7);
    // The site can be hooked again (no stale record, no conflict).
    PSE_CHECK(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), c.va(kOrig)));
    // A data write into the rel32 (retarget) also removes the hook.
    const uint32_t junk = 0x12345678;
    PSE_REQUIRE(patch::WriteValue(c.va(kSite + 1), junk));
    PSE_CHECK(HooksAt(c.va(kSite)).empty());
}

PSE_TEST("patch compat: Write of `call X` over a hand-chain keeps the hooks behind X") {
    Env e;
    Code c;
    g_n1 = reinterpret_cast<Fn0>(c.va(kOrig));
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&H1), c.va(kOrig)));
    }
    {
        patch::OwnerScope s(e.beta);
        PSE_REQUIRE(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&OverH1), reinterpret_cast<uint32_t>(&H1)));
    }
    uint8_t callH1[5] = {0xE8};
    int32_t r = static_cast<int32_t>(reinterpret_cast<uint32_t>(&H1) - (c.va(kSite) + 5));
    std::memcpy(callH1 + 1, &r, 4);
    PSE_REQUIRE(patch::Write(c.va(kSite), callH1, 5));
    auto h = HooksAt(c.va(kSite));
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].owner, e.alpha);
    PSE_CHECK_EQ(c.run(kSite), 107);
}

PSE_TEST("patch compat: byte groups are found by name - revert outside the scope, owner kept for re-apply") {
    Env e;
    Code c;
    const uint8_t on[] = {0xB8, 9, 0, 0, 0}, off[] = {0xB8, 3, 0, 0, 0};
    const BytePatch g[] = {{"test", c.va(kJump), off, on, 5, false}};
    {
        patch::OwnerScope s(e.gamma);
        PSE_REQUIRE(patch::ApplyGroup("Test group", g));
    }
    PSE_CHECK_EQ(c.run(kJump), 9);
    PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(c.va(kJump)), e.gamma);
    // A live toggle from the menu: no scope (or another scope) active.
    {
        patch::OwnerScope s(e.alpha);
        PSE_CHECK(patch::ApplyGroup("Test group", g));   // same group again: still gamma's, no conflict
        PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(c.va(kJump)), e.gamma);
    }
    PSE_REQUIRE(patch::RevertGroup("Test group", g));
    PSE_CHECK_EQ(c.run(kJump), 3);
    PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(c.va(kJump)), PSEPluginHandle{0});
    PSE_CHECK(patch::RevertGroup("Test group", g));  // not applied: fine
    PSE_REQUIRE(patch::ApplyGroup("Test group", g));
    PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(c.va(kJump)), e.gamma);
    PSE_CHECK(patch::RevertGroup("Never applied", g));
}

PSE_TEST("patch compat: WriteJump is a jmp@ byte group of the scope; re-jump replaces; Write reverts it") {
    Env e;
    Code c;
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(patch::WriteJump(c.va(kJump), reinterpret_cast<const void*>(&Plain)));
    }
    PSE_CHECK_EQ(c.run(kJump), 42);
    char name[24];
    std::snprintf(name, sizeof name, "jmp@0x%08X", c.va(kJump));
    auto h = HooksAt(c.va(kJump));
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].kind, PSE_HOOK_BYTES);
    PSE_CHECK_STR(h[0].name, name);
    PSE_CHECK_EQ(h[0].owner, e.alpha);
    PSE_CHECK(patch::WriteJump(c.va(kJump), reinterpret_cast<const void*>(&Plain)));  // same: nothing changes
    {
        patch::OwnerScope s(e.beta);
        PSE_REQUIRE(patch::WriteJump(c.va(kJump), reinterpret_cast<const void*>(&H2)));
    }
    g_n2 = reinterpret_cast<Fn0>(c.va(kOrig));
    PSE_CHECK_EQ(c.run(kJump), 1007);
    h = HooksAt(c.va(kJump));
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].owner, e.beta);
    // A feature writes the original entry back: the group goes first, then the bytes are written.
    const uint8_t entry[] = {0xB8, 3, 0, 0, 0};
    PSE_REQUIRE(patch::Write(c.va(kJump), entry, sizeof entry));
    PSE_CHECK(HooksAt(c.va(kJump)).empty());
    PSE_CHECK_EQ(c.run(kJump), 3);
    // Outside any scope, a re-jump keeps the site's last owner.
    PSE_REQUIRE(patch::WriteJump(c.va(kJump), reinterpret_cast<const void*>(&Plain)));
    h = HooksAt(c.va(kJump));
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].owner, e.beta);
}

PSE_TEST("patch compat: hooks over bytes a byte group owns stay outside the broker (logged)") {
    Env e;
    Code c;
    uint8_t same[5];
    std::memcpy(same, c.p + kSite, 5);
    const BytePatch g[] = {{"site", c.va(kSite), same, same, 5, true}};
    PSE_REQUIRE(patch::ApplyGroup("Owns the site", g));
    PSE_CHECK(patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), c.va(kOrig)));
    PSE_CHECK(DsLogHas("[warn] call hook", "Owns the site"));
    PSE_CHECK_EQ(c.callTarget(kSite), reinterpret_cast<uint32_t>(&Plain));
    for (auto& h : HooksAt(c.va(kSite))) PSE_CHECK_EQ(h.kind, PSE_HOOK_BYTES);
}

PSE_TEST("patch compat: HookIndirectCall - recorded, idempotent, a different fn fails") {
    Env e;
    Code c;
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(patch::HookIndirectCall(c.va(kIndirect), reinterpret_cast<const void*>(&Plain), c.va(kSlot)));
    }
    PSE_CHECK_EQ(c.p[kIndirect + 5], 0x90);
    PSE_CHECK_EQ(c.callTarget(kIndirect), reinterpret_cast<uint32_t>(&Plain));
    PSE_CHECK_EQ(c.run(kIndirect), 42);
    PSE_CHECK(patch::HookIndirectCall(c.va(kIndirect), reinterpret_cast<const void*>(&Plain), c.va(kSlot)));
    PSE_CHECK(!patch::HookIndirectCall(c.va(kIndirect), reinterpret_cast<const void*>(&H1), c.va(kSlot)));
    auto h = HooksAt(c.va(kIndirect));
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].kind, PSE_HOOK_INDIRECT_CALL);
    PSE_CHECK_EQ(h[0].owner, e.alpha);
    // Writing the original `call [slot]` back removes it.
    const uint32_t slot = c.va(kSlot);
    uint8_t orig[6] = {0xFF, 0x15};
    std::memcpy(orig + 2, &slot, 4);
    PSE_REQUIRE(patch::Write(c.va(kIndirect), orig, 6));
    PSE_CHECK(HooksAt(c.va(kIndirect)).empty());
    PSE_CHECK_EQ(c.run(kIndirect), 7);
}

PSE_TEST("patch compat: SubscribeCall - three subscribers in priority order, each with its own next") {
    Env e;
    Code c;
    // Registered in the "wrong" order on purpose: priority decides.
    {
        patch::OwnerScope s(e.gamma);
        PSE_REQUIRE(patch::SubscribeCall(c.va(kSite), reinterpret_cast<const void*>(&H3), c.va(kOrig), 0,
                                         reinterpret_cast<void**>(&g_n3)));
    }
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(patch::SubscribeCall(c.va(kSite), reinterpret_cast<const void*>(&H1), c.va(kOrig), 200,
                                         reinterpret_cast<void**>(&g_n1)));
    }
    {
        patch::OwnerScope s(e.beta);
        PSE_REQUIRE(patch::SubscribeCall(c.va(kSite), reinterpret_cast<const void*>(&H2), c.va(kOrig), 100,
                                         reinterpret_cast<void**>(&g_n2)));
    }
    g_order.clear();
    PSE_CHECK_EQ(c.run(kSite), 7 + 100 + 1000 + 10000);
    PSE_CHECK_STR(g_order.c_str(), "123");
    PSE_CHECK_EQ(c.callTarget(kSite), reinterpret_cast<uint32_t>(&H1));
    // Idempotent per fn.
    PSE_CHECK(patch::SubscribeCall(c.va(kSite), reinterpret_cast<const void*>(&H2), c.va(kOrig), 100,
                                   reinterpret_cast<void**>(&g_n2)));
    PSE_CHECK_EQ(HooksAt(c.va(kSite)).size(), size_t{3});
    // A plain HookCall on the original target can't join silently.
    PSE_CHECK(!patch::HookCall(c.va(kSite), reinterpret_cast<const void*>(&Plain), c.va(kOrig)));
}

// Tier 2: the real shared site on the mapped original exe - the texture bind 0x53CC9B (call 0x544220).
PSE_TEST("patch image: glyph bind 0x53CC9B takes three subscribers; Write of the original call cleans up") {
    PSE_REQUIRE_EXE();
    exe::Restore();
    Env e;
    constexpr uint32_t kBindSite = 0x53CC9B, kBind = 0x544220;
    PSE_REQUIRE(patch::Matches(kBindSite, "\xE8", 1));
    uint8_t orig[5];
    std::memcpy(orig, exe::Ptr(kBindSite), 5);
    void* n1 = nullptr;
    void* n2 = nullptr;
    void* n3 = nullptr;
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(patch::SubscribeCall(kBindSite, reinterpret_cast<const void*>(&H1), kBind, 200, &n1));
    }
    {
        patch::OwnerScope s(e.beta);
        PSE_REQUIRE(patch::SubscribeCall(kBindSite, reinterpret_cast<const void*>(&H2), kBind, 100, &n2));
    }
    {
        patch::OwnerScope s(e.gamma);
        PSE_REQUIRE(patch::SubscribeCall(kBindSite, reinterpret_cast<const void*>(&H3), kBind, 0, &n3));
    }
    PSE_CHECK_EQ(n1, reinterpret_cast<void*>(&H2));
    PSE_CHECK_EQ(n2, reinterpret_cast<void*>(&H3));
    PSE_CHECK_EQ(n3, reinterpret_cast<void*>(kBind));
    auto h = HooksAt(kBindSite);
    PSE_REQUIRE(h.size() == 3);
    for (auto& i : h) {
        if (i.owner == e.alpha) PSE_CHECK_EQ(i.chainPosition, 0u);
        if (i.owner == e.beta) PSE_CHECK_EQ(i.chainPosition, 1u);
        if (i.owner == e.gamma) PSE_CHECK_EQ(i.chainPosition, 2u);
    }
    PSE_CHECK(!patch::HookCall(kBindSite, reinterpret_cast<const void*>(&Plain), kBind));
    PSE_REQUIRE(patch::Write(kBindSite, orig, 5));
    PSE_CHECK(HooksAt(kBindSite).empty());
    PSE_CHECK(std::memcmp(exe::Ptr(kBindSite), orig, 5) == 0);
    exe::Restore();
}

PSE_TEST("patch image: every generated group through patch::ApplyGroup with an owner, reverted outside the scope") {
    PSE_REQUIRE_EXE();
    exe::Restore();
    Env e;
    for (const auto& g : gen::kAllGroups) {
        patch::OwnerScope s(e.alpha);
        PSE_CHECK(patch::ApplyGroup(g.name, g.patches, g.count));
        PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(g.patches[0].va), e.alpha);
    }
    for (const auto& g : gen::kAllGroups) PSE_CHECK(patch::RevertGroup(g.name, g.patches, g.count));
    for (const auto& g : gen::kAllGroups) PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(g.patches[0].va), PSEPluginHandle{0});
    exe::Restore();
}

// ---- pointer slots (patch::HookSlot / UnhookSlot over broker HookPointer) --------------------------------------------

PSE_TEST("patch slots: HookSlot - expected value, next = the old value, scope owner, chain, unhook restores") {
    Env e;
    Code c;
    const uint32_t slot = c.va(kSlot);
    void* unused = nullptr;
    PSE_CHECK(!patch::HookSlot(slot, reinterpret_cast<const void*>(&H1), &unused, c.va(kOrig) + 1));
    PSE_CHECK(DsLogHas("[fail] pointer hook", "not"));
    PSE_CHECK_EQ(exe::Read<uint32_t>(slot), c.va(kOrig));
    g_n1 = nullptr;
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(patch::HookSlot(slot, reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1), c.va(kOrig)));
    }
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(g_n1), c.va(kOrig));   // the feature's "old value" variable
    PSE_CHECK_EQ(exe::Read<uint32_t>(slot), reinterpret_cast<uint32_t>(&H1));
    PSE_CHECK_EQ(c.run(kIndirect), 107);
    auto h = HooksAt(slot);
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].kind, PSE_HOOK_POINTER);
    PSE_CHECK_EQ(h[0].owner, e.alpha);
    PSE_CHECK_EQ(h[0].len, 4u);
    // Same fn again: true, nothing added (Apply* re-runs).
    PSE_CHECK(patch::HookSlot(slot, reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1), c.va(kOrig)));
    PSE_CHECK_EQ(HooksAt(slot).size(), size_t{1});
    // A second feature joins the chain (higher priority runs first) instead of replacing the first.
    {
        patch::OwnerScope s(e.beta);
        PSE_REQUIRE(patch::HookSlot(slot, reinterpret_cast<const void*>(&H2), reinterpret_cast<void**>(&g_n2), c.va(kOrig), 10));
    }
    g_order.clear();
    PSE_CHECK_EQ(c.run(kIndirect), 7 + 100 + 1000);
    PSE_CHECK_STR(g_order.c_str(), "21");
    // Its expected value is the slot's ORIGINAL value, not the current head.
    PSE_CHECK(!patch::HookSlot(slot, reinterpret_cast<const void*>(&H3), reinterpret_cast<void**>(&g_n3),
                               reinterpret_cast<uint32_t>(&H2)));
    PSE_REQUIRE(patch::UnhookSlot(slot, reinterpret_cast<const void*>(&H2)));
    PSE_CHECK_EQ(c.run(kIndirect), 107);
    PSE_REQUIRE(patch::UnhookSlot(slot, reinterpret_cast<const void*>(&H1)));
    PSE_CHECK_EQ(exe::Read<uint32_t>(slot), c.va(kOrig));
    PSE_CHECK(HooksAt(slot).empty());
    PSE_CHECK(patch::UnhookSlot(slot, reinterpret_cast<const void*>(&H1)));   // not hooked: fine
    PSE_CHECK_EQ(c.run(kIndirect), 7);
}

PSE_TEST("patch slots: a Write over a pointer hook hands it back; writing a subscriber keeps the chain behind it") {
    Env e;
    Code c;
    const uint32_t slot = c.va(kSlot);
    PSE_REQUIRE(patch::HookSlot(slot, reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1)));
    PSE_REQUIRE(patch::WriteValue(slot, c.va(kOrig)));   // a feature writing the old value back (scripthost did)
    PSE_CHECK(HooksAt(slot).empty());
    PSE_CHECK_EQ(c.run(kIndirect), 7);
    PSE_REQUIRE(patch::HookSlot(slot, reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1)));
    PSE_REQUIRE(patch::HookSlot(slot, reinterpret_cast<const void*>(&H2), reinterpret_cast<void**>(&g_n2), 0, 10));
    PSE_REQUIRE(patch::WriteValue(slot, reinterpret_cast<uint32_t>(&H1)));
    auto h = HooksAt(slot);
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(c.run(kIndirect), 107);
}

PSE_TEST("patch slots: a slot inside a byte group is hooked outside the broker (warn), UnhookSlot restores it") {
    Env e;
    Code c;
    const uint32_t slot = c.va(kSlot);
    uint8_t same[4];
    std::memcpy(same, c.p + kSlot, 4);
    const BytePatch g[] = {{"slot", slot, same, same, 4, true}};
    PSE_REQUIRE(patch::ApplyGroup("Owns the slot", g));
    g_n1 = nullptr;
    PSE_REQUIRE(patch::HookSlot(slot, reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1)));
    PSE_CHECK(DsLogHas("[warn] pointer hook", "Owns the slot"));
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(g_n1), c.va(kOrig));
    PSE_CHECK_EQ(c.run(kIndirect), 107);
    PSE_CHECK(patch::HookSlot(slot, reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1)));  // again
    PSE_REQUIRE(patch::UnhookSlot(slot, reinterpret_cast<const void*>(&H1)));
    PSE_CHECK_EQ(exe::Read<uint32_t>(slot), c.va(kOrig));
    for (auto& i : HooksAt(slot)) PSE_CHECK_EQ(i.kind, PSE_HOOK_BYTES);
}

// ---- function entries (patch::HookFunction / UnhookFunction over broker HookFunction) ------------------------------

PSE_TEST("patch functions: HookFunction - prologue check, trampoline as next, chain, unhook, Write hands back") {
    Env e;
    Code c;
    const uint32_t entry = c.va(kOrig);   // mov eax, 7; ret
    const uint8_t orig[] = {0xB8, 7, 0, 0, 0, 0xC3}, wrong[] = {0xB8, 8, 0, 0, 0};
    PSE_CHECK_EQ(patch::PrologueLength(entry), 5u);
    PSE_CHECK(!patch::HookFunction(entry, reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1), wrong,
                                   sizeof wrong));
    PSE_CHECK(DsLogHas("[fail] function hook", "unexpected bytes"));
    PSE_CHECK_EQ(c.p[kOrig], 0xB8);
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(patch::HookFunction(entry, reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1), orig,
                                        sizeof orig));
    }
    PSE_CHECK_EQ(c.p[kOrig], 0xE9);
    PSE_CHECK_EQ(c.run(kSite), 107);   // the caller's `call entry` now reaches H1, H1's next the trampoline
    const auto* t = reinterpret_cast<const uint8_t*>(g_n1);
    PSE_CHECK(std::memcmp(t, orig, 5) == 0);   // moved prologue
    PSE_CHECK_EQ(t[5], 0xE9);                  // + jmp entry+5
    int32_t rel;
    std::memcpy(&rel, t + 6, 4);
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(t) + 10 + static_cast<uint32_t>(rel), entry + 5);
    auto h = HooksAt(entry);
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].kind, PSE_HOOK_FUNCTION);
    PSE_CHECK_EQ(h[0].owner, e.alpha);
    PSE_CHECK_EQ(h[0].len, 5u);
    PSE_CHECK_EQ(patch::PrologueLength(entry), 5u);   // from the record while hooked
    PSE_CHECK(patch::HookFunction(entry, reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1), orig,
                                  sizeof orig));   // same fn again
    {
        patch::OwnerScope s(e.beta);   // a second feature on the same entry: chained, not replaced
        PSE_REQUIRE(patch::HookFunction(entry, reinterpret_cast<const void*>(&H2), reinterpret_cast<void**>(&g_n2), orig,
                                        sizeof orig, 0, 100));
    }
    g_order.clear();
    PSE_CHECK_EQ(c.run(kSite), 7 + 100 + 1000);
    PSE_CHECK_STR(g_order.c_str(), "21");
    PSE_REQUIRE(patch::UnhookFunction(entry, reinterpret_cast<const void*>(&H2)));
    PSE_CHECK_EQ(c.run(kSite), 107);
    PSE_REQUIRE(patch::UnhookFunction(entry, reinterpret_cast<const void*>(&H1)));
    PSE_CHECK(std::memcmp(c.p + kOrig, orig, sizeof orig) == 0);
    PSE_CHECK(HooksAt(entry).empty());
    PSE_CHECK(patch::UnhookFunction(entry, reinterpret_cast<const void*>(&H1)));   // not hooked: fine
    // A feature writing the original prologue back: the hook goes to the broker first.
    PSE_REQUIRE(patch::HookFunction(entry, reinterpret_cast<const void*>(&Plain), nullptr));   // replacement, no next
    PSE_CHECK_EQ(c.run(kSite), 42);
    PSE_REQUIRE(patch::Write(entry, orig, 5));
    PSE_CHECK(HooksAt(entry).empty());
    PSE_CHECK_EQ(c.run(kSite), 7);
}

PSE_TEST("patch functions: prologues the broker can't move fail with [fail]; an old jmp@ at the entry gives way") {
    Env e;
    Code c;
    // jecxz in the prologue: refused, nothing written.
    const uint8_t loop[] = {0xE3, 0x10, 0x90, 0x90, 0x90, 0x90, 0xC3};
    std::memcpy(c.p + kData, loop, sizeof loop);
    PSE_CHECK(!patch::HookFunction(c.va(kData), reinterpret_cast<const void*>(&Plain), nullptr));
    PSE_CHECK(DsLogHas("[fail] function hook"));
    PSE_CHECK(std::memcmp(c.p + kData, loop, sizeof loop) == 0);
    // A WriteJump left at the entry is reverted, then the entry is detoured.
    PSE_REQUIRE(patch::WriteJump(c.va(kJump), reinterpret_cast<const void*>(&Plain)));
    PSE_REQUIRE(patch::HookFunction(c.va(kJump), reinterpret_cast<const void*>(&H1), reinterpret_cast<void**>(&g_n1)));
    PSE_CHECK_EQ(c.run(kJump), 3 + 100);
    for (auto& i : HooksAt(c.va(kJump))) PSE_CHECK_EQ(i.kind, PSE_HOOK_FUNCTION);
}

// ---- code groups (WriteJump over a length, WriteCall, RevertCode) --------------------------------------------------

PSE_TEST("patch code: WriteJump(len, original) / WriteCall are checked byte groups; RevertCode and Write take them back") {
    Env e;
    Code c;
    const uint8_t entry[] = {0xB8, 3, 0, 0, 0, 0xC3}, wrong[] = {0xB8, 4, 0, 0, 0, 0xC3};
    PSE_CHECK(!patch::WriteJump(c.va(kJump), reinterpret_cast<const void*>(&Plain), sizeof wrong, wrong));
    PSE_CHECK(DsLogHas("[fail] jump", "unexpected bytes"));
    PSE_CHECK(std::memcmp(c.p + kJump, entry, sizeof entry) == 0);
    {
        patch::OwnerScope s(e.gamma);
        PSE_REQUIRE(patch::WriteJump(c.va(kJump), reinterpret_cast<const void*>(&Plain), 6, entry));
    }
    PSE_CHECK_EQ(c.p[kJump + 5], 0x90);   // the rest of the span is nop, owned by the same group
    PSE_CHECK_EQ(c.run(kJump), 42);
    auto h = HooksAt(c.va(kJump));
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].len, 6u);
    PSE_CHECK_EQ(h[0].owner, e.gamma);
    PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(c.va(kJump) + 5), e.gamma);
    PSE_CHECK(patch::WriteJump(c.va(kJump), reinterpret_cast<const void*>(&Plain), 6, entry));   // again: fine
    PSE_REQUIRE(patch::RevertCode(c.va(kJump)));
    PSE_CHECK(std::memcmp(c.p + kJump, entry, sizeof entry) == 0);
    PSE_CHECK(HooksAt(c.va(kJump)).empty());
    PSE_CHECK(patch::RevertCode(c.va(kJump)));   // nothing there: fine
    // call over a non-call instruction: mov eax, 5 -> call Plain (returns 42), then the ret.
    const uint8_t mov5[] = {0xB8, 5, 0, 0, 0, 0xC3};
    std::memcpy(c.p + kData, mov5, sizeof mov5);
    PSE_REQUIRE(patch::WriteCall(c.va(kData), reinterpret_cast<const void*>(&Plain), 5, mov5));
    PSE_CHECK_EQ(c.p[kData], 0xE8);
    PSE_CHECK_EQ(c.run(kData), 42);
    h = HooksAt(c.va(kData));
    PSE_REQUIRE(h.size() == 1);
    char name[24];
    std::snprintf(name, sizeof name, "call@0x%08X", c.va(kData));
    PSE_CHECK_STR(h[0].name, name);
    PSE_REQUIRE(patch::Write(c.va(kData), mov5, 5));   // the original written back: the group goes first
    PSE_CHECK(HooksAt(c.va(kData)).empty());
    PSE_CHECK_EQ(c.run(kData), 5);
}

// ---- tier 2: the converted DS1 sites on the mapped original exe ----------------------------------------------------

namespace {
struct EntrySite {
    uint32_t entry;
    std::vector<uint8_t> prologue;   // what the feature checks (the original first bytes)
    const char* what;
};
// Every entry the DS1 features detour through patch::HookFunction (2026-10-19).
const EntrySite kEntrySites[] = {
    {0x480930, {0x8B, 0x44, 0x24, 0x04, 0x48}, "coop: front-end state -> screen"},
    {0x4F0080, {0x83, 0x39, 0x00, 0x75, 0x09}, "difficulty: multiplier getter (jne relocated)"},
    {0x459190, {0xA1, 0x78, 0xF5, 0x60, 0x00}, "difficulty: saves left"},
    {0x4515F0, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x10}, "splitscreen: open inventory"},
    {0x451800, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x10}, "splitscreen: open orders"},
    {0x451A30, {0x8B, 0x44, 0x24, 0x04, 0x83, 0xEC, 0x10}, "splitscreen: open give / take"},
    {0x450920, {0x51, 0x53, 0x55, 0x56, 0x8B, 0xF1}, "splitscreen: analog input (AxisValue)"},
    {0x53C9E0, {0x81, 0xEC, 0x94, 0x00, 0x00, 0x00}, "overlay + splitscreen probe: image draw"},
    {0x53A7B0, {0xA1, 0xAC, 0xCC, 0x5F, 0x00}, "dev: text draw probe"},
    {0x4BA4E0, {0x8B, 0x44, 0x24, 0x04, 0xC7, 0x81, 0xD8, 0x00, 0x00, 0x00}, "dev: time scale"},
    {0x4570D0, {0x8B, 0x44, 0x24, 0x04, 0x8B}, "hud: menu font line height"},
    {0x4112B0, {0xA1, 0x60, 0x6A, 0x60, 0x00}, "display: ClipCursor"},
    {0x4FE4C0, {0x8B, 0x41, 0x08, 0x8B, 0x51, 0x0C}, "limits: physics list add"},
    {0x4BA5B0, {0x83, 0xEC, 0x08, 0x53, 0x56}, "timing: frame timer"},
    {0x55DE20, {0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x08}, "voices (dev): sound play"},
    {0x4366D0, {0x83, 0xEC, 0x0C, 0x53, 0x55}, "voices (dev): speech"},
    {0x408680, {0x53, 0x55, 0x56, 0x8B, 0xF1}, "voices (dev): target tracking"},
    {0x456040, {0x83, 0xEC, 0x2C, 0x53, 0x55}, "textfit: choice row"},
    {0x456A70, {0x83, 0xEC, 0x0C, 0x53, 0x56}, "textfit: item text"},
    {0x5230D0, {0x81, 0xEC, 0x40, 0x03, 0x00, 0x00}, "shadows: object volume builder"},
    {0x523440, {0x83, 0xEC, 0x54, 0x53, 0x55}, "shadows: soldier volume builder"},
    {0x4C0420, {0x8B, 0x41, 0x34, 0x53, 0x56}, "memory: heap alloc"},
    {0x4C0460, {0x8B, 0x41, 0x34, 0x53, 0x56}, "memory: heap alloc aligned"},
    {0x4BFB90, {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C}, "memory: whole-file read"},
    {0x4C04F0, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x54, 0x81, 0x20}, "memory: heap release"},
    {0x4C04A0, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x51, 0x30}, "memory: heap pop"},
    {0x4BE810, {0xA1, 0x60, 0x83, 0x5F, 0x00}, "mods: resolver"},
    {0x4BD8A0, {0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x08, 0x02, 0x00, 0x00}, "mods: archive open"},
    {0x5610E0, {0x83, 0xEC, 0x0C, 0x8D, 0x44, 0x24, 0x00}, "mods: stream load"},
    {0x4966C0, {0x8B, 0x44, 0x24, 0x04, 0x56, 0x57}, "weaponicons: picture lookup"},
    {0x435BE0, {0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x08}, "bodies: skin apply"},
    {0x4B98D0, {0x8B, 0x4C, 0x24, 0x04, 0x8D, 0x44, 0x24, 0x08}, "diagnostics: error box"},
    {0x4B9850, {0x8B, 0x4C, 0x24, 0x08, 0x8D, 0x44, 0x24, 0x0C}, "diagnostics: error box (window)"},
    {0x4B9890, {0x8B, 0x4C, 0x24, 0x08, 0x8D, 0x44, 0x24, 0x0C}, "diagnostics: warning box"},
    {0x4B99D0, {0x8B, 0x4C, 0x24, 0x04, 0x8D, 0x44, 0x24, 0x08}, "diagnostics: info box"},
    {0x4BEBD0, {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x0D, 0x30, 0xC5, 0x63, 0x00}, "diagnostics: file open"},
    {0x460960, {0xE8, 0x6B, 0xF5, 0xFF, 0xFF}, "versus: remote soldier check (call relocated)"},
    {0x461060, {0x33, 0xC0, 0xA0, 0xA6, 0xBB, 0x5E, 0x00}, "versus: time limit"},
    {0x5629E0, {0x8B, 0x81, 0x88, 0x0B, 0x00, 0x00}, "versus: host send"},
    {0x562AC0, {0x8B, 0x81, 0x88, 0x0B, 0x00, 0x00}, "versus: client send"},
    {0x4F6730, {0x56, 0x8B, 0xF1, 0x8B, 0x46, 0x60, 0x85, 0xC0, 0x75, 0x05}, "ingame_input: string lookup"},
    {0x539F80, {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x09}, "barcolour (dev): text colour probe"},
    {0x450770, {0x53, 0x55, 0x56, 0x8B, 0xF1}, "lockstep: button evaluator"},
    {0x539DA0, {0xA1, 0x6C, 0x4C, 0x75, 0x00}, "lockstep: raw key held"},
    {0x539DD0, {0xA1, 0x6C, 0x4C, 0x75, 0x00}, "lockstep: raw key pressed"},
    {0x539E10, {0xA1, 0x6C, 0x4C, 0x75, 0x00}, "lockstep: raw key released"},
    {0x539820, {0xA1, 0x6C, 0x4C, 0x75, 0x00}, "lockstep: raw any key"},
    {0x55E130, {0x53, 0x8B, 0x5C, 0x24, 0x08}, "lockstep: sound still playing (replacement)"},
    {0x4B7DF0, {0xA1, 0xE4, 0xDF, 0x61, 0x00}, "determinism (dev): random float draw"},
    {0x4B7E30, {0x8B, 0x0D, 0xE8, 0xDF, 0x61, 0x00}, "determinism (dev): random int draw"},
    {0x4B7E60, {0x8B, 0x0D, 0xE8, 0xDF, 0x61, 0x00}, "determinism (dev): random mod draw"},
    {0x4B7E90, {0x8B, 0x15, 0xE8, 0xDF, 0x61, 0x00}, "determinism (dev): random range draw"},
    {0x5386D0, {0xA1, 0x3C, 0x31, 0x75, 0x00}, "determinism (dev): mouse poll (replacement)"},
    {0x539340, {0x33, 0xC0, 0xC7, 0x05, 0x48, 0x4C, 0x75, 0x00}, "determinism (dev): joystick loop (replacement)"},
};
int __cdecl Dummy() { return 0; }
}  // namespace

PSE_TEST("patch image: every converted entry prologue - HookFunction accepts it, the trampoline decodes to the original") {
    PSE_REQUIRE_EXE();
    exe::Restore();
    Env e;
    for (const EntrySite& s : kEntrySites) {
        std::printf("    0x%08X %s\n", s.entry, s.what);
        PSE_CHECK(patch::Matches(s.entry, s.prologue.data(), s.prologue.size()));
        const uint32_t len = patch::PrologueLength(s.entry);
        PSE_CHECK(len >= 5 && len <= 15);
        uint8_t orig[16];
        std::memcpy(orig, exe::Ptr(s.entry), len);
        void* next = nullptr;
        {
            patch::OwnerScope scope(e.alpha);
            PSE_REQUIRE(patch::HookFunction(s.entry, reinterpret_cast<const void*>(&Dummy), &next, s.prologue.data(),
                                            s.prologue.size()));
        }
        PSE_CHECK_EQ(exe::Read<uint8_t>(s.entry), 0xE9);
        PSE_CHECK_EQ(exe::Read<uint32_t>(s.entry + 1) + s.entry + 5, reinterpret_cast<uint32_t>(&Dummy));
        for (uint32_t i = 5; i < len; ++i) PSE_CHECK_EQ(exe::Read<uint8_t>(s.entry + i), 0xCC);
        // The trampoline: the same instructions (relative ones re-targeted at the same address), then jmp entry+len.
        const auto* t = static_cast<const uint8_t*>(next);
        PSE_REQUIRE(t);
        uint32_t o = 0, to = 0;
        while (o < len) {
            pse::x86::Insn a, b;
            const uint32_t na = pse::x86::Decode(orig + o, len - o, &a), nb = pse::x86::Decode(t + to, 15, &b);
            PSE_REQUIRE(na && nb);
            if (!a.relLen) {
                PSE_CHECK(na == nb && std::memcmp(orig + o, t + to, na) == 0);
            } else {
                int32_t da = 0, db = 0;
                if (a.relLen == 1) da = static_cast<int8_t>(orig[o + na - 1]);
                else std::memcpy(&da, orig + o + na - 4, 4);
                PSE_REQUIRE(b.relLen == 4);
                std::memcpy(&db, t + to + nb - 4, 4);
                PSE_CHECK_EQ(s.entry + o + na + static_cast<uint32_t>(da),
                             reinterpret_cast<uint32_t>(t) + to + nb + static_cast<uint32_t>(db));
            }
            o += na, to += nb;
        }
        PSE_CHECK_EQ(t[to], 0xE9);
        int32_t back;
        std::memcpy(&back, t + to + 1, 4);
        PSE_CHECK_EQ(reinterpret_cast<uint32_t>(t) + to + 5 + static_cast<uint32_t>(back), s.entry + len);
        PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(s.entry), e.alpha);
        PSE_REQUIRE(patch::UnhookFunction(s.entry, reinterpret_cast<const void*>(&Dummy)));
        PSE_CHECK(std::memcmp(exe::Ptr(s.entry), orig, len) == 0);
    }
    exe::Restore();
}

namespace {
struct SlotSite {
    uint32_t slot, original;
    const char* what;
};
// Every pointer slot the DS1 features hook through patch::HookSlot with an expected value (IAT slots aren't in the
// mapped image - they trap).
const SlotSite kSlotSites[] = {
    {0x5D93E0, 0x48CE40, "campaigns: SP menu update"},       {0x5D86C4, 0x415120, "coop: main menu set-up"},
    {0x5D93DC, 0x48CCB0, "coop: SP menu set-up"},            {0x5D85F8, 0x413670, "coop: difficulty update"},
    {0x5D9610, 0x49BAF0, "coop: uniform update"},            {0x5D8E40, 0x4595D0, "coop: mission list update"},
    {0x449A90, 0x477020, "coop: load list callback push"},   {0x4751FF, 0x477020, "coop: save list callback push"},
    {0x459509, 0x4596B0, "coop: mission draw callback push"}, {0x5D85F4, 0x4134E0, "difficulty: menu set-up"},
    {0x5ED654, 0x482050, "difficulty: API set_team_skill"},  {0x5D91B4, 0x471870, "fixmenu: Options create"},
    {0x5D91BC, 0x4719D0, "fixmenu: Options set-up"},         {0x5D97D0, 0x4A6EA0, "menus: YES/NO enter"},
    {0x5D9214, 0x471F30, "pausemenu: set-up"},               {0x5ED2B0, 0x485950, "timeofday: API SetTOD"},
    {0x5ED2B4, 0x485970, "timeofday: API GetTOD"},           {0x5ED2B8, 0x485980, "timeofday: API SetSpeed"},
    {0x5ED2C8, 0x4859F0, "timeofday: API RunUntil"},         {0x48D79B, 0x48D9C0, "splitscreen: panel callback push"},
    {0x44793D, 0x4478C0, "launcher: main dialog proc push"}, {0x44798F, 0x447900, "launcher: Detail Settings proc push"},
    {0x538C09, 0x538800, "hotplug (dev NoPads): enum callback push"},
};
}  // namespace

PSE_TEST("patch image: every converted pointer slot holds its expected original; HookSlot + UnhookSlot round trip") {
    PSE_REQUIRE_EXE();
    exe::Restore();
    Env e;
    for (const SlotSite& s : kSlotSites) {
        std::printf("    0x%08X %s\n", s.slot, s.what);
        PSE_CHECK_EQ(exe::Read<uint32_t>(s.slot), s.original);
        void* next = nullptr;
        {
            patch::OwnerScope scope(e.beta);
            PSE_REQUIRE(patch::HookSlot(s.slot, reinterpret_cast<const void*>(&Dummy), &next, s.original));
        }
        PSE_CHECK_EQ(reinterpret_cast<uint32_t>(next), s.original);
        PSE_CHECK_EQ(exe::Read<uint32_t>(s.slot), reinterpret_cast<uint32_t>(&Dummy));
        PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(s.slot), e.beta);
        PSE_REQUIRE(patch::UnhookSlot(s.slot, reinterpret_cast<const void*>(&Dummy)));
        PSE_CHECK_EQ(exe::Read<uint32_t>(s.slot), s.original);
    }
    exe::Restore();
}

namespace {
struct CodeSite {
    uint32_t site;
    std::vector<uint8_t> original;
    bool call;   // WriteCall (else WriteJump)
    const char* what;
};
// The hand-written call / jmp byte writes now made with WriteCall / WriteJump(len, original).
const CodeSite kCodeSites[] = {
    {0x538F2A, {0x39, 0x3D, 0x6C, 0x4C, 0x75, 0x00}, true, "controller: poll"},
    {0x486FDE, {0x6A, 0x12, 0xFF, 0x51, 0x08}, true, "overview: message 0x12"},
    {0x47C193, {0x8B, 0x0D, 0x2C, 0x13, 0x61, 0x00, 0x8B, 0x11, 0xFF, 0x52, 0x08}, true, "versus: scores list draw"},
    {0x40F3A5, {0xA1, 0xB4, 0x6C, 0x5E, 0x00}, true, "ingame_input: cursor flag"},
    {0x48D6D5, {0xC6, 0x44, 0x24, 0x10, 0x00}, true, "splitscreen: panel walk end"},
    {0x45E9F7, {0xD8, 0x05, 0xAC, 0x90, 0x5D, 0x00}, true, "hud: SESSION NAME width"},
    {0x45EA6C, {0xD8, 0x05, 0x9C, 0x89, 0x5D, 0x00}, true, "hud: second field width"},
    {0x4152F8, {0x66, 0x81, 0xFE, 0xFC, 0x03, 0x7E, 0x62}, false, "coop: main menu select"},
    {0x4136A6, {0x0F, 0xBF, 0xC6, 0x2D, 0x03, 0x08, 0x00, 0x00}, false, "difficulty: id -> level"},
    {0x42220F, {0x8B, 0x84, 0xBA, 0xDC, 0x00, 0x00, 0x00, 0x50, 0xE8, 0x44, 0x40, 0x01, 0x00}, false, "units: item slot"},
    {0x45BD4F, {0x0F, 0x85, 0x54, 0xFD, 0xFF, 0xFF}, false, "jpfixes: route failure"},
    {0x50CD21, {0x8B, 0x76, 0x10, 0x85, 0xF6, 0x0F, 0x85, 0x0A, 0xFF, 0xFF, 0xFF}, false, "jpfixes: path next"},
    {0x50CC7A, {0x8B, 0x44, 0x24, 0x14, 0x85, 0xC0, 0x75, 0x0E}, false, "jpfixes: path node"},
    {0x51850E, {0x39, 0xBE, 0x90, 0x0F, 0x00, 0x00, 0x0F, 0x8E, 0x4D, 0x01, 0x00, 0x00}, false, "jpfixes: stuck tank gate"},
    {0x4BFC89, {0xE9, 0xD2, 0xE6, 0xFF, 0xFF}, false, "mods: archive exists tail jump"},
};
}  // namespace

PSE_TEST("patch image: every converted call / jmp code site holds its original bytes; the group round-trips") {
    PSE_REQUIRE_EXE();
    exe::Restore();
    Env e;
    for (const CodeSite& s : kCodeSites) {
        std::printf("    0x%08X %s\n", s.site, s.what);
        PSE_CHECK(patch::Matches(s.site, s.original.data(), s.original.size()));
        {
            patch::OwnerScope scope(e.gamma);
            const void* fn = reinterpret_cast<const void*>(&Dummy);
            PSE_REQUIRE(s.call ? patch::WriteCall(s.site, fn, s.original.size(), s.original.data())
                               : patch::WriteJump(s.site, fn, s.original.size(), s.original.data()));
        }
        PSE_CHECK_EQ(exe::Read<uint8_t>(s.site), s.call ? 0xE8 : 0xE9);
        for (size_t i = 5; i < s.original.size(); ++i) PSE_CHECK_EQ(exe::Read<uint8_t>(s.site + static_cast<uint32_t>(i)), 0x90);
        PSE_CHECK_EQ(pse::HooksInterface()->OwnerOf(s.site + static_cast<uint32_t>(s.original.size()) - 1), e.gamma);
        PSE_REQUIRE(patch::RevertCode(s.site));
        PSE_CHECK(patch::Matches(s.site, s.original.data(), s.original.size()));
    }
    exe::Restore();
}

// ---- COM vtables (core/comhooks.h over patch::HookSlot) - graphics.cpp / memory.cpp --------------------------------

namespace {
// A fake COM class: vtable of 4 stdcall methods (int(this, x)), each returning class id * 10 + slot + x. Two classes
// stand for d3d8.dll's device and the D3D9 wrapper's (another implementation = another vtable).
using ComFn = int(__stdcall*)(void*, int);
template <int Cls, int Slot>
int __stdcall ComOrig(void*, int x) { return Cls * 10 + Slot + x; }
struct FakeClass {
    void** vtable;
    explicit FakeClass(bool cls2) {
        // its own page, read-only like a DLL's .rdata: the broker must unprotect it
        vtable = static_cast<void**>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        if (cls2) {
            vtable[0] = reinterpret_cast<void*>(&ComOrig<2, 0>), vtable[1] = reinterpret_cast<void*>(&ComOrig<2, 1>);
            vtable[2] = reinterpret_cast<void*>(&ComOrig<2, 2>), vtable[3] = reinterpret_cast<void*>(&ComOrig<2, 3>);
        } else {
            vtable[0] = reinterpret_cast<void*>(&ComOrig<1, 0>), vtable[1] = reinterpret_cast<void*>(&ComOrig<1, 1>);
            vtable[2] = reinterpret_cast<void*>(&ComOrig<1, 2>), vtable[3] = reinterpret_cast<void*>(&ComOrig<1, 3>);
        }
        DWORD old;
        VirtualProtect(vtable, 0x1000, PAGE_READONLY, &old);
    }
    ~FakeClass() { VirtualFree(vtable, 0, MEM_RELEASE); }
};
struct FakeObject {
    void** vtable;
    int id;
};
int CallSlot(FakeObject& o, int slot, int x) { return reinterpret_cast<ComFn>(o.vtable[slot])(&o, x); }

using ComTable = comhooks::Table<4>;
ComTable g_com;
const comhooks::Next<ComTable, g_com, 3, ComFn> g_comNext3{};
std::string g_comOrder;
int __stdcall ComHook3(void* obj, int x) {   // the feature's hook: +100, then the chain's next for obj's class
    g_comOrder += 'F';
    return g_comNext3(obj, x) + 100;
}
ComFn g_extFrontNext = nullptr, g_extBackNext = nullptr;
int __stdcall ExtFront(void* obj, int x) { g_comOrder += 'A'; return g_extFrontNext(obj, x) + 1000; }
int __stdcall ExtBack(void* obj, int x) { g_comOrder += 'B'; return g_extBackNext(obj, x) + 10000; }
}  // namespace

PSE_TEST("patch com: one record per vtable class - shared by objects, own next per class, read-only vtables") {
    Env e;
    g_com.ResetForTests();
    FakeClass a(false), b(true);
    FakeObject a1{a.vtable, 1}, a2{a.vtable, 2}, b1{b.vtable, 3};
    PSE_CHECK(!g_comNext3);
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(g_com.Hook(&a1, 3, reinterpret_cast<const void*>(&ComHook3)));
    }
    PSE_CHECK(static_cast<bool>(g_comNext3));
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(a.vtable[3]), reinterpret_cast<uint32_t>(&ComHook3));
    PSE_CHECK_EQ(CallSlot(a1, 3, 5), 13 + 5 + 100);
    PSE_CHECK_EQ(CallSlot(a2, 3, 5), 13 + 5 + 100);   // same class: hooked without anything new
    PSE_CHECK(g_com.Hooked(&a2, 3));
    PSE_CHECK(!g_com.Hooked(&b1, 3));
    PSE_CHECK_EQ(CallSlot(b1, 3, 5), 23 + 5);          // other class untouched
    // Idempotent: the second device / CreateDevice again
    PSE_CHECK(g_com.Hook(&a2, 3, reinterpret_cast<const void*>(&ComHook3)));
    PSE_CHECK_EQ(HooksAt(reinterpret_cast<uint32_t>(&a.vtable[3])).size(), size_t{1});
    // A second class (the D3D9 wrapper / another driver): its own record and original
    {
        patch::OwnerScope s(e.alpha);
        PSE_REQUIRE(g_com.Hook(&b1, 3, reinterpret_cast<const void*>(&ComHook3)));
    }
    PSE_CHECK_EQ(g_com.ClassCount(), 2);
    PSE_CHECK_EQ(CallSlot(b1, 3, 5), 23 + 5 + 100);
    PSE_CHECK_EQ(CallSlot(a1, 3, 5), 13 + 5 + 100);
    auto h = HooksAt(reinterpret_cast<uint32_t>(&b.vtable[3]));
    PSE_REQUIRE(h.size() == 1);
    PSE_CHECK_EQ(h[0].kind, PSE_HOOK_POINTER);
    PSE_CHECK_EQ(h[0].owner, e.alpha);
    // Read-only page stays read-only-ish (not writable) after the write
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(a.vtable, &mbi, sizeof mbi);
    PSE_CHECK(!(mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)));
    // Unhook: the class's slot gets its original back, the other class stays hooked
    PSE_REQUIRE(g_com.Unhook(&a1, 3));
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(a.vtable[3]), reinterpret_cast<uint32_t>(&ComOrig<1, 3>));
    PSE_CHECK_EQ(CallSlot(a1, 3, 5), 13 + 5);
    PSE_CHECK_EQ(CallSlot(b1, 3, 5), 23 + 5 + 100);
    PSE_REQUIRE(g_com.Unhook(&b1, 3));
    PSE_CHECK_EQ(CallSlot(b1, 3, 5), 23 + 5);
    PSE_CHECK(!g_comNext3);
    // Index out of range / null object
    PSE_CHECK(!g_com.Hook(&a1, 4, reinterpret_cast<const void*>(&ComHook3)));
    PSE_CHECK(!g_com.Hook(nullptr, 3, reinterpret_cast<const void*>(&ComHook3)));
    g_com.ResetForTests();
}

PSE_TEST("patch com: another plugin chains in front of and behind the feature's vtable hook (Present / EndScene)") {
    Env e;
    g_com.ResetForTests();
    FakeClass a(false);
    FakeObject a1{a.vtable, 1};
    {
        patch::OwnerScope s(e.alpha);   // dsfix.graphics at priority 0
        PSE_REQUIRE(g_com.Hook(&a1, 3, reinterpret_cast<const void*>(&ComHook3), 0));
    }
    const uint32_t slot = reinterpret_cast<uint32_t>(&a.vtable[3]);
    PSEHookId front = 0, back = 0;
    // An external plugin registers straight through the broker on the slot address (vtable of the device it got)
    PSE_REQUIRE(pse::HooksInterface()->HookPointer(e.beta, slot, reinterpret_cast<void*>(&ExtFront), 10,
                                                   reinterpret_cast<void**>(&g_extFrontNext), &front) == PSE_OK);
    PSE_REQUIRE(pse::HooksInterface()->HookPointer(e.gamma, slot, reinterpret_cast<void*>(&ExtBack), -10,
                                                   reinterpret_cast<void**>(&g_extBackNext), &back) == PSE_OK);
    g_comOrder.clear();
    PSE_CHECK_EQ(CallSlot(a1, 3, 1), 13 + 1 + 100 + 1000 + 10000);
    PSE_CHECK_STR(g_comOrder.c_str(), "AFB");
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(a.vtable[3]), reinterpret_cast<uint32_t>(&ExtFront));
    // the feature's own `next` (for its direct calls, e.g. ApplyAnisotropy's SetTextureStageState) = the plugin behind
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(g_com.Next(&a1, 3)), reinterpret_cast<uint32_t>(&ExtBack));
    auto h = HooksAt(slot);
    PSE_CHECK_EQ(h.size(), size_t{3});
    // the feature leaves; the plugins stay chained around where it was
    PSE_REQUIRE(g_com.Unhook(&a1, 3));
    g_comOrder.clear();
    PSE_CHECK_EQ(CallSlot(a1, 3, 1), 13 + 1 + 1000 + 10000);
    PSE_CHECK_STR(g_comOrder.c_str(), "AB");
    PSE_REQUIRE(pse::HooksInterface()->Unhook(e.beta, front) == PSE_OK);
    PSE_REQUIRE(pse::HooksInterface()->Unhook(e.gamma, back) == PSE_OK);
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(a.vtable[3]), reinterpret_cast<uint32_t>(&ComOrig<1, 3>));
    g_com.ResetForTests();
}

PSE_TEST("patch com: a slot that already holds the hook without a record is refused (no next to call)") {
    Env e;
    g_com.ResetForTests();
    FakeClass a(false);
    FakeObject a1{a.vtable, 1};
    void* fn = reinterpret_cast<void*>(&ComHook3);
    PSE_REQUIRE(patch::Write(reinterpret_cast<uint32_t>(&a.vtable[3]), &fn, 4));   // someone wrote it raw
    PSE_CHECK(!g_com.Hook(&a1, 3, fn));
    PSE_CHECK(!g_com.Hooked(&a1, 3));
    g_com.ResetForTests();
}

// ---- shared clocks (lockstep.cpp + determinism.cpp) on the mapped exe ----------------------------------------------

namespace {
using TimeFn0 = DWORD(WINAPI*)();
TimeFn0 g_detNext = nullptr, g_lockNext = nullptr;
bool g_detOn = false, g_lockOn = false;
DWORD WINAPI DetTime() { return g_detOn ? 1000 : g_detNext(); }
DWORD WINAPI LockTime() { return g_lockOn ? 2000 : g_lockNext(); }
using ClockFn0 = uint32_t(__cdecl*)();
ClockFn0 g_detClockNext = nullptr, g_lockClockNext = nullptr;
uint32_t __cdecl DetClock() { return g_detOn ? 1000 : g_detClockNext(); }
uint32_t __cdecl LockClock() { return g_lockOn ? 2000 : g_lockClockNext(); }
}  // namespace

PSE_TEST("patch image: timeGetTime / GetTickCount import slots chain lockstep + determinism in either order") {
    PSE_REQUIRE_EXE();
    for (int order = 0; order < 2; ++order) {
        exe::Restore();
        Env e;   // alpha = dsfix.lockstep, beta = dsfix.determinism
        for (uint32_t slot : {features::kIatTimeGetTime, features::kIatGetTickCount}) {
            std::printf("    0x%08X %s, %s first\n", slot, exe::ImportName(slot).c_str(), order ? "determinism" : "lockstep");
            const uint32_t original = exe::Read<uint32_t>(slot);
            g_detNext = g_lockNext = nullptr;
            auto lock = [&] {
                patch::OwnerScope s(e.alpha);
                return patch::HookSlot(slot, reinterpret_cast<const void*>(&LockTime), reinterpret_cast<void**>(&g_lockNext),
                                       0, features::kClockLockstep);
            };
            auto det = [&] {
                patch::OwnerScope s(e.beta);
                return patch::HookSlot(slot, reinterpret_cast<const void*>(&DetTime), reinterpret_cast<void**>(&g_detNext),
                                       0, features::kClockDeterminism);
            };
            if (order == 0) {
                PSE_REQUIRE(lock());
                PSE_REQUIRE(det());
            } else {
                PSE_REQUIRE(det());
                PSE_REQUIRE(lock());
            }
            // determinism first, then lockstep, then the real import - whatever the order of installation
            PSE_CHECK_EQ(exe::Read<uint32_t>(slot), reinterpret_cast<uint32_t>(&DetTime));
            PSE_CHECK_EQ(reinterpret_cast<uint32_t>(g_detNext), reinterpret_cast<uint32_t>(&LockTime));
            PSE_CHECK_EQ(reinterpret_cast<uint32_t>(g_lockNext), original);
            auto call = [&] { return reinterpret_cast<TimeFn0>(exe::Read<uint32_t>(slot))(); };
            g_detOn = true, g_lockOn = true;
            PSE_CHECK_EQ(call(), 1000u);   // Dev\DetTest's virtual clock wins
            g_detOn = false;
            PSE_CHECK_EQ(call(), 2000u);   // else lockstep's
            g_lockOn = false;
            auto h = HooksAt(slot);
            PSE_REQUIRE(h.size() == 2);
            PSE_CHECK_EQ(h[0].owner, e.beta);
            PSE_CHECK_EQ(h[1].owner, e.alpha);
            PSE_REQUIRE(patch::UnhookSlot(slot, reinterpret_cast<const void*>(&DetTime)));
            PSE_CHECK_EQ(exe::Read<uint32_t>(slot), reinterpret_cast<uint32_t>(&LockTime));
            PSE_REQUIRE(patch::UnhookSlot(slot, reinterpret_cast<const void*>(&LockTime)));
            PSE_CHECK_EQ(exe::Read<uint32_t>(slot), original);
        }
        // the game timers' clock calls: SubscribeCall chains (each site its own next)
        for (uint32_t site : {features::kClockCallCtor, features::kClockCallTick}) {
            PSE_CHECK_EQ(exe::Read<uint8_t>(site), 0xE8);
            g_detClockNext = g_lockClockNext = nullptr;
            auto lock = [&] {
                patch::OwnerScope s(e.alpha);
                return patch::SubscribeCall(site, reinterpret_cast<const void*>(&LockClock), features::kClockTarget,
                                            features::kClockLockstep, reinterpret_cast<void**>(&g_lockClockNext));
            };
            auto det = [&] {
                patch::OwnerScope s(e.beta);
                return patch::SubscribeCall(site, reinterpret_cast<const void*>(&DetClock), features::kClockTarget,
                                            features::kClockDeterminism, reinterpret_cast<void**>(&g_detClockNext));
            };
            if (order == 0) {
                PSE_REQUIRE(lock());
                PSE_REQUIRE(det());
            } else {
                PSE_REQUIRE(det());
                PSE_REQUIRE(lock());
            }
            PSE_CHECK_EQ(exe::Read<uint32_t>(site + 1) + site + 5, reinterpret_cast<uint32_t>(&DetClock));
            PSE_CHECK_EQ(reinterpret_cast<uint32_t>(g_detClockNext), reinterpret_cast<uint32_t>(&LockClock));
            PSE_CHECK_EQ(reinterpret_cast<uint32_t>(g_lockClockNext), features::kClockTarget);
            g_detOn = true;
            PSE_CHECK_EQ(DetClock(), 1000u);
            g_detOn = false, g_lockOn = true;
            PSE_CHECK_EQ(DetClock(), 2000u);
            g_lockOn = false;
            // a plain HookCall on the original target still can't join silently
            PSE_CHECK(!patch::HookCall(site, reinterpret_cast<const void*>(&Dummy), features::kClockTarget));
            // (each subscriber needs its own next per site - the features have one pair per site)
            PSE_REQUIRE(patch::HookCall(site, reinterpret_cast<const void*>(features::kClockTarget),
                                        reinterpret_cast<uint32_t>(&DetClock)));   // back to the original: all off
            PSE_CHECK_EQ(exe::Read<uint32_t>(site + 1) + site + 5, features::kClockTarget);
            PSE_CHECK(HooksAt(site).empty());
        }
    }
    exe::Restore();
}

PSE_TEST("patch image: DirectSoundCreate8 import slot - determinism's no-driver hook in front of dev's focus hook") {
    PSE_REQUIRE_EXE();
    exe::Restore();
    Env e;
    constexpr uint32_t kSlotDs = 0x5D8030;
    PSE_CHECK(exe::ImportName(kSlotDs).find("DirectSoundCreate8") != std::string::npos);
    const uint32_t original = exe::Read<uint32_t>(kSlotDs);
    void* devNext = nullptr;
    void* detNext = nullptr;
    {
        patch::OwnerScope s(e.alpha);   // dev.cpp GlobalFocusAudio, priority 0
        PSE_REQUIRE(patch::HookSlot(kSlotDs, reinterpret_cast<const void*>(&Dummy), &devNext));
    }
    {
        patch::OwnerScope s(e.beta);    // determinism DetNoSound
        PSE_REQUIRE(patch::HookSlot(kSlotDs, reinterpret_cast<const void*>(&H1), &detNext, 0, features::kClockDeterminism));
    }
    PSE_CHECK_EQ(exe::Read<uint32_t>(kSlotDs), reinterpret_cast<uint32_t>(&H1));
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(detNext), reinterpret_cast<uint32_t>(&Dummy));
    PSE_CHECK_EQ(reinterpret_cast<uint32_t>(devNext), original);
    PSE_REQUIRE(patch::UnhookSlot(kSlotDs, reinterpret_cast<const void*>(&H1)));
    PSE_REQUIRE(patch::UnhookSlot(kSlotDs, reinterpret_cast<const void*>(&Dummy)));
    PSE_CHECK_EQ(exe::Read<uint32_t>(kSlotDs), original);
    exe::Restore();
}

// pse.hooks broker tests: hand-assembled code in executable buffers, called through function pointers.
#include <windows.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "loader.h"
#include "pse_test.h"
#include "x86len.h"

namespace {

using Fn = int(__cdecl*)(int);

const PSEHooksInterface& H() { return *pse::HooksInterface(); }

constexpr PSEPluginHandle kAlpha = 1, kBeta = 2, kGamma = 3;
std::vector<std::string> g_logs;

const char* TestName(PSEPluginHandle p) {
    switch (p) {
        case kAlpha: return "alpha";
        case kBeta: return "beta";
        case kGamma: return "gamma";
        default: return "";
    }
}
void TestLog(PSEPluginHandle p, PSELogLevel, const char* text) { g_logs.push_back(std::string(TestName(p)) + ": " + text); }

bool LogHas(const char* a, const char* b = nullptr) {
    for (auto& l : g_logs)
        if (l.find(a) != std::string::npos && (!b || l.find(b) != std::string::npos)) return true;
    return false;
}

// Resets the broker before and after each test (buffers are freed when the test ends).
struct Fresh {
    Fresh() {
        pse::hooks::ResetForTests();
        pse::hooks::SetEnvironment(&TestName, &TestLog);
        g_logs.clear();
    }
    ~Fresh() { pse::hooks::ResetForTests(); }
};

struct Buf {
    uint8_t* p;
    Buf() {
        p = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        std::memset(p, 0xCC, 0x1000);
    }
    ~Buf() { VirtualFree(p, 0, MEM_RELEASE); }
    uint32_t va(uint32_t off = 0) const { return reinterpret_cast<uint32_t>(p) + off; }
    void put(uint32_t off, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) p[off++] = b;
    }
    void rel(uint32_t off, uint8_t op, uint32_t target) {  // op rel32 at off
        p[off] = op;
        int32_t r = static_cast<int32_t>(target - (va(off) + 5));
        std::memcpy(p + off + 1, &r, 4);
    }
    Fn fn(uint32_t off) const { return reinterpret_cast<Fn>(va(off)); }
};

// Chain members: each records its letter and adds its weight, so the result shows who ran.
std::string g_order;
Fn g_nA, g_nB, g_nC, g_nD, g_nE;
int __cdecl HA(int x) { g_order += 'A'; return g_nA(x) + 1000; }
int __cdecl HB(int x) { g_order += 'B'; return g_nB(x) + 100; }
int __cdecl HC(int x) { g_order += 'C'; return g_nC(x) + 10; }
int __cdecl HD(int x) { g_order += 'D'; return g_nD(x) + 10000; }
int __cdecl HE(int x) { g_order += 'E'; return g_nE(x) + 7; }
void** N(Fn& f) { return reinterpret_cast<void**>(&f); }

int Call(Fn f, int x) {
    g_order.clear();
    return f(x);
}

// x + 1:  mov eax,[esp+4]; inc eax; ret
void PutPlus1(Buf& b, uint32_t off) { b.put(off, {0x8B, 0x44, 0x24, 0x04, 0x40, 0xC3}); }
// caller(x) = target(x):  push [esp+4]; call target; add esp,4; ret   (call site at off + 4)
void PutCaller(Buf& b, uint32_t off, uint32_t target) {
    b.put(off, {0xFF, 0x74, 0x24, 0x04});
    b.rel(off + 4, 0xE8, target);
    b.put(off + 9, {0x83, 0xC4, 0x04, 0xC3});
}

}  // namespace

// ---- decoder ------------------------------------------------------------------------------------------------------

PSE_TEST("hooks: x86 length decoder") {
    struct Case {
        std::vector<uint8_t> bytes;
        uint32_t len;
    };
    const Case cases[] = {
        {{0x55}, 1},                                                // push ebp
        {{0x8B, 0xEC}, 2},                                          // mov ebp,esp
        {{0x83, 0xEC, 0x08}, 3},                                    // sub esp,8
        {{0x81, 0xEC, 0x00, 0x01, 0x00, 0x00}, 6},                  // sub esp,0x100
        {{0x8B, 0x44, 0x24, 0x04}, 4},                              // mov eax,[esp+4] (SIB + disp8)
        {{0x8B, 0x84, 0x24, 0x00, 0x01, 0x00, 0x00}, 7},            // mov eax,[esp+0x100] (SIB + disp32)
        {{0x8B, 0x04, 0x85, 0x00, 0x10, 0x40, 0x00}, 7},            // mov eax,[eax*4+0x401000] (SIB base 5, mod 0)
        {{0xA1, 0x24, 0xC9, 0x63, 0x00}, 5},                        // mov eax,[0x63C924] (moffs)
        {{0x8B, 0x0D, 0x24, 0xC9, 0x63, 0x00}, 6},                  // mov ecx,[disp32]
        {{0xC7, 0x05, 1, 2, 3, 4, 5, 6, 7, 8}, 10},                 // mov dword [disp32],imm32
        {{0x66, 0xC7, 0x05, 1, 2, 3, 4, 5, 6}, 9},                  // mov word [disp32],imm16
        {{0x66, 0xB8, 0x34, 0x12}, 4},                              // mov ax,0x1234
        {{0x66, 0x81, 0xC1, 0x34, 0x12}, 5},                        // add cx,0x1234
        {{0x68, 1, 2, 3, 4}, 5},                                    // push imm32
        {{0x6A, 0x05}, 2},                                          // push imm8
        {{0xE8, 0, 0, 0, 0}, 5},                                    // call rel32
        {{0xE9, 0, 0, 0, 0}, 5},                                    // jmp rel32
        {{0x74, 0x10}, 2},                                          // je rel8
        {{0x0F, 0x84, 0, 0, 0, 0}, 6},                              // je rel32
        {{0xEB, 0xFE}, 2},                                          // jmp rel8
        {{0xC2, 0x0C, 0x00}, 3},                                    // ret 0xC
        {{0xC3}, 1},                                                // ret
        {{0xC8, 0x10, 0x00, 0x00}, 4},                              // enter 16,0
        {{0xF6, 0xC1, 0x01}, 3},                                    // test cl,1 (group 3 imm)
        {{0xF7, 0xC1, 1, 2, 3, 4}, 6},                              // test ecx,imm32
        {{0x66, 0xF7, 0xC1, 1, 2}, 5},                              // test cx,imm16
        {{0xF7, 0xD8}, 2},                                          // neg eax (group 3, no imm)
        {{0xF6, 0x05, 1, 2, 3, 4, 0x80}, 7},                        // test byte [disp32],0x80
        {{0xD9, 0x45, 0x08}, 3},                                    // fld dword [ebp+8]
        {{0xD9, 0xE8}, 2},                                          // fld1
        {{0xDD, 0x05, 1, 2, 3, 4}, 6},                              // fld qword [disp32]
        {{0xDE, 0xC9}, 2},                                          // fmulp st1
        {{0xDF, 0xE0}, 2},                                          // fnstsw ax
        {{0x9B}, 1},                                                // wait
        {{0x0F, 0x28, 0xC1}, 3},                                    // movaps xmm0,xmm1
        {{0xF3, 0x0F, 0x10, 0x44, 0x24, 0x04}, 6},                  // movss xmm0,[esp+4]
        {{0x66, 0x0F, 0x6F, 0x05, 1, 2, 3, 4}, 8},                  // movdqa xmm0,[disp32]
        {{0x0F, 0xC6, 0xC1, 0x1B}, 4},                              // shufps xmm0,xmm1,0x1B
        {{0x66, 0x0F, 0x38, 0x00, 0xC1}, 5},                        // pshufb xmm0,xmm1
        {{0x66, 0x0F, 0x3A, 0x0F, 0xC1, 0x04}, 6},                  // palignr xmm0,xmm1,4
        {{0x0F, 0xA2}, 2},                                          // cpuid
        {{0x0F, 0x31}, 2},                                          // rdtsc
        {{0x0F, 0xB6, 0x45, 0x08}, 4},                              // movzx eax,byte [ebp+8]
        {{0x0F, 0xAF, 0xC1}, 3},                                    // imul eax,ecx
        {{0x0F, 0xA4, 0xC1, 0x04}, 4},                              // shld ecx,eax,4
        {{0x0F, 0xBA, 0xE0, 0x03}, 4},                              // bt eax,3
        {{0x0F, 0x0F, 0xC1, 0x9E}, 4},                              // pfadd mm0,mm1 (3DNow!)
        {{0xF0, 0x0F, 0xB1, 0x0A}, 4},                              // lock cmpxchg [edx],ecx
        {{0xF3, 0xA5}, 2},                                          // rep movsd
        {{0x64, 0xA1, 0, 0, 0, 0}, 6},                              // mov eax,fs:[0]
        {{0x67, 0x8B, 0x46, 0x10}, 4},                              // mov eax,[bp+0x10] (16-bit addressing)
        {{0x67, 0x8B, 0x06, 0x34, 0x12}, 5},                        // mov eax,[0x1234] (16-bit disp16)
        {{0x9A, 1, 2, 3, 4, 5, 6}, 7},                              // call far ptr16:32
        {{0xFF, 0x15, 1, 2, 3, 4}, 6},                              // call [disp32]
        {{0xFF, 0x24, 0x85, 1, 2, 3, 4}, 7},                        // jmp [eax*4+disp32]
        {{0x8D, 0x4C, 0x24, 0x10}, 4},                              // lea ecx,[esp+0x10]
        {{0xC0, 0xE0, 0x04}, 3},                                    // shl al,4
        {{0x69, 0xC0, 1, 2, 3, 4}, 6},                              // imul eax,eax,imm32
        {{0x6B, 0xC0, 0x0C}, 3},                                    // imul eax,eax,12
        {{0xE3, 0x00}, 2},                                          // jecxz
        {{0x0F, 0x04}, 0},                                          // undefined
        {{0x0F, 0xFF, 0xC0}, 3},                                    // ud0 (ModRM)
        {{0xC5, 0xF8, 0x77}, 0},                                    // VEX vzeroupper: not supported
        {{0x8B}, 0},                                                // truncated
        {{0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90}, 15},
        {{0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x90}, 0},
    };
    for (const Case& c : cases) {
        uint32_t len = pse::x86::Length(c.bytes.data(), static_cast<uint32_t>(c.bytes.size()));
        if (len != c.len) {
            char b[160];
            int n = std::snprintf(b, sizeof b, "bytes");
            for (uint8_t x : c.bytes) n += std::snprintf(b + n, sizeof b - n, " %02X", x);
            ::pse_test::Fail(__FILE__, __LINE__, std::string(b) + ": length " + std::to_string(len) + ", expected " +
                                                     std::to_string(c.len));
        }
    }
    pse::x86::Insn in;
    PSE_CHECK_EQ(pse::x86::Decode((const uint8_t*)"\x0F\x85\0\0\0\0", 6, &in), 6u);
    PSE_CHECK_EQ(in.relLen, 4u);
    PSE_CHECK_EQ(pse::x86::Decode((const uint8_t*)"\x66\xE8\x01\x02", 4, &in), 4u);
    PSE_CHECK_EQ(in.relLen, 2u);
    PSE_CHECK_EQ(pse::x86::Decode((const uint8_t*)"\x8B\xC1", 2, &in), 2u);
    PSE_CHECK_EQ(in.relLen, 0u);
}

// ---- code memory --------------------------------------------------------------------------------------------------

PSE_TEST("hooks: AllocCode is executable and aligned") {
    Fresh f;
    auto* a = static_cast<uint8_t*>(H().AllocCode(kAlpha, 16));
    auto* b = static_cast<uint8_t*>(H().AllocCode(kAlpha, 3));
    auto* big = static_cast<uint8_t*>(H().AllocCode(kAlpha, 0x30000));
    PSE_REQUIRE(a && b && big);
    PSE_CHECK(a != b);
    PSE_CHECK_EQ(reinterpret_cast<uintptr_t>(b) & 15, 0u);
    PSE_CHECK(H().AllocCode(0, 16) == nullptr);
    PSE_CHECK(H().AllocCode(kAlpha, 0) == nullptr);
    std::memcpy(a, "\xB8\x2A\x00\x00\x00\xC3", 6);  // mov eax,42; ret
    PSE_CHECK_EQ(reinterpret_cast<int(__cdecl*)()>(a)(), 42);
    big[0x2FFFF] = 1;  // whole block writable
}

// ---- call sites ---------------------------------------------------------------------------------------------------

PSE_TEST("hooks: call site hook and restore") {
    Fresh f;
    Buf b;
    PutPlus1(b, 0);
    PutCaller(b, 0x40, b.va(0));
    uint8_t before[5];
    std::memcpy(before, b.p + 0x44, 5);
    PSEHookId id = 0;
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x44), b.va(0), (void*)&HA, 0, N(g_nA), &id), PSE_OK);
    PSE_CHECK(id != 0);
    PSE_CHECK_EQ(Call(b.fn(0x40), 5), 1006);
    PSE_CHECK(g_nA == b.fn(0));
    PSE_CHECK_EQ(H().OwnerOf(b.va(0x44)), kAlpha);
    PSE_CHECK_EQ(H().OwnerOf(b.va(0x48)), kAlpha);
    PSE_CHECK_EQ(H().OwnerOf(b.va(0x49)), 0u);
    PSE_CHECK_EQ(H().Unhook(kAlpha, id), PSE_OK);
    PSE_CHECK(std::memcmp(before, b.p + 0x44, 5) == 0);
    PSE_CHECK_EQ(Call(b.fn(0x40), 5), 6);
    PSE_CHECK_EQ(H().OwnerOf(b.va(0x44)), 0u);
    PSE_CHECK_EQ(H().Unhook(kAlpha, id), PSE_E_NOT_FOUND);
}

PSE_TEST("hooks: call site chain order, priorities, unhook first/middle/last") {
    Fresh f;
    Buf b;
    PutPlus1(b, 0);
    PutCaller(b, 0x40, b.va(0));
    uint8_t before[5];
    std::memcpy(before, b.p + 0x44, 5);
    const uint32_t site = b.va(0x44), target = b.va(0);
    PSEHookId a, bb, c, d;
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, site, target, (void*)&HA, 0, N(g_nA), &a), PSE_OK);
    PSE_CHECK_EQ(H().HookCallSite(kBeta, site, target, (void*)&HB, 10, N(g_nB), &bb), PSE_OK);
    PSE_CHECK_EQ(H().HookCallSite(kGamma, site, target, (void*)&HC, 5, N(g_nC), &c), PSE_OK);
    PSE_CHECK_EQ(H().HookCallSite(kGamma, site, target, (void*)&HD, 0, N(g_nD), &d), PSE_OK);  // tie with A
    PSE_CHECK_EQ(Call(b.fn(0x40), 1), 11112);
    PSE_CHECK_STR(g_order.c_str(), "BCAD");
    PSE_CHECK(g_nD == b.fn(0));
    PSE_CHECK_EQ(H().OwnerOf(site), kBeta);  // the chain head

    PSE_CHECK_EQ(H().Unhook(kGamma, c), PSE_OK);  // middle
    PSE_CHECK_EQ(Call(b.fn(0x40), 1), 11102);
    PSE_CHECK_STR(g_order.c_str(), "BAD");
    PSE_CHECK_EQ(H().Unhook(kBeta, bb), PSE_OK);  // first
    PSE_CHECK_EQ(Call(b.fn(0x40), 1), 11002);
    PSE_CHECK_STR(g_order.c_str(), "AD");
    PSE_CHECK_EQ(H().OwnerOf(site), kAlpha);
    PSE_CHECK_EQ(H().Unhook(kGamma, d), PSE_OK);  // last
    PSE_CHECK_EQ(Call(b.fn(0x40), 1), 1002);
    PSE_CHECK_STR(g_order.c_str(), "A");
    PSE_CHECK(g_nA == b.fn(0));
    // Re-adding in the middle rewires both neighbours.
    PSE_CHECK_EQ(H().HookCallSite(kBeta, site, target, (void*)&HB, 7, N(g_nB), &bb), PSE_OK);
    PSE_CHECK_EQ(H().HookCallSite(kGamma, site, target, (void*)&HC, 3, N(g_nC), &c), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0x40), 1), 1112);
    PSE_CHECK_STR(g_order.c_str(), "BCA");
    PSE_CHECK_EQ(H().Unhook(kAlpha, a), PSE_OK);
    PSE_CHECK_EQ(H().Unhook(kBeta, bb), PSE_OK);
    PSE_CHECK_EQ(H().Unhook(kGamma, c), PSE_OK);
    PSE_CHECK(std::memcmp(before, b.p + 0x44, 5) == 0);
    PSE_CHECK_EQ(Call(b.fn(0x40), 1), 2);
}

PSE_TEST("hooks: call site expected target is the original one") {
    Fresh f;
    Buf b;
    PutPlus1(b, 0);
    PutPlus1(b, 0x20);
    PutCaller(b, 0x40, b.va(0));
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x44), b.va(0x20), (void*)&HA, 0, N(g_nA), nullptr),
                 PSE_E_MISMATCH);
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x40), b.va(0), (void*)&HA, 0, N(g_nA), nullptr),
                 PSE_E_MISMATCH);  // not a call
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x44), b.va(0), (void*)&HA, 0, N(g_nA), nullptr), PSE_OK);
    // The site now calls HA, but a second plugin still names the original target.
    PSE_CHECK_EQ(H().HookCallSite(kBeta, b.va(0x44), reinterpret_cast<uint32_t>(&HA), (void*)&HB, 0, N(g_nB),
                                  nullptr),
                 PSE_E_MISMATCH);
    PSE_CHECK_EQ(H().HookCallSite(kBeta, b.va(0x44), b.va(0), (void*)&HB, 0, N(g_nB), nullptr), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0x40), 1), 1102);
}

PSE_TEST("hooks: argument checks, next reuse, unhook ownership") {
    Fresh f;
    Buf b;
    PutPlus1(b, 0);
    PutCaller(b, 0x40, b.va(0));
    PSEHookId id;
    PSE_CHECK_EQ(H().HookCallSite(0, b.va(0x44), b.va(0), (void*)&HA, 0, N(g_nA), &id), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x44), b.va(0), nullptr, 0, N(g_nA), &id), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x44), b.va(0), (void*)&HA, 0, nullptr, &id), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x44), b.va(0), (void*)&HA, 0,
                                  reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(&g_nA) + 1), &id),
                 PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x44), b.va(0), (void*)&HA, 0, N(g_nA), &id), PSE_OK);
    PSE_CHECK_EQ(H().HookCallSite(kBeta, b.va(0x44), b.va(0), (void*)&HB, 0, N(g_nA), nullptr), PSE_E_EXISTS);
    PSE_CHECK_EQ(H().Unhook(kBeta, id), PSE_E_CONFLICT);
    PSE_CHECK(LogHas("unhook", "alpha"));
    PSE_CHECK_EQ(H().Unhook(kAlpha, 9999), PSE_E_NOT_FOUND);
    PSE_CHECK_EQ(H().Unhook(kAlpha, 0), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(H().Unhook(kAlpha, id), PSE_OK);
}

// ---- function detours ---------------------------------------------------------------------------------------------

PSE_TEST("hooks: function hook chain and restore") {
    Fresh f;
    Buf b;
    PutPlus1(b, 0);
    uint8_t before[8];
    std::memcpy(before, b.p, 8);
    const uint8_t prologue[] = {0x8B, 0x44, 0x24, 0x04, 0x40};
    PSEHookId a, c;
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0), 5, prologue, (void*)&HA, 0, N(g_nA), &a), PSE_OK);
    PSE_CHECK_EQ(b.p[0], 0xE9);
    PSE_CHECK_EQ(Call(b.fn(0), 4), 1005);
    PSE_CHECK_EQ(g_nA(4), 5);  // the trampoline alone
    // Later subscribers check against the original prologue, not the jump now at the entry.
    PSE_CHECK_EQ(H().HookFunction(kGamma, b.va(0), 5, prologue, (void*)&HC, 1, N(g_nC), &c), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0), 4), 1015);
    PSE_CHECK_STR(g_order.c_str(), "CA");
    PSE_CHECK_EQ(H().HookFunction(kBeta, b.va(0), 6, nullptr, (void*)&HB, 0, N(g_nB), nullptr), PSE_E_UNSUPPORTED);
    const uint8_t wrong[] = {0x8B, 0x44, 0x24, 0x08, 0x40};
    PSE_CHECK_EQ(H().HookFunction(kBeta, b.va(0), 5, wrong, (void*)&HB, 0, N(g_nB), nullptr), PSE_E_MISMATCH);
    PSE_CHECK_EQ(H().Unhook(kAlpha, a), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0), 4), 15);
    PSE_CHECK_EQ(H().Unhook(kGamma, c), PSE_OK);
    PSE_CHECK(std::memcmp(before, b.p, 8) == 0);
    PSE_CHECK_EQ(Call(b.fn(0), 4), 5);
}

PSE_TEST("hooks: function hook prologue variants") {
    Fresh f;
    Buf b;
    // push ebp; mov ebp,esp; sub esp,8; mov eax,[ebp+8]; add eax,100; mov esp,ebp; pop ebp; ret
    b.put(0x000, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0x8B, 0x45, 0x08, 0x05, 0x64, 0x00, 0x00, 0x00, 0x8B, 0xE5,
                  0x5D, 0xC3});
    // mov eax,[esp+4]; test eax,eax; je +4; add eax,5; ret; mov eax,999; ret   (je moved and widened)
    b.put(0x100, {0x8B, 0x44, 0x24, 0x04, 0x85, 0xC0, 0x74, 0x04, 0x83, 0xC0, 0x05, 0xC3, 0xB8, 0xE7, 0x03, 0x00,
                  0x00, 0xC3});
    // call helper (mov eax,7; ret); add eax,[esp+4]; ret   (rel32 call relocated)
    b.put(0x300, {0xB8, 0x07, 0x00, 0x00, 0x00, 0xC3});
    b.rel(0x200, 0xE8, b.va(0x300));
    b.put(0x205, {0x03, 0x44, 0x24, 0x04, 0xC3});
    // mov eax,[esp+4]; jmp +3; inc eax x3 (skipped); inc eax; ret   (short jmp widened)
    b.put(0x400, {0x8B, 0x44, 0x24, 0x04, 0xEB, 0x03, 0x40, 0x40, 0x40, 0x40, 0xC3});
    // mov eax,[esp+4]; jmp rel32 to the plus-one tail
    b.put(0x500, {0x8B, 0x44, 0x24, 0x04});
    b.rel(0x504, 0xE9, b.va(0x600));
    b.put(0x600, {0x40, 0xC3});

    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x000), 6, nullptr, (void*)&HA, 0, N(g_nA), nullptr), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0x000), 1), 1101);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x100), 8, nullptr, (void*)&HB, 0, N(g_nB), nullptr), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0x100), 0), 1099);
    PSE_CHECK_EQ(Call(b.fn(0x100), 3), 108);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x200), 5, nullptr, (void*)&HC, 0, N(g_nC), nullptr), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0x200), 2), 19);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x400), 6, nullptr, (void*)&HD, 0, N(g_nD), nullptr), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0x400), 2), 10003);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x500), 9, nullptr, (void*)&HE, 0, N(g_nE), nullptr), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0x500), 2), 10);
}

PSE_TEST("hooks: function hook refusals") {
    Fresh f;
    Buf b;
    // push ebp; mov ebp,esp; sub esp,8 ...
    b.put(0x000, {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08, 0xC3});
    // mov eax,[esp+4]; jecxz +0; ret
    b.put(0x100, {0x8B, 0x44, 0x24, 0x04, 0xE3, 0x00, 0xC3});
    // mov eax,[esp+4]; loop +0
    b.put(0x200, {0x8B, 0x44, 0x24, 0x04, 0xE2, 0x00, 0xC3});
    // test eax,eax; je +1 (into the moved range); inc eax; mov eax,[esp+4]
    b.put(0x300, {0x85, 0xC0, 0x74, 0x01, 0x40, 0x8B, 0x44, 0x24, 0x04, 0xC3});
    // undefined opcode 0F 04 in the prologue
    b.put(0x400, {0x55, 0x8B, 0xEC, 0x0F, 0x04, 0x90, 0xC3});
    // call rel16 (66 E8)
    b.put(0x500, {0x55, 0x66, 0xE8, 0x00, 0x00, 0x90, 0xC3});
    uint8_t before[0x600];
    std::memcpy(before, b.p, sizeof before);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0), 4, nullptr, (void*)&HA, 0, N(g_nA), nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0), 5, nullptr, (void*)&HA, 0, N(g_nA), nullptr), PSE_E_INVALID_ARG);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x100), 6, nullptr, (void*)&HA, 0, N(g_nA), nullptr),
                 PSE_E_UNSUPPORTED);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x200), 6, nullptr, (void*)&HA, 0, N(g_nA), nullptr),
                 PSE_E_UNSUPPORTED);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x300), 9, nullptr, (void*)&HA, 0, N(g_nA), nullptr),
                 PSE_E_UNSUPPORTED);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x400), 6, nullptr, (void*)&HA, 0, N(g_nA), nullptr),
                 PSE_E_UNSUPPORTED);
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0x500), 6, nullptr, (void*)&HA, 0, N(g_nA), nullptr),
                 PSE_E_UNSUPPORTED);
    const uint8_t wrong[] = {0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x10};
    PSE_CHECK_EQ(H().HookFunction(kAlpha, b.va(0), 6, wrong, (void*)&HA, 0, N(g_nA), nullptr), PSE_E_MISMATCH);
    PSE_CHECK(std::memcmp(before, b.p, sizeof before) == 0);  // nothing written
    PSE_CHECK_EQ(H().ListHooks(nullptr, 0), 0u);
}

// ---- byte groups --------------------------------------------------------------------------------------------------

PSE_TEST("hooks: byte groups apply, revert, idempotent, all-or-nothing") {
    Fresh f;
    Buf b;
    b.put(0x10, {1, 2, 3, 4});
    b.put(0x20, {5, 6});
    const uint8_t o1[] = {1, 2, 3, 4}, p1[] = {9, 9, 9, 9}, o2[] = {5, 6}, p2[] = {7, 7}, bad[] = {0, 0};
    const PSEBytePatch good[] = {{b.va(0x10), o1, p1, 4, 0, "one"}, {b.va(0x20), o2, p2, 2, 0, "two"}};
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "g", good, 2), PSE_OK);
    PSE_CHECK(std::memcmp(b.p + 0x10, p1, 4) == 0 && std::memcmp(b.p + 0x20, p2, 2) == 0);
    PSE_CHECK_EQ(H().OwnerOf(b.va(0x13)), kAlpha);
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "g", good, 2), PSE_OK);  // idempotent
    PSE_CHECK_EQ(H().ListHooks(nullptr, 0), 2u);
    PSE_CHECK_EQ(H().RevertGroup(kAlpha, "g"), PSE_OK);
    PSE_CHECK(std::memcmp(b.p + 0x10, o1, 4) == 0 && std::memcmp(b.p + 0x20, o2, 2) == 0);
    PSE_CHECK_EQ(H().OwnerOf(b.va(0x13)), 0u);
    PSE_CHECK_EQ(H().RevertGroup(kAlpha, "g"), PSE_E_NOT_FOUND);

    // Second site mismatching: the first one is not written either.
    const PSEBytePatch mixed[] = {{b.va(0x10), o1, p1, 4, 0, "one"}, {b.va(0x20), bad, p2, 2, 0, "two"}};
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "m", mixed, 2), PSE_E_MISMATCH);
    PSE_CHECK(std::memcmp(b.p + 0x10, o1, 4) == 0);
    PSE_CHECK(LogHas("unexpected bytes", "two"));
    // Overlapping patches inside one group.
    const PSEBytePatch overlap[] = {{b.va(0x10), o1, p1, 4, 0, "a"}, {b.va(0x12), o1, p1, 2, 0, "b"}};
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "o", overlap, 2), PSE_E_INVALID_ARG);

    // Already patched (e.g. on disk): fine, and revert writes the original.
    std::memcpy(b.p + 0x10, p1, 4);
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "g", good, 1), PSE_OK);
    PSE_CHECK_EQ(H().RevertGroup(kAlpha, "g"), PSE_OK);
    PSE_CHECK(std::memcmp(b.p + 0x10, o1, 4) == 0);

    // Patched bytes changed behind the broker's back: revert refuses.
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "g", good, 1), PSE_OK);
    b.p[0x10] = 0x55;
    PSE_CHECK_EQ(H().RevertGroup(kAlpha, "g"), PSE_E_MISMATCH);
    b.p[0x10] = 9;
    PSE_CHECK_EQ(H().RevertGroup(kAlpha, "g"), PSE_OK);
}

PSE_TEST("hooks: anyOriginal sites and re-applied tunable values") {
    Fresh f;
    Buf b;
    b.put(0x10, {0x12, 0x34, 0x56, 0x78});
    const uint8_t v1[] = {1, 0, 0, 0}, v2[] = {2, 0, 0, 0};
    PSEBytePatch tune{b.va(0x10), nullptr, v1, 4, 1, "scale"};
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "scale", &tune, 1), PSE_OK);
    PSE_CHECK(std::memcmp(b.p + 0x10, v1, 4) == 0);
    tune.patched = v2;  // new setting: same group, new value
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "scale", &tune, 1), PSE_OK);
    PSE_CHECK(std::memcmp(b.p + 0x10, v2, 4) == 0);
    PSE_CHECK_EQ(H().ListHooks(nullptr, 0), 1u);
    PSE_CHECK_EQ(H().RevertGroup(kAlpha, "scale"), PSE_OK);
    const uint8_t was[] = {0x12, 0x34, 0x56, 0x78};
    PSE_CHECK(std::memcmp(b.p + 0x10, was, 4) == 0);  // what was there before the first apply
}

// ---- conflicts ----------------------------------------------------------------------------------------------------

PSE_TEST("hooks: conflicts between plugins, bytes and hook sites") {
    Fresh f;
    Buf b;
    PutPlus1(b, 0);
    PutCaller(b, 0x40, b.va(0));
    b.put(0x80, {1, 2, 3, 4, 5, 6});
    const uint8_t o[] = {1, 2, 3, 4, 5, 6}, p[] = {9, 9, 9, 9, 9, 9};
    const PSEBytePatch a{b.va(0x80), o, p, 4, 0, "a"};
    const PSEBytePatch over{b.va(0x82), o + 2, p, 4, 0, "b"};
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "first", &a, 1), PSE_OK);
    PSE_CHECK_EQ(H().PatchGroup(kBeta, "second", &over, 1), PSE_E_CONFLICT);
    PSE_CHECK_EQ(b.p[0x84], 5);  // nothing written
    PSE_CHECK(LogHas("beta", "owned by alpha"));
    PSE_CHECK(LogHas("first"));
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "other", &over, 1), PSE_E_CONFLICT);  // own other group too
    PSE_CHECK_EQ(H().RevertGroup(kBeta, "first"), PSE_E_NOT_FOUND);         // revert key is per plugin

    // Bytes over a hook site, and a hook over patched bytes: refused, even for the same plugin.
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x44), b.va(0), (void*)&HA, 0, N(g_nA), nullptr), PSE_OK);
    const uint8_t callBytes[2] = {b.p[0x47], b.p[0x48]}, nops[2] = {0x90, 0x90};
    const PSEBytePatch onSite{b.va(0x47), callBytes, nops, 2, 0, "nop"};
    PSE_CHECK_EQ(H().PatchGroup(kAlpha, "nop", &onSite, 1), PSE_E_CONFLICT);
    PSE_CHECK_EQ(H().PatchGroup(kBeta, "nop", &onSite, 1), PSE_E_CONFLICT);
    PSE_CHECK(LogHas("owned by alpha", "call hook"));
    PSE_CHECK_EQ(H().HookPointer(kBeta, b.va(0x80), (void*)&HB, 0, N(g_nB), nullptr), PSE_E_CONFLICT);
    // A different kind of hook on the call site's bytes.
    PSE_CHECK_EQ(H().HookFunction(kBeta, b.va(0x40), 9, nullptr, (void*)&HB, 0, N(g_nB), nullptr), PSE_E_CONFLICT);
    PSE_CHECK_EQ(Call(b.fn(0x40), 1), 1002);

    PSE_CHECK_EQ(H().RevertGroup(kAlpha, "first"), PSE_OK);
    PSE_CHECK_EQ(H().PatchGroup(kBeta, "second", &over, 1), PSE_OK);  // free now
    PSE_CHECK_EQ(H().OwnerOf(b.va(0x85)), kBeta);
}

// ---- indirect calls + pointers ------------------------------------------------------------------------------------

namespace {
Fn g_slot;
int __cdecl Times2(int x) { return x * 2; }
int __cdecl Times3(int x) { return x * 3; }
}  // namespace

PSE_TEST("hooks: indirect call follows the IAT slot") {
    Fresh f;
    Buf b;
    g_slot = &Times2;
    const uint32_t slot = reinterpret_cast<uint32_t>(&g_slot);
    // push [esp+4]; call [slot]; add esp,4; ret
    b.put(0, {0xFF, 0x74, 0x24, 0x04, 0xFF, 0x15});
    std::memcpy(b.p + 6, &slot, 4);
    b.put(10, {0x83, 0xC4, 0x04, 0xC3});
    uint8_t before[6];
    std::memcpy(before, b.p + 4, 6);
    PSE_CHECK_EQ(H().HookIndirectCall(kAlpha, b.va(4), slot + 4, (void*)&HA, 0, N(g_nA), nullptr), PSE_E_MISMATCH);
    PSEHookId a, c;
    PSE_CHECK_EQ(H().HookIndirectCall(kAlpha, b.va(4), slot, (void*)&HA, 0, N(g_nA), &a), PSE_OK);
    PSE_CHECK_EQ(b.p[4], 0xE8);
    PSE_CHECK_EQ(b.p[9], 0x90);
    PSE_CHECK_EQ(H().HookIndirectCall(kBeta, b.va(4), slot, (void*)&HC, 5, N(g_nC), &c), PSE_OK);
    PSE_CHECK_EQ(Call(b.fn(0), 7), 1024);
    PSE_CHECK_STR(g_order.c_str(), "CA");
    g_slot = &Times3;  // someone changes the IAT slot: the chain's end follows it live
    PSE_CHECK_EQ(Call(b.fn(0), 7), 1031);
    PSE_CHECK_EQ(H().Unhook(kAlpha, a), PSE_OK);
    PSE_CHECK_EQ(H().Unhook(kBeta, c), PSE_OK);
    PSE_CHECK(std::memcmp(before, b.p + 4, 6) == 0);
    PSE_CHECK_EQ(Call(b.fn(0), 7), 21);
}

namespace {
struct FakeObj;
using Method = int(__fastcall*)(FakeObj*, void*, int);
struct FakeObj {
    Method* vtable;
    int base;
};
int __fastcall MethodAdd(FakeObj* self, void*, int x) { return self->base + x; }
int __fastcall MethodOther(FakeObj*, void*, int) { return -1; }
Method g_mA, g_mB;
int __fastcall HookMA(FakeObj* self, void* edx, int x) { g_order += 'a'; return g_mA(self, edx, x) * 10; }
int __fastcall HookMB(FakeObj* self, void* edx, int x) { g_order += 'b'; return g_mB(self, edx, x) + 1; }
}  // namespace

PSE_TEST("hooks: pointer hook on a fake vtable") {
    Fresh f;
    static Method vtable[2] = {&MethodOther, &MethodAdd};
    FakeObj obj{vtable, 40};
    auto call = [&] {
        g_order.clear();
        return obj.vtable[1](&obj, nullptr, 2);
    };
    const uint32_t slot = reinterpret_cast<uint32_t>(&vtable[1]);
    PSEHookId a, bb;
    PSE_CHECK_EQ(H().HookPointer(kAlpha, slot, (void*)&HookMA, 0, reinterpret_cast<void**>(&g_mA), &a), PSE_OK);
    PSE_CHECK(g_mA == &MethodAdd);
    PSE_CHECK_EQ(call(), 420);
    PSE_CHECK_EQ(H().HookPointer(kBeta, slot, (void*)&HookMB, -5, reinterpret_cast<void**>(&g_mB), &bb), PSE_OK);
    PSE_CHECK_EQ(call(), 430);
    PSE_CHECK_STR(g_order.c_str(), "ab");
    PSE_CHECK(vtable[0] == &MethodOther);  // neighbour slot untouched
    PSE_CHECK_EQ(H().OwnerOf(slot + 3), kAlpha);
    PSE_CHECK_EQ(H().OwnerOf(slot + 4), 0u);
    PSE_CHECK_EQ(H().Unhook(kAlpha, a), PSE_OK);
    PSE_CHECK(vtable[1] == reinterpret_cast<Method>(&HookMB));
    PSE_CHECK_EQ(call(), 43);
    PSE_CHECK_EQ(H().Unhook(kBeta, bb), PSE_OK);
    PSE_CHECK(vtable[1] == &MethodAdd);
}

// ---- introspection ------------------------------------------------------------------------------------------------

PSE_TEST("hooks: ListHooks and default names") {
    Fresh f;
    Buf b;
    PutPlus1(b, 0);
    PutCaller(b, 0x40, b.va(0));
    b.put(0x80, {1, 2});
    const uint8_t o[] = {1, 2}, p[] = {3, 4};
    const PSEBytePatch bp{b.va(0x80), o, p, 2, 0, "x"};
    PSEHookId ha, hb;
    PSE_CHECK_EQ(H().PatchGroup(kGamma, "grp", &bp, 1), PSE_OK);
    PSE_CHECK_EQ(H().HookCallSite(kAlpha, b.va(0x44), b.va(0), (void*)&HA, 1, N(g_nA), &ha), PSE_OK);
    PSE_CHECK_EQ(H().HookCallSite(kBeta, b.va(0x44), b.va(0), (void*)&HB, 9, N(g_nB), &hb), PSE_OK);
    PSEHookInfo info[4] = {};
    PSE_CHECK_EQ(H().ListHooks(info, 1), 3u);  // total even when truncated
    PSE_CHECK_EQ(H().ListHooks(info, 4), 3u);
    PSE_CHECK_EQ(info[0].kind, PSE_HOOK_BYTES);
    PSE_CHECK_EQ(info[0].owner, kGamma);
    PSE_CHECK_STR(info[0].ownerName, "gamma");
    PSE_CHECK_STR(info[0].name, "grp");
    PSE_CHECK_EQ(info[0].va, b.va(0x80));
    PSE_CHECK_EQ(info[0].len, 2u);
    PSE_CHECK_EQ(info[0].id, 0u);
    PSE_CHECK_EQ(info[1].kind, PSE_HOOK_CALL_SITE);
    PSE_CHECK_EQ(info[1].id, hb);
    PSE_CHECK_EQ(info[1].chainPosition, 0u);
    PSE_CHECK_EQ(info[1].priority, 9);
    PSE_CHECK_STR(info[1].ownerName, "beta");
    PSE_CHECK_EQ(info[2].id, ha);
    PSE_CHECK_EQ(info[2].chainPosition, 1u);
    PSE_CHECK_EQ(info[2].len, 5u);
    PSE_CHECK_EQ(info[2].size, static_cast<uint32_t>(sizeof(PSEHookInfo)));

    pse::hooks::SetEnvironment(nullptr, &TestLog);  // no name callback: "plugin#<n>"
    PSE_CHECK_EQ(H().ListHooks(info, 4), 3u);
    PSE_CHECK_STR(info[1].ownerName, "plugin#2");
}

// Dev builds only: call probe. HKCU\Software\VietnamFix\Dev\Probe = "4d7c4a:6,4d75ba:9" (function address : length of
// its first whole instructions, position-independent - they are copied into a trampoline): every distinct caller of
// each function is logged once with ecx (this) and the first 5 stack arguments (hex and as float). How the HUD layout
// was mapped (which code draws which element where).
#ifndef DS_DIST
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
struct Probe {
    uint32_t fn;
    uint32_t callers[128];
    int count;
};
Probe g_probes[8];
int g_probeCount = 0;

// ctx = esp after pushad: edi esi ebp esp ebx edx ecx eax, then the return address and the arguments
void __stdcall Log(int id, const uint32_t* ctx) {
    Probe& p = g_probes[id];
    const uint32_t ret = ctx[8];
    for (int i = 0; i < p.count; ++i)
        if (p.callers[i] == ret) return;
    if (p.count >= 128) return;
    p.callers[p.count++] = ret;
    const uint32_t* a = ctx + 9;
    float f[5];
    std::memcpy(f, a, sizeof f);
    // the first two arguments as pointers to 4 ints (rectangles, positions), when readable
    int m[2][4] = {};
    for (int i = 0; i < 2; ++i)
        if (a[i] >= 0x10000 && !IsBadReadPtr(reinterpret_cast<const void*>(a[i]), 16))
            std::memcpy(m[i], reinterpret_cast<const void*>(a[i]), 16);
    dslog::Write("[dev] probe %06X from %08X ecx %08X args %08X %08X %08X %08X %08X (f %.2f %.2f %.2f %.2f %.2f) "
                 "*a0 %d %d %d %d *a1 %d %d %d %d", p.fn, ret - 5, ctx[6], a[0], a[1], a[2], a[3], a[4], f[0], f[1], f[2],
                 f[3], f[4], m[0][0], m[0][1], m[0][2], m[0][3], m[1][0], m[1][1], m[1][2], m[1][3]);
}

bool Install(uint32_t fn, int len) {
    if (g_probeCount >= 8 || len < 5 || len > 16) return false;
    const int id = g_probeCount;
    auto* stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return false;
    uint8_t* s = stub;
    *s++ = 0x60;                                   // pushad
    *s++ = 0x54;                                   // push esp
    *s++ = 0x68;                                   // push id
    std::memcpy(s, &id, 4), s += 4;
    *s++ = 0xE8;                                   // call Log
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&Log) - (reinterpret_cast<uint32_t>(s) + 4));
    std::memcpy(s, &rel, 4), s += 4;
    *s++ = 0x61;                                   // popad
    std::memcpy(s, reinterpret_cast<const void*>(fn), len), s += len;   // the function's first instructions
    *s++ = 0xE9;                                   // jmp back
    rel = static_cast<int32_t>(fn + len - (reinterpret_cast<uint32_t>(s) + 4));
    std::memcpy(s, &rel, 4);
    g_probes[id] = {fn, {}, 0};
    ++g_probeCount;
    if (!patch::WriteJump(fn, stub)) return false;
    for (int i = 5; i < len; ++i) patch::Write(fn + i, "\x90", 1);
    return true;
}
}  // namespace

void features::ApplyProbes() {
    char list[256] = {};
    DWORD size = sizeof list;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\VietnamFix\\Dev", "Probe", RRF_RT_REG_SZ, nullptr, list, &size) !=
        ERROR_SUCCESS)
        return;
    for (char* p = list; *p;) {
        char* end;
        const unsigned long fn = std::strtoul(p, &end, 16);
        if (end == p || *end != ':') break;
        const long len = std::strtol(end + 1, &end, 10);
        dslog::Write("[dev] probe %06lX (%ld bytes): %s", fn, len, Install(fn, static_cast<int>(len)) ? "on" : "FAILED");
        p = *end ? end + 1 : end;
    }
}
#else
#include "features/features.h"
void features::ApplyProbes() {}
#endif

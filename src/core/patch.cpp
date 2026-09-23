#include "core/patch.h"

#include <windows.h>

#include <cstring>

#include "core/log.h"

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
}  // namespace

bool patch::Matches(uint32_t va, const void* bytes, size_t len) {
    return Readable(va, len) && std::memcmp(reinterpret_cast<void*>(va), bytes, len) == 0;
}

bool patch::Write(uint32_t va, const void* bytes, size_t len) {
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

bool patch::ApplyGroup(const char* group, const BytePatch* patches, size_t count) {
    unsigned already = 0;
    for (size_t i = 0; i < count; ++i) {
        const BytePatch& p = patches[i];
        if (Matches(p.va, p.patched, p.len)) {
            ++already;
            continue;
        }
        if (!p.anyOriginal && !Matches(p.va, p.original, p.len)) {
            dslog::Write("[fail] %s: unexpected bytes at 0x%08X (%s) - different exe version? Group not applied.",
                        group, p.va, p.name);
            return false;
        }
    }
    for (size_t i = 0; i < count; ++i) {
        if (!Write(patches[i].va, patches[i].patched, patches[i].len)) return false;
    }
    dslog::Write("[ok]   %s: %u patches (%u already in the exe file)", group, static_cast<unsigned>(count), already);
    return true;
}

bool patch::RevertGroup(const char* group, const BytePatch* patches, size_t count) {
    unsigned already = 0;
    for (size_t i = 0; i < count; ++i) {
        const BytePatch& p = patches[i];
        if (Matches(p.va, p.original, p.len)) {
            ++already;
            continue;
        }
        if (!p.anyOriginal && !Matches(p.va, p.patched, p.len)) {
            dslog::Write("[fail] %s: unexpected bytes at 0x%08X (%s) - not reverted.", group, p.va, p.name);
            return false;
        }
    }
    if (already == count) return true;
    for (size_t i = 0; i < count; ++i) {
        if (!Write(patches[i].va, patches[i].original, patches[i].len)) return false;
    }
    dslog::Write("[off]  %s: reverted", group);
    return true;
}

bool patch::HookCall(uint32_t site, const void* fn, uint32_t expectedTarget) {
    uint8_t code[5];
    EncodeRel(code, 0xE8, site, expectedTarget);
    if (!Matches(site, code, 5)) {
        dslog::Write("[fail] call hook at 0x%08X: does not call 0x%08X", site, expectedTarget);
        return false;
    }
    EncodeRel(code, 0xE8, site, reinterpret_cast<uint32_t>(fn));
    return Write(site, code, 5);
}

bool patch::WriteJump(uint32_t site, const void* fn) {
    uint8_t code[5];
    EncodeRel(code, 0xE9, site, reinterpret_cast<uint32_t>(fn));
    return Write(site, code, 5);
}

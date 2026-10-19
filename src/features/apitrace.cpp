// Dev: script API call trace (Dev\ApiTrace = n, dev builds only) - the parity check between a mission DLL and its
// translation (docs/sdk/mission-translation.md). Every entry of the script API table (0x5ED238, 566 cdecl functions,
// 0x8D8 bytes) gets an outermost broker pointer hook (priority 1000): a 10-byte stub `push slot; jmp Common` logs the
// call and jumps on to whatever the slot held. Both paths go through the table - the DLL's `call [api + slot]` and the
// translated mission's X86RT.call on the value it read from the table - so both traces see the same calls.
//
// File DesertStormFix-apitrace-<n>.txt next to the exe, one line per call:
//   <seq> <frame> <M|H> <slot> <a0> .. <a7>
// frame = the gameplay timer's frame count ([[0x63C958]+0xC8], restarted by the determinism test), M = called by the
// mission (the DLL module, or the translated mission's native call), H = anyone else (script host, plugins). Arguments:
// 8 dwords from the caller's stack (the comparer cuts them to the slot's real count), normalised so both paths compare:
//   D+rva  a pointer into the mission (DLL module or the translated image) - "text" appended for a string
//   S      a pointer into the caller's stack (thread stack or the emulated stack)
//   P      a pointer into other private memory (heaps: the address depends on the run's allocation order)
//   hex    anything else.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"

#ifndef DS_DIST
namespace {
constexpr uint32_t kApiTable = 0x5ED238, kApiSize = 0x8D8, kSlots = kApiSize / 4;
constexpr uint32_t kTimerB = 0x63C958;

FILE* g_out = nullptr;
uint32_t g_seq = 0, g_max = 0;
uint32_t g_next[kSlots] = {};      // broker next per slot
uint8_t* g_stubs = nullptr;        // 10 bytes per slot
DWORD g_gameThread = 0;
// the mission DLL module (cached by address)
uint32_t g_modLo = 0, g_modHi = 0;

DWORD DevDword(const char* name, DWORD fallback) {
    DWORD v, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v,
                        &size) == ERROR_SUCCESS ? v : fallback;
}

bool InMissionModule(uint32_t a) {
    if (a >= g_modLo && a < g_modHi) return true;
    if (a < 0x10000) return false;
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<const char*>(a), &m) || !m)
        return false;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
    const char* name = strrchr(path, '\\');
    name = name ? name + 1 : path;
    if (_strnicmp(name, "Mission", 7) != 0 && _strnicmp(name, "Training", 8) != 0 && _strnicmp(name, "MP_", 3) != 0 &&
        _strnicmp(name, "Outro", 5) != 0 && _strnicmp(name, "FrontEnd", 8) != 0)
        return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(reinterpret_cast<const uint8_t*>(m) + dos->e_lfanew);
    g_modLo = reinterpret_cast<uint32_t>(m), g_modHi = g_modLo + nt->OptionalHeader.SizeOfImage;
    return true;
}

bool Readable(uint32_t a, void* out, size_t n) {
    __try {
        memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Arg(char* buf, size_t len, uint32_t v, uint32_t stackLo, uint32_t stackHi) {
    uint32_t lo = 0, hi = 0, orig = 0, slo = 0, shi = 0;
    uint32_t rva = 0;
    bool mission = false;
    // the level's script module ([0x60F57C], FUN_00449260): the cached range is the FIRST module seen - after a level
    // change (FrontEnd.dll -> Mission1.dll at the same base, larger image) it was stale and the new mission's pointers
    // past the old size showed as plain hex (missionsource test, 2026-10-19)
    if (const uint32_t cur = *reinterpret_cast<const uint32_t*>(0x60F57C); cur) {   // (same base, other size: re-read)
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(cur);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(cur + dos->e_lfanew);
        g_modLo = cur, g_modHi = cur + nt->OptionalHeader.SizeOfImage;
    }
    if (features::MissionRtRanges(&lo, &hi, &orig, &slo, &shi) && v >= lo && v < hi) mission = true, rva = v - lo;
    else if (g_modHi && v >= g_modLo && v < g_modHi) mission = true, rva = v - g_modLo;
    if (mission) {
        char s[48] = {};
        if (Readable(v, s, sizeof s - 1)) {
            size_t n = 0;
            while (n < sizeof s - 1 && s[n] >= 32 && s[n] < 127) ++n;
            if (n >= 2 && (n == sizeof s - 1 || s[n] == 0)) {
                s[n] = 0;
                snprintf(buf, len, "D+%X\"%s\"", rva, s);
                return;
            }
        }
        snprintf(buf, len, "D+%X", rva);
        return;
    }
    if ((v >= stackLo && v < stackHi) || (slo && v >= slo && v < shi)) {
        snprintf(buf, len, "S");
        return;
    }
    // a pointer into heap / private memory: its value depends on the allocation order of the run, not on the mission
    MEMORY_BASIC_INFORMATION mi;
    if (v >= 0x10000 && VirtualQuery(reinterpret_cast<const void*>(v), &mi, sizeof mi) && mi.State == MEM_COMMIT &&
        mi.Type == MEM_PRIVATE && !(mi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        snprintf(buf, len, "P");
        return;
    }
    snprintf(buf, len, "%X", v);
}

void __stdcall Log(uint32_t slot, uint32_t ret, const uint32_t* args) {
    if (!g_out || (g_max && g_seq >= g_max)) return;
    if (GetCurrentThreadId() != g_gameThread) return;
    const uint32_t stub = reinterpret_cast<uint32_t>(g_stubs) + slot / 4 * 10;
    const bool mission = InMissionModule(ret) || features::MissionRtCallTarget() == stub;
    uint32_t frame = 0;
    if (const uint8_t* t = *reinterpret_cast<uint8_t* const*>(kTimerB)) Readable(reinterpret_cast<uint32_t>(t) + 0xC8, &frame, 4);
    NT_TIB* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    const uint32_t stackLo = reinterpret_cast<uint32_t>(tib->StackLimit), stackHi = reinterpret_cast<uint32_t>(tib->StackBase);
    uint32_t a[8] = {};
    Readable(reinterpret_cast<uint32_t>(args), a, sizeof a);
    char line[1024];
    int n = snprintf(line, sizeof line, "%u %u %c %03X", g_seq++, frame, mission ? 'M' : 'H', slot);
    for (int k = 0; k < 8 && n > 0 && n < static_cast<int>(sizeof line) - 80; ++k) {
        char arg[96];
        Arg(arg, sizeof arg, a[k], stackLo, stackHi);
        n += snprintf(line + n, sizeof line - n, " %s", arg);
    }
    fprintf(g_out, "%s\n", line);
    fflush(g_out);   // the tests kill the game: nothing may stay in the buffer
}

// [esp] = slot, [esp+4] = return address, [esp+8] = arguments
__declspec(naked) void Common() {
    __asm {
        pushad
        pushfd
        lea eax, [esp + 44]
        push eax
        push dword ptr [esp + 44]
        push dword ptr [esp + 44]
        call Log
        mov eax, [esp + 36]
        mov eax, dword ptr [g_next + eax]
        mov [esp + 36], eax
        popfd
        popad
        ret
    }
}
}  // namespace

void features::ApplyApiTrace() {
    const DWORD run = DevDword("ApiTrace", 0);
    if (!run) return;
    g_max = DevDword("ApiTraceMax", 300000);
    g_gameThread = GetCurrentThreadId();
    g_stubs = static_cast<uint8_t*>(VirtualAlloc(nullptr, kSlots * 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_stubs) return;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    char* slash = strrchr(path, '\\');
    snprintf(slash ? slash + 1 : path, MAX_PATH - (slash ? slash + 1 - path : 0), "DesertStormFix-apitrace-%lu.txt", run);
    g_out = fopen(path, "w");
    if (!g_out) return;
    setvbuf(g_out, nullptr, _IOFBF, 1 << 20);
    int hooked = 0;
    for (uint32_t k = 1; k < kSlots; ++k) {   // slot 0 = the version check; hooked too (LevelSetup's first call)
        uint8_t* s = g_stubs + k * 10;
        s[0] = 0x68;
        *reinterpret_cast<uint32_t*>(s + 1) = k * 4;
        s[5] = 0xE9;
        *reinterpret_cast<int32_t*>(s + 6) = static_cast<int32_t>(reinterpret_cast<uint32_t>(&Common) - reinterpret_cast<uint32_t>(s + 10));
    }
    {
        uint8_t* s = g_stubs;
        s[0] = 0x68;
        *reinterpret_cast<uint32_t*>(s + 1) = 0;
        s[5] = 0xE9;
        *reinterpret_cast<int32_t*>(s + 6) = static_cast<int32_t>(reinterpret_cast<uint32_t>(&Common) - reinterpret_cast<uint32_t>(s + 10));
    }
    FlushInstructionCache(GetCurrentProcess(), g_stubs, kSlots * 10);
    for (uint32_t k = 0; k < kSlots; ++k)
        if (patch::HookSlot(kApiTable + k * 4, g_stubs + k * 10, reinterpret_cast<void**>(&g_next[k]), 0, 1000)) ++hooked;
    dslog::Write("[dev]  API trace: %d of %u script API slots logged -> %s", hooked, kSlots, path);
}
#else
void features::ApplyApiTrace() {}
#endif

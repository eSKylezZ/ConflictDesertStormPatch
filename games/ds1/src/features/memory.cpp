// Memory: the game's "Global Heap" sized to what the mods need, plus the figures the performance overlay shows
// (address space, textures, heap) and the 4 GB (Large Address Aware) state the main menu reports.
//
// Global Heap (GAMELIB\Core\MemBuffers.cpp): a stack-style arena created once in WinMain's set-up -
// `push "Global Heap"; push 1024000; mov ecx, eax; call 0x4C01D0` at 0x40FFCB..0x40FFD7 -> [0x63C8B4].
// Fields: +0x20 mark slots (u32 each), +0x28 size, +0x2C free, +0x30 used, +0x34 buffer (size + 4 bytes: an "LBUF"
// guard follows the data), +0x38 1 = buffer not owned. Ctors: 0x4C01D0 thiscall(size, name) ret 8 allocates through the
// game allocator 0x4B8450 (which retries for ever when memory runs out - a hang, not an error); 0x4C0340 thiscall(buffer,
// size, name) ret 0xC takes the caller's buffer. Alloc 0x4C0420 thiscall(size, align) ret 8 / 0x4C0460 thiscall(size)
// ret 4 (16-byte aligned) return 0 when it doesn't fit; mark 0x4C04D0(slot), release 0x4C04F0(slot), pop 0x4C04A0(n).
// The whole-file reader FUN_004BFB90(name, &buf, &size, heap, flag) reads into the heap (heap 0 = the game allocator)
// and shows "Out of memory" when the alloc fails; the texture loaders (0x548F20, 0x549497) read every texture file
// whole into the Global Heap - the 1 MB cap on a texture file came from here.
//
// Texture files the heap can't hold (a mod enabled later through the MODS menu, or one past the cap) are read through
// the game allocator instead (FUN_004BFB90 with heap 0) - only for the texture loaders' reads (0x548F20, 0x549497,
// 0x549537, 0x549857), each between its own mark and release. A 16-byte marker is taken from the heap after the read;
// the buffer is freed (0x4B8590) once the heap is released / popped below that marker (0x4C04F0 / 0x4C04A0 hooked) -
// releases nested inside the loader only go back to marks taken after it.
//
// Size: the original 1,024,000 bytes, or that plus the largest file a mod adds (rounded up to 1 MB), capped at 64 MB
// (2 GB address space) / 256 MB (Large Address Aware) and a quarter of the largest free block. The buffer is ours
// (VirtualAlloc); if it can't be had the game's own heap is made as before. limits.cpp's enlarged pools (projectiles,
// particles, event queue, render arena) add 2 MB to the original size first.
#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <intrin.h>

#include "core/log.h"
#include "core/comhooks.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "limits/dsfix_limits.h"
#include "loader.h"

namespace {
constexpr uint32_t kHeapPushName = 0x40FFCB;  // push "Global Heap"; push 1024000; mov ecx, eax
constexpr uint32_t kHeapCreateCall = 0x40FFD7, kHeapCtor = 0x4C01D0, kHeapCtorBuffer = 0x4C0340;

constexpr uint32_t kHeapAlloc = 0x4C0420, kHeapAllocAligned = 0x4C0460;
constexpr uint32_t kReadFile = 0x4BFB90;
constexpr uint32_t kHeapRelease = 0x4C04F0, kHeapPop = 0x4C04A0, kGameFree = 0x4B8590;
constexpr uint32_t kTexReadRets[] = {0x548F25, 0x54949C, 0x54953C, 0x54985C};  // after the texture loaders' reads
const uint8_t kReleaseEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x54, 0x81, 0x20};  // mov eax,[esp+4]; mov edx,[ecx+eax*4+0x20]
const uint8_t kPopEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x51, 0x30};             // mov eax,[esp+4]; mov edx,[ecx+0x30]
constexpr uint32_t kRenderer = 0x63C924, kDeviceOffset = 0x40530;
constexpr uint32_t kMB = 1024 * 1024;

const uint8_t kHeapPushes[] = {0x68, 0x9C, 0x7C, 0x5E, 0x00, 0x68, 0x00, 0xA0, 0x0F, 0x00, 0x8B, 0xC8};
const uint8_t kAllocEntry[] = {0x8B, 0x41, 0x34, 0x53, 0x56};        // mov eax, [ecx+0x34]; push ebx; push esi
const uint8_t kReadEntry[] = {0x53, 0x55, 0x8B, 0x6C, 0x24, 0x0C};   // push ebx; push ebp; mov ebp, [esp+0xc]

using CtorFn = void*(__thiscall*)(void*, uint32_t, const char*);
using CtorBufferFn = void*(__thiscall*)(void*, void*, uint32_t, const char*);
using AllocFn = void*(__thiscall*)(void*, uint32_t, uint32_t);
using AllocAlignedFn = void*(__thiscall*)(void*, uint32_t);
using ReadFn = int(__cdecl*)(const char*, void**, uint32_t*, void*, int);

AllocFn g_alloc = nullptr;
AllocAlignedFn g_allocAligned = nullptr;
ReadFn g_read = nullptr;

uint8_t* g_heap = nullptr;   // the Global Heap object
bool g_heapOurs = false;
uint32_t g_heapPeak = 0, g_heapFails = 0;
const char* g_reading = nullptr;  // the file FUN_004BFB90 is reading (game thread)
void* g_bigBuffer = nullptr;      // a texture file read outside the heap, freed at the loader's release
uint32_t g_bigLevel = 0;          // heap "used" just after its marker: a release below it is the loader's
using ReleaseFn = void(__thiscall*)(void*, uint32_t);
ReleaseFn g_release = nullptr, g_pop = nullptr;
uint32_t g_bigReads = 0;
bool g_fallbackOk = false;      // the loaders' release calls are hooked (else a buffer would leak)

DWORD DevDword(const char* name) {
#ifdef DS_DIST
    (void)name;
    return 0;
#else
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
#endif
}

// Entry hook through the broker; *next = its trampoline (the moved prologue) or the next subscriber.
template <class Fn>
bool HookEntry(uint32_t entry, const void* fn, Fn& next, const uint8_t* prologue, size_t len) {
    return patch::HookFunction(entry, fn, reinterpret_cast<void**>(&next), prologue, len);
}

uint32_t& Field(uint8_t* heap, uint32_t off) { return *reinterpret_cast<uint32_t*>(heap + off); }

// ---- address space ----
uint32_t AddressLimit() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress));
}

void ScanAddressSpace(uint64_t& free, uint64_t& largest) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    free = largest = 0;
    auto a = reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress);
    const auto top = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress);
    MEMORY_BASIC_INFORMATION mbi;
    while (a < top && VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof mbi)) {
        if (mbi.State == MEM_FREE) {
            free += mbi.RegionSize;
            largest = std::max<uint64_t>(largest, mbi.RegionSize);
        }
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= a) break;
        a = next;
    }
}

// ---- heap size ----
// The largest file in ANY mod folder, switched on or off (2026-10-22: mods are switched on in game - MODS menu, an
// online host's mods - and the heap can't grow once made). Files the heap never holds (DLLs, Lua, audio streams ...)
// left out, as the mod index does.
uint32_t LargestFileInModsFolder(std::string& name) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string root = exe;
    root = root.substr(0, root.find_last_of("\\/")) + "\\Mods";
    uint32_t largest = 0;
    std::vector<std::string> dirs = {root};
    while (!dirs.empty()) {
        const std::string dir = dirs.back();
        dirs.pop_back();
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            const std::string n = fd.cFileName;
            if (n == "." || n == "..") continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                dirs.push_back(dir + "\\" + n);
                continue;
            }
            std::string ext = n.substr(n.find_last_of('.') + 1);
            for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            if (ext == "dll" || ext == "exe" || ext == "lua" || ext == "wav" || ext == "psf" || ext == "mp3" || ext == "ogg" ||
                ext == "sch" || fd.nFileSizeHigh)
                continue;
            if (fd.nFileSizeLow > largest) largest = fd.nFileSizeLow, name = n;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return largest;
}

uint32_t WantedHeap(uint32_t original) {
    if (const DWORD mb = DevDword("HeapMB")) return std::max<uint32_t>(original, mb * kMB);  // dev: forced size
    std::string name;
    uint32_t largest = features::LargestModFileSize(name);   // (the plugin dsfix.mods; 0 without it)
    if (features::ModsFolderEnabled()) {
        std::string any;
        const uint32_t all = LargestFileInModsFolder(any);
        if (all > largest) largest = all, name = any;
        // room for a mod switched on later or downloaded from an online host (one texture up to ~24 MB)
        largest = std::max<uint32_t>(largest, 24 * kMB);
    }
    if (largest + largest / 4 <= original / 2) return original;   // fits next to the game's own use
    uint32_t want = original + (largest + kMB - 1) / kMB * kMB;
    const bool laa = features::LargeAddressAware();
    uint64_t free = 0, block = 0;
    ScanAddressSpace(free, block);
    const uint32_t cap = std::min<uint32_t>(laa ? 256 * kMB : 64 * kMB, static_cast<uint32_t>(block / 4));
    if (want > cap) {
        features::ModLog("Memory: %s (%u KB) needs a %u MB global heap - capped at %u MB (%s address space, largest free "
                         "block %u MB)", name.c_str(), largest / 1024, want / kMB, cap / kMB, laa ? "4 GB" : "2 GB",
                         static_cast<unsigned>(block / kMB));
        want = std::max(original, cap);
    }
    return want;
}

void* __fastcall CreateHeap(void* heap, void*, uint32_t size, const char* name) {
    g_heap = static_cast<uint8_t*>(heap);
    // room for the enlarged pools: the plugin dsfix.limits (PSE\Plugins) says how much, nothing when it isn't there
    if (auto limits = static_cast<const DsfixLimitsV1*>(
            pse::MessagingInterface()->QueryInterface(pse_host::Core(), DSFIX_LIMITS_INTERFACE, 1));
        limits && limits->size >= sizeof(DsfixLimitsV1) && limits->HeapExtra)
        size += limits->HeapExtra();
    const uint32_t want = WantedHeap(size);
    if (want > size) {
        void* buf = DevDword("HeapAllocFail") ? nullptr
                                              : VirtualAlloc(nullptr, want + 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (buf) {
            g_heapOurs = true;
            features::ModLog("Memory: global heap %.1f MB (game's own: %u KB) - room for the largest mod file",
                             want / double(kMB), size / 1024);
            return reinterpret_cast<CtorBufferFn>(kHeapCtorBuffer)(heap, buf, want, name);
        }
        features::ModLog("[fail] Memory: couldn't reserve %.1f MB for the global heap - kept the game's %u KB (mod files "
                         "over ~1 MB won't load)", want / double(kMB), size / 1024);
    }
    return reinterpret_cast<CtorFn>(kHeapCtor)(heap, size, name);
}

// ---- heap use (peak, failures) ----
void Track(void* heap, const void* result, uint32_t size) {
    if (heap != g_heap) return;
    const uint32_t used = Field(g_heap, 0x30);
#ifndef DS_DIST
    if (used > g_heapPeak && used >= 512 * 1024 && used / (64 * 1024) != g_heapPeak / (64 * 1024))
        dslog::Write("Memory: global heap peak %u KB of %u KB (%s)", used / 1024, Field(g_heap, 0x28) / 1024,
                     g_reading ? g_reading : "-");
#endif
    g_heapPeak = std::max(g_heapPeak, used);
    if (!result) {
        ++g_heapFails;
        features::ModLog("[fail] Memory: %s (%u KB) didn't fit the global heap (%u KB free of %u KB)",
                         g_reading ? g_reading : "a game allocation", size / 1024, Field(g_heap, 0x2C) / 1024,
                         Field(g_heap, 0x28) / 1024);
    }
}

void* __fastcall HeapAllocHook(void* heap, void*, uint32_t size, uint32_t align) {
    void* p = g_alloc(heap, size, align);
    Track(heap, p, size);
    return p;
}

void* __fastcall HeapAllocAlignedHook(void* heap, void*, uint32_t size) {
    void* p = g_allocAligned(heap, size);
    Track(heap, p, size);
    return p;
}

// A mod's texture file that won't fit in the heap's free space (game files are all < 1 MB).
bool TooBigForHeap(const char* name) {
    if (!g_heap || !name) return false;
    const std::string path = features::ModFilePath(name);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (path.empty() || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa) || fa.nFileSizeHigh) return false;
    const uint32_t need = fa.nFileSizeLow + 1 + 16;   // the reader adds a terminating 0; alignment
    if (need <= Field(g_heap, 0x2C)) return false;
    uint64_t free = 0, block = 0;
    ScanAddressSpace(free, block);
    if (block < uint64_t(need) * 2) {   // the game allocator would retry for ever (a hang) - let the heap say no
        features::ModLog("[fail] Memory: %s (%u KB) - not enough address space left to load it", name, need / 1024);
        return false;
    }
    return true;
}

int __cdecl ReadFileHook(const char* name, void** buf, uint32_t* size, void* heap, int flag) {
    const char* outer = g_reading;
    g_reading = name;
    const auto ret = reinterpret_cast<uint32_t>(_ReturnAddress());
    bool texture = false;
    for (uint32_t r : kTexReadRets) texture |= ret == r;
    if (heap && heap == g_heap && texture && g_fallbackOk && !g_bigBuffer && TooBigForHeap(name)) {
        const int r = g_read(name, buf, size, nullptr, flag);   // heap 0: the game allocator
        if (r && buf) {
            g_bigBuffer = *buf;
            g_alloc(g_heap, 16, 4);   // the marker (if even 16 bytes don't fit, the loader's release still goes below)
            g_bigLevel = Field(g_heap, 0x30);
            if (g_bigReads++ < 20)
                features::ModLog("Memory: %s (%u KB) is bigger than the global heap's free %u KB - loaded outside it", name,
                                 size ? *size / 1024 : 0, Field(g_heap, 0x2C) / 1024);
        }
        g_reading = outer;
        return r;
    }
    const int r = g_read(name, buf, size, heap, flag);
#ifndef DS_DIST
    if (r && heap == g_heap && size && *size >= 256 * 1024)
        dslog::Write("Memory: %s read into the global heap (%u KB)", name, *size / 1024);
#endif
    g_reading = outer;
    return r;
}

void FreeBigIfReleased(void* heap) {
    if (g_bigBuffer && heap == g_heap && Field(g_heap, 0x30) < g_bigLevel) {
        reinterpret_cast<void(__cdecl*)(void*)>(kGameFree)(g_bigBuffer);
        g_bigBuffer = nullptr;
#ifndef DS_DIST
        dslog::Write("Memory: texture buffer outside the heap freed (heap back to %u KB)", Field(g_heap, 0x30) / 1024);
#endif
    }
}

void __fastcall HeapReleaseHook(void* heap, void*, uint32_t slot) {
    g_release(heap, slot);
    FreeBigIfReleased(heap);
}

void __fastcall HeapPopHook(void* heap, void*, uint32_t bytes) {
    g_pop(heap, bytes);
    FreeBigIfReleased(heap);
}

// ---- textures (all D3D8 textures the game or we create) ----
CRITICAL_SECTION g_texLock;
struct TexInfo {
    uint32_t bytes;
    uint32_t pool;
};
std::unordered_map<void*, TexInfo> g_textures;
uint64_t g_texBytes[4] = {};   // per D3DPOOL (0 default, 1 managed, 2 system memory, 3 scratch)
// Texture Release (IUnknown slot 2) per texture class (d3d8.dll's, or the D3D9 wrapper's Texture8 with Renderer >= 1):
// broker pointer hooks through core/comhooks.h, owner dsfix.memory, priority features::kD3dHookPriority.
using TexTable = comhooks::Table<3>;
TexTable g_texHooks;
using TexReleaseFn = ULONG(__stdcall*)(void*);
const comhooks::Next<TexTable, g_texHooks, 2, TexReleaseFn> g_texRelease{};

ULONG __stdcall TextureRelease(void* tex) {
    const ULONG left = g_texRelease(tex);
    if (!left) {
        EnterCriticalSection(&g_texLock);
        auto it = g_textures.find(tex);
        if (it != g_textures.end()) {
            g_texBytes[it->second.pool & 3] -= it->second.bytes;
            g_textures.erase(it);
        }
        LeaveCriticalSection(&g_texLock);
    }
    return left;
}

void HookRelease(void* tex) {
    if (g_texHooks.Hooked(tex, 2)) return;
    patch::OwnerScope owner(pse_host::Find("dsfix.memory"));  // run-time hook: owned by this feature's PSE plugin
    static int failures = 0;
    if (!g_texHooks.Hook(tex, 2, reinterpret_cast<const void*>(&TextureRelease), features::kD3dHookPriority) &&
        failures++ == 0)
        dslog::Write("[fail] Memory: texture Release hook (class %p) - texture memory figures stop going down",
                     static_cast<void*>(comhooks::Vtable(tex)));
}

uint32_t LevelBytes(uint32_t w, uint32_t h, uint32_t format) {
    switch (format) {
        case 0x31545844: return std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * 8;    // DXT1
        case 0x32545844: case 0x33545844: case 0x34545844: case 0x35545844:                   // DXT2-5
            return std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * 16;
        case 20: return w * h * 3;                                                             // R8G8B8
        case 23: case 24: case 25: case 26: case 29: case 30: case 51: return w * h * 2;       // 16-bit
        case 28: case 41: case 50: case 52: return w * h;                                      // A8, P8, L8, A4L4
        default: return w * h * 4;
    }
}
}  // namespace

bool features::LargeAddressAware() { return AddressLimit() > 0x80000000u; }

void features::NoteTexture(void* tex, uint32_t w, uint32_t h, uint32_t levels, uint32_t format, uint32_t pool) {
    if (!tex) return;
    uint64_t bytes = 0;
    for (uint32_t l = 0; (levels == 0 || l < levels) && l < 16; ++l) {
        bytes += LevelBytes(w, h, format);
        if (w == 1 && h == 1) break;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    EnterCriticalSection(&g_texLock);
    HookRelease(tex);
    auto& t = g_textures[tex];
    g_texBytes[t.pool & 3] -= t.bytes;   // a reused address (shouldn't happen) replaces the old entry
    t = {static_cast<uint32_t>(bytes), pool & 3};
    g_texBytes[pool & 3] += bytes;
    LeaveCriticalSection(&g_texLock);
}

features::MemoryStats features::GetMemoryStats() {
    static MemoryStats cached;
    static DWORD at = 0;
    if (at && GetTickCount() - at < 1000) return cached;
    at = GetTickCount() | 1;
    MemoryStats s;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    s.addressTotal = reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress) -
                     reinterpret_cast<uintptr_t>(si.lpMinimumApplicationAddress) + 1;
    uint64_t free = 0, block = 0;
    ScanAddressSpace(free, block);
    s.addressUsed = s.addressTotal - free;
    s.largestFree = block;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
        s.privateBytes = pmc.PrivateUsage;
    s.laa = LargeAddressAware();
    EnterCriticalSection(&g_texLock);
    s.textures = static_cast<uint32_t>(g_textures.size());
    s.textureBytesDefault = g_texBytes[0];
    s.textureBytesManaged = g_texBytes[1];
    s.textureBytesSystem = g_texBytes[2] + g_texBytes[3];
    LeaveCriticalSection(&g_texLock);
    const auto* r = *reinterpret_cast<const uint8_t* const*>(kRenderer);
    void* dev = r ? *reinterpret_cast<void* const*>(r + kDeviceOffset) : nullptr;
    if (dev) s.d3dFree = reinterpret_cast<UINT(__stdcall*)(void*)>((*static_cast<void***>(dev))[4])(dev);
    if (g_heap) {
        s.heapSize = Field(g_heap, 0x28);
        s.heapUsed = Field(g_heap, 0x30);
    }
    s.heapPeak = g_heapPeak;
    s.heapFails = g_heapFails;
    s.heapOurs = g_heapOurs;
    cached = s;
    return s;
}

void features::ApplyMemory() {
    static bool done = false;
    if (done) return;
    done = true;
    InitializeCriticalSection(&g_texLock);
    if (!patch::Matches(kHeapPushName, kHeapPushes, sizeof kHeapPushes) ||
        !patch::HookCall(kHeapCreateCall, reinterpret_cast<const void*>(&CreateHeap), kHeapCtor))
        dslog::Write("[fail] Memory: unexpected game code - global heap left at 1 MB");
    if (patch::Matches(kHeapAlloc, kAllocEntry, sizeof kAllocEntry) &&
        patch::Matches(kHeapAllocAligned, kAllocEntry, sizeof kAllocEntry) &&
        patch::Matches(kReadFile, kReadEntry, sizeof kReadEntry)) {
        if (HookEntry(kHeapAlloc, reinterpret_cast<const void*>(&HeapAllocHook), g_alloc, kAllocEntry, sizeof kAllocEntry) &&
            HookEntry(kHeapAllocAligned, reinterpret_cast<const void*>(&HeapAllocAlignedHook), g_allocAligned,
                      kAllocEntry, sizeof kAllocEntry) &&
            HookEntry(kReadFile, reinterpret_cast<const void*>(&ReadFileHook), g_read, kReadEntry, sizeof kReadEntry)) {
            bool ok = patch::Matches(kHeapRelease, kReleaseEntry, sizeof kReleaseEntry) &&
                      patch::Matches(kHeapPop, kPopEntry, sizeof kPopEntry);
            if (ok)
                ok = HookEntry(kHeapRelease, reinterpret_cast<const void*>(&HeapReleaseHook), g_release, kReleaseEntry,
                               sizeof kReleaseEntry) &&
                     HookEntry(kHeapPop, reinterpret_cast<const void*>(&HeapPopHook), g_pop, kPopEntry, sizeof kPopEntry);
            g_fallbackOk = ok;
            if (!ok) dslog::Write("[fail] Memory: texture loader release not hooked - big textures need the heap");
            return;
        }
    }
    dslog::Write("[fail] Memory: heap use not tracked (unexpected game code)");
}

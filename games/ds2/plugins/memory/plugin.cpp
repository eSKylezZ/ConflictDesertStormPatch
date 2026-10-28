// ds2fix.memory - DesertStorm2Fix's memory part as an external PSE plugin (games/ds2/plugins/memory): the game's "Global
// Heap" sized to what the mods need, texture files too big for it read outside it, and the memory figures (address
// space, heap, Direct3D textures). Port of DesertStormFix's memory.cpp (repo root src/features/memory.cpp).
//
// Global Heap (MemBuffers.cpp, "CLinearBuffer"), CDS II.exe 1.0 (DS1 addresses in brackets):
//   set-up       `push "Global Heap"; push 1024000; mov ecx, eax; call 0x4C0650` at 0x4136F0..0x4136FC -> [0x664380]
//                [0x40FFCB..0x40FFD7 -> [0x63C8B4]]. The object (0x68 bytes from 0x4BE2D0) is made once in the game's
//                start-up, after every plugin has loaded.
//   object       +0x00 name char[0x20], +0x20 mark slots (u32 "used" each), +0x48 size, +0x4C free, +0x50 used,
//                +0x58 buffer (size + 4: an "LBUF" guard follows the data), +0x5C 1 = buffer not owned (the dtor
//                0x4C0710 frees it only when 0), +0x60 buffer >> 31, +0x64 marks taken [+0x28 size, +0x2C free, +0x30
//                used, +0x34 buffer, +0x38 not owned]
//   ctor         0x4C0650 thiscall(size, name) ret 8: buffer from the game allocator 0x4BE320(size + 4, 0x25) - which
//                retries for ever when memory runs out (a hang, not an error) - filled with 0xFA. DS2 has no ctor taking
//                a caller's buffer [0x4C0340]: the plugin makes a 16-byte heap and swaps its own buffer in.
//   alloc        0x4C0750 thiscall(size, align) ret 8, 0x4C07A0 thiscall(size) ret 4 (16-aligned): 0 when it doesn't
//                fit (the "Out of heap space" message goes to a stubbed print 0x4CD600 - nothing shows)
//   mark         0x4C0810 thiscall() -> slot; release 0x4C0830 thiscall(slot) ret 4 (+0x64 = slot, used = marks[slot]);
//                pop 0x4C07E0 thiscall(bytes) ret 4. Sub-heaps carved out for good: decals 0x2000 + 0x12000 + 0x1000
//                (0x550B40 -> 0x4C0980 thiscall(parent, size, name)).
//   reader       0x4C49E0 cdecl(name, &buf, &size, heap, pool) [0x4BFB90]: resolves + opens the file, buf = heap alloc
//                (size + 1, align 4) or with heap 0 the game allocator 0x4BE320(size + 1, pool); free = 0x4BE3B0.
//                Texture loaders read every texture file whole into the heap, each between its own mark and release:
//                0x5421F0 (call 0x54222A), 0x5422A0 (0x5422DA), 0x5427B0 (0x542811) and 0x542E20 (0x542E5A). A file
//                that doesn't fit = no texture (silently).
//
// Size: the original 1 024 000 bytes (or what a plugin in front added) + AddHeapReserve bytes (interface) + the largest
// file in any Mods folder, on or off (rounded up to 1 MB, files the heap never holds left out), capped at 64 MB (2 GB
// address space) / 256 MB (Large Address Aware) and a quarter of the largest free block. Our buffer (VirtualAlloc); when
// it can't be had, the game's own allocation of the original size.
//
// Texture files the heap can't hold (a mod switched on later, one past the cap) are read through the game allocator
// instead - only for the four texture loaders' reads, decided by the size of the file the resolver names (the mods
// plugin's hook answers there). The buffer is freed once the loader's own release (or a pop) goes back past the mark it
// took before the read.
//
// Also: Direct3D texture memory (IDirect3DDevice9::CreateTexture / IDirect3DTexture9::Release, broker pointer hooks per
// COM class, chained with ds2fix.graphics' device hooks), a log line per level load and every 30 s in dev builds, and the
// main menu line "2 GB MEMORY MODE - RUN DESERTSTORM2FIX-PATCHER.EXE FOR 4 GB" while the exe lacks the 4 GB flag (main
// menu draw, vtable 0x5E6F58 slot 5, chained with ds2fix.menus' MainDraw). Interface ds2fix.memory v1 (ds2fix_memory.h).
#include <windows.h>
#include <d3d9.h>
#include <psapi.h>

#include <intrin.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "ds2fix_memory.h"
#include "pse/pse_api.h"

namespace {
PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSEHooksInterface* g_hooks = nullptr;

constexpr uint32_t kMB = 1024 * 1024;
constexpr uint32_t kHeapSetupCall = 0x4136FC, kHeapCtor = 0x4C0650, kHeapGlobal = 0x664380;
constexpr uint8_t kHeapPushes[] = {0x68, 0x24, 0xBA, 0x5D, 0x00, 0x68, 0x00, 0xA0, 0x0F, 0x00, 0x8B, 0xC8};  // at 0x4136F0
constexpr uint32_t kHeapAlloc = 0x4C0750, kHeapAlloc16 = 0x4C07A0, kHeapPop = 0x4C07E0, kHeapRelease = 0x4C0830;
constexpr uint8_t kAllocEntry[] = {0x8B, 0x41, 0x58, 0x8B, 0x54, 0x24, 0x08};    // mov eax,[ecx+0x58]; mov edx,[esp+8]
constexpr uint8_t kAlloc16Entry[] = {0x8B, 0x41, 0x58, 0x8B, 0x54, 0x24, 0x04};  // mov eax,[ecx+0x58]; mov edx,[esp+4]
constexpr uint8_t kPopEntry[] = {0x8B, 0x51, 0x50, 0x8B, 0x44, 0x24, 0x04};      // mov edx,[ecx+0x50]; mov eax,[esp+4]
constexpr uint8_t kReleaseEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x89, 0x41, 0x64};  // mov eax,[esp+4]; mov [ecx+0x64],eax
constexpr uint32_t kReadFile = 0x4C49E0;
constexpr uint8_t kReadEntry[] = {0xA1, 0xAC, 0xD3, 0x67, 0x00};                 // mov eax, [0x67D3AC] (search paths)
constexpr uint32_t kTexReadRets[] = {0x54222F, 0x5422DF, 0x542816, 0x542E5F};    // after the texture loaders' reads
constexpr uint32_t kResolve = 0x4C3C40, kSearchPaths = 0x67D3AC;
constexpr uint32_t kGameAlloc = 0x4BE320, kGameFree = 0x4BE3B0;
constexpr uint32_t kRenderer = 0x680038, kRendererW = 0x41A38, kRendererH = 0x41A3C;
constexpr uint32_t kMainMenuDrawSlot = 0x5E6F58 + 5 * 4;   // main menu vtable +0x14 (0x462000, or ds2fix.menus' MainDraw)
constexpr uint32_t kFontSmall = 0x62F43C, kFontAlpha = 0x526E40, kFontColour = 0x526E20, kFontDraw = 0x529490;
constexpr uint32_t kOriginalHeap = 1024000;

// ---- small helpers ----
void Log(PSELogLevel level, const char* fmt, ...) {
    if (!g_log) return;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    g_log->Write(g_self, level, "%s", line);
}

DWORD DevDword(const char* name) {
#ifdef DS_DIST
    (void)name;
    return 0;
#else
    DWORD v = 0, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
#endif
}

uint32_t& Field(uint8_t* heap, uint32_t off) { return *reinterpret_cast<uint32_t*>(heap + off); }

template <class R = uint32_t, class... A>
R ThisCall(uint32_t fn, void* self, A... a) {
    return reinterpret_cast<R(__thiscall*)(void*, A...)>(fn)(self, a...);
}

bool Matches(uint32_t va, const void* bytes, size_t len) {
    __try {
        return std::memcmp(reinterpret_cast<const void*>(va), bytes, len) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- address space ----
bool ExeHasLaaFlag() {
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
}

bool Is64BitWindows() {
    BOOL wow = FALSE;
    return IsWow64Process(GetCurrentProcess(), &wow) && wow;
}

bool LargeAddressAware() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress) > 0x80000000u;
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
uint8_t* g_heap = nullptr;   // the Global Heap object
bool g_heapOurs = false, g_heapMade = false, g_fallbackOk = false;
uint32_t g_reserve = 0, g_cap = 0, g_heapPeak = 0, g_heapFails = 0, g_bigReads = 0;
using HeapCtorFn = void*(__thiscall*)(void*, uint32_t, const char*);
void* g_heapNext = nullptr;  // broker next: ds2fix.mods' hook (passes through while we are here) or the ctor

// Files the heap never holds (as ds2fix.mods' SizesHeap): they don't size it.
bool SizesHeap(const std::string& upperName) {
    static const char* const kSkip[] = {".DLL", ".WAV", ".BIK", ".BLEND", ".BLEND1", ".EXE", ".ZIP", ".7Z", ".MP4",
                                        ".JSON", ".MD", ".PY", ".TXT", ".SCH", ".SBK", ".LUA", ".PSD", ".PSF"};
    const size_t dot = upperName.find_last_of('.');
    if (dot == std::string::npos) return true;
    for (const char* e : kSkip)
        if (upperName.compare(dot, std::string::npos, e) == 0) return false;
    return true;
}

bool ModsFolderOn() {   // ds2fix's FeatMods (default on); without ds2fix nothing serves mod files anyway
    DWORD v = 1, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\PSE\\ds2\\Plugins\\ds2fix", "FeatMods", RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v != 0;
}

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    return dir.substr(0, dir.find_last_of("\\/"));
}

// The largest file in ANY Mods folder, switched on or off: the MODS menu switches folders on while the game runs, and
// the heap can't grow once made.
uint32_t LargestModFile(std::string& name) {
    uint32_t largest = 0;
    std::vector<std::string> dirs = {GameDir() + "\\Mods"};
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
            std::string upper = n;
            CharUpperBuffA(upper.data(), static_cast<DWORD>(upper.size()));
            if (!SizesHeap(upper)) continue;
            const uint32_t size = fd.nFileSizeHigh ? 0xFFFFFFFFu : fd.nFileSizeLow;
            if (size > largest) largest = size, name = n;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return largest;
}

uint32_t Cap() {
    uint64_t free = 0, block = 0;
    ScanAddressSpace(free, block);
    return std::min<uint32_t>(LargeAddressAware() ? 256 * kMB : 64 * kMB, static_cast<uint32_t>(block / 4));
}

uint32_t WantedHeap(uint32_t base) {
    g_cap = Cap();
    if (const DWORD mb = DevDword("HeapMB")) return std::max<uint32_t>(base, mb * kMB);   // dev: forced size
    uint32_t want = base;
    std::string name;
    const uint32_t largest = ModsFolderOn() ? LargestModFile(name) : 0;
    if (largest > 256 * 1024) {   // smaller files fit next to the game's own use (its peak stays under ~600 KB)
        want = base + static_cast<uint32_t>(std::min<uint64_t>((uint64_t{largest} + kMB - 1) / kMB * kMB, 0x7FF00000u));
        Log(PSE_LOG_INFO, "Memory: largest mod file %s (%u KB)", name.c_str(), largest / 1024);
    }
    if (want > g_cap) {
        Log(PSE_LOG_WARN, "Memory: a %u MB global heap is wanted - capped at %u MB (%s address space); bigger texture "
                          "files are read outside the heap", want / kMB, g_cap / kMB, LargeAddressAware() ? "4 GB" : "2 GB");
        want = std::max(base, g_cap);
    }
    return want;
}

void* __fastcall HeapSetup(void* heap, void*, uint32_t size, const char* name) {
    g_heap = static_cast<uint8_t*>(heap);
    g_heapMade = true;
    const uint32_t base = size + g_reserve;
    const uint32_t want = WantedHeap(base);
    const auto next = reinterpret_cast<HeapCtorFn>(g_heapNext);
    if (want == size) {
        Log(PSE_LOG_INFO, "Memory: global heap %u KB (no mod file needs more; cap %u MB)", size / 1024, g_cap / kMB);
        return next(heap, size, name);
    }
    // The game's ctor with a 16-byte buffer (whatever a subscriber behind us adds to it is kept), then our buffer.
    void* obj = next(heap, 16, name);
    const uint32_t behind = Field(g_heap, 0x48) > 16 ? Field(g_heap, 0x48) - 16 : 0;
    uint32_t total = want + behind;
    void* buf = DevDword("HeapAllocFail") ? nullptr
                                          : VirtualAlloc(nullptr, total + 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    bool ours = buf != nullptr;
    if (!buf) {   // the game's own way, at the size it would have had (its allocator waits for ever if this fails too)
        total = base + behind;
        buf = reinterpret_cast<void*(__cdecl*)(uint32_t, uint32_t)>(kGameAlloc)(total + 4, 0x25);
        Log(PSE_LOG_ERROR, "[fail] Memory: couldn't reserve %.1f MB for the global heap - kept %u KB (mod textures over "
                           "the heap's size are read outside it)", (want + behind) / double(kMB), total / 1024);
    }
    reinterpret_cast<void(__cdecl*)(void*)>(kGameFree)(reinterpret_cast<void*>(Field(g_heap, 0x58)));
    Field(g_heap, 0x48) = total;
    Field(g_heap, 0x4C) = total;
    Field(g_heap, 0x50) = 0;
    Field(g_heap, 0x58) = reinterpret_cast<uint32_t>(buf);
    Field(g_heap, 0x5C) = ours ? 1 : 0;
    Field(g_heap, 0x60) = reinterpret_cast<uint32_t>(buf) >> 31;
    Field(g_heap, 0x64) = 0;
    std::memcpy(static_cast<uint8_t*>(buf) + total, "LBUF", 4);
    g_heapOurs = ours;
    if (ours)
        Log(PSE_LOG_INFO, "Memory: global heap %.1f MB (game's own: %u KB, reserved by plugins: %u KB, cap %u MB)",
            total / double(kMB), size / 1024, g_reserve / 1024, g_cap / kMB);
    return obj;
}

// ---- heap use (peak, failures) ----
const char* g_reading = nullptr;   // the file the reader is reading (game thread)
using AllocFn = void*(__thiscall*)(void*, uint32_t, uint32_t);
using Alloc16Fn = void*(__thiscall*)(void*, uint32_t);
using ReleaseFn = void(__thiscall*)(void*, uint32_t);
using ReadFn = int(__cdecl*)(const char*, void**, uint32_t*, void*, int);
void* g_allocNext = nullptr;
void* g_alloc16Next = nullptr;
void* g_releaseNext = nullptr;
void* g_popNext = nullptr;
void* g_readNext = nullptr;

void Track(void* heap, const void* result, uint32_t size) {
    if (heap != g_heap || !g_heap) return;
    const uint32_t used = Field(g_heap, 0x50);
#ifndef DS_DIST
    if (used > g_heapPeak && used >= 512 * 1024 && used / (64 * 1024) != g_heapPeak / (64 * 1024))
        Log(PSE_LOG_INFO, "Memory: global heap peak %u KB of %u KB (%s)", used / 1024, Field(g_heap, 0x48) / 1024,
            g_reading ? g_reading : "-");
#endif
    g_heapPeak = std::max(g_heapPeak, used);
    if (!result) {
        if (g_heapFails++ < 20)
            Log(PSE_LOG_ERROR, "[fail] Memory: %s (%u KB) didn't fit the global heap (%u KB free of %u KB)",
                g_reading ? g_reading : "a game allocation", size / 1024, Field(g_heap, 0x4C) / 1024,
                Field(g_heap, 0x48) / 1024);
    }
}

void* __fastcall HeapAllocHook(void* heap, void*, uint32_t size, uint32_t align) {
    void* p = reinterpret_cast<AllocFn>(g_allocNext)(heap, size, align);
    Track(heap, p, size);
    return p;
}

void* __fastcall HeapAlloc16Hook(void* heap, void*, uint32_t size) {
    void* p = reinterpret_cast<Alloc16Fn>(g_alloc16Next)(heap, size);
    Track(heap, p, size);
    return p;
}

// ---- texture files outside the heap ----
void* g_bigBuffer = nullptr;   // a texture file read outside the heap, freed at the loader's release
uint32_t g_bigMarks = 0;       // the heap's mark count at the read: a release to a slot below it = the loader's own
uint32_t g_bigUsed = 0;        // the heap's use at the read: a pop below it = the same

// The file the reader would open: the resolver's answer (ds2fix.mods' hook answers for mod files) if it is on disk, and
// too big for the heap's free space.
bool TooBigForHeap(const char* name) {
    if (!g_heap || !name) return false;
    const char* path = reinterpret_cast<const char*(__cdecl*)(const char*, uint32_t, uint32_t)>(kResolve)(
        name, *reinterpret_cast<const uint32_t*>(kSearchPaths), 0);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!path || !GetFileAttributesExA(path, GetFileExInfoStandard, &fa) || fa.nFileSizeHigh) return false;
    const uint32_t need = fa.nFileSizeLow + 1 + 4;   // the reader adds a terminating 0; alignment
    if (need <= Field(g_heap, 0x4C)) return false;
    uint64_t free = 0, block = 0;
    ScanAddressSpace(free, block);
    if (block < uint64_t(need) * 2) {   // the game allocator would retry for ever (a hang) - let the heap say no
        Log(PSE_LOG_ERROR, "[fail] Memory: %s (%u KB) - not enough address space left to load it", name, need / 1024);
        return false;
    }
    return true;
}

int __cdecl ReadWholeFile(const char* name, void** buf, uint32_t* size, void* heap, int pool) {
    const char* outer = g_reading;
    g_reading = name;
    const auto ret = reinterpret_cast<uint32_t>(_ReturnAddress());
    bool texture = false;
    for (uint32_t r : kTexReadRets) texture |= ret == r;
    const auto next = reinterpret_cast<ReadFn>(g_readNext);
    if (heap && heap == g_heap && texture && g_fallbackOk && !g_bigBuffer && TooBigForHeap(name)) {
        const int r = next(name, buf, size, nullptr, pool);   // heap 0: the game allocator
        if (r && buf && *buf) {
            g_bigBuffer = *buf;
            g_bigMarks = Field(g_heap, 0x64);
            g_bigUsed = Field(g_heap, 0x50);
            if (g_bigReads++ < 20)
                Log(PSE_LOG_INFO, "Memory: %s (%u KB) is bigger than the global heap's free %u KB - loaded outside it",
                    name, size ? *size / 1024 : 0, Field(g_heap, 0x4C) / 1024);
        }
        g_reading = outer;
        return r;
    }
    const int r = next(name, buf, size, heap, pool);
#ifndef DS_DIST
    if (r && heap == g_heap && size && *size >= 256 * 1024)
        Log(PSE_LOG_INFO, "Memory: %s read into the global heap (%u KB)", name, *size / 1024);
#endif
    g_reading = outer;
    return r;
}

void FreeBigIfReleased(void* heap) {
    if (!g_bigBuffer || heap != g_heap) return;
    if (Field(g_heap, 0x64) >= g_bigMarks && Field(g_heap, 0x50) >= g_bigUsed) return;   // an inner release
    reinterpret_cast<void(__cdecl*)(void*)>(kGameFree)(g_bigBuffer);
    g_bigBuffer = nullptr;
#ifndef DS_DIST
    Log(PSE_LOG_INFO, "Memory: texture buffer outside the heap freed (heap back to %u KB)", Field(g_heap, 0x50) / 1024);
#endif
}

void __fastcall HeapReleaseHook(void* heap, void*, uint32_t slot) {
    reinterpret_cast<ReleaseFn>(g_releaseNext)(heap, slot);
    FreeBigIfReleased(heap);
}

void __fastcall HeapPopHook(void* heap, void*, uint32_t bytes) {
    reinterpret_cast<ReleaseFn>(g_popNext)(heap, bytes);
    FreeBigIfReleased(heap);
}

// ---- Direct3D textures ----
// A vtable is shared by every object of a COM class: one broker pointer hook per class and slot, its own `next` (found
// from the object's vtable, so other plugins can chain on the same slots at their own priority).
struct ComClass {
    void** vtable;
    void* next;
};
SRWLOCK g_classLock = SRWLOCK_INIT;
std::vector<ComClass> g_devClasses, g_texClasses;   // CreateTexture (device slot 23), Release (texture slot 2)

void* NextOf(std::vector<ComClass>& classes, void* obj, int slot) {
    void** vt = *static_cast<void***>(obj);
    AcquireSRWLockShared(&g_classLock);
    void* next = nullptr;
    for (const ComClass& c : classes)
        if (c.vtable == vt) next = c.next;
    ReleaseSRWLockShared(&g_classLock);
    return next ? next : vt[slot];
}

bool HookClass(std::vector<ComClass>& classes, void* obj, int slot, void* fn) {
    void** vt = *static_cast<void***>(obj);
    AcquireSRWLockExclusive(&g_classLock);
    bool ok = false;
    for (const ComClass& c : classes) ok = ok || c.vtable == vt;
    if (!ok) {
        classes.reserve(8);   // `next` addresses must stay put: the broker writes them
        if (classes.size() < 8) {
            classes.push_back({vt, nullptr});
            ok = g_hooks->HookPointer(g_self, reinterpret_cast<uint32_t>(&vt[slot]), fn, 50, &classes.back().next,
                                      nullptr) == PSE_OK;
            if (!ok) classes.pop_back();
        }
    }
    ReleaseSRWLockExclusive(&g_classLock);
    return ok;
}

struct TexInfo {
    uint32_t bytes;
    uint32_t pool;
};
SRWLOCK g_texLock = SRWLOCK_INIT;
std::unordered_map<void*, TexInfo> g_textures;
uint64_t g_texBytes[4] = {};   // per D3DPOOL (0 default, 1 managed, 2 system memory, 3 scratch)
IDirect3DDevice9* g_device = nullptr;

uint32_t LevelBytes(uint32_t w, uint32_t h, uint32_t format) {
    switch (format) {
        case D3DFMT_DXT1: return std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * 8;
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
            return std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * 16;
        case D3DFMT_R8G8B8: return w * h * 3;
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8R3G3B2:
        case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L16: return w * h * 2;
        case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_L8: case D3DFMT_A4L4: return w * h;
        case D3DFMT_A16B16G16R16F: case D3DFMT_A16B16G16R16: return w * h * 8;
        case D3DFMT_A32B32G32R32F: return w * h * 16;
        default: return w * h * 4;
    }
}

using ReleaseComFn = ULONG(__stdcall*)(IUnknown*);
ULONG __stdcall TextureRelease(IUnknown* tex) {
    const ULONG left = reinterpret_cast<ReleaseComFn>(NextOf(g_texClasses, tex, 2))(tex);
    if (!left) {
        AcquireSRWLockExclusive(&g_texLock);
        auto it = g_textures.find(tex);
        if (it != g_textures.end()) {
            g_texBytes[it->second.pool & 3] -= it->second.bytes;
            g_textures.erase(it);
        }
        ReleaseSRWLockExclusive(&g_texLock);
    }
    return left;
}

void NoteTexture(IDirect3DTexture9* tex, uint32_t w, uint32_t h, uint32_t format, uint32_t pool) {
    static bool failed = false;
    if (!HookClass(g_texClasses, tex, 2, reinterpret_cast<void*>(&TextureRelease)) && !failed) {
        failed = true;
        Log(PSE_LOG_WARN, "Memory: texture Release hook refused - texture memory figures stop going down");
    }
    uint64_t bytes = 0;
    const uint32_t levels = tex->GetLevelCount();
    for (uint32_t l = 0; l < levels && l < 16; ++l) {
        bytes += LevelBytes(w, h, format);
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    AcquireSRWLockExclusive(&g_texLock);
    auto& t = g_textures[tex];
    g_texBytes[t.pool & 3] -= t.bytes;   // a reused address (a Release we didn't see) replaces the old entry
    t = {static_cast<uint32_t>(bytes), pool & 3};
    g_texBytes[pool & 3] += bytes;
    ReleaseSRWLockExclusive(&g_texLock);
}

using CreateTextureFn = HRESULT(__stdcall*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                            IDirect3DTexture9**, HANDLE*);
HRESULT __stdcall CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT format,
                                D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared) {
    const HRESULT hr =
        reinterpret_cast<CreateTextureFn>(NextOf(g_devClasses, dev, 23))(dev, w, h, levels, usage, format, pool, out, shared);
    if (SUCCEEDED(hr) && out && *out) NoteTexture(*out, w, h, static_cast<uint32_t>(format), static_cast<uint32_t>(pool));
    return hr;
}

// ---- figures ----
Ds2FixMemoryStatsV1 Stats() {
    Ds2FixMemoryStatsV1 s = {};
    s.size = sizeof s;
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
    s.laa = LargeAddressAware() ? 1 : 0;
    if (g_heap && g_heapMade) {
        s.heapSize = Field(g_heap, 0x48);
        s.heapUsed = Field(g_heap, 0x50);
    }
    s.heapPeak = g_heapPeak;
    s.heapFails = g_heapFails;
    s.heapOurs = g_heapOurs ? 1 : 0;
    s.bigReads = g_bigReads;
    AcquireSRWLockShared(&g_texLock);
    s.textures = static_cast<uint32_t>(g_textures.size());
    s.textureBytesDefault = g_texBytes[0];
    s.textureBytesManaged = g_texBytes[1];
    s.textureBytesSystem = g_texBytes[2] + g_texBytes[3];
    ReleaseSRWLockShared(&g_texLock);
    if (g_device) s.d3dFreeMB = g_device->GetAvailableTextureMem() / kMB;
    return s;
}

void LogStats(const char* when) {
    const Ds2FixMemoryStatsV1 s = Stats();
    Log(PSE_LOG_INFO,
        "Memory (%s): address space %u / %u MB used (%s), largest free %u MB, private %u MB; global heap %u / %u KB, "
        "peak %u KB, %u failed, %u read outside; textures %u: %.1f MB video + %.1f MB managed + %.1f MB system, D3D "
        "free %u MB", when, static_cast<unsigned>(s.addressUsed / kMB), static_cast<unsigned>(s.addressTotal / kMB),
        s.laa ? "4 GB" : "2 GB", static_cast<unsigned>(s.largestFree / kMB), static_cast<unsigned>(s.privateBytes / kMB),
        s.heapUsed / 1024, s.heapSize / 1024, s.heapPeak / 1024, s.heapFails, s.bigReads, s.textures,
        s.textureBytesDefault / double(kMB), s.textureBytesManaged / double(kMB), s.textureBytesSystem / double(kMB),
        s.d3dFreeMB);
}

int PSE_CALL OnLevelLoaded(PSEEventId, void* data, void*) {
    LogStats(data ? static_cast<const char*>(data) : "level");
    return 0;
}

#ifndef DS_DIST
int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    static DWORD last = GetTickCount();
    const DWORD every = DevDword("MemLogSec") ? DevDword("MemLogSec") * 1000 : 30000;
    if (GetTickCount() - last >= every) {
        last = GetTickCount();
        LogStats("timer");
    }
    return 0;
}
#endif

// ---- main menu notice ----
void* g_mainDrawNext = nullptr;
void __fastcall MainMenuDraw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(g_mainDrawNext)(screen);
    // Above the menu panel, inside the monitor's screen (the 1280 x 1024 front end is shown zoomed by the 3D monitor:
    // only y ~287..752 / x ~330..953 are visible; the title box starts at ~322). ds2fix.modsmenu's lines take the two
    // rows under the panel (y 725 / 743). Only on the front end's 1280 x 1024 screen.
    const auto* r = *reinterpret_cast<const uint8_t* const*>(kRenderer);
    void* font = *reinterpret_cast<void**>(kFontSmall);
    const int w = r ? *reinterpret_cast<const int*>(r + kRendererW) : 0, h = r ? *reinterpret_cast<const int*>(r + kRendererH) : 0;
#ifndef DS_DIST
    static int logged = 0;
    if (logged++ < 3) Log(PSE_LOG_INFO, "[dev] Memory: main menu draw (2D screen %dx%d, font %p)", w, h, font);
#endif
    if (!font || w != 1280 || h != 1024) return;
    ThisCall(kFontAlpha, font, 0xFF);
    ThisCall(kFontColour, font, 0xFFE0B040u);
    ThisCall(kFontDraw, font, "2 GB MEMORY MODE - RUN DESERTSTORM2FIX-PATCHER.EXE FOR 4 GB", 374, 305, 1);
}

// ---- interface ds2fix.memory v1 ----
int PSE_CALL IfAddHeapReserve(PSEPluginHandle plugin, uint32_t bytes) {
    if (g_heapMade) return 0;
    g_reserve += bytes;
    Log(PSE_LOG_INFO, "Memory: plugin %u reserves %u KB of global heap", plugin, bytes / 1024);
    return 1;
}
uint32_t PSE_CALL IfHeapSize() { return g_heap && g_heapMade ? Field(g_heap, 0x48) : 0; }
uint32_t PSE_CALL IfHeapCap() { return g_cap ? g_cap : Cap(); }
int PSE_CALL IfLargeAddressAware() { return LargeAddressAware() ? 1 : 0; }
int PSE_CALL IfLoadsOutside() { return g_fallbackOk ? 1 : 0; }
void PSE_CALL IfGetStats(Ds2FixMemoryStatsV1* out) {
    if (!out || out->size < sizeof(uint32_t)) return;
    const Ds2FixMemoryStatsV1 s = Stats();
    std::memcpy(out, &s, std::min<uint32_t>(out->size, sizeof s));
    out->size = std::min<uint32_t>(out->size, sizeof s);
}
const Ds2FixMemoryV1 kInterface = {sizeof(Ds2FixMemoryV1), 1, &IfAddHeapReserve, &IfHeapSize, &IfHeapCap,
                                   &IfLargeAddressAware, &IfLoadsOutside, &IfGetStats};

void PSE_CALL OnMessage(const PSEMessage* msg, void*) {
    if (msg->type != PSE_MSG_DEVICE_CREATED || !msg->data) return;
    g_device = static_cast<IDirect3DDevice9*>(const_cast<void*>(msg->data));
    if (!HookClass(g_devClasses, g_device, 23, reinterpret_cast<void*>(&CreateTexture)))
        Log(PSE_LOG_WARN, "Memory: IDirect3DDevice9::CreateTexture hook refused - no texture memory figures");
}

bool HookEntry(uint32_t entry, const uint8_t* prologue, uint32_t len, void* fn, void** next) {
    return g_hooks->HookFunction(g_self, entry, len, prologue, fn, 100, next, nullptr) == PSE_OK;
}
}  // namespace

// ---- plugin entry points ---------------------------------------------------------------------------------------------
extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"ds2", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "ds2fix.memory", "DesertStorm2Fix - memory",
                                      "DesertStorm2Fix", PSE_MAKE_VERSION(0, 1, 0), 0, kGames, PSE_PLUGIN_CLIENT_ONLY};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_hooks = static_cast<const PSEHooksInterface*>(pse->Query(g_self, PSE_HOOKS_INTERFACE, 1));
    const auto* messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    if (!g_log) return 0;
    if (!g_hooks || !messaging) {
        Log(PSE_LOG_ERROR, "ds2fix.memory: the loader lacks pse.hooks / pse.messaging v1");
        return 0;
    }
    uint64_t free = 0, block = 0;
    ScanAddressSpace(free, block);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    Log(PSE_LOG_INFO, "Memory: address space %u MB (%s), largest free block %u MB",
        static_cast<unsigned>((reinterpret_cast<uintptr_t>(si.lpMaximumApplicationAddress) >> 20) + 1),
        LargeAddressAware() ? "4 GB: the exe has the Large Address Aware flag"
        : ExeHasLaaFlag()   ? "the exe has the 4 GB flag, but this Windows gives 2 GB"
                            : "2 GB: DesertStorm2Fix-Patcher.exe turns 4 GB on",
        static_cast<unsigned>(block / kMB));

    if (!Matches(kHeapSetupCall - 12, kHeapPushes, sizeof kHeapPushes) ||
        g_hooks->HookCallSite(g_self, kHeapSetupCall, kHeapCtor, reinterpret_cast<void*>(&HeapSetup), 100, &g_heapNext,
                              nullptr) != PSE_OK) {
        Log(PSE_LOG_ERROR, "[fail] Memory: global heap set-up at 0x%08X not recognised - heap left to ds2fix.mods / the "
                           "game", kHeapSetupCall);
    } else if (messaging->RegisterInterface(g_self, DS2FIX_MEMORY_INTERFACE, 1, &kInterface) != PSE_OK) {
        Log(PSE_LOG_ERROR, "[fail] Memory: interface %s not registered", DS2FIX_MEMORY_INTERFACE);
    }
    const bool tracked = HookEntry(kHeapAlloc, kAllocEntry, sizeof kAllocEntry, reinterpret_cast<void*>(&HeapAllocHook), &g_allocNext) &&
                         HookEntry(kHeapAlloc16, kAlloc16Entry, sizeof kAlloc16Entry, reinterpret_cast<void*>(&HeapAlloc16Hook),
                                   &g_alloc16Next);
    if (!tracked) Log(PSE_LOG_WARN, "Memory: heap allocations not tracked (unexpected game code)");
    if (HookEntry(kReadFile, kReadEntry, sizeof kReadEntry, reinterpret_cast<void*>(&ReadWholeFile), &g_readNext)) {
        g_fallbackOk = HookEntry(kHeapRelease, kReleaseEntry, sizeof kReleaseEntry, reinterpret_cast<void*>(&HeapReleaseHook),
                                 &g_releaseNext) &&
                       HookEntry(kHeapPop, kPopEntry, sizeof kPopEntry, reinterpret_cast<void*>(&HeapPopHook), &g_popNext);
        if (!g_fallbackOk) Log(PSE_LOG_WARN, "Memory: heap release not hooked - big textures need the heap");
    } else {
        Log(PSE_LOG_WARN, "Memory: file reader at 0x%08X not recognised - big textures need the heap", kReadFile);
    }
    messaging->RegisterListener(g_self, nullptr, &OnMessage, nullptr);
    messaging->Subscribe(g_self, PSE_EVENT_LEVEL_LOAD_END, &OnLevelLoaded, nullptr, 0);
#ifndef DS_DIST
    messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
#endif
    if (Is64BitWindows() && !ExeHasLaaFlag() &&
        g_hooks->HookPointer(g_self, kMainMenuDrawSlot, reinterpret_cast<void*>(&MainMenuDraw), 100, &g_mainDrawNext,
                             nullptr) != PSE_OK)
        Log(PSE_LOG_WARN, "Memory: main menu draw not hooked - no 4 GB notice");
    Log(PSE_LOG_INFO, "[ok]   Memory: heap sizing, %s, texture accounting", g_fallbackOk ? "big textures outside the heap"
                                                                                       : "no big-texture fallback");
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {}
}

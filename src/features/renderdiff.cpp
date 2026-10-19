// Dev: which memory does rendering write? Dev\RenderDiff = n: in a mission, from its n-th rendered frame for 120
// frames, the game's data section (0x5E6000..0x773000), every soldier (0x2B10 bytes) and every scene object's render
// instance (0x2C8 bytes) are copied before the render loop (FUN_0040ef70, wrapped by framerec.cpp) and compared after
// it; at the end each dword the render changed is logged with the number of frames it changed in ("[render-diff]").
// Online lockstep needs the simulation to be independent of what each PC draws - every field listed here that the
// game logic reads is a desync source between PCs with different screens.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "features/features.h"

#ifndef DS_DIST
namespace {
constexpr uint32_t kData = 0x5E6000, kDataSize = 0x18D000;
constexpr uint32_t kObjects = 0x63C9A0, kSoldierVtable = 0x5D88E0, kSoldierSize = 0x2B10, kInstanceSize = 0x2C8;
constexpr uint32_t kLevelName = 0x606880, kDispatchMode = 0x60EC8C;
constexpr int kFrames = 120;

DWORD g_from = 0xFFFFFFFF;
int g_frame = 0;   // rendered frames in the current mission
char g_level[32] = {};
bool g_done = false;
std::vector<uint8_t> g_data;
struct Copy {
    const uint8_t* at;
    uint32_t size;
    std::vector<uint8_t> before;
    char kind;   // 'S' soldier, 'I' instance
};
std::vector<Copy> g_copies;
std::map<std::string, int> g_changed;   // "D 0x606880" / "S +0x30" / "I +0xD0" -> frames changed

bool Active() {
    if (g_from == 0xFFFFFFFF) {
        DWORD v = 0, size = sizeof v;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "RenderDiff", RRF_RT_REG_DWORD, nullptr, &v,
                     &size);
        g_from = v ? v : 0xFFFFFFFE;
    }
    if (g_from == 0xFFFFFFFE || g_done) return false;
    if (*reinterpret_cast<int8_t*>(kDispatchMode) != 0) return false;
    const char* level = reinterpret_cast<const char*>(kLevelName);
    if (_strnicmp(level, "Mission", 7) != 0) return false;
    if (strncmp(level, g_level, sizeof g_level - 1) != 0) {
        strncpy_s(g_level, level, sizeof g_level - 1);
        g_frame = 0;
    }
    return true;
}

void Readable(const uint8_t* at, uint32_t size, char kind) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!at || !VirtualQuery(at, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT ||
        reinterpret_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize < at + size)
        return;
    Copy c{at, size, std::vector<uint8_t>(at, at + size), kind};
    g_copies.push_back(std::move(c));
}
}  // namespace

void features::RenderDiffBefore() {
    if (!Active()) return;
    ++g_frame;
    if (g_frame < static_cast<int>(g_from) || g_frame >= static_cast<int>(g_from) + kFrames) return;
    g_data.assign(reinterpret_cast<const uint8_t*>(kData), reinterpret_cast<const uint8_t*>(kData) + kDataSize);
    g_copies.clear();
    const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(kObjects);
    if (!mgr) return;
    auto* const* table = *reinterpret_cast<const uint8_t* const* const*>(mgr + 0x30);
    const uint32_t maxId = *reinterpret_cast<const uint32_t*>(mgr + 0x34);
    for (uint32_t id = 0; table && id <= maxId && id < 4096; ++id)
        if (const uint8_t* o = table[id]) {
            if (*reinterpret_cast<const uint32_t*>(o) == kSoldierVtable) Readable(o, kSoldierSize, 'S');
            Readable(*reinterpret_cast<const uint8_t* const*>(o + 0x2C), kInstanceSize, 'I');
        }
}

void features::RenderDiffAfter() {
    if (g_data.empty()) return;
    char key[40];
    const auto* now = reinterpret_cast<const uint32_t*>(kData);
    const auto* was = reinterpret_cast<const uint32_t*>(g_data.data());
    for (uint32_t i = 0; i < kDataSize / 4; ++i)
        if (now[i] != was[i]) {
            snprintf(key, sizeof key, "D 0x%06X", kData + i * 4);
            ++g_changed[key];
        }
    for (const Copy& c : g_copies) {
        std::vector<bool> seen(c.size / 4);
        for (uint32_t i = 0; i < c.size / 4; ++i)
            if (reinterpret_cast<const uint32_t*>(c.at)[i] != reinterpret_cast<const uint32_t*>(c.before.data())[i]) {
                snprintf(key, sizeof key, "%c +0x%04X", c.kind, i * 4);
                ++g_changed[key];
            }
    }
    g_data.clear();
    if (g_frame == static_cast<int>(g_from) + kFrames - 1) {
        g_done = true;
        std::vector<std::pair<std::string, int>> list(g_changed.begin(), g_changed.end());
        std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        dslog::Write("[render-diff] %zu dword(s) written by the render loop in %d frames (%s):", list.size(), kFrames,
                     g_level);
        for (const auto& [k, n] : list) dslog::Write("[render-diff] %s %d", k.c_str(), n);
    }
}
#endif

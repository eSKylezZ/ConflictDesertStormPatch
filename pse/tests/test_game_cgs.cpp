// The Conflict: Global Storm game layer (pse/games/cgs, docs/sdk/reference/game-cgs.md): tier 1 only (the built
// pse_cgs.dll) - detection and the cgs ID database table. This exe runs almost everything inside a SecuROM VM with
// no static call sites (games/cgs/CLAUDE.md "Environment"), so the layer raises no core game events yet - no
// events/saves test here. Signatures are checked separately: `python scripts/gen_ids.py --verify-exe cgs --exe
// "<game folder>\ConflictGlobal.exe"` (0 mismatches as of 2026-10-24).
#include <windows.h>

#include <set>
#include <string>

#include "generated/pse_ids_cgs.h"
#include "ids.h"
#include "pse/pse_ids_cgs.h"
#include "pse_test.h"

namespace {
constexpr const char* kCgsSha = "D1EE014F6888B100C8806EAACB8FFC22D069B815CF48C3A60FFDA0EC538539A1";
constexpr const char* kDs1Sha = "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164";

const pse::ids::GameDef& Cgs() { return pse_ids_cgs::kGame; }

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

PSE_TEST("cgs layer: pse_cgs.dll detects ConflictGlobal.exe (RELOADED NoCD, or another file of it), the DS1 layer doesn't") {
    HMODULE m = LoadLibraryA(PSE_GAME_LAYER_CGS_DLL);
    PSE_REQUIRE(m);
    auto info = reinterpret_cast<const PSEGameLayerInfo*>(GetProcAddress(m, "PSEGame_Info"));
    auto detect = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m, "PSEGame_Detect"));
    PSE_REQUIRE(info && detect && GetProcAddress(m, "PSEGame_Init"));
    PSE_CHECK_STR(info->gameId, "cgs");
    PSE_CHECK_STR(info->plugin->name, "pse.game.cgs");
    PSE_REQUIRE(info->hostPlugins && info->hostPlugins[0]);
    PSE_CHECK_STR(info->hostPlugins[0], "cgsfix");
    PSE_CHECK(info->hostPlugins[1] == nullptr);

    PSEGameDetectInfo d{sizeof d, kCgsSha, 0x400000, 0x43032523, L"ConflictGlobal.exe"};
    PSE_CHECK_EQ(detect(&d), 1);
    const std::string lower = Lower(kCgsSha);
    d.exeSha256 = lower.c_str();
    PSE_CHECK_EQ(detect(&d), 1);
    d.exeSha256 = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCD";   // unknown hash, known time stamp
    PSE_CHECK_EQ(detect(&d), 1);
    d.exeSha256 = kDs1Sha;
    d.peTimestamp = 0x594D0000;   // Desert Storm 1's own build time stamp
    PSE_CHECK_EQ(detect(&d), 0);
    d.peTimestamp = 0x12345678;   // unknown build
    d.exeSha256 = "";
    PSE_CHECK_EQ(detect(&d), 0);
    d.peTimestamp = 0x43032523;
    d.exeSha256 = kCgsSha;
    d.imageBase = 0x10000000;   // not where the exe is mapped
    PSE_CHECK_EQ(detect(&d), 0);
    FreeLibrary(m);

    HMODULE m1 = LoadLibraryA(PSE_GAME_LAYER_DLL);   // and the DS1 layer says no to ConflictGlobal.exe
    PSE_REQUIRE(m1);
    auto detect1 = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m1, "PSEGame_Detect"));
    PSEGameDetectInfo d2{sizeof d2, kCgsSha, 0x400000, 0x43032523, L"ConflictGlobal.exe"};
    PSE_CHECK_EQ(detect1(&d2), 0);
    FreeLibrary(m1);
}

PSE_TEST("ids: cgs table - ids sorted + unique, names unique, one build, every id has an address") {
    const auto& g = Cgs();
    PSE_CHECK_STR(g.gameId, "cgs");
    PSE_REQUIRE(g.entryCount == 9u);
    std::set<std::string> names;
    for (uint32_t i = 0; i < g.entryCount; ++i) {
        const auto& e = g.entries[i];
        if (i) PSE_CHECK(e.id > g.entries[i - 1].id);
        PSE_CHECK(e.kind >= PSE_ID_KIND_FUNCTION && e.kind <= PSE_ID_KIND_DATA);
        if (!names.insert(Lower(e.name)).second) PSE_CHECK_STR(e.name, "(a name used twice)");
    }
    PSE_REQUIRE(g.buildCount == 1u);
    PSE_CHECK_STR(g.builds[0].name, "reloaded-nocd");
    PSE_CHECK_EQ(g.builds[0].timestamp, 0x43032523u);
    PSE_CHECK_EQ(g.builds[0].sizeOfImage, 0x00F9D000u);
    PSE_CHECK_EQ(g.builds[0].addressCount, g.entryCount);
    PSE_CHECK_EQ(PSE_ID_cgs_ModeFilter_CallTable, 1u);
    PSE_CHECK_EQ(PSE_ID_cgs_Renderer, 5u);
    PSE_CHECK_EQ(PSE_ID_cgs_Main_ActiveFlag2, 9u);
}

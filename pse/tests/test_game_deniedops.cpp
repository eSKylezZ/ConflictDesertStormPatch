// The Conflict: Denied Ops game layer (pse/games/deniedops, docs/sdk/reference/game-deniedops.md): tier 1 only (the
// built pse_deniedops.dll) - detection and the deniedops ID database table. The exe isn't mapped into this process:
// its .text is SteamStub-encrypted in the file, so a mapped image would only show encrypted code. Signatures are
// checked separately against a copy with the decrypted .text: `python scripts/gen_ids.py --verify-exe deniedops --exe
// <copy>` (pse/ids/deniedops.ids.txt header).
#include <windows.h>

#include <set>
#include <string>

#include "generated/pse_ids_deniedops.h"
#include "ids.h"
#include "pse/pse_ids_deniedops.h"
#include "pse_test.h"

namespace {
constexpr const char* kDoSha = "104AA5D5A4245F02ED96AA2C370E053A7736EDC49F5F6FA67846D73BECB64838";
constexpr const char* kDs1Sha = "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164";

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

PSE_TEST("deniedops layer: pse_deniedops.dll detects ConflictDeniedOps.exe (Jan 2008 build), the DS1 layer doesn't") {
    HMODULE m = LoadLibraryA(PSE_GAME_LAYER_DENIEDOPS_DLL);
    PSE_REQUIRE(m);
    auto info = reinterpret_cast<const PSEGameLayerInfo*>(GetProcAddress(m, "PSEGame_Info"));
    auto detect = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m, "PSEGame_Detect"));
    PSE_REQUIRE(info && detect && GetProcAddress(m, "PSEGame_Init"));
    PSE_CHECK_STR(info->gameId, "deniedops");
    PSE_CHECK_STR(info->plugin->name, "pse.game.deniedops");
    PSE_REQUIRE(info->hostPlugins && info->hostPlugins[0]);
    PSE_CHECK_STR(info->hostPlugins[0], "deniedopsfix");
    PSE_CHECK(info->hostPlugins[1] == nullptr);

    PSEGameDetectInfo d{sizeof d, kDoSha, 0x400000, 0x478652D0, L"ConflictDeniedOps.exe"};
    PSE_CHECK_EQ(detect(&d), 1);
    const std::string lower = Lower(kDoSha);
    d.exeSha256 = lower.c_str();
    PSE_CHECK_EQ(detect(&d), 1);
    d.exeSha256 = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCD";   // unknown hash, known time stamp
    PSE_CHECK_EQ(detect(&d), 1);
    d.exeSha256 = kDs1Sha;
    d.peTimestamp = 0x594D0000;   // Desert Storm 1's build
    PSE_CHECK_EQ(detect(&d), 0);
    d.exeSha256 = kDoSha;
    d.peTimestamp = 0x478652D0;
    d.imageBase = 0x10000000;   // not where the exe is mapped
    PSE_CHECK_EQ(detect(&d), 0);
    FreeLibrary(m);

    HMODULE m1 = LoadLibraryA(PSE_GAME_LAYER_DLL);   // and the DS1 layer says no to Denied Ops
    PSE_REQUIRE(m1);
    auto detect1 = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m1, "PSEGame_Detect"));
    PSEGameDetectInfo d2{sizeof d2, kDoSha, 0x400000, 0x478652D0, L"ConflictDeniedOps.exe"};
    PSE_CHECK_EQ(detect1(&d2), 0);
    FreeLibrary(m1);
}

PSE_TEST("ids: deniedops table - ids sorted + unique, names unique, one build, every id has an address") {
    const auto& g = pse_ids_deniedops::kGame;
    PSE_CHECK_STR(g.gameId, "deniedops");
    PSE_REQUIRE(g.entryCount == 17u);
    std::set<std::string> names;
    for (uint32_t i = 0; i < g.entryCount; ++i) {
        const auto& e = g.entries[i];
        if (i) PSE_CHECK(e.id > g.entries[i - 1].id);
        PSE_CHECK(e.kind >= PSE_ID_KIND_FUNCTION && e.kind <= PSE_ID_KIND_DATA);
        if (!names.insert(Lower(e.name)).second) PSE_CHECK_STR(e.name, "(a name used twice)");
    }
    PSE_REQUIRE(g.buildCount == 1u);
    PSE_CHECK_STR(g.builds[0].name, "nodisc-1.0");
    PSE_CHECK_EQ(g.builds[0].timestamp, 0x478652D0u);
    PSE_CHECK_EQ(g.builds[0].sizeOfImage, 0x00B4F000u);
    PSE_CHECK_EQ(g.builds[0].addressCount, g.entryCount);
    PSE_CHECK_EQ(PSE_ID_deniedops_File_Open, 1u);
    PSE_CHECK_EQ(PSE_ID_deniedops_Iat_SetAppType, 14u);
    PSE_CHECK_EQ(PSE_ID_deniedops_Mode_Table, 17u);
}

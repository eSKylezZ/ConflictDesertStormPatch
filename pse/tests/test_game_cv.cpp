// The Conflict: Vietnam game layer (pse/games/cv, docs/sdk/reference/game-cv.md): tier 1 only (the built pse_cv.dll) -
// detection and the cv ID database table. Unlike DS1/DS2/TGE the CV layer raises no core game events yet (no verified
// function entry / prologue for the frame loop, render call, level load or window shutdown - see pse_game_cv.h), so
// there is no events/saves test here. Signatures are checked separately: `python scripts/gen_ids.py --verify-exe cv`
// against the real Vietnam.exe (not mapped into this test process).
#include <windows.h>

#include <set>
#include <string>

#include "generated/pse_ids_cv.h"
#include "ids.h"
#include "pse/pse_ids_cv.h"
#include "pse_test.h"

namespace {
constexpr const char* kCvSha = "636576CA842066A56660C1DAE615FBA4DB9726AEA1EF851EE5FD4C6D8CE3710C";
constexpr const char* kDs1Sha = "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164";

const pse::ids::GameDef& Cv() { return pse_ids_cv::kGame; }

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

PSE_TEST("cv layer: pse_cv.dll detects Vietnam.exe (Sep 2004 build, or another file of it), the DS1 layer doesn't") {
    HMODULE m = LoadLibraryA(PSE_GAME_LAYER_CV_DLL);
    PSE_REQUIRE(m);
    auto info = reinterpret_cast<const PSEGameLayerInfo*>(GetProcAddress(m, "PSEGame_Info"));
    auto detect = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m, "PSEGame_Detect"));
    PSE_REQUIRE(info && detect && GetProcAddress(m, "PSEGame_Init"));
    PSE_CHECK_STR(info->gameId, "cv");
    PSE_CHECK_STR(info->plugin->name, "pse.game.cv");
    PSE_REQUIRE(info->hostPlugins && info->hostPlugins[0]);
    PSE_CHECK_STR(info->hostPlugins[0], "cvfix");
    PSE_CHECK(info->hostPlugins[1] == nullptr);

    PSEGameDetectInfo d{sizeof d, kCvSha, 0x400000, 0x414ECA49, L"Vietnam.exe"};
    PSE_CHECK_EQ(detect(&d), 1);
    const std::string lower = Lower(kCvSha);
    d.exeSha256 = lower.c_str();
    PSE_CHECK_EQ(detect(&d), 1);
    d.exeSha256 = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCD";   // unknown hash, known time stamp
    PSE_CHECK_EQ(detect(&d), 1);
    d.exeSha256 = kDs1Sha;
    d.peTimestamp = 0x594D0000;   // Desert Storm 1's own build time stamp
    PSE_CHECK_EQ(detect(&d), 0);
    d.peTimestamp = 0x411BBEAF;   // Vietnam retail / "1.0 Fixed" (a DIFFERENT build - earlier time stamp)
    d.exeSha256 = "";
    PSE_CHECK_EQ(detect(&d), 0);
    d.peTimestamp = 0x414ECA49;
    d.exeSha256 = kCvSha;
    d.imageBase = 0x10000000;   // not where the exe is mapped
    PSE_CHECK_EQ(detect(&d), 0);
    FreeLibrary(m);

    HMODULE m1 = LoadLibraryA(PSE_GAME_LAYER_DLL);   // and the DS1 layer says no to Vietnam.exe
    PSE_REQUIRE(m1);
    auto detect1 = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m1, "PSEGame_Detect"));
    PSEGameDetectInfo d2{sizeof d2, kCvSha, 0x400000, 0x414ECA49, L"Vietnam.exe"};
    PSE_CHECK_EQ(detect1(&d2), 0);
    FreeLibrary(m1);
}

PSE_TEST("ids: cv table - ids sorted + unique, names unique, one build, every id has an address") {
    const auto& g = Cv();
    PSE_CHECK_STR(g.gameId, "cv");
    PSE_REQUIRE(g.entryCount == 5u);
    std::set<std::string> names;
    for (uint32_t i = 0; i < g.entryCount; ++i) {
        const auto& e = g.entries[i];
        if (i) PSE_CHECK(e.id > g.entries[i - 1].id);
        PSE_CHECK(e.kind >= PSE_ID_KIND_FUNCTION && e.kind <= PSE_ID_KIND_DATA);
        if (!names.insert(Lower(e.name)).second) PSE_CHECK_STR(e.name, "(a name used twice)");
    }
    PSE_REQUIRE(g.buildCount == 1u);
    PSE_CHECK_STR(g.builds[0].name, "nocd-200409");
    PSE_CHECK_EQ(g.builds[0].timestamp, 0x414ECA49u);
    PSE_CHECK_EQ(g.builds[0].sizeOfImage, 0x0053BD70u);
    PSE_CHECK_EQ(g.builds[0].addressCount, g.entryCount);
    PSE_CHECK_EQ(PSE_ID_cv_Launcher_ModeFilter, 1u);
    PSE_CHECK_EQ(PSE_ID_cv_File_Resolve, 2u);
    PSE_CHECK_EQ(PSE_ID_cv_File_Flags, 5u);
}

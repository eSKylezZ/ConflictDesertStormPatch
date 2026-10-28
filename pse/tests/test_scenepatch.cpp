// Several mods' .envpatch / .rmdpatch of one file merged (pse/records/scenepatch.h) - made-up patches, no game files.
#include "scenepatch.h"
#include "pse_test.h"

using namespace pse;

PSE_TEST("scene patches: mods' line changes merged, the later mod wins an overlap") {
    const std::vector<records::PatchText> patches = {
        {"A", "base S.ENV\n@ 2 1 1\nA2\n"},                 // line 3 changed
        {"B", "base S.ENV\n@ 5 0 1\nNEW_B\n"},              // an object added before line 6
        {"C", "base S.ENV\r\n@ 5 0 1\r\nNEW_C\r\n@ 8 2 0\r\n"},   // another added there + lines 9-10 removed
        {"D", "base S.ENV\n@ 2 1 1\nD2\n"},                 // line 3 again: D wins
        {"E", "base S.ENV\n@ 5 0 1\nNEW_B\n"},              // the same as B: no conflict, kept once
        {"F", "no base line\n"},
    };
    std::string merged;
    std::vector<std::string> report;
    PSE_REQUIRE(records::MergeEnvPatches("S.ENV", patches, &merged, &report));
    PSE_CHECK_EQ(merged, std::string("base S.ENV\n@ 2 1 1\nD2\n@ 5 0 1\nNEW_B\n@ 5 0 1\nNEW_C\n@ 8 2 0\n"));
    PSE_REQUIRE(report.size() == 2);
    PSE_CHECK_EQ(report[0], std::string("[fail] F: S.ENV patch not used - no \"base\" line"));
    PSE_CHECK_EQ(report[1], std::string("conflict: S.ENV lines 3-3: A / D (D wins)"));
}

PSE_TEST("scene patches: an insert inside another mod's change, a different base, out-of-order hunks") {
    std::string merged;
    std::vector<std::string> report;
    PSE_REQUIRE(records::MergeEnvPatches("S.ENV", {{"A", "base S.ENV\n@ 2 4 0\n"}, {"B", "base S.ENV\n@ 3 0 1\nX\n"}},
                                         &merged, &report));
    PSE_CHECK_EQ(merged, std::string("base S.ENV\n@ 3 0 1\nX\n"));
    PSE_REQUIRE(report.size() == 1);
    PSE_CHECK_EQ(report[0], std::string("conflict: S.ENV at line 4: A / B (B wins)"));
    report.clear();
    PSE_REQUIRE(records::MergeEnvPatches("S.ENV", {{"A", "base T.ENV\n@ 1 1 1\nA\n"}, {"B", "base S.ENV\n@ 2 1 1\nB\n"},
                                                   {"C", "base S.ENV\n@ 4 0 1\nX\n@ 1 0 1\nY\n"}},
                                         &merged, &report));
    PSE_CHECK_EQ(merged, std::string("base S.ENV\n@ 2 1 1\nB\n"));
    PSE_REQUIRE(report.size() == 2);
    PSE_CHECK_EQ(report[0], std::string("[fail] C: S.ENV patch not used - hunks out of order at @ 1 0 1"));
    PSE_CHECK_EQ(report[1], std::string("conflict: S.ENV: A's patch is of T.ENV, B's of S.ENV (B wins)"));
    report.clear();
    PSE_CHECK(!records::MergeEnvPatches("S.ENV", {{"A", "nothing"}}, &merged, &report));
    PSE_CHECK(merged.empty());
}

PSE_TEST("scene patches: route markers merged per marker, the largest count") {
    const std::vector<records::PatchText> patches = {
        {"A", "base L.RMD\ncount 10\n3 1 2 3 0 65535 65535 65535 65535 0 1\n5 1 1 1 0 1 65535 65535 65535 0 1\n"},
        {"B", "base L.RMD\ncount 12\n5 9 9 9 0 1 65535 65535 65535 0 1\n7 2 2 2 6 65535 65535 65535 65535 0 1\n"},
        {"C", "base L.RMD\ncount 8\n3 1 2 3 0 65535 65535 65535 65535 0 1\n"},   // marker 3 as A has it
    };
    std::string merged;
    std::vector<std::string> report;
    PSE_REQUIRE(records::MergeRmdPatches("L.RMD", patches, &merged, &report));
    PSE_CHECK_EQ(merged, std::string("base L.RMD\ncount 12\n3 1 2 3 0 65535 65535 65535 65535 0 1\n"
                                     "5 9 9 9 0 1 65535 65535 65535 0 1\n7 2 2 2 6 65535 65535 65535 65535 0 1\n"));
    PSE_REQUIRE(report.size() == 1);
    PSE_CHECK_EQ(report[0], std::string("conflict: L.RMD marker 5: A / B (B wins)"));
}

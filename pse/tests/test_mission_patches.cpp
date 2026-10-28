// Mission patches (docs/sdk/reference/mission-patches.md, pse/build/mission2lua/m2r_patch.cpp): the header, edit blocks,
// matching exactly once, all-or-nothing, the mission's environment handed out before start.lua. Tier 1: made-up text.
#include <string>
#include <vector>

#include "mission2lua.h"
#include "pse_test.h"

using namespace pse::m2l;

namespace {

const char* kBundle =
    "--@archive mission1\n"
    "local MISSION = (function()\n"
    "end)()\n"
    "function f_1000()\n"
    "  api.give_item(obj.HERO_FOLEY, obj.FOLEY_AMMO)\n"
    "  g_1 = 2\n"
    "end\n"
    "function f_2000()\n"
    "  g_1 = 2\n"
    "end\n"
    "-- ======================== scripts/start.lua ========================\n"
    "_G.init = function() end\n";

MissionPatch Patch(const std::string& text) {
    MissionPatch p;
    p.label = "Test\\Mission1.patch.lua";
    std::vector<uint8_t> raw(text.begin(), text.end());
    p.text = NormalizePatchText(raw);
    return p;
}

}  // namespace

PSE_TEST("mission patches: an edit block applies where its lines match once") {
    MissionPatch p = Patch(
        "--@patches mission1.DLL\r\n--@readable 1\r\n\r\n--[[@edit\r\n  api.give_item(obj.HERO_FOLEY, obj.FOLEY_AMMO)\r\n"
        "  g_1 = 2\r\n--@to\r\n  api.give_item(obj.HERO_FOLEY, obj.FOLEY_AMMO)\r\n  mine()\r\n  g_1 = 2\r\n]]\r\n"
        "function mine() end\r\n");
    std::string why;
    PSE_REQUIRE(ParseMissionPatch("Mission1.dll", &p, &why));
    PSE_CHECK_EQ(p.edits.size(), 1u);
    PSE_CHECK_EQ(p.edits[0].line, 4);
    PSE_CHECK_EQ(p.edits[0].from.size(), 2u);
    PSE_CHECK_EQ(p.edits[0].to.size(), 3u);
    std::string b = kBundle;
    PSE_REQUIRE(ApplyMissionPatchEdits(&b, p, &why));
    PSE_CHECK(b.find("  api.give_item(obj.HERO_FOLEY, obj.FOLEY_AMMO)\n  mine()\n  g_1 = 2\nend\nfunction f_2000()") !=
              std::string::npos);
    PSE_CHECK(p.text.find('\r') == std::string::npos);   // CRLF made LF
}

PSE_TEST("mission patches: a block matching 0 or 2 places refuses the whole patch, the mission is untouched") {
    MissionPatch p = Patch(
        "--@patches Mission1.dll\n--@readable 1\n--[[@edit\n  api.give_item(obj.HERO_FOLEY, obj.FOLEY_AMMO)\n--@to\n"
        "  -- (gone)\n]]\n--[[@edit\n  g_1 = 2\n--@to\n  g_1 = 3\n]]\n");
    std::string why;
    PSE_REQUIRE(ParseMissionPatch("Mission1.dll", &p, &why));
    std::string b = kBundle;
    PSE_CHECK(!ApplyMissionPatchEdits(&b, p, &why));   // `  g_1 = 2` is in both functions
    PSE_CHECK(why.find("line 8") != std::string::npos && why.find("matches 2 places") != std::string::npos);
    PSE_CHECK(b == kBundle);   // the first block's change was not kept
    MissionPatch q = Patch("--@patches Mission1.dll\n--@readable 1\n--[[@edit\n  nowhere()\n--@to\n]]\n");
    PSE_REQUIRE(ParseMissionPatch("Mission1.dll", &q, &why));
    PSE_CHECK(!ApplyMissionPatchEdits(&b, q, &why));
    PSE_CHECK(why.find("matches 0 places") != std::string::npos);
}

PSE_TEST("mission patches: an empty --@to deletes, a long-bracket level allows ]] in the lines") {
    MissionPatch p = Patch(
        "--@patches Mission1.dll\n--@readable 1\n--[==[@edit\nfunction f_2000()\n  g_1 = 2\nend\n--@to\n"
        "function f_2000()\n  local t = {a = {}}; t.a[1] = t[\"a\"]\nend\n]==]\n--[[@edit\n_G.init = function() end\n--@to\n]]\n");
    std::string why;
    PSE_REQUIRE(ParseMissionPatch("Mission1.dll", &p, &why));
    PSE_CHECK_EQ(p.edits.size(), 2u);
    PSE_CHECK_EQ(p.edits[1].to.size(), 0u);
    std::string b = kBundle;
    PSE_REQUIRE(ApplyMissionPatchEdits(&b, p, &why));
    PSE_CHECK(b.find("t.a[1] = t[\"a\"]") != std::string::npos);
    PSE_CHECK(b.find("_G.init") == std::string::npos);
}

PSE_TEST("mission patches: a patch for another DLL, another version or with a broken block is refused") {
    std::string why;
    MissionPatch a = Patch("--@patches Mission2.dll\n--@readable 1\n");
    PSE_CHECK(!ParseMissionPatch("Mission1.dll", &a, &why));
    PSE_CHECK(why.find("--@patches Mission1.dll") != std::string::npos);
    MissionPatch b = Patch("--@patches Mission1.dll\n--@readable 2\n");
    PSE_CHECK(!ParseMissionPatch("Mission1.dll", &b, &why));
    PSE_CHECK(why.find("version 2") != std::string::npos);
    MissionPatch c = Patch("--@patches Mission1.dll\n");
    PSE_CHECK(!ParseMissionPatch("Mission1.dll", &c, &why));
    MissionPatch d = Patch("--@patches Mission1.dll\n--@readable 1\n--[[@edit\n  g_1 = 2\n");
    PSE_CHECK(!ParseMissionPatch("Mission1.dll", &d, &why));
    PSE_CHECK(why.find("isn't closed") != std::string::npos);
    MissionPatch e = Patch("--@patches Mission1.dll\n--@readable 1\n--[[@edit\n  g_1 = 2\n]]\n");
    PSE_CHECK(!ParseMissionPatch("Mission1.dll", &e, &why));
    PSE_CHECK(why.find("no --@to") != std::string::npos);
    MissionPatch f = Patch("--@patches Mission1.dll\n--@readable 1\n--[[@edit\n--@to\n  x()\n]]\n");
    PSE_CHECK(!ParseMissionPatch("Mission1.dll", &f, &why));
    PSE_CHECK(why.find("nothing before --@to") != std::string::npos);
    // the header ends at the first line that isn't a comment
    MissionPatch g = Patch("--@readable 1\nlocal x = 1\n--@patches Mission1.dll\n");
    PSE_CHECK(!ParseMissionPatch("Mission1.dll", &g, &why));
    // a BOM is not part of the header
    MissionPatch h = Patch("\xEF\xBB\xBF--@patches Mission1.dll\n--@readable 1\n");
    PSE_CHECK(ParseMissionPatch("Mission1.dll", &h, &why));
}

PSE_TEST("mission patches: the mission's environment is handed out right before start.lua") {
    std::string b = kBundle;
    PSE_REQUIRE(HandOutMissionEnv(&b));
    PSE_CHECK(b.find("_G.__pse_mission_env = _ENV") < b.find("-- ======================== scripts/start.lua"));
    std::string none = "return 1\n";
    PSE_CHECK(!HandOutMissionEnv(&none));
    PSE_CHECK(std::string(MissionPatchPrelude()).find("TEXT = setmetatable") != std::string::npos);
}

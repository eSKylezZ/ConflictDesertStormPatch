// Placed references (pse/records/placed.h) on a small made-up scene - no game files needed. The real scenes are
// checked against the ModKit's Python by scripts/records_placed_gate.py.
#include "data.h"
#include "placed.h"
#include "pse_test.h"
#include "scenepatch.h"

using namespace pse;

namespace {
// a Desert Storm HLOI (scene version 72): model, zone, texture, pos, rot, 3 ints, unit, int, script id, stat, int,
// in play, then the AI / kit / link fields (all empty here)
std::string Hloi(const char* unit, int sid, const char* stat, const char* pos) {
    return std::string("HLOI 1 \"MODEL\" \"ZONE1\" \"NOTEXTURE\"\t ") + pos + "\t 0.00 90.00 0.00\t 1 0 3 \"" + unit +
           "\" 1 " + std::to_string(sid) + " \"" + stat + "\" -1 1 0 0 0 0 0 0 0 1.0 1.0 1.0 1.0 1.0 3 1.0 0 0 0 0 0 " +
           "\"NONE\" 0 0 0.00 0.00 0 0 0.00 0.00 0.0 0.0 0.0 0.0 0.0 0.0 0 0 0 0 ";
}
std::string Scene() {
    return "Version 72\r\n" + Hloi("Unit_A", 4, "GUARD_A", "10.00 20.00 30.00") + "\r\n" +
           Hloi("IR_VEH_BMP1", 5, "12", "-5.50 0.00 7.25") + "\r\nHLOX\r\n" +
           "NEWENVWAYPT 1 0 \"PATH_A\" 1.000000 2.000000 3.000000 0.000000 0.000000 0.000000 5000 2 \r\n"
           "NEWENVWAYPT 2 0 \"PATH_B\" 4.000000 5.000000 6.000000 0.000000 0.000000 0.000000 0 -1 \r\n"
           "ENDWAYPTS\r\n1\r\nREGN \"TRIG\" 1 1.0 2.0 3.0 50.0\r\nENDREGIONS\r\n";
}
records::Edits Edits(const char* mod, const char* text) {
    records::Edits e;
    e.mod = mod;
    e.file = "e.lua";
    std::string err;
    PSE_REQUIRE(data::Parse(text, &e.doc, &err));
    return e;
}
}  // namespace

PSE_TEST("records: placed edits -> the level's scene, token by token") {
    std::vector<records::Edits> edits;
    edits.push_back(Edits("M", R"(return {
  { id = "ds1:test/object/guard_a", pos = { x = 110, y = 20.5, z = 30 }, scene = { model = "OTHER" } },
  { id = "ds1:test/object/new_guy", based_on = "ds1:test/object/guard_a" },
  { id = "ds1:test/object/bmp1_5", delete = true },
  { id = "ds1:test/waypoint/path_b", next = "ds1:test/waypoint/path_a", wait_ms = 1234 },
  { id = "ds1:Test/region/trig", radius = 75.5 },
  { id = "ds1:weapon/m16", damage = 3 },
  { id = "ds1:other/object/x", pos = { x = 1, y = 1, z = 1 } },
})"));
    const std::vector<std::string> levels = records::PlacedLevels(edits);
    PSE_REQUIRE(levels.size() == 2);
    PSE_CHECK_EQ(levels[0], std::string("other"));
    PSE_CHECK_EQ(levels[1], std::string("test"));   // "Test" folded in: the level part ignores case
    std::vector<std::string> report;
    const std::string out = records::ApplyPlaced(Scene(), "test", edits, {}, &report);
    PSE_REQUIRE(report.size() == 2);
    PSE_CHECK_EQ(report[0], std::string("new ds1:test/object/new_guy based on ds1:test/object/guard_a, script id 6 (M)"));
    PSE_CHECK_EQ(report[1], std::string("ds1:test/object/bmp1_5 taken out of play (M)"));
    std::string want = Scene();
    auto replace = [&](const std::string& a, const std::string& b) {
        const size_t at = want.find(a);
        PSE_REQUIRE(at != std::string::npos);
        want.replace(at, a.size(), b);
    };
    // the copy: the guard's line as the scene had it, at the end of the HLOI section, new script id + its own stat
    std::string copy = Hloi("Unit_A", 6, "NEW_GUY", "10.00 20.00 30.00");
    replace("HLOX\r\n", copy + "\r\nHLOX\r\n");
    replace(Hloi("Unit_A", 4, "GUARD_A", "10.00 20.00 30.00"), Hloi("Unit_A", 4, "GUARD_A", "110.00 20.50 30.00"));
    replace("\"MODEL\" \"ZONE1\" \"NOTEXTURE\"\t 110.00", "\"OTHER\" \"ZONE1\" \"NOTEXTURE\"\t 110.00");
    replace("\"12\" -1 1 0", "\"12\" -1 0 0");   // bmp1_5 (no stat name: unit + script id) out of play
    replace("0.000000 0 -1 ", "0.000000 1234 1 ");
    replace("1 1.0 2.0 3.0 50.0", "1 1.0 2.0 3.0 75.5");
    PSE_CHECK_EQ(out, want);
}

PSE_TEST("records: placed edits refused with ModKit's report lines") {
    std::vector<records::Edits> edits;
    edits.push_back(Edits("Bad", R"(return {
  { id = "ds1:test/object/guard_a", pos = 5 },
  { id = "ds1:test/object/guard_a", scene = { nope = 1 } },
  { id = "ds1:test/object/guard_a", in_play = 1, script_id = 99, unit = 7 },
  { id = "ds1:test/object/guard_a", base = "ds1:unit/nobody" },
  { id = "ds1:test/object/guard_a", model = 'say "hi"' },
  { id = "ds1:test/waypoint/path_a", delete = true },
  { id = "ds1:test/waypoint/path_a", next = "ds1:test/waypoint/nowhere" },
  { id = "ds1:test/region/trig", radius = { x = 1 } },
  { id = "ds1:test/marker/m", pos = { x = 1, y = 1, z = 1 } },
  { id = "ds1:test/object/ghost", pos = { x = 1, y = 1, z = 1 } },
})"));
    std::vector<std::string> report;
    const std::string out = records::ApplyPlaced(Scene(), "test", edits, {}, &report);
    const std::vector<std::string> want = {
        "[fail] Bad/e.lua: ds1:test/object/guard_a pos can't be set: pos takes { x = ..., y = ..., z = ... }",
        "[fail] Bad/e.lua: ds1:test/object/guard_a scene can't be set: \"HLOI has no field 'nope'\"",
        "[fail] Bad/e.lua: ds1:test/object/guard_a base ds1:unit/nobody isn't a record",
        "[fail] Bad/e.lua: ds1:test/object/guard_a model can't be set: string can't contain quotes or newlines: 'say \"hi\"'",
        "[fail] Bad/e.lua: ds1:test/waypoint/path_a - only objects can be deleted (set it elsewhere instead)",
        "[fail] Bad/e.lua: ds1:test/waypoint/path_a next ds1:test/waypoint/nowhere isn't a waypoint of test",
        "[fail] Bad/e.lua: ds1:test/region/trig radius can't be set: radius takes a value, not a table",
        "[fail] Bad/e.lua: ds1:test/marker/m - no placed kind marker",
        "[fail] Bad/e.lua: ds1:test/object/ghost isn't in test's scene - a new one needs based_on = <an object of this level>",
    };
    PSE_CHECK_EQ(report.size(), want.size());
    for (size_t i = 0; i < report.size() && i < want.size(); ++i) PSE_CHECK_EQ(report[i], want[i]);
    // in_play = 1 and unit = 7 ("7" quoted) went through; script_id is never set by an edit
    std::string expect = Scene();
    const std::string a = "\"Unit_A\" 1 4";
    expect.replace(expect.find(a), a.size(), "\"7\" 1 4");
    PSE_CHECK_EQ(out, expect);
}

PSE_TEST("records: a scene's object script ids (mods.cpp checks two mods' added objects against them)") {
    std::string scene = Scene();
    scene.insert(scene.find("HLOX"), Hloi("Unit_A", 5, "TWIN", "0.00 0.00 0.00") + "\r\n");
    const std::vector<int64_t> ids = records::SceneScriptIds(scene);
    PSE_REQUIRE(ids.size() == 3);
    PSE_CHECK(ids[0] == 4 && ids[1] == 5 && ids[2] == 5);
}

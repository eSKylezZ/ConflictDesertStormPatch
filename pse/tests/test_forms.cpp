// Records (pse/records/forms.h) on a small made-up schema + tables - no game files needed. The real tables are checked
// against the ModKit's Python by scripts/records_forms_gate.py.
#include "data.h"
#include "forms.h"
#include "pse_test.h"

using namespace pse;

namespace {
const char* kSchema = R"(return {
  tables = {
    {
      file = "GUNS.TXT",
      type = "weapon",
      columns = {
        { name = "key", kind = "text" },
        { name = "model", kind = "text", none = "-1" },
        { name = "damage", kind = "int" },
        { name = "range", kind = "fixed2" },
        { name = "silenced", kind = "yesno" },
      },
    },
    {
      file = "KITS.TXT",
      type = "unit_kit",
      rows_per_key = true,
      columns = {
        { name = "soldier", kind = "text" },
        { name = "item", kind = "ref", ref = { "weapon" } },
        { name = "amount", kind = "int" },
      },
    },
  },
})";
const std::string kGuns = "M16,Rifle01,14,850.00,NO\r\nAK47,-1,12,780.00,YES\r\nODD,x,1.5,7.1,maybe\r\n";
const std::string kKits = "Grunt,M16,2\r\nSniper,AK47,1\r\nGrunt,AK47,1\r\n";

data::Document Doc(const char* text) {
    data::Document d;
    std::string e;
    PSE_REQUIRE(data::Parse(text, &d, &e));
    return d;
}
}  // namespace

PSE_TEST("records: tables -> records -> tables, byte for byte") {
    const data::Document schema = Doc(kSchema);
    records::TableFiles in = {{"GUNS.TXT", kGuns}, {"KITS.TXT", kKits}};
    records::Forms forms;
    std::string e;
    PSE_REQUIRE(records::Generate(in, schema.root, &forms, &e));
    const data::Value* m16 = forms["weapon"].root.Get("records")->entries[0].value.get();
    PSE_CHECK_EQ(m16->Get("id")->s, std::string("ds1:weapon/m16"));
    PSE_CHECK_EQ(m16->Get("damage")->i, int64_t(14));
    PSE_CHECK(m16->Get("range")->f == 850.0);
    PSE_CHECK(m16->Get("silenced")->kind == data::Kind::Bool && !m16->Get("silenced")->b);
    const data::Value* ak = forms["weapon"].root.Get("records")->entries[1].value.get();
    PSE_CHECK(ak->Get("model") == nullptr);   // "-1" = none: left out
    const data::Value* odd = forms["weapon"].root.Get("records")->entries[2].value.get();
    PSE_CHECK(odd->Get("damage")->kind == data::Kind::String);   // "1.5" in an int column stays text
    PSE_CHECK(odd->Get("range")->kind == data::Kind::String);    // "7.1" isn't fixed2
    // kits: one record per key, rows not together in the file -> by line
    const data::Value* grunt = forms["unit_kit"].root.Get("records")->entries[0].value.get();
    PSE_CHECK_EQ(grunt->Get("rows")->entries.size(), size_t(2));
    PSE_CHECK_EQ(grunt->Get("rows")->entries[0].value->Get("item")->s, std::string("ds1:weapon/m16"));
    // through text and back
    for (auto& [type, doc] : forms) {
        std::string text;
        PSE_REQUIRE(data::Write(doc, &text, &e));
        PSE_REQUIRE(data::Parse(text, &doc, &e));
    }
    const records::TableFiles back = records::Tables(forms, schema.root);
    PSE_CHECK_EQ(back.at("GUNS.TXT"), kGuns);
    PSE_CHECK_EQ(back.at("KITS.TXT"), kKits);
}

PSE_TEST("records: mods merge field by field; conflicts and problems reported") {
    const data::Document schema = Doc(kSchema);
    records::Forms forms;
    std::string e;
    PSE_REQUIRE(records::Generate({{"GUNS.TXT", kGuns}, {"KITS.TXT", kKits}}, schema.root, &forms, &e));
    std::vector<records::Edits> edits(2);
    edits[0].mod = "A";
    edits[0].file = "a.lua";
    edits[0].doc = Doc(R"(return { { id = "ds1:weapon/m16", damage = 20 }, { id = "ds1:weapon/ak47", range = 1.0 } })");
    edits[1].mod = "B";
    edits[1].file = "b.lua";
    edits[1].doc = Doc(R"(return {
      { id = "ds1:weapon/m16", damage = 30, silenced = true },
      { id = "ds1:weapon/new_gun", based_on = "ds1:weapon/m16" },
      { id = "ds1:weapon/ak47", clear = { "model", "damage" } },
      { id = "ds1:weapon/odd", delete = true },
      { id = "ds1:unit_kit/elite", based_on = "ds1:unit_kit/grunt" },
      { id = "ds1:weapon/ghost", damage = 1 },
    })");
    std::vector<std::string> report;
    records::Merge(&forms, edits, schema.root, &report);
    const std::vector<std::string> want = {
        "conflict: ds1:weapon/m16 damage: A / B (B wins)",
        "new ds1:weapon/new_gun based on ds1:weapon/m16 (B)",
        "[fail] B/b.lua: ds1:weapon/ak47 damage can't be cleared - the game has no \"none\" for that field",
        "deleted ds1:weapon/odd (B)",
        "new ds1:unit_kit/elite based on ds1:unit_kit/grunt (B)",
        "[fail] B/b.lua: ds1:weapon/ghost isn't a game record - a new one needs based_on = <a record id>",
    };
    PSE_CHECK_EQ(report.size(), want.size());
    for (size_t i = 0; i < want.size() && i < report.size(); ++i) PSE_CHECK_EQ(report[i], want[i]);
    const records::TableFiles out = records::Tables(forms, schema.root);
    PSE_CHECK_EQ(out.at("GUNS.TXT"), std::string("M16,Rifle01,30,850.00,YES\r\nAK47,-1,12,1.00,YES\r\n"
                                                  "NEW_GUN,Rifle01,30,850.00,YES\r\n"));
    // no key given: the id's name in upper case; the copied rows keep the lines they were copied from
    PSE_CHECK_EQ(out.at("KITS.TXT"), std::string("Grunt,M16,2\r\nELITE,M16,2\r\nSniper,AK47,1\r\nGrunt,AK47,1\r\n"
                                                  "ELITE,AK47,1\r\n"));
}

PSE_TEST("records: limits - field / key longer than max_len, table full (max_rows)") {
    std::string schemaText = kSchema;
    const auto put = [&](const std::string& at, const std::string& add) {
        const size_t p = schemaText.find(at);
        PSE_REQUIRE(p != std::string::npos);
        schemaText.insert(p + at.size(), add);
    };
    put("{ name = \"key\", kind = \"text\"", ", max_len = 6");
    put("{ name = \"model\", kind = \"text\", none = \"-1\"", ", max_len = 8");
    put("type = \"weapon\",", " max_rows = 4,");
    const data::Document schema = Doc(schemaText.c_str());
    records::Forms forms;
    std::string e;
    PSE_REQUIRE(records::Generate({{"GUNS.TXT", kGuns}, {"KITS.TXT", kKits}}, schema.root, &forms, &e));
    std::vector<records::Edits> edits(1);
    edits[0].mod = "L";
    edits[0].file = "l.lua";
    edits[0].doc = Doc(R"(return {
      { id = "ds1:weapon/m16", model = "Rifle01_long", damage = 21 },
      { id = "ds1:weapon/far_too_long", based_on = "ds1:weapon/m16", damage = 5 },
      { id = "ds1:weapon/g2", based_on = "ds1:weapon/m16", key = "NAME_TOO_LONG" },
      { id = "ds1:weapon/g3", based_on = "ds1:weapon/m16" },
      { id = "ds1:weapon/g4", based_on = "ds1:weapon/m16" },
    })");
    std::vector<std::string> report;
    records::Merge(&forms, edits, schema.root, &report);
    const std::vector<std::string> want = {
        "[fail] L/l.lua: ds1:weapon/m16 model is 12 characters - the game reads at most 8; not changed",
        "[fail] L/l.lua: ds1:weapon/far_too_long not added - its key is 12 characters, the game reads at most 6",
        "[fail] L/l.lua: ds1:weapon/g2 not added - its key is 13 characters, the game reads at most 6",
        "new ds1:weapon/g3 based on ds1:weapon/m16 (L)",
        "[fail] L/l.lua: ds1:weapon/g4 not added - GUNS.TXT holds at most 4 records",
    };
    PSE_CHECK_EQ(report.size(), want.size());
    for (size_t i = 0; i < want.size() && i < report.size(); ++i) PSE_CHECK_EQ(report[i], want[i]);
    const records::TableFiles out = records::Tables(forms, schema.root);
    PSE_CHECK_EQ(out.at("GUNS.TXT"),
                 std::string("M16,Rifle01,21,850.00,NO\r\nAK47,-1,12,780.00,YES\r\nODD,x,1.5,7.1,maybe\r\n"
                             "G3,Rifle01,21,850.00,NO\r\n"));
}

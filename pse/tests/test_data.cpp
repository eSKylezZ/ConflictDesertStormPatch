// PSE data files (pse/records/data.h): reader, canonical writer, refusals.
#include <cmath>
#include <limits>

#include "data.h"
#include "pse_test.h"

using namespace pse::data;

namespace {
std::string Canon(const std::string& in, std::string* error = nullptr) {
    Document d;
    std::string e;
    if (!Parse(in, &d, &e)) {
        if (error) *error = e;
        return "<parse error: " + e + ">";
    }
    std::string out;
    if (!Write(d, &out, &e)) return "<write error: " + e + ">";
    return out;
}
bool Refused(const std::string& in, const char* expectWhere) {
    Document d;
    std::string e;
    if (Parse(in, &d, &e)) return false;
    return e.rfind(expectWhere, 0) == 0;
}
}  // namespace

PSE_TEST("data: canonical layout - key order, one-line tables, nesting") {
    const std::string in = "return { zeta = 1, alpha = { 3, 2, 1 }, id = \"ds1:weapon/m16a2\", [2] = true, [1] = false,"
                           " nested = { b = { c = 'x' } } }";
    PSE_CHECK_EQ(Canon(in), std::string("return {\n"
                                        "  id = \"ds1:weapon/m16a2\",\n"
                                        "  [1] = false,\n"
                                        "  [2] = true,\n"
                                        "  alpha = { 3, 2, 1 },\n"
                                        "  nested = {\n"
                                        "    b = { c = \"x\" },\n"
                                        "  },\n"
                                        "  zeta = 1,\n"
                                        "}\n"));
    PSE_CHECK_EQ(Canon("return {}"), std::string("return {}\n"));
    PSE_CHECK_EQ(Canon("return { 1, 2, x = 3 }"), std::string("return { 1, 2, x = 3 }\n"));
}

PSE_TEST("data: canonical output is stable (writing it again changes nothing)") {
    const std::string in = "-- header line\n--[[ block\n  two ]]\nreturn {\n  -- about b\n  b = 2, a = { 'q\\n', 1.5e-7, 0x10 },\n"
                           "  [\"not a name\"] = { [10] = 1 }, list = { { 1 }, { 2 } } }";
    const std::string once = Canon(in), twice = Canon(once);
    PSE_CHECK_EQ(once, twice);
    PSE_CHECK(once.rfind("-- header line\n-- block\n--  two\nreturn {\n", 0) == 0);
    PSE_CHECK(once.find("  -- about b\n  b = 2,\n") != std::string::npos);
    PSE_CHECK(once.find("[\"not a name\"] = { [10] = 1 },") != std::string::npos);
    PSE_CHECK(once.find("a = { \"q\\n\", 1.5e-07, 16 },") != std::string::npos);
}

PSE_TEST("data: long tables wrap at 100 columns") {
    std::string in = "return { names = {";
    for (int i = 0; i < 20; ++i) in += "\"name" + std::to_string(i) + "\",";
    in += "} }";
    const std::string out = Canon(in);
    PSE_CHECK(out.find("  names = {\n    \"name0\",\n") != std::string::npos);
    for (size_t s = 0, e; (e = out.find('\n', s)) != std::string::npos; s = e + 1) PSE_CHECK(e - s <= 100);
}

PSE_TEST("data: strings - escapes read, canonical escapes written, UTF-8 kept") {
    PSE_CHECK_EQ(Canon("return { 'it''s' }", nullptr).substr(0, 7), std::string("<parse "));
    PSE_CHECK_EQ(Canon("return { \"a\\tb\\\\c\\\"d\\65\\x41\\u{e9}\\z   e\" }"),
                 std::string("return { \"a\\tb\\\\c\\\"dAA\xC3\xA9" "e\" }\n"));
    PSE_CHECK_EQ(Canon("return { [[\nlong\n]] }"), std::string("return { \"long\\n\" }\n"));
    PSE_CHECK_EQ(Canon("return { \"\\1\" .. \"x\" }").substr(0, 7), std::string("<parse "));
    PSE_CHECK_EQ(Canon("return { \"\\0011\" }"), std::string("return { \"\\0011\" }\n"));
    PSE_CHECK_EQ(Canon("return { 'caf\xC3\xA9' }"), std::string("return { \"caf\xC3\xA9\" }\n"));
}

PSE_TEST("data: numbers - integers, floats (Python repr style), negatives, hex") {
    PSE_CHECK_EQ(FormatFloat(1.0), std::string("1.0"));
    PSE_CHECK_EQ(FormatFloat(0.1), std::string("0.1"));
    PSE_CHECK_EQ(FormatFloat(1e16), std::string("1e+16"));
    PSE_CHECK_EQ(FormatFloat(1e15), std::string("1000000000000000.0"));
    PSE_CHECK_EQ(FormatFloat(0.0001), std::string("0.0001"));
    PSE_CHECK_EQ(FormatFloat(0.00001), std::string("1e-05"));
    PSE_CHECK_EQ(FormatFloat(-2.5e-300), std::string("-2.5e-300"));
    PSE_CHECK_EQ(FormatFloat(123.456), std::string("123.456"));
    PSE_CHECK_EQ(FormatFloat(-0.0), std::string("-0.0"));
    PSE_CHECK_EQ(Canon("return { -5, - 3, 2.50, 1e3, -0x10, 9223372036854775807, -9223372036854775808 }"),
                 std::string("return { -5, -3, 2.5, 1000.0, -16, 9223372036854775807, -9223372036854775808 }\n"));
    PSE_CHECK(Refused("return { 9223372036854775808 }", "1:"));
    Document d;
    d.root.Push(Value::Float(std::numeric_limits<double>::infinity()));
    std::string out, e;
    PSE_CHECK(!Write(d, &out, &e));
}

PSE_TEST("data: anything but literals is refused, with line:column") {
    PSE_CHECK(Refused("x = 1", "1:1:"));
    PSE_CHECK(Refused("return {\n  a = os.time(),\n}", "2:7:"));
    PSE_CHECK(Refused("return { a = nil }", "1:"));
    PSE_CHECK(Refused("return { a = 1 + 2 }", "1:"));
    PSE_CHECK(Refused("return { a = 1, a = 2 }", "1:"));
    PSE_CHECK(Refused("return { 10, [1] = 5 }", "1:"));
    PSE_CHECK(Refused("return { [true] = 1 }", "1:"));
    PSE_CHECK(Refused("return { 1 } extra", "1:"));
    PSE_CHECK(Refused("return { function() end }", "1:"));
    PSE_CHECK(Refused("return { \"open }", "1:"));
    PSE_CHECK(Refused("return { 1", "1:"));
}

PSE_TEST("data: Get / Set / Push / Equals") {
    Document d;
    d.root.Set("name", Value::String("M16A2"));
    d.root.Set("id", Value::String("ds1:weapon/m16a2"));
    Value& stats = d.root.Set("stats", Value::Table());
    stats.Set("damage", Value::Float(12.5));
    d.root.Push(Value::Int(7));
    std::string out, e;
    PSE_REQUIRE(Write(d, &out, &e));
    PSE_CHECK_EQ(out, std::string("return {\n  7,\n  id = \"ds1:weapon/m16a2\",\n  name = \"M16A2\",\n"
                                  "  stats = { damage = 12.5 },\n}\n"));
    Document back;
    PSE_REQUIRE(Parse(out, &back, &e));
    PSE_CHECK(back.root.Equals(d.root));
    PSE_CHECK(back.root.Get("stats")->Get("damage")->f == 12.5);
    back.root.Set("name", Value::String("other"));
    PSE_CHECK(!back.root.Equals(d.root));
}

// The mod priority list (games/ds1/src/core/modorder.h): load order rules - first start alphabetical, new mods last,
// gone ones dropped, moving, a joiner adopting the host's order; and the online check seeing an order difference.
#include "../../games/ds1/src/core/modorder.h"  // NOLINT: the unit under test (pure code)
#include "loader.h"
#include "pse_test.h"

namespace {
std::string J(const std::vector<std::string>& v) { return modorder::Join(v); }
}

PSE_TEST("modorder: first start = alphabetical (case-insensitive), as before the list existed") {
    PSE_CHECK_STR(J(modorder::Resolve({"zeta", "Alpha", "beta"}, {})).c_str(), "Alpha|beta|zeta");
}

PSE_TEST("modorder: saved order kept, new mods at the end (alphabetical among themselves), gone ones dropped") {
    const auto saved = modorder::Split("Zeta|Gone|alpha");
    PSE_CHECK_STR(J(modorder::Resolve({"Alpha", "Zeta", "NewB", "NewA"}, saved)).c_str(), "Zeta|Alpha|NewA|NewB");
    // the folder's own spelling wins over the saved one; duplicates in the saved value don't duplicate
    PSE_CHECK_STR(J(modorder::Resolve({"ALPHA"}, modorder::Split("alpha|Alpha"))).c_str(), "ALPHA");
}

PSE_TEST("modorder: split / join, empty parts ignored, '_' off-prefix stripped by Bare") {
    PSE_CHECK_EQ(modorder::Split("").size(), size_t(0));
    PSE_CHECK_STR(J(modorder::Split("|a||b|")).c_str(), "a|b");
    PSE_CHECK_STR(modorder::Bare("__Off").c_str(), "Off");
    PSE_CHECK_STR(modorder::Bare("On").c_str(), "On");
}

PSE_TEST("modorder: move earlier / later, not past the ends") {
    std::vector<std::string> o = {"A", "B", "C"};
    PSE_CHECK(modorder::Move(o, "c", -1));
    PSE_CHECK_STR(J(o).c_str(), "A|C|B");
    PSE_CHECK(!modorder::Move(o, "A", -1));
    PSE_CHECK(!modorder::Move(o, "B", +1));
    PSE_CHECK(!modorder::Move(o, "X", +1));
    PSE_CHECK(modorder::Move(o, "A", +1));
    PSE_CHECK_STR(J(o).c_str(), "C|A|B");
}

PSE_TEST("modorder: a joiner adopts the host's order for the shared mods, its others follow") {
    PSE_CHECK_STR(J(modorder::Adopt({"A", "Mine", "B", "C"}, {"C", "a"})).c_str(), "C|A|Mine|B");
    // a mod just downloaded from the host isn't on the joiner's list yet: it gets the host's place
    PSE_CHECK_STR(J(modorder::Adopt({"A", "Mine"}, {"New", "A"})).c_str(), "New|A|Mine");
    PSE_CHECK(modorder::SameRelativeOrder({"A", "x", "B"}, {"a", "B", "y"}));
    PSE_CHECK(!modorder::SameRelativeOrder({"A", "B"}, {"B", "A"}));
}

PSE_TEST("compat: same mods in another load order is a difference (mods differ, not a plugin refusal)") {
    pse::compat::Set a, b;
    a.mods = {{"Alpha", "h1"}, {"Beta", "h2"}};
    b.mods = {{"Beta", "h2"}, {"alpha", "h1"}};
    const auto d = pse::compat::Compare(a, b, "the host");
    PSE_CHECK_EQ(d.size(), size_t(1));
    PSE_CHECK(d[0].find("mod load order differs") == 0);
    PSE_CHECK(pse::compat::Compare(a, a, "x").empty());
    b.mods = {{"Alpha", "h1"}, {"Gamma", "h3"}, {"Beta", "h2"}};   // an extra mod in between: order still the same
    for (const std::string& line : pse::compat::Compare(a, b, "the host"))
        PSE_CHECK(line.find("load order") == std::string::npos);
}

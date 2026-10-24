#pragma once
// Several mods' patches of one game file merged into one (docs/sdk/data-files.md "Several mods, one level"), so mods
// can change the same level the way Creation Engine plugins edit the same cell:
//
//   .envpatch  "base <SCENE>.ENV" + hunks "@ <start> <delete> <insert>" (line changes against the base scene). Hunks
//              of different mods that touch different lines all apply; inserts at the same place (two mods adding an
//              object before HLOX) are kept, in mod order; changes that overlap: the later mod's wins.
//   .rmdpatch  "base <LEVEL>.RMD", "count <n>", marker rows "<i> ..." (route markers that differ from the base). Rows
//              of different markers all apply; the same marker set differently: the later mod's row; count = the
//              largest.
//
// Patches against different base files can't merge: the later mod's patch is used alone. Every overlap / different
// value is a report line "conflict: <name> <where>: <mod a> / <mod b> (<mod b> wins)"; a patch the merge can't read
// is "[fail] <mod>: ...". DesertStormFix's mods.cpp builds the merged file from the player's own base file.
#include <cstdint>
#include <string>
#include <vector>

namespace pse::records {

struct PatchText {
    std::string mod;    // shown in the report
    std::string text;   // the patch file's text
};

// The patches of one output file (`name`, for the report) in mod order -> one patch of the same kind. false = none of
// them could be read (*merged empty).
bool MergeEnvPatches(const std::string& name, const std::vector<PatchText>& patches, std::string* merged,
                     std::vector<std::string>* report);
bool MergeRmdPatches(const std::string& name, const std::vector<PatchText>& patches, std::string* merged,
                     std::vector<std::string>* report);

// The script ids of a scene's objects (HLOI lines, file order) - to find ids two mods both gave a new object.
std::vector<int64_t> SceneScriptIds(const std::string& scene);

}  // namespace pse::records

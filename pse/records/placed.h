#pragma once
// Placed references (DESIGN.md §13.3, docs/sdk/data-files.md "Placed references"): mods' records\*.lua edits of a
// level's objects, waypoints and regions ("ds1:mission3/object/cs_hero_foley") applied to that level's scene
// (<LEVEL>.ENV). The C++ twin of the ModKit's conflict_ds_tools/placed.py apply_edits - the same scene text and the
// same report lines (scripts/records_placed_gate.py).
//
// Editor IDs come from the scene as the readable mission scripts name things (mission2lua_readable.refs_from_env):
// objects by their HLOI stat name (or unit + script id), waypoints by name, regions by name, made into Lua names.
// Only the scene layouts of Desert Storm (scene version 70-72) are read: HLOI, NEWENVWAYPT, REGN.
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "forms.h"

namespace pse::records {

// "ds1:<level>/<kind>/<name>" -> its three parts; false for any other id (table records have two parts).
bool PlacedIdParts(const std::string& id, std::string* level, std::string* kind, std::string* name);
// The levels the edits name (lower case, sorted) - every edit whose id has three parts; the level part ignores case.
std::vector<std::string> PlacedLevels(const std::vector<Edits>& edits);
// Applies the edits of `level` (lower case: "mission3") to its scene (the bytes as the archive holds them, latin-1)
// and returns the new scene; `forms` = the records `base = <record id>` is looked up in. *report gets placed.py's
// lines ("new ...", "... taken out of play", "[fail] <mod>/<file>: ...").
std::string ApplyPlaced(const std::string& scene, const std::string& level, const std::vector<Edits>& edits,
                        const Forms& forms, std::vector<std::string>* report);

// The editor IDs alone (mission2lua_readable.refs_from_env): HLOI script id -> object name, NEWENVWAYPT id -> waypoint
// name, REGN index -> region name. False when the scene can't be read.
bool SceneRefNames(const std::string& sceneBytes, std::map<int64_t, std::string>* objects,
                   std::map<int64_t, std::string>* waypoints, std::vector<std::string>* regions);

// The camera records' names in scene order (CAMFREE / CAMTARGPOS / CAMTARGOBJ / CAMCHASEOBJ; "" = unnamed): the n-th is
// camera n of the script API's set_view_camera.
bool SceneCameraNames(const std::string& sceneBytes, std::vector<std::string>* names);
// _ref_ident: text -> a Lua name not in *used (and not a keyword), added to *used.
std::string RefIdent(const std::string& text, std::set<std::string>* used);

}  // namespace pse::records

#pragma once
// DS2 game layer: the co-save "<save>.pse" around the game's own save / load, and NEW_GAME (docs/sdk/reference/
// game-ds2.md "Saves", serialization.md). Called by the layer's save / load / level hooks (ds2_events.cpp); pure loader
// + file code, so pse_tests compiles it with pse_core (pse/tests/test_game_ds2.cpp). Same behaviour as DS1's
// ds1_saves.cpp (minus DesertStormFix's legacy sidecar) - the differences are DS2's path rules:
//   Save_Game  0x41C600 thiscall(saver [0x62F458], const wchar_t* path, int flag) ret 8: flag != 0 -> no file; else
//              the buffer goes to path + ".sav" (always appended, fopen "wb")
//   Save_Load  0x41C7C0 thiscall(saver, const wchar_t* path) ret 4: reads path with its extension replaced by ".sav"
//              (_splitpath / _makepath: the text after the last '.' of the file name, if any)
// The game narrows the UTF-16 path by dropping the high bytes; the layer hands the plugins the path as UTF-8.
#include <string>

#include "pse/pse_api.h"

namespace ds2_saves {
// The layer's plugin (log lines are its own). 0 = the loader's.
void SetOwner(PSEPluginHandle self);

// The .sav file the game writes for a Save_Game path / reads for a Save_Load path, and its co-save ("<name>.pse").
std::string SaveFileFor(const std::string& path);
std::string LoadFileFor(const std::string& path);
std::string CoSavePathFor(const std::string& savPath);

// Before the game writes `sav` (the .sav file): PSE_MSG_SAVE_BEGIN (data = that file).
void SaveBegin(const std::string& sav);
// After it: written = the game wrote the file. Then pse::cosave::Save(co-save) and PSE_MSG_SAVE_END. existedBefore =
// the .sav was there before this save (a co-save left over from a save deleted outside the game is removed first).
void SaveEnd(const std::string& sav, bool written, bool existedBefore);
// The game's load of `sav`: PSE_MSG_LOAD_BEGIN before, and after it (ok = the game read it) the co-save (or a revert)
// to the plugins, then PSE_MSG_LOAD_END.
void LoadBegin(const std::string& sav);
void LoadEnd(const std::string& sav, bool ok);
// Every level start ("FrontEnd" or a mission's name): a mission started from the front end without a save being
// loaded = NEW_GAME (pse::cosave::NewGame + PSE_MSG_NEW_GAME, data = the level name).
void LevelStart(const char* level);

// Tests: forget the state (load pending, came from the front end).
void ResetForTests();
}  // namespace ds2_saves

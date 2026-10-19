#pragma once
// DS1 game layer: the co-save "<save>.pse" around the game's own save / load, and NEW_GAME (docs/sdk/reference/
// serialization.md "DS1"). Called by the layer's save / load / level-init hooks (ds1_events.cpp); pure loader + file
// code, so pse_tests compiles it with pse_core (pse/tests/test_ds1_layer_events.cpp).
// The game's saves: Documents\SCi\DesertStorm\<name>.sav, written whole by the saver FUN_00415650(this [0x60EDC4],
// path, flag) (flag 1 = no file, e.g. "DS_auto"), read by FUN_00415B60(this, path); the game never deletes or renames
// a save file (no DeleteFileA / MoveFileA caller).
#include <string>

#include "pse/pse_api.h"
#include "pse_game_ds1_host.h"

namespace ds1_saves {
// The layer's plugin (log lines are its own). 0 = the loader's.
void SetOwner(PSEPluginHandle self);
// DesertStormFix's reader of its old ".sav.dsfix" sidecars (pse.game.ds1.host SetLegacyCosave); null = none.
void SetLegacy(PSEDs1LegacyCosaveFn fn);

// Before the game writes "<path>": PSE_MSG_SAVE_BEGIN.
void SaveBegin(const char* path);
// After it: written = the game wrote the file. Then pse::cosave::Save("<name>.pse") and PSE_MSG_SAVE_END.
// existedBefore = the .sav was there before this save (a co-save left over from a save deleted outside the game
// is removed first, so it can't hand its chunks to an unrelated new save).
void SaveEnd(const char* path, bool written, bool existedBefore);
// The game's load: PSE_MSG_LOAD_BEGIN before the game reads the file ...
void LoadBegin(const char* path);
// ... and after it (ok = the game read it): the co-save (or the legacy reader's image, or a revert) to the plugins,
// then PSE_MSG_LOAD_END.
void LoadEnd(const char* path, bool ok);
// Level init (every level): a game level started from the front end without a save being loaded = NEW_GAME
// (pse::cosave::NewGame + PSE_MSG_NEW_GAME, data = the level name).
void LevelStart(const char* level);

// "<dir>\Name.sav" (".sav" added if missing, as the game's FUN_004be6e0 does) and its co-save "<dir>\Name.pse".
std::string WithSav(const char* path);
std::string CoSavePathFor(const std::string& savPath);
// Tests: forget the state (load pending, came from the front end).
void ResetForTests();
}  // namespace ds1_saves

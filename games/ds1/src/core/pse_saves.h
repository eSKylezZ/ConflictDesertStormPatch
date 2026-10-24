#pragma once
// DesertStormFix side of the DS1 co-save (docs/sdk/reference/serialization.md "DS1"). The co-save itself - "<save>.pse"
// written / read around the game's own save / load, PSE_MSG_SAVE_* / LOAD_* / NEW_GAME - belongs to the game layer
// (pse/games/ds1/ds1_saves.cpp, hooks in ds1_events.cpp) since 2026-10-19. What stays here is DesertStormFix's own
// knowledge: its chunks (core/pse_saves_format.h, written by dsfix.loadout / dsfix.gear / dsfix.vehicles through
// pse.serialization) and the reader of its old "<save>.sav.dsfix" text sidecar, handed to the layer through
// pse.game.ds1.host SetLegacyCosave (pse_host.cpp).
#include <cstdint>

#include "pse/pse_api.h"

namespace pse_saves {
// PSEDs1LegacyCosaveFn: the co-save image (the chunks the plugins write today) of "<savPath>.dsfix", copied into buf
// (up to bufLen bytes); returns its full size, 0 = no sidecar.
uint32_t PSE_CALL LegacyCosave(const char* savPath, void* buf, uint32_t bufLen);
}  // namespace pse_saves

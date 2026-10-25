// dsfix.missions: mission scripts - the Lua script host for .lua levels (scripthost.cpp) and its films (movie.cpp),
// translated mission DLLs and their x86 runtime (missionsource.cpp, missionrt.cpp), the script API trace (apitrace.cpp,
// dev) and the co-op branches of the shipped mission scripts (coopscripts.cpp). One plugin DLL since 2026-10-25: they
// share the Lua state. The loader's private pse.lua.host goes to host plugins only - dsfix.dll hands it on
// (dsfix.host LuaHostTable). What dsfix.dll needs of them is the interface dsfix.missions (missions.spec; without the
// plugin: no .lua levels, every mission runs as its DLL, the shipped scripts' single-player branches).
#include <windows.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "features/features.h"
#include "kit/kit.h"
#include "missions/generated/missions_iface.h"

#include "missions/generated/missions_export.inc"

namespace {
bool Load() {
    // MISSIONS FROM LUA lived in the key of the internal plugin dsfix.missionsource until 2026-10-25
    kit::ImportFrom("dsfix.missionsource", "TranslatedMissions");
    features::ApplyCoopScripts();
    features::ApplyScriptHost();
    features::ApplyMissionRt();
    features::ApplyMissionSource();
    features::ApplyApiTrace();
    return RegisterMissionsInterface();
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.missions", "Mission scripts (Lua, translated missions)", PSE_PLUGIN_SIMULATION, Load)

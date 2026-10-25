# What dsfix.dll calls in the plugin dsfix.missions (Lua mission scripts, translated missions + their runtime, Lua
# films, the script API trace, co-op mission scripts); `= value` is dsfix.dll's answer without the plugin.
# scripts/gen_host_calls.py -> plugins/missions/generated/, src/generated/missions_wrappers.inc.
interface dsfix.missions
side plugin missions

# scripthost.cpp
bool features::ScriptLevelActive() = false
bool features::ScriptLoadingSave() = false
bool features::ScriptHidesHud() = false
void features::SetScriptHud(bool on)
const char* features::ScriptText(uint32_t hash) = nullptr
const char* features::QuickStartLevel() = nullptr
void features::ScriptReseed(uint32_t seed)
void features::EnsureDormancyHook()
# movie.cpp
bool features::ScriptFilmPlaying() = false
# missionsource.cpp
std::string features::MissionReplacement(const char* dllName) = ""
const char* features::MissionScriptText() = nullptr

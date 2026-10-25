# What dsfix.dll calls in the plugin dsfix.loadout (briefing / loadout screen, kits + unlocks, gear, vehicle swaps, the
# mission overview, mid-level skins); `= value` is dsfix.dll's answer without the plugin.
# scripts/gen_host_calls.py -> plugins/loadout/generated/, src/generated/loadout_wrappers.inc.
interface dsfix.loadout
side plugin loadout

# dsfix.dll's frame dispatcher (launcher.cpp) calls these in its own order
void features::OnFrameCustomise()
void features::OnFrameGear()
void features::OnFrameLoadoutDev()
void features::OnFrameOverview()
void features::OnFrameSkins()

# the briefing / loadout screen (customise.cpp)
bool features::CustomiseOpen() = false
bool features::CustomisePending() = false
bool features::CustomiseHidesHud() = false
bool features::CustomiseClose() = false
void features::CustomiseDevInput(uint32_t bits)
bool features::CustomiseDevVehicle(int dir) = false
std::string features::LinkedFrom() = ""
bool features::LoadoutCommand(int from, const std::string& text) = false
void features::ResetScriptLoadout()
void features::SetScriptLoadout(bool on)
void features::SetScriptBriefing(const std::string& text)
void features::SetScriptKit(int id, const std::string& uniform, const std::string& main, const std::string& sidearm, const std::string& launcher, const std::string& grenades, bool lock)
void features::ApplyScriptKitNow(int id)

# kits (loadout.cpp)
bool features::LoadoutUsesFillers() = false
int features::KitSlotOf(const char* weapon) = -1
bool features::KitSwap(void* soldier, void* oldItem, const std::string& weapon, int slot, int mags) = false
void* features::GiveSpawnedWeapon(void* soldier, const char* weapon, int mags) = nullptr
bool features::GiveSpawnedMedikit(void* soldier) = false
void* features::PhotoSpawnSoldier(int index, const float pos[3], float yaw) = nullptr
std::vector<std::string> features::PhotoSpawnUnits() = {}
std::vector<std::string> features::PhotoWeaponChoices() = {}

# the overview (overview.cpp)
bool features::OverviewPlaying() = false
bool features::OverviewSkipPending() = false

# vehicle swaps (vehicles.cpp)
bool features::VehicleReloadSkip() = false
bool features::VehicleSwapsThisLoad() = false
void features::VehiclesLevelStart(const char* level)

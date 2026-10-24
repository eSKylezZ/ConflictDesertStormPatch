# What DesertStormFix's own plugin DLLs call in dsfix.dll (scripts/gen_host_calls.py -> src/generated/dsfix_host*).
# Each line exactly as declared in its header, without default arguments. Add what a moved feature calls; the order
# doesn't matter to anyone but the layout hash (plugins and dsfix.dll ship together).
interface dsfix.host
side host
pod padio::JoyState

# input / window (dev.cpp, ingame_input.cpp, controller.cpp, padio.cpp)
bool features::GameFocused()
bool features::KeyHeld(int vk)
bool features::KeyboardInUse()
bool features::PadStyleXbox()
uint32_t features::ReadJoystick(int joystick)
bool padio::Fill(int joystick, padio::JoyState& s)
# texts (ingame_input.cpp, mods.cpp, core/pse_host.cpp)
const char* features::GameText(uint32_t hash)  @GameTextHash
const char* features::ModText(uint32_t hash)
std::string features::LevelDisplayName(const char* level)
std::string features::LevelTitleText(const char* level)
# screens / HUD (services.cpp, customise.cpp, overview.cpp, movie.cpp, scripthost.cpp, viewmode.cpp)
void* features::ServiceScreen(uint32_t state)
bool features::HudHidden()
bool features::CustomiseHidesHud()
bool features::CustomiseOpen()
bool features::CustomisePending()
bool features::OverviewPlaying()
bool features::ScriptFilmPlaying()
bool features::ScriptHidesHud()
bool features::ViewPeekPass()
# loadout (loadout.cpp)
int features::KitSlotOf(const char* weapon)
bool features::KitSwap(void* soldier, void* oldItem, const std::string& weapon, int slot, int mags)
void* features::PhotoSpawnSoldier(int index, const float pos[3], float yaw)
std::vector<std::string> features::PhotoSpawnUnits()
std::vector<std::string> features::PhotoWeaponChoices()
# levels / online (console.cpp, core/net.cpp)
bool features::RestartLevel()
int net::LocalPlayer()

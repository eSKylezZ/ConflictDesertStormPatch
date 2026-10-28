# What dsfix.dll calls in the plugin dsfix.mods (the Mods folder: index, resolver, merged tables, sound caches, record
# edits, .weapon / .skin / .vehicle / .gear / .unit files; the shared asset store + pse.vfs provider; mod weapon
# pictures; the MODS menu; custom campaigns); `= value` is dsfix.dll's answer without the
# plugin (the game's own files only). Plugins reach most of these through dsfix.host (src/core/dsfix_host.spec), whose
# dsfix.dll side calls these wrappers. scripts/gen_host_calls.py -> plugins/mods/generated/, src/generated/mods_wrappers.inc.
interface dsfix.mods
side plugin mods

# the frame dispatcher (launcher.cpp)
void features::OnFrameCampaigns()
void features::OnFrameModsMenu()
void features::OnFrameWeaponIcons()

# front-end screens + their texts (frontend.cpp's router, the Lookup hook in ingame_input.cpp)
void* features::ModsMenuScreen() = nullptr
uint32_t features::ModsMenuItemText() = 0
const char* features::ModsMenuText(uint32_t hash) = nullptr
void* features::CampaignsScreen() = nullptr
const char* features::CampaignsText(uint32_t hash) = nullptr
void features::CampaignsMenuItem(void* singlePlayerScreen, bool coop)
bool features::CampaignLevelPicked() = false
const char* features::CampaignNext(const char* level) = "FrontEnd.dll"
bool features::CampaignPortLevel(const char* level) = false
const char* features::ModText(uint32_t hash) = nullptr

# the mods in use + their files
int features::ModsGeneration() = 0
const std::vector<std::string>& features::LoadedMods() = {}
std::vector<std::string> features::LoadedPorts() = {}
std::vector<std::string> features::ModOrder() = {}
void features::SetModOrder(const std::string& order)
bool features::ReloadMods() = false
std::string features::ModFilePath(const char* name) = ""
bool features::ModHasFile(const char* name) = false
bool features::ReadGameTable(const char* name, std::string& out) = false
std::string features::ModLevelRows() = ""
bool features::AssetStoreHasFile(const char* name) = false

# weapons / skins / gear / vehicles from mods (dsfix.services, dsfix.loadout, dsfix.voices, dsfix.bodies ...)
const std::vector<std::string>& features::ModWeapons() = {}
std::vector<std::string> features::KitWeaponIds() = {}
int features::ModWeaponMagazines(const std::string& id) = 0
int features::WeaponFactions(const char* id) = 0
std::string features::SkinBody(const char* texture) = ""
std::string features::SkinNormalMap(const char* texture) = ""
std::string features::SkinVoice(const char* texture) = ""
int features::SpeechLanguage() = 0
std::string features::ModGearRows() = ""
std::string features::ModPouchSetRows() = ""
std::string features::ModVehicleRows() = ""
std::string features::SkinVariantRows() = ""
uint32_t features::ModVehicleHandlingCount() = 0
bool features::ModVehicleHandlingAt(uint32_t i, std::string& model, float& grip, float& spin) = false
void features::BorrowUnitData(const char* level)
void features::BorrowVehicleDataList(const char* level, const std::string& units)
bool features::VehicleDataComplete(const std::string& unit) = false
std::string features::VehicleGuns(const std::string& unit) = ""
void features::PadLevelPools(const char* level, int extra)

# the Global Heap (dsfix.dll's memory.cpp: sized for the largest mod file; 0 = no mods)
uint32_t features::LargestModFileSize(std::string& name) = 0

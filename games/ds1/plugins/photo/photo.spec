# What dsfix.dll calls in the plugin dsfix.photo (photo mode, photo frames, free camera, cheats, pause menu items);
# `= value` is dsfix.dll's answer without the plugin. scripts/gen_host_calls.py -> plugins/photo/generated/,
# src/generated/photo_wrappers.inc.
interface dsfix.photo
side plugin photo

uint32_t features::CheatFlags() = 0
void features::SetCheatFlags(uint32_t flags)
bool features::CheatsActive() = false
const char* features::CheatsText(uint32_t hash) = nullptr
bool features::FreeCamActive() = false
bool features::FreeCamFlying() = false
void features::PhotoBeforePresent(void* device)
void features::PhotoAfterPresent(void* device)
bool features::PhotoAnimate(const void* instance, float& dt) = false
float features::PhotoFov() = 0.0f
bool features::PhotoModeOpen() = false
const char* features::PhotoText(uint32_t hash) = nullptr
void features::ReloadPhotoFrames()

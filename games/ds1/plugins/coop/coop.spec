# What dsfix.dll calls in the plugin dsfix.coop (split screen, the co-op join screen + session, controls / pad layouts,
# controllers, hot-plug, direct pad I/O, several keyboards / mice, versus); `= value` is dsfix.dll's answer without
# the plugin. scripts/gen_host_calls.py -> plugins/coop/generated/, src/generated/coop_wrappers.inc.
interface dsfix.coop
side plugin coop
pod padio::JoyState
enum features::PadType

# dsfix.dll's frame dispatcher (launcher.cpp) calls these in its own order
void features::OnFrameSplitScreen()
void features::OnFrameCoop()
void features::OnFrameControls()
void features::OnFrameHotplug()
void features::OnFrameMultiKbm()
void features::OnFrameVersus()
void features::FollowActivePad()
void padio::Update()

# split screen (splitscreen.cpp)
int features::SplitScreenPlayers() = 0
bool features::SplitHudOffset(float& x, float& y) = false
void features::SplitHudReset()
void features::SplitHudView(int view, float x, float y, float w, float h)
void features::AxisParts(uint8_t* block, int action, float& pad, bool& touched, bool& key, float& gyro)
float features::LookScale(int player) = 1.0f

# the co-op join screen + session (coop.cpp)
int features::CoopPlayers() = 0
int features::CoopJoystick(int player) = -1
int features::CoopPromptStyle(int player) = -1
uint32_t features::CoopDeviceActions(int device, features::PadType& type, std::string& name) = 0
void features::BeginOnlineSession(int players, bool host)
const char* features::CoopText(uint32_t hash) = nullptr
bool features::CoopDevBegin() = false
void* features::CoopJoinScreen() = nullptr

# versus (versus.cpp)
void* features::VersusScreen() = nullptr
void* features::VersusSetupScreen() = nullptr
bool features::VersusEnabled() = false
uint32_t features::VersusItemText() = 0
const char* features::VersusText(uint32_t hash) = nullptr

# controls / pads (controls.cpp, padlayout.cpp, controller.cpp, hotplug.cpp, padio.cpp, multikbm.cpp)
void* features::ControlsScreen(uint32_t state) = nullptr
const char* features::ControlsText(uint32_t hash) = nullptr
int features::PlayerPadButton(int player, int action) = -1
uint32_t features::ReadJoystick(int joystick) = 0
bool padio::Fill(int joystick, padio::JoyState& s) = false
void features::SetPadsBackground(bool on)
bool features::KbmStart() = false
void features::KbmInject(int fake, bool mouse, int code, bool up, int dx, int dy, int buttonFlags, int wheel)

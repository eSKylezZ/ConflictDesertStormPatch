# What dsfix.dll calls in the plugin dsfix.online (online connection, lobby, lockstep / host authority, own view +
# nameplates, the determinism harness); `= value` is dsfix.dll's answer without the plugin.
# scripts/gen_host_calls.py -> plugins/online/generated/, src/generated/online_wrappers.inc.
interface dsfix.online
side plugin online
pod features::HostSettings

# dsfix.dll's frame dispatcher (launcher.cpp OnFrameEarly / OnFrame) calls these in its own order
void features::OnFrameLockstep()
void features::OnFrameDeterminism()
void features::OnFrameOnline()
void features::OnFrameViews()

# lockstep / host authority (lockstep.cpp, authority.cpp, determinism.cpp)
bool features::LockstepActive() = false
bool features::LockstepAxis(uint8_t* block, int action, int opposite, float& value) = false
void features::LockstepAxisTrace(const uint8_t* block, int action, float value, bool synced)
bool features::LockstepBehind() = false
void features::LockstepCatchUp(bool on)
void features::LockstepFixBlocks()
bool features::LockstepMouse(uint8_t* block, float* dx, float* dy) = false
uint32_t features::LockstepSeed() = 0
bool features::LockstepSkipCutscenes(bool own) = own
bool features::LockstepSkipRender() = false
bool features::DetActive() = false
void features::AuthorityDevSkipVote(int player)

# the host's settings, the lobby (hostsync.cpp, lobbynet.cpp, lobbymenu.cpp)
const features::HostSettings* features::HostSync() = nullptr
bool features::LoadoutForMission() = settings::Get().featLoadout
std::vector<std::string> features::LobbyConsole(const std::string& arg) = {}
void features::LobbyDevPress(int device, uint32_t actions)
int features::LobbyDevice(int player) = -1
bool features::LobbyLocal(int player) = false
int features::LobbyPcs() = 0
std::string features::LobbyPlayerName(int player) = ""

# screens / texts (online.cpp, viewmode.cpp)
uint32_t features::CoopMenuState() = 1
void* features::OnlineScreen(uint32_t state) = nullptr
const char* features::OnlineText(uint32_t hash) = nullptr
bool features::OnlineJoinerHoldsLevel() = false
bool features::ViewHidden(int player) = false
bool features::ViewPeekPass() = false

# the connection (core/net.cpp) + the console's net commands (glue.cpp)
bool net::Active() = false
int net::LocalPlayer() = 0
bool net::SendCommand(const std::string& text) = false
bool net::SendCommandTo(int player, const std::string& text) = false
uint32_t features::OnlineConnectedPcMask() = 0
int features::OnlineConnectedPeers() = 0
std::vector<std::string> features::OnlineConsoleNet(const std::string& arg) = {}
std::vector<std::string> features::OnlineConsoleEvents() = {}
void features::OnlineShutdown()

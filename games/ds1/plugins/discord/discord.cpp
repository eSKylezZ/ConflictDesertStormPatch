// dsfix.discord - Discord Rich Presence for Conflict: Desert Storm, as an external PSE plugin
// (<game>\PSE\Plugins\dsfix.discord.dll + .pse). Built from the SDK only: pse/pse_api.h + pse/pse_game_ds1.h.
//
// Discord's local IPC (named pipe \\.\pipe\discord-ipc-0..9, frames of {uint32 opcode, uint32 length} + JSON:
// opcode 0 handshake {"v":1,"client_id":...}, opcode 1 commands, opcode 2 close) - no SDK.
//
// The game thread takes a snapshot once per frame (PSE_EVENT_FRAME, through pse.game.ds1); a background thread sends
// it when it changes (Discord allows ~5 activity updates per 20 s, we wait >= 4 s) and reconnects when Discord
// (re)starts. Shows the mission, single player / split screen with the player count, and the time since the mission
// started. Setting "DiscordPresence" (pse.settings, live; launcher Enhancements column + OPTIONS > DESERTSTORMFIX >
// DISCORD STATUS). Until 2026-10-19 it was the internal feature src/features/discord.cpp with the setting in plugin
// "dsfix"'s key - Load copies that value once (ImportOldSetting).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <pse/pse_api.h>
#include <pse/pse_game_ds1.h>

namespace {
// Discord's own application for the game (its list of detectable games,
// https://discord.com/api/v9/applications/detectable: "Conflict: Desert Storm", conflict_desert_storm/desertstorm.exe)
// - Discord shows "Playing Conflict Desert Storm". Its Rich Presence art assets can't be ours, so no image is sent;
// a registered app of our own (Discord Developer Portal) with an asset key in kLargeImage would add one.
constexpr char kAppId[] = "1124357225184505886";
constexpr char kLargeImage[] = "";
constexpr char kName[] = "dsfix.discord";   // = PSEPlugin_Info.name and the manifest's name
constexpr char kSetting[] = "DiscordPresence";
// The setting's old home: plugin "dsfix" (DesertStormFix's core) in the same pse.settings tree, and the marker this
// plugin leaves in its own key once it has looked there.
constexpr char kOldPlugin[] = "dsfix";
constexpr char kImportedMarker[] = "ImportedFromDsfix";

PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSESettingsInterface* g_settings = nullptr;
const PSEMessagingInterface* g_messaging = nullptr;
const PSEGameDs1V1* g_game = nullptr;

#define LOG(...) g_log->Write(g_self, PSE_LOG_INFO, __VA_ARGS__)

enum class Mode { Menus, SinglePlayer, SplitScreen };
struct Snapshot {
    Mode mode = Mode::Menus;
    int players = 1;
    char level[64] = {};
    char title[128] = {};  // "Mission 4: Desert Watch" (UTF-8)
    bool operator==(const Snapshot& o) const {
        return mode == o.mode && players == o.players && strcmp(level, o.level) == 0;
    }
};

CRITICAL_SECTION g_lock;
Snapshot g_shared;          // written by the game thread, read by the sender
HANDLE g_wake = nullptr;    // set when the snapshot changes (or to stop)
HANDLE g_stop = nullptr;    // set by PSEPlugin_Unload
HANDLE g_thread = nullptr;
volatile bool g_enabled = false;

// JSON string body: quotes and backslashes escaped, control characters dropped.
std::string JsonSafe(const char* s) {
    std::string out;
    for (const char* c = s; *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (static_cast<unsigned char>(*c) >= 0x20) out += *c;
    }
    return out;
}

// "Mission4.env" -> "Mission 4" etc. Only for a level LevelTitle gave nothing for (it always does for a mission).
std::string FallbackTitle(const char* level) {
    std::string name(level);
    if (auto dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    return name;
}

// Sleeps up to ms; true = stop requested.
bool Wait(DWORD ms) { return WaitForSingleObject(g_stop, ms) == WAIT_OBJECT_0; }

// ---- IPC (sender thread only) ----
HANDLE g_pipe = INVALID_HANDLE_VALUE;

void Disconnect() {
    if (g_pipe != INVALID_HANDLE_VALUE) CloseHandle(g_pipe);
    g_pipe = INVALID_HANDLE_VALUE;
}

bool Send(uint32_t op, const std::string& json) {
    std::string frame(8 + json.size(), '\0');
    const uint32_t len = static_cast<uint32_t>(json.size());
    memcpy(&frame[0], &op, 4);
    memcpy(&frame[4], &len, 4);
    memcpy(&frame[8], json.data(), json.size());
    DWORD written = 0;
    if (!WriteFile(g_pipe, frame.data(), static_cast<DWORD>(frame.size()), &written, nullptr) ||
        written != frame.size()) {
        Disconnect();
        return false;
    }
    return true;
}

// Discord answers every command; read (and drop) what is there so the pipe never fills. Returns the last reply.
std::string Drain() {
    std::string last;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            Disconnect();
            return last;
        }
        if (avail < 8) return last;
        uint32_t header[2];
        DWORD got = 0;
        if (!ReadFile(g_pipe, header, 8, &got, nullptr) || got != 8) {
            Disconnect();
            return last;
        }
        std::string body(header[1], '\0');
        if (header[1] && (!ReadFile(g_pipe, &body[0], header[1], &got, nullptr) || got != header[1])) {
            Disconnect();
            return last;
        }
        if (header[0] == 2) {  // close: Discord refused (e.g. unknown application id)
            LOG("Discord: closed by Discord: %.200s", body.c_str());
            Disconnect();
            return body;
        }
        last = body;
    }
}

bool Connect() {
    for (int i = 0; i < 10 && g_pipe == INVALID_HANDLE_VALUE; ++i) {
        char name[32];
        snprintf(name, sizeof name, "\\\\.\\pipe\\discord-ipc-%d", i);
        g_pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    if (!Send(0, std::string("{\"v\":1,\"client_id\":\"") + kAppId + "\"}")) return false;
    for (int i = 0; i < 20 && g_pipe != INVALID_HANDLE_VALUE; ++i) {  // wait up to 2 s for READY
        DWORD avail = 0;
        if (PeekNamedPipe(g_pipe, nullptr, 0, nullptr, &avail, nullptr) && avail >= 8) {
            Drain();
            if (g_pipe != INVALID_HANDLE_VALUE) LOG("Discord: connected");
            return g_pipe != INVALID_HANDLE_VALUE;
        }
        if (Wait(100)) break;
    }
    Disconnect();
    return false;
}

std::string ActivityJson(const Snapshot& s, time_t start) {
    std::string details, state;
    switch (s.mode) {
        case Mode::Menus: details = "In the menus"; break;
        case Mode::SinglePlayer:
            details = JsonSafe(s.title[0] ? s.title : FallbackTitle(s.level).c_str());
            state = "Single player";
            break;
        case Mode::SplitScreen:
            details = JsonSafe(s.title[0] ? s.title : FallbackTitle(s.level).c_str());
            state = "Split screen \xC2\xB7 " + std::to_string(s.players) + " players";  // UTF-8 middle dot
            break;
    }
    char json[768];
    int n = snprintf(json, sizeof json,
                     "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"%lu\",\"args\":{\"pid\":%lu,\"activity\":{"
                     "\"details\":\"%s\"%s%s%s",
                     GetTickCount(), GetCurrentProcessId(), details.c_str(), state.empty() ? "" : ",\"state\":\"",
                     state.c_str(), state.empty() ? "" : "\"");
    if constexpr (sizeof kLargeImage > 1)
        if (n > 0 && n < static_cast<int>(sizeof json))
            n += snprintf(json + n, sizeof json - n,
                          ",\"assets\":{\"large_image\":\"%s\",\"large_text\":\"Conflict: Desert Storm\"}",
                          kLargeImage);
    if (s.mode != Mode::Menus && n > 0 && n < static_cast<int>(sizeof json))
        n += snprintf(json + n, sizeof json - n, ",\"timestamps\":{\"start\":%lld}", static_cast<long long>(start));
    if (n > 0 && n < static_cast<int>(sizeof json)) snprintf(json + n, sizeof json - n, "}}}");
    return json;
}

DWORD WINAPI Sender(void*) {
    Snapshot sent;
    bool haveSent = false;
    time_t missionStart = time(nullptr);
    DWORD lastSend = 0;
    const HANDLE waits[2] = {g_stop, g_wake};
    for (;;) {
        if (WaitForMultipleObjects(2, waits, FALSE, 2000) == WAIT_OBJECT_0) break;
        if (!g_enabled) {
            if (g_pipe != INVALID_HANDLE_VALUE) Disconnect();  // closing the pipe clears the status
            haveSent = false;
            continue;
        }
        Snapshot now;
        EnterCriticalSection(&g_lock);
        now = g_shared;
        LeaveCriticalSection(&g_lock);
        if (g_pipe == INVALID_HANDLE_VALUE) {
            haveSent = false;
            if (!Connect()) {
                if (Wait(10000)) break;  // Discord not running: look again later
                continue;
            }
        }
        Drain();
        if (haveSent && now == sent) continue;
        if (GetTickCount() - lastSend < 4000) continue;  // stay under Discord's rate limit; retried on the next wake
        if (!haveSent || strcmp(now.level, sent.level) != 0) missionStart = time(nullptr);
        if (Send(1, ActivityJson(now, missionStart))) {
            sent = now, haveSent = true, lastSend = GetTickCount();
            if (Wait(200)) break;
            std::string reply = Drain();  // SET_ACTIVITY echo, or an error
            LOG("Discord: %.240s", reply.c_str());
        }
    }
    Disconnect();
    return 0;
}

// ---- game thread ----
int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    if (!g_thread) return 0;
    Snapshot s;
    g_game->LevelName(s.level, sizeof s.level);
    // Front end / outro / nothing = menus; any other level counts as the mission, also while paused or in a cutscene.
    if (g_game->LevelKind() != PSE_DS1_LEVEL_MISSION) {
        s.mode = Mode::Menus;
        s.level[0] = 0;
    } else {
        s.players = static_cast<int>(g_game->SplitScreenPlayers());
        s.mode = s.players > 1 ? Mode::SplitScreen : Mode::SinglePlayer;
        // "Mission 4: Desert Watch" / "Basic Training"; looked up once per level.
        static char titledLevel[64] = {};
        static char title[128] = {};
        if (strcmp(titledLevel, s.level) != 0) {
            strcpy_s(titledLevel, s.level);
            g_game->LevelTitle(title, sizeof title);
            LOG("Discord: level %s -> \"%s\"%s", s.level, title, s.mode == Mode::SplitScreen ? " (split screen)" : "");
        }
        strcpy_s(s.title, title);
    }
    EnterCriticalSection(&g_lock);
    const bool changed = !(s == g_shared);
    if (changed) g_shared = s;
    LeaveCriticalSection(&g_lock);
    if (changed) SetEvent(g_wake);
    return 0;
}

// Setting on -> sender thread started (once); off -> the thread closes the pipe (clears the status) at its next wake.
void Apply() {
    g_enabled = g_settings->GetInt(g_self, kSetting) != 0 && strcmp(kAppId, "0") != 0;
    if (g_wake) SetEvent(g_wake);
    if (!g_enabled || g_thread) return;
    g_thread = CreateThread(nullptr, 0, Sender, nullptr, 0, nullptr);
    if (g_thread) LOG("Discord: status on (app %s)", kAppId);
    else g_log->Write(g_self, PSE_LOG_ERROR, "Discord: cannot start the sender thread (%lu)", GetLastError());
}

void PSE_CALL OnSettingChanged(const char* key, void*) {
    if (strcmp(key, kSetting) != 0) return;
    Apply();
    LOG("Discord: status %s", g_enabled ? "on" : "off");
}

// One-time copy of the setting from plugin "dsfix"'s key (where DesertStormFix kept it until 2026-10-19), before
// Declare reads ours. pse.settings keys: HKCU\Software\PSE\<game id>\Plugins\<plugin name> (reference/settings.md).
void ImportOldSetting(const char* gameId) {
    const std::string root = std::string("Software\\PSE\\") + gameId + "\\Plugins\\";
    const std::string own = root + kName, old = root + kOldPlugin;
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, own.c_str(), 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return;
    DWORD v = 0, size = sizeof v;
    if (RegQueryValueExA(key, kImportedMarker, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS) {
        RegCloseKey(key);
        return;
    }
    size = sizeof v;
    const bool haveOwn =
        RegQueryValueExA(key, kSetting, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS;
    DWORD oldValue = 0;
    size = sizeof oldValue;
    const bool haveOld = !haveOwn && RegGetValueA(HKEY_CURRENT_USER, old.c_str(), kSetting, RRF_RT_REG_DWORD, nullptr,
                                                  &oldValue, &size) == ERROR_SUCCESS;
    bool ok = true;
    if (haveOld) {
        ok = RegSetValueExA(key, kSetting, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&oldValue), sizeof oldValue) ==
             ERROR_SUCCESS;
        LOG("Discord: %s %lu copied from HKCU\\%s%s", kSetting, oldValue, old.c_str(), ok ? "" : " - FAILED");
    }
    const DWORD one = 1;
    if (ok) RegSetValueExA(key, kImportedMarker, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&one), sizeof one);
    RegCloseKey(key);
}
}  // namespace

extern "C" {

namespace {
const char* const kGames[] = {"ds1", nullptr};
}

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo),
    1,
    kName,
    "Discord status",
    "DesertStormFix",
    PSE_MAKE_VERSION(1, 0, 0),
    0,
    kGames,
    PSE_PLUGIN_CLIENT_ONLY,  // only talks to the Discord app on this PC
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_settings = static_cast<const PSESettingsInterface*>(pse->Query(g_self, PSE_SETTINGS_INTERFACE, 1));
    g_messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    g_game = static_cast<const PSEGameDs1V1*>(pse->Query(g_self, PSE_GAME_DS1_INTERFACE, 1));
    if (!g_log) return 0;
    if (!g_settings || !g_messaging || !g_game || g_game->size < sizeof(PSEGameDs1V1)) {
        g_log->Write(g_self, PSE_LOG_ERROR, "Discord: needs pse.log, pse.settings, pse.messaging and pse.game.ds1 v1");
        return 0;
    }

    ImportOldSetting(pse->gameId);
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = kSetting;
    d.type = PSE_SETTING_BOOL;
    d.flags = PSE_SETTING_LIVE | PSE_SETTING_LAUNCHER;
    d.defaultInt = 1;
    d.minInt = 0;
    d.maxInt = 1;
    d.label = "DISCORD STATUS";
    d.help = "show the mission / mode in your Discord status";
    d.group = "SYSTEM";
    if (g_settings->Declare(g_self, &d) != PSE_OK) return 0;
    g_settings->OnChange(g_self, &OnSettingChanged, nullptr);

    InitializeCriticalSection(&g_lock);
    g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_stop = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_wake || !g_stop || !g_messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0)) return 0;
    Apply();
    LOG("Discord: loaded (status %s, exe %s)", g_enabled ? "on" : "off", g_game->ExeKnown() ? "known build" : "unknown build");
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {
    if (g_thread) {
        SetEvent(g_stop);
        // The thread waits on g_stop everywhere (connect retries, the 10 s back-off); a pipe call could still block -
        // then keep the DLL in memory rather than free code a thread is running.
        if (WaitForSingleObject(g_thread, 3000) != WAIT_OBJECT_0) {
            HMODULE self = nullptr;
            GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               reinterpret_cast<LPCSTR>(&PSEPlugin_Unload), &self);
            g_log->Write(g_self, PSE_LOG_WARN, "Discord: sender thread still busy at exit - DLL kept loaded");
            return;
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_wake) CloseHandle(g_wake);
    if (g_stop) CloseHandle(g_stop);
    g_wake = g_stop = nullptr;
    if (g_log) LOG("Discord: unloaded");
}

}  // extern "C"

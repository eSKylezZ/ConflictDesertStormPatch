// Discord Rich Presence through Discord's local IPC (named pipe \\.\pipe\discord-ipc-0..9, frames of
// {uint32 opcode, uint32 length} + JSON: opcode 0 handshake {"v":1,"client_id":...}, opcode 1 commands) - no SDK.
//
// The game thread takes a snapshot once per frame (features::OnFrameDiscord); a background thread sends it when it
// changes (Discord allows ~5 activity updates per 20 s) and reconnects when Discord (re)starts. Shows the mission,
// single player / split screen with the player count, and the time since the mission started. Switched by the
// launcher setting "DiscordPresence".
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <string>

#include "core/log.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
// Discord's own application for the game (its list of detectable games,
// https://discord.com/api/v9/applications/detectable: "Conflict: Desert Storm", conflict_desert_storm/desertstorm.exe)
// - Discord shows "Playing Conflict Desert Storm". Its Rich Presence art assets can't be ours, so no image is sent;
// a registered app of our own (Discord Developer Portal) with an asset key in kLargeImage would add one.
constexpr char kAppId[] = "1124357225184505886";
constexpr char kLargeImage[] = "";

constexpr uint32_t kStrings = 0x60EDCC;       // the game's text table (catalog.dat, current language)
constexpr uint32_t kLookup = 0x4F6730;        // thiscall (table, hash) -> char* (ingame_input.cpp's Lookup)
constexpr uint32_t kCurrentLevel = 0x606880;  // loaded level's environment file ("Mission4.env")

enum class Mode { Menus, SinglePlayer, SplitScreen };
struct Snapshot {
    Mode mode = Mode::Menus;
    int players = 1;
    char level[32] = {};
    char title[128] = {};  // "Mission 4: Desert Watch" (UTF-8, JSON-safe)
    bool operator==(const Snapshot& o) const {
        return mode == o.mode && players == o.players && strcmp(level, o.level) == 0;
    }
};

// Level -> catalog.dat title (hash of MISSn_TITLE / TRAINn_TITLE). Longest prefixes first.
struct LevelTitleKey {
    const char* prefix;
    uint32_t hash;
};
constexpr LevelTitleKey kTitles[] = {
    {"mission11b", 150235186u},  {"mission11", 3976419625u}, {"mission12b", 481082815u},
    {"mission12", 2484535804u},  {"mission10", 3169110448u}, {"mission1", 2836128679u},
    {"mission2", 3491740530u},   {"mission3", 2180525035u},  {"mission4", 3969226520u},
    {"mission5b", 2041233653u},  {"mission5", 3178366849u},  {"mission6", 3295043412u},
    {"mission7", 2508312525u},   {"mission8", 1926267181u},  {"mission9", 590811572u},
    {"training1", 1029915587u},  {"training2", 1148656406u}, {"training3", 361794447u},
};

CRITICAL_SECTION g_lock;
Snapshot g_shared;          // written by the game thread, read by the sender
HANDLE g_wake = nullptr;    // set when the snapshot changes
bool g_started = false;
volatile bool g_enabled = false;

// "Mission4.env" -> "Mission 4", "Mission5a.env" -> "Mission 5A", "MP_MISSION2.env" -> "Multiplayer mission 2".
std::string LevelTitle(const char* dll) {
    std::string name(dll);
    if (auto dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    auto startsWith = [&](const char* p) { return _strnicmp(name.c_str(), p, strlen(p)) == 0; };
    std::string prefix, rest;
    if (startsWith("mp_mission")) prefix = "Multiplayer mission ", rest = name.substr(10);
    else if (startsWith("mission")) prefix = "Mission ", rest = name.substr(7);
    else if (startsWith("training")) prefix = "Training ", rest = name.substr(8);
    else return name;
    for (char& c : rest) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return prefix + rest;
}

// The level's title in the game's language, UTF-8 and JSON-safe ("" if unknown). Game thread only.
std::string GameTitle(const char* level) {
    uint32_t hash = 0;
    for (const auto& t : kTitles)
        if (_strnicmp(level, t.prefix, strlen(t.prefix)) == 0) {
            hash = t.hash;
            break;
        }
    void* table = *reinterpret_cast<void**>(kStrings);
    if (!hash || !table) return "";
    const char* text = reinterpret_cast<const char*(__thiscall*)(void*, uint32_t)>(kLookup)(table, hash);
    if (!text || !*text) return "";
    wchar_t wide[128];
    if (!MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 128)) return "";
    bool upper = true;  // "BASIC TRAINING" -> "Basic Training"
    for (const wchar_t* c = wide; *c; ++c)
        if (iswlower(*c)) upper = false;
    if (upper)
        for (wchar_t* c = wide; *c; ++c)
            if (c != wide && iswalpha(c[-1])) *c = towlower(*c);
    char utf8[256];
    if (!WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof utf8, nullptr, nullptr)) return "";
    std::string out;
    for (const char* c = utf8; *c; ++c) {
        if (*c == '"' || *c == '\\') out += '\\';
        if (static_cast<unsigned char>(*c) >= 0x20) out += *c;
    }
    return out;
}

// ---- IPC ----
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
            dslog::Write("Discord: closed by Discord: %.200s", body.c_str());
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
            std::string reply = Drain();
            if (g_pipe != INVALID_HANDLE_VALUE) dslog::Write("Discord: connected");
            return g_pipe != INVALID_HANDLE_VALUE;
        }
        Sleep(100);
    }
    Disconnect();
    return false;
}

std::string ActivityJson(const Snapshot& s, time_t start) {
    std::string details, state;
    switch (s.mode) {
        case Mode::Menus: details = "In the menus"; break;
        case Mode::SinglePlayer:
            details = s.title[0] ? s.title : LevelTitle(s.level);
            state = "Single player";
            break;
        case Mode::SplitScreen:
            details = s.title[0] ? s.title : LevelTitle(s.level);
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
    for (;;) {
        WaitForSingleObject(g_wake, 2000);
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
                Sleep(10000);  // Discord not running: look again later
                continue;
            }
        }
        Drain();
        if (haveSent && now == sent) continue;
        if (GetTickCount() - lastSend < 4000) continue;  // stay under Discord's rate limit; retried on the next wake
        if (!haveSent || strcmp(now.level, sent.level) != 0) missionStart = time(nullptr);
        if (Send(1, ActivityJson(now, missionStart))) {
            sent = now, haveSent = true, lastSend = GetTickCount();
            Sleep(200);
            std::string reply = Drain();  // SET_ACTIVITY echo, or an error
            dslog::Write("Discord: %.240s", reply.c_str());
        }
    }
}
}  // namespace

void features::ApplyDiscord() {
    g_enabled = settings::Get().discordPresence && strcmp(kAppId, "0") != 0;
    if (!g_enabled || g_started) return;
    InitializeCriticalSection(&g_lock);
    g_wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (HANDLE t = CreateThread(nullptr, 0, Sender, nullptr, 0, nullptr)) {
        CloseHandle(t);
        g_started = true;
        dslog::Write("[ok]   Discord status");
    }
}

void features::OnFrameDiscord() {
    if (!g_started) return;
    Snapshot s;
    const char* level = reinterpret_cast<const char*>(kCurrentLevel);
    strncpy_s(s.level, level, _TRUNCATE);
    // Front-end = menus; any other level counts as the mission, also while paused or in a cutscene.
    const bool menu = !s.level[0] || _strnicmp(s.level, "frontend", 8) == 0 || _strnicmp(s.level, "outro", 5) == 0;
    if (menu) {
        s.mode = Mode::Menus;
        s.level[0] = 0;
    } else {
        s.players = SplitScreenPlayers();
        s.mode = s.players > 1 ? Mode::SplitScreen : Mode::SinglePlayer;
        // "Mission 4: Desert Watch" / "Basic Training"; looked up once per level.
        static char titledLevel[32] = {};
        static std::string title;
        if (strcmp(titledLevel, s.level) != 0) {
            strcpy_s(titledLevel, s.level);
            std::string name = GameTitle(s.level);
            std::string prefix = LevelTitle(s.level);
            const bool training = _strnicmp(s.level, "training", 8) == 0;
            title = name.empty() ? prefix : training ? name : prefix + ": " + name;
        }
        strncpy_s(s.title, title.c_str(), _TRUNCATE);
    }
    EnterCriticalSection(&g_lock);
    const bool changed = !(s == g_shared);
    if (changed) g_shared = s;
    LeaveCriticalSection(&g_lock);
    if (changed) SetEvent(g_wake);
}

// pse.game.ds1 v1 (pse/include/pse/pse_game_ds1.h, docs/sdk/reference/game-ds1.md): read-only wrappers over the game's
// state, served by the built-in plugin "pse.game.ds1". Depends on game memory, the loader and Windows only - the
// DesertStormFix side (split screen, online session) comes in through Providers (set by DesertStormFix's pse_host.cpp
// through the private interface pse.game.ds1.host, ds1_layer.cpp). Part of the game layer PSE\Games\pse_ds1.dll;
// compiled into pse_tests as well (pse/tests/test_game_ds1.cpp).
#include "pse_game_ds1.h"

#include <windows.h>
#include <wincrypt.h>

#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#include "loader.h"

namespace {
constexpr uint32_t kFrontEndState = 0x617C18;  // u32: front-end state machine
constexpr uint32_t kDispatchMode = 0x60EC8C;   // u32: main loop dispatch
constexpr uint32_t kLevelName = 0x606880;      // char[]: loaded scene ("Mission4.env")
constexpr uint32_t kLevelNameMax = 64;
constexpr uint32_t kWorld = 0x63C948;          // world object; +0x1B8C bit 0 = cinematic
constexpr uint32_t kCinematicOffset = 0x1B8C;
constexpr uint32_t kStrings = 0x60EDCC;        // text table (catalog.dat, current language)
constexpr uint32_t kLookup = 0x4F6730;         // thiscall (table, hash) -> const char*
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478, kBlockSoldier = 0x310;
constexpr uint32_t kSoldierPos = 0x30, kSoldierHealth = 0x54;
constexpr uint32_t kGameWindow = 0x606A60;     // HWND
constexpr char kKnownSha256[] = "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164";

pse_game_ds1::Providers g_providers;

// Fault-guarded reads: the addresses are fixed, but pointers read from them may be stale.
bool Peek(uint32_t va, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(va)), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uint32_t Peek32(uint32_t va) {
    uint32_t v = 0;
    return Peek(va, &v, 4) ? v : 0;
}

uint32_t CopyOut(const std::string& s, char* buf, uint32_t bufLen) {
    if (buf && bufLen) {
        const size_t n = s.size() < bufLen - 1 ? s.size() : bufLen - 1;
        std::memcpy(buf, s.data(), n);
        buf[n] = 0;
    }
    return static_cast<uint32_t>(s.size());
}

std::string CurrentLevel() {
    char name[kLevelNameMax + 1] = {};
    if (!Peek(kLevelName, name, kLevelNameMax)) return "";
    return std::string(name, strnlen(name, kLevelNameMax));
}

bool StartsWith(const char* s, const char* prefix) { return _strnicmp(s, prefix, strlen(prefix)) == 0; }

uint32_t KindOf(const char* level) {
    if (!level || !*level) return PSE_DS1_LEVEL_NONE;
    if (StartsWith(level, "frontend") || StartsWith(level, "intro")) return PSE_DS1_LEVEL_FRONTEND;
    if (StartsWith(level, "outro")) return PSE_DS1_LEVEL_OUTRO;
    return PSE_DS1_LEVEL_MISSION;
}

// Level -> catalog key of its title. Longest prefixes first (mission11 also covers Mission11a, mission5 Mission5a).
struct LevelTitleKey {
    const char* prefix;
    const char* key;
};
constexpr LevelTitleKey kTitles[] = {
    {"mission11b", "MISS11B_TITLE"}, {"mission11", "MISS11_TITLE"}, {"mission12b", "MISS12B_TITLE"},
    {"mission12", "MISS12_TITLE"},   {"mission10", "MISS10_TITLE"}, {"mission1", "MISS1_TITLE"},
    {"mission2", "MISS2_TITLE"},     {"mission3", "MISS3_TITLE"},   {"mission4", "MISS4_TITLE"},
    {"mission5b", "MISS5B_TITLE"},   {"mission5", "MISS5_TITLE"},   {"mission6", "MISS6_TITLE"},
    {"mission7", "MISS7_TITLE"},     {"mission8", "MISS8_TITLE"},   {"mission9", "MISS9_TITLE"},
    {"training1", "TRAIN1_TITLE"},   {"training2", "TRAIN2_TITLE"}, {"training3", "TRAIN3_TITLE"},
};

uint32_t TitleHash(const char* level) {
    for (const auto& t : kTitles)
        if (StartsWith(level, t.prefix)) return pse_game_ds1::TextHash(t.key);
    return 0;
}

// The game's text for a hash (ANSI, as stored), nullptr if unknown / no table. Game thread.
// The lookup's binary search reads the table's key arrays (globals 0x640728 / 0x640734 / 0x640738), which a level load
// frees and rebuilds: asked mid-load (the loadout screen's late draw did, scripted level change with the screen up)
// it faulted at 0x569FD7. No arrays = no text; anything else that faults = no text, never a crash.
constexpr uint32_t kSearchHigh = 0x640728, kSearchHash = 0x640734, kSearchLow = 0x640738;
const char* GuardedLookup(void* table, uint32_t hash) {
    __try {
        return reinterpret_cast<const char*(__thiscall*)(void*, uint32_t)>(kLookup)(table, hash);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
const char* RawText(uint32_t hash) {
    void* table = reinterpret_cast<void*>(static_cast<uintptr_t>(Peek32(kStrings)));
    if (!hash || !table || !Peek32(kSearchHigh) || !Peek32(kSearchHash) || !Peek32(kSearchLow)) return nullptr;
    const char* text = GuardedLookup(table, hash);
    __try {
        return text && *text ? text : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

std::wstring Widen(const char* ansi) {
    const int n = MultiByteToWideChar(CP_ACP, 0, ansi, -1, nullptr, 0);
    if (n <= 0) return L"";
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, 0, ansi, -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}
std::string Utf8(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// "BASIC TRAINING" -> "Basic Training" (only when the text has no lower-case letter), control characters dropped,
// UTF-8. The old discord.cpp GameTitle without the JSON escaping.
std::string NiceTitle(const char* text) {
    std::wstring w = Widen(text);
    bool upper = true;
    for (wchar_t c : w)
        if (iswlower(c)) upper = false;
    if (upper)
        for (size_t i = 1; i < w.size(); ++i)
            if (iswalpha(w[i - 1])) w[i] = static_cast<wchar_t>(towlower(w[i]));
    std::string out;
    for (char c : Utf8(w))
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    return out;
}

// ---- the table ----
uint32_t PSE_CALL FrontEndState() { return Peek32(kFrontEndState); }
uint32_t PSE_CALL DispatchMode() { return Peek32(kDispatchMode); }
uint32_t PSE_CALL LevelName(char* buf, uint32_t bufLen) { return CopyOut(CurrentLevel(), buf, bufLen); }
uint32_t PSE_CALL LevelKind() { return KindOf(CurrentLevel().c_str()); }
int PSE_CALL InCutscene() {
    const uint32_t world = Peek32(kWorld);
    uint8_t flags = 0;
    return world && Peek(world + kCinematicOffset, &flags, 1) && (flags & 1) ? 1 : 0;
}
uint32_t PSE_CALL TextHashFn(const char* key) { return pse_game_ds1::TextHash(key); }
uint32_t PSE_CALL LookupText(uint32_t hash, char* buf, uint32_t bufLen) {
    const char* text = RawText(hash);
    return CopyOut(text ? Utf8(Widen(text)) : std::string(), buf, bufLen);
}
uint32_t PSE_CALL LevelTitle(char* buf, uint32_t bufLen) {
    return CopyOut(pse_game_ds1::LevelTitleUtf8(CurrentLevel().c_str()), buf, bufLen);
}
uint32_t PSE_CALL SplitScreenPlayers() {
    const int n = g_providers.splitScreenPlayers ? g_providers.splitScreenPlayers() : 0;
    return n > 0 ? static_cast<uint32_t>(n) : 0;
}
PSEDs1Online Online() {
    PSEDs1Online o{};
    o.size = sizeof o;
    if (g_providers.online) g_providers.online(&o);
    o.size = sizeof o;
    return o;
}
uint32_t PSE_CALL LocalPlayerCount() {
    if (LevelKind() != PSE_DS1_LEVEL_MISSION) return 0;
    const PSEDs1Online o = Online();
    if (o.active && o.localPlayer) return 1;
    const uint32_t split = SplitScreenPlayers();
    return split > 4 ? 4 : split > 1 ? split : 1;
}
int PSE_CALL GetLocalPlayer(uint32_t index, PSEDs1Player* out) {
    if (!out || out->size < sizeof(PSEDs1Player) || index >= LocalPlayerCount()) return 0;
    const PSEDs1Online o = Online();
    const uint32_t block = o.active && o.localPlayer ? o.localPlayer - 1 : index;
    if (block >= 4) return 0;
    const uint32_t soldier = Peek32(kBlocks + block * kBlockSize + kBlockSoldier);
    float pos[3], health;
    if (!soldier || !Peek(soldier + kSoldierPos, pos, sizeof pos) || !Peek(soldier + kSoldierHealth, &health, 4))
        return 0;
    out->block = block;
    out->soldier = soldier;
    out->position = {pos[0], pos[1], pos[2]};
    out->health = health;
    return 1;
}
int PSE_CALL GetOnline(PSEDs1Online* out) {
    if (!out || out->size < sizeof(PSEDs1Online)) return 0;
    *out = Online();
    return out->active ? 1 : 0;
}
void* PSE_CALL GameWindow() { return reinterpret_cast<void*>(static_cast<uintptr_t>(Peek32(kGameWindow))); }
const std::string& ExeHash() {
    static const std::string hash = [] {
        wchar_t path[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
        return n && n < MAX_PATH ? pse_game_ds1::ExeFileSha256(path) : std::string();
    }();
    return hash;
}
const char* PSE_CALL ExeSha256() { return ExeHash().c_str(); }
int PSE_CALL ExeKnown() { return ExeHash() == kKnownSha256 ? 1 : 0; }

const PSEGameDs1V1 kTable = {
    sizeof(PSEGameDs1V1), 1, &FrontEndState, &DispatchMode, &LevelName, &LevelKind, &InCutscene, &TextHashFn,
    &LookupText, &LevelTitle, &SplitScreenPlayers, &LocalPlayerCount, &GetLocalPlayer, &GetOnline, &GameWindow,
    &ExeSha256, &ExeKnown,
};

}  // namespace

// External linkage: the layer's PSEGame_Info points at it (ds1_layer.cpp).
const PSEPluginInfo pse_game_ds1::kPluginInfo = {sizeof(PSEPluginInfo), 1, PSE_GAME_DS1_PLUGIN,
                                                 "Desert Storm game interface", "PSE", PSE_MAKE_VERSION(1, 0, 0),
                                                 PSE_API_VERSION, nullptr, PSE_PLUGIN_CLIENT_ONLY};
namespace {
const PSEPluginInfo& kInfo = pse_game_ds1::kPluginInfo;
}  // namespace

void pse_game_ds1::SetProviders(const Providers& p) { g_providers = p; }
const PSEPluginInfo* pse_game_ds1::PluginInfo() { return &kInfo; }
const char* pse_game_ds1::KnownSha256() { return kKnownSha256; }
const PSEGameDs1V1* pse_game_ds1::Table() { return &kTable; }

PSEPluginHandle pse_game_ds1::Register() {
    PSEPluginHandle h = pse::FindPlugin(PSE_GAME_DS1_PLUGIN);
    if (!h) h = pse::RegisterInternalPlugin(&kInfo);
    if (h) pse::MessagingInterface()->RegisterInterface(h, PSE_GAME_DS1_INTERFACE, 1, &kTable);
    return h;
}

uint32_t pse_game_ds1::TextHash(const char* key) {
    uint32_t h = 1;  // archive LFSR (FUN_004b9a10) over tolower(key), like FUN_004b9ab0
    for (const char* p = key ? key : ""; *p; ++p) {
        const auto c = static_cast<unsigned char>(tolower(static_cast<unsigned char>(*p)));
        for (int b = 0; b < 8; ++b) {
            const uint32_t in = ((c >> b) ^ h ^ (h >> 1) ^ (h >> 21) ^ (h >> 31)) & 1;
            h = (h << 1) | in;
        }
    }
    return h;
}

std::string pse_game_ds1::LevelDisplayName(const char* level) {
    std::string name(level ? level : "");
    if (auto dot = name.find('.'); dot != std::string::npos) name.resize(dot);
    std::string prefix, rest;
    if (StartsWith(name.c_str(), "mp_mission")) prefix = "Multiplayer mission ", rest = name.substr(10);
    else if (StartsWith(name.c_str(), "mission")) prefix = "Mission ", rest = name.substr(7);
    else if (StartsWith(name.c_str(), "training")) prefix = "Training ", rest = name.substr(8);
    else return name;
    for (char& c : rest) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return prefix + rest;
}

std::string pse_game_ds1::LevelTitleText(const char* level) {
    const char* text = level ? RawText(TitleHash(level)) : nullptr;
    return text ? text : "";
}

std::string pse_game_ds1::LevelTitleUtf8(const char* level) {
    if (KindOf(level) != PSE_DS1_LEVEL_MISSION) return "";
    const char* text = RawText(TitleHash(level));
    const std::string name = text ? NiceTitle(text) : std::string();
    const std::string prefix = LevelDisplayName(level);
    if (name.empty()) return prefix;
    return StartsWith(level, "training") ? name : prefix + ": " + name;
}

std::string pse_game_ds1::ExeFileSha256(const wchar_t* path) {
    const HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER size{};
    std::vector<uint8_t> data;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (64 << 20)) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr) || got != data.size()) data.clear();
    }
    CloseHandle(f);
    if (data.size() < 0x40) return "";
    // IMAGE_FILE_HEADER.Characteristics (e_lfanew + 4 + 18): Large Address Aware 0x20 cleared, as the patcher does.
    uint32_t lfanew = 0;
    std::memcpy(&lfanew, &data[0x3C], 4);
    if (lfanew + 4 + 20 <= data.size()) data[lfanew + 4 + 18] &= static_cast<uint8_t>(~0x20);
    std::string out;
    HCRYPTPROV prov = 0;
    if (!CryptAcquireContextW(&prov, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return "";
    HCRYPTHASH hash = 0;
    if (CryptCreateHash(prov, CALG_SHA_256, 0, 0, &hash)) {
        BYTE digest[32];
        DWORD len = sizeof digest;
        if (CryptHashData(hash, data.data(), static_cast<DWORD>(data.size()), 0) &&
            CryptGetHashParam(hash, HP_HASHVAL, digest, &len, 0) && len == sizeof digest) {
            char hex[65];
            for (int i = 0; i < 32; ++i) std::snprintf(hex + i * 2, 3, "%02X", digest[i]);
            out = hex;
        }
        CryptDestroyHash(hash);
    }
    CryptReleaseContext(prov, 0);
    return out;
}

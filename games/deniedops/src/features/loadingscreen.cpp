// Loading-screen texts from a mod (2026-11-01, `deniedopsfix.loadingscreen`, under FeatMods): an operation's loading
// screen (SCR hash 0xC0647285, built by LoadingScreen.cpp) shows
//   - the picture of CampaignGame.xml's <loadingscreen><image imagename="m_BAS_LoadingMap.bmp"> = the texture
//     <OP>_LOADINGMAP of the package <OP>_HARDCODED (Packages\<OP>_HARDCODED.apak) - a mod replaces it as data (the
//     whole package served by name, see mods.cpp);
//   - the header LOADING (catalog key LOADING), the operation's caps title bottom right (opnamecapsid, e.g.
//     M_BAS_TITLE_CAPS = "MILITARY COMMAND BASE") and one gameplay tip at a time in the bar over the picture: the tip
//     table 0x98CAE8 (24-byte entries {+0 level, +4 text hash, +8 hash, +0xC..+0x14 control ids}, ended by +0 == 0x19; 71 entries, +8 = the pad wording when it differs)
//     read by 0x445520 through the catalog lookup 0x5EFD20.
// A mod changes those texts with `<OP>.loading` (OP = the current operation's dllname stem, record [0xA29290 +
// 0x10720C] +0x12C; the file served by name like any mod file), UTF-8 text:
//     # comment
//     KEY = text           a catalog key (LFSR hash of the lower-cased name) answered while the loading screen is the
//                          top screen (LOADING, M_BAS_TITLE_CAPS ...); 0xHASH = text works too
//     tip = text           the tips: every tip of the table shows these lines instead (tip i -> line i mod n);
//                          `tip =` with no text hides the tips
// Hooks the catalog lookup entry 0x5EFD20 (a broker chain with modsmenu.cpp's hook on the same entry).
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kLookup = 0x5EFD20;           // thiscall(catalog, hash) ret 4 -> wchar_t*
constexpr uint8_t kLookupEntry[] = {0x55, 0x8B, 0x6C, 0x24, 0x08};
constexpr uint32_t kFlow = 0xA29290, kOpCurrent = 0x10720C, kOpDll = 0x12C;
constexpr uint32_t kGui = 0xBD0C40;              // -> GUI manager
constexpr uint32_t kTopScreen = 0x6FB2E0;        // thiscall(gui, 4) -> top screen (+0x14 name hash)
constexpr uint32_t kLoadingScreenHash = 0xC0647285;
constexpr uint32_t kTipTable = 0x98CAE8, kTipStride = 24, kTipEnd = 0x19;
constexpr int kMaxTips = 512;   // 71 in the shipped exe

using LookupFn = const wchar_t*(__fastcall*)(void* catalog, void*, uint32_t hash);
LookupFn g_next = nullptr;

uint32_t Hash(const char* s) {
    uint32_t x = 1;
    for (; *s; ++s) {
        uint32_t c = static_cast<unsigned char>(*s);
        if (c >= 'A' && c <= 'Z') c += 32;
        for (int b = 0; b < 8; ++b) {
            uint32_t in = ((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1;
            x = (x << 1) | in;
        }
    }
    return x;
}

struct Texts {
    std::string op;                              // dllname the file belongs to ("" = none loaded)
    std::string path;
    FILETIME time = {};
    std::map<uint32_t, std::wstring> keys;
    std::vector<std::wstring> tips;
    bool hasTips = false;
};
std::mutex g_lock;   // the loading screen draws on its own thread
Texts g_texts;
std::vector<uint32_t> g_tipHashes;
DWORD g_checked = 0;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

void Parse(Texts& t, FILE* f) {
    char line[1024];
    bool first = true;
    while (fgets(line, sizeof line, f)) {
        char* p = line;
        if (first && static_cast<unsigned char>(p[0]) == 0xEF && static_cast<unsigned char>(p[1]) == 0xBB &&
            static_cast<unsigned char>(p[2]) == 0xBF)
            p += 3;   // UTF-8 BOM
        first = false;
        std::string s = Trim(p);
        if (s.empty() || s[0] == '#' || s[0] == ';' || (s.size() > 1 && s[0] == '-' && s[1] == '-')) continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Trim(s.substr(0, eq)), text = Trim(s.substr(eq + 1));
        if (key.empty()) continue;
        if (_stricmp(key.c_str(), "tip") == 0) {
            t.hasTips = true;
            if (!text.empty()) t.tips.push_back(Widen(text));
            continue;
        }
        uint32_t h = 0;
        if (key.size() > 2 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X'))
            h = static_cast<uint32_t>(strtoul(key.c_str() + 2, nullptr, 16));
        else
            h = Hash(key.c_str());
        if (h) t.keys[h] = Widen(text);
    }
}

bool ReadOpDll(char* buf, size_t size) {   // no C++ objects here (SEH)
    __try {
        const uintptr_t op = *reinterpret_cast<uintptr_t*>(kFlow + kOpCurrent);
        if (!op) return false;
        memcpy(buf, reinterpret_cast<const char*>(op + kOpDll), size - 1);
        buf[size - 1] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string CurrentOp() {
    char buf[64] = {};
    return ReadOpDll(buf, sizeof buf) ? std::string(buf) : std::string();
}

std::string Stem(const std::string& dll) {
    const size_t dot = dll.find_last_of('.');
    return dot == std::string::npos ? dll : dll.substr(0, dot);
}

// Reloads <OP>.loading when the operation or the file changed (checked at most every 500 ms). Under g_lock.
void Refresh() {
    const DWORD now = GetTickCount();
    if (g_checked && now - g_checked < 500) return;
    g_checked = now;
    const std::string op = CurrentOp();
    std::string path = op.empty() ? std::string() : features::ModFilePath((Stem(op) + ".loading").c_str());
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!path.empty() && !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) path.clear();
    if (op == g_texts.op && path == g_texts.path && CompareFileTime(&a.ftLastWriteTime, &g_texts.time) == 0) return;
    Texts t;
    t.op = op;
    t.path = path;
    t.time = a.ftLastWriteTime;
    FILE* f = nullptr;
    if (!path.empty() && !fopen_s(&f, path.c_str(), "rb") && f) {
        Parse(t, f);
        fclose(f);
        dslog::Write("Loading screen: %s for %s - %u text(s), %u tip line(s)%s", path.c_str(), op.c_str(),
                     static_cast<unsigned>(t.keys.size()), static_cast<unsigned>(t.tips.size()),
                     t.hasTips && t.tips.empty() ? " (tips hidden)" : "");
    }
    g_texts = std::move(t);
}

bool LoadingScreenOnTop() {
    __try {
        void* gui = *reinterpret_cast<void**>(kGui);
        if (!gui) return false;
        void* top = reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(kTopScreen)(gui, nullptr, 4);
        return top && *reinterpret_cast<uint32_t*>(static_cast<char*>(top) + 0x14) == kLoadingScreenHash;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int TipIndex(uint32_t hash) {
    for (size_t i = 0; i < g_tipHashes.size(); ++i)
        if (g_tipHashes[i] == hash) return static_cast<int>(i);
    return -1;
}

const wchar_t* __fastcall Lookup(void* catalog, void* edx, uint32_t hash) {
    if (hash && hash != 0xFFFFFFFFu) {
        std::lock_guard<std::mutex> lock(g_lock);
        Refresh();
        if (!g_texts.path.empty()) {
            const int tip = g_texts.hasTips ? TipIndex(hash) : -1;
            if (tip >= 0)   // tips only show on the loading screen (0x445520)
                return g_texts.tips.empty() ? L"" : g_texts.tips[tip % g_texts.tips.size()].c_str();
            auto it = g_texts.keys.find(hash);
            if (it != g_texts.keys.end() && LoadingScreenOnTop()) return it->second.c_str();
        }
    }
    return g_next(catalog, edx, hash);
}

void Apply() {
    bool ended = false;
    for (int i = 0; i < kMaxTips; ++i) {
        const uint8_t* e = reinterpret_cast<const uint8_t*>(kTipTable + i * kTipStride);
        if (*reinterpret_cast<const uint32_t*>(e) == kTipEnd) {
            ended = true;
            break;
        }
        g_tipHashes.push_back(*reinterpret_cast<const uint32_t*>(e + 4));
        const uint32_t pad = *reinterpret_cast<const uint32_t*>(e + 8);   // a pad / console wording of the same tip
        if (pad != g_tipHashes.back()) g_tipHashes.push_back(pad);
    }
    if (g_tipHashes.empty() || !ended) {
        dslog::Write("[fail] Loading screen: tip table 0x%X not as expected - no mod loading-screen texts", kTipTable);
        g_tipHashes.clear();
        return;
    }
    if (!patch::HookFunction(kLookup, reinterpret_cast<void*>(&Lookup), reinterpret_cast<void**>(&g_next), kLookupEntry,
                             sizeof kLookupEntry)) {
        dslog::Write("[fail] Loading screen: unexpected game code - no mod loading-screen texts");
        return;
    }
    dslog::Write("[ok]   Loading screen: <operation>.loading texts from mods (%u tip texts in the table)",
                 static_cast<unsigned>(g_tipHashes.size()));
}
}  // namespace

void features::ApplyLoadingScreen() {
    if (!Get().mods) {
        dslog::Write("[off]  Loading screen: off (FeatMods)");
        return;
    }
    if (Hash("SCR_GAME_MENU") != 0xF23EA323) {
        dslog::Write("[fail] Loading screen: name hash self-check failed");
        return;
    }
    AfterUnpack(&Apply);
}

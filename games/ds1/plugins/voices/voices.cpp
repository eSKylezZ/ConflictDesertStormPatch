// Squad voices: each squad soldier speaks with the SAS (British) or the Delta (American) actors, or a voice a .skin mod
// brings - VOICE row of the Squad Loadout screen, remembered per soldier (registry Voice<SP|Coop><role>: 0 auto =
// the uniform's accent or the skin's own voice, 1 SAS, 2 Delta, 3 the skin's own).
//
// A soldier's speech bank is soldier +0x40 (FUN_0055dd20(sound manager [0x63C9AC], UNITS.TXT col 11 "Bradley.sbk", 1)
// at 0x4228E5 / 0x4385AE). The level sound caches carry the other accent's copies <Name>_US / _UK.sbk (mods.cpp
// SoundCache) and the .skin voices' banks; while playing, every squad soldier is pointed at the bank he should use,
// looked up among the loaded banks only (FUN_0055e2b0(hash) - FUN_0055dd20 would make an empty bank for a missing
// name and try to load it from a file).
// Optional (settings featVoices), live: off = every squad soldier is pointed back at his own bank (UNITS.TXT's) and
// the loadout screen hides the VOICE row.
// Its own plugin since 2026-10-24 (PSE\Plugins\dsfix.voices.dll, dsfix kit): the choices are its hidden settings
// Voice<SP|Coop><role> (copied once from dsfix's key); FeatVoices / FeatPlayerVoice stay DesertStormFix's, the .skin
// voices, speech language and the host's choices online come from dsfix.services; publishes dsfix.voices v1.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "core/log.h"
#include "core/patch.h"
#include "dsfix_voices.h"
#include "generated/patch_tables.h"
#include "kit/kit.h"

namespace features {   // the count under its old name
constexpr int kVoiceCount = DSFIX_VOICE_COUNT;
}

namespace {
const DsfixServicesV1* g_services = nullptr;
bool Feat(const char* key) { return g_services->DsfixSetting(key) != 0; }
std::string SkinVoice(const char* texture) { return g_services->SkinVoice(texture); }
int PSE_CALL VoiceChoice(int role);   // below (the interface's Choice)
constexpr uint32_t kSoundManager = 0x63C9AC, kFindBank = 0x55E2B0;
constexpr uint32_t kCharacters = 0x63C9A0, kSquadTeam = 0x5F9B94;
constexpr uint32_t kSkinTable = 0x619848, kSkinCount = 0x6193A8, kSkinEntrySize = 0x5C;
constexpr uint32_t kLevelName = 0x606880, kDispatchMode = 0x60EC8C, kSplitFlag = 0x606410;
const char* const kHeroes[4] = {"Bradley", "Ramirez", "Jones", "Foley"};
const char* const kLabels[features::kVoiceCount] = {"AUTO", "SAS", "DELTA", "OWN (MOD)"};

int g_choice[2][4] = {};   // [co-op][role]
bool g_loaded[2] = {};
char g_level[32] = {};
std::unordered_map<std::string, void*> g_banks;   // bank name -> loaded bank this level (null = not loaded)

int Mode() { return *reinterpret_cast<int*>(kSplitFlag) ? 1 : 0; }

std::string ValueName(int role) {
    char name[32];
    snprintf(name, sizeof name, "Voice%s%d", Mode() ? "Coop" : "SP", role);
    return name;
}

void Load() {   // read every time: pse.settings values (cheap), so a change from elsewhere counts too
    const int m = Mode();
    g_loaded[m] = true;
    for (int r = 0; r < 4; ++r) {
        const int v = kit::GetInt(ValueName(r).c_str());   // the plugin's hidden settings (plugin Load declares them)
        if (v >= 0 && v < features::kVoiceCount) g_choice[m][r] = v;
    }
}

uint32_t ElfHash(const char* s) {   // FUN_004b9bf0, the sound bank key
    uint32_t h = 0;
    for (; *s; ++s) {
        h = (h << 4) + static_cast<unsigned char>(*s);
        if (const uint32_t g = h & 0xF0000000) h = (h & 0x0FFFFFFF) ^ (g >> 24);
    }
    return h;
}

#ifndef DS_DIST
// Dev check: plays from the last voice bank we assigned (sound play FUN_0055de20 = thiscall(manager, bank, id, ...);
// its entry `push esi; mov esi,ecx; mov ecx,[esp+8]` jumps to this counter first).
void* volatile g_watchBank = nullptr;
volatile LONG g_watchHits = 0;
volatile uint32_t g_watchId = 0;
void* g_playNext = nullptr;  // entry hook chains (patch::HookFunction): next subscriber / broker trampoline
// Dev SpeechLog = 1: every speech request FUN_004366d0 (thiscall(soldier, sample, quiet sample, AI only, not when dead,
// at a position, …) ret 0x18) for player 1's soldier is logged with its caller, and whether it reached the sound play
// (call at 0x4367E8, returns to 0x4367ED).
DWORD DevDword(const char* name, DWORD fallback) {
    DWORD v = fallback, size = sizeof v;
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}
volatile LONG g_speechPlays = 0;
volatile uint32_t g_speechLastId = 0;
volatile LONG g_playerPlays = 0;   // of them by player 1's soldier (esi = soldier at the play call)
bool g_speechLog = false;
void __cdecl NoteSpeech(uint8_t* soldier, const uint32_t* stack) {
    auto* player = *reinterpret_cast<uint8_t* const*>(0x60F5B8 + 0x310);
    if (soldier != player) return;
    dslog::Write("[dev] Speech: player soldier asked to say %X / %X (AI only %u, not dead %u, a5 %u a6 %u) from %08X; "
                 "%ld played so far", stack[1], stack[2], stack[3], stack[4], stack[5], stack[6], stack[0] - 5,
                 g_speechPlays);
}
void* g_speechNext = nullptr;
// FUN_00408680 (soldier's target tracking, from the combat update 0x407B60): calls for player 1's soldier
volatile LONG g_trackCalls = 0;
void* g_trackNext = nullptr;
__declspec(naked) void TrackStub() {
    __asm {
        mov eax, ds:[0x60F5B8 + 0x310]
        cmp eax, ecx
        jne other
        lock inc g_trackCalls
    other:
        jmp dword ptr [g_trackNext]
    }
}
__declspec(naked) void SpeechStub() {
    __asm {
        pushad
        lea eax, [esp + 32]
        push eax
        push ecx
        call NoteSpeech
        add esp, 8
        popad
        jmp dword ptr [g_speechNext]
    }
}

__declspec(naked) void PlayStub() {
    __asm {
        cmp dword ptr [esp], 0x4367ED
        jne notSpeech
        lock inc g_speechPlays
        mov eax, [esp + 8]
        mov g_speechLastId, eax
        mov eax, ds:[0x60F5B8 + 0x310]
        cmp eax, esi
        jne notSpeech
        lock inc g_playerPlays
    notSpeech:
        mov eax, [esp + 4]
        cmp eax, g_watchBank
        jne skip
        lock inc g_watchHits
        mov eax, [esp + 8]
        mov g_watchId, eax
    skip:
        jmp dword ptr [g_playNext]
    }
}
#endif

void* LoadedBank(const std::string& name) {
    auto it = g_banks.find(name);
    if (it != g_banks.end()) return it->second;
    void* mgr = *reinterpret_cast<void**>(kSoundManager);
    void* bank = mgr ? reinterpret_cast<void*(__thiscall*)(void*, uint32_t)>(kFindBank)(mgr, ElfHash(name.c_str())) : nullptr;
    g_banks[name] = bank;
    if (bank) dslog::Write("Voices: bank %s loaded", name.c_str());
    return bank;
}

// The skin table side (0 SAS, 1 Delta, 2 Russian, 3 Iraqi) of the soldier's skin entry.
int SideOf(const uint8_t* entry, int role) {
    for (int u = 0; u < 4; ++u) {
        const int n = reinterpret_cast<int*>(kSkinCount)[u * 4 + role];
        auto* table = reinterpret_cast<uint8_t**>(kSkinTable)[u * 4 + role];
        if (table && entry >= table && entry < table + n * kSkinEntrySize) return u;
    }
    return -1;
}

// The bank a squad soldier should speak with ("" = leave him as he is).
std::string WantedBank(uint8_t* soldier, int role, const char* own) {
    std::string hero;   // his own bank's name without ".sbk", if it's one of the squad's
    for (const char* h : kHeroes)
        if (_stricmp(own, (std::string(h) + ".sbk").c_str()) == 0) hero = h;
    if (hero.empty()) return "";
    const int host = g_services->HostValue("voices");   // online: the host's settings on every PC
    if (!(host >= 0 ? host != 0 : Feat("FeatVoices"))) return hero + ".sbk";   // off: his own bank (undoes an earlier assignment)
    auto* entry = *reinterpret_cast<uint8_t**>(soldier + 0x2A68);
    const std::string custom = entry ? SkinVoice(reinterpret_cast<const char*>(entry + 4)) : "";
    const int choice = VoiceChoice(role);
    if (!custom.empty() && (choice == 0 || choice == 3)) return custom + ".sbk";
    const int speech = g_services->SpeechLanguage();
    if (speech < 0) return "";   // not the English game: one voice
    const bool american = choice == 2 || (choice != 1 && entry && SideOf(entry, role) == 1);
    if (american == (speech == 6)) return hero + ".sbk";
    return hero + (american ? "_US" : "_UK") + ".sbk";
}

void UpdateSquad() {
    auto mgr = *reinterpret_cast<const uint8_t* const*>(kCharacters);
    if (!mgr) return;
    const uint8_t team = *reinterpret_cast<const uint8_t*>(kSquadTeam);
    if (team >= mgr[0x10] || 1 >= mgr[0x11]) return;
    auto lists = *reinterpret_cast<const uintptr_t* const*>(mgr + 4);
    if (!lists) return;
    int n = 0;
    for (uintptr_t s = lists[team * mgr[0x11] + 1]; s && n < 8; s = *reinterpret_cast<uintptr_t*>(s + 0x14), ++n) {
        auto* soldier = reinterpret_cast<uint8_t*>(s);
        auto* unit = *reinterpret_cast<uint8_t**>(soldier + 0x24);
        if (!unit) continue;
        const int role = *reinterpret_cast<int8_t*>(unit + 0x1C7);
        if (role < 0 || role > 3) continue;
        const std::string want = WantedBank(soldier, role, reinterpret_cast<const char*>(unit + 0x1A4));
        if (want.empty()) continue;
        void* bank = LoadedBank(want);
        void*& current = *reinterpret_cast<void**>(soldier + 0x40);
        if (bank && current != bank && current) {
            current = bank;
#ifndef DS_DIST
            g_watchBank = bank;
#endif
            dslog::Write("Voices: %s speaks with %s", reinterpret_cast<const char*>(unit + 0x1A4), want.c_str());
        }
    }
}
// ---- interface dsfix.voices (dsfix.dll's VOICE row and online token) ----
const char* PSE_CALL VoiceLabel(int v) { return kLabels[v < 0 || v >= features::kVoiceCount ? 0 : v]; }

int PSE_CALL VoiceChoice(int role) {
    Load();
    if (role < 0 || role > 3) return 0;
    char name[8] = "voice0";
    name[5] = static_cast<char>('0' + role);
    if (const int host = g_services->HostValue(name); host >= 0) return host;   // online: the host's
    return g_choice[Mode()][role];
}

int PSE_CALL CoopVoiceChoice(int role) {   // what this PC would send as host (co-op choices, whatever the mode now)
    if (role < 0 || role > 3) return 0;
    if (Mode() == 1) {
        Load();
        return g_choice[1][role];
    }
    char name[32];
    snprintf(name, sizeof name, "VoiceCoop%d", role);
    const int v = kit::GetInt(name);
    return v >= 0 && v < features::kVoiceCount ? v : 0;
}

void PSE_CALL SetVoiceChoice(int role, int v) {
    Load();
    if (role < 0 || role > 3) return;
    g_choice[Mode()][role] = v;
    kit::SetInt(ValueName(role).c_str(), v);
}

// What the VOICE row shows for a soldier: the choice, and for AUTO what it comes to.
std::string VoiceText(uint8_t* soldier, int role) {
    const int choice = VoiceChoice(role);
    auto* entry = *reinterpret_cast<uint8_t**>(soldier + 0x2A68);
    const bool custom = entry && !SkinVoice(reinterpret_cast<const char*>(entry + 4)).empty();
    if (choice == 3 || (choice == 0 && custom)) return choice == 0 ? "AUTO - OWN (MOD)" : kLabels[3];
    if (choice) return kLabels[choice];
    return entry && SideOf(entry, role) == 1 ? "AUTO - DELTA" : "AUTO - SAS";
}
uint32_t PSE_CALL VoiceTextInto(void* soldier, int role, char* buf, uint32_t bufLen) {
    const std::string t = soldier ? VoiceText(static_cast<uint8_t*>(soldier), role) : std::string("AUTO");
    if (buf && bufLen) strncpy_s(buf, bufLen, t.c_str(), _TRUNCATE);
    return static_cast<uint32_t>(t.size());
}

int PSE_CALL SoldierHasOwnVoice(void* soldier) {
    auto* entry = soldier ? *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(soldier) + 0x2A68) : nullptr;
    return entry && !SkinVoice(reinterpret_cast<const char*>(entry + 4)).empty() ? 1 : 0;
}

const DsfixVoicesV1 kInterface = {sizeof(DsfixVoicesV1), 1, &VoiceLabel, &VoiceChoice, &SetVoiceChoice,
                                  &CoopVoiceChoice, &VoiceTextInto, &SoldierHasOwnVoice};

// FeatPlayerVoice (live): patch_defs PLAYER_VOICE applied / reverted as the setting changes.
void SyncPlayerVoice() {
    static int state = -1;
    const bool on = Feat("FeatPlayerVoice");
    if (state == static_cast<int>(on)) return;
    state = on;
    const bool ok = on ? patch::ApplyGroup("Voices: player soldier talks", gen::kPlayerVoice)
                       : patch::RevertGroup("Voices: player soldier talks", gen::kPlayerVoice);
    dslog::Write(!ok ? "[fail] Voices: player soldier speech patch (unexpected game code)"
                 : on ? "[ok]   Voices: the soldier you control talks too" : "[off]  Voices: the game's speech rules");
}

void OnFrameVoices() {
    SyncPlayerVoice();
    const char* level = reinterpret_cast<const char*>(kLevelName);
    if (strncmp(level, g_level, sizeof g_level - 1) != 0) {   // a new level loads its own banks
        strncpy_s(g_level, level, sizeof g_level - 1);
        g_banks.clear();
    }
    if (*reinterpret_cast<int8_t*>(kDispatchMode) != 0) return;
    static int tick = 0;
    if (++tick % 15) return;   // a few times a second is plenty
#ifndef DS_DIST
    static LONG logged = 0;
    if (g_watchHits != logged) {
        logged = g_watchHits;
        dslog::Write("[dev] Voices: %ld plays from the assigned voice bank (last id %08X)", logged, g_watchId);
    }
    if (g_speechLog) {
        static LONG tracked = -1;
        static void* lastTarget = nullptr;
        auto* player = *reinterpret_cast<uint8_t* const*>(0x60F5B8 + 0x310);
        void* target = player ? *reinterpret_cast<void**>(player + 0x255C) : nullptr;
        if (g_trackCalls != tracked || target != lastTarget) {
            if ((g_trackCalls == 0) != (tracked <= 0) || target != lastTarget)
                dslog::Write("[dev] Speech: player target tracking calls %ld, target %p", g_trackCalls, target);
            tracked = g_trackCalls;
            lastTarget = target;
        }
    }
    static LONG spoken = 0;
    if (g_speechLog && g_speechPlays != spoken) {
        spoken = g_speechPlays;
        dslog::Write("[dev] Speech: %ld speech plays, %ld by the player soldier (last sample %X)", spoken, g_playerPlays,
                     g_speechLastId);
    }
#endif
    static int loggedOn = -1;
    if (loggedOn != static_cast<int>(Feat("FeatVoices"))) {
        loggedOn = Feat("FeatVoices");
        dslog::Write(loggedOn ? "[ok]   Voices: squad accents / skin voices" : "[off]  Voices: the soldiers' own speech banks");
    }
    __try {
        UpdateSquad();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dslog::Write("[fail] Voices: squad update faulted");
    }
}

void ApplyVoices() {
#ifndef DS_DIST
    static const uint8_t entry[] = {0x56, 0x8B, 0xF1, 0x8B, 0x4C, 0x24, 0x08};
    patch::HookFunction(0x55DE20, reinterpret_cast<const void*>(&PlayStub), &g_playNext, entry, sizeof entry);
    g_speechLog = kit::DevDword("SpeechLog", 0) != 0;
    static const uint8_t speech[] = {0x83, 0xEC, 0x0C, 0x53, 0x55};
    if (g_speechLog)
        patch::HookFunction(0x4366D0, reinterpret_cast<const void*>(&SpeechStub), &g_speechNext, speech, sizeof speech);
    static const uint8_t track[] = {0x53, 0x55, 0x56, 0x8B, 0xF1};
    if (g_speechLog)
        patch::HookFunction(0x408680, reinterpret_cast<const void*>(&TrackStub), &g_trackNext, track, sizeof track);
#endif
}

bool Load_() {
    g_services = kit::Services();
    if (!g_services) return false;
    for (const char* mode : {"SP", "Coop"})
        for (int r = 0; r < 4; ++r) {
            char name[32];
            snprintf(name, sizeof name, "Voice%s%d", mode, r);
            if (!kit::DeclareInt(name, 0, 0, features::kVoiceCount - 1)) return false;
        }
    ApplyVoices();
    if (!kit::OnFrame(&OnFrameVoices)) return false;
    return kit::Messaging()->RegisterInterface(kit::Self(), DSFIX_VOICES_INTERFACE, 1, &kInterface) == PSE_OK;
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.voices", "Squad voices", PSE_PLUGIN_SIMULATION, Load_)

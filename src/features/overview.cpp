// The mission's night-vision overview (a fly-over of the objectives with the radio narration, OBJ_Mn_*.psf), on
// demand from the mission briefing screen's OVERVIEW tab instead of by itself after the briefing.
//
// Every MissionN script plays it when the briefing (front-end state 0x14) is closed: the briefing's update sends
// script message 0x3B, the script runs MovieStart (API +0x594, 0x486A00), input off (+0x51C(0)) and its builder,
// which queues the shots through API +0x518 (queue [0x63C978]): night vision on (+0x658), letterbox, viewfinder overlay,
// per shot fade / level path camera (+0x4FC(n)) / narration stream, and at the end everything off again and MovieEnd
// (+0x598, 0x486F10), which sends script message 0x12 - the script's mission start set-up (AI, objectives, time).
//
// - The automatic one is skipped as it starts (cinematics.cpp's movie-cancel answer while OverviewSkipPending()), so
//   the script's own end chain and set-up still run.
// - The OVERVIEW tab replays it by calling the script's builder again (found in the loaded MissionN.dll as the
//   function that pushes the address of its `mov eax,[api]; jmp [eax+0x658]` thunk - the night-vision command, one
//   per script). MovieEnd's message 0x12 is held back meanwhile (`push 0x12; call [ecx+8]` at 0x486FDE -> our stub),
//   and the hour the builder's SetTOD calls change is put back afterwards. Esc / Enter / Cross / Circle skip it: the
//   queue is cleared and the end chain's calls are made here.
#include <windows.h>

#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kApi = 0x5ED238;   // script API table (cdecl functions)
constexpr uint32_t kMovieStart = 0x486A00, kSoundManager = 0x63C9AC, kStopStream = 0x55F820;
constexpr uint32_t kLevelName = 0x606880, kFrontEndState = 0x617C18, kGameState = 0x63C948, kHideFlag = 0x617DCC;
constexpr uint32_t kEnvironment = 0x63C948, kAtmosphere = 0x1A58;
constexpr uint32_t kMessage12 = 0x486FDE;   // push 0x12; call [ecx+8]  (5 bytes)
constexpr uint8_t kMessage12Bytes[] = {0x6A, 0x12, 0xFF, 0x51, 0x08};

char g_level[32] = {};
uint32_t g_builder = 0;      // the loaded script's overview builder (0 = none / not found)
bool g_searched = false;
bool g_briefingSeen = false, g_skipPending = false, g_skipSawMovie = false;
DWORD g_skipSince = 0;
bool g_playing = false;
DWORD g_playStart = 0;
float g_savedHour = 0, g_savedSpeed = 0, g_savedUntil = 0;
int g_savedRun = 0;
uint32_t g_prevKeys = 0, g_prevPads = 0;
volatile int g_hold12 = 0;   // MovieEnd's message 0x12 held back (our replay)

bool Cinematic() {
    auto* state = *reinterpret_cast<uint8_t**>(kGameState);
    return state && (*reinterpret_cast<uint32_t*>(state + 0x1B8C) & 1);
}
bool MissionLevel() { return _strnicmp(reinterpret_cast<const char*>(kLevelName), "Mission", 7) == 0; }
uint8_t* Atmosphere() {
    auto* env = *reinterpret_cast<uint8_t**>(kEnvironment);
    return env ? *reinterpret_cast<uint8_t**>(env + kAtmosphere) : nullptr;
}
void Api(uint32_t off, int a = 0) { reinterpret_cast<void(__cdecl*)(int)>(*reinterpret_cast<uint32_t*>(kApi + off))(a); }

// MovieEnd's message send: passed on, or (our replay) dropped with the stack as the caller's `add esp, 8` expects.
__declspec(naked) void Message12Stub() {
    __asm {
        cmp g_hold12, 0
        jne hold
        pop eax              // return address
        push 0x12
        push eax
        jmp dword ptr [ecx + 8]
    hold:
        pop eax
        push 0
        jmp eax
    }
}

// The builder in the loaded MissionN.dll: the function holding `push <thunk>` for its jmp [eax+0x658] thunk.
uint32_t FindBuilder() {
    char name[40] = {};
    strncpy_s(name, reinterpret_cast<const char*>(kLevelName), 31);
    if (char* dot = strchr(name, '.')) *dot = 0;
    strcat_s(name, ".dll");
    auto* base = reinterpret_cast<uint8_t*>(GetModuleHandleA(name));
    if (!base) return 0;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_CNT_CODE)) continue;
        uint8_t* t = base + sec->VirtualAddress;
        const uint32_t n = sec->Misc.VirtualSize;
        uint32_t thunk = 0;
        for (uint32_t o = 0; o + 11 <= n && !thunk; ++o)
            if (t[o] == 0xA1 && t[o + 5] == 0xFF && t[o + 6] == 0xA0 && *reinterpret_cast<uint32_t*>(t + o + 7) == 0x658)
                thunk = reinterpret_cast<uint32_t>(t + o);
        if (!thunk) return 0;
        for (uint32_t o = 0; o + 5 <= n; ++o) {
            if (t[o] != 0x68 || *reinterpret_cast<uint32_t*>(t + o + 1) != thunk) continue;
            uint32_t s = o;   // back to the function's start: after padding (CC / 90) or a ret before a usual first opcode
            auto first = [](uint8_t b) {
                return b == 0x55 || b == 0x53 || b == 0x56 || b == 0x57 || b == 0x83 || b == 0x8B || b == 0x6A || b == 0x51 ||
                       b == 0xA1 || b == 0x81 || b == 0x64;
            };
            while (s > 2 && !((t[s - 1] == 0xCC || t[s - 1] == 0x90) && (t[s - 2] == 0xCC || t[s - 2] == 0x90 || t[s - 2] == 0xC3)) &&
                   !(t[s - 1] == 0xC3 && first(t[s])))
                --s;
            return reinterpret_cast<uint32_t>(t + s);
        }
    }
    return 0;
}

void Finish() {
    if (uint8_t* a = Atmosphere()) {   // the builder's SetTOD calls (night, then a fixed hour) undone
        *reinterpret_cast<float*>(a + 0x3C) = g_savedHour;
        *reinterpret_cast<float*>(a + 0x40) = g_savedSpeed;
        *reinterpret_cast<int*>(a + 0x44) = g_savedRun;
        *reinterpret_cast<float*>(a + 0x48) = g_savedUntil;
    }
    g_playing = false;
    g_hold12 = 0;
    dslog::Write("Overview: finished after %.1f s", (GetTickCount() - g_playStart) / 1000.0);
}

// Skip: no more queued shots, and what the script's end chain does.
void Skip() {
    Api(0x560);         // clear the command queue
    if (void* mgr = *reinterpret_cast<void**>(kSoundManager)) reinterpret_cast<void(__thiscall*)(void*)>(kStopStream)(mgr);
    Api(0x7F8);         // viewfinder off
    Api(0x65C);         // night vision off
    Api(0x534);         // letterbox off
    Api(0x4FC, 0);      // the player's camera
    Api(0x4F4);         // camera reset
    Api(0x584);         // HUD on
    Api(0x51C, 1);      // input on
    Api(0x598);         // MovieEnd (message 0x12 held back)
    dslog::Write("Overview: skipped");
}

uint32_t SkipInput() {   // edges of Esc / Enter / Space and pad Cross / Circle
    uint32_t keys = 0;
    for (int vk : {VK_ESCAPE, VK_RETURN, VK_SPACE}) keys = keys << 1 | (features::KeyHeld(vk) ? 1u : 0u);
    uint32_t pads = 0;
    for (int j = 0; j < 8; ++j) pads |= features::ReadJoystick(j) & (1u << 1 | 1u << 2);
    const uint32_t edge = (keys & ~g_prevKeys) | (pads & ~g_prevPads);
    g_prevKeys = keys, g_prevPads = pads;
    return edge;
}
}  // namespace

bool features::OverviewSkipPending() { return g_skipPending; }
bool features::OverviewPlaying() { return g_playing; }

bool features::OverviewAvailable() {
    if (!MissionLevel()) return false;
    if (!g_searched) {
        g_searched = true;
        g_builder = FindBuilder();
        dslog::Write("Overview: %s builder %08X", g_level, g_builder);
    }
    return g_builder != 0;
}

void features::StartOverview() {
    if (g_playing || !OverviewAvailable()) return;
    if (uint8_t* a = Atmosphere()) {
        g_savedHour = *reinterpret_cast<float*>(a + 0x3C);
        g_savedSpeed = *reinterpret_cast<float*>(a + 0x40);
        g_savedRun = *reinterpret_cast<int*>(a + 0x44);
        g_savedUntil = *reinterpret_cast<float*>(a + 0x48);
    }
    g_hold12 = 1;
    g_playing = true;
    g_playStart = GetTickCount();
    g_prevKeys = ~0u, g_prevPads = ~0u;   // what's held now (the tab switch) doesn't skip it
    *reinterpret_cast<uint32_t*>(kHideFlag) = 0;
    reinterpret_cast<void(__cdecl*)()>(kMovieStart)();
    Api(0x51C, 0);
    reinterpret_cast<void(__cdecl*)(int)>(g_builder)(0);
    dslog::Write("Overview: replaying (%s)", g_level);
}

void features::ApplyOverview() {
    if (!patch::Matches(kMessage12, kMessage12Bytes, sizeof kMessage12Bytes)) {
        dslog::Write("[fail] Overview: MovieEnd message at 0x%08X not recognised - no replay", kMessage12);
        return;
    }
    uint8_t call[5] = {0xE8};
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&Message12Stub) - (kMessage12 + 5));
    memcpy(call + 1, &rel, 4);
    patch::Write(kMessage12, call, sizeof call);
}

void features::OnFrameOverview() {
    const char* level = reinterpret_cast<const char*>(kLevelName);
    if (strncmp(level, g_level, sizeof g_level - 1) != 0) {
        strncpy_s(g_level, level, sizeof g_level - 1);
        g_builder = 0, g_searched = false;
        g_briefingSeen = g_skipPending = g_skipSawMovie = false;
        g_playing = false, g_hold12 = 0;
    }
    if (!MissionLevel()) return;
    const uint32_t state = *reinterpret_cast<uint32_t*>(kFrontEndState);
    // the automatic overview: skipped from the moment the briefing closes until it has started and ended
    if (state == 0x14) g_briefingSeen = true;
    if (g_briefingSeen && state != 0x14 && !g_skipPending && !g_skipSawMovie) {
        g_skipPending = true;
        g_skipSince = GetTickCount();
    }
    if (g_skipPending) {
        if (Cinematic()) g_skipSawMovie = true;
        else if (g_skipSawMovie || GetTickCount() - g_skipSince > 10000) g_skipPending = false, g_skipSawMovie = true;
    }
    if (!g_playing) return;
    if (GetTickCount() - g_playStart > 1000 && !Cinematic()) {   // MovieEnd ran: the replay is over
        Finish();
        return;
    }
    if (SkipInput() && GetTickCount() - g_playStart > 500) {
        Skip();
        Finish();
    }
}

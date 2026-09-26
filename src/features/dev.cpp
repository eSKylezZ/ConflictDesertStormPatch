// Development-only hooks (compiled out of DS_DIST builds), driven by HKCU\Software\DesertStormFix\Dev.
//
// StartLevel (REG_SZ, e.g. "mission1.dll"): boot straight into that level instead of the front-end. The game's
// start-up options come from a built-in "command line" string (pointer at 0x5e7050 -> "-f -d"), parsed by
// FUN_0040fdc0: -f = front-end, a plain word = level to load (copied into the buffer at 0x5e6f28), otherwise the
// game starts in state 0xe (mission). The pointer is aimed at our own "-d <level>" string.
//
// DtClampLo / DtClampHi (DWORD, index range into kDtSites): those callers of the frame-time getter FUN_004BA480
// (float seconds, timer+0xA8) get at least 1/120 s - used to bisect which system breaks below 8 ms steps.
#include <windows.h>

#include <intrin.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

#ifndef DS_DIST
namespace {
constexpr uint32_t kDtGetter = 0x4BA480;
constexpr uint32_t kDtSites[] = {
    0x404B69, 0x4077DC, 0x40F58B, 0x40F755, 0x40FA0E, 0x4141B7, 0x4141CA, 0x419BFF, 0x419EB0, 0x419F34,
    0x41A687, 0x41F9FD, 0x42307C, 0x423089, 0x4239E8, 0x425C6C, 0x425DB9, 0x42608F, 0x42DFF2, 0x42E3ED,
    0x42E633, 0x432E1D, 0x432EB5, 0x43A0E0, 0x43B4F7, 0x43D9FD, 0x43DB25, 0x43EDC0, 0x43FEF3, 0x440435,
    0x4417A2, 0x443BA9, 0x443C09, 0x443CB6, 0x443D16, 0x44AD12, 0x44AD33, 0x44F52C, 0x44F6D4, 0x44F721,
    0x44FB0C, 0x44FC30, 0x45AD47, 0x45AD7E, 0x47398A, 0x489D3B, 0x48ABFA, 0x48C09B, 0x48F512, 0x49D73B,
    0x4CCCE7, 0x4D114E, 0x4D18D9, 0x4D1BE4, 0x4D3ABF, 0x4D3AEB, 0x4D419C, 0x4D6D28, 0x4D6F91, 0x4E32EA,
    0x4E332B, 0x4E94D6, 0x4EC89A, 0x4EC930, 0x4EFC6A, 0x4EFCBF, 0x4EFCE5, 0x4EFD0B, 0x4EFD31, 0x4F094C,
    0x4F0D06, 0x4F25BB, 0x4F3E31, 0x4F4B2C, 0x4F4B41, 0x4FEF8B, 0x4FF8AB, 0x4FFBA2, 0x501078, 0x502321,
    0x502379, 0x503AC9, 0x504090, 0x508216, 0x50822D, 0x508240, 0x50865A, 0x508C45, 0x508D0E, 0x50A6A5,
    0x50DCD6, 0x511AC4, 0x513CE7, 0x51671D, 0x519B6E, 0x521765, 0x538729, 0x53AA04, 0x55F312};

float __fastcall ClampedDt(const uint8_t* timer) {
    const float dt = *reinterpret_cast<const float*>(timer + 0xA8);
    return dt < 1.0f / 120 ? 1.0f / 120 : dt;
}

DWORD DevDword(const char* name, DWORD fallback) {
    DWORD v, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v,
                        &size) == ERROR_SUCCESS ? v : fallback;
}

// MoverLog (DWORD 1): logs FUN_0050DC20 (character mover) calls for moving characters during a cutscene.
constexpr uint32_t kMover = 0x50DC20;
constexpr uint32_t kMoverCalls[] = {0x42003C, 0x42A51C, 0x43FC63, 0x443058};
using MoverFn = int(__thiscall*)(float*, int, void*, int, int, float*);
int g_moverLogged = 0;

int __fastcall LoggedMover(float* m, void*, int a2, void* a3, int a4, int a5, float* a6) {
    auto state = *reinterpret_cast<const uint8_t**>(0x63C948);
    const bool cut = state && (state[0x1B8C] & 1);
    const auto* i = reinterpret_cast<const int*>(m);
    const float px = m[0x11], py = m[0x12], pz = m[0x13], ex = m[0x62], ey = m[0x63], ez = m[0x64], vy = m[0x45];
    const int grounded = i[0x48];
    const int r = reinterpret_cast<MoverFn>(kMover)(m, a2, a3, a4, a5, a6);
    const bool moving = ex != 0 || ey != 0 || ez != 0 || vy != 0 || grounded != i[0x48] || m[0x12] != py;
    if (cut && moving && g_moverLogged < 4000) {
        ++g_moverLogged;
        const float dt = *reinterpret_cast<const float*>(*reinterpret_cast<const uint8_t**>(0x63C954) + 0xA8);
        dslog::Write("mover %p dt=%.4f pos %.1f,%.1f,%.1f -> %.1f,%.1f,%.1f ext %.2f,%.2f,%.2f vy %.2f->%.2f gnd %d->%d "
                     "speed %.2f/%.2f f65=%d f54=%d ret %d",
                     m, dt, px, py, pz, m[0x11], m[0x12], m[0x13], ex, ey, ez, vy, m[0x45], grounded, i[0x48], m[0x1E],
                     m[0x1F], i[0x65], i[0x54], r);
    }
    return r;
}

// TextProbe (DWORD 1): logs each distinct caller of the text draw FUN_0053a7b0 (font, text, x, baseline y, flag)
// with its text and position, and the font's sheet scale - to find which code draws a piece of text.
constexpr uint32_t kTextDraw = 0x53A7B0, kTextDrawCont = 0x53A7B5;
uint32_t g_textCallers[256];
int g_textCallerCount = 0;

void __cdecl TextProbe(uint32_t caller, const uint8_t* font, const char* text, int x, int y) {
    static char seen[64][48];
    static int seenCount = 0;
    if (caller - 5 == 0x508027 && text) {  // the text queue: every distinct string
        for (int i = 0; i < seenCount; ++i)
            if (strncmp(seen[i], text, 47) == 0) return;
        if (seenCount < 64) strncpy_s(seen[seenCount++], text, 47);
        const uint8_t* sheet = font ? *reinterpret_cast<const uint8_t* const*>(font) : nullptr;
        dslog::Write("[dev]  queued text font %p scale %.2f at %d,%d \"%.40s\"", font,
                     sheet ? *reinterpret_cast<const float*>(sheet + 0x24) : 0.0f, x, y, text);
        return;
    }
    for (int i = 0; i < g_textCallerCount; ++i)
        if (g_textCallers[i] == caller) return;
    if (g_textCallerCount >= 256) return;
    g_textCallers[g_textCallerCount++] = caller;
    const uint8_t* sheet = font ? *reinterpret_cast<const uint8_t* const*>(font) : nullptr;
    dslog::Write("[dev]  text 0x%06X font %p scale %.2f at %d,%d \"%.40s\"", caller - 5, font,
                 sheet ? *reinterpret_cast<const float*>(sheet + 0x24) : 0.0f, x, y, text ? text : "(null)");
}

__declspec(naked) void TextDrawStub() {
    __asm {
        pushad
        push dword ptr [esp + 0x2C]  // y
        push dword ptr [esp + 0x2C]  // x
        push dword ptr [esp + 0x2C]  // text
        push ecx                     // font (ecx unchanged by pushad)
        push dword ptr [esp + 0x30]  // return address
        call TextProbe
        add esp, 20
        popad
        mov eax, dword ptr ds:[0x5FCCAC]
        push kTextDrawCont
        ret
    }
}

void ApplyDtClamp() {
    if (DevDword("TextProbe", 0)) patch::WriteJump(kTextDraw, reinterpret_cast<const void*>(&TextDrawStub));
    if (DevDword("MoverLog", 0))
        for (uint32_t site : kMoverCalls) patch::HookCall(site, &LoggedMover, kMover);
    const DWORD lo = DevDword("DtClampLo", 0), hi = std::min<DWORD>(DevDword("DtClampHi", 0), std::size(kDtSites));
    for (DWORD i = lo; i < hi; ++i) patch::HookCall(kDtSites[i], &ClampedDt, kDtGetter);
    if (lo < hi) dslog::Write("[dev]  dt >= 1/120 s for getter callers %lu..%lu", lo, hi - 1);
}
}  // namespace
#endif

#ifndef DS_DIST
namespace {
// PlayLog (DWORD 1): play-test logging - every time-scale change (FUN_004ba4e0 replaced: timer +0xd8 = 0,
// +0x28 = scale; logs the caller) and, per frame, each player block's soldier / view / devices when they change.
void __fastcall SetTimeScale(uint8_t* timer, void*, float scale) {
    const float old = *reinterpret_cast<float*>(timer + 0x28);
    *reinterpret_cast<uint32_t*>(timer + 0xD8) = 0;
    *reinterpret_cast<float*>(timer + 0x28) = scale;
    static bool traced = false;
    if (scale < 0.999f && old >= 0.999f && !traced) {  // the first slow-down: who started it (code addresses on the stack)
        traced = true;
        auto* stack = static_cast<uint32_t*>(_AddressOfReturnAddress());
        char chain[512] = "";
        size_t len = 0;
        for (int i = 0; i < 64 && len < sizeof chain - 12; ++i)
            if (stack[i] >= 0x401000 && stack[i] < 0x5D7000)
                len += snprintf(chain + len, sizeof chain - len, " %06X", stack[i]);
        dslog::Write("[dev]  slow-down started, stack:%s", chain);
    }
    if ((scale < 0.999f) != (old < 0.999f))
        dslog::Write("[dev]  time scale %.2f -> %.2f from 0x%p", old, scale, _ReturnAddress());
}
bool g_playLog = false;

void WatchPlayers() {
    static uintptr_t lastSoldier[4];
    static uint32_t lastView[4], lastDevices[4];
    static char lastLevel[32];
    const char* level = reinterpret_cast<const char*>(0x606880);
    if (strncmp(level, lastLevel, sizeof lastLevel - 1) != 0) {
        strncpy_s(lastLevel, level, sizeof lastLevel - 1);
        dslog::Write("[dev]  level %s, players %u", level, *reinterpret_cast<uint16_t*>(0x610798));
    }
    const int players = *reinterpret_cast<uint16_t*>(0x610798);
    for (int i = 0; i < players && i < 4; ++i) {
        const uintptr_t block = 0x60F5B8 + i * 0x478;
        const uintptr_t soldier = *reinterpret_cast<uintptr_t*>(block + 0x310);
        const uint32_t view = *reinterpret_cast<uint32_t*>(block + 0x474), devices = *reinterpret_cast<uint32_t*>(block + 0x38);
        if (soldier != lastSoldier[i] || view != lastView[i] || devices != lastDevices[i]) {
            dslog::Write("[dev]  player %d: soldier %p (squad slot %d) view %u devices %u joystick %d", i + 1,
                         reinterpret_cast<void*>(soldier),
                         soldier ? *reinterpret_cast<int8_t*>(*reinterpret_cast<uintptr_t*>(soldier + 0x24) + 0x1C7) : -1,
                         view, devices, *reinterpret_cast<int32_t*>(block + 0x3C8));
            lastSoldier[i] = soldier, lastView[i] = view, lastDevices[i] = devices;
        }
    }
}

}  // namespace
#endif

void features::ApplyDevHooks() {
#ifndef DS_DIST
    ApplyDtClamp();
    g_playLog = DevDword("PlayLog", 0) != 0;
    if (g_playLog) {
        static const uint8_t entry[] = {0x8B, 0x44, 0x24, 0x04, 0xC7, 0x81, 0xD8, 0x00, 0x00, 0x00};
        if (patch::Matches(0x4BA4E0, entry, sizeof entry)) patch::WriteJump(0x4BA4E0, reinterpret_cast<const void*>(&SetTimeScale));
        dslog::Write("[dev]  play-test logging on");
    }
    constexpr uint32_t kOptionsPtr = 0x5E7050, kOptionsDefault = 0x5E708C;  // -> "-f -d"
    static char options[64];
    char level[32] = {};
    DWORD size = sizeof level;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "StartLevel", RRF_RT_REG_SZ, nullptr, level,
                     &size) != ERROR_SUCCESS || !level[0] || strlen(level) > 20)
        return;
    const uint32_t expected = kOptionsDefault;
    if (!patch::Matches(kOptionsPtr, &expected, 4)) return;
    snprintf(options, sizeof options, "-d %s", level);
    patch::WriteValue(kOptionsPtr, reinterpret_cast<uint32_t>(options));
    dslog::Write("[dev]  Start level: %s", level);
#endif
}

// Dev: F9 in a mission shows a test message in the pop-up bar (FUN_004d33a0 on the view controller [0x63c98c]: text,
// duration ms) - the bar used for picked-up items and warnings.
void features::OnFrameDev() {
#ifndef DS_DIST
    if (g_playLog) WatchPlayers();
    {
        static uint32_t last = 0xFFFFFFFF;
        const uint32_t v = *reinterpret_cast<uint32_t*>(0x60FA00);
        if (v != last) {
            dslog::Write("[dev]  mouse acceleration global = %u (front-end state 0x%X)", v, *reinterpret_cast<uint32_t*>(0x617C18));
            last = v;
        }
    }
    static bool down = false;
    const bool now = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    if (now && !down) {
        void* controller = *reinterpret_cast<void**>(0x63C98C);
        if (controller)
            reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(0x4D33A0)(controller, "You have killed a civilian", 5000);
    }
    down = now;
#endif
}

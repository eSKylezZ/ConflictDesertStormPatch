// Dev builds only: scripted menu input for background tests. The game reads the keyboard from window messages
// (window proc 0x4777A0 -> 0x65BAE0(msg, wParam, lParam): key table 0xC4CB98 by scan code | extended << 8), which an
// unfocused window doesn't get - so Dev\KeyScript (string) feeds that handler itself, on the game thread, from the
// per-frame input update 0x65BAC0 (entry hook). Tokens, space separated, run in order:
//   w<ms>   wait            up down left right enter esc space   press (held 120 ms, then 150 ms gap)
//   k<hex>  press that scan code (| 0x100 = extended key)
//   continue  wait for the level loading screen's CONTINUE, then press it (message 0x25F)
//   menu    go to the main menu (SCR_GAME_MENU) the way its own code does (0x6FB7C0) - skips the profile screen
// The clock starts at the first input update after the front-end GUI exists ([0xBD0C40]).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

#ifdef DS_DIST
void features::ApplyDevInput() {}
#else
namespace {
constexpr uint32_t kInputUpdate = 0x65BAC0, kKeyHandler = 0x65BAE0, kSelectScreen = 0x6FB7C0;

struct Step {
    int kind;        // 0 wait, 1 key, 2 main menu, 3 loading screen CONTINUE
    uint32_t value;  // ms / scan code | extended << 8
};
std::vector<Step> g_steps;
size_t g_next = 0;
DWORD g_due = 0;
bool g_started = false;
uint32_t g_held = 0;   // key to release, 0 none
DWORD g_release = 0;

void Key(uint32_t code, bool down) {
    uint32_t lp = (code << 16) | 1;
    if (!down) lp |= 0xC0000000;
    if (code & 0x100) lp |= 1u << 24;
    UINT vk = MapVirtualKeyA((code & 0xFF) | ((code & 0x100) ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    reinterpret_cast<void(__cdecl*)(int, int, int)>(kKeyHandler)(down ? WM_KEYDOWN : WM_KEYUP, static_cast<int>(vk),
                                                                 static_cast<int>(lp));
}

using UpdateFn = void(__cdecl*)();
UpdateFn g_nextUpdate = nullptr;

void __cdecl InputUpdate() {
    g_nextUpdate();
    void* gui = *reinterpret_cast<void**>(0xBD0C40);
    if (!gui) return;
    DWORD now = GetTickCount();
    if (!g_started) {
        g_started = true;
        g_due = now;
        dslog::Write("[dev] KeyScript: %zu step(s)", g_steps.size());
    }
    if (g_held && now >= g_release) {
        Key(g_held, false);
        g_held = 0;
    }
    while (g_next < g_steps.size() && now >= g_due && !g_held) {
        const Step& s = g_steps[g_next++];
        if (s.kind == 0) {
            g_due = now + s.value;
        } else if (s.kind == 1) {
            Key(s.value, true);
            g_held = s.value;
            g_release = now + 120;
            g_due = now + 270;
            dslog::Write("[dev] KeyScript: key %03X", s.value);
        } else if (s.kind == 3) {
            // Loading screen (SCR hash 0xC0647285, vtable 0x92A5BC): once its CONTINUE is up (+0x88 != 0), send it the
            // message its button sends (0x25F -> 0x5A7300). Waits (re-checks every frame) until then.
            void* top = reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(0x6FB2E0)(gui, nullptr, 4);
            if (!top || *reinterpret_cast<uint32_t*>(static_cast<char*>(top) + 0x14) != 0xC0647285 ||
                !*reinterpret_cast<uint32_t*>(static_cast<char*>(top) + 0x88)) {
                --g_next;
                break;
            }
            void** vt = *reinterpret_cast<void***>(top);
            reinterpret_cast<int(__fastcall*)(void*, void*, int, int)>(vt[3])(top, nullptr, 0x25F, 0);
            dslog::Write("[dev] KeyScript: loading screen CONTINUE");
            g_due = now + 500;
        } else {
            reinterpret_cast<void(__fastcall*)(void*, void*, uint32_t, int, int)>(kSelectScreen)(gui, nullptr, 0xF23EA323, 4, 0);
            dslog::Write("[dev] KeyScript: main menu");
            g_due = now + 500;
        }
    }
}

void Apply() {
    // The input update's entry (called from the level loop and the front end's loops alike): call 0x65B880.
    constexpr uint8_t kEntry[] = {0xE8, 0xBB, 0xFD, 0xFF, 0xFF};
    if (patch::HookFunction(kInputUpdate, reinterpret_cast<void*>(&InputUpdate), reinterpret_cast<void**>(&g_nextUpdate),
                            kEntry, sizeof kEntry))
        dslog::Write("[dev] KeyScript: input hook installed");
}
}  // namespace

void features::ApplyDevInput() {
    char buf[2048] = {};
    DWORD size = sizeof buf;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DeniedOpsFix\\Dev", "KeyScript", RRF_RT_REG_SZ, nullptr, buf, &size) !=
            ERROR_SUCCESS || !buf[0])
        return;
    struct Name {
        const char* name;
        uint32_t code;
    };
    const Name names[] = {{"up", 0x148}, {"down", 0x150}, {"left", 0x14B}, {"right", 0x14D}, {"enter", 0x1C}, {"esc", 0x01}, {"space", 0x39}};
    char* ctx = nullptr;
    for (char* t = strtok_s(buf, " ", &ctx); t; t = strtok_s(nullptr, " ", &ctx)) {
        if (t[0] == 'w' && t[1] >= '0' && t[1] <= '9') {
            g_steps.push_back({0, static_cast<uint32_t>(atoi(t + 1))});
        } else if (t[0] == 'k' && t[1]) {   // k<hex>: any scan code (| 0x100 = extended)
            g_steps.push_back({1, static_cast<uint32_t>(strtoul(t + 1, nullptr, 16))});
        } else if (!_stricmp(t, "continue")) {
            g_steps.push_back({3, 0});
        } else if (!_stricmp(t, "menu")) {
            g_steps.push_back({2, 0});
        } else {
            for (const Name& n : names)
                if (!_stricmp(t, n.name)) g_steps.push_back({1, n.code});
        }
    }
    if (!g_steps.empty()) AfterUnpack(&Apply);
}
#endif

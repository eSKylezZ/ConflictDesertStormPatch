// Dev builds only: scripted menu input for background tests. The game reads the keyboard from window messages
// (window proc 0x4777A0 -> 0x65BAE0(msg, wParam, lParam): key table 0xC4CB98 by scan code | extended << 8), which an
// unfocused window doesn't get - so Dev\KeyScript (string) feeds that handler itself, on the game thread, from the
// per-frame input update 0x65BAC0 (entry hook). Tokens, space separated, run in order:
//   w<ms>   wait            up down left right enter esc space   press (held 120 ms, then 150 ms gap)
//   k<hex>  press that scan code (| 0x100 = extended key)
//   hold:<key>[+<key>...]:<ms>  hold keys (names below or hex scan codes) for ms - e.g. hold:w:2000 walks forward,
//           hold:w+left:1500 runs while turning (default.key: TURN_LEFT / TURN_RIGHT = the arrow keys)
//   continue  wait for the level loading screen's CONTINUE, then press it (message 0x25F)
//   menu    go to the main menu (SCR_GAME_MENU) the way its own code does (0x6FB7C0) - skips the profile screen
//   win     mission success: the mission scripts' own end-mission call (script API +0xCB4 = 0x48EB70 with 0x10);
//           waits until a mission script exists
//   op:<dll>  start that mission (features::StartMission, campaigns.cpp); op:<mod>:<n> = mission n (0-based) of
//           Mods\<mod>\campaign.txt, chained like a pick in the CUSTOM CAMPAIGNS lists
//   path:<script id>:<waypoint id>  send that soldier along the waypoint chain from there: script API +0x5DC =
//           0x4925B0 cdecl(script id, waypoint id) - what the .SI event SIEV_AssignHLOToNewWaypoint runs
//           (CSIEVAssignHLOToNewWaypoint vtable 0x91CD28, execute 0x4A0120)
// The clock starts at the first input update after the front-end GUI exists ([0xBD0C40]).
// Dev\FlowLog = 1 logs the game flow object's state changes (campaigns.cpp hooks into that flow).
#include <windows.h>

#include <cstdint>
#include <cctype>
#include <cstdio>
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
    int kind;        // 0 wait, 1 key, 2 main menu, 3 loading screen CONTINUE, 4 mission success, 5 start a mission,
                     // 6 hold a key, 7 send a soldier to a waypoint (value = index into g_paths)
    uint32_t value;  // ms / scan code | extended << 8 / index into g_texts
};
std::vector<Step> g_steps;
std::vector<std::string> g_texts;
struct Hold {
    unsigned ms;
    std::vector<uint32_t> keys;   // held together (hold:w+left:2000)
};
std::vector<Hold> g_holds;   // hold steps (step value = index)
std::vector<std::pair<int, int>> g_paths;   // path steps: (script id, waypoint id)
LONG CALLBACK FaultLog(EXCEPTION_POINTERS* e);

// Dev\FlowLog = 1: every change of the game flow object 0xA29290 (states +0x1097B4 / +0x1097B8 / +0x1097BC, after-
// state +0x1097D0, current operation +0x10720C and its dllname +0x12C, picked operation id +0x109870, single-op flag
// [0x9D77E0]), of the top GUI screen and of the mission script object [0x9F2B3C] - one line per change.
void FlowLog() {
    static const bool on = [] {
        DWORD v = 0, size = sizeof v;
        return RegGetValueA(HKEY_CURRENT_USER, "Software\\DeniedOpsFix\\Dev", "FlowLog", RRF_RT_REG_DWORD, nullptr, &v,
                            &size) == ERROR_SUCCESS && v;
    }();
    if (!on) return;
    static bool veh = (AddVectoredExceptionHandler(1, &FaultLog), true);
    (void)veh;
    const uint8_t* flow = reinterpret_cast<const uint8_t*>(0xA29290);
    auto rd = [&](uint32_t off) { return *reinterpret_cast<const uint32_t*>(flow + off); };
    void* gui = *reinterpret_cast<void**>(0xBD0C40);
    uint32_t top = 0;
    if (gui) {
        void* s = reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(0x6FB2E0)(gui, nullptr, 4);
        if (s) top = *reinterpret_cast<uint32_t*>(static_cast<char*>(s) + 0x14);
    }
    uint32_t now[] = {rd(0x1097B4), rd(0x1097B8), rd(0x1097BC), rd(0x1097D0), rd(0x10720C), rd(0x109870),
                      *reinterpret_cast<uint32_t*>(0x9D77E0), top, *reinterpret_cast<uint32_t*>(0x9F2B3C)};
    static uint32_t last[9] = {0xFFFFFFFF};
    if (!memcmp(now, last, sizeof now)) return;
    memcpy(last, now, sizeof now);
    const char* dll = now[4] ? reinterpret_cast<const char*>(now[4] + 0x12C) : "-";
    dslog::Write("[dev] Flow: state %u / %u / %u after %u op %08X (%s) pick %d single %u screen %08X script %08X", now[0],
                 now[1], now[2], now[3], now[4], dll, static_cast<int>(now[5]), now[6], now[7], now[8]);
}
size_t g_next = 0;
DWORD g_due = 0;
bool g_started = false;
std::vector<uint32_t> g_held;   // keys to release
DWORD g_release = 0;

void Key(uint32_t code, bool down) {
    uint32_t lp = (code << 16) | 1;
    if (!down) lp |= 0xC0000000;
    if (code & 0x100) lp |= 1u << 24;
    UINT vk = MapVirtualKeyA((code & 0xFF) | ((code & 0x100) ? 0xE000 : 0), MAPVK_VSC_TO_VK_EX);
    reinterpret_cast<void(__cdecl*)(int, int, int)>(kKeyHandler)(down ? WM_KEYDOWN : WM_KEYUP, static_cast<int>(vk),
                                                                 static_cast<int>(lp));
}

// Dev\FlowLog also logs the first access violations (EIP, address, stack words that look like exe code) - the game
// ends without a crash dump.
LONG CALLBACK FaultLog(EXCEPTION_POINTERS* e) {
    static LONG count = 0;
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    // any exception but the debugger chatter (OutputDebugString 0x40010006 / 0x4001000A, thread names 0x406D1388):
    // a load job that throws (C++ 0xE06D7363 ...) is caught by the job pool and the level never finishes loading
    if (code == 0x40010006 || code == 0x4001000A || code == 0x406D1388 || InterlockedIncrement(&count) > 8)
        return EXCEPTION_CONTINUE_SEARCH;
    const CONTEXT* c = e->ContextRecord;
    char line[512];
    int n = code == EXCEPTION_ACCESS_VIOLATION
                ? sprintf_s(line, "[dev] Fault: EIP %08lX %s %08IX, eax %08lX ecx %08lX edx %08lX esi %08lX edi %08lX, stack",
                            c->Eip, e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                            e->ExceptionRecord->ExceptionInformation[1], c->Eax, c->Ecx, c->Edx, c->Esi, c->Edi)
                : sprintf_s(line, "[dev] Exception %08lX at %p (thread %lu), eax %08lX ecx %08lX esi %08lX edi %08lX, stack",
                            code, e->ExceptionRecord->ExceptionAddress, GetCurrentThreadId(), c->Eax, c->Ecx, c->Esi,
                            c->Edi);
    if (code == 0xE06D7363 && e->ExceptionRecord->NumberParameters >= 3) {   // MSVC throw: the thrown type's name
        __try {   // ThrowInfo +0xC -> CatchableTypeArray +4 -> CatchableType +4 -> TypeDescriptor +8 (name)
            const uint8_t* ti = reinterpret_cast<const uint8_t*>(e->ExceptionRecord->ExceptionInformation[2]);
            const uint8_t* cta = *reinterpret_cast<const uint8_t* const*>(ti + 0xC);
            const uint8_t* ct = *reinterpret_cast<const uint8_t* const*>(cta + 4);
            const char* name = reinterpret_cast<const char*>(*reinterpret_cast<const uint8_t* const*>(ct + 4) + 8);
            n += sprintf_s(line + n, sizeof line - n, " (C++ %.60s)", name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    const uint32_t* sp = reinterpret_cast<const uint32_t*>(c->Esp);
    for (int i = 0, shown = 0; i < 256 && shown < 12 && n < 480; ++i) {
        uint32_t v = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, 4, nullptr)) break;
        if (v >= 0x401000 && v < 0x90E000) n += sprintf_s(line + n, sizeof line - n, " %08X", v), ++shown;
    }
    dslog::Write("%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

using UpdateFn = void(__cdecl*)();
UpdateFn g_nextUpdate = nullptr;

void __cdecl InputUpdate() {
    g_nextUpdate();
    FlowLog();
    void* gui = *reinterpret_cast<void**>(0xBD0C40);
    if (!gui) return;
    DWORD now = GetTickCount();
    if (!g_started) {
        g_started = true;
        g_due = now;
        dslog::Write("[dev] KeyScript: %zu step(s)", g_steps.size());
    }
    if (!g_held.empty() && now >= g_release) {
        for (uint32_t k : g_held) Key(k, false);
        g_held.clear();
    }
    while (g_next < g_steps.size() && now >= g_due && g_held.empty()) {
        const Step& s = g_steps[g_next++];
        if (s.kind == 0) {
            g_due = now + s.value;
        } else if (s.kind == 1) {
            Key(s.value, true);
            g_held = {s.value};
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
        } else if (s.kind == 4) {
            // Mission success the way the debug key "Successful completion of mission" (0x4389C0) sends it: message
            // 0x31 to the mission script object [0x9F2B3C] (its +4 = message handler, cdecl(msg, param)).
            void** script = *reinterpret_cast<void***>(0x9F2B3C);
            if (!script) {
                --g_next;   // no mission running yet: wait
                break;
            }
            // The debug key / the debug screen CCompleteMission only send message 0x31, and the mission DLLs act on it
            // only with their own debug flag set (M_CMB: [0x1000F398] == 1 -> api +0xCB4 (0x10)). So this calls that
            // script API slot itself: +0xCB4 = 0x48EB70 cdecl(result) = end the mission (0x48BC20(0), then
            // 0x459A20 thiscall([0xA08B5C], result, 0)); 0x10 = success.
            reinterpret_cast<void(__cdecl*)(int)>(0x48EB70)(0x10);
            dslog::Write("[dev] KeyScript: mission success (script API +0xCB4, 0x10)");
            g_due = now + 500;
        } else if (s.kind == 6) {   // hold: value = index into g_holds
            const Hold& h = g_holds[s.value];
            for (uint32_t k : h.keys) Key(k, true);
            g_held = h.keys;
            g_release = now + h.ms;
            g_due = g_release + 150;
            dslog::Write("[dev] KeyScript: hold key %03X (+%zu more) for %u ms", h.keys[0], h.keys.size() - 1, h.ms);
        } else if (s.kind == 7) {
            const auto [id, wp] = g_paths[s.value];
            reinterpret_cast<void(__cdecl*)(int, int)>(0x4925B0)(id, wp);
            dslog::Write("[dev] KeyScript: path - soldier %d to waypoint %d (script API +0x5DC)", id, wp);
            g_due = now + 100;
        } else if (s.kind == 5) {
            const std::string& t = g_texts[s.value];
            const size_t colon = t.find(':');
            const bool ok = colon == std::string::npos
                                ? features::StartMission(t.c_str())
                                : features::StartCampaignMission(t.substr(0, colon).c_str(), atoi(t.c_str() + colon + 1));
            dslog::Write("[dev] KeyScript: start mission %s - %s", t.c_str(), ok ? "started" : "refused");
            g_due = now + 500;
        } else {
            reinterpret_cast<void(__fastcall*)(void*, void*, uint32_t, int, int)>(kSelectScreen)(gui, nullptr, 0xF23EA323, 4, 0);
            dslog::Write("[dev] KeyScript: main menu");
            g_due = now + 500;
        }
    }
}

// Dev\FlowLog also shows where a scene part's text reader is when it starts skipping to ENDGARDEN (0x884F90,
// stdcall(stream) ret 4 - token scan until the word, no end-of-file test: a part read out of step loops there for
// ever). Logged once: the stream object's first dwords and the text around any of them that points at printable text.
constexpr uint32_t kSkipToEndGarden = 0x884F90;
using SkipFn = int(__fastcall*)(void*, void*, void*);
SkipFn g_nextSkip = nullptr;

int __fastcall SkipToEndGarden(void* ecx, void* edx, void* stream) {
    static LONG once = 0;
    if (InterlockedExchange(&once, 1) == 0) {
        const uint32_t* s = static_cast<const uint32_t*>(stream);
        dslog::Write("[dev] Scene reader: skipping to ENDGARDEN, stream %p", stream);
        char before[241] = {}, after[41] = {};
        const char* cur = *static_cast<const char* const*>(stream);   // +0 = the read cursor
        ReadProcessMemory(GetCurrentProcess(), cur - 240, before, 240, nullptr);
        ReadProcessMemory(GetCurrentProcess(), cur, after, 40, nullptr);
        for (char* t : {before, after})
            for (char* c = t; c < t + strlen(t); ++c)
                if (*c == '\r' || *c == '\n') *c = '|';
        dslog::Write("[dev]   cursor %p: ...%s<HERE>%s", cur, before, after);
        for (int i = 0; i < 16; ++i) {
            uint32_t v = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), s + i, &v, 4, nullptr)) break;
            char text[161] = {};
            if (v > 0x10000 && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(v - 80), text, 160,
                                                 nullptr)) {
                int printable = 0;
                for (char& c : text) {
                    if (c == '\r' || c == '\n') c = '|';
                    printable += (c >= 32 && c < 127);
                }
                if (printable > 120) {
                    dslog::Write("[dev]   +%02X %08X  ...%.80s<HERE>%.80s", i * 4, v, text, text + 80);
                    continue;
                }
            }
            dslog::Write("[dev]   +%02X %08X", i * 4, v);
        }
    }
    return g_nextSkip(ecx, edx, stream);
}

void Apply() {
    // The input update's entry (called from the level loop and the front end's loops alike): call 0x65B880.
    constexpr uint8_t kEntry[] = {0xE8, 0xBB, 0xFD, 0xFF, 0xFF};
    if (patch::HookFunction(kInputUpdate, reinterpret_cast<void*>(&InputUpdate), reinterpret_cast<void**>(&g_nextUpdate),
                            kEntry, sizeof kEntry))
        dslog::Write("[dev] KeyScript: input hook installed");
    constexpr uint8_t kSkipEntry[] = {0x81, 0xEC, 0x04, 0x01, 0x00, 0x00};   // sub esp, 0x104
    patch::HookFunction(kSkipToEndGarden, reinterpret_cast<void*>(&SkipToEndGarden),
                        reinterpret_cast<void**>(&g_nextSkip), kSkipEntry, sizeof kSkipEntry);
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
        } else if (!_stricmp(t, "win")) {
            g_steps.push_back({4, 0});
        } else if (!_strnicmp(t, "hold:", 5)) {
            char key[32] = {};
            unsigned ms = 1000;
            if (sscanf_s(t + 5, "%31[^:]:%u", key, static_cast<unsigned>(sizeof key), &ms) >= 1) {
                Hold h{ms, {}};
                char* kctx = nullptr;
                for (char* k = strtok_s(key, "+", &kctx); k; k = strtok_s(nullptr, "+", &kctx)) {
                    uint32_t code = 0;
                    for (const Name& n : names)
                        if (!_stricmp(k, n.name)) code = n.code;
                    if (!code && k[1] == 0 && isalpha(static_cast<unsigned char>(k[0])))
                        code = MapVirtualKeyA(static_cast<UINT>(toupper(k[0])), MAPVK_VK_TO_VSC);
                    if (!code) code = strtoul(k, nullptr, 16);
                    if (code) h.keys.push_back(code);
                }
                if (!h.keys.empty()) {
                    g_holds.push_back(h);
                    g_steps.push_back({6, static_cast<uint32_t>(g_holds.size() - 1)});
                }
            }
        } else if (!_strnicmp(t, "path:", 5)) {
            int id = 0, wp = 0;
            if (sscanf_s(t + 5, "%d:%d", &id, &wp) == 2) {
                g_paths.push_back({id, wp});
                g_steps.push_back({7, static_cast<uint32_t>(g_paths.size() - 1)});
            }
        } else if (!_strnicmp(t, "op:", 3)) {
            g_texts.push_back(t + 3);
            g_steps.push_back({5, static_cast<uint32_t>(g_texts.size() - 1)});
        } else {
            for (const Name& n : names)
                if (!_stricmp(t, n.name)) g_steps.push_back({1, n.code});
        }
    }
    if (!g_steps.empty()) AfterUnpack(&Apply);
}
#endif

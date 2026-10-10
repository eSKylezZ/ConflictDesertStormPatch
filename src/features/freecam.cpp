// Free camera (needs the cheats unlocked, cheats.cpp): F6 or L3 + R3 in a mission.
//
// The game's own debug camera: the debug key "Toggle Camera State" (0x444870, start flag M only) puts player 1 in
// camera mode 3 (FUN_00472b30; player [FUN_0040a280(0)] +8 = mode, 1 = normal) and back. In mode 3 the per-frame camera
// code (0x40F4ED..0x40F631) turns the camera by the mouse (DIMOUSESTATE 0x754C24 lX / lY) and moves it by the velocity
// 0x6069C4/C8/CC (x dt(ms) x 1000... per ms; zeroed after every use), which the debug scroll keys fill from the camera
// object [0x63C940]'s direction vectors (+0x128 forward, +0x134 / +0x140 sideways / up - 0x4447D0 / 0x4446D0 / 0x444650).
// We toggle through the same handler and fill the velocity ourselves: keyboard W A S D, Q / E down / up, Shift faster;
// pad: left stick, L2 / R2 down / up, L1 faster, right stick looks (added to the mouse state the camera reads).
// Esc / Start leave it (then pause as usual).
// While flying the HUD is hidden (HudHidden: splitscreen.cpp's HUD wrappers, action prompts) and player 1's input
// block is switched off (joystick +0x3C8 = -1 disables the whole block, keyboard too - like the customise screen's
// freeze), so W doesn't walk the soldier and Shift doesn't open the inventory; we read the pad / keys ourselves.
//
// World drawing (research/freecam_zones.md): a portal renderer - 0x4DDEB0 calls FUN_004cda70 (thiscall start zone
// [world+0x1A54]; frustum, flag; ret 8), which draws the start zone and recurses through portals the camera faces
// (FUN_0051b550). From a free camera above the canyon walls the portals (vertical openings between sections) are
// behind / edge-on and only the start zone was drawn. While flying, that call draws every zone instead, each once with
// the whole view frustum (FUN_004d0440 thiscall (zone; 1, frustum, 1, 1) ret 0x10 - its octree still culls): the zones
// reachable through the portal graph (zone data +0x20C: portal list +0x1C, next +4, zone behind +0x21C) plus the
// world list's top-level zones ([world+0x1A48], next +0xB0, parent +0xBC = 0, zone data set) - collected per flight.
#include "features.h"

#include "../core/log.h"
#include "../core/patch.h"
#include "padio.h"

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint32_t kToggle = 0x444870, kPlayer = 0x40A280, kCameraObject = 0x63C940;
constexpr uint32_t kVelocity = 0x6069C4, kMouse = 0x754C24;
constexpr uint32_t kDispatchMode = 0x60EC8C, kLevelName = 0x606880, kFrontEndState = 0x617C18, kGameWindow = 0x606A60;
constexpr uint32_t kBlock0Joystick = 0x60F5B8 + 0x3C8;
constexpr int kInGame = 0xE;
constexpr uint32_t kZoneDrawCall = 0x4DDEB0, kZoneDraw = 0x4CDA70, kZoneDrawOne = 0x4D0440, kWorld = 0x63C948;

template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}

uint8_t* Player0() { return reinterpret_cast<uint8_t*(__cdecl*)(int)>(kPlayer)(0); }
int Mode() {
    uint8_t* p = Player0();
    return p ? *reinterpret_cast<int*>(p + 8) : 0;
}

bool InMission() {
    const char* level = reinterpret_cast<const char*>(kLevelName);
    return Global<int>(kDispatchMode) == 0 && level[0] && _strnicmp(level, "frontend", 8) != 0;
}

bool Focused() { return GetForegroundWindow() == Global<HWND>(kGameWindow); }
bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

const float* Vector(uint32_t offset) {
    auto cam = Global<uint8_t*>(kCameraObject);
    return cam ? reinterpret_cast<const float*>(cam + offset) : nullptr;
}

bool g_wasDown = false;
bool g_flying = false;     // we switched player 1's input block off
bool g_zonesDirty = true;  // collect the zones again (each flight: the world object outlives a level)
int g_savedJoystick = 0;   // its joystick index, restored when the camera ends

void Detach(bool on) {
    int& joystick = Global<int>(kBlock0Joystick);
    if (on && !g_flying) g_savedJoystick = joystick, joystick = -1, g_flying = true;
    if (!on && g_flying) joystick = g_savedJoystick, g_flying = false, g_zonesDirty = true;
}

void Toggle() {
    const int before = Mode();
    reinterpret_cast<void(__cdecl*)()>(kToggle)();
    dslog::Write("Free camera: mode %d -> %d", before, Mode());
#ifndef DS_DIST
    for (uint32_t off : {0x128u, 0x134u, 0x140u})
        if (const float* v = Vector(off)) dslog::Write("[dev] free camera vector +0x%X = (%.3f, %.3f, %.3f)", off, v[0], v[1], v[2]);
#endif
}

std::vector<uint8_t*> g_zones;
const void* g_zonesFor = nullptr;   // the world + start zone they were collected for

uint8_t* ZoneData(uint8_t* zone) { return zone ? *reinterpret_cast<uint8_t**>(zone + 0x20C) : nullptr; }

void CollectZones(uint8_t* start) {
    g_zones.clear();
    std::unordered_set<uint8_t*> seen;
    auto add = [&](uint8_t* z) {
        if (z && ZoneData(z) && seen.insert(z).second) g_zones.push_back(z);
    };
    add(start);
    for (size_t i = 0; i < g_zones.size(); ++i) {   // breadth-first over the portals (closed ones too: doors)
        for (uint8_t* portal = *reinterpret_cast<uint8_t**>(ZoneData(g_zones[i]) + 0x1C); portal;
             portal = *reinterpret_cast<uint8_t**>(portal + 4))
            add(*reinterpret_cast<uint8_t**>(portal + 0x21C));
        if (g_zones.size() > 4096) break;
    }
    if (auto world = Global<uint8_t*>(kWorld))   // zones no portal leads to
        for (uint8_t* o = *reinterpret_cast<uint8_t**>(world + 0x1A48); o; o = *reinterpret_cast<uint8_t**>(o + 0xB0))
            if (!*reinterpret_cast<void**>(o + 0xBC)) add(o);
    dslog::Write("Free camera: %u zones to draw", static_cast<unsigned>(g_zones.size()));
}

void __fastcall DrawZones(uint8_t* zone, void*, void* frustum, int flag) {
    using Original = void(__thiscall*)(uint8_t*, void*, int);
    if (!g_flying || !zone || !ZoneData(zone)) return reinterpret_cast<Original>(kZoneDraw)(zone, frustum, flag);
    const void* key = Global<void*>(kWorld);
    if (g_zonesDirty || key != g_zonesFor || g_zones.empty()) CollectZones(zone), g_zonesFor = key, g_zonesDirty = false;
    using DrawOne = void(__thiscall*)(uint8_t*, int, void*, int, int);
    for (uint8_t* z : g_zones) reinterpret_cast<DrawOne>(kZoneDrawOne)(z, 1, frustum, 1, 1);
}

}  // namespace

void features::ApplyFreeCam() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!patch::HookCall(kZoneDrawCall, reinterpret_cast<const void*>(&DrawZones), kZoneDraw))
        dslog::Write("[fail] Free camera: zone draw call at 0x%08X not recognised - far zones stay hidden", kZoneDrawCall);
}

bool features::FreeCamActive() { return g_flying; }
bool features::HudHidden() { return g_flying || CustomiseHidesHud(); }

void features::OnFrameFreeCam() {
    if (g_flying && (!InMission() || Mode() != 3)) Detach(false);   // level ended / camera switched back elsewhere
    if (!CheatsUnlocked() || !InMission() || Global<int>(kFrontEndState) != kInGame || !Focused()) {
        g_wasDown = true;   // the toggle needs a fresh press once play resumes
        return;
    }
    const int mode = Mode();
    padio::JoyState pad{};
    const int joystick = g_flying ? g_savedJoystick : Global<int>(kBlock0Joystick);
    const bool havePad = joystick >= 0 && padio::Fill(joystick, pad);
    const uint32_t buttons = joystick >= 0 ? ReadJoystick(joystick) : 0;
    // Esc / Start leave the camera too (the pause key can't reach the game while its input block is off).
    const bool down = Key(VK_F6) || ((buttons & (1u << 9)) && (buttons & (1u << 10))) ||
                      (g_flying && (Key(VK_ESCAPE) || (buttons & (1u << 11))));
    if (down && !g_wasDown && (mode == 1 || mode == 3)) {
        dslog::Write("Free camera: toggle (F6 %d, joystick %d buttons 0x%X)", Key(VK_F6), joystick, buttons);
        Toggle();
    }
    g_wasDown = down;
    if (Mode() != 3) {
        Detach(false);
        return;
    }
    Detach(true);

    // Movement: forward / back, sideways, up / down in the camera's own directions.
    float fwd = 0, side = 0, up = 0;
    if (Key('W')) fwd += 1;
    if (Key('S')) fwd -= 1;
    if (Key('D')) side += 1;
    if (Key('A')) side -= 1;
    if (Key('E')) up += 1;
    if (Key('Q')) up -= 1;
    bool fast = Key(VK_SHIFT);
    if (havePad) {
        fwd -= pad.y / 128.0f;
        side += pad.x / 128.0f;
        if (pad.buttons[5] & 0x80) up += 1;   // R2
        if (pad.buttons[4] & 0x80) up -= 1;   // L2
        if (pad.buttons[6] & 0x80) fast = true;
        // Right stick looks: added to the mouse movement the camera code reads this frame.
        Global<LONG>(kMouse) += static_cast<LONG>(pad.z / 10);
        Global<LONG>(kMouse + 4) += static_cast<LONG>(pad.rz / 10);
    }
    const float speed = fast ? 2.0f : 0.5f;   // per ms (x 1000 per second): 5 m/s, 20 m/s with Shift / L1
    const float* f = Vector(0x128);
    const float* s = Vector(0x134);
    const float* u = Vector(0x140);
    if (!f || !s || !u) return;
    float* vel = reinterpret_cast<float*>(kVelocity);
    for (int i = 0; i < 3; ++i) vel[i] = (f[i] * fwd + s[i] * side + u[i] * up) * speed;
}

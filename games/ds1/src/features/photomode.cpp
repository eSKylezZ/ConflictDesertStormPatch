// Photo mode (research/photo_mode.md): F5 in a mission, or PHOTO MODE on the pause menu (pausemenu.cpp).
//
// The world freezes like the loadout screen does (time multiplier [0x5F7E64] = 0.0001 - exactly 0 turns the scene
// into fog colour; every input block's devices +0x38 = 0), the free camera flies (freecam.cpp FreeCamPhoto: the game's
// camera mode 3, every zone drawn, HUD hidden; its movement is scaled back up by 1 / multiplier) and split screen shows
// one full-screen view (as customise.cpp). Zoom = the photo's own vertical FOV (splitscreen.cpp PlayerFov).
// Enter / Cross takes a photo: the hints are left out of that frame, and the frame is read back right before Present
// (graphics.cpp calls PhotoBeforePresent / PhotoAfterPresent) through the game's own Direct3D 8 interface, so it works
// the same on every renderer (0 the system's D3D8; 1-3 src/render's D3D8 objects, whose CopyRects resolves
// anti-aliasing with StretchRect and reads back with GetRenderTargetData). The system's D3D8 can't copy a
// multisampled back buffer: then the front buffer is read after Present (GetFrontBuffer, desktop-sized, cropped to the
// game window's client area). Saved as PNG (WIC, worker thread) to <game>\Screenshots, or
// Documents\DesertStormFix\Screenshots when the game folder is under Program Files (the exe has no manifest - writes
// there would land in the VirtualStore) or can't be written.
// Not available online, in cutscenes / films, or while the loadout screen is pending or open.
#include <windows.h>

#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "features/features.h"
#include "features/overlay.h"
#include "generated/standard_anims.h"


namespace {
constexpr uint32_t kTimeMultiplier = 0x5F7E64, kDispatchMode = 0x60EC8C, kLevelName = 0x606880;
constexpr uint32_t kFrontEndState = 0x617C18, kGameState = 0x63C948, kRenderer = 0x63C924, kGameWindow = 0x606A60;
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478, kBlockDevices = 0x38;
constexpr uint32_t kBlockCount = 0x610798;
constexpr int kInGame = 0xE;
constexpr float kFreeze = 0.0001f, kDefaultFov = 70.0f, kMinFov = 20.0f, kMaxFov = 110.0f;
constexpr uint32_t kPadTriangle = 1u << 0, kPadCircle = 1u << 1, kPadCross = 1u << 2, kPadStart = 1u << 11;
constexpr uint32_t kPadUp = 1u << 12, kPadDown = 1u << 14, kPadSelect = 1u << 8;

template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}

bool InMission() {
    const char* level = reinterpret_cast<const char*>(kLevelName);
    return Global<int>(kDispatchMode) == 0 && level[0] && _strnicmp(level, "frontend", 8) != 0;
}
bool Cutscene() {
    const auto* state = Global<uint8_t*>(kGameState);
    return state && (state[0x1B8C] & 1);
}
int BlockCount() {
    const int n = Global<int>(kBlockCount);
    return n < 0 ? 0 : n > 4 ? 4 : n;
}
int ScreenW() { return *reinterpret_cast<int*>(Global<uint8_t*>(kRenderer) + 0x40688); }
int ScreenH() { return *reinterpret_cast<int*>(Global<uint8_t*>(kRenderer) + 0x4068C); }

enum class Phase { Closed, Open, Closing };   // Closing: camera back, input returned once the keys are let go
Phase g_phase = Phase::Closed;
bool g_requested = false;    // PHOTO MODE picked on the pause menu: open once play resumes
bool g_keyWasDown = true;    // F5 needs a fresh press
uint32_t g_held = 0;         // keys / buttons held when the screen opened or since the last action (edge detection)
float g_savedMultiplier = 1.0f;
int g_savedDevices[4] = {};
std::string g_level;
float g_fov = kDefaultFov;
bool g_hideHints = false;
int g_frame = 0;              // photoframes.cpp: 0 none (kept between openings)
bool g_shot = false;          // take a photo of the next rendered frame (hints left out)
bool g_frontFallback = false; // the back buffer couldn't be read: the front buffer after Present
std::mutex g_toastLock;   // the save thread sets the message
std::string g_toast;
DWORD g_toastUntil = 0;
void Toast(std::string text, DWORD ms) {
    std::lock_guard<std::mutex> lock(g_toastLock);
    g_toast = std::move(text);
    g_toastUntil = GetTickCount() + ms;
}
LARGE_INTEGER g_lastTick{};

// ---- split screen: one full-screen view (renderer viewport count +0x20, viewports +0x24 x 0x18, full +0x40650) ----
uint8_t g_savedViews[4 + 0x18] = {};
bool g_singleView = false;
void SingleView(bool on) {
    auto* r = Global<uint8_t*>(kRenderer);
    if (!r || on == g_singleView) return;
    if (on) {
        if (*reinterpret_cast<uint32_t*>(r + 0x20) < 2) return;
        memcpy(g_savedViews, r + 0x20, sizeof g_savedViews);
        *reinterpret_cast<uint32_t*>(r + 0x20) = 1;
        memcpy(r + 0x24, r + 0x40650, 0x18);
    } else {
        memcpy(r + 0x20, g_savedViews, sizeof g_savedViews);
    }
    g_singleView = on;
}

// ---- input --------------------------------------------------------------------------------------------------------
enum Input : uint32_t {
    kShoot = 1, kHints = 2, kExit = 4, kZoomIn = 8, kZoomOut = 0x10, kResetZoom = 0x20, kToggleKey = 0x40,
    kNextActor = 0x80, kPosePrev = 0x100, kPoseNext = 0x200, kFrameBack = 0x400, kFrameFwd = 0x800, kTurnLeft = 0x1000,
    kTurnRight = 0x2000, kResetActor = 0x4000, kNextFrame = 0x8000, kSpawn = 0x10000, kNextUnit = 0x20000,
    kRemove = 0x40000, kPlace = 0x80000, kNextWeapon = 0x100000,
};
constexpr uint32_t kPadSquare = 1u << 3, kPadR1 = 1u << 7, kPadL3 = 1u << 9, kPadR3 = 1u << 10, kPadRight = 1u << 13, kPadLeft = 1u << 15;
uint32_t ReadInput() {
    uint32_t in = 0;
    if (features::GameFocused()) {
        auto key = [](int vk) { return features::KeyHeld(vk); };
        if (key(VK_RETURN)) in |= kShoot;
        if (key('H')) in |= kHints;
        if (key(VK_ESCAPE)) in |= kExit;
        if (key(VK_F5)) in |= kToggleKey;
        if (key('Z')) in |= kZoomIn;
        if (key('X')) in |= kZoomOut;
        if (key('R')) in |= kResetZoom;
        if (key(VK_TAB)) in |= kNextActor;
        if (key(VK_LEFT)) in |= kPosePrev;
        if (key(VK_RIGHT)) in |= kPoseNext;
        if (key(VK_OEM_COMMA)) in |= kFrameBack;
        if (key(VK_OEM_PERIOD)) in |= kFrameFwd;
        if (key('T')) in |= kTurnLeft;
        if (key('Y')) in |= kTurnRight;
        if (key(VK_BACK)) in |= kResetActor;
        if (key('F')) in |= kNextFrame;
        if (key('N')) in |= kSpawn;
        if (key('U')) in |= kNextUnit;
        if (key(VK_DELETE)) in |= kRemove;
        if (key('G')) in |= kPlace;
        if (key('J')) in |= kNextWeapon;
        uint32_t buttons = 0;
        for (int j = 0; j < 8; ++j) buttons |= features::ReadJoystick(j);
        if (buttons & kPadCross) in |= kShoot;
        if (buttons & (kPadCircle | kPadStart)) in |= kExit;
        if ((buttons & kPadL3) && !(buttons & kPadSquare)) in |= kSpawn;
        if (buttons & kPadSquare) {   // held: the D-pad works the pose's frame and facing; R1 / Select: unit / remove
            if (buttons & kPadR1) in |= kNextUnit;
            if (buttons & kPadSelect) in |= kRemove;
            if (buttons & kPadL3) in |= kPlace;
            if (buttons & kPadTriangle) in |= kNextWeapon;
            if (buttons & kPadUp) in |= kFrameFwd;
            if (buttons & kPadDown) in |= kFrameBack;
            if (buttons & kPadLeft) in |= kTurnLeft;
            if (buttons & kPadRight) in |= kTurnRight;
        } else {
            if (buttons & kPadTriangle) in |= kHints;
            if (buttons & kPadR1) in |= kNextActor;
            if (buttons & kPadSelect) in |= kResetActor;
            if (buttons & kPadUp) in |= kZoomIn;
            if (buttons & kPadDown) in |= kZoomOut;
            if (buttons & kPadLeft) in |= kPosePrev;
            if (buttons & kPadRight) in |= kPoseNext;
        }
        if (buttons & kPadR3) in |= kNextFrame;
    }
    return in;
}

// ---- actors: soldiers already in the level, posed -----------------------------------------------------------------
// Soldiers = scene objects (table [[0x63C9A0]+0x30], highest id +0x34) with the soldier vtable 0x5D88E0, in play (+0xC
// bit 0), alive (+0x54), not in a vehicle (+0x267C). A pose = what script API +0x52C play_scripted_animation (0x485FD0)
// does on the soldier: FUN_0049f760(anim, 3) (thiscall), movement state 0x4B (FUN_0042d240), FUN_0042d3a0(state +0x2674,
// 0); undone like its anim-0 branch: FUN_0049f760(0, 3) + the movement rules' default state ([[0x60EC80]+8] +
// [[0x60EC80]+4] * 4 + 0x12C). Animations by file name through the anim manager [0x63C968] (FUN_00500a40, as API +0x524)
// - the standard set every level registers, so nothing new is loaded (ladder ones left out: registering them in a level
// without ladders crashed). Held: the instance's channels get dt 0 from customise.cpp's animation hook (PhotoAnimate),
// the frame keys give them real time forwards / backwards. Facing: rotation (degrees) via FUN_004c9a40 / FUN_004c9a70 +
// the soldier's wanted facing FUN_0049e7b0, as API +0x66C does. Everything is put back when photo mode closes.
constexpr uint32_t kSoldierVtable = 0x5D88E0, kObjects = 0x63C9A0, kAnimManager = 0x63C968, kMoveRules = 0x60EC80;
constexpr uint32_t kSetScriptAnim = 0x49F760, kSetMoveState = 0x42D240, kApplyMoveState = 0x42D3A0, kLoadAnim = 0x500A40;
constexpr uint32_t kGetRotation = 0x4C9A40, kSetRotation = 0x4C9A70, kSetFacing = 0x49E7B0;
constexpr uint32_t kCameras = 0x63C9D8;
constexpr float kPickRange = 4000.0f;   // cm

std::string ActorName(const uint8_t* s);
struct Actor {
    uint8_t* soldier = nullptr;
    int pose = -1;        // index into g_poses, -1 = the game's own animation
    float rot[3] = {};    // rotation when first touched
    bool turned = false;
    float settle = 0.0f;  // real seconds the new pose still plays (the switch is a blend that needs animation time)
    bool spawned = false; // photo mode made him: removed when it closes
    bool moved = false;   // placed somewhere else: put back on close
    float pos[3] = {};    // position when first touched
};
std::vector<Actor> g_actors;        // soldiers changed in this photo session
bool SpawnedHere(const uint8_t* soldier) {
    for (const Actor& a : g_actors)
        if (a.spawned && a.soldier == soldier) return true;
    return false;
}
uint8_t* g_selected = nullptr;
float g_scrub = 0.0f;               // this frame's animation time for posed soldiers (0 = held)
float g_frameDt = 0.0f;             // real seconds of this frame
std::vector<const char*> g_poses;   // file names

void BuildPoses() {
    if (!g_poses.empty()) return;
    for (const auto& a : generated::kStandardAnims)
        if (!StrStrIA(a.file, "ladder") &&
            std::find_if(g_poses.begin(), g_poses.end(), [&](const char* f) { return _stricmp(f, a.file) == 0; }) == g_poses.end())
            g_poses.push_back(a.file);
}

uint8_t** ObjectTable(uint32_t& maxId) {
    const uint8_t* mgr = Global<uint8_t*>(kObjects);
    if (!mgr) return nullptr;
    maxId = *reinterpret_cast<const uint32_t*>(mgr + 0x34);
    return *reinterpret_cast<uint8_t** const*>(mgr + 0x30);
}
bool Poseable(const uint8_t* o) {
    return o && *reinterpret_cast<const uint32_t*>(o) == kSoldierVtable && (o[0xC] & 1) &&
           *reinterpret_cast<const float*>(o + 0x54) > 0.0f && !*reinterpret_cast<void* const*>(o + 0x267C) &&
           *reinterpret_cast<void* const*>(o + 0x2C);
}
// In the object table - or one of ours: spawned soldiers have no script id, so the id table never holds them.
bool StillThere(const uint8_t* soldier) {
    if (SpawnedHere(soldier)) return true;
    uint32_t maxId = 0;
    uint8_t** t = ObjectTable(maxId);
    if (!t) return false;
    for (uint32_t i = 0; i <= maxId && i < 100000; ++i)
        if (t[i] == soldier) return true;
    return false;
}
const float* Pos(const uint8_t* o) { return reinterpret_cast<const float*>(o + 0x30); }

// The view camera (camera manager [0x63C9D8]: camera id of view 0 at +0xEC0C, cameras at +8 + id * 0x1D8; eye +0x68,
// angles +0x50 in degrees: x pitch (+ = down), y yaw with forward (sin, cos) in x / z) - the free camera moves it.
bool Camera(float eye[3], float& pitch, float& yaw) {
    auto* mgr = Global<uint8_t*>(kCameras);
    if (!mgr) return false;
    const int id = *reinterpret_cast<int*>(mgr + 0xEC0C);
    if (id < 0 || id > 127) return false;
    const uint8_t* cam = mgr + 8 + id * 0x1D8;
    memcpy(eye, cam + 0x68, 12);
    pitch = *reinterpret_cast<const float*>(cam + 0x50);
    yaw = *reinterpret_cast<const float*>(cam + 0x54);
    return true;
}
// The camera mode's mouse look never stops at straight up / down (pitch kept in 0..360: 257.8 = upside down, seen in a
// test) - held at 89 degrees either way.
void ClampPitch() {
    auto* mgr = Global<uint8_t*>(kCameras);
    if (!mgr) return;
    const int id = *reinterpret_cast<int*>(mgr + 0xEC0C);
    if (id < 0 || id > 127) return;
    float& pitch = *reinterpret_cast<float*>(mgr + 8 + id * 0x1D8 + 0x50);
    if (pitch > 89.0f && pitch < 271.0f) pitch = pitch < 180.0f ? 89.0f : 271.0f;
}
struct Basis {
    float f[3], r[3], u[3];
};
Basis ViewBasis(float pitch, float yaw) {
    constexpr float kRad = 0.017453292f;
    const float cp = std::cos(pitch * kRad), sp = std::sin(pitch * kRad), cy = std::cos(yaw * kRad), sy = std::sin(yaw * kRad);
    Basis b{{sy * cp, -sp, cy * cp}, {cy, 0.0f, -sy}, {}};
    b.u[0] = b.f[1] * b.r[2] - b.f[2] * b.r[1];
    b.u[1] = b.f[2] * b.r[0] - b.f[0] * b.r[2];
    b.u[2] = b.f[0] * b.r[1] - b.f[1] * b.r[0];
    if (b.u[1] < 0)
        for (float& c : b.u) c = -c;
    return b;
}

// Next soldier within range, ordered by the angle from the view's centre (the one in the middle first).
void NextActor() {
    float eye[3], pitch, yaw;
    uint32_t maxId = 0;
    uint8_t** t = ObjectTable(maxId);
    if (!t || !Camera(eye, pitch, yaw)) return;
    const Basis b = ViewBasis(pitch, yaw);
    struct Cand {
        uint8_t* s;
        float angle;
    };
    std::vector<Cand> list;
    std::vector<uint8_t*> all(t, t + std::min<uint32_t>(maxId + 1, 100000));
    for (const Actor& a : g_actors)
        if (a.spawned) all.push_back(a.soldier);   // (not in the id table)
    for (uint8_t* o : all) {
        if (!Poseable(o)) continue;
        const float d[3] = {Pos(o)[0] - eye[0], Pos(o)[1] + 100.0f - eye[1], Pos(o)[2] - eye[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len > kPickRange || len < 1.0f) continue;
        const float c = (d[0] * b.f[0] + d[1] * b.f[1] + d[2] * b.f[2]) / len;
        if (c < 0.2f) continue;   // behind / far off to the side
        list.push_back({o, std::acos(std::min(1.0f, c))});
    }
    if (list.empty()) {
        g_selected = nullptr;
        Toast("NO SOLDIER IN VIEW", 2000);
        return;
    }
    std::sort(list.begin(), list.end(), [](const Cand& a, const Cand& c) { return a.angle < c.angle; });
    size_t at = 0;
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].s == g_selected) at = i + 1;
    g_selected = list[at % list.size()].s;
    dslog::Write("Photo mode: soldier %s selected (%u in view)", ActorName(g_selected).c_str(), static_cast<unsigned>(list.size()));
}

Actor* Touch(uint8_t* soldier) {
    for (Actor& a : g_actors)
        if (a.soldier == soldier) return &a;
    Actor a;
    a.soldier = soldier;
    reinterpret_cast<void(__thiscall*)(void*, float*)>(kGetRotation)(soldier, a.rot);
    memcpy(a.pos, soldier + 0x30, 12);
    g_actors.push_back(a);
    return &g_actors.back();
}
Actor* Find(const uint8_t* soldier) {
    for (Actor& a : g_actors)
        if (a.soldier == soldier) return &a;
    return nullptr;
}

void Unpose(uint8_t* s) {
    reinterpret_cast<void(__thiscall*)(void*, void*, int)>(kSetScriptAnim)(s, nullptr, 3);
    const uint8_t* rules = Global<uint8_t*>(kMoveRules);
    if (!rules) return;
    const int idx = *reinterpret_cast<const int*>(rules + 4);
    const uint8_t* table = *reinterpret_cast<uint8_t* const*>(rules + 8);
    reinterpret_cast<void(__thiscall*)(void*, int)>(kSetMoveState)(s, *reinterpret_cast<const int*>(table + idx * 4 + 0x12C));
}

void SetPose(Actor& a, int pose) {
    void* mgr = Global<void*>(kAnimManager);
    if (!mgr || g_poses.empty()) return;
    if (pose < 0) {
        Unpose(a.soldier);
        a.pose = -1;
        return;
    }
    void* anim = reinterpret_cast<void*(__thiscall*)(void*, const char*)>(kLoadAnim)(mgr, g_poses[pose]);
    if (!anim) {
        dslog::Write("Photo mode: animation %s not loaded", g_poses[pose]);
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*, void*, int)>(kSetScriptAnim)(a.soldier, anim, 3);
    reinterpret_cast<void(__thiscall*)(void*, int)>(kSetMoveState)(a.soldier, 0x4B);
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kApplyMoveState)(a.soldier, *reinterpret_cast<int*>(a.soldier + 0x2674), 0);
    a.pose = pose;
    a.settle = 0.5f;
    dslog::Write("Photo mode: %s pose %d %s (move state 0x%X)", ActorName(a.soldier).c_str(), pose, g_poses[pose],
                 *reinterpret_cast<int*>(a.soldier + 0x2674));
}

void Turn(Actor& a, float degrees) {
    float r[3];
    reinterpret_cast<void(__thiscall*)(void*, float*)>(kGetRotation)(a.soldier, r);
    r[1] += degrees;
    while (r[1] >= 360.0f) r[1] -= 360.0f;
    while (r[1] < 0.0f) r[1] += 360.0f;
    reinterpret_cast<void(__thiscall*)(void*, float*)>(kSetFacing)(a.soldier, r);
    reinterpret_cast<void(__thiscall*)(void*, float*)>(kSetRotation)(a.soldier, r);
    a.turned = true;
}

void PlaceAt(uint8_t* s, const float pos[3]);
void ResetActor(Actor& a) {
    if (a.pose >= 0) Unpose(a.soldier), a.pose = -1;
    if (a.moved) PlaceAt(a.soldier, a.pos), a.moved = false;
    if (a.turned) {
        reinterpret_cast<void(__thiscall*)(void*, float*)>(kSetFacing)(a.soldier, a.rot);
        reinterpret_cast<void(__thiscall*)(void*, float*)>(kSetRotation)(a.soldier, a.rot);
        a.turned = false;
    }
}

// Spawned soldiers leave the way script API +0xC8 deactivates an object: SetActive(0) (vtable +4) + FUN_00482410(obj, 0).
void Despawn(uint8_t* s) {
    (*reinterpret_cast<void(__thiscall**)(void*, int)>(*reinterpret_cast<uint8_t**>(s) + 4))(s, 0);
    reinterpret_cast<void(__cdecl*)(void*, int)>(0x482410)(s, 0);
}

void ReleaseActors() {
    int spawned = 0;
    for (Actor& a : g_actors)
        if (a.spawned) Despawn(a.soldier), ++spawned;
        else if (StillThere(a.soldier)) ResetActor(a);
    if (spawned) dslog::Write("Photo mode: %d spawned soldier(s) removed", spawned);
    if (!g_actors.empty()) dslog::Write("Photo mode: %u soldier(s) put back", static_cast<unsigned>(g_actors.size()));
    g_actors.clear();
    g_selected = nullptr;
}

std::string ActorName(const uint8_t* s) {
    const uint8_t* unit = *reinterpret_cast<uint8_t* const*>(s + 0x24);
    const char* name = unit ? *reinterpret_cast<const char* const*>(unit + 4) : nullptr;
    std::string out = name ? name : "SOLDIER";
    for (char& c : out) c = c == '_' ? ' ' : static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return out;
}
std::string PoseName(int pose) {
    if (pose < 0 || pose >= static_cast<int>(g_poses.size())) return "OWN ANIMATION";
    std::string out = g_poses[pose];
    if (const size_t dot = out.find('.'); dot != std::string::npos) out.resize(dot);
    for (char& c : out) c = c == '_' ? ' ' : static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return out;
}

// Where a world point lands on the screen (false: behind the camera).
bool Project(const float p[3], float& sx, float& sy) {
    float eye[3], pitch, yaw;
    if (!Camera(eye, pitch, yaw)) return false;
    const Basis b = ViewBasis(pitch, yaw);
    const float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]};
    const float z = d[0] * b.f[0] + d[1] * b.f[1] + d[2] * b.f[2];
    if (z < 10.0f) return false;
    const float w = static_cast<float>(ScreenW()), h = static_cast<float>(ScreenH());
    const float t = std::tan(g_fov * 0.5f * 0.017453292f);
    const float x = (d[0] * b.r[0] + d[1] * b.r[1] + d[2] * b.r[2]) / z / (t * w / h);
    const float y = (d[0] * b.u[0] + d[1] * b.u[1] + d[2] * b.u[2]) / z / t;
    sx = (x * 0.5f + 0.5f) * w;
    sy = (0.5f - y * 0.5f) * h;
    return true;
}

// ---- the point the camera looks at ----
// World ray FUN_004de610 thiscall(world [0x63C948]; object to ignore (or 0), from, to, hit record (0xC0 bytes, zeroed by
// it), query flags, 0) ret 0x18 -> non-zero on a hit; flags 0x1C88 as the soldiers' ground probe (0x4201D4) uses them.
constexpr uint32_t kWorld = 0x63C948, kWorldRay = 0x4DE610, kSetPosition = 0x49CE70;
int g_hitOffset = -1;   // where the hit point sits in the record (found once by matching a straight-down ray)

bool Ray(const float from[3], const float to[3], float out[3]) {
    auto* world = Global<uint8_t*>(kWorld);
    if (!world) return false;
    uint32_t hit[0x30 + 4] = {};
    using RayFn = int(__thiscall*)(void*, void*, const float*, const float*, void*, uint32_t, int);
    if (!reinterpret_cast<RayFn>(kWorldRay)(world, nullptr, from, to, hit, 0x1C88, 0)) return false;
    if (g_hitOffset < 0) {
        // straight down from above: the hit's x / z equal the ray's - the first float triple doing so is the point
        const float* f = reinterpret_cast<const float*>(hit);
        for (int i = 0; i + 2 < 0x30 && g_hitOffset < 0; ++i)
            if (std::fabs(f[i] - from[0]) < 0.5f && std::fabs(f[i + 2] - from[2]) < 0.5f && f[i + 1] < from[1] &&
                f[i + 1] > to[1])
                g_hitOffset = i;
#ifndef DS_DIST
        std::string dump;
        for (int i = 0; i < 0x30; ++i) {
            char b[32];
            snprintf(b, sizeof b, " %X:%.1f", i * 4, f[i]);
            dump += b;
        }
        dslog::Write("[dev] Photo mode: ray hit record (from %.0f %.0f %.0f) -> point at +0x%X:%s", from[0], from[1], from[2],
                     g_hitOffset * 4, dump.c_str());
#endif
        if (g_hitOffset < 0 || std::fabs(from[0] - to[0]) > 0.5f || std::fabs(from[2] - to[2]) > 0.5f) {
            g_hitOffset = std::fabs(from[0] - to[0]) > 0.5f ? -1 : g_hitOffset;   // only a vertical ray can find it
            if (g_hitOffset < 0) return false;
        }
    }
    memcpy(out, reinterpret_cast<const float*>(hit) + g_hitOffset, 12);
    return true;
}

// Ground under a point (a ray from 2 m above down to the world's floor [world +0x1A80]).
bool Ground(float x, float y, float z, float& groundY) {
    auto* world = Global<uint8_t*>(kWorld);
    if (!world) return false;
    const float from[3] = {x, y + 200.0f, z}, to[3] = {x, *reinterpret_cast<float*>(world + 0x1A80), z};
    float hit[3];
    if (!Ray(from, to, hit)) return false;
    groundY = hit[1];
    return true;
}

// Where the view centre meets the world within 40 m (on the ground below that point), else 4 m ahead at a nearby
// soldier's height.
uint8_t* PlayerSoldier();
void LookPoint(float out[3]) {
    float eye[3], pitch, yaw;
    if (!Camera(eye, pitch, yaw)) return;
    const Basis b = ViewBasis(pitch, yaw);
    const float to[3] = {eye[0] + b.f[0] * 4000.0f, eye[1] + b.f[1] * 4000.0f, eye[2] + b.f[2] * 4000.0f};
    constexpr float kRad = 0.017453292f;
    out[0] = eye[0] + std::sin(yaw * kRad) * 400.0f;
    out[1] = eye[1] - 170.0f;
    out[2] = eye[2] + std::cos(yaw * kRad) * 400.0f;
    if (const uint8_t* ref = g_selected ? g_selected : PlayerSoldier()) out[1] = Pos(ref)[1];
    float gy, hit[3];
    if (g_hitOffset < 0) Ground(eye[0], eye[1], eye[2], gy);   // a straight-down ray first: finds the record's layout
    const bool hitSomething = g_hitOffset >= 0 && Ray(eye, to, hit);
    if (hitSomething) memcpy(out, hit, 12);
    const bool grounded = Ground(out[0], out[1], out[2], gy);
    if (grounded) out[1] = gy;
#ifndef DS_DIST
    dslog::Write("[dev] Photo mode: look point from (%.0f %.0f %.0f) pitch %.1f yaw %.1f: %s (%.0f %.0f %.0f)%s", eye[0],
                 eye[1], eye[2], pitch, yaw, hitSomething ? "ray hit" : "nothing hit - 4 m ahead", out[0], out[1], out[2],
                 grounded ? " on the ground" : " (no ground below)");
#endif
}

void PlaceAt(uint8_t* s, const float pos[3]) {
    float p[3] = {pos[0], pos[1], pos[2]};
    reinterpret_cast<void(__thiscall*)(void*, float*, int, int, float, int)>(kSetPosition)(s, p, 1, 1, 800.0f, 0);
}

void Place() {
    if (!g_selected) return;
    float p[3];
    LookPoint(p);
    Actor* a = Touch(g_selected);
    PlaceAt(g_selected, p);
    a->moved = !a->spawned;
    dslog::Write("Photo mode: %s placed at (%.0f, %.0f, %.0f)", ActorName(g_selected).c_str(), p[0], p[1], p[2]);
}

// ---- spawning: copies of the level's own soldiers (loadout.cpp keeps one scene record per unit) ----
int g_unit = 0;
std::vector<std::string> g_units;
std::string UnitName(int i) {
    if (i < 0 || i >= static_cast<int>(g_units.size())) return "-";
    std::string s = g_units[i];
    for (char& c : s) c = c == '_' ? ' ' : static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}
uint8_t* PlayerSoldier() { return *reinterpret_cast<uint8_t**>(kBlocks + 0x310); }

void Spawn() {
    if (g_units.empty()) g_units = features::PhotoSpawnUnits();
    if (g_units.empty()) {
        dslog::Write("Photo mode: no soldier records captured in this level");
        Toast("NO SOLDIER TYPES IN THIS LEVEL", 2000);
        return;
    }
    float eye[3], pitch, yaw;
    if (!Camera(eye, pitch, yaw)) return;
    float pos[3];
    LookPoint(pos);
    float face = yaw + 180.0f;
    if (face >= 360.0f) face -= 360.0f;
    auto* s = static_cast<uint8_t*>(features::PhotoSpawnSoldier(g_unit, pos, face));
    if (!s) {
        Toast("COULD NOT SPAWN " + UnitName(g_unit), 2000);
        return;
    }
    Actor* a = Touch(s);
    a->spawned = true;
    g_selected = s;
    dslog::Write("Photo mode: spawned %s at (%.0f, %.0f, %.0f) [%p]", g_units[g_unit].c_str(), pos[0], pos[1], pos[2], s);
}

void Remove() {
    for (size_t i = 0; i < g_actors.size(); ++i)
        if (g_actors[i].soldier == g_selected && g_actors[i].spawned) {
            Despawn(g_selected);
            g_actors.erase(g_actors.begin() + static_cast<long>(i));
            g_selected = nullptr;
            return;
        }
    Toast("ONLY SPAWNED SOLDIERS CAN BE REMOVED", 2000);
}

// ---- weapons: a spawned soldier's gun (held item = soldier +0x26B0; definition = item +0x24: +4 id, +0x80 name hash) ----
const char* HeldId(const uint8_t* s) {
    const uint8_t* item = *reinterpret_cast<uint8_t* const*>(s + 0x26B0);
    const uint8_t* def = item ? *reinterpret_cast<uint8_t* const*>(item + 0x24) : nullptr;
    return def ? *reinterpret_cast<const char* const*>(def + 4) : nullptr;
}
std::string WeaponName(const uint8_t* s) {   // as the game shows it (a mod's display name first), else the id
    const uint8_t* item = *reinterpret_cast<uint8_t* const*>(s + 0x26B0);
    const uint8_t* def = item ? *reinterpret_cast<uint8_t* const*>(item + 0x24) : nullptr;
    if (!def) return "NONE";
    const uint32_t hash = *reinterpret_cast<const uint32_t*>(def + 0x80);
    const char* text = features::ModText(hash);
    if (!text || !*text) text = features::GameText(hash);
    std::string out = text && *text ? text : HeldId(s) ? HeldId(s) : "?";
    if (!(text && *text) && out.compare(0, 7, "US_WPN_") == 0) out.erase(0, 7);
    for (char& c : out) c = c == '_' ? ' ' : static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return out;
}

// The next weapon of the level's list into his hands; the one he held goes back (loadout's KitSwap: the new one is
// given first - taking a held weapon first falls back to the knife). Only spawned soldiers: the level's own keep their
// kits (and the mission's ammo) untouched.
void NextWeapon() {
    Actor* a = Find(g_selected);
    if (!a || !a->spawned) {
        Toast("ONLY SPAWNED SOLDIERS CHANGE WEAPONS", 2000);
        return;
    }
    const std::vector<std::string> list = features::PhotoWeaponChoices();
    if (list.empty()) {
        Toast("NO WEAPONS IN THIS LEVEL", 2000);
        return;
    }
    const char* cur = HeldId(a->soldier);
    size_t next = 0;
    for (size_t i = 0; cur && i < list.size(); ++i)
        if (_stricmp(list[i].c_str(), cur) == 0) next = (i + 1) % list.size();
    void* old = cur && features::KitSlotOf(cur) >= 0 ? *reinterpret_cast<void**>(a->soldier + 0x26B0) : nullptr;
    const int slot = features::KitSlotOf(list[next].c_str());
    if (!features::KitSwap(a->soldier, old, list[next], slot)) {
        Toast("COULD NOT GIVE " + list[next], 2000);
        return;
    }
    const char* now = HeldId(a->soldier);
    dslog::Write("Photo mode: %s weapon %s -> %s (holds %s, %u choices)", ActorName(a->soldier).c_str(), cur ? cur : "-",
                 list[next].c_str(), now ? now : "-", static_cast<unsigned>(list.size()));
}

// Per frame while open: the selected soldier's pose / frame / facing from the pressed and held keys.
void UpdateActors(uint32_t in, uint32_t pressed, float dt) {
    g_scrub = 0.0f;
    g_frameDt = dt;
    for (Actor& a : g_actors) a.settle = std::max(0.0f, a.settle - dt);
    if (g_selected && !StillThere(g_selected)) g_selected = nullptr;
    if (pressed & kNextActor) NextActor();
    if (pressed & kNextUnit) {
        g_units = features::PhotoSpawnUnits();
        g_unit = g_units.empty() ? 0 : (g_unit + 1) % static_cast<int>(g_units.size());
    }
    if (pressed & kSpawn) Spawn();
    if ((pressed & kRemove) && g_selected) Remove();
    if (pressed & kPlace) Place();
    if ((pressed & kNextWeapon) && g_selected) NextWeapon();
    if (!g_selected) return;
    BuildPoses();
    if (pressed & (kPosePrev | kPoseNext)) {
        Actor* a = Touch(g_selected);
        const int n = static_cast<int>(g_poses.size());
        int next = a->pose + ((pressed & kPoseNext) ? 1 : -1);
        if (next >= n) next = -1;
        if (next < -1) next = n - 1;
        SetPose(*a, next);
    }
    if (pressed & kResetActor)
        if (Actor* a = Find(g_selected)) ResetActor(*a);
    if (in & (kTurnLeft | kTurnRight)) Turn(*Touch(g_selected), ((in & kTurnRight) ? 90.0f : -90.0f) * dt);
    if (in & kFrameFwd) g_scrub = dt;
    if (in & kFrameBack) g_scrub = -dt;
}
// ---- open / close -------------------------------------------------------------------------------------------------
bool Allowed() {
    return InMission() && net::LocalPlayer() == 0 && !Cutscene() && !features::CustomiseOpen() &&
           !features::CustomisePending() && !features::ScriptFilmPlaying() && !features::OverviewPlaying();
}

void Open() {
    if (!features::FreeCamPhoto(true)) {
        dslog::Write("Photo mode: the camera isn't free (player 1 has no soldier / another camera mode) - not opened");
        return;
    }
    g_level = reinterpret_cast<const char*>(kLevelName);
    auto& mult = Global<float>(kTimeMultiplier);
    g_savedMultiplier = mult;
    mult = kFreeze;
    for (int i = 0; i < BlockCount(); ++i) {
        int& dev = Global<int>(kBlocks + i * kBlockSize + kBlockDevices);
        g_savedDevices[i] = dev;
        dev = 0;
    }
    SingleView(true);
    g_fov = kDefaultFov;
    g_hideHints = false;
    g_units = features::PhotoSpawnUnits();
    if (g_unit >= static_cast<int>(g_units.size())) g_unit = 0;
    g_held = ReadInput();
    QueryPerformanceCounter(&g_lastTick);
    g_phase = Phase::Open;
    dslog::Write("Photo mode: opened in %s", g_level.c_str());
}

// Camera back at once; time and input only once nothing is held (a held Esc would pause the game the moment its
// input block works again).
int g_releasedFrames = 0;
void BeginClose() {
    g_releasedFrames = 0;
    ReleaseActors();
    features::FreeCamPhoto(false);
    SingleView(false);
    g_shot = g_frontFallback = false;
    g_phase = Phase::Closing;
}
void FinishClose() {
    Global<float>(kTimeMultiplier) = g_savedMultiplier;
    for (int i = 0; i < BlockCount(); ++i) Global<int>(kBlocks + i * kBlockSize + kBlockDevices) = g_savedDevices[i];
    g_phase = Phase::Closed;
    g_keyWasDown = true;
    dslog::Write("Photo mode: closed");
}
// The level ended / the game paused under us: everything back right away.
void AbortClose() {
    if (g_phase == Phase::Open) BeginClose();
    FinishClose();
}

// ---- saving ---------------------------------------------------------------------------------------------------------
std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}
bool StartsWithI(const std::wstring& s, const std::wstring& prefix) {
    return !prefix.empty() && s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}
bool MakeDirs(const std::wstring& dir) {
    const int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}
std::wstring PhotoFolder() {
    static std::wstring folder;
    if (!folder.empty()) return folder;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    dir.resize(dir.find_last_of(L"\\/") + 1);
    wchar_t pf64[MAX_PATH] = {};
    GetEnvironmentVariableW(L"ProgramW6432", pf64, MAX_PATH);
    const bool protectedDir = StartsWithI(dir, KnownFolder(FOLDERID_ProgramFiles)) ||
                              StartsWithI(dir, KnownFolder(FOLDERID_ProgramFilesX86)) || StartsWithI(dir, pf64);
    std::wstring game = dir + L"Screenshots";
    if (!protectedDir && MakeDirs(game)) {
        const std::wstring probe = game + L"\\.write_test";
        HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            return folder = game;
        }
    }
    folder = KnownFolder(FOLDERID_Documents) + L"\\DesertStormFix\\Screenshots";
    MakeDirs(folder);
    return folder;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    std::wstring w(MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, nullptr, 0), L'\0');
    MultiByteToWideChar(CP_ACP, 0, s.c_str(), -1, w.data(), static_cast<int>(w.size()));
    w.resize(wcslen(w.c_str()));
    return w;
}
// "Mission 1 - Rescue 2026-10-16 14-03-22.png" (title in the game's language; characters files can't have dropped)
std::wstring PhotoName() {
    std::wstring name = Widen(features::LevelDisplayName(g_level.c_str()));
    std::string title = features::LevelTitleText(g_level.c_str());
    if (!title.empty()) {   // "RESCUE" -> "Rescue"
        bool first = true;
        for (char& c : title) {
            const unsigned char u = static_cast<unsigned char>(c);
            c = static_cast<char>(first ? toupper(u) : tolower(u));
            first = !isalpha(u);
        }
        name += L" - " + Widen(title);
    }
    if (name.empty()) name = L"Desert Storm";
    for (wchar_t& c : name)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L' ';
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[40];
    swprintf(stamp, 40, L" %04u-%02u-%02u %02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    std::wstring base = PhotoFolder() + L"\\" + name + stamp;
    std::wstring path = base + L".png";
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++n) path = base + L" (" + std::to_wstring(n) + L").png";
    return path;
}

bool WritePng(const std::wstring& path, const std::vector<uint8_t>& bgr, int w, int h) {
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    const bool ok =
        SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
        SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
        SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
        SUCCEEDED(frame->SetSize(w, h)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
        IsEqualGUID(format, GUID_WICPixelFormat24bppBGR) &&
        SUCCEEDED(frame->WritePixels(h, w * 3, static_cast<UINT>(bgr.size()), const_cast<BYTE*>(bgr.data()))) &&
        SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

void Save(std::vector<uint8_t>&& bgr, int w, int h) {
    const std::wstring path = PhotoName();
    std::wstring shown = path.substr(path.find_last_of(L'\\') + 1);
    Toast("SAVING PHOTO...", 10000);
    std::thread([pixels = std::move(bgr), w, h, path, shown]() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool ok = WritePng(path, pixels, w, h);
        if (SUCCEEDED(co)) CoUninitialize();
        char narrow[MAX_PATH * 2] = {};
        WideCharToMultiByte(CP_ACP, 0, path.c_str(), -1, narrow, sizeof narrow - 1, nullptr, nullptr);
        dslog::Write("Photo mode: %ux%u %s %s", w, h, ok ? "saved to" : "FAILED to save", narrow);
        char toast[300] = {};
        WideCharToMultiByte(CP_ACP, 0, shown.c_str(), -1, toast, 200, nullptr, nullptr);
        Toast(ok ? std::string("PHOTO SAVED: ") + toast : "PHOTO COULD NOT BE SAVED", 4000);
    }).detach();
}

// ---- reading the frame through Direct3D 8 ------------------------------------------------------------------------
struct SurfaceDesc {  // D3DSURFACE_DESC (D3D8)
    uint32_t format, type, usage, pool, size, multiSample, width, height;
};
struct LockedRect {
    int pitch;
    void* bits;
};
struct DisplayMode {
    UINT width, height, refresh;
    uint32_t format;
};
constexpr uint32_t kFmtA8R8G8B8 = 21, kFmtX8R8G8B8 = 22, kFmtR5G6B5 = 23, kFmtX1R5G5B5 = 24, kFmtA1R5G5B5 = 25;
constexpr DWORD kLockReadOnly = 0x10;

template <class Fn>
Fn Slot(void* obj, int index) {
    return reinterpret_cast<Fn>((*reinterpret_cast<void***>(obj))[index]);
}
void Release(void* obj) {
    if (obj) Slot<ULONG(__stdcall*)(void*)>(obj, 2)(obj);
}
bool Desc(void* surface, SurfaceDesc& d) { return SUCCEEDED(Slot<HRESULT(__stdcall*)(void*, SurfaceDesc*)>(surface, 8)(surface, &d)); }

// Copies a lockable surface's rect into 24-bit BGR rows.
bool ReadPixels(void* surface, const RECT& r, std::vector<uint8_t>& out) {
    SurfaceDesc d{};
    if (!Desc(surface, d)) return false;
    LockedRect lr{};
    if (FAILED(Slot<HRESULT(__stdcall*)(void*, LockedRect*, const RECT*, DWORD)>(surface, 9)(surface, &lr, nullptr, kLockReadOnly)))
        return false;
    const int w = r.right - r.left, h = r.bottom - r.top;
    out.resize(static_cast<size_t>(w) * h * 3);
    bool ok = true;
    for (int y = 0; y < h && ok; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(lr.bits) + static_cast<size_t>(y + r.top) * lr.pitch;
        uint8_t* o = &out[static_cast<size_t>(y) * w * 3];
        for (int x = 0; x < w; ++x, o += 3) {
            const int sx = x + r.left;
            if (d.format == kFmtA8R8G8B8 || d.format == kFmtX8R8G8B8) {
                o[0] = row[sx * 4], o[1] = row[sx * 4 + 1], o[2] = row[sx * 4 + 2];
            } else if (d.format == kFmtR5G6B5) {
                const uint16_t v = reinterpret_cast<const uint16_t*>(row)[sx];
                o[0] = static_cast<uint8_t>((v & 31) * 255 / 31), o[1] = static_cast<uint8_t>(((v >> 5) & 63) * 255 / 63),
                o[2] = static_cast<uint8_t>((v >> 11) * 255 / 31);
            } else if (d.format == kFmtX1R5G5B5 || d.format == kFmtA1R5G5B5) {
                const uint16_t v = reinterpret_cast<const uint16_t*>(row)[sx];
                o[0] = static_cast<uint8_t>((v & 31) * 255 / 31), o[1] = static_cast<uint8_t>(((v >> 5) & 31) * 255 / 31),
                o[2] = static_cast<uint8_t>(((v >> 10) & 31) * 255 / 31);
            } else {
                ok = false;
                break;
            }
        }
    }
    Slot<HRESULT(__stdcall*)(void*)>(surface, 10)(surface);
    return ok;
}

using GetBackBufferFn = HRESULT(__stdcall*)(void*, UINT, UINT, void**);
using CreateRenderTargetFn = HRESULT(__stdcall*)(void*, UINT, UINT, uint32_t, uint32_t, BOOL, void**);
using CreateImageSurfaceFn = HRESULT(__stdcall*)(void*, UINT, UINT, uint32_t, void**);
using CopyRectsFn = HRESULT(__stdcall*)(void*, void*, const RECT*, UINT, void*, const POINT*);
using GetFrontBufferFn = HRESULT(__stdcall*)(void*, void*);
using GetDisplayModeFn = HRESULT(__stdcall*)(void*, DisplayMode*);
enum : int { kGetDisplayMode = 8, kGetBackBuffer = 16, kCreateRenderTarget = 25, kCreateImageSurface = 27, kCopyRects = 28,
             kGetFrontBuffer = 30 };

// Back buffer -> (resolved copy if multisampled) -> system-memory image -> pixels.
bool ReadBackBuffer(void* dev, std::vector<uint8_t>& out, int& w, int& h) {
    void *bb = nullptr, *rt = nullptr, *img = nullptr;
    bool ok = false;
    SurfaceDesc d{};
    if (SUCCEEDED(Slot<GetBackBufferFn>(dev, kGetBackBuffer)(dev, 0, 0, &bb)) && Desc(bb, d)) {
        void* src = bb;
        bool resolved = true;
        if (d.multiSample) {   // the system's D3D8 refuses this; src/render's StretchRect resolves it
            resolved = SUCCEEDED(Slot<CreateRenderTargetFn>(dev, kCreateRenderTarget)(dev, d.width, d.height, d.format, 0, TRUE, &rt)) &&
                       SUCCEEDED(Slot<CopyRectsFn>(dev, kCopyRects)(dev, bb, nullptr, 0, rt, nullptr));
            src = rt;
        }
        const RECT all{0, 0, static_cast<LONG>(d.width), static_cast<LONG>(d.height)};
        if (resolved && SUCCEEDED(Slot<CreateImageSurfaceFn>(dev, kCreateImageSurface)(dev, d.width, d.height, d.format, &img)) &&
            SUCCEEDED(Slot<CopyRectsFn>(dev, kCopyRects)(dev, src, nullptr, 0, img, nullptr)))
            ok = ReadPixels(img, all, out);
        if (!ok && resolved && rt) ok = ReadPixels(rt, all, out);   // lockable resolve target
        w = static_cast<int>(d.width), h = static_cast<int>(d.height);
    }
    Release(img);
    Release(rt);
    Release(bb);
    return ok;
}

// After Present: the front buffer (the adapter's whole desktop when windowed) cropped to the game's client area.
bool ReadFrontBuffer(void* dev, std::vector<uint8_t>& out, int& w, int& h) {
    DisplayMode mode{};
    if (FAILED(Slot<GetDisplayModeFn>(dev, kGetDisplayMode)(dev, &mode))) return false;
    HWND wnd = Global<HWND>(kGameWindow);
    RECT client{};
    POINT origin{};
    if (!wnd || !GetClientRect(wnd, &client) || !ClientToScreen(wnd, &origin)) return false;
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT r{origin.x - mi.rcMonitor.left, origin.y - mi.rcMonitor.top, 0, 0};
    r.left = std::max<LONG>(0, r.left), r.top = std::max<LONG>(0, r.top);
    r.right = std::min<LONG>(static_cast<LONG>(mode.width), r.left + client.right);
    r.bottom = std::min<LONG>(static_cast<LONG>(mode.height), r.top + client.bottom);
    if (r.right <= r.left || r.bottom <= r.top) return false;
    void* img = nullptr;
    bool ok = SUCCEEDED(Slot<CreateImageSurfaceFn>(dev, kCreateImageSurface)(dev, mode.width, mode.height, kFmtA8R8G8B8, &img)) &&
              SUCCEEDED(Slot<GetFrontBufferFn>(dev, kGetFrontBuffer)(dev, img)) && ReadPixels(img, r, out);
    Release(img);
    w = r.right - r.left, h = r.bottom - r.top;
    return ok;
}

// ---- hints ----------------------------------------------------------------------------------------------------------
std::string Icon(overlay::Icon icon, const char* fallback) {
    if (!overlay::GlyphsReady()) return fallback;
    return overlay::IconChar(icon);
}

void DrawLine(const std::string& text, int y, uint32_t argb, bool box) {
    void* font = overlay::MenuFont();
    if (!font) return;
    const int w = ScreenW(), h = ScreenH();
    const int tw = overlay::TextWidth(font, text.c_str());
    if (box) {
        const float pad = h * 0.012f, lineH = h * 0.045f;
        const float rect[1][4] = {{(w - tw) / 2.0f - pad * 2, y - lineH * 0.78f, tw + pad * 4, lineH * 1.1f}};
        overlay::FillRects(rect, 1, 0xA0000000);
    }
    overlay::DrawLabel(font, text.c_str(), (w - tw) / 2, y, argb);
}

void LateDraw() {
    if (g_phase != Phase::Open) return;
    features::PhotoFrameText();   // the frame's words are part of the photo
    if (g_shot || g_frontFallback) return;
    const int h = ScreenH();
    if (!g_hideHints) {
        const bool kb = features::KeyboardInUse(), xb = features::PadStyleXbox();
        using I = overlay::Icon;
        char fov[32];
        snprintf(fov, sizeof fov, "PHOTO MODE   FOV %.0f", g_fov);
        DrawLine(fov, static_cast<int>(h * 0.06f), 0xFFE8D8A0, true);
        std::string line1, line2;
        if (kb) {
            line1 = Icon(I::KbEnter, "[ENTER]") + " TAKE PHOTO   " + Icon(I::KbW, "W") + Icon(I::KbA, "A") + Icon(I::KbS, "S") +
                    Icon(I::KbD, "D") + " MOVE   " + Icon(I::Mouse, "[MOUSE]") + " LOOK   " + Icon(I::KbQ, "Q") + "/" +
                    Icon(I::KbE, "E") + " DOWN / UP   " + Icon(I::KbShift, "[SHIFT]") + " FAST";
            line2 = Icon(I::KbZ, "Z") + "/" + Icon(I::KbX, "X") + " ZOOM   " + Icon(I::KbR, "R") + " RESET ZOOM   " +
                    Icon(I::KbF, "F") + " FRAME: " + features::PhotoFrameName(g_frame) + "   " + Icon(I::KbH, "H") +
                    " HIDE HINTS   " + Icon(I::KbEscape, "[ESC]") + " EXIT";
        } else {
            line1 = Icon(xb ? I::XbCross : I::PsCross, "[X]") + " TAKE PHOTO   " + Icon(xb ? I::XbStickL : I::PsStickL, "[L]") +
                    " MOVE   " + Icon(xb ? I::XbStickR : I::PsStickR, "[R]") + " LOOK   " + Icon(xb ? I::XbL2 : I::PsL2, "L2") +
                    "/" + Icon(xb ? I::XbR2 : I::PsR2, "R2") + " DOWN / UP   " + Icon(xb ? I::XbL1 : I::PsL1, "L1") + " FAST";
            line2 = Icon(xb ? I::XbDpadVertical : I::PsDpadVertical, "[D-PAD]") + " ZOOM   " +
                    Icon(xb ? I::XbR3 : I::PsR3, "R3") + " FRAME: " + features::PhotoFrameName(g_frame) + "   " +
                    Icon(xb ? I::XbTriangle : I::PsTriangle, "[TRIANGLE]") + " HIDE HINTS   " +
                    Icon(xb ? I::XbCircle : I::PsCircle, "[O]") + " EXIT";
        }
        std::string line3;
        if (kb)
            line3 = Icon(I::KbTab, "[TAB]") + " SOLDIER   " + Icon(I::KbArrowsHorizontal, "[<- ->]") + " POSE   " +
                    Icon(I::KbComma, ",") + "/" + Icon(I::KbPeriod, ".") + " FRAME   " + Icon(I::KbT, "T") + "/" +
                    Icon(I::KbY, "Y") + " TURN   " + Icon(I::KbBackspace, "[BACKSPACE]") + " RESET SOLDIER";
        else
            line3 = Icon(xb ? I::XbR1 : I::PsR1, "R1") + " SOLDIER   " +
                    Icon(xb ? I::XbDpadHorizontal : I::PsDpadHorizontal, "[D-PAD]") + " POSE   " +
                    Icon(xb ? I::XbSquare : I::PsSquare, "[SQUARE]") + "+" +
                    Icon(xb ? I::XbDpadVertical : I::PsDpadVertical, "[D-PAD]") + " FRAME   " +
                    Icon(xb ? I::XbSquare : I::PsSquare, "[SQUARE]") + "+" +
                    Icon(xb ? I::XbDpadHorizontal : I::PsDpadHorizontal, "[D-PAD]") + " TURN   " +
                    Icon(xb ? I::XbSelect : I::PsSelect, "[SELECT]") + " RESET SOLDIER";
        std::string line4;
        if (kb)
            line4 = Icon(I::KbN, "N") + " SPAWN " + UnitName(g_unit) + "   " + Icon(I::KbU, "U") + " SOLDIER TYPE   " +
                    Icon(I::KbJ, "J") + " WEAPON   " + Icon(I::KbG, "G") + " PLACE HERE   " + Icon(I::KbDelete, "[DEL]") +
                    " REMOVE SPAWNED";
        else
            line4 = Icon(xb ? I::XbL3 : I::PsL3, "L3") + " SPAWN " + UnitName(g_unit) + "   " +
                    Icon(xb ? I::XbSquare : I::PsSquare, "[SQUARE]") + "+" + Icon(xb ? I::XbR1 : I::PsR1, "R1") +
                    " SOLDIER TYPE   " + Icon(xb ? I::XbSquare : I::PsSquare, "[SQUARE]") + "+" +
                    Icon(xb ? I::XbTriangle : I::PsTriangle, "[TRIANGLE]") + " WEAPON   " +
                    Icon(xb ? I::XbSquare : I::PsSquare, "[SQUARE]") + "+" +
                    Icon(xb ? I::XbL3 : I::PsL3, "L3") + " PLACE HERE   " + Icon(xb ? I::XbSquare : I::PsSquare, "[SQUARE]") +
                    "+" + Icon(xb ? I::XbSelect : I::PsSelect, "[SELECT]") + " REMOVE SPAWNED";
        DrawLine(line4, static_cast<int>(h * 0.79f), 0xFFFFFFFF, true);
        {   // aiming dot: where spawn / place-here go
            const float c = std::max(2.0f, h * 0.004f), cx = ScreenW() * 0.5f, cy = h * 0.5f;
            const float dot[2][4] = {{cx - c * 2.5f, cy - c * 0.5f, c * 5, c}, {cx - c * 0.5f, cy - c * 2.5f, c, c * 5}};
            overlay::FillRects(dot, 2, 0xC0FFD040);
        }
        DrawLine(line3, static_cast<int>(h * 0.845f), 0xFFFFFFFF, true);
        DrawLine(line1, static_cast<int>(h * 0.90f), 0xFFFFFFFF, true);
        DrawLine(line2, static_cast<int>(h * 0.955f), 0xFFFFFFFF, true);
        if (g_selected) {
            const Actor* a = Find(g_selected);
            const std::string what = ActorName(g_selected) + "   POSE: " + PoseName(a ? a->pose : -1) + "   WEAPON: " +
                                     WeaponName(g_selected);
            DrawLine(what, static_cast<int>(h * 0.735f), 0xFFE8D8A0, true);
            const float head[3] = {Pos(g_selected)[0], Pos(g_selected)[1] + 160.0f, Pos(g_selected)[2]};
            float sx, sy;
            void* font = overlay::MenuFont();
            if (font && Project(head, sx, sy)) {
                const std::string tag = "V";
                overlay::DrawLabel(font, tag.c_str(), static_cast<int>(sx) - overlay::TextWidth(font, tag.c_str()) / 2,
                                   static_cast<int>(sy), 0xFFFFD040);
            }
        }
    }
    std::lock_guard<std::mutex> lock(g_toastLock);
    if (!g_toast.empty() && GetTickCount() < g_toastUntil) DrawLine(g_toast, static_cast<int>(h * 0.13f), 0xFF90FF90, true);
}
}  // namespace

bool features::PhotoModeOpen() { return g_phase != Phase::Closed; }
bool features::PhotoModeShowing() { return g_phase == Phase::Open; }

// customise.cpp's animation hook: a posed soldier's instance is held (dt 0) or scrubbed (g_scrub).
bool features::PhotoAnimate(const void* instance, float& dt) {
    if (g_phase != Phase::Open || !instance) return false;
    for (const Actor& a : g_actors)
        if (a.pose >= 0 && *reinterpret_cast<void* const*>(a.soldier + 0x2C) == instance) {
            dt = a.settle > 0.0f ? g_frameDt : g_scrub;
            return true;
        }
    return false;
}
float features::PhotoFov() { return g_phase == Phase::Open ? g_fov : 0.0f; }
bool features::PhotoModeAvailable() { return g_phase == Phase::Closed && Allowed(); }
void features::RequestPhotoMode() { g_requested = true; }

const char* features::PhotoText(uint32_t hash) { return hash == kPhotoModeText ? "PHOTO MODE" : nullptr; }

// Per game frame, after the input poll (cheats.cpp's frame hook) and before the free camera reads its keys.
void features::OnFramePhoto() {
    static bool first = true;
    if (first) dslog::Write("Photo mode: running (F5 / pause menu)"), first = false;
    overlay::AddLateDraw(&features::PhotoFrameArt);   // (registered first: drawn under LateDraw's words)
    overlay::AddLateDraw(&LateDraw);
    const int state = Global<int>(kFrontEndState);
    if (g_phase != Phase::Closed && (!InMission() || g_level != reinterpret_cast<const char*>(kLevelName) ||
                                     state != kInGame || net::LocalPlayer() != 0)) {
        dslog::Write("Photo mode: level / state changed - closed");
        AbortClose();
        return;
    }
    const uint32_t in = ReadInput();
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = std::min(0.1f, static_cast<float>(now.QuadPart - g_lastTick.QuadPart) / static_cast<float>(freq.QuadPart));
    g_lastTick = now;

    switch (g_phase) {
    case Phase::Closed: {
        const bool f5 = (in & kToggleKey) != 0;
        if (g_requested && state == kInGame) {
            g_requested = false;
            if (Allowed()) Open();
        } else if (f5 && !g_keyWasDown) {
            if (state == kInGame && Allowed()) Open();
            else
                dslog::Write("Photo mode: F5 refused (state 0x%X, mission %d, online %d, cutscene %d, loadout %d/%d, film %d, "
                             "overview %d)", state, InMission(), net::LocalPlayer(), Cutscene(), features::CustomiseOpen(),
                             features::CustomisePending(), features::ScriptFilmPlaying(), features::OverviewPlaying());
        }
        g_keyWasDown = f5;
        break;
    }
    case Phase::Open: {
        const uint32_t pressed = in & ~g_held;
        g_held = in;
        if (pressed & (kExit | kToggleKey)) {
            BeginClose();
            break;
        }
        if ((pressed & kShoot) && !g_shot && !g_frontFallback) g_shot = true;
        if (pressed & kHints) g_hideHints = !g_hideHints;
        if (pressed & kResetZoom) g_fov = kDefaultFov;
        if (in & kZoomIn) g_fov = std::max(kMinFov, g_fov - 30.0f * dt);
        if (in & kZoomOut) g_fov = std::min(kMaxFov, g_fov + 30.0f * dt);
        UpdateActors(in, pressed, dt);
        if (pressed & kNextFrame) {
            g_frame = (g_frame + 1) % std::max(1, features::PhotoFrameCount());
            features::SetPhotoFrame(g_frame);
        }
#ifndef DS_DIST
        {   // dev: numpad 8 / 2 / 4 / 6 turn the camera through the mouse state it reads next frame (scripted tests can't
            // move a mouse; in background mode the mouse isn't polled, so the state is set and cleared again here)
            static bool turning = false;
            auto* mouse = reinterpret_cast<int32_t*>(0x754C24);   // DIMOUSESTATE lX, lY
            const int dy = (features::KeyHeld(VK_NUMPAD8) ? 3 : 0) - (features::KeyHeld(VK_NUMPAD2) ? 3 : 0);   // (8 = up)
            const int dx = (features::KeyHeld(VK_NUMPAD6) ? 3 : 0) - (features::KeyHeld(VK_NUMPAD4) ? 3 : 0);
            if (dx || dy || turning) mouse[0] = dx, mouse[1] = dy;
            turning = dx || dy;
        }
#endif
        ClampPitch();
        Global<float>(kTimeMultiplier) = kFreeze;   // (nothing else may thaw it while we are open)
        break;
    }
    case Phase::Closing:   // the game's key table keeps a released key as "released" for a frame or two - a pause
        g_releasedFrames = in ? 0 : g_releasedFrames + 1;   // binding fired on Esc's release when input came back at once
        if (g_releasedFrames >= 6) FinishClose();
        break;
    }
}

void features::PhotoBeforePresent(void* dev) {
    if (!g_shot || !dev) return;
    g_shot = false;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (ReadBackBuffer(dev, pixels, w, h)) return Save(std::move(pixels), w, h);
    g_frontFallback = true;   // the frame is still unpresented: read it from the front buffer right after Present
}

void features::PhotoAfterPresent(void* dev) {
    if (!g_frontFallback || !dev) return;
    g_frontFallback = false;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (ReadFrontBuffer(dev, pixels, w, h)) return Save(std::move(pixels), w, h);
    dslog::Write("Photo mode: the frame couldn't be read (back and front buffer)");
    Toast("PHOTO COULD NOT BE TAKEN", 4000);
}

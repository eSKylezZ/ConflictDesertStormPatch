// Players' names and colours in a mission (lobby stage 4, user 2026-10-22: "a name for the player ... have this be
// toggleable"): over every other player's soldier a nameplate in that player's colour, and in split screen each view
// carries its player's name tag. Names / colours: the online lobby's table (lobbynet.cpp), local split screen "PLAYER n"
// in the join screen's colours (the palette's first four). OPTIONS > DESERTSTORMFIX > NAMEPLATES (FeatNameplates,
// live, each PC its own choice) turns the soldiers' plates off; the view tags stay.
//
// Each view's camera: the call after its transparents (0x40E356 -> FUN_004e2d20, thiscall world; a broker chain with
// weather.cpp) runs once per view with that view's VIEW / PROJECTION transforms and viewport set on the device and the
// current player / view index in [0x63C9E8] - recorded there, drawn in a late draw over the whole frame.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/lobbynet.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kTransparentsCall = 0x40E356, kTransparents = 0x4E2D20;
constexpr uint32_t kRenderer = 0x63C924, kDevice = 0x40530, kCurrentView = 0x63C9E8;
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478, kGameState = 0x63C948, kFrontEndState = 0x617C18;
constexpr uint32_t kDispatchMode = 0x60EC8C, kSoldierVtable = 0x5D88E0;
constexpr float kNear = 4000.0f, kFar = 8000.0f;   // cm: full until 40 m, gone at 80 m
constexpr float kHead = 205.0f;                     // cm above the soldier's feet (+0x30)

struct ViewInfo {
    bool valid = false;
    int player = 0;
    float m[16];          // VIEW * PROJECTION (row vectors)
    float x, y, w, h;     // viewport (back-buffer pixels)
    float eye[3];
};
ViewInfo g_views[4];
void* g_next = reinterpret_cast<void*>(kTransparents);

void** Vt(void* o) { return *static_cast<void***>(o); }
using Xform = HRESULT(__stdcall*)(void*, DWORD, float*);
using Viewport = HRESULT(__stdcall*)(void*, DWORD*);

int Players() { return std::max(features::CoopPlayers(), features::SplitScreenPlayers()); }

void __fastcall Transparents(void* world) {
    reinterpret_cast<void(__thiscall*)(void*)>(g_next)(world);
    if (Players() < 2) return;
    const auto* r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    void* dev = r ? *reinterpret_cast<void* const*>(r + kDevice) : nullptr;
    const int view = *reinterpret_cast<int*>(kCurrentView);
    if (!dev || view < 0 || view >= 4) return;
    float v[16], p[16];
    DWORD vp[6] = {};
    if (FAILED(reinterpret_cast<Xform>(Vt(dev)[38])(dev, 2 /*VIEW*/, v)) ||
        FAILED(reinterpret_cast<Xform>(Vt(dev)[38])(dev, 3 /*PROJECTION*/, p)) ||
        FAILED(reinterpret_cast<Viewport>(Vt(dev)[41])(dev, vp)))
        return;
    ViewInfo& out = g_views[view];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += v[i * 4 + k] * p[k * 4 + j];
            out.m[i * 4 + j] = s;
        }
    for (int j = 0; j < 3; ++j) out.eye[j] = -(v[12] * v[j * 4 + 0] + v[13] * v[j * 4 + 1] + v[14] * v[j * 4 + 2]);
    out.x = static_cast<float>(vp[0]), out.y = static_cast<float>(vp[1]);
    out.w = static_cast<float>(vp[2]), out.h = static_cast<float>(vp[3]);
    out.player = view;   // (the split render loop's view = player, Split_ViewportToPlayer is the identity)
    out.valid = true;
}

bool Name(int player, std::string& name, uint32_t& rgb) {
    if (player < 0 || player >= lobby::kMaxPlayers) return false;
    if (features::LobbyOnline()) {
        const lobby::Slot& s = lobbynet::Current().slots[player];
        if (!s.used) return false;
        name = s.name;
        rgb = lobby::kPalette[s.colour];
        return true;
    }
    char t[16];
    snprintf(t, sizeof t, "PLAYER %d", player + 1);
    name = t;
    rgb = lobby::kPalette[player];
    return true;
}

bool Hidden() {
    const auto* state = *reinterpret_cast<uint8_t* const*>(kGameState);
    return !state || (state[0x1B8C] & 1) || *reinterpret_cast<int8_t*>(kDispatchMode) != 0 ||
           *reinterpret_cast<uint32_t*>(kFrontEndState) != 0xE || features::CustomiseHidesHud() || features::HudHidden() ||
           features::PhotoModeOpen();
}

uint32_t Argb(uint32_t rgb, float a) {
    return (static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24) | (rgb & 0xFFFFFF);
}
uint32_t Light(uint32_t rgb) {   // the colour, lifted towards white (text on dark)
    auto c = [&](int sh) { const uint32_t v = (rgb >> sh) & 0xFF; return (v + (255 - v) / 2) << sh; };
    return c(16) | c(8) | c(0);
}

void LateDraw() {
    ViewInfo views[4];
    for (int i = 0; i < 4; ++i) views[i] = g_views[i], g_views[i].valid = false;   // this frame's only
    if (Players() < 2 || Hidden()) return;
    void* font = overlay::MenuFont();
    if (!font) return;
    const float line = static_cast<float>(std::max(8, overlay::TextWidth(font, "M")));
    struct Label { std::string text; float x, y; uint32_t argb; };
    Label labels[32];
    int n = 0;
    const bool plates = settings::Get().featNameplates;
    for (const ViewInfo& v : views) {
        if (!v.valid || v.w <= 0) continue;
        std::string name;
        uint32_t rgb;
        // the view's own player: a tag in its top-left corner for the first seconds after the view appears / changes,
        // then fading (user 2026-10-22: at the top centre it covered the objective banner and the pick-up bar)
        static float lastRect[4][4] = {};
        static DWORD shownSince[4] = {}, lastSeen[4] = {};
        const int vi = v.player & 3;
        const DWORD now = GetTickCount();
        if (lastRect[vi][0] != v.x || lastRect[vi][1] != v.y || lastRect[vi][2] != v.w || lastRect[vi][3] != v.h ||
            now - lastSeen[vi] > 1000)
            shownSince[vi] = now, lastRect[vi][0] = v.x, lastRect[vi][1] = v.y, lastRect[vi][2] = v.w, lastRect[vi][3] = v.h;
        lastSeen[vi] = now;
        const float age = (now - shownSince[vi]) / 1000.0f, tagA = age < 6.0f ? 1.0f : std::max(0.0f, 1.0f - (age - 6.0f));
        if (tagA > 0.0f && Name(v.player, name, rgb) && n < 32) {
            const float tw = static_cast<float>(overlay::TextWidth(font, name.c_str()));
            const float tx = v.x + line * 1.2f, ty = v.y + line * 2.2f;
            overlay::QueueScreenRect(tx - line * 0.6f, ty - line * 1.6f, tw + line * 1.2f, line * 2.2f, Argb(0x000000, 0.45f * tagA));
            overlay::QueueScreenRect(tx - line * 0.6f, ty + line * 0.6f, tw + line * 1.2f, std::max(2.0f, line / 4), Argb(rgb, 0.95f * tagA));
            labels[n++] = {name, tx, ty, Argb(0xFFFFFF, 0.95f * tagA)};
        }
        for (int p = 0; p < Players() && p < 4; ++p) {   // every other player's soldier
            if (p == v.player || !Name(p, name, rgb)) continue;
            auto* soldier = *reinterpret_cast<uint8_t**>(kBlocks + p * kBlockSize + 0x310);
            if (!soldier || *reinterpret_cast<uint32_t*>(soldier) != kSoldierVtable) continue;
            const float* pos = reinterpret_cast<float*>(soldier + 0x30);
            const float wp[4] = {pos[0], pos[1] + kHead, pos[2], 1.0f};
            float c[4] = {};
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k) c[j] += wp[k] * v.m[k * 4 + j];
            const float dx = pos[0] - v.eye[0], dy = pos[1] - v.eye[1], dz = pos[2] - v.eye[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            // Down (health 0 - a Medi-Kit brings them back; user 2026-10-22: "hard to tell when someone is down"): a red,
            // pulsing tag with the distance at any range, held at the view's edge when the soldier is off screen, also with
            // the nameplates setting off.
            if (*reinterpret_cast<float*>(soldier + 0x54) <= 0.0f) {
                float sx, sy;
                if (c[3] > 1.0f) {
                    sx = v.x + (c[0] / c[3] + 1) * 0.5f * v.w, sy = v.y + (1 - c[1] / c[3]) * 0.5f * v.h;
                } else {   // behind: towards the side it is on, at the bottom
                    sx = c[0] < 0 ? v.x + v.w : v.x, sy = v.y + v.h * 0.8f;
                }
                const std::string text = name + " - DOWN, " + std::to_string(static_cast<int>(d / 100.0f)) + " m";
                const float tw = static_cast<float>(overlay::TextWidth(font, text.c_str()));
                sx = std::clamp(sx, v.x + tw / 2 + line, v.x + v.w - tw / 2 - line);
                sy = std::clamp(sy, v.y + line * 4.0f, v.y + v.h - line * 6.0f);
                const float pulse = 0.65f + 0.35f * std::sin(GetTickCount() / 160.0f);
                overlay::QueueScreenRect(sx - tw / 2 - line * 0.5f, sy - line * 1.6f, tw + line, line * 2.1f, Argb(0x600000, 0.75f * pulse));
                overlay::QueueScreenRect(sx - tw / 2 - line * 0.5f, sy + line * 0.5f, tw + line, std::max(2.0f, line / 4), Argb(0xFF2020, pulse));
                if (n < 32) labels[n++] = {text, sx - tw / 2, sy, Argb(0xFFFFFF, 1.0f)};
                continue;
            }
            if (!plates || c[3] <= 1.0f) continue;   // (behind the camera)
            const float sx = v.x + (c[0] / c[3] + 1) * 0.5f * v.w, sy = v.y + (1 - c[1] / c[3]) * 0.5f * v.h;
            if (sx < v.x || sx > v.x + v.w || sy < v.y || sy > v.y + v.h) continue;
            if (d > kFar) continue;
            const float a = d < kNear ? 1.0f : 1.0f - (d - kNear) / (kFar - kNear);
            const float tw = static_cast<float>(overlay::TextWidth(font, name.c_str()));
            overlay::QueueScreenRect(sx - tw / 2 - line * 0.4f, sy - line * 1.5f, tw + line * 0.8f, line * 1.9f, Argb(0x000000, 0.4f * a));
            overlay::QueueScreenRect(sx - tw / 2 - line * 0.4f, sy + line * 0.4f, tw + line * 0.8f, std::max(2.0f, line / 5), Argb(rgb, a));
            if (n < 32) labels[n++] = {name, sx - tw / 2, sy, Argb(Light(rgb), a)};
        }
    }
    overlay::Flush();   // the shapes, then the names over them
    for (int i = 0; i < n; ++i)
        overlay::DrawLabel(font, labels[i].text.c_str(), static_cast<int>(labels[i].x), static_cast<int>(labels[i].y), labels[i].argb);
}
}  // namespace

void features::ApplyNameplates() {
    static bool done = false;
    if (done) return;
    done = true;
    const bool ok = patch::SubscribeCall(kTransparentsCall, reinterpret_cast<const void*>(&Transparents), kTransparents, 50,
                                         &g_next);
    if (ok) overlay::AddLateDraw(&LateDraw);
    dslog::Write(ok ? "[ok]   Nameplates: players' names over their soldiers / on their views"
                    : "[fail] Nameplates: view hook - no nameplates");
}

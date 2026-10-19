// Photo mode frames (photomode.cpp): drawn over the scene and part of the photo; the hints are not.
//
// Art = one sprite atlas made from shapes and noise by scripts/gen_photo_frames.py (src/generated/photo_frames.png,
// RCDATA 303, 2048x2048; rectangles in photo_frames.h). All words on the frames are drawn with the game's own fonts
// (small [0x60EDB4] / large [0x60EDB8]), so they follow the game's language where they come from it: mission title,
// briefing date and place (campaign [0x60EDC0] +0x12EC / +0x12F0 / +0x12F4, catalog hashes), the hour from the
// atmosphere ([[0x63C948]+0x1A58] +0x3C). Mods add whole-screen PNG frames (alpha = the window onto the scene) as
// Mods\<mod>\frames\*.png - stretched to the screen.
// Drawing order: overlay late draws run in registration order and each one's queued shapes are drawn right after it;
// FrameArt (bands = FillRects at once, sprites queued) is registered before photomode.cpp's text draw, so words sit on
// top of the art.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/log.h"
#include "features/features.h"
#include "features/overlay.h"
#include "generated/photo_frames.h"

namespace {
using generated::PhotoSprite;
constexpr int kAtlasResource = 303;
constexpr uint32_t kRenderer = 0x63C924, kCampaign = 0x60EDC0, kGameState = 0x63C948, kLevelName = 0x606880;

int W() { return *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(kRenderer) + 0x40688); }
int H() { return *reinterpret_cast<int*>(*reinterpret_cast<uint8_t**>(kRenderer) + 0x4068C); }
void* Small() { return *reinterpret_cast<void**>(0x60EDB4); }
void* Large() { return *reinterpret_cast<void**>(0x60EDB8); }

void* Atlas() { return overlay::LoadTexture(kAtlasResource, generated::kPhotoAtlasSize, generated::kPhotoAtlasSize); }

void Sprite(const PhotoSprite& s, float x, float y, float w, float h, uint32_t argb = 0xFFFFFFFF) {
    void* tex = Atlas();
    if (!tex) return;
    const float a = static_cast<float>(generated::kPhotoAtlasSize);
    const float c[4][2] = {{x, y}, {x + w, y}, {x, y + h}, {x + w, y + h}};
    // half a texel in: no bleeding from the neighbouring sprite
    overlay::QueueScreenQuad(tex, c, (s.x + 0.5f) / a, (s.y + 0.5f) / a, (s.x + s.w - 0.5f) / a, (s.y + s.h - 0.5f) / a, argb);
}
// Mirrored in both directions (the halftone's big dots in the outer corner).
void SpriteFlipped(const PhotoSprite& s, float x, float y, float w, float h, uint32_t argb = 0xFFFFFFFF) {
    void* tex = Atlas();
    if (!tex) return;
    const float a = static_cast<float>(generated::kPhotoAtlasSize);
    const float c[4][2] = {{x + w, y + h}, {x, y + h}, {x + w, y}, {x, y}};
    overlay::QueueScreenQuad(tex, c, (s.x + 0.5f) / a, (s.y + 0.5f) / a, (s.x + s.w - 0.5f) / a, (s.y + s.h - 0.5f) / a, argb);
}
// A rectangle covered by square tiles of a sprite (a pattern stays a pattern on long thin bands); the last tile of a
// row / column shows only the part that fits.
void Tiled(const PhotoSprite& s, float x, float y, float w, float h, float tile) {
    void* tex = Atlas();
    if (!tex || tile < 1.0f) return;
    const float a = static_cast<float>(generated::kPhotoAtlasSize);
    for (float ty = y; ty < y + h; ty += tile)
        for (float tx = x; tx < x + w; tx += tile) {
            const float tw = std::min(tile, x + w - tx), th = std::min(tile, y + h - ty);
            const float c[4][2] = {{tx, ty}, {tx + tw, ty}, {tx, ty + th}, {tx + tw, ty + th}};
            overlay::QueueScreenQuad(tex, c, (s.x + 0.5f) / a, (s.y + 0.5f) / a, (s.x + 0.5f + (s.w - 1) * tw / tile) / a,
                                     (s.y + 0.5f + (s.h - 1) * th / tile) / a);
        }
}
// A sprite sized by height (keeps its aspect).
void SpriteH(const PhotoSprite& s, float x, float y, float h, uint32_t argb = 0xFFFFFFFF) {
    Sprite(s, x, y, h * s.w / s.h, h, argb);
}
void Fill(float x, float y, float w, float h, uint32_t argb) {
    const float r[1][4] = {{x, y, w, h}};
    overlay::FillRects(r, 1, argb);
}
// Four bands around a window (l, t, r, b = band thickness), solid or a stretched sprite.
void Bands(float l, float t, float r, float b, uint32_t argb, const PhotoSprite* tex = nullptr) {
    const float w = static_cast<float>(W()), h = static_cast<float>(H());
    if (tex) {
        Sprite(*tex, 0, 0, w, t, argb);
        Sprite(*tex, 0, h - b, w, b, argb);
        Sprite(*tex, 0, t, l, h - t - b, argb);
        Sprite(*tex, w - r, t, r, h - t - b, argb);
    } else {
        const float rects[4][4] = {{0, 0, w, t}, {0, h - b, w, b}, {0, t, l, h - t - b}, {w - r, t, r, h - t - b}};
        overlay::FillRects(rects, 4, argb);
    }
}

// ---- text (drawn by photomode.cpp's late draw, after the art) ----
struct Text {
    std::string s;
    bool large;
    float x, y;    // baseline position
    int align;     // -1 left, 0 centre, 1 right
    uint32_t argb;
    float maxW;    // > 0: shortened ("...") to fit
};
std::vector<Text> g_text;
void Say(const std::string& s, bool large, float x, float y, int align, uint32_t argb, float maxW = 0.0f) {
    g_text.push_back({s, large, x, y, align, argb, maxW});
}

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string CampaignText(uint32_t offset) {
    const uint8_t* c = *reinterpret_cast<uint8_t* const*>(kCampaign);
    if (!c) return "";
    const uint32_t hash = *reinterpret_cast<const uint32_t*>(c + offset);
    const char* t = hash ? features::GameText(hash) : nullptr;
    return t ? t : "";
}
std::string Title() {
    std::string t = CampaignText(0x12EC);
    if (t.empty()) t = features::LevelTitleText(reinterpret_cast<const char*>(kLevelName));
    return Upper(t.empty() ? "OPERATION DESERT STORM" : t);
}
std::string Date() {
    const std::string d = CampaignText(0x12F0);
    return d.empty() ? "January 17, 1991" : d;
}
std::string Place() { return CampaignText(0x12F4); }
std::string Clock() {   // "03:12" from the hour of day
    float hour = 12.0f;
    if (auto* world = *reinterpret_cast<uint8_t**>(kGameState))
        if (auto* atmo = *reinterpret_cast<uint8_t**>(world + 0x1A58)) hour = *reinterpret_cast<float*>(atmo + 0x3C);
    hour = std::fmod(std::max(0.0f, hour), 24.0f);
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d", static_cast<int>(hour), static_cast<int>((hour - std::floor(hour)) * 60.0f));
    return b;
}
// "17 JAN 1991" style from "January 17, 1991" (falls back to the text as it is).
std::string ShortDate() {
    const std::string d = Date();
    char month[32] = {};
    int day = 0, year = 0;
    if (sscanf(d.c_str(), "%31s %d, %d", month, &day, &year) == 3 && day > 0 && year > 0) {
        char b[32];
        snprintf(b, sizeof b, "%d %.3s %d", day, Upper(month).c_str(), year);
        return b;
    }
    return Upper(d);
}

// ---- the frames ----
// Each draws its art now and queues its words. s = screen height (sizes scale with it), w = width.
void FieldPhoto() {   // instant print: cream border, wide bottom, tape on the corners, handwritten-ish caption
    const float w = static_cast<float>(W()), h = static_cast<float>(H()), s = h;
    Bands(s * 0.045f, s * 0.045f, s * 0.045f, s * 0.17f, 0xFFF4F0E4, &generated::kPhotoPaper);
    Fill(s * 0.045f, s * 0.045f, w - s * 0.09f, 2, 0x40000000);   // inner edge shadow
    Sprite(generated::kPhotoTape, -s * 0.02f, s * 0.01f, s * 0.22f, s * 0.065f, 0xFFFFFFFF);
    Sprite(generated::kPhotoTape, w - s * 0.2f, s * 0.005f, s * 0.22f, s * 0.065f, 0xFFFFFFFF);
    Say(Title(), true, w * 0.5f, h - s * 0.085f, 0, 0xFF303A5A);
    std::string sub = ShortDate();
    if (!Place().empty()) sub = Upper(Place()) + "   " + sub;
    Say(sub, false, w * 0.5f, h - s * 0.035f, 0, 0xFF505868);
}

void Classified() {   // dossier: manila surround, paper clip, red stamp, typed header
    const float w = static_cast<float>(W()), h = static_cast<float>(H()), s = h;
    Bands(s * 0.06f, s * 0.11f, s * 0.06f, s * 0.08f, 0xFFFFFFFF, &generated::kPhotoManila);
    Fill(s * 0.06f, s * 0.11f, w - s * 0.12f, 3, 0x60000000);
    Sprite(generated::kPhotoPaperClip, s * 0.11f, s * 0.035f, s * 0.05f, s * 0.15f);
    const float sw = s * 0.36f, sh = s * 0.11f, sx = w - sw - s * 0.09f, sy = h - sh - s * 0.12f;
    Sprite(generated::kPhotoStamp, sx, sy, sw, sh, 0xE6FFFFFF);
    Say("TOP SECRET", true, sx + sw * 0.5f, sy + sh * 0.66f, 0, 0xE0B21A1A);
    Say("OPERATION DESERT STORM  -  EYES ONLY", false, s * 0.2f, s * 0.07f, -1, 0xFF2A2418);
    Say("SUBJECT: " + Title(), false, w - s * 0.07f, s * 0.07f, 1, 0xFF2A2418);
    Say("FILED " + ShortDate() + "   REF 91-" + std::to_string(GetTickCount() % 9000 + 1000), false, s * 0.07f,
        h - s * 0.03f, -1, 0xFF2A2418);
}

void NightVision() {   // image intensifier: green wash, vignette, scanlines, read-outs
    const float w = static_cast<float>(W()), h = static_cast<float>(H());
    Fill(0, 0, w, h, 0x5A10FF30);
    std::vector<float> lines;
    for (float y = 0; y < h; y += 4) lines.insert(lines.end(), {0.0f, y, w, 1.5f});
    overlay::FillRects(reinterpret_cast<const float(*)[4]>(lines.data()), static_cast<int>(lines.size() / 4), 0x30000000);
    Sprite(generated::kPhotoVignette, 0, 0, w, h, 0xFF000000);
    Say("NVG  GEN III", false, h * 0.06f, h * 0.08f, -1, 0xFF9CFF9C);
    Say("REC  " + Clock(), false, w - h * 0.06f, h * 0.08f, 1, 0xFF9CFF9C);
    Say(Title(), false, w * 0.5f, h - h * 0.05f, 0, 0xFF9CFF9C);
}

void GunCamera() {   // strike camera: letterbox, brackets, reticle, telemetry
    const float w = static_cast<float>(W()), h = static_cast<float>(H()), s = h;
    Fill(0, 0, w, s * 0.1f, 0xFF000000);
    Fill(0, h - s * 0.1f, w, s * 0.1f, 0xFF000000);
    const uint32_t g = 0xE0E8FFE8;
    const float t = std::max(2.0f, s * 0.003f), L = s * 0.08f, m = s * 0.16f;
    const float rects[][4] = {
        {m, m, L, t}, {m, m, t, L}, {w - m - L, m, L, t}, {w - m - t, m, t, L},
        {m, h - m - t, L, t}, {m, h - m - L, t, L}, {w - m - L, h - m - t, L, t}, {w - m - t, h - m - L, t, L},
        {w * 0.5f - s * 0.06f, h * 0.5f - t / 2, s * 0.045f, t}, {w * 0.5f + s * 0.015f, h * 0.5f - t / 2, s * 0.045f, t},
        {w * 0.5f - t / 2, h * 0.5f - s * 0.06f, t, s * 0.045f}, {w * 0.5f - t / 2, h * 0.5f + s * 0.015f, t, s * 0.045f},
    };
    overlay::FillRects(rects, static_cast<int>(sizeof rects / sizeof rects[0]), g);
    Say("STRIKE CAM 2   WHT HOT", false, s * 0.05f, s * 0.065f, -1, g);
    Say(ShortDate() + "  " + Clock() + "Z", false, w - s * 0.05f, s * 0.065f, 1, g);
    Say("TGT  " + Title(), false, s * 0.05f, h - s * 0.035f, -1, g);
    Say("RNG 0450M   ALT 12000", false, w - s * 0.05f, h - s * 0.035f, 1, g);
}

void Film() {   // 35 mm negative strip: black edges, sprocket holes, edge print
    const float w = static_cast<float>(W()), h = static_cast<float>(H()), s = h;
    const float band = s * 0.11f;
    Fill(0, 0, w, band, 0xFF101010);
    Fill(0, h - band, w, band, 0xFF101010);
    std::vector<float> holes;
    const float hw = s * 0.03f, hh = s * 0.042f, gap = s * 0.062f;
    for (float x = gap * 0.3f; x < w; x += gap) {
        holes.insert(holes.end(), {x, band * 0.12f, hw, hh});
        holes.insert(holes.end(), {x, h - band * 0.12f - hh, hw, hh});
    }
    overlay::FillRects(reinterpret_cast<const float(*)[4]>(holes.data()), static_cast<int>(holes.size() / 4), 0xFFD8D2C4);
    const uint32_t orange = 0xFFE89A30;
    Say("DESERT 400", false, s * 0.08f, band * 0.92f, -1, orange);
    Say("24", false, w * 0.5f, band * 0.92f, 0, orange);
    Say("> 24A", false, w - s * 0.08f, band * 0.92f, 1, orange);
    Say(Title() + "   " + ShortDate(), false, w * 0.5f, h - band * 0.12f - hh - s * 0.008f, 0, orange);
}

void DogTags() {   // desert camo border, a dog tag hanging in the corner
    const float w = static_cast<float>(W()), h = static_cast<float>(H()), s = h;
    const float b = s * 0.05f;
    Tiled(generated::kPhotoCamo, 0, 0, w, b, b * 3);
    Tiled(generated::kPhotoCamo, 0, h - b, w, b, b * 3);
    Tiled(generated::kPhotoCamo, 0, b, b, h - 2 * b, b * 3);
    Tiled(generated::kPhotoCamo, w - b, b, b, h - 2 * b, b * 3);
    Fill(s * 0.05f, s * 0.05f, w - s * 0.1f, 3, 0x70000000);
    const float th = s * 0.3f, tw = th * generated::kPhotoDogTag.w / generated::kPhotoDogTag.h;
    const float tx = w - tw - s * 0.06f, ty = h - th - s * 0.02f;
    Sprite(generated::kPhotoDogTag, tx, ty, tw, th);
    Say("SAS", false, tx + tw * 0.5f, ty + th * 0.42f, 0, 0xFF2A2C30);
    Say(Title(), false, tx + tw * 0.5f, ty + th * 0.55f, 0, 0xFF2A2C30, tw * 0.62f);
    Say("1991", false, tx + tw * 0.5f, ty + th * 0.68f, 0, 0xFF2A2C30);
}

void Postcard() {   // holiday postcard: white edge, stamp, postmark, big greeting
    const float w = static_cast<float>(W()), h = static_cast<float>(H()), s = h;
    Bands(s * 0.035f, s * 0.035f, s * 0.035f, s * 0.035f, 0xFFFAF6EC, &generated::kPhotoPaper);
    const float ph = s * 0.2f;
    SpriteH(generated::kPhotoPostage, w - s * 0.07f - ph * 220 / 270, s * 0.06f, ph);
    SpriteH(generated::kPhotoPostmark, w - s * 0.36f, s * 0.12f, s * 0.11f, 0xD0FFFFFF);
    Say("GREETINGS FROM THE GULF!", true, s * 0.08f, s * 0.13f, -1, 0xFFFFF0C8);
    Say("WISH YOU WERE HERE", false, s * 0.08f, s * 0.19f, -1, 0xFFFFF0C8);
}

void Vhs() {   // home video: PLAY, tape speed, date stamp, a band of tracking noise
    const float w = static_cast<float>(W()), h = static_cast<float>(H()), s = h;
    Sprite(generated::kPhotoStatic, 0, h * 0.86f, w, s * 0.035f, 0x90FFFFFF);
    Sprite(generated::kPhotoVignette, 0, 0, w, h, 0x70000000);
    const uint32_t white = 0xFFF4F4F4;
    Say("PLAY  >", true, s * 0.07f, s * 0.11f, -1, white);
    Say("SP", true, w - s * 0.07f, s * 0.11f, 1, white);
    Say(Clock(), true, s * 0.07f, h - s * 0.06f, -1, white);
    Say(Upper(Date()), true, w - s * 0.07f, h - s * 0.06f, 1, white);
}

void Newspaper() {   // front page: masthead, headline, the photo in the middle, columns under it
    const float w = static_cast<float>(W()), h = static_cast<float>(H()), s = h;
    const float top = s * 0.2f, side = s * 0.06f, bottom = s * 0.14f;
    Bands(side, top, side, bottom, 0xFFFFFFFF, &generated::kPhotoNewsprint);
    Fill(side, s * 0.085f, w - 2 * side, std::max(2.0f, s * 0.004f), 0xFF1C1C1C);
    Fill(side, top - s * 0.012f, w - 2 * side, std::max(1.0f, s * 0.002f), 0xFF1C1C1C);
    std::vector<float> cols;
    const int n = 5;
    const float cw = (w - 2 * side) / n;
    for (int c = 0; c < n; ++c)
        for (float y = h - bottom + s * 0.03f; y < h - s * 0.015f; y += s * 0.016f)
            cols.insert(cols.end(), {side + c * cw + s * 0.01f, y, cw - s * 0.02f - ((static_cast<int>(y) + c * 7) % 5) * s * 0.01f,
                                     std::max(1.0f, s * 0.004f)});
    overlay::FillRects(reinterpret_cast<const float(*)[4]>(cols.data()), static_cast<int>(cols.size() / 4), 0xA0303030);
    Say("THE DESERT DISPATCH", true, w * 0.5f, s * 0.07f, 0, 0xFF1C1C1C);
    Say("ALLIES STRIKE - " + Title(), true, w * 0.5f, top - s * 0.035f, 0, 0xFF1C1C1C);
    Say(Upper(Date()) + "   LATE EDITION", false, side, s * 0.115f, -1, 0xFF404040);
    Say("PRICE 25p", false, w - side, s * 0.115f, 1, 0xFF404040);
}

void Comic() {   // comic panel: thick ink border, halftone corner, caption box
    const float w = static_cast<float>(W()), h = static_cast<float>(H()), s = h;
    Bands(s * 0.03f, s * 0.03f, s * 0.03f, s * 0.03f, 0xFFFFFFFF);
    const float k = s * 0.012f;
    const float ink[4][4] = {{s * 0.03f, s * 0.03f, w - s * 0.06f, k}, {s * 0.03f, h - s * 0.03f - k, w - s * 0.06f, k},
                             {s * 0.03f, s * 0.03f, k, h - s * 0.06f}, {w - s * 0.03f - k, s * 0.03f, k, h - s * 0.06f}};
    overlay::FillRects(ink, 4, 0xFF101010);
    SpriteFlipped(generated::kPhotoHalftone, w - s * 0.03f - k - s * 0.35f, h - s * 0.03f - k - s * 0.35f, s * 0.35f,
                  s * 0.35f, 0x90FFFFFF);
    const float bx = s * 0.03f + k, by = s * 0.03f + k, bw = s * 0.62f, bh = s * 0.07f;
    Fill(bx, by, bw, bh, 0xFFFFE45C);
    Fill(bx, by + bh, bw, k * 0.5f, 0xFF101010);
    Fill(bx + bw, by, k * 0.5f, bh + k * 0.5f, 0xFF101010);
    Say("MEANWHILE, IN THE DESERT...", false, bx + s * 0.02f, by + bh * 0.68f, -1, 0xFF101010);
}

struct Builtin {
    const char* name;
    void (*draw)();
};
constexpr Builtin kBuiltins[] = {
    {"FIELD PHOTO", &FieldPhoto}, {"CLASSIFIED", &Classified}, {"NIGHT VISION", &NightVision},
    {"GUN CAMERA", &GunCamera},   {"35MM FILM", &Film},         {"DOG TAGS", &DogTags},
    {"POSTCARD", &Postcard},      {"VHS", &Vhs},                {"NEWSPAPER", &Newspaper},
    {"COMIC", &Comic},
};
constexpr int kBuiltinCount = static_cast<int>(sizeof kBuiltins / sizeof kBuiltins[0]);

// ---- mod frames: Mods\<mod>\frames\*.png, whole screen ----
struct ModFrame {
    std::string path, name;
    void* tex = nullptr;
    void* device = nullptr;
    int w = 0, h = 0, texW = 0, texH = 0;
};
std::vector<ModFrame> g_mod;
bool g_scanned = false;

void ScanMods() {
    if (g_scanned) return;
    g_scanned = true;
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string root(exe);
    root.resize(root.find_last_of("\\/") + 1);
    root += "Mods\\";
    WIN32_FIND_DATAA mod;
    HANDLE m = FindFirstFileA((root + "*").c_str(), &mod);
    if (m == INVALID_HANDLE_VALUE) return;
    do {
        if (!(mod.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || mod.cFileName[0] == '.' || mod.cFileName[0] == '_') continue;
        const std::string dir = root + mod.cFileName + "\\frames\\";
        WIN32_FIND_DATAA f;
        HANDLE h = FindFirstFileA((dir + "*.png").c_str(), &f);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            ModFrame fr;
            fr.path = dir + f.cFileName;
            fr.name = Upper(std::string(f.cFileName).substr(0, strlen(f.cFileName) - 4));
            g_mod.push_back(fr);
        } while (FindNextFileA(h, &f));
        FindClose(h);
    } while (FindNextFileA(m, &mod));
    FindClose(m);
    if (!g_mod.empty()) dslog::Write("Photo mode: %u frame(s) from mods", static_cast<unsigned>(g_mod.size()));
}

void DrawMod(ModFrame& f) {
    void* dev = overlay::CurrentDevice();
    if (f.device != dev) {   // (textures belong to a device - a new one after a mode change)
        f.tex = overlay::LoadTextureFile(f.path.c_str(), f.w, f.h, f.texW, f.texH);
        f.device = dev;
        if (!f.tex) dslog::Write("Photo mode: frame %s could not be read", f.path.c_str());
    }
    if (!f.tex || !f.texW || !f.texH) return;
    overlay::QueueScreenImage(f.tex, 0, 0, static_cast<float>(W()), static_cast<float>(H()),
                              static_cast<float>(f.w) / f.texW, static_cast<float>(f.h) / f.texH);
}

int g_current = 0;   // 0 = none, 1..builtins, then mod frames
}  // namespace

int features::PhotoFrameCount() {
    ScanMods();
    return 1 + kBuiltinCount + static_cast<int>(g_mod.size());
}
std::string features::PhotoFrameName(int i) {
    if (i <= 0) return "NONE";
    if (i <= kBuiltinCount) return kBuiltins[i - 1].name;
    const size_t m = static_cast<size_t>(i - 1 - kBuiltinCount);
    return m < g_mod.size() ? g_mod[m].name : "NONE";
}
void features::SetPhotoFrame(int i) { g_current = i; }

// Late draw (registered before photomode.cpp's): the art; its words are kept for PhotoFrameText.
void features::PhotoFrameArt() {
    g_text.clear();
    if (g_current <= 0 || !features::PhotoModeShowing()) return;
    if (g_current <= kBuiltinCount) return kBuiltins[g_current - 1].draw();
    const size_t m = static_cast<size_t>(g_current - 1 - kBuiltinCount);
    if (m < g_mod.size()) DrawMod(g_mod[m]);
}

// photomode.cpp's late draw, before its hints: the frame's words.
void features::PhotoFrameText() {
    for (const Text& t : g_text) {
        void* font = t.large ? Large() : Small();
        if (!font) continue;
        std::string str = t.s;
        if (t.maxW > 0 && overlay::TextWidth(font, str.c_str()) > t.maxW) {
            while (str.size() > 1 && overlay::TextWidth(font, (str + "..").c_str()) > t.maxW) str.pop_back();
            str += "..";
        }
        const int tw = overlay::TextWidth(font, str.c_str());
        const float x = t.align < 0 ? t.x : t.align == 0 ? t.x - tw * 0.5f : t.x - tw;
        overlay::DrawLabel(font, str.c_str(), static_cast<int>(x), static_cast<int>(t.y), t.argb);
    }
}

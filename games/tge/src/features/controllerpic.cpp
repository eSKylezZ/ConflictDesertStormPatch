// The controller picture (part of tgefix.controller): a PS5 / Xbox pad drawn with callouts naming what each button does
// in the game - DesertStormFix's CONTROLLER screen art (games/ds1/src/features/controls.cpp, Zacksly art cut by
// scripts/gen_controller_art.py -> generated/controller_art.h anchors, RCDATA 301 / 302) on TGE's overlay.
//
// The labels come from the player's key file (features::KeyFilePath: Documents\SCi\GreatEscape\current.key, else the game folder's, else default.key; TGE's PS2-on-USB
// numbering 0 Triangle 1 Circle 2 Cross 3 Square 4 L2 5 R2 6 L1 7 R1 8 Select 9 L3 10 R3 11 Start 12-15 D-pad) with
// controller.cpp's PadModernLayout swap applied (game 4 <-> 6, 5 <-> 7), so they show what the physical buttons do.
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "features/features.h"
#include "features/overlay.h"
#include "generated/controller_art.h"

namespace {
// Actions worth a callout, by priority (the first one bound to a button names it), with the text shown.
struct Named {
    const char* action;
    const char* label;
};
constexpr Named kActions[] = {
    {"FIRE", "Fire"},           {"ACTION", "Action"},           {"STANCE", "Crouch"},       {"INVENTORY", "Inventory"},
    {"ORDERS_BUTTON", "Orders"}, {"INVENTORY_DROP", "Drop item"}, {"STEALTH_BUTTON", "Sneak"}, {"JUMP", "Jump"},
    {"TARGET_RIGHT", "Next target"}, {"TARGET_LEFT", "Previous target"}, {"HEAD_TOGGLE", "Aim"},
    {"HEAD_ZOON_NEXT", "Zoom"}, {"PAUSE", "Pause"}, {"OBJECTIVES", "Objectives"}, {"PEEK_UP", "Peek"},
    {"PEEK_LEFT", "Peek"},      {"PEEK_BACK", "Peek"},          {"PEEK_RIGHT", "Peek"},     {"INVENTORY_UP", "Item up"},
    {"INVENTORY_DOWN", "Item down"},
};

struct Callouts {
    std::string label[18];   // game buttons 0-15, left stick, right stick (physical, after the modern swap)
    bool dpadMerged = false; // the four D-pad directions do the same: one callout at the pad's middle (label[12])
    FILETIME stamp{};
};
Callouts g_callouts;

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    return dir.substr(0, dir.find_last_of("\\/"));
}

}  // namespace

std::string features::KeyFilePath() {
    std::vector<std::string> paths;
    char docs[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PERSONAL, nullptr, 0, docs)))
        paths.push_back(std::string(docs) + "\\SCi\\GreatEscape\\current.key");   // where the game saves it
    paths.push_back(GameDir() + "\\current.key");
    paths.push_back(GameDir() + "\\default.key");
    for (const std::string& p : paths)
        if (GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    return {};
}

namespace {
// Physical button for a game button number (controller.cpp's PadModernLayout swap).
int Physical(int game) {
    if (!features::Get().padModernLayout) return game;
    switch (game) {
        case 4: return 6;
        case 6: return 4;
        case 5: return 7;
        case 7: return 5;
        default: return game;
    }
}

void Load() {
    const std::string path = features::KeyFilePath();
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (path.empty() || !GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) return;
    static bool modern = false;
    if (CompareFileTime(&info.ftLastWriteTime, &g_callouts.stamp) == 0 && modern == features::Get().padModernLayout &&
        !g_callouts.label[16].empty())
        return;
    modern = features::Get().padModernLayout;
    g_callouts = Callouts{};
    g_callouts.stamp = info.ftLastWriteTime;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return;
    std::vector<std::pair<std::string, int>> joy;   // action -> game button
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char action[64] = {}, code[64] = {};
        int slot = 0;
        if (sscanf_s(line, "%63[^,],%d=%63s", action, 64, &slot, code, 64) != 3) continue;
        if (strncmp(code, "JOY_BUTTON_", 11) == 0) joy.push_back({action, atoi(code + 11)});
    }
    fclose(f);
    for (const Named& n : kActions)
        for (const auto& [action, button] : joy)
            if (action == n.action && button >= 0 && button < 16 && g_callouts.label[Physical(button)].empty())
                g_callouts.label[Physical(button)] = n.label;
    // the sticks share their anchors with L3 / R3: one callout each ("Move  |  press: Zoom")
    g_callouts.label[16] = g_callouts.label[9].empty() ? "Move" : "Move  |  press: " + g_callouts.label[9];
    g_callouts.label[17] = g_callouts.label[10].empty() ? "Look" : "Look  |  press: " + g_callouts.label[10];
    g_callouts.label[9].clear();
    g_callouts.label[10].clear();
    std::string* d = &g_callouts.label[12];
    if (!d[0].empty() && d[0] == d[1] && d[0] == d[2] && d[0] == d[3]) {
        d[0] += " (D-pad)";
        d[1].clear(), d[2].clear(), d[3].clear();
        g_callouts.dpadMerged = true;
    }
}
}  // namespace

// The pad picture with callouts into the rectangle (back-buffer pixels); xbox = the Xbox picture, else PS5.
void features::DrawControllerPicture(float x, float y, float w, float h, bool xbox, float alpha) {
    Load();
    const gen::ControllerArt& art = xbox ? gen::kXboxArt : gen::kPs5Art;
    void* tex = overlay::LoadTexture(xbox ? gen::kXboxArtResource : gen::kPs5ArtResource, gen::kControllerTexW,
                                     gen::kControllerTexH);
    if (!tex) return;
    // The picture in the middle 56 % of the width, callout columns left and right of it.
    const float text = std::max(12.0f, h * 0.034f);
    float pw = w * 0.56f, ph = pw * art.height / art.width;
    if (ph > h * 0.9f) ph = h * 0.9f, pw = ph * art.width / art.height;
    const float px = x + (w - pw) / 2, py = y + (h - ph) / 2;
    const uint32_t a = static_cast<uint32_t>(255 * std::clamp(alpha, 0.0f, 1.0f)) << 24;
    overlay::QueueImage(tex, px, py, pw, ph, static_cast<float>(art.width) / gen::kControllerTexW,
                        static_cast<float>(art.height) / gen::kControllerTexH, alpha);
    // Callouts: buttons left of the picture's middle go to the left column, the rest right; each column sorted by
    // the anchors' height and spread evenly so labels never overlap.
    struct Item {
        int button;
        float ax, ay;
    };
    std::vector<Item> left, right;
    for (int b = 0; b < 18; ++b) {
        if (g_callouts.label[b].empty()) continue;
        float ax = art.anchor[b][0], ay = art.anchor[b][1];
        if (b == 12 && g_callouts.dpadMerged) {
            ax = (art.anchor[12][0] + art.anchor[13][0] + art.anchor[14][0] + art.anchor[15][0]) / 4;
            ay = (art.anchor[12][1] + art.anchor[13][1] + art.anchor[14][1] + art.anchor[15][1]) / 4;
        }
        const Item it{b, px + ax * pw, py + ay * ph};
        (art.anchor[b][0] < 0.5f ? left : right).push_back(it);
    }
    auto column = [&](std::vector<Item>& items, bool onLeft) {
        std::sort(items.begin(), items.end(), [](const Item& p, const Item& q) { return p.ay < q.ay; });
        const float top = py, step = items.size() > 1 ? ph / (items.size() - 1) : 0;
        const float edge = onLeft ? px - w * 0.03f : px + pw + w * 0.03f;
        for (size_t i = 0; i < items.size(); ++i) {
            const float ly = items.size() > 1 ? top + step * i : items[i].ay;
            overlay::QueueLine(items[i].ax, items[i].ay, edge, ly, std::max(1.5f, h * 0.003f), a | 0xD0D0D0);
            overlay::QueueRect(items[i].ax - 3, items[i].ay - 3, 6, 6, a | 0xFFFFFF);
            overlay::QueueText(g_callouts.label[items[i].button].c_str(), onLeft ? edge - 6 : edge + 6, ly - text * 0.6f,
                               text, a | 0xFFFFFF, onLeft ? 2 : 0);
        }
    };
    column(left, true);
    column(right, false);
}

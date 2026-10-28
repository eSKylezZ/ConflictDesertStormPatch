#pragma once
// TGE's menu files (.gui, version 0x1D: GE_FrontEnd, Notebook, GE_Ingame, GE_Loading, GE_PC - all loaded into one CGUI
// [0x7D8EC4]) as an editable model: Parse -> change screens / controls / navigation -> Write, byte-exact for unchanged
// files. Layout mapped 2026-10-25 from CGUI::Load 0x4BA870 and the per-type loaders (factory jump table 0x4BB580):
//   header: u16 version, u32 design w / h (640 x 480), 4 x u32 safe area; lists (u32 n + entries): .loc {len + name,
//     u32 platform}, textures (len + name -> <name>.dds), fonts, sprite sheets (.img), background bitmaps.
//   screens: u32 n; per screen u32 hash, len + name, u8 flags (+0x68: 0x40 keep 640 x 480 centred, 0x80 fit, 0 scale),
//     len + background, u32 control count + controls, u32 hash of the control focused first.
//   control: u32 type, u32 hash, len + name, u16 flags (+0x80: 2 visible, 1 focusable), 4 x f32 rect, u32 n states x 18
//     bytes {u16 flags (0x10 text by key, 0x20 face = texture), 3 x f32, u32 ARGB}, then by type: 2 sprite (faces),
//     3 text (faces), 4 button / 10 group (children), 5 frame (sprite faces + u32 n x 4 f32), 6 box (nothing),
//     7 slider (children + items + 5 f32: +0xBC, +0xC8, min, max, step), 8 toggle (children + items), 9 list
//     (children + items + 5 u32). Items = u32 n + n control hashes (arrows / scroll buttons).
//   navigation: u32 screen count; per screen (file order) a screen table + u32 control count + one table per top-level
//     control; table = u32 n x {u32 key (0 left, 1 right, 2 up, 3 down, 4 accept, 5 back), u32 type (0 open screen,
//     1 focus control, 3 follow the target's entry, 4 / 5 increment / decrement, 7 back), u32 target, u32 source}.
// Names hash with the archive LFSR over the lower-cased name (NameHash); text keys are catalog keys (looked up by
// FUN_0047EA90 every frame).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gui {
uint32_t NameHash(const std::string& name);

struct State {
    uint16_t flags = 0;
    float v[3] = {};
    uint32_t argb = 0;
};
struct SpriteFace {
    bool texture = false;   // state flag 0x20: {len + texture name, u8}
    std::string sheet;      // else: sheet name, u32 image-name length, [u32 image index], image name, u8
    uint32_t index = 0;
    std::string image;
    uint8_t b = 0;
};
struct TextFace {
    std::string font;
    bool keyed = false;     // state flag 0x10: u32 key hash + key; else u32 buffer size + literal text
    uint32_t hashOrSize = 0;
    std::string text;
    uint8_t align = 0, b = 0;
};
struct Link {
    uint32_t key, type, target, source;
};
struct Control {
    uint32_t type = 0, hash = 0;
    std::string name;
    uint16_t flags = 0;
    float rect[4] = {};
    std::vector<State> states;
    std::vector<SpriteFace> sprites;
    std::vector<std::vector<float>> frame;   // type 5: n x 4 floats
    std::vector<TextFace> texts;
    std::vector<std::unique_ptr<Control>> children;
    std::vector<uint32_t> items;
    float slider[5] = {};
    uint32_t list[5] = {};
    std::unique_ptr<Control> Clone() const;
};
struct Screen {
    uint32_t hash = 0;
    std::string name;
    uint8_t flags = 0;
    std::string background;
    std::vector<std::unique_ptr<Control>> controls;
    uint32_t focus = 0;
    std::vector<Link> nav;                       // the screen's own table (Esc ...)
    std::vector<std::vector<Link>> controlNav;   // one per top-level control
    std::unique_ptr<Screen> Clone() const;
};
struct File {
    uint16_t version = 0;
    uint32_t design[2] = {}, safe[4] = {};
    std::vector<std::pair<std::string, uint32_t>> locs;
    std::vector<std::string> textures, fonts, sheets, bitmaps;
    std::vector<std::unique_ptr<Screen>> screens;

    bool Parse(const uint8_t* data, size_t size, std::string* error = nullptr);
    std::vector<uint8_t> Write() const;

    Screen* FindScreen(uint32_t hash);
    // Inserts `s` after the screen `after` (0 = at the end).
    Screen* AddScreen(std::unique_ptr<Screen> s, uint32_t after = 0);
};

// A control by hash anywhere under the screen (children included); top-level index in *top if given.
Control* Find(Screen& s, uint32_t hash, size_t* top = nullptr);
// The control's nav table (top-level controls only), or null.
std::vector<Link>* NavOf(Screen& s, uint32_t hash);
// Every hash in a screen copy renamed to "~<tag><n>" names (controls, the screen, focus, nav targets / sources inside
// it); returns old -> new.
std::vector<std::pair<uint32_t, uint32_t>> Rename(Screen& s, const std::string& tag);
// One control and its children renamed ("~<tag><n>", n counting on), item lists inside it remapped; old -> new.
std::vector<std::pair<uint32_t, uint32_t>> RenameTree(Control& c, const std::string& tag, int& n);
// Removes a top-level control and its nav table; links in other tables that pointed at it stay (fix them).
bool Remove(Screen& s, uint32_t hash);
// Points one text face at a catalog key (literal faces become keyed: their state gets flag 0x10); returns the hash.
uint32_t SetKey(Control& c, size_t face, const std::string& key);
// Inserts a top-level control (with its nav table) at index `at` of the screen.
Control* Insert(Screen& s, size_t at, std::unique_ptr<Control> c, std::vector<Link> nav);
// Moves a control and all its children by dy.
void MoveY(Control& c, float dy);
}  // namespace gui

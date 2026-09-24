#pragma once
// Split-screen view rectangles as fractions of the screen, per player count and orientation. Same layouts as
// the Xbox build's tables (default.xbe 0x234e88..0x235088, see CLAUDE.md "Xbox version"):
//   2 players: 50/50        3 players: 50/25/25 (player 1 gets the half)        4 players: quarters
// Used by the launcher preview now, and by the split-screen renderer later.
#include <cstdint>

namespace splitscreen {
enum class Layout : uint32_t {
    Horizontal = 0,  // views stacked top / bottom
    Vertical = 1,    // views side by side
};
constexpr uint32_t kLayoutCount = 2;

struct View {
    float x, y, w, h;
};

// View i (0-based) of `players` (2-4); a single player gets the full screen.
constexpr View ViewRect(Layout layout, int players, int i) {
    if (players == 4) return {(i % 2) * 0.5f, (i / 2) * 0.5f, 0.5f, 0.5f};
    const bool side = layout == Layout::Vertical;
    if (players == 2) return side ? View{i * 0.5f, 0, 0.5f, 1} : View{0, i * 0.5f, 1, 0.5f};
    if (players == 3) {
        if (i == 0) return side ? View{0, 0, 0.5f, 1} : View{0, 0, 1, 0.5f};
        const float q = (i - 1) * 0.5f;  // the other two share the remaining half
        return side ? View{0.5f, q, 0.5f, 0.5f} : View{q, 0.5f, 0.5f, 0.5f};
    }
    return {0, 0, 1, 1};
}
}  // namespace splitscreen

"""Bakes the in-game button icons: src/generated/game_icons.h (enum) + src/generated/game_icons.bin (pixels).
Run:  python scripts/gen_game_icons.py        (needs Pillow)

Source: assets/input-prompts (Kenney "Input Prompts" 1.5, CC0 - see License.txt there), 64 px "Default" icons.
The .bin is embedded in the DLL as an RCDATA resource (src/game_icons.rc) and uploaded to Direct3D textures by
src/features/overlay.cpp: all icons back to back, straight-alpha ARGB (D3DFMT_A8R8G8B8, little-endian 0xAARRGGBB),
SIZE x SIZE each, top-down - so the DLL ships no image files and decodes nothing at runtime.
Pad buttons are listed in the game's own button order (default.key, PS2 adapter numbering) so code can index them.
"""
import os
import struct
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "assets", "input-prompts")
DST_H = os.path.join(ROOT, "src", "generated", "game_icons.h")
DST_BIN = os.path.join(ROOT, "src", "generated", "game_icons.bin")
SIZE = 64

PS, XB, KB = "PlayStation Series/Default", "Xbox Series/Default", "Keyboard & Mouse/Default"
# Game joystick buttons 0-15: 0 Triangle 1 Circle 2 Cross 3 Square 4 L2 5 R2 6 L1 7 R1 8 Select 9 L3 10 R3 11 Start
# 12-15 D-pad up/right/down/left.
PAD_ORDER = ["Triangle", "Circle", "Cross", "Square", "L2", "R2", "L1", "R1", "Select", "L3", "R3", "Start",
             "DpadUp", "DpadRight", "DpadDown", "DpadLeft"]
PS_FILES = ["playstation_button_color_triangle", "playstation_button_color_circle", "playstation_button_color_cross",
            "playstation_button_color_square", "playstation_trigger_l2", "playstation_trigger_r2",
            "playstation_trigger_l1", "playstation_trigger_r1", "playstation5_button_create",
            "playstation_button_l3", "playstation_button_r3", "playstation5_button_options",
            "playstation_dpad_up", "playstation_dpad_right", "playstation_dpad_down", "playstation_dpad_left"]
XB_FILES = ["xbox_button_color_y", "xbox_button_color_b", "xbox_button_color_a", "xbox_button_color_x", "xbox_lt",
            "xbox_rt", "xbox_lb", "xbox_rb", "xbox_button_view", "xbox_ls", "xbox_rs", "xbox_button_menu",
            "xbox_dpad_up", "xbox_dpad_right", "xbox_dpad_down", "xbox_dpad_left"]

ICONS = []  # (enum name, file)
for name, f in zip(PAD_ORDER, PS_FILES):
    ICONS.append(("Ps" + name, f"{PS}/{f}.png"))
ICONS += [("PsDpadHorizontal", f"{PS}/playstation_dpad_horizontal.png"),
          ("PsDpadVertical", f"{PS}/playstation_dpad_vertical.png"),
          ("PsStickL", f"{PS}/playstation_stick_l.png"), ("PsStickR", f"{PS}/playstation_stick_r.png")]
for name, f in zip(PAD_ORDER, XB_FILES):
    ICONS.append(("Xb" + name, f"{XB}/{f}.png"))
ICONS += [("XbDpadHorizontal", f"{XB}/xbox_dpad_horizontal.png"), ("XbDpadVertical", f"{XB}/xbox_dpad_vertical.png"),
          ("XbStickL", f"{XB}/xbox_stick_l.png"), ("XbStickR", f"{XB}/xbox_stick_r.png")]
# Keyboard: letters, digits, F keys, then the rest by Kenney name.
for c in "abcdefghijklmnopqrstuvwxyz0123456789":
    ICONS.append(("Kb" + c.upper(), f"{KB}/keyboard_{c}.png"))
for i in range(1, 13):
    ICONS.append((f"KbF{i}", f"{KB}/keyboard_f{i}.png"))
for name, f in [("Space", "space"), ("Enter", "enter"), ("Escape", "escape"), ("Backspace", "backspace"),
                ("Tab", "tab"), ("Shift", "shift"), ("Ctrl", "ctrl"), ("Alt", "alt"), ("Capslock", "capslock"),
                ("ArrowUp", "arrow_up"), ("ArrowDown", "arrow_down"), ("ArrowLeft", "arrow_left"),
                ("ArrowRight", "arrow_right"), ("ArrowsHorizontal", "arrows_horizontal"),
                ("ArrowsVertical", "arrows_vertical"), ("Arrows", "arrows_all"), ("Minus", "minus"),
                ("Equals", "equals"), ("BracketOpen", "bracket_open"), ("BracketClose", "bracket_close"),
                ("Semicolon", "semicolon"), ("Apostrophe", "apostrophe"), ("Comma", "comma"), ("Period", "period"),
                ("Slash", "slash_forward"), ("Backslash", "slash_back"), ("Tilde", "tilde"), ("Insert", "insert"),
                ("Delete", "delete"), ("Home", "home"), ("End", "end"), ("PageUp", "page_up"),
                ("PageDown", "page_down"), ("NumpadEnter", "numpad_enter"), ("NumpadPlus", "numpad_plus"),
                ("Asterisk", "asterisk"), ("Any", "any")]:
    ICONS.append(("Kb" + name, f"{KB}/keyboard_{f}.png"))
ICONS += [("MouseLeft", f"{KB}/mouse_left.png"), ("MouseRight", f"{KB}/mouse_right.png"),
          ("MouseMiddle", f"{KB}/mouse_scroll.png"), ("Mouse", f"{KB}/mouse.png")]


def pixels(path):
    im = Image.open(os.path.join(SRC, path)).convert("RGBA")
    if im.size != (SIZE, SIZE):
        im = im.resize((SIZE, SIZE), Image.LANCZOS)
    raw = im.tobytes()  # RGBA
    out = bytearray()
    for i in range(0, len(raw), 4):
        r, g, b, a = raw[i:i + 4]
        out += struct.pack("<I", a << 24 | r << 16 | g << 8 | b)
    return out


blob = bytearray()
for name, path in ICONS:
    blob += pixels(path)
with open(DST_BIN, "wb") as f:
    f.write(blob)

lines = ["// Generated by scripts/gen_game_icons.py from assets/input-prompts (Kenney, CC0) - do not edit.",
         "// Pixels: game_icons.bin (RCDATA resource kGameIconsResource), kGameIconSize^2 ARGB words per icon.",
         "#pragma once", "", "namespace gen {",
         f"inline constexpr int kGameIconSize = {SIZE};",
         "inline constexpr int kGameIconsResource = 300;",
         "// Pad icons Ps*/Xb* first 16 follow the game's joystick button numbers (Triangle/Y = 0 ... D-pad left = 15).",
         "enum class GameIcon {"]
for i in range(0, len(ICONS), 8):
    lines.append("    " + " ".join(n + "," for n, _ in ICONS[i:i + 8]))
lines += ["    Count", "};", "}  // namespace gen", ""]
with open(DST_H, "w", newline="\n") as f:
    f.write("\n".join(lines))
print(f"wrote {DST_H} + {DST_BIN}: {len(ICONS)} icons, {SIZE}x{SIZE}, {len(blob)} bytes")

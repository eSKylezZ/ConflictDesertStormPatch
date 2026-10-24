"""Sprite atlas for photo mode's frames (src/features/photoframes.cpp).

Everything is drawn here from shapes and noise - no fonts, no third-party art (all text on the frames is drawn in the
game with its own font). Writes src/generated/photo_frames.png (RCDATA 303 via src/game_icons.rc, 2048x2048) and
photo_frames.h (sprite rectangles in atlas pixels).
Run: python scripts/gen_photo_frames.py
"""
import math
import os
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "src", "generated")
SIZE = 2048


def noise(w, h, seed, lo, hi, blur=0):
    rnd = random.Random(seed)
    im = Image.new("L", (w, h))
    im.putdata([rnd.randint(lo, hi) for _ in range(w * h)])
    return im.filter(ImageFilter.GaussianBlur(blur)) if blur else im


def tinted(gray, rgb, alpha=255):
    """Grey noise (128 = the colour) -> RGBA around rgb."""
    r, g, b = rgb
    out = Image.new("RGBA", gray.size)
    px = []
    for v in gray.getdata():
        d = v - 128
        px.append((max(0, min(255, r + d)), max(0, min(255, g + d)), max(0, min(255, b + d)), alpha))
    out.putdata(px)
    return out


def paper(w, h, seed, rgb, grain=10):
    base = noise(w, h, seed, 128 - grain, 128 + grain, 0.6)
    fibres = noise(w, h, seed + 1, 120, 136, 2.5)
    return tinted(ImageChops.add(ImageChops.subtract(base, Image.new("L", (w, h), 128)), fibres), rgb)


def tape(w, h, seed):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    rnd = random.Random(seed)
    # torn ends: jagged polygon
    pts = [(0, 0)]
    for x in range(0, w + 1, 12):
        pts.append((x, rnd.randint(0, 6)))
    for y in range(0, h + 1, 8):
        pts.append((w - rnd.randint(0, 10), y))
    for x in range(w, -1, -12):
        pts.append((x, h - rnd.randint(0, 6)))
    for y in range(h, -1, -8):
        pts.append((rnd.randint(0, 10), y))
    d.polygon(pts, fill=(236, 226, 190, 175))
    grain = noise(w, h, seed + 7, 0, 40, 1)
    a = im.getchannel("A")
    im = ImageChops.subtract(im, Image.merge("RGBA", (grain, grain, grain, Image.new("L", (w, h), 0))))
    im.putalpha(a)
    return im


def paperclip(w, h):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for width, col in ((14, (60, 60, 64, 120)), (9, (170, 172, 178, 255)), (3, (235, 236, 240, 255))):
        m = 14
        # outer loop then inner loop, as one wire
        d.rounded_rectangle([m, m, w - m, h - m], radius=(w - 2 * m) // 2, outline=col, width=width)
        d.rounded_rectangle([m + 18, m + 40, w - m - 18, h - m - 70], radius=(w - 2 * m - 36) // 2, outline=col,
                            width=width)
    return im


def stamp_box(w, h, seed, rgb=(178, 26, 26)):
    """Rubber-stamp border (double rounded rectangle, uneven ink) - the words are the game's font."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([6, 6, w - 7, h - 7], radius=14, outline=rgb + (255,), width=10)
    d.rounded_rectangle([22, 22, w - 23, h - 23], radius=8, outline=rgb + (255,), width=4)
    ink = noise(w, h, seed, 0, 255, 1.2).point(lambda v: 255 if v > 70 else int(v * 2))
    im.putalpha(ImageChops.multiply(im.getchannel("A"), ink))
    return im


def vignette(w, h, strength=255, power=2.2):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    px = []
    for y in range(h):
        for x in range(w):
            dx, dy = (x + 0.5) / w * 2 - 1, (y + 0.5) / h * 2 - 1
            r = min(1.0, math.sqrt(dx * dx + dy * dy) / math.sqrt(2))
            px.append((0, 0, 0, int(strength * (r ** power))))
    im.putdata(px)
    return im


def camo(w, h, seed):
    rnd = random.Random(seed)
    cols = [(196, 170, 120), (160, 128, 86), (122, 98, 66), (214, 196, 156)]
    im = Image.new("RGBA", (w, h), cols[0] + (255,))
    d = ImageDraw.Draw(im)
    for col in cols[1:]:
        for _ in range(42):
            cx, cy = rnd.randint(-40, w + 40), rnd.randint(-40, h + 40)
            pts = []
            n = rnd.randint(7, 11)
            for i in range(n):
                a = i / n * 2 * math.pi
                rr = rnd.randint(18, 64)
                pts.append((cx + math.cos(a) * rr * 1.6, cy + math.sin(a) * rr))
            d.polygon(pts, fill=col + (255,))
    # "chocolate chip" spots
    for _ in range(160):
        cx, cy = rnd.randint(0, w), rnd.randint(0, h)
        rr = rnd.randint(3, 7)
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr], fill=(rnd.choice([(40, 32, 26), (240, 232, 214)])) + (255,))
    return im.filter(ImageFilter.GaussianBlur(0.8))


def dog_tag(w, h):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    top = 70
    d.rounded_rectangle([20, top, w - 20, h - 20], radius=60, fill=(70, 72, 76, 255))
    d.rounded_rectangle([26, top + 6, w - 26, h - 26], radius=56, fill=(176, 180, 186, 255))
    shine = Image.new("L", (w, h))
    sd = ImageDraw.Draw(shine)
    for i in range(0, w + h, 3):
        sd.line([(i, 0), (i - h, h)], fill=int(40 + 30 * math.sin(i / 37.0)))
    metal = Image.merge("RGBA", (shine, shine, shine, Image.new("L", (w, h), 0)))
    a = im.getchannel("A")
    im = ImageChops.add(im, metal)
    im.putalpha(a)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([46, top + 40, w - 46, h - 46], radius=36, outline=(120, 124, 130, 255), width=3)  # rim
    d.ellipse([w // 2 - 14, top + 14, w // 2 + 14, top + 42], fill=(0, 0, 0, 0), outline=(90, 92, 96, 255), width=3)
    # ball chain going up
    for i in range(10):
        y = top + 26 - i * 9
        if y < 0:
            break
        d.ellipse([w // 2 - 5 + i * 3, y - 5, w // 2 + 5 + i * 3, y + 5], fill=(150, 152, 158, 255),
                  outline=(80, 80, 86, 255))
    return im


def postage(w, h, seed):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, w - 1, h - 1], fill=(250, 246, 236, 255))
    r = 7
    for x in range(r, w, 2 * r + 4):   # perforations
        d.ellipse([x - r, -r, x + r, r], fill=(0, 0, 0, 0))
        d.ellipse([x - r, h - r, x + r, h + r], fill=(0, 0, 0, 0))
    for y in range(r, h, 2 * r + 4):
        d.ellipse([-r, y - r, r, y + r], fill=(0, 0, 0, 0))
        d.ellipse([w - r, y - r, w + r, y + r], fill=(0, 0, 0, 0))
    m = 22
    d.rectangle([m, m, w - m, h - m], fill=(232, 150, 70, 255))                  # desert sky
    d.ellipse([w - m - 70, m + 20, w - m - 20, m + 70], fill=(255, 226, 120, 255))  # sun
    d.polygon([(m, h - m - 60), (w // 2, h - m - 110), (w - m, h - m - 70), (w - m, h - m), (m, h - m)],
              fill=(196, 120, 52, 255))                                          # dune
    # camel silhouette
    cx, cy = w // 2 - 10, h - m - 60
    sil = (60, 34, 20, 255)
    d.ellipse([cx - 36, cy - 30, cx + 30, cy + 6], fill=sil)
    d.ellipse([cx - 14, cy - 46, cx + 12, cy - 20], fill=sil)
    for lx in (-28, -14, 10, 22):
        d.rectangle([cx + lx, cy, cx + lx + 6, cy + 36], fill=sil)
    d.polygon([(cx + 24, cy - 16), (cx + 46, cy - 46), (cx + 56, cy - 44), (cx + 34, cy - 8)], fill=sil)
    d.ellipse([cx + 46, cy - 54, cx + 66, cy - 40], fill=sil)
    # palm tree
    px, py = m + 34, h - m - 40
    d.line([(px, py), (px + 6, py - 70)], fill=sil, width=6)
    for a in range(0, 360, 60):
        d.line([(px + 6, py - 70), (px + 6 + math.cos(math.radians(a)) * 30, py - 70 + math.sin(math.radians(a)) * 14 - 6)],
               fill=sil, width=5)
    return im


def postmark(w, h, seed):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    col = (40, 40, 60, 210)
    d.ellipse([10, 10, 170, 170], outline=col, width=6)
    d.ellipse([28, 28, 152, 152], outline=col, width=3)
    for i in range(5):
        y = 40 + i * 24
        pts = [(150 + x, y + 8 * math.sin(x / 18.0)) for x in range(0, w - 150, 4)]
        d.line(pts, fill=col, width=5)
    ink = noise(w, h, seed, 0, 255, 1.0).point(lambda v: 255 if v > 60 else v * 3)
    im.putalpha(ImageChops.multiply(im.getchannel("A"), ink))
    return im


def halftone(w, h):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    step = 16
    for y in range(0, h + step, step):
        for x in range(0, w + step, step):
            off = step // 2 if (y // step) % 2 else 0
            t = 1 - min(1.0, math.hypot(x / w, y / h) / 1.2)
            r = 1 + 6.5 * t
            d.ellipse([x + off - r, y - r, x + off + r, y + r], fill=(220, 40, 60, 255))
    return im


def static_strip(w, h, seed):
    g = noise(w, h, seed, 0, 255)
    a = noise(w, h, seed + 3, 0, 255).point(lambda v: 200 if v > 150 else 0)
    return Image.merge("RGBA", (g, g, g, a)).filter(ImageFilter.BoxBlur(0.6))


def scanlines(w, h):
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    for y in range(0, h, 4):
        d.line([(0, y), (w, y)], fill=(0, 0, 0, 90), width=2)
    return im


def main():
    atlas = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    sprites = []
    cursor = [0, 0, 0]  # x, y, row height

    def place(name, im):
        x, y, rowh = cursor
        if x + im.width > SIZE:
            x, y, rowh = 0, y + rowh + 2, 0
        assert y + im.height <= SIZE, name
        atlas.paste(im, (x, y))
        sprites.append((name, x, y, im.width, im.height))
        cursor[:] = [x + im.width + 2, y, max(rowh, im.height)]

    place("Vignette", vignette(512, 512))
    place("Camo", camo(512, 512, 11))
    place("Paper", paper(512, 512, 3, (240, 236, 222), 8))
    place("Manila", paper(512, 512, 5, (214, 184, 128), 12))
    place("Newsprint", paper(512, 512, 9, (226, 220, 200), 10))
    place("Halftone", halftone(384, 384))
    place("Tape", tape(320, 96, 21))
    place("PaperClip", paperclip(110, 300))
    place("Stamp", stamp_box(560, 170, 31))
    place("DogTag", dog_tag(300, 470))
    place("Postage", postage(220, 270, 41))
    place("Postmark", postmark(380, 180, 43))
    place("Static", static_strip(1024, 48, 51))
    place("Scanlines", scanlines(256, 256))

    atlas.save(os.path.join(GEN, "photo_frames.png"), optimize=True)
    with open(os.path.join(GEN, "photo_frames.h"), "w", newline="\n") as f:
        f.write("// Generated by scripts/gen_photo_frames.py - do not edit.\n")
        f.write("// Photo mode frame sprites in photo_frames.png (RCDATA 303): x, y, w, h in atlas pixels.\n")
        f.write("#pragma once\n\nnamespace generated {\n")
        f.write(f"constexpr int kPhotoAtlasSize = {SIZE};\n")
        f.write("struct PhotoSprite { int x, y, w, h; };\n")
        for name, x, y, w, h in sprites:
            f.write(f"constexpr PhotoSprite kPhoto{name} = {{{x}, {y}, {w}, {h}}};\n")
        f.write("}  // namespace generated\n")
    print(len(sprites), "sprites,", os.path.getsize(os.path.join(GEN, "photo_frames.png")) // 1024, "KB")


if __name__ == "__main__":
    main()

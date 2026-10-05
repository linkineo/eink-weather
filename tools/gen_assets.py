#!/usr/bin/env python3
"""Generate the 1-bit fonts and icons used by main/ui.c -> main/ui_assets.{c,h}.

    python3 tools/gen_assets.py

Fonts: Barlow / Barlow Condensed (SIL Open Font License 1.1, see
THIRD_PARTY_LICENSES.md), downloaded once from the google/fonts repository into
build/fonts/. Glyphs are rasterised with Pillow and thresholded to 1 bit.
Icons are drawn with Pillow at 4x and downsampled, so they stay crisp at 1 bit.
Requires Pillow (pip install pillow).
"""
import math
import os
import urllib.request

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(ROOT, "build", "fonts")
OUT_C = os.path.join(ROOT, "main", "ui_assets.c")
OUT_H = os.path.join(ROOT, "main", "ui_assets.h")

FONT_URLS = {
    "Barlow-Medium.ttf": "https://github.com/google/fonts/raw/main/ofl/barlow/Barlow-Medium.ttf",
    "Barlow-SemiBold.ttf": "https://github.com/google/fonts/raw/main/ofl/barlow/Barlow-SemiBold.ttf",
    "BarlowCondensed-Bold.ttf": "https://github.com/google/fonts/raw/main/ofl/barlowcondensed/BarlowCondensed-Bold.ttf",
}

TEXT_CHARS = "".join(chr(c) for c in range(32, 127)) + "àâçéèêëîïôùûüÀÂÇÉÈÊÎÔÙÛ°²·’—"
NUM_CHARS = " 0123456789,.-°"

# name, ttf, pixel size, charset
FONTS = [
    ("font_title", "Barlow-SemiBold.ttf", 22, TEXT_CHARS),
    ("font_label", "Barlow-SemiBold.ttf", 15, TEXT_CHARS),
    ("font_body", "Barlow-Medium.ttf", 15, TEXT_CHARS),
    ("font_small", "Barlow-Medium.ttf", 13, TEXT_CHARS),
    ("font_unit", "Barlow-SemiBold.ttf", 13, TEXT_CHARS),
    ("font_val", "BarlowCondensed-Bold.ttf", 32, NUM_CHARS),
    ("font_mid", "BarlowCondensed-Bold.ttf", 96, NUM_CHARS),
    ("font_big", "BarlowCondensed-Bold.ttf", 136, NUM_CHARS),
]

THRESHOLD = 110  # 0..255 coverage; a bit below half keeps strokes solid


def fetch_fonts():
    os.makedirs(CACHE, exist_ok=True)
    for name, url in FONT_URLS.items():
        path = os.path.join(CACHE, name)
        if not os.path.exists(path):
            print("downloading", name)
            urllib.request.urlretrieve(url, path)


def pack_rows(img):
    """1-bit packed rows, MSB first, ceil(w/8) bytes per row; 1 = black."""
    w, h = img.size
    px = img.load()
    out = bytearray()
    for y in range(h):
        for bx in range(0, w, 8):
            b = 0
            for i in range(8):
                x = bx + i
                if x < w and px[x, y]:
                    b |= 0x80 >> i
            out.append(b)
    return out


def render_font(ttf, size, chars):
    font = ImageFont.truetype(os.path.join(CACHE, ttf), size)
    ascent, descent = font.getmetrics()
    h = ascent + descent
    # Top of the capitals / digits within the glyph box, used to place text by cap top.
    cap_top = font.getbbox("H0", anchor="ls")[1] + ascent
    glyphs = []
    for ch in chars:
        adv = max(1, int(round(font.getlength(ch))))
        left, _, right, _ = font.getbbox(ch, anchor="ls")
        x0 = min(0, left)
        w = max(adv, right) - x0
        img = Image.new("L", (max(1, w), h), 0)
        ImageDraw.Draw(img).text((-x0, ascent), ch, font=font, fill=255, anchor="ls")
        bw = img.point(lambda v: 1 if v >= THRESHOLD else 0, "1")
        glyphs.append((ord(ch), bw, adv, x0))
    return h, ascent, cap_top, glyphs


# ---- icons (drawn at 4x, white background = 0, ink = 1) ------------------------
S = 4


def icon_canvas(w, h):
    img = Image.new("L", (w * S, h * S), 0)
    return img, ImageDraw.Draw(img)


def finish(img, w, h):
    return img.resize((w, h), Image.LANCZOS).point(lambda v: 1 if v >= 100 else 0, "1")


def line(d, pts, width):
    d.line([(x * S, y * S) for x, y in pts], fill=255, width=int(width * S), joint="curve")
    for x, y in (pts[0], pts[-1]):
        r = width * S / 2
        d.ellipse([x * S - r, y * S - r, x * S + r, y * S + r], fill=255)


def icon_sun(n=26):
    img, d = icon_canvas(n, n)
    c = n / 2
    r = n * 0.18
    d.ellipse([(c - r) * S, (c - r) * S, (c + r) * S, (c + r) * S], fill=255)
    for k in range(8):
        a = k * math.pi / 4
        line(d, [(c + math.cos(a) * n * 0.32, c + math.sin(a) * n * 0.32),
                 (c + math.cos(a) * n * 0.44, c + math.sin(a) * n * 0.44)], n * 0.09)
    return finish(img, n, n)


def icon_house(n=18):
    img, d = icon_canvas(n, n)
    k = n / 24
    line(d, [(3 * k, 11 * k), (12 * k, 4 * k), (21 * k, 11 * k), (21 * k, 21 * k),
             (3 * k, 21 * k), (3 * k, 11 * k)], 2.4 * k)
    line(d, [(10 * k, 21 * k), (10 * k, 15 * k), (14 * k, 15 * k), (14 * k, 21 * k)], 2.4 * k)
    return finish(img, n, n)


def hollow(img, stroke):
    """Keep only a `stroke`-wide outline of a filled shape."""
    er = img.filter(ImageFilter.MinFilter(int(stroke * S) | 1))
    return Image.composite(Image.new("L", img.size, 0), img, er)


def icon_cloud_rain(n=18):
    k = n / 24
    img, d = icon_canvas(n, n)
    for cx, cy, r in ((8, 11, 4.6), (13, 8.5, 5.6), (17.5, 11.5, 3.6)):
        d.ellipse([(cx - r) * k * S, (cy - r) * k * S, (cx + r) * k * S, (cy + r) * k * S], fill=255)
    d.rectangle([8 * k * S, 11 * k * S, 17.5 * k * S, 15.1 * k * S], fill=255)
    img = hollow(img, 2.2 * k)
    d = ImageDraw.Draw(img)
    for x in (8, 12, 16):
        line(d, [(x * k, 18 * k), ((x - 1) * k, 21 * k)], 2.2 * k)
    return finish(img, n, n)


def icon_drop(n=16, filled=True):
    img, d = icon_canvas(n, n)
    k = n / 24
    # Teardrop: tip at (12, 3), round bottom centred at (12, 14.5), r = 6.
    left = [(12 - 6 * math.sin(u * math.pi / 2), 3 + 11.5 * u) for u in (i / 20 for i in range(21))]
    bottom = [(12 + 6 * math.cos(a), 14.5 + 6 * math.sin(a))
              for a in (math.pi - i * math.pi / 30 for i in range(31))]
    right = [(24 - x, y) for x, y in reversed(left)]
    d.polygon([(x * k * S, y * k * S) for x, y in left + bottom + right], fill=255)
    if not filled:
        img = hollow(img, 2.4 * k)
    return finish(img, n, n)


def icon_wifi(n=14):
    img, d = icon_canvas(n, n)
    k = n / 24
    for r in (13, 9.5, 5.5):
        d.arc([(12 - r) * k * S, (20 - r) * k * S, (12 + r) * k * S, (20 + r) * k * S],
              start=225, end=315, fill=255, width=int(2.6 * k * S))
    rr = 1.6 * k * S
    d.ellipse([12 * k * S - rr, 20 * k * S - rr, 12 * k * S + rr, 20 * k * S + rr], fill=255)
    return finish(img, n, n)


ICONS = [
    ("icon_sun", icon_sun),
    ("icon_house", icon_house),
    ("icon_cloud_rain", icon_cloud_rain),
    ("icon_drop", lambda: icon_drop(16, True)),
    ("icon_drop_empty", lambda: icon_drop(16, False)),
    ("icon_wifi", icon_wifi),
]


def c_bytes(data, indent="    "):
    lines = []
    for i in range(0, len(data), 16):
        lines.append(indent + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    return "\n".join(lines)


def main():
    fetch_fonts()
    c = ['// GENERATED by tools/gen_assets.py - do not edit.',
         '// Fonts: Barlow / Barlow Condensed, SIL Open Font License 1.1 (THIRD_PARTY_LICENSES.md).',
         '#include "ui_assets.h"', '']
    h = ['// GENERATED by tools/gen_assets.py - do not edit.',
         '#ifndef _UI_ASSETS_H_', '#define _UI_ASSETS_H_', '', '#include "ui_types.h"', '']
    for name, ttf, size, chars in FONTS:
        fh, ascent, cap_top, glyphs = render_font(ttf, size, chars)
        bits = bytearray()
        entries = []
        for cp, img, adv, x0 in glyphs:
            entries.append("    {0x%04x, %d, %d, %d, %d}," % (cp, img.size[0], adv, x0, len(bits)))
            bits += pack_rows(img)
        c.append("static const uint8_t %s_bits[] = {\n%s\n};" % (name, c_bytes(bits)))
        c.append("static const ui_glyph_t %s_glyphs[] = {\n%s\n};" % (name, "\n".join(entries)))
        c.append("const ui_font_t %s = {%d, %d, %d, %d, %s_glyphs, %s_bits};\n"
                 % (name, fh, ascent, cap_top, len(glyphs), name, name))
        h.append("extern const ui_font_t %s;   // %s %dpx" % (name, ttf[:-4], size))
    h.append("")
    for name, fn in ICONS:
        img = fn()
        c.append("static const uint8_t %s_bits[] = {\n%s\n};" % (name, c_bytes(pack_rows(img))))
        c.append("const ui_bitmap_t %s = {%d, %d, %s_bits};\n" % (name, img.size[0], img.size[1], name))
        h.append("extern const ui_bitmap_t %s;" % name)
    h += ["", "#endif // _UI_ASSETS_H_", ""]
    with open(OUT_C, "w") as f:
        f.write("\n".join(c))
    with open(OUT_H, "w") as f:
        f.write("\n".join(h))
    print("wrote", OUT_C, "and", OUT_H)


if __name__ == "__main__":
    main()

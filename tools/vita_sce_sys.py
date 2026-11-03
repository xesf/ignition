#!/usr/bin/env python3
"""Writes the Vita package's LiveArea images into OUT/:

    icon0.png                      128 x 128
    livearea/contents/bg.png       840 x 500
    livearea/contents/startup.png  280 x 158

With DISC, the extracted disc, the images are made from the game's own
pictures: the installer's backdrop (INSTALL.PIC) and the logo
(BALTAZAR/DATA/IGN_LOGO.PIC). Without it, or when those files are
missing, a road running to the horizon on a dusk sky is drawn instead, so
no art from the game is kept in the repository. The PNGs are 8 bit
paletted, as the LiveArea needs.

    tools/vita_sce_sys.py OUT [DISC [ICON]]

With ICON, a picture file (JPEG or PNG; needs Pillow), icon0.png is made
from it instead: the middle square of the picture, scaled to 128 x 128.
"""

import os
import struct
import sys
import zlib

DUSK = [
    (16, 12, 32),       # 0 night
    (60, 28, 72),       # 1 dusk
    (196, 72, 52),      # 2 horizon
    (244, 160, 56),     # 3 sun
    (40, 40, 44),       # 4 road
    (236, 228, 200),    # 5 lines
    (28, 64, 36),       # 6 grass
]


def dusk_pixel(x, y, w, h):
    horizon = h * 0.55
    if y < horizon:
        t = y / horizon
        sun_x, sun_y, sun_r = w * 0.5, horizon, h * 0.22
        if (x - sun_x) ** 2 + (y - sun_y) ** 2 < sun_r ** 2 and int(y) % 6 < 4:
            return 3
        return 0 if t < 0.35 else 1 if t < 0.8 else 2
    # The road narrows to a point on the horizon.
    d = (y - horizon) / (h - horizon)
    half = d * w * 0.42
    off = x - w * 0.5
    if abs(off) > half:
        return 6
    if abs(off) < max(1.0, half * 0.03) and int(d * 24 + 0.5) % 2 == 0:
        return 5
    if abs(abs(off) - half * 0.92) < max(1.0, half * 0.03):
        return 5
    return 4


def write_png(path, w, h, pixels, palette):
    """pixels: w * h palette indices; palette: up to 256 (r, g, b)."""
    rows = b"".join(b"\0" + bytes(pixels[y * w:(y + 1) * w]) for y in range(h))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 3, 0, 0, 0))
            + chunk(b"PLTE", b"".join(bytes(c) for c in palette))
            + chunk(b"IDAT", zlib.compress(rows, 9))
            + chunk(b"IEND", b""))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def dusk(out):
    for name, w, h in IMAGES:
        pixels = [dusk_pixel(x, y, w, h) for y in range(h) for x in range(w)]
        write_png(os.path.join(out, name), w, h, pixels, DUSK)


# ------------------------------------------------------------ the game's art

class Picture:
    """An RGB picture, rows of (r, g, b); None is see-through."""

    def __init__(self, w, h, rgb):
        self.w, self.h, self.rgb = w, h, rgb

    def crop(self, x, y, w, h):
        return Picture(w, h, [self.rgb[(y + j) * self.w + x + i]
                              for j in range(h) for i in range(w)])

    def scaled(self, w, h):
        """Each pixel the average of the source pixels it covers."""
        out = []
        sx, sy = self.w / w, self.h / h
        for j in range(h):
            y0, y1 = j * sy, (j + 1) * sy
            for i in range(w):
                x0, x1 = i * sx, (i + 1) * sx
                r = g = b = a = cover = 0.0
                for y in range(int(y0), min(self.h, int(y1 + 0.999))):
                    fy = min(y + 1, y1) - max(y, y0)
                    for x in range(int(x0), min(self.w, int(x1 + 0.999))):
                        f = fy * (min(x + 1, x1) - max(x, x0))
                        if f <= 0:
                            continue
                        cover += f
                        c = self.rgb[y * self.w + x]
                        if c is not None:
                            r += c[0] * f
                            g += c[1] * f
                            b += c[2] * f
                            a += f
                # A pixel mostly see-through stays so.
                if a < cover * 0.5:
                    out.append(None)
                else:
                    out.append((round(r / a), round(g / a), round(b / a)))
        return Picture(w, h, out)

    def paste(self, other, x, y):
        for j in range(other.h):
            for i in range(other.w):
                c = other.rgb[j * other.w + i]
                if c is not None and 0 <= x + i < self.w and 0 <= y + j < self.h:
                    self.rgb[(y + j) * self.w + x + i] = c


def read_pic(path, clear=None):
    """A .PIC: a 0x40-byte header (width at +6, height at +8), an 8-byte
    colour header, 256 colours of 8 bits at 0x48, a 6-byte pixel header,
    then the pixels at 0x34e. Index `clear` is see-through."""
    with open(path, "rb") as f:
        d = f.read()
    w, h = struct.unpack_from("<HH", d, 6)
    pal = [tuple(d[0x48 + 3 * i:0x4b + 3 * i]) for i in range(256)]
    px = d[0x34e:0x34e + w * h]
    return Picture(w, h, [None if p == clear else pal[p] for p in px])


def quantise(pic):
    """Palette indices and a palette of 256 colours at most: the most used
    colours are kept, and the rest take the nearest of them."""
    count = {}
    for c in pic.rgb:
        count[c] = count.get(c, 0) + 1
    palette = sorted(count, key=lambda c: -count[c])[:256]
    index = {c: i for i, c in enumerate(palette)}
    near = {}
    pixels = []
    for c in pic.rgb:
        i = index.get(c)
        if i is None:
            i = near.get(c)
            if i is None:
                i = min(range(len(palette)), key=lambda k: sum(
                    (a - b) ** 2 for a, b in zip(palette[k], c)))
                near[c] = i
        pixels.append(i)
    return pixels, palette


def game_art(out, disc):
    backdrop = read_pic(os.path.join(disc, "INSTALL.PIC"))
    logo = read_pic(os.path.join(disc, "BALTAZAR", "DATA", "IGN_LOGO.PIC"), clear=0)
    # INSTALL.PIC is 640 x 480, its logo and "DOS INSTALLATION" in the top
    # third. Below those, the cars on the track.
    below = backdrop.crop(0, 152, 640, 328)

    # The LiveArea: the cars, cut to its shape, with the logo doubled.
    bg = below.crop(44, 0, 551, 328).scaled(840, 500)
    big = logo.scaled(logo.w * 2, logo.h * 2)
    bg.paste(big, (840 - big.w) // 2, 24)

    # The gate: the logo on the cars.
    startup = below.crop(40, 6, 560, 316).scaled(280, 158)
    startup.paste(logo, (280 - logo.w) // 2, (158 - logo.h) // 2)

    # The bubble: the logo's lightning badge, doubled, on the cars.
    icon = below.crop(200, 40, 256, 256).scaled(128, 128)
    badge = logo.crop(logo.w - 53, 0, 53, logo.h)
    badge = badge.scaled(badge.w * 2, badge.h * 2)
    icon.paste(badge, (128 - badge.w) // 2, (128 - badge.h) // 2)

    for name, pic in (("icon0.png", icon),
                      ("livearea/contents/bg.png", bg),
                      ("livearea/contents/startup.png", startup)):
        pixels, palette = quantise(pic)
        write_png(os.path.join(out, name), pic.w, pic.h, pixels, palette)


IMAGES = [
    ("icon0.png", 128, 128),
    ("livearea/contents/bg.png", 840, 500),
    ("livearea/contents/startup.png", 280, 158),
]


def icon_from_file(out, path):
    """icon0.png from a picture: its square, from a little above the
    middle, as 128 x 128 and 256 colours."""
    try:
        from PIL import Image
    except ImportError:
        print("no Pillow: not using " + path)
        return
    im = Image.open(path).convert("RGB")
    side = min(im.size)
    top = max(0, min(im.height - side, (im.height - side) * 2 // 5))
    left = (im.width - side) // 2
    im = im.crop((left, top, left + side, top + side)).resize((128, 128), Image.LANCZOS)
    q = im.quantize(256, method=Image.Quantize.MEDIANCUT)
    flat = q.getpalette()[:768]
    palette = [tuple(flat[3 * i:3 * i + 3]) for i in range(len(flat) // 3)]
    write_png(os.path.join(out, "icon0.png"), 128, 128, list(q.getdata()), palette)
    print("icon from " + path)


def main():
    if len(sys.argv) not in (2, 3, 4):
        sys.exit(__doc__)
    out = sys.argv[1]
    disc = sys.argv[2] if len(sys.argv) >= 3 else ""
    icon = sys.argv[3] if len(sys.argv) == 4 else ""
    if disc and os.path.isfile(os.path.join(disc, "INSTALL.PIC")) \
            and os.path.isfile(os.path.join(disc, "BALTAZAR", "DATA", "IGN_LOGO.PIC")):
        game_art(out, disc)
        print("LiveArea art from " + disc)
    else:
        dusk(out)
    if icon and os.path.isfile(icon):
        icon_from_file(out, icon)


if __name__ == "__main__":
    main()

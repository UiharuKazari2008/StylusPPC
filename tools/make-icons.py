"""Draws the icons of WMDroid's Windows Mobile programs (glossy, WM 6.5 style) and writes them as
classic BMP-format .ico files (Windows CE does not read PNG-compressed icons) next to each
program's sources: guest/<Program>/app.ico. tools/build-guest.ps1 embeds them.

Run: python tools/make-icons.py   (needs Pillow)
"""
import math
import os
import struct

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIZES = (16, 24, 32, 48, 64)
SS = 4  # supersampling


def glossy_tile(d, s, top, bottom, radius=0.22):
    """A rounded glossy tile filling the canvas (s = side in pixels)."""
    r = int(s * radius)
    m = int(s * 0.04)
    grad = Image.new("RGBA", (s, s))
    gd = ImageDraw.Draw(grad)
    for y in range(s):
        t = y / (s - 1)
        c = tuple(int(top[i] + (bottom[i] - top[i]) * t) for i in range(3)) + (255,)
        gd.line([(0, y), (s, y)], fill=c)
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([m, m, s - 1 - m, s - 1 - m], r, fill=255)
    tile = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    tile.paste(grad, (0, 0), mask)
    # Gloss on the upper half.
    gloss = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    gdraw = ImageDraw.Draw(gloss)
    gdraw.rounded_rectangle([m + s * 0.02, m + s * 0.02, s - 1 - m - s * 0.02, s * 0.5], r, fill=(255, 255, 255, 70))
    tile = Image.alpha_composite(tile, Image.composite(gloss, Image.new("RGBA", (s, s)), mask))
    # Rim.
    ImageDraw.Draw(tile).rounded_rectangle([m, m, s - 1 - m, s - 1 - m], r, outline=(0, 0, 0, 110), width=max(1, s // 48))
    d.alpha_composite(tile)


def gear(s):
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    glossy_tile(img, s, (96, 150, 230), (24, 62, 150))
    d = ImageDraw.Draw(img)
    cx = cy = s / 2
    ro, ri, teeth = s * 0.33, s * 0.25, 8
    pts = []
    for i in range(teeth * 2):
        a0 = math.pi * 2 * i / (teeth * 2)
        r = ro if i % 2 == 0 else ri
        for da in (-0.17, 0.17):
            a = a0 + da
            pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    d.polygon(pts, fill=(240, 244, 250, 255))
    d.ellipse([cx - s * 0.1, cy - s * 0.1, cx + s * 0.1, cy + s * 0.1], fill=(40, 80, 170, 255))
    return img


def globe(s):
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    glossy_tile(img, s, (70, 170, 120), (16, 92, 60))
    d = ImageDraw.Draw(img)
    c, r = s * 0.46, s * 0.27
    d.ellipse([c - r, c + s * 0.06 - r, c + r, c + s * 0.06 + r], fill=(60, 140, 230, 255), outline=(230, 245, 255, 255), width=max(1, s // 32))
    w = max(1, s // 40)
    d.ellipse([c - r * 0.45, c + s * 0.06 - r, c + r * 0.45, c + s * 0.06 + r], outline=(230, 245, 255, 220), width=w)
    d.line([c - r, c + s * 0.06, c + r, c + s * 0.06], fill=(230, 245, 255, 220), width=w)
    # A satellite above right with signal arcs.
    sx, sy = s * 0.74, s * 0.24
    d.rectangle([sx - s * 0.05, sy - s * 0.03, sx + s * 0.05, sy + s * 0.03], fill=(255, 190, 40, 255))
    d.rectangle([sx - s * 0.15, sy - s * 0.02, sx - s * 0.06, sy + s * 0.02], fill=(250, 240, 200, 255))
    d.rectangle([sx + s * 0.06, sy - s * 0.02, sx + s * 0.15, sy + s * 0.02], fill=(250, 240, 200, 255))
    for k in (0.12, 0.2):
        d.arc([sx - s * k, sy - s * k + s * 0.08, sx + s * k, sy + s * k + s * 0.08], 100, 170, fill=(255, 255, 255, 230), width=w)
    return img


def send(s):
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    glossy_tile(img, s, (60, 140, 235), (20, 70, 170))
    d = ImageDraw.Draw(img)
    w = max(1, s // 14)
    # Arrow pointing up-right.
    d.line([s * 0.28, s * 0.72, s * 0.62, s * 0.38], fill=(255, 255, 255, 255), width=w)
    d.polygon([(s * 0.70, s * 0.30), (s * 0.48, s * 0.33), (s * 0.67, s * 0.52)], fill=(255, 255, 255, 255))
    # Radio waves.
    for k in (0.16, 0.26):
        d.arc([s * 0.68 - s * k, s * 0.32 - s * k, s * 0.68 + s * k, s * 0.32 + s * k], -80, 10,
              fill=(220, 236, 255, 230), width=max(1, s // 24))
    return img


def lobby(s):
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    glossy_tile(img, s, (210, 120, 70), (120, 48, 36))
    d = ImageDraw.Draw(img)
    def person(cx, cy, scale, fill):
        r = s * 0.07 * scale
        d.ellipse([cx - r, cy - r * 2.4, cx + r, cy - r * 0.3], fill=fill)
        d.pieslice([cx - r * 2.1, cy - r * 0.2, cx + r * 2.1, cy + r * 2.6], 200, 340, fill=fill)
    person(s * 0.38, s * 0.58, 1.15, (255, 244, 230, 255))
    person(s * 0.62, s * 0.60, 0.92, (255, 220, 190, 255))
    return img


def write_ico(path, draw):
    images = []
    for size in SIZES:
        big = draw(size * SS)
        img = big.resize((size, size), Image.LANCZOS)
        images.append(img)
    entries, blobs = [], []
    offset = 6 + 16 * len(images)
    for img in images:
        w, h = img.size
        px = img.load()
        xor = bytearray()
        for y in range(h - 1, -1, -1):
            for x in range(w):
                r, g, b, a = px[x, y]
                xor += bytes((b, g, r, a))
        row = ((w + 31) // 32) * 4
        andm = bytearray()
        for y in range(h - 1, -1, -1):
            bits = bytearray(row)
            for x in range(w):
                if px[x, y][3] < 128:
                    bits[x // 8] |= 0x80 >> (x % 8)
            andm += bits
        header = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, 32, 0, len(xor) + len(andm), 0, 0, 0, 0)
        blob = header + xor + andm
        entries.append(struct.pack("<BBBBHHII", w % 256, h % 256, 0, 0, 1, 32, len(blob), offset))
        blobs.append(blob)
        offset += len(blob)
    with open(path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)))
        for e in entries:
            f.write(e)
        for b in blobs:
            f.write(b)
    print("wrote", path)


if __name__ == "__main__":
    write_ico(os.path.join(ROOT, "guest", "Helper", "app.ico"), gear)
    write_ico(os.path.join(ROOT, "guest", "GpsTest", "app.ico"), globe)
    os.makedirs(os.path.join(ROOT, "guest", "SendBluetooth"), exist_ok=True)
    write_ico(os.path.join(ROOT, "guest", "SendBluetooth", "app.ico"), send)
    os.makedirs(os.path.join(ROOT, "guest", "InfraLobby"), exist_ok=True)
    write_ico(os.path.join(ROOT, "guest", "InfraLobby", "app.ico"), lobby)

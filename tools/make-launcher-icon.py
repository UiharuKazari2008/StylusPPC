"""Turns a square icon picture on a plain blue background into Android adaptive-icon layers.

usage: python tools/make-launcher-icon.py <source.png> [preview-dir]
Writes app/src/main/res/mipmap-*/ic_launcher_foreground.png and ic_launcher_monochrome.png
(108 dp layers), the background colours, and a 512 px store icon (tools/ic_launcher-512.png).
"""
import os
import sys
from collections import deque

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "app", "src", "main", "res")
DENSITIES = {"mdpi": 108, "hdpi": 162, "xhdpi": 216, "xxhdpi": 324, "xxxhdpi": 432}


def background_mask(im):
    """Flood-fills the blue backdrop from the borders (each row compared with its own edge colour)."""
    w, h = im.size
    px = im.load()
    row_bg = [px[2, y] for y in range(h)]

    def is_bg(x, y):
        r, g, b = px[x, y]
        br, bg_, bb = row_bg[y]
        # Blue backdrop: blue-dominant and close to the row's edge colour (the shadow is darker blue).
        return b > 140 and b > r + 60 and abs(r - br) + abs(g - bg_) + abs(b - bb) < 90

    seen = bytearray(w * h)
    q = deque()
    for x in range(w):
        q.append((x, 0)); q.append((x, h - 1))
    for y in range(h):
        q.append((0, y)); q.append((w - 1, y))
    while q:
        x, y = q.popleft()
        i = y * w + x
        if seen[i] or not is_bg(x, y):
            continue
        seen[i] = 1
        if x > 0: q.append((x - 1, y))
        if x < w - 1: q.append((x + 1, y))
        if y > 0: q.append((x, y - 1))
        if y < h - 1: q.append((x, y + 1))
    mask = Image.new("L", (w, h), 255)
    mp = mask.load()
    for y in range(h):
        for x in range(w):
            if seen[y * w + x]:
                mp[x, y] = 0
    return mask


def main():
    src = sys.argv[1]
    preview = sys.argv[2] if len(sys.argv) > 2 else None
    im = Image.open(src).convert("RGB")
    mask = background_mask(im)
    # Clean edges: drop specks, then a 1 px soften.
    mask = mask.filter(ImageFilter.MinFilter(3)).filter(ImageFilter.MaxFilter(3))
    fg = im.convert("RGBA")
    fg.putalpha(mask)
    box = mask.getbbox()
    fg = fg.crop(box)

    # Adaptive icons: 108 dp canvas, 72 dp visible, the centre 66 dp circle always visible. The
    # subject's longer side gets 50 dp: its rounded pixel corners stay inside a circular mask.
    canvas = 1024
    bw, bh = fg.size
    k = canvas * 50 / 108 / max(bw, bh)
    fg = fg.resize((max(1, round(bw * k)), max(1, round(bh * k))), Image.LANCZOS)
    layer = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    layer.paste(fg, ((canvas - fg.width) // 2, (canvas - fg.height) // 2), fg)

    # Monochrome (themed icons, Android 13+): one colour; the launcher tints it.
    mono = Image.new("RGBA", (canvas, canvas), (0, 0, 0, 0))
    lp = layer.load()
    mp = mono.load()
    for y in range(canvas):
        for x in range(canvas):
            r, g, b, a = lp[x, y]
            if a < 200:
                continue
            # Solid silhouette; the bluish screen is cut out as a window.
            if not (b > r + 40 and b > 120):
                mp[x, y] = (255, 255, 255, 255)

    # Smooth away single-pixel specks from the anti-aliased edges.
    a = mono.getchannel("A").filter(ImageFilter.MedianFilter(7))
    mono = Image.new("RGBA", (canvas, canvas), (255, 255, 255, 0))
    mono.putalpha(a)

    for name, size in DENSITIES.items():
        d = os.path.join(RES, "mipmap-" + name)
        os.makedirs(d, exist_ok=True)
        layer.resize((size, size), Image.LANCZOS).save(os.path.join(d, "ic_launcher_foreground.png"))
        # (The themed icon is drawable/ic_launcher_monochrome.xml, a hand-drawn vector; this traced
        # one only feeds the preview.)

    # Background colours: the source's top and bottom edge, as a vertical gradient.
    px = im.load()
    top = px[im.width // 2, 2]
    bottom = px[im.width // 2, im.height - 3]
    print("background top #%02X%02X%02X bottom #%02X%02X%02X" % (top + bottom))

    # Store icon (512, full bleed square) and previews.
    full = Image.new("RGBA", (canvas, canvas))
    dr = ImageDraw.Draw(full)
    for y in range(canvas):
        t = y / (canvas - 1)
        dr.line([(0, y), (canvas, y)], fill=tuple(round(top[i] * (1 - t) + bottom[i] * t) for i in range(3)) + (255,))
    full.alpha_composite(layer)
    full.resize((512, 512), Image.LANCZOS).save(os.path.join(HERE, "ic_launcher-512.png"))
    if preview:
        os.makedirs(preview, exist_ok=True)
        # What a launcher shows: the centre 72 dp of the 108 dp layers.
        cut = round(canvas * 18 / 108)
        full = full.crop((cut, cut, canvas - cut, canvas - cut)).resize((canvas, canvas), Image.LANCZOS)
        mono = mono.crop((cut, cut, canvas - cut, canvas - cut)).resize((canvas, canvas), Image.LANCZOS)
        circle = Image.new("L", (canvas, canvas), 0)
        ImageDraw.Draw(circle).ellipse((0, 0, canvas, canvas), fill=255)
        round_icon = Image.new("RGBA", (canvas, canvas), (255, 255, 255, 0))
        round_icon.paste(full, (0, 0), circle)
        squircle = Image.new("L", (canvas, canvas), 0)
        ImageDraw.Draw(squircle).rounded_rectangle((0, 0, canvas, canvas), radius=canvas // 4, fill=255)
        sq = Image.new("RGBA", (canvas, canvas), (255, 255, 255, 0))
        sq.paste(full, (0, 0), squircle)
        themed = Image.new("RGBA", (canvas, canvas), (255, 255, 255, 0))
        tint_bg = Image.new("RGBA", (canvas, canvas), (0xDD, 0xE3, 0xF8, 255))
        tint_fg = Image.new("RGBA", (canvas, canvas), (0x2A, 0x3F, 0x7A, 255))
        tint_bg.paste(tint_fg, (0, 0), mono)
        themed.paste(tint_bg, (0, 0), circle)
        sheet = Image.new("RGBA", (canvas * 3 + 80, canvas + 40), (240, 240, 240, 255))
        for i, p in enumerate((round_icon, sq, themed)):
            sheet.alpha_composite(p, (20 + i * (canvas + 20), 20))
        sheet.convert("RGB").resize((sheet.width // 3, sheet.height // 3), Image.LANCZOS).save(
            os.path.join(preview, "icon-preview.png"))


if __name__ == "__main__":
    main()

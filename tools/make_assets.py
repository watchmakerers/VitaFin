#!/usr/bin/env python3
"""Generate the LiveArea / bubble artwork. The Vita needs these as 8-bit indexed PNGs."""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "sce_sys")

PURPLE = (0xAA, 0x5C, 0xC3)
BLUE = (0x00, 0xA4, 0xDC)


def gradient(w, h, a, b, diagonal=True):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = ((x / w) * 0.5 + (y / h) * 0.5) if diagonal else y / h
            px[x, y] = tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
    return img


def emblem(draw, cx, cy, r, fill=(255, 255, 255)):
    """A rounded play triangle inside a soft ring: an original mark, not the Jellyfin logo."""
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=fill, width=max(2, r // 9))
    pts = []
    for k in range(3):
        a = math.radians(k * 120)
        pts.append((cx + r * 0.08 + math.cos(a) * r * 0.55, cy + math.sin(a) * r * 0.55))
    draw.polygon(pts, fill=fill)


def font(size):
    for f in ("/usr/share/fonts/noto/NotoSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def save_indexed(img, path):
    img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(path, optimize=True)
    print("wrote", os.path.relpath(path, ROOT))


def icon0():
    img = gradient(128, 128, PURPLE, BLUE)
    d = ImageDraw.Draw(img)
    emblem(d, 64, 64, 40)
    save_indexed(img, os.path.join(OUT, "icon0.png"))


def pic0():
    img = gradient(960, 544, (0x1a, 0x12, 0x2a), (0x04, 0x2a, 0x40))
    glow = Image.new("RGB", (960, 544))
    gd = ImageDraw.Draw(glow)
    gd.ellipse([520, 60, 1000, 540], fill=(70, 50, 110))
    img = Image.blend(img, glow.filter(ImageFilter.GaussianBlur(90)), 0.35)
    d = ImageDraw.Draw(img)
    emblem(d, 760, 290, 120, fill=(255, 255, 255))
    save_indexed(img, os.path.join(OUT, "pic0.png"))


def bg():
    img = gradient(840, 500, (0x22, 0x16, 0x34), (0x03, 0x30, 0x48))
    d = ImageDraw.Draw(img)
    for i in range(6):
        r = 180 + i * 70
        d.ellipse([640 - r, 420 - r, 640 + r, 420 + r], outline=(255, 255, 255), width=1)
    img = Image.blend(gradient(840, 500, (0x22, 0x16, 0x34), (0x03, 0x30, 0x48)), img, 0.18)
    save_indexed(img, os.path.join(OUT, "livearea", "contents", "bg.png"))


def startup():
    img = gradient(280, 158, PURPLE, BLUE)
    d = ImageDraw.Draw(img)
    emblem(d, 62, 79, 38)
    f = font(34)
    d.text((112, 52), "VitaFin", font=f, fill=(255, 255, 255))
    d.text((114, 94), "for Jellyfin", font=font(16), fill=(235, 235, 245))
    save_indexed(img, os.path.join(OUT, "livearea", "contents", "startup.png"))


if __name__ == "__main__":
    os.makedirs(os.path.join(OUT, "livearea", "contents"), exist_ok=True)
    icon0()
    pic0()
    bg()
    startup()
    sys.exit(0)

#!/usr/bin/env python3
"""Generate main/boot_art/boot_art.bin, the pre-rendered layers of the boot animation.

The boot animation (main/startup_anim.cpp) is the ice-melt reveal of the Arctic
mark. Its static layers are rendered here once, at full 720x1280 resolution, and
embedded in the firmware LZ4-compressed; the device only animates them.

Layers (all RGB565A8, i.e. a 16-bit colour plane followed by an 8-bit alpha plane):
    0 logo    crisp iceberg icon + ARCTIC wordmark (cropped to its bounds)
    1 frozen  the same mark blurred and paled, as seen through the ice
    2 tag     the HEAT PUMPS tagline
    3 ice     the ice block texture (frost, cracks, beveled rim) at ICE_X, ICE_Y

Blob layout (little-endian):
    u32 magic 'BART', u16 version, u16 count
    count x { u16 id, u16 cf, i16 x, i16 y, u16 w, u16 h, u32 raw_size, u32 offset, u32 comp_size }
    LZ4 block payloads

Requires: numpy, pillow, lz4.  Font: Anton (SIL OFL 1.1, see OFL.txt).

    python tools/boot_art/gen_boot_art.py            # writes main/boot_art/boot_art.bin
    python tools/boot_art/gen_boot_art.py --preview  # also writes boot_art_preview.png here
"""
import math
import os
import struct
import sys

import lz4.block
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(ROOT, "main", "boot_art", "boot_art.bin")
FONT = os.path.join(HERE, "Anton-Regular.ttf")

W, H = 720, 1280
BLUE = (15, 117, 188)
CYAN = (1, 175, 240)
ORANGE = (247, 148, 30)

# Must match startup_anim.cpp.
ICE_X, ICE_Y, ICE_W, ICE_H = 60, 260, 600, 570

rng = np.random.default_rng(7)


def build_logo():
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    s = 260
    x0, y0 = (W - s) // 2, 300
    wl = y0 + int(s * 0.52)  # waterline
    d.rounded_rectangle([x0, y0, x0 + s, y0 + s], 18, fill=ORANGE)
    d.rectangle([x0, wl, x0 + s, y0 + s - 1], fill=BLUE)
    cx = x0 + s / 2

    def pts(p):  # icon units: u in [-0.5, 0.5] from the centre, v relative to the waterline
        return [(cx + u * s, wl + v * s) for (u, v) in p]

    d.polygon(pts([(-0.42, 0), (-0.26, -0.17), (-0.15, -0.24), (-0.03, -0.40), (0.04, -0.33),
                   (0.13, -0.27), (0.24, -0.15), (0.42, 0)]), fill=(255, 255, 255))
    d.polygon(pts([(-0.42, 0), (0.42, 0), (0.34, 0.16), (0.22, 0.24), (0.14, 0.40), (0.02, 0.31),
                   (-0.12, 0.42), (-0.22, 0.26), (-0.33, 0.14)]), fill=CYAN)
    m = Image.new("L", (W, H), 0)
    ImageDraw.Draw(m).rounded_rectangle([x0, y0, x0 + s, y0 + s], 18, fill=255)
    im.putalpha(Image.fromarray(np.minimum(np.array(im.split()[3]), np.array(m))))
    f = ImageFont.truetype(FONT, 190)
    bb = d.textbbox((0, 0), "ARCTIC", font=f)
    d.text(((W - (bb[2] - bb[0])) // 2 - bb[0], 600 - bb[1]), "ARCTIC", font=f, fill=BLUE + (255,))
    return im


def build_tagline():
    im = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(FONT, 64)
    txt = "H E A T   P U M P S"
    bb = d.textbbox((0, 0), txt, font=f)
    d.text(((W - (bb[2] - bb[0])) // 2 - bb[0], 985 - bb[1]), txt, font=f, fill=ORANGE + (255,))
    return np.asarray(im).astype(np.float32) / 255


def build_frozen(logo_im):
    blur = np.asarray(logo_im.filter(ImageFilter.GaussianBlur(5))).astype(np.float32) / 255
    grey = blur[..., :3].mean(axis=2, keepdims=True)
    rgb = 0.55 * blur[..., :3] + 0.45 * (grey * 0.5 + np.array([0.75, 0.88, 0.97]) * 0.5)
    return np.dstack([rgb, blur[..., 3] * 0.9])


def build_ice():
    h, w = ICE_H, ICE_W
    noise = rng.random((h // 40 + 2, w // 40 + 2))
    tex = np.asarray(Image.fromarray((noise * 255).astype(np.uint8)).resize((w, h), Image.BICUBIC)
                     .filter(ImageFilter.GaussianBlur(18))).astype(np.float32) / 255
    vert = np.linspace(0, 1, h)[:, None]
    base = np.array([0.62, 0.82, 0.95])
    rgb = base * (0.85 + 0.25 * tex[..., None]) + 0.10 * (1 - vert[..., None])
    a = 0.38 + 0.18 * tex + 0.08 * (1 - vert)
    img = Image.new("L", (w, h), 0)
    d = ImageDraw.Draw(img)
    for _ in range(7):  # cracks
        x, y = rng.uniform(0, w), rng.uniform(0, h)
        ang = rng.uniform(0, math.pi * 2)
        for _ in range(rng.integers(4, 9)):
            ang += rng.normal(0, 0.6)
            ln = rng.uniform(25, 70)
            nx, ny = x + math.cos(ang) * ln, y + math.sin(ang) * ln
            d.line([(x, y), (nx, ny)], fill=int(rng.uniform(140, 255)), width=int(rng.integers(1, 3)))
            x, y = nx, ny
    cracks = np.asarray(img.filter(ImageFilter.GaussianBlur(0.6))).astype(np.float32) / 255
    rgb = rgb * (1 - cracks[..., None] * 0.5) + cracks[..., None] * 0.5
    a = np.clip(a + cracks * 0.3, 0, 1)
    m = Image.new("L", (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle([0, 0, w - 1, h - 1], 34, fill=255)
    mask = np.asarray(m.filter(ImageFilter.GaussianBlur(2))).astype(np.float32) / 255
    rim = np.asarray(m.filter(ImageFilter.GaussianBlur(14))).astype(np.float32) / 255
    edge = np.clip((mask - rim) * 3, 0, 1)
    rgb = rgb * (1 - edge[..., None] * 0.6) + edge[..., None] * 0.6
    a = np.clip(a + edge * 0.35, 0, 1) * mask
    return np.dstack([np.clip(rgb, 0, 1), a])


def to_rgb565a8(rgba):
    b = (np.clip(rgba, 0, 1) * 255 + 0.5).astype(np.uint8)
    r, g, bl = (b[..., i].astype(np.uint16) for i in range(3))
    c = ((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3)
    return c.astype("<u2").tobytes() + b[..., 3].tobytes()


def crop(rgba):
    ys, xs = np.nonzero(rgba[..., 3] > 0.5 / 255)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    return rgba[y0:y1, x0:x1], int(x0), int(y0)


def main():
    logo_im = build_logo()
    logo = np.asarray(logo_im).astype(np.float32) / 255
    frozen = build_frozen(logo_im)
    tag = build_tagline()
    ice = build_ice()

    layers = []
    for lid, rgba in ((0, logo), (1, frozen), (2, tag)):
        c, x, y = crop(rgba)
        layers.append((lid, x, y, c))
    layers.append((3, ICE_X, ICE_Y, ice))

    header = struct.pack("<4sHH", b"BART", 1, len(layers))
    table, payload = b"", b""
    base = len(header) + 24 * len(layers)
    for lid, x, y, rgba in layers:
        raw = to_rgb565a8(rgba)
        comp = lz4.block.compress(raw, mode="high_compression", compression=12, store_size=False)
        h, w = rgba.shape[:2]
        table += struct.pack("<HHhhHHIII", lid, 0, x, y, w, h, len(raw), base + len(payload), len(comp))
        payload += comp
        print(f"layer {lid}: {w}x{h} at ({x},{y})  {len(raw)} -> {len(comp)} bytes")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, "wb") as f:
        f.write(header + table + payload)
    print(f"wrote {OUT} ({len(header) + len(table) + len(payload)} bytes)")

    if "--preview" in sys.argv:
        yy = np.linspace(0, 1, H)[:, None, None]
        img = np.array([10, 16, 30]) / 255 * (1 - yy) + np.array([27, 48, 80]) / 255 * yy
        img = img * np.ones((H, W, 3))
        for lid, x, y, rgba in layers:
            if lid == 1:
                continue
            h, w = rgba.shape[:2]
            a = rgba[..., 3:4]
            img[y:y + h, x:x + w] = img[y:y + h, x:x + w] * (1 - a) + rgba[..., :3] * a
        Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8)).save(os.path.join(HERE, "boot_art_preview.png"))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate the app icon (48x48, SMDH) and banner (256x128, .cia) from
code, so the art is reproducible and tweakable in-repo.

Motif: a CRT-dark screen with scanlines, a stereo pair of wireframe cubes
(magenta = left eye, cyan = right eye, white where they fuse) and a green
terminal prompt. Run:  python3 assets/gen_assets.py
"""
import math
import os
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

BG = (8, 9, 16)
SCAN = (14, 16, 26)
MAGENTA = (255, 64, 200)
CYAN = (64, 224, 255)
FUSE = (235, 235, 245)
GREEN = (80, 240, 120)
BEZEL = (40, 44, 60)


def cube_edges(cx, cy, size, yaw=0.6, pitch=0.42):
    """Project a wireframe cube (orthographic) -> list of 2D segments."""
    verts = []
    for x in (-1, 1):
        for y in (-1, 1):
            for z in (-1, 1):
                # yaw about Y, then pitch about X
                x1 = x * math.cos(yaw) + z * math.sin(yaw)
                z1 = -x * math.sin(yaw) + z * math.cos(yaw)
                y1 = y * math.cos(pitch) - z1 * math.sin(pitch)
                verts.append((cx + x1 * size, cy - y1 * size))
    edges = []
    for i in range(8):
        for j in range(i + 1, 8):
            diff = i ^ j
            if diff in (1, 2, 4):  # differ in exactly one axis bit
                edges.append((verts[i], verts[j]))
    return edges


def draw_stereo_cube(im, cx, cy, size, sep, width):
    """Magenta/cyan offset cubes composited additively -> white where fused."""
    layers = []
    for dx, color in ((-sep, MAGENTA), (sep, CYAN)):
        layer = Image.new("RGB", im.size, (0, 0, 0))
        d = ImageDraw.Draw(layer)
        for a, b in cube_edges(cx + dx, cy, size):
            d.line([a, b], fill=color, width=width)
        layers.append(layer)
    from PIL import ImageChops
    add = ImageChops.add(layers[0], layers[1])
    mask = add.convert("L").point(lambda v: 255 if v > 8 else 0)
    im.paste(ImageChops.lighter(im, add), (0, 0), mask)


def scanlines(draw, w, h, step=3):
    for y in range(0, h, step):
        draw.line([(0, y), (w, y)], fill=SCAN)


def make_icon(path):
    im = Image.new("RGB", (48, 48), BG)
    d = ImageDraw.Draw(im)
    scanlines(d, 48, 48)
    draw_stereo_cube(im, cx=24, cy=19, size=11, sep=2, width=1)
    d = ImageDraw.Draw(im)
    # terminal prompt:  > and a block cursor
    d.line([(6, 38), (11, 41)], fill=GREEN, width=2)
    d.line([(11, 41), (6, 44)], fill=GREEN, width=2)
    d.rectangle([16, 39, 21, 44], fill=GREEN)
    # CRT bezel frame
    d.rectangle([0, 0, 47, 47], outline=BEZEL)
    im.save(path)


def big_text(text, scale):
    """Default bitmap font upscaled with nearest-neighbor -> chunky pixels."""
    font = ImageFont.load_default()
    x0, y0, x1, y1 = font.getbbox(text)
    small = Image.new("RGB", (x1 - x0 + 2, y1 - y0 + 2), (0, 0, 0))
    ImageDraw.Draw(small).text((1 - x0, 1 - y0), text, font=font,
                               fill=(255, 255, 255))
    return small.resize((small.width * scale, small.height * scale),
                        Image.NEAREST)


def tint(mono, color):
    r, g, b = mono.split()
    return Image.merge("RGB", (r.point(lambda v: v * color[0] // 255),
                               g.point(lambda v: v * color[1] // 255),
                               b.point(lambda v: v * color[2] // 255)))


def make_banner(path):
    im = Image.new("RGB", (256, 128), BG)
    d = ImageDraw.Draw(im)
    scanlines(d, 256, 128)
    draw_stereo_cube(im, cx=207, cy=60, size=25, sep=5, width=2)

    title = big_text("3dBBS", 5)
    mask = title.convert("L").point(lambda v: 255 if v > 64 else 0)
    im.paste(tint(title, GREEN), (12, 30), mask)

    sub = big_text("BBS terminal in stereoscopic 3D", 1)
    mask = sub.convert("L").point(lambda v: 255 if v > 64 else 0)
    im.paste(tint(sub, (150, 160, 180)), (16, 96), mask)

    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 255, 127], outline=BEZEL)
    im.save(path)


if __name__ == "__main__":
    make_icon(os.path.join(HERE, "icon.png"))
    make_banner(os.path.join(HERE, "banner.png"))
    print("wrote assets/icon.png (48x48), assets/banner.png (256x128)")

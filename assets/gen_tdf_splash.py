#!/usr/bin/env python3
"""Bake TheDraw-font renderings of the product name into a C table.

The splash only ever draws one string, so there is no reason to ship a TDF
parser (or the 22MB font library) on the console: the fonts are parsed here
and only the finished cell grids are compiled in.

TheDraw fonts cover ASCII 33..126 via a 94-entry offset table, and a great
many of them simply omit the digits — 0xFFFF in that table. "3D BBS" needs
a '3', so slightly over half the library cannot render it at all. Those are
dropped here rather than silently rendering "D BBS" on the device.

Usage:
  gen_tdf_splash.py <tdfonts-dir> [--text "3D BBS"] [--budget-kb 96]

Writes source/gfx/tdf_splash_data.c/.h and assets/TDF-FONTS-CREDITS.md.
"""
import argparse
import glob
import hashlib
import os
import sys

MAGIC = b"\x13TheDraw FONTS file\x1a"
SEP = b"\x55\xaa\x00\xff"
NUM_CHARS = 94
HDR = 213
TYPE_COLOR = 2

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


class Font:
    __slots__ = ("name", "type", "spacing", "glyphs", "src")


def parse_glyph(data, off, is_color):
    if off + 2 > len(data):
        return None
    w, h = data[off], data[off + 1]
    if not (0 < w <= 200 and 0 < h <= 100):
        return None
    cells = {}
    i, x, y = off + 2, 0, 0
    while i < len(data):
        ch = data[i]
        i += 1
        if ch == 0x00:
            break
        if ch == 0x0D:
            y, x = y + 1, 0
            continue
        attr = 0x0F
        if is_color:
            if i >= len(data):
                break
            attr = data[i]
            i += 1
        if y < h and x < w:
            cells[(y, x)] = (ch, attr)
        x += 1
    return w, h, cells


def parse_file(path):
    raw = open(path, "rb").read()
    if not raw.startswith(MAGIC):
        return []
    fonts, pos = [], raw.find(SEP)
    while pos != -1:
        if pos + HDR > len(raw):
            break
        namelen = raw[pos + 4]
        f = Font()
        f.name = raw[pos + 5:pos + 5 + min(namelen, 16)].decode(
            "cp437", "replace").strip("\x00").strip()
        f.type = raw[pos + 21]
        f.spacing = raw[pos + 22]
        f.src = os.path.basename(path)
        blocksize = int.from_bytes(raw[pos + 23:pos + 25], "little")
        table = raw[pos + 25:pos + 25 + NUM_CHARS * 2]
        gdata = raw[pos + HDR:pos + HDR + blocksize]
        f.glyphs = {}
        for idx in range(NUM_CHARS):
            o = int.from_bytes(table[idx * 2:idx * 2 + 2], "little")
            if o == 0xFFFF or o >= len(gdata):
                continue
            g = parse_glyph(gdata, o, f.type == TYPE_COLOR)
            if g:
                f.glyphs[chr(33 + idx)] = g
        fonts.append(f)
        pos = raw.find(SEP, pos + 1)
    return fonts


def render(font, text):
    out, x, height = {}, 0, 0
    for ch in text:
        if ch == " ":
            x += font.spacing if font.spacing else 4
            continue
        g = font.glyphs.get(ch)
        if not g:
            return None
        gw, gh, cells = g
        for (gy, gx), cell in cells.items():
            out[(gy, x + gx)] = cell
        x += gw
        height = max(height, gh)
    if not out:
        return None
    w = max(px for _, px in out) + 1
    h = max(max(py for py, _ in out) + 1, height)
    return w, h, out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("fontdir")
    ap.add_argument("--text", default="3D BBS")
    ap.add_argument("--budget-kb", type=int, default=96)
    ap.add_argument("--max-width", type=int, default=110)
    ap.add_argument("--min-height", type=int, default=4)
    args = ap.parse_args()

    cand, seen, stats = [], set(), {"fonts": 0, "nodigit": 0, "dup": 0}
    for path in sorted(glob.glob(os.path.join(args.fontdir, "*.tdf"))):
        for f in parse_file(path):
            stats["fonts"] += 1
            if f.type != TYPE_COLOR:
                continue
            r = render(f, args.text)
            if not r:
                stats["nodigit"] += 1
                continue
            w, h, cells = r
            if w > args.max_width or h < args.min_height or h > 20:
                continue
            # Many fonts ship as near-identical variants across files; the
            # rendered art is what matters, so dedupe on that.
            key = hashlib.sha1(repr(sorted(cells.items())).encode()).digest()
            if key in seen:
                stats["dup"] += 1
                continue
            seen.add(key)
            cand.append((f, w, h, cells))

    # Spread the budget across heights so the splash cycles through chunky
    # and delicate fonts rather than whatever sorts first.
    byh = {}
    for c in cand:
        byh.setdefault(c[2], []).append(c)
    for v in byh.values():
        v.sort(key=lambda c: c[1])   # narrow first: more legible when small
    budget = args.budget_kb * 1024
    chosen, used, i = [], 0, 0
    order = sorted(byh)
    while True:
        added = False
        for h in order:
            v = byh[h]
            if i < len(v):
                f, w, hh, cells = v[i]
                cost = w * hh * 2
                if used + cost <= budget:
                    chosen.append(v[i])
                    used += cost
                    added = True
        i += 1
        if not added:
            break

    print(f"fonts parsed        : {stats['fonts']}")
    print(f"cannot render {args.text!r}: {stats['nodigit']}")
    print(f"duplicate renderings: {stats['dup']}")
    print(f"candidates          : {len(cand)}")
    print(f"baked               : {len(chosen)}  ({used // 1024} KB)")

    # The collections ship greyscale "silver" variants next to the colour
    # originals, and roughly a sixth of what survives the digit filter is
    # effectively monochrome. Scoring it here lets the splash favour colour
    # without dropping those fonts from the rotation altogether.
    GREY = {0, 7, 8, 15}
    blob, banners = bytearray(), []
    for f, w, h, cells in chosen:
        nb = chrom = 0
        for y in range(h):
            for x in range(w):
                ch, attr = cells.get((y, x), (32, 0))
                if ch in (32, 0):
                    continue
                nb += 1
                if (attr & 0x0F) not in GREY or ((attr >> 4) & 7) not in GREY:
                    chrom += 1
        chroma = int(255 * chrom / nb) if nb else 0
        banners.append((f, w, h, len(blob), chroma))
        for y in range(h):
            for x in range(w):
                ch, attr = cells.get((y, x), (32, 0))
                blob += bytes((ch, attr))

    hdr = os.path.join(ROOT, "source", "gfx", "tdf_splash_data.h")
    src = os.path.join(ROOT, "source", "gfx", "tdf_splash_data.c")
    with open(hdr, "w") as fp:
        fp.write(f"""// GENERATED by assets/gen_tdf_splash.py — do not edit.
// "{args.text}" rendered in {len(banners)} TheDraw fonts, as CP437 cell
// grids (char, CGA attribute). See assets/TDF-FONTS-CREDITS.md.
#ifndef TDF_SPLASH_DATA_H
#define TDF_SPLASH_DATA_H

#include <3ds/types.h>

typedef struct {{
\tu8 w, h;      // cells
\tu8 chroma;    // 0-255: share of cells using a non-grey CGA colour.
\t              // The collections ship greyscale "silver" variants beside
\t              // the colour originals; a low score marks a banner the
\t              // splash should tint rather than draw as-is.
\tu32 off;      // into tdfCells; w*h pairs of (char, attr)
}} TdfBanner;

#define TDF_BANNER_COUNT {len(banners)}
extern const TdfBanner tdfBanners[TDF_BANNER_COUNT];
extern const u8 tdfCells[];

#endif
""")
    with open(src, "w") as fp:
        fp.write('// GENERATED by assets/gen_tdf_splash.py — do not edit.\n')
        fp.write('#include "tdf_splash_data.h"\n\n')
        fp.write(f"const TdfBanner tdfBanners[TDF_BANNER_COUNT] = {{\n")
        for f, w, h, off, chroma in banners:
            fp.write(f"\t{{ {w}, {h}, {chroma}, {off} }},   // {f.name}\n")
        fp.write("};\n\nconst u8 tdfCells[] = {\n")
        for i in range(0, len(blob), 24):
            fp.write("\t" + ",".join(str(b) for b in blob[i:i + 24]) + ",\n")
        fp.write("};\n")

    cred = os.path.join(ROOT, "assets", "TDF-FONTS-CREDITS.md")
    with open(cred, "w") as fp:
        fp.write("# TheDraw font credits\n\n"
                 "The pre-login splash renders the product name in TheDraw\n"
                 "(`.tdf`) fonts drawn by ANSI-scene artists, mostly in the\n"
                 "1990s. Only the finished renderings of one string are\n"
                 "compiled in (see `assets/gen_tdf_splash.py`); no font file\n"
                 "is redistributed.\n\n"
                 "Fonts come from the collection shipped with Synchronet\n"
                 "(`ctrl/tdfonts/` in SynchronetBBS/sbbs), which is where\n"
                 "the artwork has been redistributed for decades. Font names\n"
                 "are as recorded in each file's header; where a name carries\n"
                 "an artist or group tag it is preserved verbatim.\n\n"
                 "If you are an author here and want a font removed, open an\n"
                 "issue and it will be dropped from the next release.\n\n"
                 "| Font | Source file | Size |\n|---|---|---|\n")
        for f, w, h, _, _c in banners:
            fp.write(f"| {f.name} | {f.src} | {w}x{h} |\n")

    print(f"wrote {os.path.relpath(hdr, ROOT)}, {os.path.relpath(src, ROOT)}, "
          f"{os.path.relpath(cred, ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

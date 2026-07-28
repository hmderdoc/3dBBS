#!/usr/bin/env python3
"""Convert a raw 8xN bitmap font dump (from dumpfont) into a PNG glyph atlas.

Atlas layout: 16x16 grid of glyphs, glyph g at ((g%16)*8, (g//16)*H).
White where a font bit is set, transparent elsewhere — the renderer tints
glyphs with the foreground color at draw time.

Usage: mkfont.py <font.bin> <glyph_height> <out.png>
"""
import struct, sys, zlib


def write_png(path, w, h, rgba):
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    raw = b"".join(b"\x00" + rgba[y * w * 4:(y + 1) * w * 4] for y in range(h))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    with open(path, "wb") as f:
        f.write(png)


def main():
    binpath, gh, outpath = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    data = open(binpath, "rb").read()
    assert len(data) == 256 * gh, f"expected {256*gh} bytes, got {len(data)}"

    w, h = 16 * 8, 16 * gh
    px = bytearray(w * h * 4)
    for g in range(256):
        gx, gy = (g % 16) * 8, (g // 16) * gh
        for row in range(gh):
            bits = data[g * gh + row]
            for col in range(8):
                if bits & (0x80 >> col):
                    o = ((gy + row) * w + gx + col) * 4
                    px[o:o + 4] = b"\xff\xff\xff\xff"
    write_png(outpath, w, h, bytes(px))
    print(f"wrote {outpath}: {w}x{h}")


if __name__ == "__main__":
    main()

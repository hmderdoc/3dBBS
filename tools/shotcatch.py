#!/usr/bin/env python3
"""Receive multi-view captures pushed by a dev build of 3dBBS.

Press R on the console (dev builds only) and it sweeps the viewpoint across
N renders with the scene frozen, streaming each one here — no SD card, no FTP.
The result is assembled into a smooth parallax loop, the "3D photo" effect,
which is what actually conveys depth to someone without the hardware. A
two-view wiggle at real stereo disparity reads as a glitch instead.

This works because the geometry is real: the app renders genuine views at
different eye offsets. Nothing is estimated from a flat image, which is what
depth-map approaches (Facebook's old 3D photos) had to do.

Wire format:
    "3DSHOT03", u16 width, u16 height, u8 bytesPerPixel, u8 viewCount
    then viewCount raw framebuffers, swept from one extreme to the other.

Bytes arrive as the GPU left them: column-major (the panel is mounted
rotated) and BGR. Both are undone here rather than on the console, so a
layout mistake costs a Python edit and not a netload.

Usage:
  shotcatch.py [-o outdir] [-p port] [--ms 70] [--scale 2]
"""
import argparse
import os
import socket
import struct
import sys
import time

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow:  python3 -m pip install pillow")

MAGIC = b"3DSHOT03"


def recv_exactly(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(min(65536, n - len(buf)))
        if not chunk:
            raise EOFError(f"connection closed with {n - len(buf)} bytes left")
        buf += chunk
    return bytes(buf)


def to_image(raw, w, h):
    """Framebuffer bytes -> upright RGB image.

    `w`/`h` are as libctru reports them: 240x400 for the top screen, because
    the panel is mounted rotated and the framebuffer's long axis is its
    height. Upright pixel (x, y) sits at byte (x * 240 + (239 - y)) * 3, so
    memory runs up each column and columns advance with x.

    Read at (w, h) that gives a 240-wide, 400-tall image whose row r is
    column x=r and whose column c is y=239-c. A genuine TRANSPOSE — not a
    rotation, which is a transpose plus a flip and is how this first came out
    sideways — puts x on the horizontal axis, leaving y inverted; the
    vertical flip corrects it.
    """
    src = Image.frombytes("RGB", (w, h), raw)          # 240 wide, 400 tall
    b, g, r = src.split()
    src = Image.merge("RGB", (r, g, b))                # BGR -> RGB
    return (src.transpose(Image.TRANSPOSE)             # -> 400 wide, 240 tall
               .transpose(Image.FLIP_TOP_BOTTOM))


def parallax_gif(views, path, ms, scale):
    # Ping-pong so the loop reverses instead of snapping back to the start,
    # which would put one hard cut in an otherwise smooth orbit.
    seq = views + views[-2:0:-1]
    if scale > 1:
        # Nearest neighbour: 400x240 pixel art on an 8x16 bitmap font, where
        # smooth interpolation just makes mud.
        seq = [f.resize((f.width * scale, f.height * scale), Image.NEAREST)
               for f in seq]
    seq[0].save(path, save_all=True, append_images=seq[1:], duration=ms,
                loop=0, format="GIF", optimize=True, disposal=2)
    return path


def anaglyph(l, r, path, scale):
    if scale > 1:
        l = l.resize((l.width * scale, l.height * scale), Image.NEAREST)
        r = r.resize((r.width * scale, r.height * scale), Image.NEAREST)
    lr, _, _ = l.split()
    _, rg, rb = r.split()
    Image.merge("RGB", (lr, rg, rb)).save(path)
    return path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--outdir", default="shots")
    ap.add_argument("-p", "--port", type=int, default=2327)
    ap.add_argument("--ms", type=int, default=70,
                    help="parallax frame duration in ms (default 70)")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--keep-views", action="store_true",
                    help="also write each individual view as a PNG")
    ap.add_argument("--once", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("0.0.0.0", args.port))
    srv.listen(4)
    print(f"listening on 0.0.0.0:{args.port} — press R on the 3DS", flush=True)

    while True:
        conn, addr = srv.accept()
        try:
            hdr = recv_exactly(conn, 14)
            if hdr[:8] != MAGIC:
                print(f"! {addr[0]}: bad magic {hdr[:8]!r}", flush=True)
                continue
            w, h, bpp, n = struct.unpack("<HHBB", hdr[8:14])
            size = w * h * bpp
            print(f"{addr[0]}: {n} views, {w}x{h} fb, {n * size / 1024:.0f}KB",
                  flush=True)

            views = []
            for i in range(n):
                views.append(to_image(recv_exactly(conn, size), w, h))
                print(f"  view {i + 1}/{n}", flush=True)

            stamp = time.strftime("%Y%m%d_%H%M%S")
            stem = os.path.join(args.outdir, stamp)
            print("wrote", parallax_gif(views, stem + "_parallax.gif",
                                        args.ms, args.scale), flush=True)

            if n >= 4:
                # Anaglyph from a pair a quarter of the way in from each end:
                # the extremes are further apart than is comfortable to fuse.
                a, b = views[n // 4], views[-(n // 4) - 1]
                print("wrote", anaglyph(a, b, stem + "_anaglyph.png",
                                        args.scale), flush=True)

            if args.keep_views:
                for i, v in enumerate(views):
                    v.save(f"{stem}_view{i:02d}.png")
        except (EOFError, OSError) as e:
            print(f"! {addr[0]}: {e}", flush=True)
        finally:
            conn.close()
        if args.once:
            return 0


if __name__ == "__main__":
    sys.exit(main())

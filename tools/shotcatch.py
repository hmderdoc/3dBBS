#!/usr/bin/env python3
"""Receive stereo screen captures pushed by a dev build of 3dBBS.

Press R on the console (dev builds only) and it connects here and sends both
top-screen eye framebuffers — no SD card, no FTP. The build forces a usable
interocular distance for the captured frame, so unlike Rosalina screenshots
the pair has depth regardless of where the physical 3D slider sits.

Wire format:
    "3DSHOT02", u16 width, u16 height, u8 bytesPerPixel, u8 eyeCount
    then eyeCount raw framebuffers, left eye first.

The bytes arrive exactly as the GPU left them: the 3DS framebuffer is stored
column-major (rotated 90 degrees) and BGR rather than RGB. Both are undone
here rather than on the console, so the layout can be corrected without
rebuilding and re-deploying.

Usage:
  shotcatch.py [-o outdir] [-p port] [--convert]
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import time

try:
    from PIL import Image
except ImportError:
    sys.exit("needs Pillow:  python3 -m pip install pillow")

MAGIC = b"3DSHOT02"
HERE = os.path.dirname(os.path.abspath(__file__))


def recv_exactly(conn, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = conn.recv(min(65536, n - len(buf)))
        if not chunk:
            raise EOFError(f"connection closed with {n - len(buf)} bytes to go")
        buf += chunk
    return bytes(buf)


def to_image(raw, w, h):
    """Framebuffer bytes -> upright RGB image.

    `w`/`h` are as libctru reports them: 240x400 for the top screen, because
    the panel is mounted rotated and the framebuffer's long axis is its
    height. Upright pixel (x, y) lives at byte (x * 240 + (239 - y)) * 3,
    so memory runs up each column and columns advance with x.

    Read at (w, h) that makes a 240-wide, 400-tall image whose row r is
    column x=r and whose column c is y=239-c. A genuine TRANSPOSE (not a
    rotation — a rotation is a transpose plus a flip, which is how this
    first came out sideways) lands x on the horizontal axis, leaving y
    inverted; the vertical flip fixes that. Channels are BGR.
    """
    src = Image.frombytes("RGB", (w, h), raw)          # 240 wide, 400 tall
    b, g, r = src.split()
    src = Image.merge("RGB", (r, g, b))                # BGR -> RGB
    return (src.transpose(Image.TRANSPOSE)             # -> 400 wide, 240 tall
               .transpose(Image.FLIP_TOP_BOTTOM))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--outdir", default="shots")
    ap.add_argument("-p", "--port", type=int, default=2327)
    ap.add_argument("--convert", action="store_true",
                    help="run stereo.py on each pair as it lands")
    ap.add_argument("--once", action="store_true", help="exit after one capture")
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
            w, h, bpp, eyes = struct.unpack("<HHBB", hdr[8:14])
            n = w * h * bpp
            print(f"{addr[0]}: {w}x{h} fb, {bpp}Bpp, {eyes} eye(s), "
                  f"{eyes * n / 1024:.0f}KB", flush=True)

            stamp = time.strftime("%Y%m%d_%H%M%S")
            names = []
            for i in range(eyes):
                img = to_image(recv_exactly(conn, n), w, h)
                suffix = "_top.bmp" if i == 0 else "_top_right.bmp"
                path = os.path.join(args.outdir, stamp + suffix)
                img.save(path)
                names.append(path)
                print("  wrote", path, f"({img.width}x{img.height})", flush=True)

            if eyes < 2:
                print("  ! only one eye — the right framebuffer aliased the "
                      "left, so this frame has no stereo in it", flush=True)
            elif args.convert:
                subprocess.run([sys.executable, os.path.join(HERE, "stereo.py"),
                                names[0], names[1], "-o", args.outdir],
                               check=False)
        except (EOFError, OSError) as e:
            print(f"! {addr[0]}: {e}", flush=True)
        finally:
            conn.close()
        if args.once:
            return 0


if __name__ == "__main__":
    sys.exit(main())

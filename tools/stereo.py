#!/usr/bin/env python3
"""Turn a 3DS stereo screenshot pair into something shareable.

Rosalina (Luma3DS: L+Down+Select -> "Take screenshot") writes into
sdmc:/luma/screenshots/:

    <datetime>_top.bmp         top screen, LEFT eye
    <datetime>_bot.bmp         bottom screen
    <datetime>_top_right.bmp   top screen, RIGHT eye

The right-eye file is only written when the console is actually in 3D mode
with differing eye framebuffers — i.e. THE 3D SLIDER MUST BE UP when you
take the shot. With the slider down you get a flat screenshot and no pair.

Outputs (pick any, default all):
  wiggle    animated GIF alternating the eyes. No glasses, no headset —
            motion parallax alone reads as depth. This is the one that
            works on Reddit, Discord, GitHub, phones.
  anaglyph  single PNG for red/cyan glasses.
  sbs       side-by-side, for headsets or free-viewing (--cross swaps the
            eyes for cross-eyed viewing instead of parallel).

Usage:
  stereo.py <left.bmp> <right.bmp> [-o outdir] [--only wiggle]
  stereo.py --pair <dir>            # newest _top/_top_right pair in dir
"""
import argparse
import glob
import os
import sys

try:
    from PIL import Image, ImageChops
except ImportError:
    sys.exit("needs Pillow:  python3 -m pip install pillow")


def find_pair(d):
    tops = sorted(glob.glob(os.path.join(d, "*_top.bmp")))
    if not tops:
        sys.exit(f"no *_top.bmp in {d}")
    for left in reversed(tops):
        right = left[:-len("_top.bmp")] + "_top_right.bmp"
        if os.path.exists(right):
            return left, right
    sys.exit("found left-eye shots but no *_top_right.bmp — the 3D slider "
             "was down when they were taken, so there is no second eye")


def load(path):
    im = Image.open(path).convert("RGB")
    return im


def wiggle(l, r, out, ms):
    # Two frames ping-ponging is the classic wigglegram. Intermediate views
    # are deliberately NOT synthesised: blending two eyes produces ghosting
    # rather than parallax, which looks worse than the honest two-frame loop.
    l.save(out, save_all=True, append_images=[r], duration=ms, loop=0,
           format="GIF", optimize=True, disposal=2)
    return out


def anaglyph(l, r, out):
    # Red from the left eye, green+blue from the right: the standard
    # red/cyan arrangement.
    lr, _, _ = l.split()
    _, rg, rb = r.split()
    Image.merge("RGB", (lr, rg, rb)).save(out)
    return out


def sbs(l, r, out, cross):
    a, b = (r, l) if cross else (l, r)
    w, h = a.size
    canvas = Image.new("RGB", (w * 2, h))
    canvas.paste(a, (0, 0))
    canvas.paste(b, (w, 0))
    canvas.save(out)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("left", nargs="?")
    ap.add_argument("right", nargs="?")
    ap.add_argument("--pair", metavar="DIR",
                    help="use the newest complete pair in this directory")
    ap.add_argument("-o", "--outdir", default=".")
    ap.add_argument("--only", choices=["wiggle", "anaglyph", "sbs"],
                    help="produce just one output")
    ap.add_argument("--ms", type=int, default=120,
                    help="wiggle frame duration in ms (default 120)")
    ap.add_argument("--cross", action="store_true",
                    help="side-by-side for cross-eyed rather than parallel")
    ap.add_argument("--scale", type=int, default=2,
                    help="integer upscale; the top screen is only 400x240 "
                         "(default 2)")
    args = ap.parse_args()

    if args.pair:
        lp, rp = find_pair(args.pair)
    elif args.left and args.right:
        lp, rp = args.left, args.right
    else:
        ap.error("give two files or --pair DIR")

    l, r = load(lp), load(rp)
    if l.size != r.size:
        sys.exit(f"eye images differ in size: {l.size} vs {r.size}")

    if args.scale > 1:
        n = args.scale
        # Nearest neighbour: this is pixel art with a 8x16 bitmap font, and
        # smooth interpolation just makes it mud.
        l = l.resize((l.width * n, l.height * n), Image.NEAREST)
        r = r.resize((r.width * n, r.height * n), Image.NEAREST)

    os.makedirs(args.outdir, exist_ok=True)
    stem = os.path.join(args.outdir,
                        os.path.basename(lp)[:-len("_top.bmp")]
                        if lp.endswith("_top.bmp")
                        else os.path.splitext(os.path.basename(lp))[0])

    made = []
    if args.only in (None, "wiggle"):
        made.append(wiggle(l, r, stem + "_wiggle.gif", args.ms))
    if args.only in (None, "anaglyph"):
        made.append(anaglyph(l, r, stem + "_anaglyph.png"))
    if args.only in (None, "sbs"):
        made.append(sbs(l, r, stem + "_sbs.png", args.cross))

    # Disparity is worth reporting: if it is ~0 the shot has no depth in it
    # (slider was barely up), and if it is large the wiggle will look like a
    # jump cut rather than a parallax shift.
    diff = ImageChops.difference(l, r).convert("L")
    nonzero = sum(1 for p in diff.getdata() if p > 12)
    pct = 100.0 * nonzero / (diff.width * diff.height)
    print(f"left : {lp}\nright: {rp}")
    print(f"pixels differing between eyes: {pct:.1f}%"
          + ("   <-- almost no depth; was the 3D slider up?" if pct < 1.0 else ""))
    for m in made:
        print("wrote", m)


if __name__ == "__main__":
    sys.exit(main())

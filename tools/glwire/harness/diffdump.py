#!/usr/bin/env python3
"""What the host's replay misses, pixel by pixel: the guest's panel against the host's picture.

    diffdump.py PREFIX [-o OUTDIR] [--tolerance N]

Reads the two files GLWIRE_DUMP=<PREFIX> wrote for one frame -- <PREFIX>-panel-WxH.rgba (the guest's
own pixels, RGBA, top row first) and <PREFIX>-picture-WxH.bgra (what the host drew from the record
stream, BGRA, rows from the bottom) -- and reports, as one screen:

  * how many pixels the panel paints that the picture leaves black ("missing"), and the other way
    round ("extra"), with the bounding box of each;
  * a coarse 12 x 26 grid of the missing pixels, so a terminal shows where on the screen they are;
  * three PNGs in OUTDIR: panel.png, picture.png, diff.png (red: only the panel has it, blue: only the
    picture) for looking at.

P1 asks the question this answers: the picture is what the host can draw without the guest, so what is
missing is exactly what the record stream does not carry.

--tolerance N is what P2' asks instead: two renderers never agree pixel for pixel, so "the same frame"
has to be a tolerant question.  It prints how many pixels differ by at most N in every channel, and the
verdict at the plan's line (>= 95% of the pixels within 8).
"""
import glob
import os
import sys

import numpy as np
from PIL import Image


def load_pair(prefix):
    panels = sorted(glob.glob(prefix + "-panel-*.rgba"))
    pictures = sorted(glob.glob(prefix + "-picture-*.bgra"))
    if not panels or not pictures:
        sys.exit(f"diffdump: need both {prefix}-panel-*.rgba and {prefix}-picture-*.bgra")
    panel_path, picture_path = panels[0], pictures[0]
    width, height = map(int, os.path.basename(panel_path).split("-")[-1].split(".")[0].split("x"))
    panel = np.fromfile(panel_path, dtype=np.uint8)
    picture = np.fromfile(picture_path, dtype=np.uint8)
    if panel.size != width * height * 4 or picture.size != width * height * 4:
        sys.exit(f"diffdump: {panel.size}/{picture.size} bytes, expected {width * height * 4}")
    # both as height x width x RGB, the way the screen shows them
    p = panel.reshape(height, width, 4)[:, :, :3].astype(np.int16)
    q = picture.reshape(height, width, 4)[:, :, [2, 1, 0]].astype(np.int16)
    q = q[::-1]                                    # picture rows come bottom first
    return panel_path, picture_path, width, height, p, q


def bbox(mask):
    ys, xs = np.nonzero(mask)
    if ys.size == 0:
        return None
    return int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())


def grid(mask, cols=12, rows=26):
    height, width = mask.shape
    out = []
    for row in range(rows):
        line = []
        for col in range(cols):
            block = mask[row * height // rows:(row + 1) * height // rows,
                         col * width // cols:(col + 1) * width // cols]
            share = block.mean() if block.size else 0.0
            line.append(" .:-=+*#%@"[min(9, int(share * 10))] if share > 0 else " ")
        out.append("".join(line))
    return out


def main():
    args = [a for a in sys.argv[1:]]
    if not args:
        sys.exit(__doc__)
    prefix = args[0]
    outdir = None
    tolerance = 0
    if "--tolerance" in args:
        tolerance = int(args[args.index("--tolerance") + 1])
    if "-o" in args:
        outdir = args[args.index("-o") + 1]
        os.makedirs(outdir, exist_ok=True)
    panel_path, picture_path, width, height, p, q = load_pair(prefix)
    p_sum, q_sum = p.sum(axis=2), q.sum(axis=2)
    missing = (p_sum > 0) & (q_sum == 0)            # the panel has it, the record stream did not
    extra = (q_sum > 0) & (p_sum == 0)
    different = (np.abs(p - q).sum(axis=2) > 12)
    total = width * height
    print(f"diffdump: {width}x{height}")
    print(f"  panel   {panel_path}")
    print(f"  picture {picture_path}")
    print(f"  painted: panel {int((p_sum > 0).sum())} ({100.0 * (p_sum > 0).sum() / total:.1f}%), "
          f"picture {int((q_sum > 0).sum())} ({100.0 * (q_sum > 0).sum() / total:.1f}%)")
    print(f"  missing (panel paints, picture black): {int(missing.sum())} "
          f"({100.0 * missing.sum() / total:.2f}%), box {bbox(missing)}")
    print(f"  extra   (picture paints, panel black):  {int(extra.sum())} "
          f"({100.0 * extra.sum() / total:.2f}%), box {bbox(extra)}")
    print(f"  differing by more than 12/765: {int(different.sum())} "
          f"({100.0 * different.sum() / total:.2f}%), box {bbox(different)}")
    if tolerance > 0:
        channel = np.abs(p - q).max(axis=2)
        within = (channel <= tolerance).sum()
        share = 100.0 * within / total
        print(f"  within {tolerance} in every channel: {int(within)}/{total} ({share:.2f}%) -> "
              f"{'SAME FRAME' if share >= 95.0 else 'NOT the same frame'} (the line is 95%)")
    print("  missing, on a 12 x 26 grid of the screen:")
    for line in grid(missing):
        print("   |" + line + "|")
    if outdir:
        Image.fromarray(p.astype(np.uint8), "RGB").save(os.path.join(outdir, "panel.png"))
        Image.fromarray(q.astype(np.uint8), "RGB").save(os.path.join(outdir, "picture.png"))
        diff = np.zeros((height, width, 3), dtype=np.uint8)
        diff[missing] = (255, 0, 0)
        diff[extra] = (0, 96, 255)
        both = ~missing & ~extra & (p_sum > 0) & (q_sum > 0)
        diff[both] = (40, 40, 40)
        Image.fromarray(diff, "RGB").save(os.path.join(outdir, "diff.png"))
        print(f"  wrote {outdir}/panel.png {outdir}/picture.png {outdir}/diff.png")


if __name__ == "__main__":
    main()

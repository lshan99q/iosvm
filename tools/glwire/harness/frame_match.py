#!/usr/bin/env python3
"""Which saved host picture is the screen showing?

screenshot.png is the panel (1125x2436, RGB); a picture is 750x1624 BGRA.  The kernel scales the
750-wide surface to the panel, so the screenshot is downsampled to 750x1624 by nearest and compared
with each picture: the mean absolute difference and the share of pixels within a tolerance.  The
frame that matches with the smallest difference is what the display was last given -- if none comes
close, the screen is not showing what the host drew and the black screenshot means something else.

    frame_match.py screenshot.png PICTURE_DIR [--tolerance 8] [--top 3]
"""
import argparse
import glob
import os
import sys

import numpy as np
from PIL import Image


def load_picture(path):
    width, height = 750, 1624
    base = os.path.basename(path)
    for part in base.split("-"):
        if "x" in part and part.replace("x", "").isdigit():
            width, height = (int(v) for v in part.split("x"))
    raw = np.fromfile(path, dtype=np.uint8)
    if raw.size != width * height * 4:
        raise SystemExit("%s: %d bytes, expected %d" % (path, raw.size, width * height * 4))
    image = raw.reshape(height, width, 4)[:, :, [2, 1, 0]]
    return image


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("screenshot")
    parser.add_argument("pictures")
    parser.add_argument("--tolerance", type=int, default=8)
    parser.add_argument("--top", type=int, default=3)
    args = parser.parse_args()

    shot = Image.open(args.screenshot).convert("RGB")
    paths = sorted(glob.glob(os.path.join(args.pictures, "*.bgra")),
                   key=lambda p: os.path.getmtime(p))
    if not paths:
        raise SystemExit("no pictures in %s" % args.pictures)
    rows = []
    for path in paths:
        picture = load_picture(path)
        height, width = picture.shape[:2]
        small = np.asarray(shot.resize((width, height), Image.NEAREST), dtype=np.int16)
        diff = np.abs(small - picture.astype(np.int16))
        mean = float(diff.mean())
        near = float((diff.max(axis=2) <= args.tolerance).mean())
        rows.append((mean, near, path))
    print("%8s %8s  %s" % ("mean", "within", "picture"))
    for mean, near, path in sorted(rows)[: args.top]:
        print("%8.2f %7.1f%%  %s" % (mean, near * 100.0, path))
    best = min(rows)
    print("\nbest: %s  mean %.2f  within %d: %.1f%%" % (best[2], best[0], args.tolerance,
                                                         best[1] * 100.0))


if __name__ == "__main__":
    main()

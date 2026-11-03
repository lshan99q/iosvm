#!/usr/bin/env python3
"""The host's picture dumps -> PNG, and a look at what is in them.

    bgra2png.py PREFIX [-o out.png]

host_glwire writes <prefix>-picture-WxH.bgra (BGRA, rows from the bottom, as glReadPixels gives
them) and <prefix>-panel-WxH.rgba (the guest's own pixels, RGBA, top row first).  Both are written
as PNG here, flipped the way the screen shows them, plus a one-line census: how many pixels are
not black, how many distinct colours, and the average of the four channels.
"""
import os
import sys
import glob
import numpy as np
from PIL import Image


def load(path):
    return np.fromfile(path, dtype=np.uint8)


def save(pixels, width, height, path, order):
    arr = pixels.reshape(height, width, 4)
    if order == "bgra":
        arr = arr[:, :, [2, 1, 0, 3]]
    Image.fromarray(arr, "RGBA").transpose(Image.FLIP_TOP_BOTTOM).save(path)
    return arr


def main():
    prefix = sys.argv[1]
    out = None
    if "-o" in sys.argv:
        out = sys.argv[sys.argv.index("-o") + 1]
    for path in sorted(glob.glob(prefix + "-picture-*.bgra")):
        width, height = map(int, os.path.basename(path).split("-")[-1].split(".")[0].split("x"))
        pixels = load(path)
        if pixels.size != width * height * 4:
            print(f"{path}: {pixels.size} bytes, expected {width * height * 4}")
            continue
        arr = save(pixels, width, height, out or (path + ".png"), "bgra")
        rgb = arr[:, :, :3].astype(np.int32)
        notblack = int((rgb.sum(axis=2) > 0).sum())
        colours = len(np.unique(arr.reshape(-1, 4), axis=0))
        print(f"{path}: {width}x{height}, {pixels.size} bytes, not-black {notblack}/{width * height} "
              f"({100.0 * notblack / (width * height):.1f}%), {colours} colours, mean RGBA "
              f"{arr.reshape(-1, 4).mean(axis=0).round(1)}")
    for path in sorted(glob.glob(prefix + "-panel-*.rgba")):
        width, height = map(int, os.path.basename(path).split("-")[-1].split(".")[0].split("x"))
        pixels = load(path)
        if pixels.size != width * height * 4:
            print(f"{path}: {pixels.size} bytes, expected {width * height * 4}")
            continue
        arr = save(pixels, width, height, path + ".png", "rgba")
        rgb = arr[:, :, :3].astype(np.int32)
        notblack = int((rgb.sum(axis=2) > 0).sum())
        colours = len(np.unique(arr.reshape(-1, 4), axis=0))
        print(f"{path}: {width}x{height}, not-black {notblack}/{width * height} "
              f"({100.0 * notblack / (width * height):.1f}%), {colours} colours")


if __name__ == "__main__":
    main()

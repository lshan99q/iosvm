#!/usr/bin/env python3
"""panel_probe.py -- prove that QEMU's console surface follows the guest framebuffer.

The last research note said the console surface was a copy taken at the first mode set, because a
magenta probe written into the panel never appeared in a screendump. It did appear: QEMU's PPM
writer emits `pixman_image_get_stride` bytes per row, which for a 1125-pixel wide RGB image is 3376
bytes (one padding byte), not 1125 * 3 = 3375. Every reader that assumed packed RGB got a sheared
picture -- one that looks like a frame from the middle of a slide-in animation -- and wrote the
wrong conclusion down.

This script treats the picture the way QEMU writes it, then checks it against the guest's own
framebuffer in the RAM file at three moments:

    1. the settled screen, before anything is written:  every pixel equal;
    2. after a magenta rectangle is written into the guest framebuffer: the rectangle is magenta in
       the screendump, every other pixel still equal;
    3. after the rectangle is restored: every pixel equal again.

    ./panel_probe.py <ram.img> [--base 0x33b9eb000] [--qmp 4445] [--out DIR]

The machine may be running or paused; pausing before the run keeps the guest from drawing over the
probe. Nothing is restarted.
"""
import argparse
import json
import mmap
import os
import socket
import time

import numpy as np
from PIL import Image

RAMP = 0x40000000
W, H = 1125, 2436


def qmp(port, commands):
    s = socket.create_connection(("127.0.0.1", port), timeout=20)
    f = s.makefile("rwb", buffering=0)
    f.readline()                     # the greeting

    def call(obj):
        f.write((json.dumps(obj) + "\n").encode())
        while True:
            line = f.readline()
            if not line:
                raise SystemExit("panel_probe: QMP closed")
            message = json.loads(line)
            if "return" in message or "error" in message:
                return message

    call({"execute": "qmp_capabilities"})
    out = [call(c) for c in commands]
    s.close()
    return out


def screendump(port, path):
    qmp(port, [{"execute": "screendump", "arguments": {"filename": path}}])
    for _ in range(50):
        if os.path.exists(path) and os.path.getsize(path) > 0:
            break
        time.sleep(0.1)
    return path


def read_ppm(path):
    """QEMU's P6, with its one padding byte (the pixman line stride, not width * 3) per row."""
    data = open(path, "rb").read()
    magic, size, maxval, pixels = data.split(b"\n", 3)
    assert magic == b"P6" and maxval == b"255", "not an 8-bit P6"
    width, height = map(int, size.split())
    stride = len(pixels) // height
    assert stride >= width * 3, "a PPM row is shorter than its pixels"
    packed = np.frombuffer(pixels[:stride * height], dtype=np.uint8).reshape(height, stride)
    return packed[:, :width * 3].reshape(height, width, 3), stride


def framebuffer(ram, base):
    offset = base - RAMP
    return np.ndarray((H, W, 4), dtype=np.uint8, buffer=ram, offset=offset)


def compare(rgb, bgra):
    """RGB from the PPM against BGRA from the guest; returns per-pixel differing count."""
    guest = bgra[:, :, [2, 1, 0]]
    return int(np.any(rgb != guest, axis=2).sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ram")
    ap.add_argument("--base", type=lambda v: int(v, 0), default=0x33B9EB000)
    ap.add_argument("--qmp", type=int, default=4445)
    ap.add_argument("--out", default="docs/demo")
    ap.add_argument("--probe", default="40,40,120,120", help="x,y,w,h")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    x, y, pw, ph = (int(v) for v in args.probe.split(","))

    file = open(args.ram, "r+b")
    ram = mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_WRITE)
    if framebuffer(ram, args.base).shape != (H, W, 4):
        raise SystemExit("panel_probe: the framebuffer is not in this RAM file")

    print("panel_probe: 1. the settled screen", flush=True)
    before = read_ppm(screendump(args.qmp, "/tmp/panel_probe_before.ppm"))[0]
    fb = framebuffer(ram, args.base)
    diff = compare(before, fb)
    print("panel_probe:    screendump vs guest framebuffer: %d differing pixels" % diff)
    Image.fromarray(before).save(os.path.join(args.out, "panel-before.png"))

    print("panel_probe: 2. a magenta rectangle at %d,%d %dx%d" % (x, y, pw, ph), flush=True)
    saved = fb[y:y + ph, x:x + pw].copy()
    fb[y:y + ph, x:x + pw] = (255, 0, 255, 255)       # B, G, R, A = magenta
    ram.flush()
    after = read_ppm(screendump(args.qmp, "/tmp/panel_probe_after.ppm"))[0]
    box = after[y:y + ph, x:x + pw]
    magenta = int(np.all(box == (255, 0, 255), axis=2).sum())
    masked = after.copy()
    masked[y:y + ph, x:x + pw] = before[y:y + ph, x:x + pw]
    rest = int(np.any(masked != before, axis=2).sum())
    print("panel_probe:    magenta pixels in the screendump: %d of %d; other differing pixels: %d" %
          (magenta, pw * ph, rest))
    Image.fromarray(after).save(os.path.join(args.out, "panel-probe.png"))
    Image.fromarray(after[y:y + ph, x:x + pw]).resize((pw, ph)).save(
        os.path.join(args.out, "panel-probe-crop.png"))

    print("panel_probe: 3. restoring the framebuffer", flush=True)
    fb[y:y + ph, x:x + pw] = saved
    ram.flush()
    restored = read_ppm(screendump(args.qmp, "/tmp/panel_probe_restored.ppm"))[0]
    rest2 = compare(restored, framebuffer(ram, args.base))
    print("panel_probe:    after restore: %d differing pixels" % rest2)

    ok = diff == 0 and magenta == pw * ph and rest == 0 and rest2 == 0
    print("panel_probe: %s" % ("PASS: the console surface is live" if ok else "FAIL"))
    ram.close()
    file.close()
    return 0 if ok else 1


raise SystemExit(main())

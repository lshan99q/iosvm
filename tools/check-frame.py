#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""check-frame.py — is this screenshot evidence that something was drawn?

    tools/check-frame.py logs/screen.png --min-unique-colors 64
    tools/check-frame.py logs/screen.png --min-unique-colors 64 --json

The v1.0 acceptance test is "a captured frame with at least N distinct colours", because a black
screen, a white screen and a noise field all fail in different ways and only a colour count
catches "the renderer produced nothing". Needs no third-party module: it reads PNGs directly.

Exit codes: 0 pass, 1 fail, 2 usage/read error.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"


def read_png(path: Path) -> tuple[int, int, int, bytes]:
    blob = path.read_bytes()
    if not blob.startswith(PNG_MAGIC):
        raise ValueError("not a PNG (bad magic)")
    pos = len(PNG_MAGIC)
    width = height = depth = ctype = None
    idat = bytearray()
    while pos + 8 <= len(blob):
        length = struct.unpack(">I", blob[pos:pos + 4])[0]
        tag = blob[pos + 4:pos + 8]
        payload = blob[pos + 8:pos + 8 + length]
        pos += 12 + length
        if tag == b"IHDR":
            width, height, depth, ctype, _, _, interlace = struct.unpack(">IIBBBBB", payload)
            if interlace:
                raise ValueError("interlaced PNG is not handled; re-save without interlacing")
        elif tag == b"IDAT":
            idat += payload
        elif tag == b"IEND":
            break
    if width is None:
        raise ValueError("no IHDR")
    if depth != 8 or ctype not in (2, 6):
        raise ValueError(f"only 8-bit RGB/RGBA is handled, got depth {depth} ctype {ctype}")
    channels = 3 if ctype == 2 else 4
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(height * stride)
    prev = bytearray(stride)
    src = 0
    for y in range(height):
        f = raw[src]
        src += 1
        line = bytearray(raw[src:src + stride])
        src += stride
        # Undo the per-scanline filter (PNG spec §9.2). An unknown filter is a corrupt file,
        # not a licence to treat it as filter 0.
        if f > 4:
            raise ValueError(f"unknown PNG filter type {f} on row {y}")
        if f == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif f == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return width, height, channels, bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Check a screenshot for evidence of rendering.")
    ap.add_argument("png")
    ap.add_argument("--min-unique-colors", type=int, default=64,
                    help="fail below this many distinct RGB triples (default: 64)")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    path = Path(args.png)
    if not path.is_file():
        print(f"check-frame: no such file: {path}", file=sys.stderr)
        return 2
    try:
        w, h, ch, px = read_png(path)
    except (ValueError, zlib.error) as exc:
        print(f"check-frame: {path}: {exc}", file=sys.stderr)
        return 2

    colors = set()
    dark = bright = 0
    total = w * h
    for i in range(0, len(px), ch):
        r, g, b = px[i], px[i + 1], px[i + 2]
        if len(colors) < 1 << 20:
            colors.add((r, g, b))
        if r + g + b < 24:
            dark += 1
        elif r + g + b > 720:
            bright += 1

    n = len(colors)
    ok = n >= args.min_unique_colors
    result = {
        "file": str(path),
        "width": w,
        "height": h,
        "unique_colors": n,
        "min_unique_colors": args.min_unique_colors,
        "dark_fraction": round(dark / total, 4),
        "bright_fraction": round(bright / total, 4),
        "pass": ok,
    }
    if args.json:
        print(json.dumps(result, indent=2))
    else:
        verdict = "PASS" if ok else "FAIL"
        print(f"{verdict} {path}: {w}x{h}, {n} unique colours "
              f"(need >= {args.min_unique_colors}), dark {result['dark_fraction']:.1%}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

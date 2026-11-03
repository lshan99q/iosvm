#!/usr/bin/env python3
"""Compare the guest's own composited panel with the host's replayed picture, byte for byte.

The panel is RGBA (what eagl.c uploaded), the picture is BGRA (what glReadPixels returned).
The two are equal only when every logical R, G, B, A byte is equal at the same row and column,
which is the acceptance for "the host replay drew the frame the guest drew".

    ./compare_frame.py panel.rgba picture.bgra [--diff out.png]

Exit status is 0 on an exact match, 1 on a difference, 2 on a malformed input.
"""
import hashlib
import struct
import sys
import zlib


def read(path, width, height, bgra=False):
    data = open(path, "rb").read()
    want = width * height * 4
    if len(data) != want:
        raise SystemExit("%s: %d bytes, expected %d for %dx%d" % (path, len(data), want, width, height))
    if not bgra:
        return data
    out = bytearray(len(data))
    for at in range(0, len(data), 4):
        out[at] = data[at + 2]
        out[at + 1] = data[at + 1]
        out[at + 2] = data[at]
        out[at + 3] = data[at + 3]
    return bytes(out)


def png(path, width, height, pixels):
    def chunk(kind, payload):
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(
            ">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)

    rows = b"".join(b"\0" + pixels[y * width * 4:(y + 1) * width * 4] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
                           chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


def main():
    args = sys.argv[1:]
    if len(args) < 2:
        raise SystemExit(__doc__)
    panel, picture = args[0], args[1]
    width = int(args[args.index("--width") + 1]) if "--width" in args else 750
    height = int(args[args.index("--height") + 1]) if "--height" in args else 1624
    panel_pixels = read(panel, width, height)
    picture_pixels = read(picture, width, height, bgra=True)
    panel_md5 = hashlib.md5(panel_pixels).hexdigest()
    picture_md5 = hashlib.md5(picture_pixels).hexdigest()
    differing = 0
    first = last = None
    low = [width, height]
    high = [-1, -1]
    for y in range(height):
        row = y * width * 4
        for x in range(width):
            at = row + x * 4
            if panel_pixels[at:at + 4] != picture_pixels[at:at + 4]:
                differing += 1
                low[0] = min(low[0], x)
                low[1] = min(low[1], y)
                high[0] = max(high[0], x)
                high[1] = max(high[1], y)
                if first is None:
                    first = (x, y)
                last = (x, y)
    print("compare_frame: %dx%d panel md5 %s picture md5 %s" % (width, height, panel_md5, picture_md5))
    if differing == 0:
        print("compare_frame: MATCH, 0 differing pixels")
        return 0
    print("compare_frame: DIFF, %d differing pixels (%.4f%%), first %s last %s, box %d,%d..%d,%d" %
          (differing, 100.0 * differing / (width * height), first, last,
           low[0], low[1], high[0], high[1]))
    if "--diff" in args:
        out = args[args.index("--diff") + 1]
        diff = bytearray(panel_pixels)
        for y in range(height):
            row = y * width * 4
            for x in range(width):
                at = row + x * 4
                if panel_pixels[at:at + 4] != picture_pixels[at:at + 4]:
                    diff[at:at + 4] = b"\xff\0\xff\xff"
        png(out, width, height, diff)
        print("compare_frame: wrote %s" % out)
    return 1


if __name__ == "__main__":
    sys.exit(main())

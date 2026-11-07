#!/usr/bin/env python3
"""Compare two screenshots with a tolerance, the P0 screenshot-regression question.

    imgdiff.py A.png B.png [--tolerance 8] [--threshold 95]
               [--crop x,y,w,h] [--out DIR]

This is the *screenshot* regression metric: two runs of the same UI are never
pixel-identical, so the question is "what share of pixels agree within N".  It is
not the same-frame check used by diffdump.py (which compares one run's panel and
picture); keep the two apart.

--crop x,y,w,h cuts the same rectangle out of both images before counting, so a
label band can be measured without the rest of the screen.  (x,y) is the top-left
corner in the 1125x2436 screenshot coordinate space, w/h are pixels.

Output, one line per fact so it can be grepped:

    imgdiff: A <path> (WxH RGB)
    imgdiff: B <path> (WxH RGB)
    imgdiff: size match|MISMATCH WxH
    imgdiff: crop x,y WxH (origin x,y)          (only with --crop)
    imgdiff: different pixels: N/M (P%)
    imgdiff: within tolerance T: X/Y (Z%)
    imgdiff: diff bbox: (x0,y0,x1,y1) ...       (right/bottom exclusive)
    imgdiff: out-of-tolerance bbox: ...
    imgdiff: alpha different pixels: N/M (P%)   (only when both images have alpha)
    imgdiff: PASS|FAIL: within tolerance T is Z% (line threshold%)

Only the RGB channels decide PASS/FAIL; alpha, when both images carry it, is
reported on its own line so a transparent-vs-opaque difference cannot hide.

Exit code: 0 when within >= threshold, 1 when below it, 2 on a bad image/crop.

Only the standard library and Pillow are used (no numpy).
"""
import argparse
import os
import sys

from PIL import Image, ImageChops


def fail(message):
    """A usage/input error: exit 2, so it cannot be mistaken for a failed threshold."""
    print(f"imgdiff: {message}", file=sys.stderr)
    sys.exit(2)


def parse_crop(text):
    parts = text.split(",")
    if len(parts) != 4:
        raise argparse.ArgumentTypeError("crop must be x,y,w,h")
    try:
        x, y, w, h = (int(p) for p in parts)
    except ValueError:
        raise argparse.ArgumentTypeError("crop must be four integers: x,y,w,h")
    if w <= 0 or h <= 0 or x < 0 or y < 0:
        raise argparse.ArgumentTypeError("crop needs x,y >= 0 and w,h > 0")
    return x, y, w, h


def load(path):
    try:
        image = Image.open(path)
        image.load()
    except (OSError, ValueError) as error:
        fail(f"cannot read {path}: {error}")
    return image


def crop_same(image, crop, label):
    x, y, w, h = crop
    if x + w > image.width or y + h > image.height:
        fail(f"crop {x},{y} {w}x{h} is outside {label} {image.width}x{image.height}")
    if image.mode in ("RGBA", "LA", "PA"):
        # crop in the native mode so alpha comes along
        return image.crop((x, y, x + w, y + h))
    return image.crop((x, y, x + w, y + h)).convert("RGB")


def has_alpha(image):
    return "A" in image.getbands()


def fmt_box(box, offset):
    if box is None:
        return "none"
    left, upper, right, lower = box
    return f"({left + offset[0]},{upper + offset[1]},{right + offset[0]},{lower + offset[1]})"


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Tolerance-aware screenshot diff for the stage 19 P0 line "
                    "(within tolerance >= 95% passes).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Exit code: 0 when within >= threshold, 1 when below it, 2 on a bad image/crop.")
    parser.add_argument("a", metavar="A.png")
    parser.add_argument("b", metavar="B.png")
    parser.add_argument("--tolerance", type=int, default=8,
                        help="per-channel tolerance, 0..255 (default 8)")
    parser.add_argument("--threshold", type=float, default=95.0,
                        help="pass line as a percentage of pixels (default 95)")
    parser.add_argument("--crop", type=parse_crop, metavar="x,y,w,h",
                        help="compare the same rectangle in both images")
    parser.add_argument("--out", metavar="DIR",
                        help="write diff.png (8x amplified) and A-crop.png/B-crop.png when --crop")
    args = parser.parse_args(argv)

    if not 0 <= args.tolerance <= 255:
        parser.error("--tolerance must be between 0 and 255")
    if not 0.0 <= args.threshold <= 100.0:
        parser.error("--threshold must be between 0 and 100")

    a_image = load(args.a)
    b_image = load(args.b)
    offset = (0, 0)

    print(f"imgdiff: A {args.a} ({a_image.width}x{a_image.height} {a_image.mode})")
    print(f"imgdiff: B {args.b} ({b_image.width}x{b_image.height} {b_image.mode})")

    if args.crop is not None:
        x, y, w, h = args.crop
        offset = (x, y)
        print(f"imgdiff: crop {x},{y} {w}x{h} (origin {x},{y})")
        a_crop = crop_same(a_image, args.crop, "A")
        b_crop = crop_same(b_image, args.crop, "B")
        size_note = "match" if a_crop.size == b_crop.size else "MISMATCH"
        print(f"imgdiff: size {size_note} {a_crop.width}x{a_crop.height}")
        if a_crop.size != b_crop.size:
            fail("cropped sizes differ, cannot compare")
    else:
        print(f"imgdiff: size {'match' if a_image.size == b_image.size else 'MISMATCH'} "
              f"{a_image.width}x{a_image.height}")
        if a_image.size != b_image.size:
            fail("image sizes differ; pass --crop to compare the same rectangle")
        a_crop, b_crop = a_image, b_image

    # Only RGB decides the verdict.  Convert copies so the saved crops keep their
    # original mode (and alpha, for the separate alpha line).
    a_rgb = a_crop.convert("RGB")
    b_rgb = b_crop.convert("RGB")
    width, height = a_rgb.size
    total = width * height

    difference = ImageChops.difference(a_rgb, b_rgb)
    diff_bytes = difference.tobytes()

    tolerance = args.tolerance
    different = 0
    within = 0
    any_box = None
    bad_box = None
    for index in range(0, total * 3, 3):
        dr = diff_bytes[index]
        dg = diff_bytes[index + 1]
        db = diff_bytes[index + 2]
        largest = dr if dr >= dg else dg
        if db > largest:
            largest = db
        if largest == 0:
            within += 1
            continue
        pixel = index // 3
        px = pixel % width
        py = pixel // width
        if any_box is None:
            any_box = [px, py, px + 1, py + 1]
        else:
            if px < any_box[0]:
                any_box[0] = px
            if py < any_box[1]:
                any_box[1] = py
            if px + 1 > any_box[2]:
                any_box[2] = px + 1
            if py + 1 > any_box[3]:
                any_box[3] = py + 1
        if largest <= tolerance:
            within += 1
        else:
            different += 1
            if bad_box is None:
                bad_box = [px, py, px + 1, py + 1]
            else:
                if px < bad_box[0]:
                    bad_box[0] = px
                if py < bad_box[1]:
                    bad_box[1] = py
                if px + 1 > bad_box[2]:
                    bad_box[2] = px + 1
                if py + 1 > bad_box[3]:
                    bad_box[3] = py + 1

    different_pct = 100.0 * different / total if total else 100.0
    within_pct = 100.0 * within / total if total else 100.0
    print(f"imgdiff: different pixels: {different}/{total} ({different_pct:.2f}%)")
    print(f"imgdiff: within tolerance {tolerance}: {within}/{total} ({within_pct:.2f}%)")
    print(f"imgdiff: diff bbox: {fmt_box(any_box, offset)} (right/bottom exclusive)")
    print(f"imgdiff: out-of-tolerance bbox: {fmt_box(bad_box, offset)} (right/bottom exclusive)")

    if has_alpha(a_crop) and has_alpha(b_crop):
        alpha_difference = ImageChops.difference(a_crop.getchannel("A"), b_crop.getchannel("A"))
        alpha_differing = sum(1 for value in alpha_difference.tobytes() if value)
        alpha_pct = 100.0 * alpha_differing / total if total else 0.0
        print(f"imgdiff: alpha different pixels: {alpha_differing}/{total} ({alpha_pct:.2f}%)")

    passed = within_pct >= args.threshold
    verdict = "PASS" if passed else "FAIL"
    print(f"imgdiff: {verdict}: within tolerance {tolerance} is {within_pct:.2f}% "
          f"(line {args.threshold:.2f}%)")

    if args.out is not None:
        try:
            os.makedirs(args.out, exist_ok=True)
            # amplify the difference so a small one is visible; keep it RGB
            amplified = difference.point(lambda value: min(255, value * 8))
            amplified.save(os.path.join(args.out, "diff.png"))
            if args.crop is not None:
                a_crop.save(os.path.join(args.out, "A-crop.png"))
                b_crop.save(os.path.join(args.out, "B-crop.png"))
            print(f"imgdiff: wrote {os.path.join(args.out, 'diff.png')}"
                  + (" and A-crop.png/B-crop.png" if args.crop is not None else ""))
        except OSError as error:
            fail(f"cannot write {args.out}: {error}")

    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())

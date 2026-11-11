#!/usr/bin/env python3
"""Count readable-looking white text lines in a SpringBoard screenshot.

The harness has no OCR (no tesseract/pytesseract in the image).  This is the
documented fallback: connected white components, grouped into horizontal text
lines in the lower part of the screen.  A label line needs at least two
components, a width of 8..300 px and a height of 3..20 px; icon glyphs and the
status bar normally form different shapes.  The count is a *heuristic* and is
reported as `names=` in gate.md.

usage: gate_names.py SHOT.png [--min 5] [--verbose]
exit 0 if count >= min, 1 otherwise, 2 on an unreadable image.
"""
import sys

def main():
    args = sys.argv[1:]
    if not args:
        print("gate_names: usage: gate_names.py SHOT.png [--min N] [--verbose]", file=sys.stderr)
        return 2
    path = args[0]
    minimum = 5
    verbose = False
    i = 1
    while i < len(args):
        if args[i] == "--min" and i + 1 < len(args):
            minimum = int(args[i + 1]); i += 2
        elif args[i] == "--verbose":
            verbose = True; i += 1
        else:
            print(f"gate_names: unknown argument {args[i]}", file=sys.stderr); return 2
    try:
        from PIL import Image
    except Exception as exc:
        print(f"gate_names: PIL unavailable ({exc}); cannot check names", file=sys.stderr)
        return 2
    try:
        im = Image.open(path).convert("RGB")
    except Exception as exc:
        print(f"gate_names: cannot read {path}: {exc}", file=sys.stderr)
        return 2
    w, h = im.size
    px = im.load()
    # white/near-white text, below the status bar and above the dock's bottom edge
    y0, y1 = int(h * 0.18), int(h * 0.92)
    mask = bytearray(w * (y1 - y0))
    for y in range(y0, y1):
        row = (y - y0) * w
        for x in range(w):
            r, g, b = px[x, y]
            if r >= 195 and g >= 195 and b >= 195 and max(r, g, b) - min(r, g, b) <= 45:
                mask[row + x] = 1
    # 8-connected components
    seen = bytearray(len(mask))
    comps = []
    for idx, on in enumerate(mask):
        if not on or seen[idx]:
            continue
        stack = [idx]; seen[idx] = 1
        xs = []; ys = []; area = 0
        while stack:
            p = stack.pop(); area += 1
            yy, xx = divmod(p, w)
            xs.append(xx); ys.append(yy + y0)
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx == 0 and dy == 0:
                        continue
                    nx, ny = xx + dx, yy + dy
                    if 0 <= nx < w and 0 <= ny < y1 - y0:
                        q = ny * w + nx
                        if mask[q] and not seen[q]:
                            seen[q] = 1; stack.append(q)
        bw = max(xs) - min(xs) + 1; bh = max(ys) - min(ys) + 1
        if 2 <= area <= 500 and 2 <= bw <= 100 and 2 <= bh <= 20:
            comps.append((min(xs), min(ys), bw, bh, area))
    # group components into horizontal lines by y-centre and small x gap
    comps.sort(key=lambda c: (c[1], c[0]))
    lines = []
    for c in comps:
        cy = c[1] + c[3] / 2.0
        placed = False
        for line in lines:
            if abs(line["cy"] - cy) <= 5.0 and c[0] - line["x1"] <= 24:
                line["x1"] = max(line["x1"], c[0] + c[2])
                line["x0"] = min(line["x0"], c[0])
                line["n"] += 1
                line["cy"] = (line["cy"] * (line["n"] - 1) + cy) / line["n"]
                placed = True
                break
        if not placed:
            lines.append({"x0": c[0], "x1": c[0] + c[2], "cy": cy, "n": 1})
    good = [l for l in lines if l["n"] >= 2 and 8 <= l["x1"] - l["x0"] <= 320]
    if verbose:
        for l in good:
            print(f"  line y={l['cy']:.0f} x={l['x0']}..{l['x1']} components={l['n']}")
    print(len(good))
    return 0 if len(good) >= minimum else 1

if __name__ == "__main__":
    sys.exit(main())

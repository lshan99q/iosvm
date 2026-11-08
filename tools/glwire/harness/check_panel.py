#!/usr/bin/env python3
"""Sanity-check a GLWIRE_DUMP panel before trusting diffdump.

    check_panel.py PREFIX

Reads PREFIX-panel-*.rgba and PREFIX-picture-*.bgra (and the -n1- pair when
present) and answers, in this order:

  1. is the panel an actual picture?  (unique colours, top colours, painted %,
     pure-black / pure-green share) -- a boot mask is black + one flat green
     block and is rejected here, before any percentage is quoted;
  2. how close panel and picture are in each of the two pairings that matter:
       same index : panel(N) vs picture(N)          (what diffdump does)
       cross pair : panel(N+1) vs picture(N)        (the documented relation
                                                      picture(N) == panel(N+1))
     each with the better row orientation.

Exit: 0 if the panel looks like a picture and the best pairing is >=95% within 8;
      1 if the panel is suspect/mask-like; 2 on missing files.
"""
import glob
import sys

import numpy as np
from PIL import Image


def load_rgba(path):
    w, h = map(int, path.split("-")[-1].split(".")[0].split("x"))
    a = np.fromfile(path, dtype=np.uint8)
    return a.reshape(h, w, 4)


def load_bgra(path):
    w, h = map(int, path.split("-")[-1].split(".")[0].split("x"))
    a = np.fromfile(path, dtype=np.uint8)
    b = a.reshape(h, w, 4)
    return b[:, :, [2, 1, 0, 3]]


def score(panel, picture, tol=8):
    """Better of the two row orientations; returns (share, orientation)."""
    a = panel[:, :, :3].astype(np.int16)
    b = picture[:, :, :3].astype(np.int16)
    best = (-1.0, "raw")
    for name, q in (("raw", b), ("flipped", b[::-1])):
        if q.shape != a.shape:
            continue
        share = float((np.abs(a - q).max(axis=2) <= tol).mean())
        if share > best[0]:
            best = (share, name)
    return best


def describe(panel):
    flat = panel.reshape(-1, 4)
    cols, counts = np.unique(flat, axis=0, return_counts=True)
    order = np.argsort(-counts)[:6]
    painted = int((flat[:, :3].sum(axis=1) > 0).sum())
    black = int(((flat[:, :3].sum(axis=1) == 0) & (flat[:, 3] > 0)).sum())
    green = int(((flat[:, 1] > 200) & (flat[:, 0] < 50) & (flat[:, 2] < 50)).sum())
    total = flat.shape[0]
    print(f"  colours: {len(cols)}; painted {painted}/{total} ({100.0*painted/total:.1f}%)")
    print(f"  black {black} ({100.0*black/total:.1f}%), pure green {green} ({100.0*green/total:.1f}%)")
    for i in order:
        print(f"    {cols[i].tolist()} x {int(counts[i])}")
    # mask verdict: one flat colour covering most non-black pixels, very few colours
    suspect = len(cols) <= 8 or green > 0.10 * total
    print(f"  verdict: {'MASK-LIKE (reject before diffing)' if suspect else 'looks like a picture'}")
    return suspect


def pair(prefix, a_tag, b_tag):
    panels = sorted(glob.glob(f"{prefix}-{a_tag}panel-*.rgba"))
    pictures = sorted(glob.glob(f"{prefix}-{b_tag}picture-*.bgra"))
    if not panels or not pictures:
        print(f"  missing: {a_tag}panel={len(panels)} {b_tag}picture={len(pictures)}")
        return None
    p = load_rgba(panels[0])
    q = load_bgra(pictures[0])
    share, orient = score(p, q)
    print(f"  {panels[0].split('/')[-1]} vs {pictures[0].split('/')[-1]}: "
          f"within 8 = {share*100:.2f}% ({orient})")
    return share


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    prefix = sys.argv[1]
    panels = sorted(glob.glob(prefix + "-panel-*.rgba"))
    if not panels:
        print(f"check_panel: no {prefix}-panel-*.rgba")
        sys.exit(2)
    print(f"panel: {panels[0]}")
    suspect = describe(load_rgba(panels[0]))
    if suspect:
        print("check_panel: panel is mask-like; diffdump percentage would be meaningless")
        sys.exit(1)
    print("pairings (tol 8):")
    s1 = pair(prefix, "", "")
    # cross pair needs the -n1- panel and the plain picture; handle name shapes explicitly
    n1 = sorted(glob.glob(prefix + "-n1-panel-*.rgba"))
    pic = sorted(glob.glob(prefix + "-picture-*.bgra"))
    s2 = None
    if n1 and pic:
        p = load_rgba(n1[0]); q = load_bgra(pic[0])
        share, orient = score(p, q)
        s2 = share
        print(f"  {n1[0].split('/')[-1]} vs {pic[0].split('/')[-1]}: "
              f"within 8 = {share*100:.2f}% ({orient})  [picture(N)==panel(N+1)]")
    best = max([x for x in (s1, s2) if x is not None], default=0.0)
    print(f"best pairing: {best*100:.2f}% -> {'PASS' if best >= 0.95 else 'FAIL'} (line 95%)")
    sys.exit(0 if best >= 0.95 else 1)


if __name__ == "__main__":
    main()

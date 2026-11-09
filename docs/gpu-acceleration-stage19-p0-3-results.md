# Stage 19 — P0 third run + P1 backdrop verdict (one boot)

**Date:** 2026-11-09 19:01–19:18Z.  **P0: PASS, 3/3.**  **P1: C/other — the dotted
line is the bottom row(s) of the 42-high glyph/label tile (P1c), not a 750x200
backdrop tiled on the wire (A) and not the backdrop sampling uncropped to the
glyph rect (B).**  The 90pt backdrop may add a second line, but it is not the
direct source of the visible Weather run.

## P0 three runs

| run | end | biggest shot | names | `diffdump f-n1` tol 8 | panel | 0x502 |
|---|---|---|---|---|---|---|
| v7 | ui 630s | 330,931 B | 10 App + 2 widget | **99.85% SAME** | 6052 colours / green 0 | 0 |
| v9 | ui 630s | 326,461 B | 10 App + 2 widget | **99.98% SAME** | 11,774 colours / green 0 | 0 |
| **p0-3** | **ui 660s** | **327,177 B** | **≥14 readable** | **98.81% SAME** | 8455 colours / green 0 | 0 |

The original criterion was "at least 2 of 3"; all three now pass.  p0-3:
`end=ui ui=660s ui_bytes=310605 ... hostframes=1080 ... panic=0 ... biggest=327177`;
`check_panel`: pure green 0, looks like a picture; same-index 98.82%, cross pair
100.00%; `f-panel` and `f-n1-panel` md5 differ.  The `extra` 14,555 px (1.19%,
box 54,1092,173,1253) is a small UI element, not the v8 89.53% loading-animation
mismatch: here `missing` is 0 and the cross pair is 100%.

Visual note: the UI-detection first frame carries the SpringBoard debug overlay
`rdar:45025538` and a cut right dock; the names are still readable and the 800 s
dump is a different (later) pair.  This is the known first-frame capture race,
not a P0 criterion.

## P1: the dotted line under the labels

At the UI-time Weather label (host log, frame 117):

```
text draw frame 117 #549 program 13 fbo 6 texture 9 178x90 ... bbox 113,433 291,523
  ... texmat 3f800000 3f800000 00000000 00000000 readrect 0,0 178x90 ... u0 tex 69 178x90
text draw frame 117 #550 program 15 ... bbox 137,457 267,499
  ... readrect 0,0 178x90 ... u0 tex 70 130x42
label tile dump tile-f121-t9-178x90.bgra 178x90 nonzero 707 grey 0..255
```

The 178x90 label tile has nonzero rows 34–52 (the glyph) and row 61 (54 px of
dashes), rows 62–89 zero.  The glyph texture is 130x42 (42 rows); its 42 rows map
to the tile's rows 20–61, so the dash row at 61 is the glyph texture's **last
row**, not the 178x90 backdrop's bottom (the backdrop bottom would be row 89, and
rows 62–89 are zero).  The dot band is therefore baked into the 42-high glyph
tile alpha (P1c's 101x42 Notes tile has the same bottom 8 rows), and prog15
carries it into the label tile.

The earlier frame 8 draw shows the same relation with a 750x200 label tile:
`texture 12 750x200 ... readrect 0,0 184x90 ... texmat 1,1,0,0` — the 750x200 is
the early label-tile texture, sampled 1:1 at its top-left 184x90; there is no
wrap/repeat evidence for a 750x200 being tiled.  The 90pt backdrop (178x90) is
the canvas, but the visible dash row sits at the glyph tile's bottom, not the
backdrop's.

**Verdict: C/other (glyph/label tile alpha), not A (750x200 tiling) and not B
(backdrop sampling uncropped to the glyph rect).**  Fix in the guest glyph/label
tile generation: do not write the dash rows into the 42/90-high tile (P1c/P1d/P1e
already ruled out trim/sanitize as clean fixes).  Host-side discarding the bottom
row would be a fallback only after proving it does not cut descenders or the
backdrop corners.

Limitation: this host_glwire (119,552 / 28f484a1) did not emit the
`label state texmatD` line (`label state` count 0), so texmatD is the one missing
observation point; the `u0 tex` sizes, `readrect`, `texmat` and the tile dump are
the substitute evidence.  Image: `/tmp/root-byref.apfs` 3207a35e (GL=1,
GLREMOTE=1, GLWIRE_LOCAL=1), system 8bf5f838.  The first 18:51 attempt used
root-local (no GLREMOTE, host 0 frames) and was discarded; the P0 recipe itself
was not changed.  Artifacts (not committed): `/host/iosvm/logs/s19o-p0-3/`.

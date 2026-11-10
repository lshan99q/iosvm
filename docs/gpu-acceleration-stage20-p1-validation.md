# Stage 20 P1 validation — `glyph_last_row=1` scrub

**Verdict: FAIL.** The 54 px dashes remain in the guest screenshot, and the
Weather text lost strokes: the scrub clears the 42-high glyph's last row (the
letter bottom), not the label tile's dash row.  Back to `glyph_last_row=0`.

## Boot

- 2026-11-10T08:00:56Z → 08:18:04Z; `end=ui ui=660s ui_bytes=311327`
  `guestframes=41 hostframes=742 MATCH=44 DIFF=698 panic=0 biggest=328445`.
- final lib `bd9dd073` in `/tmp/root-byref-final.apfs` (image `4f3d081f`);
  `XCMD="glwire_local=1 glyph_last_row=1"`, `HOST_ENV="GLWIRE_NO_PANEL=1"`;
  host `17090add`.

## Evidence

- **Guest screenshot** (`s20-p1-validation-shot.png`, 321 K): the Weather
  label shows missing strokes; the dashes under it are still present
  (`p1val-label-plus-band.png` / `p1val-label-lower40.png`).
- **Host tile dump** (`tile-f301/f311/f321-t97-178x90`): row 61 = **44** alpha px
  (max 224), row 62 = **50** px (max 202) — still dots.  Per code's note this is
  the recorder's raw pointer; the guest screenshot is the criterion, and it also
  still has the band.
- **diffdump** `f-n1 --tolerance 8`: **99.50% SAME** (missing 6,135 px / 0.50%,
  box 416,736–479,831 — the Weather strokes; extra 0).  ≥95% numerically.
- **check_panel**: cross `f-n1-panel` vs `f-picture` **100.00%**, "looks like a
  picture".
- **P0 three**: screenshot 328,445 B ≥300 K ✓; diff 99.50% ≥95% ✓; readable
  names — the P0 3/3 reference still ≥14, but this Weather label is regressed
  (missing strokes), so "Notes/Weather 不回归" fails.

## Next

1. Set `glyph_last_row=0` (the code's instruction) — the scrub row is wrong.
2. The fix should target the label tile's dash row (178x90 row 61/62), not the
   glyph's last row; the guest screenshot is the acceptance surface.

Artifacts: `/host/iosvm/logs/s20-p1-validation/`.

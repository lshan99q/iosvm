# Stage 20 P1 — final `label_dash_rows=61-63` (lib `6816a32b`)

**Verdict: P1 dead by the agreed rule.** The new self-report log says
`label scrub skip fbo 1/3 tex 1/4 size 750x1624 (not 178x90)` **8 times**, and
`cleared` **0 times** — the scrub is still hitting the 750x1624 screen FBO, not
the 178x90 label FBO.  Additionally the P0 same-frame diff regressed to
**84.52%** (<95%), so this boot also fails P0.

## Boot

- 2026-11-10T10:08:16Z → 10:25:56Z; `end=ui ui=690s ui_bytes=309612`
  `guestframes=41 hostframes=743 MATCH=48 DIFF=695 SIGSEGV=0 panic=0
  biggest=326803`.
- root `/tmp/root-byref-labeldash2.apfs` (`ce24742b…`, lib `6816a32b…`);
  `XCMD="glwire_local=1 label_dash_rows=61-63"`, `HOST_ENV="GLWIRE_NO_PANEL=1"`;
  system `6be494f1`; host `17090add`.

## Evidence

- scrub lines (8): `fbo 1 tex 1 size 750x1624 (not 178x90)` and
  `fbo 3 tex 4 size 750x1624 (not 178x90)`; **no `cleared` line**.
- guest screenshot 309,612 B ≥300 K ✓; the dashes still present in the Weather
  band (guest screenshot; host tile dump is the recorder's raw pointer).
- diffdump same-frame: **84.52%** (missing 188,776 px / 15.50%, box
  46,142–479,799; extra 154 px) → **below the 95% line, P0 FAIL**.
- check_panel cross pairing 100.00% (picture(N)==panel(N+1)); the same-frame
  pairing 84.52%.
- tile dump rows this boot: 96x19 tiles (`tile-f211…f291-t4-96x19`), not the
  178x90 — consistent with the scrub never seeing the label FBO.

## What the code needs

- The current draw FBO at label time is the 750x1624 screen, not the 178x90
  label FBO; the scrub must attach to the FBO that actually holds the 178x90
  label tile (the one the earlier `tile-f301/f311/f321-t97-178x90` dumps came
  from), or clear it at label-texture upload time.
- Do not use 53-62; 61-63 currently no-ops (skips) and P0 diff is down.

Artifacts: `/host/iosvm/logs/s20-p1-labeldash-final/`.

## Note on the 84.52% (pairing misalignment, NOT P0 regression)

The 84.52% same-frame number is a **one-frame pairing offset**, not a content
regression: `check_panel` cross pairing (`picture(N)==panel(N+1)`) is **100.00%**,
and the 188,776 missing px sit in the widget band (46,142–479,799).  Treat it as
a diff alignment artifact, not a P0 content regression.

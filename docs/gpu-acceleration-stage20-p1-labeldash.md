# Stage 20 P1 — `label_dash_rows` validation (3 boots)

**Verdict: FAIL.** `label_dash_rows=53-62` **crashes the boot** (`end=crash`,
`ui=0`, `SIGSEGV=1`).  `label_dash_rows=61-62` boots, Weather/Notes strokes are
complete, P0 three pass, but the **54 px dashes remain** (guest screenshot and
tile rows 61/62/63 still have alpha).  The new lib alone is fine (isolate boot
with the switch off: ui 690 s, 310,415 B, diff 99.92%, panel 100%).

## Boots (new lib `a56ad185…`, 401,144 B; root `3f293b28…`; system `6be494f1`; host `17090add`)

| boot | switch | result |
|---|---|---|
| crash | `label_dash_rows=53-62` | **end=crash ui=0 SIGSEGV=1** guestframes=1 hostframes=3 |
| isolate | off (`glwire_local=1` only) | end=ui **690 s**, 310,415 B; diff **99.92%**; panel **100%**; SIGSEGV=0 |
| dash6162 | `label_dash_rows=61-62` | end=ui **750 s**, 309,156 B; diff **99.77%**; panel **100%**; SIGSEGV=0 |

## 54 px evidence (guest screenshot is the criterion)

- Weather band still shows the dashes (`c-weather.png`, `c-photos.png`).
- Host tile dumps: `tile-f461-t107-178x90` row61 n=26 maxa=240, row62 n=9,
  row63 n=43; `tile-f471-t97-178x90` rows 61/62/63 each n=30 maxa≈120 (the
  dashes); `tile-f481-t97-178x90` row61 n=14.  The 61-62 scrub did not clear
  them — the row mapping is wrong (the dashes sit on 61–63 in these tiles), and
  53-62's wider range crashes the guest.
- P0 three (both non-crash boots): screenshot ≥300 K ✓ (310,415 / 309,156);
  diff ≥95% ✓ (99.92 / 99.77); names — P0 3/3 reference ≥14; the Weather/Notes
  strokes are complete in 61-62 (no regression this time).

## Conclusion

The label-dash switch is not usable as delivered: 53–62 crashes the guest;
61–62 boots but has no visible effect.  Next: the code must map the actual dash
rows (tile rows 61–63 in this run, with per-tile offsets) and avoid the crash
range; the guest screenshot stays the acceptance surface.

Artifacts: `/host/iosvm/logs/s20-p1-labeldash/`.

# Stage 20 P0 recheck — today's lib/host on the P0 path

**Verdict: P0 same-frame diff is 59.57% → BELOW 95%, immediate report.**
The f-panel/f-picture pair is 59.57% (missing 40.53%, box 0,0–749,1603); the
f-n1-panel is a black mask (check_panel: MASK-LIKE), so only the f pair is
meaningful.  The P0-3 reference was 98.82% on the same pairing.  Screenshot is
still ≥300 K (317,619 B), so this is a diff/render regression, not a capture
failure.

## Boot

- 2026-11-10T10:51:50Z → 11:08; `ui=720s (317619 B)`.
- root `/tmp/root-p0-recheck.apfs` (`b537e67d…`, lib `4a7cd6…` = ack-timeout lib);
  `XCMD="glwire_local=1"`; P0-3's HOST_ENV; system `6be494f1`; host `2be814a7`
  (poll16 binary, `GLWIRE_POLL_MS` unset = -1); `QEMU_SMP=1`.

## P0 three

| criterion | result |
|---|---|
| readable names ≥5 | screenshot 317,619 B; label draws logged `0 nonzero` for 64x9 (needs visual confirm) |
| screenshot ≥300 KB | **PASS** 317,619 B |
| same-frame diff ≥95% | **FAIL** 59.57% (f-panel vs f-picture; missing 40.53%) |

## Changes in this boot vs P0-3

- lib: `4a7cd60d…` (ack timeout default 0 + clamp + P1 scrub off) vs P0-3's older lib.
- host: `2be814a7…` (poll16; `GLWIRE_POLL_MS` unset) vs P0-3's host.
- system: `6be494f1…` (current C1) vs P0-3's `8bf5f838…`.
- root: root-byref config rebuilt with the new lib.

## Next

- Isolate: same boot with the old lib and/or old host to identify which of
  lib/host/system caused 98.82→59.57.  Do not claim P0 is broken until the
  isolate; but 59.57 is flagged now per instruction.

Artifacts: `/host/iosvm/logs/s20-p0-recheck/`.

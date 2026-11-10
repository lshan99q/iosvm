# Stage 20 frame-sync — cell3 baseline vs GLWIRE_SOCKBUF

**Verdict: candidate 1 (host-side socket buffer alone) is NOT established.**
The 120-frame host window fell from **1.07 to 0.46 fps**; the guest frames rose
(24→153) but the guest setsockopt failed: `hakios-remote: socket buffer 8388608 B
not applied (errno 55)`.  The code's two-sided condition is not met.

## Numbers (system `6be494f1`, cell3 root-w15, 120-frame host window)

| case | window | ui | host frames | fps | guest frames | socket |
|---|---|---|---|---|---|---|
| baseline: original root/host, buffer off, P1 off | 112.0 s | 640 s | 133→373 | **1.07** | 24 (15→39) | none |
| switch: final root/host + 8 MB, P1 off | 260.0 s | 660 s | 146→385 | **0.46** | 153 (20→173) | host applied; **guest errno 55** |

- host: `host_glwire: socket buffer 8388608 B (GLWIRE_SOCKBUF)`.
- guest: `hakios-remote: socket buffer 8388608 B not applied (errno 55)` —
  ENOBUFS on the guest's `setsockopt`, so only the host side was raised.
- Steady host intervals: baseline 0.74–0.80 s (~1.25 fps); switch 0.72–1.03 s
  (~1.0–1.4 fps); the per-frame replay is unchanged (29–35 ms); the window
  average is dominated by the early 494 s/96 s gaps in both.
- The guest frames did rise (0.21→0.59 fps over the window), but the host window
  did not; this is not the code's "host fps ≥2" success.

## Cell1 guardrail

P4 attempt3 cell1 (same QEMU_SMP=1/harness, `/tmp/root-local.apfs`, no recorder):
**120/120, 10.91 fps**, unchanged; the buffer switch is not applied to cell1.

## P0 pixels / names

The cell3 W1.5 screenshots (311,851 B baseline; switch shot in the archive) and
the P0 3/3 reference (v7 99.85 / v9 99.98 / p0-3 98.81; names ≥14; biggest
330,931/326,461/327,177 B; 0x502 0) are unchanged.  No P0 regression is claimed
from this frame-sync boot (cell3 is the remote path, not the P0 local run).

## Next

1. Fix the guest ENOBUFS (SO_RCVBUF/SO_SNDBUF, errno 55) before judging the
   buffer candidate; the host alone is not the experiment.
2. Do not stack noflush/frame-delay yet (per code's "无变化 → 候选1降级").

## Artifacts

`/host/iosvm/logs/s20-framesync/` (baseline/switch guest/host/drive logs, shots,
report, meta).

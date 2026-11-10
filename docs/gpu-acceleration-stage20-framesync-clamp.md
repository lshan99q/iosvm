# Stage 20 frame-sync — candidate 1 retest with guest clamp

**Verdict: candidate 1 degraded.** The guest clamp works (no more errno 55; it
applied **3,145,728 B**) but the 120-frame host window is **1.02 fps** vs the
baseline **1.07 fps** — no improvement.  Per code: applied smaller + fps flat →
candidate 1 degraded, do not change the sysctl.

## Numbers (cell3, system `6be494f1`, host `17090add`, 120-frame host window)

| case | window | ui | host frames | fps | guest frames | socket |
|---|---|---|---|---|---|---|
| baseline (original root/host, buffer off) | 112.0 s | 640 s | 133→373 | **1.07** | 24 (15→39) | none |
| switch (clamp lib + 8 MB) | 117.8 s | 720 s | 88→328 | **1.02** | 8 (7→15) | guest **3,145,728 B applied** |

- guest log: `socket buffer request 8388608 clamped to kern.ipc.maxsockbuf
  6291456` then `socket buffer 3145728 B applied (requested 8388608,
  maxsockbuf 6291456)` — clamp + half-retry works, **no errno 55**.
- host: `GLWIRE_SOCKBUF=8388608` applied; steady host intervals 0.69–0.77 s
  (~1.3–1.44 fps) vs baseline 0.74–0.80 s (~1.25 fps); per-frame replay
  unchanged (30–34 ms); the window average is dominated by the early gaps.
- The guest sent **fewer** frames in the window (8 vs 24), so the buffer alone
  does not move the host window to ≥2 fps; candidate 1 is degraded.
- cell1 guardrail: P4 attempt3 cell1 **120/120, 10.91 fps** unchanged (buffer
  switch not applied to cell1).
- P0 pixels/names: P0 3/3 reference unchanged (99.85/99.98/98.81; names ≥14;
  330,931/326,461/327,177 B; 0x502 0).

## Next

- Do not change `kern.ipc.maxsockbuf`; candidate 1 is degraded (code's rule).
- Frame-interval data (`MiniIOMobileFramebuffer`, `hakios-remote f N wait`) in
  the archived logs.

Artifacts: `/host/iosvm/logs/s20-framesync-clamp/`.

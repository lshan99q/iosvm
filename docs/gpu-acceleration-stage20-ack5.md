# Stage 20 ack5 — new lib 2f8fbc + REMOTE_ACK_ARM_AFTER=5

**Verdict: the ack sequence works and the guest rate lifts to ~1.3 fps, but the
UI still never produces a screenshot in this run.** So this fixes ack-side
starvation; the UI/present side remains.

## Recipe

- root `/tmp/root-w15-ack5.apfs` `0528a8d1…` (785bcbac config + lib `2f8fbc…`);
- system `8bf5f838`; host `b1e87d…` poll16;
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_ack_ms=50"`.

## Counters

| item | value |
|---|---|
| ack lines | 6 (`applied`, `armed after 5 complete frames`, `dropped 1/3…3/3`, `disabled after 3 drops`) |
| frame dropped | 3/3 |
| ack timeout disabled | 1 (`using the old infinite wait`) |
| guest `MiniIOMobileFramebuffer` | 5–8 frames / 5–6 s (~1.0–1.4 fps) |
| host frames | 1254; steady 1.46–1.60 fps |
| UI shot | none (30 min) |

## A/B comparison

| case | ack | guest | host | UI |
|---|---|---|---|---|
| A (steady lib) | 0 | 2/110 s (starved) | 371 (1.23 fps) | 600 s ✓ |
| B (ack5) | 50 | **5–8 / 5–6 s (~1.3 fps)** | 1254 (1.5 fps) | **none** |

- The arm-after-5 works as designed (first 5 frames complete, then arm, 3 drops,
  disabled); ack timeout no longer kills the first frame.
- Remaining: UI/present side, not ack.

Artifacts: `/host/iosvm/logs/s20-ack5/`.

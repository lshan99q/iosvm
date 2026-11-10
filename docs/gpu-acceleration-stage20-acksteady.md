# Stage 20 acksteady — A (ack=0) vs B (ack=50) on new lib 3160eed

**A passes; B still fails to reach UI.** With the same root/lib, A
(`glwire_ack_ms=0`) gives UI 600 s / 1.23 fps / ack lines 0; B (`glwire_ack_ms=50`)
gets only 3 host frames, no UI, guest 2–4 frames per 5–28 s, and 5 ack lines
(armed after first frame, dropped 1/3 and 2/3; no 3/3, no disabled).

## Recipe

- root `/tmp/root-w15-acksteady.apfs` `6b759e22…` (785bcbac config + new lib
  `3160eed5…`); system `8bf5f838`; host `b1e87d…` poll16;
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16"`;
- A: `glwire_ack_ms=0`; B: `glwire_ack_ms=50`.

## A vs B

| case | XCMD | UI | host frames | guest | ack lines |
|---|---|---|---|---|---|
| A | glwire_ack_ms=0 | **600 s** | 371 (132→371, 1.23 fps) | 10→12 | **0** |
| B | glwire_ack_ms=50 | **never** (13:37+) | **3** | 2/19 s, 2/28 s | 5 (armed; dropped 1/3, 2/3) |

- ack sequence: `ack timeout 50 ms applied after the first frame` →
  `ack timeout armed after the first complete frame` → `frame dropped 1/3` →
  `frame dropped 2/3`; no `3/3`, no `disabled`.
- First-minute logs: `logs/s20-acksteady50/first-minute-guest.log` (1200 lines),
  `first-minute-host.log` (200 lines).
- Conclusion: new lib's default path is fine (A); the ack=50 path still breaks
  the cell3 boot (B) — returned to code, no blind changes.

Artifacts: `logs/s20-acksteady0/`, `logs/s20-acksteady50/`.

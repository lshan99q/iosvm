# Stage 20 ack785 — same 785bcbac config + ack lib 4a7cd6

**Verdict: UI never comes up with lib 4a7cd6 → ack comparison still invalid.**
The isolate (same root config, original lib, poll16, no ack) boots to UI at
600 s; swapping only the lib to `4a7cd60d…` (ack timeout + clamp + P1 scrub)
gives host 0 frames, no screenshot, ack 2, guest 4/461 s — the lib itself
regresses the boot, not the ack switch.

## Recipe

- root `/tmp/root-w15-ack3.apfs` `f26a5dfa…` (785bcbac config + lib `4a7cd6`);
- host poll16; `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_ack_ms=50"`;
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16"`.

## Counters (10:45 boot, UI never)

| item | value |
|---|---|
| ack timeout | **2** |
| guest `MiniIOMobileFramebuffer` | 4/461 s, 2/65 s |
| host frames | **0** |
| poll idle | 76 |
| UI shot | none |

## Contrast with the isolate (same config, original lib)

| boot | lib | UI | host frames | guest frames |
|---|---|---|---|---|
| isolate 785 | original | **600 s ✓** | 366 | 103 (14→117) |
| ack785 | 4a7cd6 | **never** | 0 | 4/461 s |

- So the ack test cannot be judged until lib 4a7cd6's boot regression is fixed;
  the ack switch itself is not implicated.
- The earlier ack2 root (6378fa12) had the same lib — same UI failure.

Artifacts: `/host/iosvm/logs/s20-ack785/`.

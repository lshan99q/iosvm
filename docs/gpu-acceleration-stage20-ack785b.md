# Stage 20 ack785b — 8bf5f838 + 785bcbac config + lib 4a7cd6 + ack50

**Verdict: UI still never comes up on 8bf5f838 → the lib 4a7cd6 boot regression
is confirmed independent of the system; ack remains untestable.**

## Counters (11:43 boot, UI never)

| item | value |
|---|---|
| ack timeout | 2 |
| guest `MiniIOMobileFramebuffer` | 2/63 s, 2/26 s, 2/20 s |
| host frames | 0 |
| poll idle | 79 |
| UI shot | none |

## Contrast

| boot | system | lib | UI | host frames | guest |
|---|---|---|---|---|---|
| isolate 785 | 8bf | original | **600 s ✓** | 366 | 103 (14→117) |
| ack785 (6be) | 6be | 4a7cd6 | never | 0 | 4/461 s |
| ack785b (8bf) | **8bf** | 4a7cd6 | **never** | 0 | 2/63 s |

- 8bf is confirmed on disk; the lib 4a7cd6 alone breaks the boot on both
  systems. ack switch not implicated; do not use lib 4a7cd6 for cell3 until
  fixed.

Artifacts: `/host/iosvm/logs/s20-ack785b/`.

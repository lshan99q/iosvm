# Stage 20 ack timeout 50 ms — candidate 2 (cell3, no P0)

**Verdict: inconclusive / does not show the ack wait as the bottleneck.**
The ack timeout fired only twice; the guest stayed at ~3 display frames per
~48–70 s; the host received **0 frames** and logged 89 `poll idle 1.01 s` lines;
the UI never came up (boot ended in extension check-in timeouts).

## Recipe

- root `/tmp/root-w15-ack2.apfs` `6378fa12…` (cell3 GL=1 GLREMOTE=1 CONFIGD=1;
  lib `4a7cd60d…` with `glwire_ack_ms` default 0);
- host poll16 (`2be814a7…`, `GLWIRE_POLL_MS=16`);
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_ack_ms=50"`;
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16"`;
- `WINDOW_FRAMES=120 WINDOW_AFTER=60 ...`; boot 12:19→12:49 (30 min UI timeout).

## Counters

| item | value |
|---|---|
| ack timeout lines | **2** (`ack timeout after 50 ms, frame dropped (errno 35)`) |
| guest `MiniIOMobileFramebuffer` | 4/6 s, 3/70 s, 3/48 s (≈0.05–0.06 fps) |
| host `host_glwire: frame` | **0** |
| host `poll idle` | **89** lines (`1.01 s`, timeout 16 ms, no guest data) |
| UI | never (extension check-in timeouts; no shot) |

- The 0 host frames + no UI makes this boot not a clean ack comparison; the ack
  count is far below "every frame times out", so the ack wait is not proven to
  be the limiter.  A re-run needs the UI up, or the ack test at the pre-UI phase
  cannot measure fps.

Artifacts: `/host/iosvm/logs/s20-ack50b/`.

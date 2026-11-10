# Stage 20 isolate 785bcbac + poll16 (no ack) — UI baseline for ack rerun

**Verdict: UI comes up fine (600 s); guest frames 14→117 (103 frames in the
window); host 126→366, 0.79 fps; poll-idle 1,101 lines.** This is the known-good
cell3 root from P4 attempt2; the earlier ack2 root (6378fa12) was the problem,
not the ack switch.

## Recipe

- root `/tmp/root-w15.apfs` **785bcbac82c9a117684b2d40b7375d48** (P4 attempt2
  cell3; original lib);
- host `b1e87d…` poll16 (`GLWIRE_POLL_MS=16`);
- `XCMD="glwire_local=0 glframes=1 touch_lat=1"` — **no ack switch**;
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16"`;
- 120-frame host window.

## Numbers

- `drive: ui=600s frame-window 120 frames in 152.76s -> 0.79 fps (host 126→366,
  guest 14→117)`.
- UI shot 305 K; host frames 366; poll idle 1,101 lines.
- Guest frames 103 in the window (~0.67 fps); host window 0.79 fps.
- Baselines: original cell3 (no poll) 1.07 fps (host 133→373, guest 24);
  poll16 earlier 1.11 (host 120→359, guest 2 — that boot's guest was starved).

Artifacts: `/host/iosvm/logs/s20-iso785/`.

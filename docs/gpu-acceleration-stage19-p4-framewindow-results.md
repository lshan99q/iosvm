# Stage 19 P4 — 120-frame window result (attempt3)

**Date:** 2026-11-09 14:56–16:45Z (the owner stopped the run after cell3).
**Verdict:** **pure-local is the only usable number (10.91 fps); by-reference and W1.5
stall the guest sender under TCG single-core + softpipe (0.003–0.1 fps).**  `host 0x502`
is 0 in every cell that ran; cell4 was not run by the owner's verdict, not missed.

## Table

| cell | ui | frames | fps | span | shot B | host 0x502 |
|---|---|---|---|---|---|---|
| cell1-pure | 740s | 120 | 10.91 | 11.00s | 439411 | 0 |
| cell2-byref | 640s | 0/120 TIMEOUT | - | - | 439138 | 0 |
| cell3-w15 | 660s | 11/120 TIMEOUT | - | - | 311851 | 0 |
| cell4-w15w3 | NOT RUN | - | - | - | - | - (attempt2: 0) |

The window is 120 frames, not 20 s: `WINDOW_FRAMES=120 WINDOW_SPAN=120`, starting at
frame 120 after the interface; cell1 has no recorder and uses `WINDOW_CLOCK=guest`, the
others use `WINDOW_CLOCK=host`.  A row that cannot reach 120 in 1800 s says
`window frames/120 TIMEOUT`.

## Decisive evidence

- **cell3 guest sender:** `MiniIOMobileFramebuffer: 2 frames in 772366 ms (0 a second),
  85869 us each in the driver ...` — **0.0026 fps**; the last host frame is `frame 250
  ... interval 772.23 s (0.00 fps)`.  In 1800 s the host went 121 -> 250 (window 11/120);
  the guest went 12 -> 96.
- **cell2 by-reference:** host 104 -> 183 in 1800 s (**0.044 fps**), guest 12 -> 48
  (**0.02 fps**); the window never reached frame 120 (0/120).
- **cell1 pure local:** 120 frames in 11.00 s = **10.91 fps** (guest clock; host 0 frames
  by design, `REQUIRE_HOST_FRAMES=0`).
- **The host replay is not the bottleneck:** `host_glwire: frame` lines are 29.6–59.0 ms
  per frame (16–33 fps capability), `panel/picture` MATCH (cell2) or DIFF by mode.  The
  stall is in the guest's recording/sending, with the 2 Hz swipe loop adding 13,281 IOHID
  events in cell3.

## Why cell4 is NOT RUN

After cell2 (0/120 in 1800 s) and cell3 (11/120, 0.0026 fps guest) the remote paths are
already proven unusable; the owner stopped attempt3 and ruled out the remaining ~50 min.
This is a verdict, not a missing row.  If it is ever revisited, use a single-cell
verification with `MAX_SECS=1800`, not another four-cell sweep.

## Environment / harness

- Images: cell1 `f39b9105…` (no recorder), cell2 `3207a35e…` (`glwire_local=1`),
  cell3 `785bcbac…` (`glremote_on=1`); cell3=cell4 image (cell4 not run);
  `system.apfs 8bf5f838…` untouched.
- `QEMU_SMP=1`, `WINDOW_AFTER=60`, `MAX_SECS=3000`, `MINSHOT=200000`, `NOUI_BY=1800`,
  `WINDOW_TIMEOUT=1800`; `QEMU_EFI/QEMU_VARS` as in the P4 runbook.
- The run started after repo `e41a47a` (frame-window support) but used a dec63f4 copy with
  three operational patches: `WINDOW_TIMEOUT`, a 3600-iteration loop cap, and
  `kill_tree "$hpid"` (the dec63f4 hpid was the host subshell; its `host_glwire` child
  survived and made the next cell's preflight refuse — fixed in repo `f30c8d2`).
- `shot B` is the UI-detection first frame, not the window's last frame.  cell3's
  right-bottom missing corner is the known first-frame race (frame 128 = 82,080
  records/0 image refs, frame 129 = 236,026/48 refs); the late-frame verification boot
  was cancelled by the stop decision, so do not read it as a data gap.

Artifacts (not committed): `/host/iosvm/logs/s19o-p4-cells/attempt3-framewindow/`
(`four-cell-framewindow.md/.csv`, three cells' logs/drives/shots, `report.md`, `harness/`).

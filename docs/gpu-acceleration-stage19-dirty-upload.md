# DIRTY_UPLOAD: fixed 64x64 tile delta from the host picture

## Switch

- guest boot-arg **`glwire_dirty_upload=1`** (or env `GLWIRE_DIRTY_UPLOAD`), default **0/off**.
- host needs no env: the guest puts a 10th field in the R line; host compares the current picture
  with the previous one in 64x64 tiles and replies `D <tiles> <bytes>` followed by
  `<x y w h>` + packed tile rows.
- guest patches its full `remote_frame`; `frame_end` still copies the full picture (the dirty path
  never does a partial-stride copy). First frame / size change sends all tiles = full picture.

## Logs

- host every frame: `host_glwire: dirty upload n/312 tiles, X of 4872000 B`
- **fallback full R was tried and reverted**: scroll2 with `58d5b876` stopped at frame 113
  (guest 4/486 s), worse than the tile path `c9f357c8` (639 frames, guest 0.72 fps).  Do not send
  one full R from the dirty path; keep the 64x64 D path (host c9f357c8).
- guest every dirty frame: `hakios-remote: dirty n tiles, X B`
- default: no `dirty` line; 10th request field is 0, host sends the old `R <frame_bytes>` and guest
  takes the old full-picture path.

## Validation

- static UI: after first full frame, n should be small (clock/widget changes only); scroll: n rises.
- target: guest ≥5 fps and screenshot content has no stale tiles; host must keep emitting frames.
- default proof: `glwire_dirty_upload=0`, guest log has no `dirty` line, host log has no
  `dirty upload`; e2e default PASS (`painted 16/16`); host 4x4 two-frame test: first `D 1 64`,
  second `D 0 0`.
- lib **401,864 B / md5 35d51b50981d4c8474aae449c16b0699**; host **119,792 B / md5
  c9f357c8f7fd529d810ce8fc41c864db** (fallback reverted); patch `logs/s19o-p1-dirty.patch`.
- dirty1 result: static 0/0, 1/16KB, 3/49KB; scroll 48/768KB, 310/4.8MB, 134/2MB, 298/4.65MB.
  The scroll window is inherently full-screen dirty; before fallback the host sent 310 tile
  headers + ~19,840 row sends and guest 0.72 fps, worse than full 1.23.  Fallback keeps scroll at
  full-frame cadence; it cannot make a full-screen-changing scroll ≥5 fps.  Δstatic is the win.

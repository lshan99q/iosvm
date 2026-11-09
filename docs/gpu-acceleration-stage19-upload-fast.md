# Full-frame upload: no per-pixel loop, but one extra 4.9 MB copy

## Finding

- The guest full-frame path is **row memcpy**, not per-pixel: `eagl.c:1470` copies
  `copy_used = width*4` bytes per row from `remote_frame` into the IOSurface; the socket receive is
  `eagl_remote.c:764` with a large `recv` per call.
- The host `host_glwire` `write_picture_rows()` is row memcpy; `hash_picture()` is per-pixel but runs
  on the native host, not in the guest.
- So 4.9 MB / ~740 ms is the **socket + intermediate remote_frame copy**, not a guest per-pixel
  conversion.  The fast path receives the host's BGRA rows **directly into the IOSurface**, removing
  the intermediate 4.9 MB copy and the second row loop.

## Switch and log

- guest boot-arg **`glwire_upload_fast=1`** (or env `GLWIRE_UPLOAD_FAST`), default **0/off**.
- When the reply is a full `R <frame_bytes>` and the target surface stride is known, receive one
  row per `recv` into the surface; `eagl.c` skips its copy because flush returns 3.
- log: `hakios-remote: upload 4872000 B in <ms> ms (<MB/s>)`.
- default: no target is set, flush returns 1 and the old row-copy path runs byte-for-byte; dirty D
  and scale partial paths are untouched.

## Test

`XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`
with NO_PANEL+NO_INPLACE+POLL16.  Target: `upload` <150ms; if still ~700ms, the remaining cost is
QEMU TCP, so next is RGB565/host-side conversion, not row loops.

Lib **402,248 B / md5 e06c91977a517fc70e9f8f244f95c3f9**; patch 147 lines.  Host stays
c9f357c8 (dirty, no fallback).

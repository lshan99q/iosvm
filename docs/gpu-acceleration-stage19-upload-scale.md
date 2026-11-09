# Payload experiment: return only a quarter of the picture

## Where the 4.9 MB goes

In cell3 no-inplace, the guest does **not** upload the panel; `eagl_remote_flush()` requests a frame
and the host returns the picture: `R 750 1624 4872000` bytes, received at
`src/eagl_remote.c:764` and copied into the surface at `src/eagl.c:1458`.  That is the 4.9 MB per
frame; the host replay itself is only 29–35 ms.

## Switch (default 1 = unchanged)

- host env **`GLWIRE_UPLOAD_SCALE=2`** (or 4): pack the top-left `width/scale x height/scale`
  pixels into a contiguous buffer and send only those bytes; reply `R <w*h*4>`.
- guest boot-arg **`glwire_upload_scale=2`** (or env `GLWIRE_UPLOAD_SCALE`): expect the quarter,
  receive it, and `frame_end` copies only the quarter rows/columns; the rest of the surface is left
  as it was (experimental, so the picture is partial by design).
- scale 1: host sends `frame_bytes`, guest expects `width*height*4`, copies every row — the old
  byte count and loop.  Startup gains only the `upload scale 1` text.

## Verification

- host `GLWIRE_UPLOAD_SCALE=2` + guest `XCMD="... glwire_upload_scale=2 glwire_ack_ms=0"`:
  - guest `hakios-remote: upload scale 2 (1/4 of the picture)`
  - host `host_glwire: upload scale 2 sends 1218000 of 4872000 picture B`
  - guest `hakios-remote: f ... wait` should drop from ~830 ms toward <400 ms; target ≥2 fps.
- default e2e PASS (painted 16/16); host quarter test `R 16` for a 4x4 frame.
- lib **401,776 B / md5 27f627afff539eb537c76aa4505f3d7c**; host **119,728 B / md5
  fd6667ad7f11d265fae93466e6f18220**; patch `logs/s19o-p1-uploadscale.patch` (108 lines).

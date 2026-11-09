# RLE does not depend on 565: the 51 KB ratio is a black input picture

## Answer: 3 (independent)

Host:
- `host_glwire.c:4258` `rle_wanted && !rgb565_wanted` packs the **BGRA** picture in 4-byte
  pixel runs (`memcpy(packed_rle + out, picture + i*4, 4)`, `:4280`, `:4301`).
- `host_glwire.c:4246` only skips RLE when the readback has no picture; it does not require 565.

Guest:
- `eagl_remote.c:1106` handles `Z`, and `:1119` chooses `pixel_bytes = 4` when
  `raw == width*height*4`; a 565 payload is the separate `raw == width*height*2` branch.
- `:1208` logs `upload-rle bgra` for the 4-byte path; no 565 conversion is invoked.

So **RLE does not silently treat BGRA as 565**.  The `51,202 B (1%)` is exactly what a
**uniform black** 4.87 MB picture packs to; the host log from rle3/rle6 shows
`read-back ... viewport 0,0 0x0` and both 750x1624 panel uploads black.  With
`glwire_local=0` the guest does not draw locally; the surface is black until the host supplies the
first picture.  The RLE runs never got that first picture (host empty readback), not a format bug.

5653 worked because its host frame had a panel-sized draw/texture; the RLE runs' readback was 0x0.
`565=1 + rle=1` is a valid combination (host packs the 2-byte form; guest chooses the 2-byte
decoder); it should be tested before adding any artificial "rle needs 565" guard.

## Option channel

`eagl_remote.c:414` is the init self-report; `/tmp/glwire_opts` (or
`/redwood/etc/glwire_opts`) is read as `key=value` lines and overrides the environment.  The guest
side is done.  Runtime host writes to guest `/tmp` are not possible with the current QEMU (no 9p);
the file must be preloaded into the root or a host→guest handshake must be added.

Sample:
```
hakios-remote: options raw="debug=0x8 ... glwire_local=0 glframes=1 touch_lat=1 glwire_upload_rle=1 glwire_upload_565=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0" parsed local=1 fast=1 565=0 rle=1 dirty=0 ack=0 scale=1
```

# BGRA RLE: compress the picture and avoid the RGB565 conversion

## Why this instead of a 565 worker

`s20-5653`: 2.44 MB RGB565 takes 532–596 ms at 4.1–4.7 MB/s; 565 expansion runs in the render
thread and guest present fell to 0.08–0.5 fps.  Moving only the conversion to a worker still leaves
~550 ms of TCP and still needs the expansion.  Compressing the **BGRA** picture instead removes the
565 conversion entirely: the guest just unpacks BGRA runs and memcpy's rows to the surface.

Actual screenshot (1125x2436): BGRA 10.96 MB -> RLE **810 KB (13.5x)**; the 4.87 MB panel frame is
therefore ~360 KB, which at 4.5 MB/s is ~80 ms.  Host packing is native and ~1 ms; guest unpack is a
byte-run loop, no per-pixel channel work.

## Switch (default 0)

- host env **`GLWIRE_UPLOAD_RLE=1`**, guest boot-arg **`glwire_upload_rle=1`** (env same).
- RLE format: 16-bit/32-bit pixel runs; control `0x80|(run-3)` + one pixel for repeats, or
  `literal-1` + literal pixels.  Same format for BGRA (pixel=4 B) and RGB565 (pixel=2 B).
- host replies `Z <raw> <packed>` + packed bytes; guest detects raw==4*w*h (BGRA) or 2*w*h (565).
- BGRA path copies rows straight to the IOSurface/remote_frame; **no 565 expansion**.
- If `GLWIRE_UPLOAD_565=1` is also set, the host packs the 565 form; default RLE alone uses BGRA.
- Default 0: old BGRA full R; e2e default PASS; host 4x4 BGRA RLE `Z 64 5` and 565 RLE `Z 32 3`
  tests pass.

## Logs / acceptance

- host `bgra rle raw 4872000 -> packed N B (Z%), pack X ms`
- guest `upload-rle bgra raw 4872000 -> packed N B (Z%), unpack X ms`
- target packed **≤800 KB**, host pack **<50 ms**, guest back to 10–14 fps, host frames grow,
  screenshot colors exact for BGRA (no 565 quantization; 565 optional).
- RLE3: host logs `read-back ... viewport 0,0 0x0` for most late frames: no panel draw in that
  frame, so the host was packing a black picture and the guest kept its only 3 real panels.
  RLE now replies `D 0 0` (`rle skip empty readback`) when the viewport is 0x0; the guest keeps
  the surface (`sent==3` for RLE+fast) instead of overwriting it with black.
- RLE3: unpack 84–105 ms (target met).  The host actually logged **106 frames** (the "3 frames"
  report counted only the sparse frame lines); guest `upload-rle` also 106.  The guest display
  showed only 3 because the fast/RLE path wrote the first panel surface and the display alternates
  with the second panel surface; `eagl.c` now copies the fast frame to the second surface before
  the swap.  If display still stalls, the next issue is present/second-surface, not unpack.
- If guest still starves after packed is small and the second surface is filled, the guest unpack
  loop is the next worker candidate; log `unpack ms` decides.

Lib **419,072 B / md5 ff3f63d5f25b2fab83803bbc6d3f448e**; host **128,144 B / md5
3562ed74b2d10cbacebc4b9914989e8e**.  RLE2: packed **51,202 B (1% of 4.87 MB, ~95x)**, host pack 1 ms, but guest unpack was **396–451 ms** because the repeat path did a 4-byte `memcpy` per pixel.  The repeat path now uses `memset` for 0/0xffffffff and a 32-bit fill otherwise; literals stay one `memcpy`.  If unpack is still >150 ms, the next step is a worker, not another RLE tweak.

Options observability: init now logs
`options raw="<kern.bootargs>" parsed local=... fast=... 565=... rle=... dirty=... ack=... scale=...`;
`/tmp/glwire_opts` (or `/redwood/etc/glwire_opts`) is read at init as `key=value` lines and overrides
the environment.  `strings libhakios.gl.dylib | grep upload-rle` is part of the build check.

First RLE run (565+rle) exposed a **request format-string bug**: the format had 10 specifiers while
the argument list had 12, so `remote_upload_565()`/`remote_upload_rle()` were dropped and host saw
rgb565=0/rle=0.  The format now has 12 `%d` fields; guest logs `upload565=1` and `upload-rle=1`.

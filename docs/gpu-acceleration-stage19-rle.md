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
- If guest still starves after packed is small, the guest unpack loop is the next worker candidate;
  log `unpack ms` decides.

Lib **418,872 B / md5 974a0e52c05e33aee1b19edfab531775**; host **124,000 B / md5
d80f7ca34e49216c2d31fce7b7e8d169**; patch `logs/s19o-p1-rle.patch` (149 lines).

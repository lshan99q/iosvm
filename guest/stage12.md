# The guest-side additions of stage 12

## `surfprobe.c` — what a surface nobody binds holds (P1'''''')

The kernel's own log says SpringBoard makes a `312x202 'LA08'` overlay (the text) and a `184x124
'BGRA'` surface (a widget snapshot), and the render server's texture binds name neither. Whether
they hold pixels at all decides between "the text was never drawn" and "it was drawn and never
composited". Two copies of one sampler answer it from both sides:

- **kernel**: the block in `patches/0017-stage12-surface-sample.patch`, in
  `MiniIOMobileFramebuffer.cpp` (the presenter samples the named ids every five seconds);
- **user space**: `surfprobe.c`, to be pasted into `userland/src/inject_sb.c` and started from the
  constructor (`start_surface_sampler()`), which looks the surfaces up with `IOSurfaceLookup` and
  hashes them the same way. `build.sh`'s `libhakios.sb.dylib` link line needs `-framework IOSurface`.

Both are gated by `surfsample=<id>` / `surfsample_b=<id>`; without those boot arguments nothing runs.

## What it found (s12surf, 2026-11-05 18:12–18:33)

```
MiniIOSurface: SpringBoard[10] created surface 3: 184x124 'BGRA', 736 bytes per row
MiniIOSurface: SpringBoard[10] created surface 4: 312x202 'LA08', 640 bytes per row
MiniIOSurface: surfsample 4 312x202 'LA08' 640 bytes a row, rows b9fee455 549649bc c1bd17f0 c1bd17f0 b9fee455, 45412 of 126048 sampled bytes not zero
surfprobe: id=4 312x202 bpr 640 bpe 2 rows … 45412 of 126048 sampled bytes not zero (pid 10)
MiniIOSurface: surfsample 3 184x124 'BGRA' 736 bytes a row, rows … 79425 of 91264 sampled bytes not zero
```

The glyph overlay is **36.0 % non-zero**, the widget surface **87.0 %** — the pixels are there. In the
same boot only backboardd (pid 16) ever logs `end of frame`/`flush` (1,001 / 999), and neither
surface is in the frame's 28 images or in the 26 `-texImageIOSurface:` lines. The missing step is
the client→render-server handoff, not the drawing.

Two sampler bugs worth remembering: the kernel version first required `width * 4` bytes a row, which
a 2-bytes-a-pixel `LA08` can never satisfy (it printed "no pixels to read" for the glyph surface);
and a sparse sample (every 16th row, every 64th byte) read 36 % content as "2 of 130" — a full scan
is what makes the number mean anything.

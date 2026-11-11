# P1 final (frozen): the dash lives in the native legacy GL_ALPHA 178x90 tile

## 1. True source chain

```
guest CA/UIKit
  -> glTexImage2D(GL_ALPHA, 178x90)            (the label tile; rows 61-63 carry the dash)
  -> host host_legacy_teximage()               host_glwire.c:387
  -> glTexImage2D native upload (no error)     host_glwire.c:436
     "glwire: legacy texture 178x90 internal 0x1906 format 0x1906: taken natively"
  -> later program-13 draw samples the 178x90 texture and composites it into the panel
```

It is **not** a 750x200 label FBO cut by `glCopyTexSubImage2D`, and not a
`glCopyTexSubImage2D` destination at all: both P1 map runs had `glCopyTexSubImage2D`
mapping/count **0**, while the host log had the legacy 178x90 native upload.

## 2. Why the three scrub points missed

| # | Point | Evidence |
|---|---|---|
| 1 | guest `glyph_last_row=1` (42-high glyph's last row) | cleared the glyph stroke bottom, not the 90-high tile; Weather lost strokes, the 54px dash remained (`logs/s20-p1-validation/report.md`). |
| 2 | guest `label_dash_rows=61-62` / `61-63` | scrub ran while the current FBO was the 750x1624 screen, not the 178x90 tile: `skip ... fbo 1/3 tex 1/4 size 750x1624 (not 178x90)` x8, `cleared 0` (`logs/s20-p1-labeldash-final/`). |
| 3 | host `disp.inc:66 glCopyTexSubImage2D` (4cc74d3) | two boot maps printed no `tile rows`/`cleared`: the 178x90 tile never comes through that copy. Host only logged `legacy texture 178x90 ... taken natively` (`logs/s20-p1-map/`). |

The correct point is the **host legacy alpha upload**, not one of the three.

## 3. If reopened later

Function: `host_glwire.c:387 host_legacy_teximage()`.
Minimal change: before the native `glTexImage2D`, copy the 178x90 alpha upload, zero rows
61-63, upload the copy (or set alpha rows to 0). Cost: one 16 KB copy + 534-byte memset per
178x90 tile upload; no GPU readback, no extra frame. The switch already exists
(`GLWIRE_LABEL_ROW_SCRUB=61-63`, default off) and logs
`glwire: legacy 178x90 scrub rows 61-63 before native alpha upload`; **not booted/verified**.

Even so, P1 is a cosmetic dash under labels; the three missed mechanisms already consumed the
budget. **P1 is frozen**; no further code or experiments this phase.

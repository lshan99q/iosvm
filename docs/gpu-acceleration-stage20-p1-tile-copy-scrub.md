# P1 targeted fix: the host glCopyTexSubImage2D that fills the 178x90 label tile

The host call that cuts the 750x200 label layer into the 178x90 tile is
`tools/glwire/dispatch.inc:66` (`glCopyTexSubImage2D`, opcode 8).  It copies the
source framebuffer rectangle (`a4,a5,a6,a7`) into the tile texture at (`a2,a3`).

Added:
- when the destination texture is 178x90, print the real row mapping:
  `glwire: tile rows 61-63 <- layer rows X-Y (copy sx,sy w,h -> dx,dy)`
- `GLWIRE_LABEL_ROW_SCRUB=61-63` (default off): after the copy and before the tile is
  composited, clear tile rows 61-63 with `glTexSubImage2D` (RGBA or ALPHA, matching the
  tile internal format).  Log `glwire: cleared 178x90 tile rows 61-63 (format ...)`.

Expected acceptance: guest screenshot has no 54px dash under the label, Weather/Notes
strokes intact; host log has the mapping line and, with the switch, the cleared line.
Default off: only the mapping line is new; copy pixels unchanged.

Host **132,720 B / md5 85794fffd3c18a4530343c524ae55135** (includes the 78c135 RLE
gate and this P1 block); guest lib unchanged 3a42016b.

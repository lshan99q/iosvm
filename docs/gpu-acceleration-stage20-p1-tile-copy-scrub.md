# P1 targeted fix: the host glCopyTexSubImage2D that fills the 178x90 label tile

**Correction from the two P1 map runs (mapping 0 both times):** the 178x90 tile is not filled by
`glCopyTexSubImage2D`.  Host log only has
`glwire: legacy texture 178x90 internal 0x1906 format 0x1906: taken natively`.
The real path is `host_glwire.c:387 host_legacy_teximage()` — a guest `glTexImage2D` of a
178x90 **GL_ALPHA** tile, uploaded natively (log at `:436`).  P1 is judged dead per the two-boot
budget; this correction is for the record.

The earlier `glCopyTexSubImage2D` probe (`dispatch.inc:66`) stays default-off and never fires for
this tile.  A correct scrub now exists in `host_legacy_teximage()`: with
`GLWIRE_LABEL_ROW_SCRUB=61-63` it copies the alpha upload, zeros rows 61-63, and logs
`glwire: legacy 178x90 scrub rows 61-63 before native alpha upload`; default off, pixels unchanged.

Host **132,720 B / md5 6aa395ce7e064eaf4c3d624cc855e59e**.

Added:
- when the destination texture is 178x90, print the real row mapping:
  `glwire: tile rows 61-63 <- layer rows X-Y (copy sx,sy w,h -> dx,dy)`
- `GLWIRE_LABEL_ROW_SCRUB=61-63` (default off): after the copy and before the tile is
  composited, clear tile rows 61-63 with `glTexSubImage2D` (RGBA or ALPHA, matching the
  tile internal format).  Log `glwire: cleared 178x90 tile rows 61-63 (format ...)`.

Expected acceptance (if the lead reopens P1): guest screenshot has no 54px dash under the label,
Weather/Notes strokes intact; host log has the legacy scrub line.  P1 remains dead; no new boot from
this agent.

Guest lib unchanged 3a42016b.

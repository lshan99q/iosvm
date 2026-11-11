# RLE is dead; 565-only is the performance line

Final run (rle14): image1 texture was selected (`read-back fallback image1 texture 1`) but the input
assertion reported **0.00% nonblack** — the image1 texture itself is black, and segments2/panel were
already black.  Host picture sources are exhausted: FBO, panel, image1 texture, segments2 all black
for this `glwire_local=0` + RLE/fast path.

Decision: **RLE is not usable; do not run it.**  The performance line closes on **565-only**
(`GLWIRE_UPLOAD_565=1` / `glwire_upload_565=1`), which is proven: 2,436,000 B, 329 host frames,
normal UI.  RLE code stays opt-in/default-off for the record; no further RLE experiments.

Default-path safety: the RLE-only fallbacks (image1 texture, panel copy, segments2, black-input
assert) are now gated by `rle_wanted`; with RLE off the host runs the old unconditional
`glReadPixels`/565 path byte-for-byte.  Host **132,592 B / md5 78c135e22621fcda0fc72ae847952882**;
guest lib unchanged **419,080 B / md5 3a42016b7533c3f94805302c26ed9046**.  e2e default PASS.

Next (not RLE): guest present/frame_end surface write.  The RLE path showed the surface data can be
black before the host ever compresses it; that is an independent present problem, not a payload
codec problem.

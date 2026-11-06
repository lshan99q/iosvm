# M3 acceptance capture — stage eighteen, first shadow experiment

- **Milestone**: M3, stage 18 P0 (`s18label1`, 2026-11-06 21:05–21:30)
- **Commit**: b558b31 (host) / 8a047f4 (docs)
- **Guest firmware**: Darwin 22.5.0, 20F66, iPhone10,3; root.apfs 2026-11-06 13:47
- **Host**: Linux x86_64, Tesla V100-SXM2-16GB, NVIDIA 580.178.04, QEMU TCG with `QEMU_SMP=1`
- **Command**: `QEMU_SMP=1 HOST_ENV="GLWIRE_BACKDROP=1 GLWIRE_DRAW_LOG=1 GLWIRE_PANEL_SHADOW=1 \
  GLWIRE_SHADOW_APPLY=1 GLWIRE_SHADOW_MODE=1" tools/glwire/harness/boot-measure.sh s18label1 \
  .../userland/root.apfs 1500`
- **What it shows**: with the frame's picture taken from the shadow of the first label composite,
  every app icon's label position now holds the label layer: red bars with lighter glyph-shaped
  bars inside, in the right place, for the first time in the project. The run had 6080 frames and no
  panic (the first stable recipe, one vCPU).
- **What it does not show**: the labels are not text. Their sampler matrix (`texmat0`) is
  `nan nan nan 0`, so every glyph sample of every label draw is undefined; what reaches the screen is
  the mask times the darken pass over a synthetic backdrop. No widget content, no wallpaper.
- **Reproduced**: 2 runs of this configuration (s18label1, s18t2), same red bars

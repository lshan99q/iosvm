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

# Metal translation acceptance capture — stage 29 (experiment branch)

Two files, one frame of the experiment, taken 2026-11-21/22 23:43–23:52 on the branch
`experiment/metal-translation`.

- **Milestone**: stage 29 M2/M3 (experiment only; not a delivered path), run `phys3`/`mw-t4`
- **Commit**: branch `experiment/metal-translation` (the stage record names the run; this file is
  committed with it)
- **Guest firmware**: Darwin 22.5.0, 20F66, iPhone10,3 — the contributor's own IPSW, this host's
  root image built by `tools/metalwire/prepare-guest.sh translate`; client `metal_frame -n 400 -c 8`
- **Host**: Linux x86_64; Vulkan device 1, `Tesla V100-SXM2-16GB`, PCI `0000:05:00.0`, driver
  580.178.4.0; translator `host/metalwire/metalwire_host --gpu 1 --ram <qemu ram.img> --crc-stride 8`;
  QEMU TCG, one vCPU, `CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1"`, instance 3 (ports 4474/4475)
- **Command**: `tools/metalwire/prepare-guest.sh translate` then `tools/metalwire/run-guest.sh` with
  the host above; the frame is `frame-000059.ppm` of run `phys3`
- **What it shows**: `metal-translation-frame.png` — the picture the V100 read back after executing
  the Metal client's recorded blit: the deterministic 750×1624 test picture (vertical ramp bands,
  a dark ruler every 128 rows, the frame-counter blocks at the top, the border), byte-identical to
  the bytes the guest uploaded (`memcmp`; 400/400 frames `crc_ok=1` in this run).
  `metal-translation-webview.png` — that same frame served by `tools/glwire/webview.py --shm
  /dev/shm/metalwire-frame` and rendered by a headless Chromium at `http://127.0.0.1:6091/frame.jpg`,
  i.e. the browser path unchanged from the Replay work.
- **What it does not show**: no CoreAnimation, no iOS UI, no shader. This is a synthetic Metal client
  we wrote. It proves the device seam, the wire, the host's Vulkan execution and the publication
  path; it says nothing about CA's Metal renderer (which cannot run here — no renderer library, no
  AIR→SPIR-V compiler). Do not caption it as "Metal works for the port".
- **Notes**: `metal-translation-frame.png` sha256
  `9c79bc256dabfbe316a2616fd03c2ed1bfb295768bca352f4ae8d3d194d9d58d`;
  `metal-translation-webview.png` sha256
  `83d0eff811ef3127914ac4ef8c057932ae0328c16188b98ab5b4fd141b16aab0`. Both are the project's own
  pixels, no Apple asset. The source frame is kept in the run log tree, not in the repository.

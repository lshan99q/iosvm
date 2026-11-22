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

# Metal translation acceptance capture — stage 33 / WP-3 (BootUI picture)

- **Milestone**: WP-3 of ENG-METAL-004 (experiment only; not a delivered path), run `ca-metal-wp3b`
- **Commit**: `50232e8` (the picture commit; the tree that introduced this capture)
- **Guest firmware**: Darwin 22.5.0, 20F66, iPhone10,3 — the contributor's own IPSW; run tree
  `/home/lshan/DSH-desktop/mt-work`, image built by `tools/metalwire/prepare-guest.sh ui`, clients
  file `metal_frame`, `metal_probe`, `backboardd`; injected `libcainject.dylib`
- **Host**: Linux x86_64; Vulkan device 1 = llvmpipe / lavapipe (CPU; the other V100 was left to the
  other experiment); `host/metalwire/metalwire_host --gpu 1 --port 9111 --ram <qemu ram.img>
  --shm /dev/shm/metalwire-frame --out /tmp/wp3-frames --png-every 1`; QEMU TCG, one vCPU,
  `CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1"`, instance 3 (ports 4474/4475)
- **Command**: guest `tools/metalwire/run-guest.sh ca-metal-wp3b 300`; the host executed the two
  recorded CoreAnimation frames (`render frame 1/2: executed 3 draw(s) on texture 40/48`), wrote the
  picture into the guest's real IOSurface and read it back by reference; the capture is a
  downscale of `frame-000001.png`
- **What it shows**: `wp3-boot-picture.png` — the BootUI Apple logo as CoreAnimation's three Metal
  draws compose it on the host's Vulkan. It is the first picture from the Metal path (T1: the
  overlay's opaque black quad, the layer image, the luma pass), not from glwire.
- **What it does not show**: not SpringBoard's desktop and not the window server's finished frame
  path; the three draws are a minimal subset of CA's renderer (protocol 2/3 carry no dynamic
  pipeline or compute state), the logo layer's image is the only textured draw, and nothing is
  claimed about frame rate on lavapipe. Do not caption it as "the iOS desktop".
- **Notes**: converted from the run's JPEG to PNG so the provenance gate (`docs/demo/**/*.png`) can
  declare it; `wp3-boot-picture.png` sha256
  `acb0e075d7084b4b6d0a1bd9861859deaab329795dfe8f6f9c6601f04cd1942b`. Project's own pixels, no
  Apple asset.

# Metal translation acceptance capture — stage 34 / WP-4 (desktop composites)

Two frames of the same hybrid boot (`wp4-desk`), taken 2026-11-22 09:34 UTC (17:34 +08) on the
branch `experiment/ca-metal-device`.

- **Milestone**: WP-4 of ENG-METAL-006 (experiment only; not a delivered path)
- **Commit**: the tree that carries `docs/stage34-wp4-desktop.md` (stage 34)
- **Guest firmware**: Darwin 22.5.0, 20F66, iPhone10,3 — the contributor's own IPSW; run tree
  `/home/lshan/DSH-desktop/mt-work`, image built by `tools/metalwire/prepare-guest.sh ui` plus
  `bin/glremote_on`, clients `metal_frame`, `metal_probe`, `backboardd`; injected
  `libcainject.dylib`
- **Host**: Linux x86_64; **both** host programs on device 1 = llvmpipe/lavapipe (the V100 was not
  used): `metalwire_host --gpu 1 --port 9111 --ram <ram.img> --out /tmp/wp4-desk-frames
  --png-every 1`, and `host_glwire --no-panel 9210 <ram.img>` with `GLWIRE_SOFTWARE_GL=1` and the
  Mesa EGL vendor; QEMU TCG, one vCPU, `CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1"`, instance 3
- **Command**: `MW_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1" tools/metalwire/run-guest.sh
  wp4-desk 1300`; the captures are `frame-000014.png` and `frame-000652.png` of that run's
  `--out` tree
- **What it shows**: `wp4-desktop-composite.png` — the first 55-draw home-screen composite
  CoreAnimation produced after the BootUI shell-changed event, executed on the host's Vulkan: the
  icon grid's geometry, the dock band and the white glyph/label masks are in their recorded places.
  `wp4-desktop-late.png` — a later steady-state composite (10 draws) of the same boot: the desktop
  path is still executing after the BootUI overlay was removed (09:34:18 UTC).
- **What it does not show**: a correct desktop. The wallpaper/background layers are
  transparent-black and icons/labels are white: the host's four hand-written GLSL base functions
  implement only a slice of the function-constant behaviour CA's composite pipelines select. The
  images are evidence of the path (record → wire → Vulkan → the guest's own IOSurface), not of a
  translated picture. No frame-rate claim, no V100.
- **Notes**: both images are downscaled (375x812) from the run's 750x1624 PNGs and are the
  project's own pixels; sha256 `7b237925dfa7b2c456e06f6ef9863ad1c349e7dc16f6b401a482b49dc0cbbd7d`
  (`wp4-desktop-composite.png`, the 55-draw composite) and
  `3bf3a5e3acef7ec51aa36e0b3ad33f537ce14454dcaa1ec0d498bc7e9d7b4d9b` (`wp4-desktop-late.png`).
  The run's file names count acknowledged frames, so the 55-draw composite (render frame 14) is
  `frame-000013.png`.

# Metal translation acceptance capture — stage 34 / WP-4 (the home screen through the Metal path)

One frame of boot `wp4-v4`, taken 2026-11-22 10:19–10:20 UTC (18:19 +08).

- **Milestone**: WP-4 of ENG-METAL-006 (experiment only; not a delivered path)
- **Commit**: the tree that carries `docs/stage34-wp4-desktop.md` (stage 34)
- **Guest firmware**: Darwin 22.5.0, 20F66, iPhone10,3 — the contributor's own IPSW; run tree
  `/home/lshan/DSH-desktop/mt-work`, image with `bin/glremote_on`, clients `metal_frame`,
  `metal_probe`, `backboardd`, injected `libcainject.dylib`, and the **v4** guest plug-in
  (`HakiosMetal` 245304 bytes)
- **Host**: both host programs on device 1 = llvmpipe/lavapipe (V100 unused):
  `metalwire_host --gpu 1 --port 9111 --ram <ram.img> --out /tmp/wp4-v4-frames --png-every 1
  --missing-report /tmp/wp4-v4-missing.txt`, and `host_glwire --no-panel 9210 <ram.img>` with
  `GLWIRE_SOFTWARE_GL=1` + the Mesa EGL vendor; QEMU TCG, one vCPU, `CMDLINE="cpus=1 fb_beat=0
  fbscale=2 metal=1"`, instance 3
- **Command**: `MW_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1" tools/metalwire/run-guest.sh
  wp4-v4 1500`; the capture is `frame-000272.png` of that run's `--out` tree, downscaled to 375×812
- **What it shows**: CoreAnimation's desktop composite executed by the host's Vulkan — the app
  icons with their real images (FaceTime, Calendar, Photos, Mail, Notes, Reminders, Clock, News,
  Podcasts, App Store, Maps…), their labels, the folder widgets and the red status bar. 462 frames
  were acknowledged in this boot with 0 wire errors; three blur pipelines have no host shader
  (`docs/stage34-missing-pipelines.txt`) and 674 of their draws were skipped.
- **What it does not show**: a finished desktop. The picture is **vertically flipped** (status bar
  at the bottom, glyphs upside down) — the hybrid host-GL surfaces' row order, not a wire issue;
  the wallpaper/background-blur band is black because `downsample_blur_vert_lpf` /
  `downsample_4_frag_lpf` / `narrow_blur_23/15_frag_lpf` are not implemented, and a 256×1
  RGBA16Float (Metal format 115) ramp is skipped for the same reason. No frame-rate or V100 claim.
- **Notes**: project's own pixels; sha256
  `82803d0d4232096833110c633f01b651b695d4a743965d6071d0f5b4e155fa5f`.

# Metal translation acceptance capture — stage 35 / WP-4 (the upright desktop)

One frame of boot `wp4-v13`, taken 2026-11-23 06:31 +08 on the branch
`experiment/ca-metal-device`.

- **Milestone**: the WP-4 close of ENG-METAL-006 (experiment only; not a delivered path)
- **Commit**: `b232e33a0ed2a747fb67af177d982b427bf248b2` — the tree the boot ran; the capture is
  committed with the stage-35 record `docs/stage35-wp4-close.md`
- **Guest firmware**: Darwin 22.5.0, 20F66, iPhone10,3 — the contributor's own IPSW; run tree
  `/home/lshan/DSH-desktop/mt-work`, image built by `tools/metalwire/prepare-guest.sh ui` plus
  `bin/glremote_on`; clients `metal_frame`, `metal_probe`, `backboardd`; injected
  `libcainject.dylib` (`aa123ccd…`) and `HakiosMetal` (`1152abab…`); `root.apfs` `964abbea…`
  (the baseline, without gesture scripts)
- **Host**: Linux x86_64; `metalwire_host` on device 0 = llvmpipe/lavapipe
  (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json --gpu 0`; the V100 was not used), with
  `--port 9111 --ram <ram.img> --out /tmp/wp4v13-frames --png-every 1 --verbose --missing-report
  /tmp/wp4v13-missing.txt` and `MW_DUMP_DRAWS=1`; `host_glwire --no-panel 9210 <ram.img>` with
  `GLWIRE_SOFTWARE_GL=1` (it logged 0 clients); QEMU TCG, one vCPU,
  `CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1"`, instance 3 (ports 4474/4475)
- **Command**: `MW_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1" tools/metalwire/run-guest.sh
  wp4-v13 1800`; the capture is `frame-good-750x1624.png` of that run's `--out` tree, downscaled
  exactly to 375×812
- **What it shows**: the upright home screen through the Metal path — the status bar at the top
  (with the image's `rdar:45025538` banner), the real app icons with their images and labels, the
  Search row and the Dock at the bottom. The Dock's white band and the Search capsule are gone:
  their `Tmua` material pipeline now samples the support texture instead of painting opaque white
  (D3/D5 fixed on the host). 706 frames were acknowledged, 7,363 draws executed, `--missing-report`
  `frames 706, pipelines 0, skipped draws 0`, format 115 0, wire error 0; the two 256×1
  RGBA16Float ramps are created as Vulkan images.
- **What it does not show**: a finished or "100% correct" desktop. The wallpaper is black (D1),
  Weather/Calendar are white placeholders (D2), the status bar carries the image's debug banner
  (D7), and the presentation alternates in two frames — every second frame loses the
  status-bar/Dock/Search layers (D8; this capture is the good half, and the boot's stage-2/stage-3
  captures happened to land on the other one). Five of the eight blur kernels are implemented and
  fixture-verified but were never requested by the guest, because Control Center/Spotlight would
  not open. No frame-rate claim and no V100 claim here; the measured numbers are in
  `docs/stage35-wp4-close.md`.
- **Notes**: project's own pixels, no Apple asset. sha256
  `1de573dfc5e0206af73ab51ceb04cc8c7ed2ebaecb59febf9d7c9647990d82b2` — the LANCZOS downscale of
  the good 750×1624 frame (sha256
  `ee3d98bf03c606c251e729ffdec2e61b9befe56e204e39533c3ca7dff9202ae2`); it is pixel-identical to
  the run's own 375×812 thumbnail.

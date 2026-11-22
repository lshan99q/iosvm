# stage 32: the host executes CoreAnimation's Metal render encoder (stage C, WP-2)

> Status: **measured. The host creates Vulkan pipelines from the wire's base functions and executes
> CoreAnimation's three draws / thirteen sets, on Vulkan device 1 (lavapipe). No picture is claimed:**
> the frame is executed, not yet composed (the wire gaps in §5 are the difference). Branch
> `experiment/ca-metal-device`, 2026-11-22. This continues
> [`docs/stage31-ca-metal-wire.md`](stage31-ca-metal-wire.md) and carries out WP-2 of
> ENG-METAL-003; the spike that gates it is
> [`docs/spike-wp2-vulkan-glsl.md`](spike-wp2-vulkan-glsl.md).

## 1. What changed

The decode-only host grew an execution half, and the shaders it needs are in the tree:

| piece | file | what it does |
|---|---|---|
| four base functions | `host/metalwire/shaders/*.{vert,frag}` | hand-written Vulkan GLSL for `fixed_vert_lpf_spc`, `fixed_frag_lpf_cpf`, `std_vert1_lpf`, `luma_boost_lpf`; function constants become specialization constants |
| shader build | `host/metalwire/Makefile`, `embed_spirv.py` | `glslangValidator` + `spirv-val` at build time; the SPIR-V is embedded in `build/wp2_shaders.h`, so the host is still one binary |
| execution half | `host/metalwire/metalwire_host.c` | per-draw encoder state, Metal→Vulkan object mapping, dynamic-rendering pipeline creation, five buffer bindings, both textures, sampler, framebuffer-fetch snapshot, submit, read-back |
| synthetic render client | `host/metalwire/metalwire_gen.py --render` | a protocol-2 frame with the wire's 13-set shape, used by the spike and by tests without a guest |
| spike driver | `host/metalwire/wp2_spike.py` | runs the four composition variants on lavapipe and checks the result |
| object dump | `host/metalwire/metalwire_host.c --dump DIR` | writes decoded buffers and pipeline/sampler/texture descriptors to disk (how the 13-set interface and the per-draw geometry were read out) |

The 13 sets are replayed in arrival order, exactly as `PROTOCOL.md` describes. The execution path is
separate from the v1 blit path; a frame that carries `MW_RENDER_*` messages takes the new path, a
blit frame takes the old one.

## 2. The Metal → Vulkan decisions

* **Descriptor buffers, offsets by push constant.** CA binds one 256 KiB buffer at five offsets
  (0, 240, 336, 540, 544) across its two stages. Vulkan descriptor offsets are alignment-
  constrained, so the host binds the whole buffer once and passes the four byte offsets the shaders
  need as a 16-byte push constant. Metal's per-stage index spaces are kept (vertex buffers 2/3,
  fragment buffers 1/2), while the vertex *attribute* stream (index 1) stays a real vertex binding.
* **Dynamic rendering** (Vulkan 1.3) instead of `VkRenderPass`/`VkFramebuffer`: the load/store
  action and clear value arrive per encoder, so the loadOp is chosen at record time.
* **Framebuffer fetch as a sampled snapshot.** Metal's `[[color(0)]]` has no core-Vulkan
  equivalent; before a fetching draw the colour image is copied to a scratch image and bound as a
  sampled texture (`texelFetch` on the fragment coordinate). The spike measured that this
  reproduces fixed-function source-over; the alternative extension routes were measured too but are
  not used (see the spike §4).
* **Blending** follows the wire: `VkPipelineColorBlendAttachmentState.blendEnable` is set from
  `blendingEnabled`, which for all three recorded pipelines is off.
* **Textures** get `VkImageView`s at creation; a sampled image is moved to
  `SHADER_READ_ONLY_OPTIMAL` on first use. The R8Unorm path is the same path (the spike proves the
  ramp is sampled).
* **Read-back is for evidence, not for a picture.** After the draws the target is copied to the
  host buffer and its CRC is logged/acknowledged, and the frame is published through the existing
  glwire SHM/publication code (WP-3 owns the actual picture).

## 3. Evidence: the real recorded frame

Boot configuration: the WP-1 recipe — one vCPU, `metal=1`, clients file `metal_frame`, `metal_probe`,
`backboardd`, the injected dylib and the experiment's own root image — and the host on **lavapipe**
(`--gpu 1`, `--ram mt-work/hakios/qemu/ram.img`; V100 `0000:05:00.0` untouched). Boot
`ca-metal-c-4`, serial log `mt-work/logs/ca-metal-c-4/serial.log`.

Host, on the two CoreAnimation frames the boot produced:

```
metalwire: hello: version 2 pid 20 program "backboardd"
metalwire: render: encoder 0: target texture 40 (750x1624 format 80), load 2 store 1, clear 0.000 0.000 0.000 0.000, depth 0
metalwire: render: encoder 0 ended: 3 draws, 13 sets
metalwire: render: pipeline 41 "com.apple.coreanimation.draw.PBGRA_Xfc" -> Vulkan pipeline (topology 3, texture 0, fetch 0)
metalwire: render: pipeline 44 "com.apple.coreanimation.draw.PBGRABsov_TimgXfc_Isrc" -> Vulkan pipeline (topology 3, texture 1, fetch 1)
metalwire: render: pipeline 47 "com.apple.coreanimation.draw.PBGRA_TlubXf_Isrc" -> Vulkan pipeline (topology 3, texture 1, fetch 1)
metalwire: render frame 1: executed 3 draw(s) on texture 40 (750x1624), crc 0x2163bd98, 1218000/4872000 non-zero byte(s)
metalwire: render frame 1: 1 encoder(s), 3 draws, 13 sets, target texture 40 750x1624, status 0 (executed on Vulkan)
metalwire: render frame 2: executed 3 draw(s) on texture 48 (750x1624), crc 0x2163bd98, 1218000/4872000 non-zero byte(s)
metalwire: render frame 2: 1 encoder(s), 3 draws, 13 sets, target texture 48 750x1624, status 0 (executed on Vulkan)
```

`1218000/4872000` non-zero bytes is 750×1624 pixels with alpha 1 and RGB 0: the first recorded draw
wrote opaque black over the whole target. Draw B's layer image is invisible because its pixels
never left the guest (`replaceRegion:` is not serialized), and draw C's luma pass is the identity
because the recorded uniform's EDR scale is zero. `status 0` and the clean log are the acceptance
here; the picture is WP-3's.

No Vulkan error was logged on either frame. When the same sequence is run with
`VK_LAYER_KHRONOS_validation` (1.3.275) loaded, it reports no `VUID` violations. The one bug found
on the way (`VkRenderingInfo.layerCount` left 0) crashed llvmpipe's LLVM JIT; the validation
layer named it in one line, and it is fixed in the tree.

## 4. Regression

* `make test` (lavapipe, v1 blit path): PASS — 2 frames, CRC, CSV, SHM, PPM/PNG.
* Synthetic-client regression on the same host: `metalwire_gen.py 750 1624 --frames 60` →
  `60/60 frame(s) acknowledged`, `60 executed with crc_ok`, zero mismatches.
* `wp2_spike.py --validation`: PASS — three pipelines executed, no validation error, the four
  composition variants produce the numbers in the spike note.

## 5. What this does not establish, and the wire gap it found

- **Not a picture.** Protocol 2 does not carry the two per-draw offset setters CoreAnimation calls
  between its draws: the guest log shows `-[MTLRenderCommandEncoder setVertexBufferOffset:atIndex:]`
  and `setFragmentBufferOffset:atIndex:` as "asked and not implemented", and the captured buffer
  keeps the three quads at 336/576/816. The host therefore executes all three draws with the last
  full binding. That is a faithful execution of the 13 sets the wire has, but draw B's geometry is
  not the recorded geometry. **WP-3 (or a reviewed protocol 3 addition) needs those two opcodes
  before a picture can be expected**; the finding is recorded in `host/metalwire/PROTOCOL.md`.
- **Not the V100.** The instruction for this session was to use GPU 1 and leave the V100 to the
  other experiment; `--gpu 1` on this host is lavapipe. The stage-C PCI-id rule is unchanged in the
  host (`--pci 0000:05:00.0`), and the same execution path is what would run there, but the V100
  acceptance row of ENG-METAL-003 is still open.
- **Not the frame rate.** The ack is not GPU timing on the target card; lavapipe is a CPU device.
- **Not the two missing base functions.** `std_vert1_lpf` and `luma_boost_lpf` are reconstructed
  from the wire (their Metal source is not in the guest image); the GLSL says so in its header.

## 6. Files changed

- `host/metalwire/shaders/` + `shaders/README.md` — the four base functions and their provenance.
- `host/metalwire/metalwire_host.c` — execution half, `--dump`, dynamic rendering, per-draw state.
- `host/metalwire/Makefile`, `embed_spirv.py`, `build/wp2_shaders.h` (generated) — shader build.
- `host/metalwire/metalwire_gen.py` — `--render`/`--variant` synthetic protocol-2 client.
- `host/metalwire/wp2_spike.py` — the spike's measurement driver.
- `host/metalwire/README.md`, `PROTOCOL.md` — execution path, options, the offset-setter gap.
- `docs/spike-wp2-vulkan-glsl.md` — the spike record (Q1/Q2/Q3).
- `README.md` — the state table's stage-32 row.

## 7. Reproduce

```sh
# host, from the repository root, on lavapipe (Vulkan device 1)
host/metalwire/metalwire_host --gpu 1 --port 9111 \
    --ram ~/DSH-desktop/mt-work/hakios/qemu/ram.img --frames 0

# guest: the WP-1 recipe (docs/stage31-ca-metal-wire.md §6), then
MW_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1" tools/metalwire/run-guest.sh ca-metal-c-4 300

# without a guest, the synthetic render frame:
host/metalwire/metalwire_gen.py --render --variant fetch --port 9111
```

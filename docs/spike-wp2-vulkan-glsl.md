# spike WP-2: the four CoreAnimation base functions in Vulkan GLSL, on lavapipe

> Status: **spike done, measured.** Branch `experiment/ca-metal-device`, 2026-11-22. Every number
> and log line below was produced on this host in this session, on **Vulkan device 1 = lavapipe**
> (`--gpu 1`; the V100 at PCI `0000:05:00.0` was in use by another experiment and was not touched).
> This answers the three questions in ENG-METAL-003 §3.2 and is the gate for WP-2's integration
> half (`docs/stage32-ca-metal-execute.md`).

## 1. What was written

Four hand-written Vulkan GLSL files — no Apple source, no third-party translator — under
[`host/metalwire/shaders/`](../host/metalwire/shaders/):

| file | Metal base function | role in the BootUI frame |
|---|---|---|
| `fixed_vert_lpf_spc.vert` | `fixed_vert_lpf_spc` | position through the CA matrix; texcoords through the texture matrix; colour varying (pipelines A and B) |
| `fixed_frag_lpf_cpf.frag` | `fixed_frag_lpf_cpf` | layer fill / textured source-over; reads the destination when `Bsov` is set (pipelines A and B) |
| `std_vert1_lpf.vert` | `std_vert1_lpf` | the standard variant used by the luma pass (pipeline C) |
| `luma_boost_lpf.frag` | `luma_boost_lpf` | destination luminance → 128×1 R8Unorm ramp → scaled rewrite (pipeline C) |

The interface was reconstructed from the recorded draw (`docs/stage31-ca-metal-wire.md`) and from
the on-disk sibling shader family (`default.pipelinelib`, the `lph_*` modules): the argument
signatures, buffer locations, texture/sampler locations and the 176-byte
`CA::OGL::Metal::Uniform` are the same in that family. Only descriptive notes were used; no Apple
bytes are in the tree (see [`shaders/README.md`](../host/metalwire/shaders/README.md)).

Build (glslang is a build-time dependency; the SPIR-V is embedded into the host binary):

```sh
cd host/metalwire
make shaders      # glslangValidator -V --target-env vulkan1.1 + spirv-val, then embed_spirv.py
```

All four modules compile and pass `spirv-val`:

```
fixed_vert_lpf_spc.vert  OK
fixed_frag_lpf_cpf.frag  OK
std_vert1_lpf.vert       OK
luma_boost_lpf.frag      OK
```

## 2. Q1 — do the four base functions cover the three pipelines?

**Yes, at the level the wire can express.** The three pipelines of the recorded frame map to the
four files as ENG-METAL-003 says: A and B share `fixed_vert_lpf_spc` + `fixed_frag_lpf_cpf`, C uses
`std_vert1_lpf` + `luma_boost_lpf`. The differences between A and B are function constants in the
Metal base function; here they are Vulkan specialization constants (`HAS_TEXTURE`, `FETCH_DEST`,
`PREMULTIPLY`, `USE_TEXTURE_MATRIX`), selected at pipeline creation from the pipeline's name.

Measured: the synthetic CA-shaped frame (`metalwire_gen.py --render`) creates and executes all
three Vulkan pipelines on lavapipe (`3 pipelines` in `wp2_spike.py`); the real CoreAnimation boot
below executes its three recorded draws through them.

The two base functions that the guest image does not carry (`std_vert1_lpf`, `luma_boost_lpf`) are
**[INFERRED]**: the wire constrains only their interface and effect (a full-screen pass that reads
the destination and the R8Unorm ramp). The sibling corpus' closest behaviour is the
luminance-weighted source-over of its `lph` modules; the GLSL here is a minimal, documented
reconstruction, not a decompiled copy.

## 3. Q2 — is the 13-set interface consumed completely, including R8Unorm?

The authoritative list is the real frame (`docs/stage31-ca-metal-wire.md` §3). Note that the
"4 buffer" wording of ENG-METAL-003 is a miscount: the frame has **one buffer object with five
bindings**, plus two textures and one sampler. The rewrites consume all of them:

| # | wire binding | Vulkan mapping in the host | consumed by |
|---|---|---|---|
| 1 | vertex buffer 1, offset 336 (attribute stream, stride 48) | `vkCmdBindVertexBuffers` binding 1 + the pipeline's vertex descriptor | locations 0–3 |
| 2 | vertex buffer 2, offset 240 (64 B `mvp_matrix`) | wire buffer at push-constant byte offset 240 | `fixed_vert_lpf_spc`, `std_vert1_lpf` |
| 3 | vertex buffer 3, offset 544 (16 B texture matrix) | wire buffer at push-constant byte offset 544 | `fixed_vert_lpf_spc`, `std_vert1_lpf` |
| 4 | fragment buffer 1, offset 0 (176 B `CA::OGL::Metal::Uniform`) | wire buffer at push-constant byte offset 0 | `fixed_frag_lpf_cpf`, `luma_boost_lpf` |
| 5 | fragment buffer 2, offset 540 (4 B `lod_bias`) | wire buffer at push-constant byte offset 540 | `fixed_frag_lpf_cpf` |
| 6 | fragment texture 3 = **BGRA8** 129×158 layer image | sampled image, fragment binding 1 | `fixed_frag_lpf_cpf` (draw B) |
| 7 | fragment texture 3 = **R8Unorm** 128×1 luma ramp | sampled image, fragment binding 1 (R8 view) | `luma_boost_lpf` (draw C) |
| 8 | fragment sampler 0 | sampler, fragment binding 2 | draws B and C |
| 9 | viewport 750×1624; viewport/scissor are the remaining "sets" | `vkCmdSetViewport` / `vkCmdSetScissor` | every draw |
| 10 | 16-attribute vertex descriptor (4 valid attributes) | `VkVertexInputAttributeDescription` (Float4, Float2, Float2, Half4) | `fixed_vert_lpf_spc`, `std_vert1_lpf` |

The one Metal concept without a wire "set" is the framebuffer fetch input `[[color(0)]]` (pipeline
B/C). It is translated to a sampled snapshot of the colour attachment (core Vulkan has no
framebuffer fetch; see §4). That is an implicit attachment in Metal, so it does not belong to the
13-set list.

**R8Unorm is measured, not just bound.** The spike's `fetch-noramp` variant leaves the 128×1 ramp
empty; the same frame then reads a different colour (see §4 table), which means the R8 texture is
actually sampled by the shader — the WP-1 loss of the 13th set is not repeated.

## 4. Q3 — what is CoreAnimation's blending behaviour?

**The descriptor's `blendingEnabled = NO` is the whole truth about fixed-function blending: it is
off. Composition happens inside the fragment shaders, through Metal's framebuffer-fetch input.**

Evidence, in the order it was obtained:

1. The wire carries `blend 0` for all three pipelines; pipeline B's factors (One,
   OneMinusDestinationColor) are dead values while blending is disabled.
2. The on-disk sibling family (`default.pipelinelib`, `lph` specialisations) contains 166 fragment
   modules that take a `[[color(0)]]` input; 121 of the 305 `cpf` modules do. The BootUI pipeline
   names contain `Bsov` ("blend source-over") and `Tlub` (texture luma boost), and draw C reads the
   destination written by draw B.
3. Measured on lavapipe with the synthetic CA-shaped frame (`wp2_spike.py`):
   draw A = opaque green background, draw B = 50%-alpha red texture with white tint, draw C = the
   luma pass with a seeded R8 ramp. Centre pixel, BGRA read-back:

| variant | centre pixel (R,G,B,A) | what it shows |
|---|---|---|
| `fetch` — `Bsov` name, `blendEnabled = NO`, shader reads the destination | **(108, 72, 18, 255)** | in-shader source-over of red over green |
| `nofetch` — same draws, shader does not read the destination | (154, 0, 0, 128) | the second draw **replaces**; the green is gone |
| `fixed` — `blendEnabled = YES`, SRC_ALPHA / ONE_MINUS_SRC_ALPHA | **(108, 72, 18, 191)** | fixed-function source-over gives the same RGB |
| `fetch-noramp` — CA path with the R8 ramp left empty | (76, 51, 13, 255) | the R8 ramp content matters (sampled) |

So: with the descriptor as recorded, `vkPipelineColorBlendState.blendEnable = VK_FALSE` is the
correct mapping; the shader must fetch and composite, and when it does, the result matches what a
fixed-function source-over equation would have produced. A translation that only mutes the blend
state and does not provide the fetch input produces the `nofetch` row — a replaced picture.

The translator spells the fetch as a sampled copy of the colour attachment. This was measured to
work on lavapipe without validation errors; the alternatives (`VK_EXT_rasterization_order_attachment_access`
in a single subpass, and `VK_KHR_dynamic_rendering_local_read`) also work on this Mesa version but
need an extension/feature set the V100's driver has not been asked for yet, so WP-2 keeps the
portable copy. Cost is two 750×1624 copies per frame (the draw B and draw C fetches).

## 5. Real CoreAnimation frame — the wire's own data

A boot with the WP-1 recipe (`metal=1`, clients file with `backboardd`) and the executing host on
lavapipe (`host/metalwire/metalwire_host --gpu 1 --port 9111 --ram …`) produced two CoreAnimation
frames and the host executed both:

```
metalwire: render: encoder 0: target texture 40 (750x1624 format 80), load 2 store 1, clear 0.000 0.000 0.000 0.000, depth 0
metalwire: render: encoder 0 ended: 3 draws, 13 sets
metalwire: render frame 1: executed 3 draw(s) on texture 40 (750x1624), crc 0x2163bd98, 1218000/4872000 non-zero byte(s)
metalwire: render frame 1: 1 encoder(s), 3 draws, 13 sets, target texture 40 750x1624, status 0 (executed on Vulkan)
metalwire: render frame 2: executed 3 draw(s) on texture 48 (750x1624), crc 0x2163bd98, 1218000/4872000 non-zero byte(s)
```

`1218000/4872000` non-zero bytes is 750×1624 pixels whose only non-zero channel is alpha: the
first draw wrote its opaque black colour across the whole target (RGB 0, A 1), which is exactly
what the recorded first draw does. Draw B's image is invisible because Metal's `replaceRegion:` is
not serialized yet (WP-3's texture-upload gap), and draw C's luma pass is the identity because the
recorded uniform's EDR scale is zero. **No Vulkan error was logged** (see §6).

## 6. Validation and regression

* `wp2_spike.py --validation` runs all four variants with
  `VK_LAYER_KHRONOS_validation` (1.3.275) loaded and reports zero `VUID` errors.
* `make test` (the v1 blit path) still passes on lavapipe: 2 frames, CRC, CSV, SHM, PPM/PNG.
* The synthetic client regression `metalwire_gen.py 750 1624 --frames 60` on the same lavapipe host:
  `60/60 frame(s) acknowledged`, `60 executed with crc_ok`, zero `crc_ok=0`.

## 7. Findings and limitations (input to WP-2 integration / WP-3)

1. **Protocol 2 is missing the two per-draw offset setters.** The guest log shows
   `-[MTLRenderCommandEncoder setVertexBufferOffset:atIndex:]` and
   `setFragmentBufferOffset:atIndex:` being asked ("asked and not implemented") between the draws;
   the captured buffer lays the three quads at 336/576/816 with their index buffers at
   528/768/1008. The encoder's 13 sets carry the buffer bindings, not those offsets, so a host
   replaying protocol 2 executes all three draws with the last full binding (quad A). It is
   structurally correct — three pipelines, three draws, thirteen sets — but draw B's geometry is
   not the recorded geometry. **WP-3 needs two new opcodes (or a version 3 addition) before a
   picture can be expected.** WP-2 does not change the protocol: the finding is recorded here and
   in `PROTOCOL.md`.
2. `std_vert1_lpf` and `luma_boost_lpf` are not in the guest image; their GLSL is inferred from the
   wire, tagged as such in the source, and must be revisited if WP-3 finds the missing library.
3. The V100 was not exercised in this session (the user's instruction: use GPU 1, do not disturb
   the other experiment; `--gpu 1` is lavapipe). The host still selects the V100 by PCI id for the
   stage-C rule; the same Vulkan code path is what would run there, but WP-2's V100 acceptance row
   remains open.
4. The lavapipe path needed `VkRenderingInfo.layerCount = 1`; with `layerCount = 0` llvmpipe
   crashed inside its LLVM JIT (the validation layer named it in one line). Recorded because it is
   a host bug that the V100 would have hidden.

## 8. Reproduce

```sh
cd host/metalwire
make shaders && make                     # glslang + spirv-val + embed + host
./wp2_spike.py --validation              # 4 variants, 5 PASS lines
# and the real frame, in another shell, with the guest boot of docs/stage31 §6:
./metalwire_host --gpu 1 --port 9111 --ram ~/DSH-desktop/mt-work/hakios/qemu/ram.img --frames 0
```

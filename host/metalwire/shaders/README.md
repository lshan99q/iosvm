# The WP-2 shaders: four CoreAnimation base functions in Vulkan GLSL

These four files are hand-written Vulkan GLSL. They are the WP-2 rewrite of the four Metal base
functions the metalwire wire names (`fixed_vert_lpf_spc`, `fixed_frag_lpf_cpf`, `std_vert1_lpf`,
`luma_boost_lpf`), and they are what `metalwire_host` uses to execute a protocol-2 render frame.
The build compiles them with `glslangValidator -V --target-env vulkan1.1`, validates the SPIR-V
with `spirv-val`, and embeds it into the host binary (`../embed_spirv.py`).

## Provenance

Nothing here is Apple code and nothing here is copied from an Apple binary.

- The **interface** (which attributes, buffers, textures, samplers and offsets the functions take)
  is the wire's: the 16-attribute vertex descriptor, the 13 sets and the captured buffer layout in
  `docs/stage31-ca-metal-wire.md` and `docs/spike-wp2-vulkan-glsl.md`. That record is the
  authoritative source, and the host prints it with `metalwire_host --dump DIR`.
- The **shape of the semantics** was cross-checked against the guest image's sibling shader family
  (`QuartzCore.framework/default.pipelinelib`, the `lph_*` specialisations) as read-only evidence:
  its argument metadata gives the `CA::OGL::Metal::Uniform` byte table (176 B: `image_arg`,
  `blend_colors`, `edr_factors`, `hdr_scale`, `premultiply`, `linear_edr`, `clamp_range`,
  `filter_arg`, `coord_arg`, the 40-byte constant union), the `mvp_matrix`/`texture_matrix` buffer
  roles, and the fact that 166 fragment modules take a framebuffer-fetch colour. No byte of Apple's
  shaders is reproduced or committed; the analysis lives in `docs/research/ca-metallib-url.md` and
  the WP-2 notes. This follows the project's provenance rule (Apple assets stay on the
  contributor's disk).
- `std_vert1_lpf` and `luma_boost_lpf` are **not** in the guest image. Their GLSL is a minimal
  reconstruction from the wire and is marked `[INFERRED]` in the file header.

If the project ever obtains the missing `default.metallib`, the inferred two should be revisited
first; everything else can be checked against the record.

## Interface

The host maps the wire to one descriptor set and a 16-byte push constant:

| binding | type | stage | wire origin |
|---|---|---|---|
| set 0, binding 0 | `readonly buffer WireBuffer { uint wire[]; }` (std430, whole CA buffer) | vertex + fragment | the one buffer object the frame binds at five offsets |
| set 0, binding 1 | sampled image | fragment | Metal fragment texture 3 (BGRA8 layer image, or the 128×1 R8Unorm ramp) |
| set 0, binding 2 | sampler | fragment | Metal fragment sampler 0 |
| set 0, binding 3 | sampled image | fragment | the colour-attachment snapshot that spells Metal's `[[color(0)]]` fetch |
| push constant | `uint vertex_mvp, vertex_texmat, fragment_uniform, fragment_lod` | both | the byte offsets of the Metal buffers inside the wire buffer |

The vertex attribute stream is a normal Vertex buffer: Metal stream 1, stride 48, four attributes
(`Float4 position @0`, `Float2 texcoord0 @16`, `Float2 texcoord1 @24`, `Half4 colour @32`).

Function constants of the Metal base functions are specialization constants here:

| shader | constant | selects |
|---|---|---|
| `fixed_vert_lpf_spc.vert`, `std_vert1_lpf.vert` | `USE_TEXTURE_MATRIX` | `Timg`/`Tlub` variants that multiply texcoords by the texture matrix |
| `fixed_frag_lpf_cpf.frag` | `HAS_TEXTURE` | the no-texture fill vs the textured layer draw |
| | `FETCH_DEST` | the `Bsov` source-over path that reads the destination |
| | `PREMULTIPLY` | a pipeline whose premultiply flag was baked (the uniform byte is the runtime flag) |

`luma_boost_lpf.frag` always reads the destination and the R8Unorm ramp; when the recorded uniform's
EDR scale is zero it is the identity, which is what the wire's BootUI frame asks for.

## Files

| file | base function |
|---|---|
| [`fixed_vert_lpf_spc.vert`](fixed_vert_lpf_spc.vert) | `fixed_vert_lpf_spc` |
| [`fixed_frag_lpf_cpf.frag`](fixed_frag_lpf_cpf.frag) | `fixed_frag_lpf_cpf` |
| [`std_vert1_lpf.vert`](std_vert1_lpf.vert) | `std_vert1_lpf` `[INFERRED]` |
| [`luma_boost_lpf.frag`](luma_boost_lpf.frag) | `luma_boost_lpf` `[INFERRED]` |

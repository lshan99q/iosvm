# metalwire protocol v1 (Metal → Vulkan translation experiment)

Status: experimental, branch `experiment/metal-translation`. The wire is not a public ABI.

The contract lives in `metalwire_protocol.h` (shared by both ends). This note explains the
conventions the two implementations rely on.

## Transport

- TCP, one connection, guest is the client (`10.0.2.2:9111` by default, QEMU's user network maps
  that to the host's loopback).
- Every message is a `struct mw_msg` header followed by `length` payload bytes. Integers are
  little-endian host order. The host refuses a message whose `magic`/`version` is not ours.
- One acknowledgement per `MW_FRAME_END`, sent as the **raw** `struct mw_ack` (56 bytes, its own
  magic `MWAK`), not wrapped in a `struct mw_msg` header: it is a fixed-size reply, and its own
  magic keeps a reader that is resynchronising from mistaking it for a request. The guest's frame
  loop is synchronous in v1: record, send, wait for that frame's ack. The host may answer out of
  order but v1 clients do not pipeline.

## Object model

The guest owns ids. `MW_CREATE_BUFFER`/`MW_CREATE_TEXTURE` allocate on the host and are not
repeated per frame; `MW_DESTROY_*` release. The test client creates one 750x1624 texture and one
picture-sized buffer and reuses them. The first use of an object may come *inside* a frame: the
records are applied in arrival order (a create is live as soon as it is read), and only the GPU
work is deferred to `MW_FRAME_END`. A host must not require creates to precede `MW_FRAME_BEGIN`.

- `MW_CREATE_BUFFER`: id, options (for the log), length. Storage is host-visible staging.
- `MW_UPLOAD_BUFFER`: the `length` bytes directly after the struct. Offsets are buffer-relative.
- `MW_CREATE_TEXTURE`: id, width, height, pixel format (`MW_FORMAT_BGRA8_UNORM` = 80 for the test),
  usage (for the log). v1 textures are `VK_IMAGE_TILING_OPTIMAL`, and the host does the layout
  transitions itself (copy destination, then transfer source for read-back).
- `MW_COPY_BUFFER_TO_TEXTURE`: one rectangle copy, row-major, `bytes_per_row` as Metal's
  `sourceBytesPerRow`; `origin_x/y` is the destination offset. v1 allows only one copy per frame.
- `MW_PRESENT`: the texture that is the frame's picture. The host reads it back and publishes it.
- `MW_FRAME_BEGIN` / `MW_FRAME_END`: a frame's command group, and its CRC-32 (`zlib` polynomial,
  initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF — the usual CRC-32) over the picture bytes the
  guest believes it sent (height rows of width*4 bytes, no padding).

## What "executed on the GPU" means here

The host does the copy on the V100 through Vulkan: a staging buffer upload, a
`vkCmdCopyBufferToImage`, a `vkCmdCopyImageToBuffer` read-back, a fence wait and the SHM
publication. It does not perform the copy on the CPU. The pixel data still crosses the socket,
exactly like glwire's picture does; the translation being measured is Metal command → Vulkan
command, and the host-side cost includes the real GPU round trip.

## What the guest must not assume

- No swapchain, no surface: the host is headless and presents by reading the texture back. That is
  how a VM's window path has to work (the host decodes the picture for the browser).
- The host may have several physical devices; it chooses one (`--gpu`, default 1) because the V100
  in slot 0 belongs to another experiment. It prints the chosen device and refuses to run on a
  device it was not asked for.
- The ack's `crc_ok` is the pixel-level check: 1 means the host read back exactly the guest's
  bytes. A mismatch is a bug in the translation, not in the test.

## SHM publication (the browser path)

When the host is given `--shm PATH` it publishes each frame in glwire's format, so
`tools/glwire/webview.py --shm PATH` serves the translated frame unchanged:

    uint64 magic = 0x57464C47 ("GLFW"), uint64 seq, uint32 width, uint32 height,
    uint32 bytes, uint32 pad, uint64 CLOCK_MONOTONIC ns, then width*height*4 bytes of BGRA

The file is grown once and rewritten in place; `webview.py` re-encodes whenever `seq` changes.

## Protocol 2: render encoders (stage C)

`MW_VERSION` is 2. The v1 messages are unchanged (the synthetic blit client is the same path it
was), and the new opcodes replay an `MTLRenderCommandEncoder` that the guest's device recorded:

| opcode | payload | meaning |
|---|---|---|
| `MW_CREATE_RENDER_PIPELINE` 0x0073 | `mw_create_render_pipeline` + `mw_vertex_attribute[]` + `mw_vertex_layout[]` | one render pipeline state: name, vertex/fragment base functions, color format, blend state, vertex descriptor |
| `MW_CREATE_SAMPLER` 0x0074 | `mw_create_sampler` | one sampler state's descriptor |
| `MW_CREATE_DEPTH_STENCIL` 0x0075 | `mw_create_depth_stencil` | one depth-stencil state's depth half |
| `MW_RENDER_BEGIN` 0x0060 | `mw_render_begin` | an encoder starts on its render target (color attachment 0, optional depth), with load/store actions and clear values |
| `MW_RENDER_SET_PIPELINE` 0x0061 | `mw_render_set_pipeline` | `setRenderPipelineState:` |
| `MW_RENDER_SET_DEPTH_STENCIL` 0x0062 | `mw_render_set_depth_stencil` | `setDepthStencilState:` |
| `MW_RENDER_SET_VERTEX_BUFFER` / `..._FRAGMENT_BUFFER` 0x0063 / 0x0067 | `mw_render_set_buffer` | `set*Buffer:offset:atIndex:` |
| `MW_RENDER_SET_VERTEX_BYTES` / `..._FRAGMENT_BYTES` 0x0064 / 0x0068 | `mw_render_set_bytes` + the bytes | `set*Bytes:length:atIndex:`, body inside the message like `MW_UPLOAD_BUFFER` |
| `MW_RENDER_SET_VERTEX_TEXTURE` / `..._FRAGMENT_TEXTURE` 0x0065 / 0x0069 | `mw_render_set_texture` | `set*Texture:atIndex:` |
| `MW_RENDER_SET_VERTEX_SAMPLER` / `..._FRAGMENT_SAMPLER` 0x0066 / 0x006A | `mw_render_set_sampler` | `set*SamplerState:atIndex:` |
| `MW_RENDER_SET_VIEWPORT` 0x006B | `mw_render_set_viewport` | `setViewport:` |
| `MW_RENDER_SET_SCISSOR` 0x006C | `mw_render_set_scissor` | `setScissorRect:` |
| `MW_RENDER_SET_CULL_MODE` / `..._WINDING` / `..._FILL_MODE` 0x006D / 0x006E / 0x006F | `mw_render_set_state` | the small enum setters |
| `MW_RENDER_SET_BLEND_COLOR` 0x0070 | `mw_render_set_blend_color` | `setBlendColorRed:...` |
| `MW_RENDER_DRAW` 0x0071 | `mw_render_draw` | one draw (primitive, indexed/non-indexed, instances, base vertex), with the state the encoder held when it was recorded |
| `MW_RENDER_END` 0x0072 | `mw_render_end` | the encoder ends; draw/set counts for the log |

Rules the two ends rely on:

- **Object creation is hoisted, not repeated per frame.** A pipeline/sampler/depth state, a
  texture and a buffer each have their create message before the first render message naming
  them in a connection. Buffers are uploaded by reference when the kernel supports `hakios_vtop`
  (the same `MW_UPLOAD_BUFFER_PHYS` as v1), by body otherwise.
- **The ops are applied in arrival order.** `MW_RENDER_BEGIN` opens an encoder; the set/draw
  messages are the calls in the order CoreAnimation made them; `MW_RENDER_END` closes it. A frame
  may carry several encoders.
- **A render frame has no guest CRC.** The guest did not draw the pixels, so `MW_FRAME_END.crc` is
  0 and `width`/`height` are the render target's. The host answers `crc_ok = 1` without a
  read-back; in protocol 2 the host decodes and validates the commands, and paints only once the
  WP-2 shaders exist. A `MW_PRESENT` naming the last render target follows the last
  `MW_RENDER_END`, so the frame's picture is well-defined for the execution half.
- **One render frame means one acknowledged frame.** The v1 ack path is unchanged: one
  `struct mw_ack` per `MW_FRAME_END`.

## Protocol 2 execution (WP-2)

The host now executes the render encoder instead of only decoding it. The mapping is in
[`README.md`](README.md#render-encoders-stage-c-wp-2) and the four base functions are in
[`shaders/`](shaders/). Two facts about the wire matter to the execution half:

- **The encoder state that reaches a draw is the state of the set/draw stream.** A `MW_RENDER_DRAW`
  carries only its own arguments; the pipeline, buffers, textures, sampler, viewport and scissor
  are the ones applied before it. This is the Metal model and the host replays it.
- **Two Metal setters are not on the wire yet.** CoreAnimation calls
  `-[MTLRenderCommandEncoder setVertexBufferOffset:atIndex:]` and
  `setFragmentBufferOffset:atIndex:` between its draws (the guest log records them as "asked and
  not implemented"); their arguments are not serialized. The captured buffer shows what they mean:
  the three quads sit at 336/576/816 and their index buffers at 528/768/1008, while the 13 sets
  bind only the first offsets. A protocol 2 replay therefore executes every draw with the encoder's
  last full binding — structurally correct, but not the recorded geometry. Closing this needs two
  new opcodes (or a version 3 addition) and is WP-3 input; WP-2 deliberately does not change the
  wire.

The protocol number stays 2: no message shape changed in WP-2.

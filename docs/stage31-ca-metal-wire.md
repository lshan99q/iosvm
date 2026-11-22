# stage 31: CoreAnimation's Metal calls on the metalwire wire (stage C, WP-1)

> Status: **measured. CoreAnimation's render encoder is serialized on protocol 2 and decoded by the
> host; nothing is drawn yet (that is WP-2).** Branch `experiment/ca-metal-device`, 2026-11-22.
> Every number and log line was taken on this host in this session. This continues
> [`docs/stage30-ca-metal.md`](stage30-ca-metal.md) and implements WP-1 of `ENG-METAL-002`; the
> wire contract is [`host/metalwire/PROTOCOL.md`](../host/metalwire/PROTOCOL.md) and
> [`metalwire_protocol.h`](../host/metalwire/metalwire_protocol.h).

## 1. What changed

Three pieces, in the order the data flows:

1. **The wire (protocol 2).** `MW_VERSION` 2 adds 25 opcodes: the three render objects
   (`MW_CREATE_RENDER_PIPELINE` 0x0073, `MW_CREATE_SAMPLER`, `MW_CREATE_DEPTH_STENCIL`) and the
   render-encoder calls (`MW_RENDER_BEGIN` 0x0060 ... `MW_RENDER_END` 0x0072). The v1 blit/present
   messages are unchanged; the synthetic client's path is the same path it was, at version 2.
2. **The guest.** `HakiosMetal.m` copies, out of CoreAnimation's objects and into plain C, the
   render pass (color attachment, load/store actions, clear color, optional depth), every setter
   it implements (pipeline, depth-stencil, buffers, bytes, textures, samplers, viewport, scissor,
   cull/winding/fill, blend color, stencil reference) and every draw (primitive, indexed/non-indexed,
   instances, base vertex/instance). At `-commit` the command buffer sends `MW_FRAME_BEGIN`, the
   objects and calls in arrival order, one `MW_PRESENT` naming the last render target, and
   `MW_FRAME_END` with `crc = 0` (the guest did not draw the picture, so it cannot hash it).
3. **The host.** `metalwire_host` decodes and validates the stream: every texture, buffer and
   pipeline a call names must have its create message first; the render pass's target must exist;
   a draw must follow a pipeline binding. It logs the first render frame's calls as they decode, and
   acknowledges the frame without a read-back, because the execution half (WP-2's shaders) is not
   there yet. `MW_CREATE_TEXTURE` now maps Metal's usage bits (shaderRead/shaderWrite/renderTarget)
   to Vulkan usage flags, and `R8Unorm` joined the wire's formats: CoreAnimation's luma-boost ramp
   is a 128x1 `R8Unorm` texture.

## 2. Evidence (boot `ca-metal-c-2`)

Configuration: one vCPU, `metal=1` + clients file `metal_frame`, `metal_probe`, **`backboardd`**,
the injected `libcainject.dylib`, the experiment's own root image; host
`metalwire_host --pci 0000:05:00.0 --port 9111 --ram mt-work/hakios/qemu/ram.img`.

Guest, in order (log `mt-work/logs/ca-metal-c-2/serial.log`):

```
metalwire: CA render pass: encoder 0 target texture 40 750x1624 format 80 load 2 store 1 clear 0.00,0.00,0.00,0.00
metalwire: newRenderPipelineStateWithDescriptor:error:: serialized pipeline 41 name "com.apple.coreanimation.draw.PBGRA_Xfc" vertex "fixed_vert_lpf_spc" fragment "fixed_frag_lpf_cpf"
metalwire: newRenderPipelineStateWithDescriptor:error:: serialized pipeline 44 name "com.apple.coreanimation.draw.PBGRABsov_TimgXfc_Isrc" vertex "fixed_vert_lpf_spc" fragment "fixed_frag_lpf_cpf"
metalwire: newRenderPipelineStateWithDescriptor:error:: serialized pipeline 47 name "com.apple.coreanimation.draw.PBGRA_TlubXf_Isrc" vertex "std_vert1_lpf" fragment "luma_boost_lpf"
metalwire: CA render encoder: 3 draws, 13 sets, pipeline com.apple.coreanimation.draw.PBGRA_TlubXf_Isrc (serialized at commit)
metalwire: CA frame: 1 render encoders, 3 draws (serializing on the wire)
metalwire: hello to 10.0.2.2:9111: pid 20, program backboardd
```

Host, decoding the two frames the boot produced (`/tmp/mw-ca-host2.log`):

```
metalwire: hello: version 2 pid 20 program "backboardd"
metalwire: render: encoder 0: target texture 40 (750x1624 format 80), load 2 store 1, clear 0.000 0.000 0.000 0.000, depth 0
metalwire: CA set: viewport 0.0 0.0 750.0x1624.0 z 0.000..1.000
metalwire: render pipeline 41: name "com.apple.coreanimation.draw.PBGRA_Xfc", vertex "fixed_vert_lpf_spc", fragment "fixed_frag_lpf_cpf", format 80, blend 0, 16 attributes
metalwire: CA set: pipeline 41 "com.apple.coreanimation.draw.PBGRA_Xfc"
metalwire: CA set: scissor 750x1624 at 0,0
metalwire: CA set: fragment buffer buffer 42 offset 0 index 1
metalwire: CA set: vertex buffer buffer 42 offset 240 index 2
metalwire: CA set: vertex buffer buffer 42 offset 336 index 1
metalwire: CA draw: pipeline 41 prim 3 indexed (indices 6, vertices 0, instances 1, base vertex 0)
...
metalwire: render sampler 45: min 0 mag 0 mip 0 address 0/0/0 normalized 1
metalwire: CA set: fragment sampler sampler 45 index 0
...
metalwire: CA set: fragment texture texture 46 index 3
metalwire: CA draw: pipeline 47 prim 3 indexed (indices 6, vertices 0, instances 1, base vertex 0)
metalwire: render: encoder 0 ended: 3 draws, 13 sets
metalwire: render frame 1: 1 encoder(s), 3 draws, 13 sets, target texture 40 750x1624 (decoded and acknowledged; no execution until the WP-2 shaders exist)
...
metalwire: render frame 2: 1 encoder(s), 3 draws, 13 sets, target texture 48 750x1624 (decoded and acknowledged; no execution until the WP-2 shaders exist)
```

No `frame error` line: every object a call names existed, and all 13 sets/3 draws decoded. The
frame's target is CoreAnimation's display IOSurface texture (750x1624 BGRA8, loadAction 2 =
clear, storeAction 1 = store). The three draw pipelines are the ones stage 30 recorded; the wire
also carries the base functions each pipeline was built from.

Regression: `make test` in `host/metalwire/` still passes on lavapipe (2 frames, CRC, CSV, SHM,
PPM/PNG); the synthetic client on the V100 (`metalwire_gen.py 750 1624 --frames 3`) acknowledged
3/3 with `crc_ok=1` on the device chosen by PCI id.

## 3. What the wire now says about CoreAnimation's BootUI frame

The authoritative call list for this boot (the full per-call log is in the boot's serial log):

| | |
|---|---|
| render pass | one color attachment, the display IOSurface texture (750x1624, `BGRA8Unorm`), loadAction clear, storeAction store, clear (0,0,0,0); no depth attachment |
| state | viewport 750x1624 at 0,0; scissor 750x1624 at 0,0; no cull/winding/fill/blend-color/stencil-reference call in this frame |
| pipelines | `PBGRA_Xfc` and `PBGRABsov_TimgXfc_Isrc` from `fixed_vert_lpf_spc` + `fixed_frag_lpf_cpf`; `PBGRA_TlubXf_Isrc` from `std_vert1_lpf` + `luma_boost_lpf`; all with 16 vertex attributes recorded, `blendingEnabled = NO`, color format `BGRA8Unorm` |
| buffers | one CA buffer (the pipeline's constant data), bound as fragment buffer 1, vertex buffers 1/2/3 and fragment buffer 2 with offsets 0..544 |
| textures | the layer's image texture (fragment texture 3) and the 128x1 `R8Unorm` luma-boost ramp (fragment texture 3) |
| draws | 3 indexed triangle draws, 6 indices each, 1 instance, base vertex 0 |

The 6 extra render encoders that CoreAnimation creates earlier (776x24 scratch targets, 0 draws)
are recorded by the plug-in but never reach the wire: their command buffers are not committed in
this boot. They are not part of the frame.

## 4. What this does not establish

- **A picture.** The host decodes and acknowledges; it does not execute the draws. No shader is
  compiled, no Vulkan pipeline is created, no pixel is read back. The IOSurface is still a
  placeholder in the guest (the texture object carries its geometry, not its memory).
- **Frame-rate.** Nothing here is timed, and the acknowledgement is not GPU work.
- **Correct blending/sampling.** `blendingEnabled = NO` on all three pipelines is what the
  descriptor said; whether CoreAnimation's shaders blend internally is a WP-2 question, not
  something this stage can answer.
- **A complete API surface.** Compute, heaps, events, fences and indirect commands are still not
  made; only what CoreAnimation asked for is on the wire.

## 5. GPU selection and isolation

The host picks the V100 by PCI id (`--pci 0000:05:00.0`) and refuses to run when no device matches.
The index is not stable on this host: the other V100 (`0000:04:00.0`) is bound to `vfio-pci` for
another experiment's Windows VM, so `vkEnumeratePhysicalDevices` renumbers every remaining device
(and the lavapipe software device is enumerated too). `--gpu` remains for the synthetic tests but
is not the stage-C rule. The other experiment's services were not touched: its GL replay listener
on 9110 stayed up, and no QEMU instance but the experiment's own (instance 3) was started.

## 6. Reproduce

```sh
# host, from the repository root, on the V100 chosen by PCI id
host/metalwire/metalwire_host --pci 0000:05:00.0 --port 9111 \
    --ram ~/DSH-desktop/mt-work/hakios/qemu/ram.img --frames 0 --shm ""

# guest image: plug-in + injection + the window server in the clients file
guest/metal-plugin/build.sh
guest/ca-metal-inject/build.sh
guest/ca-metal-inject/install.sh on
tools/metalwire/prepare-guest.sh ui
printf 'metal_frame\nmetal_probe\nbackboardd\n' | sudo tee \
  ~/DSH-desktop/mt-work/staging/lib/HakiosMetal.clients >/dev/null
sudo rm -f /tmp/ca-root.apfs
sudo python3 ~/DSH-desktop/iosvm-work/hakios-xnu-kernel/tools/apfs/mkapfs.py -z --plain fw \
  -o /tmp/ca-root.apfs "redwood=$HOME/DSH-desktop/mt-work/staging"
sudo chown "$(id -u):$(id -g)" /tmp/ca-root.apfs
cp /tmp/ca-root.apfs ~/DSH-desktop/mt-work/hakios/userland/root.apfs

MW_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1" tools/metalwire/run-guest.sh ca-metal-c 300
grep -E 'CA render pass|CA set:|CA draw:|CA render encoder: 3' \
  ~/DSH-desktop/mt-work/logs/ca-metal-c/serial.log
```

The host log is whatever path `metalwire_host` was started with; the guest's is
`mt-work/logs/<tag>/serial.log`.

## 7. Files changed

- `host/metalwire/metalwire_protocol.h` — protocol 2 opcodes and structures (render objects, pass,
  calls, draw, sampler, depth-stencil); `R8Unorm`.
- `guest/metal-plugin/HakiosMetal.m` — the render-pass/pipeline/sampler/depth capture, the op
  recorder, the serializer and the commit branch; `R8Unorm` is a wire texture format now.
- `host/metalwire/metalwire_host.c` — render message decoding/validation, pipeline table, the
  decode-only frame acknowledgement, Metal usage bits on textures, `--pci` selection.
- `host/metalwire/metalwire_gen.py` — protocol version 2.
- `host/metalwire/PROTOCOL.md`, `host/metalwire/README.md` — the contract and the new option.

## 8. Next (WP-2's gate, before any shader code)

WP-2 cannot start coding until two questions are answered, and neither is a code question:

1. **Compliance.** Reading `QuartzCore.framework/default.metallib` on the host at run time is the
   same boundary the project already uses for the guest's userland images (Apple assets live on
   the contributor's disk, never in the repository: `PROVENANCE.md`). A translator that consumes
   it, or a hand-written GLSL set derived from its modules, must keep every byte of Apple's
   shaders out of the tree. Option two (hand-written Vulkan GLSL) can avoid the file entirely if
   the semantics are reconstructed from the logged draws; that is the cheaper compliance path.
2. **Route and time box.** `metal2vulkan`'s AIR→SPIR-V claim needs a one-week spike, including its
   licence and PROVENANCE/NOTICE row; the fallback is the hand-written set above. Only the
   pipelines this wire shows are in scope: the three draw pipelines and the two base functions
   per pipeline, no preventive work.

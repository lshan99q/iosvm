# metalwire host — executing a guest Metal frame on Vulkan (blits, and CA's render encoder)

Status: experimental, branch `experiment/ca-metal-device` (WP-4). This is the host half of the
metalwire experiment (see [`PROTOCOL.md`](PROTOCOL.md) and
[ADR-002](../../docs/adr/ADR-002-api-translation.md)); the guest half is
`guest/metal-plugin/`. Nothing here is Apple code, and nothing here is a Metal implementation: a
Metal call became a wire message before this program saw it.

```
 guest Metal device             host (this directory)                         browser
 ─────────────────              ──────────────────────                         ───────
 MTLBlitCommandEncoder   TCP    metalwire_host
   copyBuffer:toTexture  ────►   ┌ buffer ──► HOST_VISIBLE staging
   presentDrawable:             │ vkCmdCopyBufferToImage ──► VkImage (optimal tiling)
   commit/wait                  │ vkCmdCopyImageToBuffer ──► read-back
   750x1624 BGRA8   port 9111   │ CRC-32, CSV row, ack
                                └ glwire SHM ─────────────────────────► webview.py
```

`metalwire_host` is a headless TCP server. It creates one Vulkan device, executes each committed
frame as a real GPU transfer (buffer → image → read-back buffer), checks the read-back CRC against
the guest's, publishes the picture in the same shared-memory format `tools/glwire` already uses, and
answers one acknowledgement per frame. There is no window, no surface and no swapchain.

Since WP-2 it also executes **protocol 2 render encoders**: the wire's CA pipelines are created
as real Vulkan pipelines from the eight hand-written base functions in [`shaders/`](shaders/) (the
four WP-2 layer/luma functions and the four WP-4 blur functions), and the recorded draws are
replayed with the wire's buffers, textures, sampler, viewport and scissor. The blit path is
unchanged and is what `make test` still measures; see
[`../../docs/stage32-ca-metal-execute.md`](../../docs/stage32-ca-metal-execute.md).

WP-3 (protocol 3) closes the two gaps that kept WP-2 from a picture: the per-draw buffer-offset
setters are on the wire, and a texture's `replaceRegion:` bytes are uploaded to the Vulkan image
(including `R8Unorm`). The display texture is a real IOSurface: the guest resolves its physical
segments with `hakios_vtop`, the host writes the executed picture into the guest's surface and
reads it back by reference for the browser. A boot frame now renders as the BootUI Apple logo —
the picture path is in [`../../docs/stage33-ca-metal-picture.md`](../../docs/stage33-ca-metal-picture.md).

WP-4 (protocol 4) is about the desktop, where the wire sees pipeline and texture kinds the BootUI
never did: `MW_CREATE_LIBRARY`/`MW_PIPELINE_LIBRARY_REF` carry a library's hash and entry-function
names (the AIR bytes cannot be compiled here), and the host no longer turns an unknown base function
into a wrong shader — it marks the pipeline unsupported, skips its draws and keeps the frame. A
texture format the host does not implement (and `A8Unorm`, which CoreAnimation's desktop masks use)
costs its draws, not the frame, and `--missing-report` writes the work list. `RGBA16Float` (Metal
115, the colour ramp the desktop boot reported as unimplemented) is a real
`VK_FORMAT_R16G16B16A16_SFLOAT` image, 8 bytes a texel, and its own regression in `make test`. The
background/Dock blur family (`downsample_blur_vert_lpf` + `downsample_4_frag_lpf` /
`downsample_8_frag_lpf` / `narrow_blur_7/11/15/19/23/27_frag_lpf`, the 674 skipped draws of boot
wp4-v4 and the further 1916 of boot wp4-v6's Control Center/Spotlight backdrop) now
has its four hand-written shaders and a fixture that executes all three pipelines; the kernel
weights are `[INFERRED]` binomial kernels and each file says so. The desktop run itself is
[`../../docs/stage34-wp4-desktop.md`](../../docs/stage34-wp4-desktop.md).

## Files

| file | what it is |
|---|---|
| [`metalwire_host.c`](metalwire_host.c) | the server: TCP, Vulkan, SHM publication, CSV/timing, PPM/PNG dumps |
| [`metalwire_gen.py`](metalwire_gen.py) | a synthetic wire client; sends deterministic frames so the host can be tested without the guest |
| [`metalwire_test.py`](metalwire_test.py) | the test behind `make test`; runs the host on lavapipe and checks frames, CRC, CSV, SHM and image files |
| [`Makefile`](Makefile) | `make`, `make test`, `make clean` |
| [`metalwire_protocol.h`](metalwire_protocol.h), [`PROTOCOL.md`](PROTOCOL.md) | the wire contract, shared with the guest |
| [`shaders/`](shaders/) | the eight Vulkan GLSL base functions (four WP-2, four WP-4 blur) and their provenance note |
| [`embed_spirv.py`](embed_spirv.py) | build step: embeds the compiled SPIR-V into `build/wp2_shaders.h` |
| [`wp2_spike.py`](wp2_spike.py) | the WP-2 spike: four composition variants on lavapipe, PASS/FAIL |

## Build

Needs a C compiler, the Vulkan loader and headers (`libvulkan-dev`) and Python 3 (standard library
only) for the tools:

```sh
make            # builds the eight shaders (glslangValidator + spirv-val), then ./metalwire_host
make shaders    # only the SPIR-V and the generated build/wp2_shaders.h
make test       # builds, then runs the lavapipe test (skips cleanly without lavapipe)
make spike      # builds, then runs the four-variant WP-2 spike on lavapipe
make clean
```

It needs `glslangValidator` (and `spirv-val`) at build time; the host itself needs only the Vulkan
loader. The host links `-lvulkan`; it uses no Vulkan device extension. `VK_EXT_pci_bus_info` is only queried
for the log line when the driver exposes it.

## Run

Real run on the second V100 (the intended target; GPU 0 is another experiment's):

```sh
./metalwire_host --gpu 1 --port 9111 --frames 100 \
    --shm /dev/shm/metalwire-frame --csv /tmp/mw.csv

# in another shell, without a guest:
./metalwire_gen.py 750 1624 --frames 100 --port 9111
```

With the guest bundle, start the host first and let the guest connect to `10.0.2.2:9111` (QEMU's
user network maps that to the host's loopback). The listener binds `127.0.0.1` by default; pass
`--bind 0.0.0.0` if the guest reaches it another way.

### Options

| option | meaning |
|---|---|
| `--port PORT` | TCP port (default `9111`; `0` picks a free one and prints it) |
| `--bind ADDRESS` | IPv4 address to bind (default `127.0.0.1`) |
| `--gpu INDEX` | Vulkan physical-device index (default: `$METALWIRE_GPU`, else `1`) |
| `--pci ADDR` | choose the device by PCI id (`DOMAIN:BUS:DEVICE.FUNCTION`, e.g. `0000:05:00.0`); overrides `--gpu`, which is the stage-C rule because a vfio-bound card renumbers the indices |
| `--shm PATH` | publish frames in glwire's format (default: `$METALWIRE_SHM`, else `/dev/shm/metalwire-frame`; `--shm ""` disables) |
| `--frames N` | exit after N acknowledged frames (`0`, the default, runs until stopped) |
| `--once` | shorthand for `--frames 1` |
| `--out DIR` | save every frame as `DIR/frame-%06u.ppm` (P6) |
| `--png-every N` | also save every Nth frame as `DIR/frame-%06u.png` (default `0` = never) |
| `--csv FILE` | write the per-frame timing table |
| `--dump DIR` | write decoded buffers and pipeline/sampler/texture descriptors to `DIR` (debug aid) |
| `--missing-report FILE` | write the pipelines that had no embedded shader, with their library hashes and skipped draw counts (WP-4 work list) |
| `--verbose` | log every wire message, object and Vulkan memory allocation |
| `--help` | usage |

The device is chosen explicitly and never silently falls back. `--pci 0000:05:00.0` picks the
V100 by its address (the stage-C rule); `--gpu 1` means the second entry
`vkEnumeratePhysicalDevices` returns, whatever the driver. At startup the host prints the device
name, type, Vulkan API version, vendor/device ids, PCI bus/device/function (when
`VK_EXT_pci_bus_info` exists), and the chosen queue family. A non-existent index is an error.

## What is measured

The host logs one line per frame and writes the same numbers to the CSV:

```
seq,bytes,recv_us,exec_us,gpu_us,readback_us,publish_us,total_us,crc_ok,fps
```

| column | from → to |
|---|---|
| `recv_us` | `MW_FRAME_BEGIN` arrival → `MW_FRAME_END` arrival (the frame's commands and picture over TCP; not the guest's encode time before the begin) |
| `exec_us` | frame-end handling starts → `vkQueueSubmit` returned (command-buffer recording + submit) |
| `gpu_us` | `vkQueueSubmit` → fence signalled (the V100's transfer work) |
| `readback_us` | fence → read-back bytes copied/CRC-32 computed (CPU) |
| `publish_us` | → glwire SHM updated (and frame files written, when `--out`) |
| `total_us` | `MW_FRAME_END` arrival → acknowledgement sent |
| `fps` | completions in the last up-to-1 s window, as seen by the host |

`bytes` is the picture size (`width * height * 4`). At exit (after `--frames`, `--once`, `SIGINT` or
`SIGTERM`) the host prints per-stage mean/median/min/max. `CLOCK_MONOTONIC` is used throughout.
The CRC is the zlib CRC-32 over the read-back picture; the guest sends the same CRC over the bytes
it believes it sent, and the ack carries `crc_ok`. A mismatch is reported as status
`MW_STATUS_CRC_MISMATCH` (4) and still publishes the frame, because seeing a wrong picture is more
useful than hiding it.

`--out` writes an uncompressed-but-valid PNG (stored deflate blocks, no library) next to the PPM
when `--png-every` asks for it; file writing is part of `publish_us`, so leave `--out` off for
measurement runs.

## Where the picture goes

The SHM file is byte-for-byte the format `tools/glwire/host_glwire.c` publishes and
`tools/glwire/webview.py` reads:

```
uint64 magic = 0x57464C47 ("GLFW"), uint64 seq, uint32 width, uint32 height,
uint32 bytes, uint32 pad, uint64 CLOCK_MONOTONIC ns, then width*height*4 bytes of BGRA
```

So the existing bridge serves a translated frame unchanged, and the existing frame-rate watcher sees
it too:

```sh
# from the repository root; display-only, no QEMU needed
python3 tools/glwire/webview.py --shm /dev/shm/metalwire-frame --port 6082
python3 tools/metalwire/fps-watch.py /dev/shm/metalwire-frame
```

`webview.Picture('/dev/shm/metalwire-frame').snapshot()` was used as a compatibility check while
testing (it returned `(101, 750, 1624, 4872000)` after a 100-frame run).

## Tests

```sh
make test
```

runs the host on **lavapipe** (`VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`, physical
device 0 because the ICD variable hides the GPUs), drives it with `metalwire_gen.py`, and checks:

* two frames acknowledged: frame 0 with `crc_ok=1`, frame 1 with a deliberately wrong guest CRC and
  `crc_ok=0` (status 4) — so both the positive path and the CRC check are exercised,
* the CSV header and one row per frame, with plausible timings,
* the SHM header is glwire's magic/geometry and the published pixels are exactly the picture the
  client sent,
* the PPM files hold that same picture in RGB order, and the PNG files are valid PNGs of the right
  size,
* the v4 fallback: a frame with one pipeline this host has no shader for executes its other draws,
  skips that one, is acknowledged and is listed in `--missing-report`,
* format 115: a 256x1 `RGBA16Float` texture (8 bytes a texel) is created, uploaded through a
  buffer→texture blit and presented; the ack's CRC covers the raw 2048 half-float bytes, the
  `--byte-check` memcmp says the read-back is byte-identical, the PPM/PNG hold the ramp converted
  to 8 bits, no wire error occurs, and an unknown format in the same frame still takes the
  skip-draw fallback,
* the blur family: one frame draws all eight backdrop pipelines (`Tds4`, `Tds8`, `Tnb7`, `Tn11`,
  `Tn15`, `Tn19`, `Tn23`, `Tn27`) into one 64x64 target, each pipeline painting its own 16x16 tile
  of a 4x2 mosaic from the same input texture; the frame executes 8 of 8 draws (0 skipped, empty
  missing report) and every tile matches the box/binomial kernel the shaders document to within
  2/255, including its PNG,
* the material semantics (WP-4 D3/D5): a `PBGRABsov_TmuaXfcn_IsrcCcl_Isup` draw with a non-white
  32x32 support texture, white layer colour and fetch=1 over a cleared target; the host must
  specialise it as `texture 1, fetch 1` and the picture must be the support pattern, not opaque
  white. A host without `Tmua` in its texture list paints (255,255,255) and the test fails,
* the draw dump (WP-4 D3/D5 support): with `MW_DUMP_DRAWS=1` and `MW_DUMP_DRAWS_FILE=<path>` the
  host writes one machine-readable line per executed draw -- `seq`, `frame`, the pipeline name,
  the stream-1 vertex colour half4, the fragment uniform's buffer/offset and first 16 bytes
  (hex and f32), the target-space bbox from the mvp and the draw's vertices, the bound texture and
  the fetch flag. The test asserts the line's format and the labels fixture's known values
  (white colour, bbox 0,0,32,32, texture 90, fetch 0) and that without the switch no file and no
  line appear,
* the cross-pass chain (WP-4 D3/D5): one frame carries two render encoders (source -> texture
  101, texture 101 -> texture 102) and a second frame samples 101 again; both pictures must repeat
  the source. A host that composites every draw into the frame's last target renders the second
  pass from an unwritten texture and the test sees black (0 of 12288 bytes non-zero),
* the label mask (WP-4 D4): a 32x32 A8 glyph mask (Metal's alpha-only label format) is uploaded
  with replaceRegion and drawn over black with a white colour and premultiply on; the test asserts
  every pixel is grey coverage (R == G == B == the mask byte, within 1) and the SHM alpha carries
  the same coverage. A host without the A8 view swizzle writes (coverage, 0, 0) -- red glyphs --
  and the test fails on 992 of 1024 pixels,
* the direction (WP-4 1.1): a 64x64 source whose every row is a different colour is copied 1:1
  through CA's mvp into a render target, then a framebuffer-fetch draw blends 50% over it. The
  test asserts the present read-back, the PPM and the PNG hold source row y at output row y, and
  that the SHM picture and the IOSurface by-reference write-back (a fake `--ram` file) hold the
  same last frame -- 0 tolerance. The same fixture against a host without the negative viewport
  height comes out exactly flipped, so the assertion bites.

No GPU is needed. If the lavapipe ICD is absent the test prints `SKIP` and exits 0, so `make test`
stays green on a machine with only a proprietary driver or no Vulkan at all.

The CI test exercises the BGRA8 (Metal 80) format and the RGBA16Float (Metal 115) path. The RGBA8
(Metal 70) path was checked by hand on lavapipe: the same 100-frame generator with `--format rgba8`
produced `crc_ok=1` frames, the SHM picture was the expected RGBA→BGRA swap, and the PPM held the
right RGB order.

Robustness checks done by hand while building this (not part of `make test`):

* an unknown message type inside a frame makes that frame fail with `MW_STATUS_BAD_COMMAND`, the
  connection survives, and the next frame is still pixel-correct;
* a guest that connects and disconnects before finishing a frame does not crash or hang the host;
  it accepts the next connection and serves it;
* `SIGINT` (and `SIGTERM`) prints the summary and exits 0.

## Measurements

Branch `experiment/metal-translation`, host commit `623fb2d`, 2026-11-21, `-O2` build, one 750x1624
BGRA8 picture per frame, driven by `metalwire_gen.py` (the synthetic client). The generator is
synchronous and computes its picture in Python before each frame, so its own end-to-end rate is
lower than the host's; the host-side per-frame work is the number that describes the translation
path.

### GPU 1 — Tesla V100-SXM2-16GB, PCI `0000:05:00.0`

```sh
./metalwire_host --gpu 1 --port 9111 --frames 100 \
    --shm /dev/shm/metalwire-frame --csv /tmp/mw.csv
./metalwire_gen.py 750 1624 --frames 100 --port 9111
```

Host log:

```
metalwire: device 1: "Tesla V100-SXM2-16GB" (discrete GPU), API 1.4.312, vendor 0x10de device 0x1db1 driver 0x912c8100
metalwire: device 1: PCI 0000:05:00.0 (VK_EXT_pci_bus_info)
metalwire: queue family 0: flags 0x00f, 16 queue(s); compute+transfer
metalwire: summary: 100 frame(s) acknowledged, 100 executed with crc_ok, 1.406 s, 71.1 fps
```

Per-frame breakdown (µs from the CSV; mean / median / min / max):

| stage | mean | median | min | max |
|---|---:|---:|---:|---:|
| recv | 3310 | 3325 | 2276 | 6054 |
| exec | 47 | 32 | 28 | 1507 |
| gpu | 953 | 949 | 826 | 2071 |
| readback | 2461 | 2420 | 2381 | 2697 |
| publish | 696 | 656 | 565 | 3267 |
| total | 4178 | 4085 | 3830 | 8263 |

All 100 frames read back byte-identical to the guest's picture (`crc_ok=1`). The generator's own
summary for the same run: `100/100` acknowledged, 1.849 s, 54.1 fps; encoded mean 13.664 ms
(includes its Python picture and the TCP upload), round-trip mean 18.449 ms.

The saved frame verified by looking at it: `/tmp/mw-frames/frame-000001.png`
(`/tmp/mw-frames/frame-000001.ppm` is the same picture). It shows the red 8-pixel border, the blue →
orange diagonal gradient, exactly the first frame-counter block lit (frame index 1 = `0b1`) and the
yellow marker at its frame-1 position — i.e. the picture the client sent, read back from the V100.

### lavapipe — llvmpipe (LLVM 20.1.2), CPU, API 1.4.318

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json ./metalwire_host --gpu 0 \
    --port 9111 --frames 100 --shm /tmp/mw-lvp-frame.shm --csv /tmp/mw-lvp.csv
./metalwire_gen.py 750 1624 --frames 100 --port 9111
```

`100/100` frames, `100 executed with crc_ok`, 1.438 s, 69.5 fps host-side; total mean 4.528 ms,
median 4.427 ms; recv 3.252/3.266; exec 0.009/0.009; gpu 1.411/1.365; readback 2.412/2.369; publish
0.674/0.656 (mean/median, ms). `make test` (2 frames, one good and one bad CRC) passes in ~3.4 s.

The GPU's transfer is faster than lavapipe's, as expected (gpu 0.95 ms vs 1.41 ms), but the whole
path is dominated by the CPU-side 4.87 MB checks and the 4.87 MB that still crosses TCP — which is
the honest shape of this design, not a GPU bottleneck.

## Render encoders (stage C, WP-2)

A protocol 2 frame that carries `MW_RENDER_*` messages is executed on Vulkan instead of blitted.
The execution half:

* creates one `VkPipeline` per wire pipeline (and per primitive topology) on first use, from the
  embedded shader modules; the wire's base-function names choose the modules, the pipeline
  name chooses the specialization constants (texture, destination fetch, texture matrix);
* composites each draw into its own recorded target: a frame carries one render encoder per CA
  render pass, and the passes target different textures (CA's backdrop/blur chain renders into
  intermediate surfaces that the next pass samples). The first draw into a target honours the
  wire's load action, later draws into the same target load, a texture that a previous pass
  rendered is transitioned with a colour-attachment-write dependency, and the frame's last target
  stays the one read back and published. Feeding every draw to the last target made the backdrop
  chain sample never-written textures (the white Dock/Search band of the boot);
* builds the vertex input from the wire's 16-attribute descriptor (only attributes with a real
  format are bound) and the instance/stride information from the layouts;
* binds the wire's one buffer at the four byte offsets the shaders take as push constants, the
  layer image or the 128x1 R8Unorm ramp at fragment texture 3, the wire sampler at 0;
* translates Metal's `[[color(0)]]` fetch (draws B and C of the BootUI frame) to a sampled copy of
  the colour attachment, because core Vulkan has no framebuffer fetch and a fixed-function blend
  equation cannot express CA's shader-side composition;
* honours `blendingEnabled` (the recorded pipelines all say NO), the viewport, the scissor and the
  render pass's load/store/clear values;
* compensates the one convention difference Vulkan does not share with Metal: Metal's NDC has y
  up and CA's mvp puts a layer's top at NDC y=+1, while Vulkan's viewport maps NDC y down. The
  viewport is submitted with a negative height (`y = ca_y + ca_height, height = -ca_height`), so
  the viewport covers the same framebuffer rectangle but NDC y=+1 lands on its top edge. Negative
  viewport height is core Vulkan 1.1 (VK_KHR_maintenance1), and this path already needs a Vulkan
  1.3 device with dynamic rendering. Doing it at that one point leaves the vertex shaders a
  faithful transcription of CA's mvp and keeps every consumer of the picture -- the colour
  attachment read-back, the PPM/PNG files, the SHM picture and the IOSurface by-reference
  write-back -- in the same framebuffer row order. Without it the whole desktop came out
  vertically mirrored (stage 34's symptom: status bar at the bottom); the BootUI only looked right
  because the Apple logo is nearly symmetric;
* treats CA's `Tmua` base function as a textured material draw: it binds a support texture and
  fetches the destination, and its white layer colour is only the *modulation* -- the support
  texture decides the pixels. Missing it from the texture list specialised the pipeline with
  HAS_TEXTURE=0, so the fragment shader painted the wire's white colour opaque over the fetched
  destination: the flat white Dock/Search band of boot wp4-v12 (dump lines 100/103/310/313, host
  log "texture 0, fetch 1"). A `Tmua` draw without a bound texture keeps the dummy view and the
  previous behaviour;
* creates the image view of an `A8Unorm` texture (CA's glyph/label mask) with a
  `(1, 1, 1, coverage)` component swizzle: the byte is coverage, which Metal's A8 view exposes as
  alpha with a zero colour, so the host's R8 image would otherwise feed the fragment stage
  `(coverage, 0, 0)` and every label came out red (boot wp4-v6's D4). `R8Unorm` (the luma ramp,
  format 10) keeps a plain view;
* can write a per-draw diagnostic dump when `MW_DUMP_DRAWS=1` is in the environment (file from
  `MW_DUMP_DRAWS_FILE`, default `/tmp/metalwire-draws.txt`): one grep-able `draw ...` line per
  executed draw with the vertex colour, the fragment uniform's first 16 bytes and the draw's
  target-space bounding box. The switch is off by default and changes nothing when unset;
* reads the target back after the draws, logs its CRC and a non-zero-byte count, publishes it in
  glwire's SHM format when `--shm` is on, and acknowledges the frame as before.

The synthetic client can produce the same frame shape without a guest:

```sh
./metalwire_host --gpu 1 --port 9131 --shm /dev/shm/wp2
./metalwire_gen.py --render --variant fetch --port 9131
./wp2_spike.py --validation          # all four composition variants, with the validation layer
```

`--variant` is the spike's blending experiment: `fetch` (the CA path: `blendingEnabled = NO` and
the shader composites), `nofetch` (no destination read: replace), `fixed` (fixed-function
source-over), `fetch-noramp` (CA path with the R8 ramp empty, proving the ramp is sampled). See
[`../../docs/spike-wp2-vulkan-glsl.md`](../../docs/spike-wp2-vulkan-glsl.md).

WP-3's wire additions are exercised by the real guest; the synthetic client still speaks v2 and
remains the regression path. The host accepts both versions and echoes the peer's version in the
ack. `--out DIR` now also writes render frames (`frame-%06u.ppm`, plus PNG with `--png-every`) and
`--ram` is opened read/write when possible, which enables the IOSurface write-back; a read-only
RAM file or a guest that cannot resolve the surface falls back to the Vulkan read-back.

`MW_DEBUG_DRAWS=N` (environment) executes only the first N draws of a render frame; it is a
bring-up aid, not a protocol feature.

## Limits, and things that are not true

* **This is not CoreAnimation.** The test client is a synthetic Metal program (device, queue,
  buffer, texture, one blit, present, wait). The port's CoreAnimation has no Metal renderer here (no
  ARM shader compiler / AIR → SPIR-V), so these numbers say nothing about coverage of the system UI.
  They measure the smallest end-to-end version of the planned path.
* **The delivered/measured blit path is not a picture either.** The v1 numbers below are blit
  frames (buffer → texture → read-back). Render-encoder execution is a picture since WP-3 for the
  BootUI frame (Apple logo on black, drawn by CA's three recorded pipelines); the two reconstructed
  base functions (`std_vert1_lpf`, `luma_boost_lpf`) and the wider SpringBoard pipeline set are
  still limits. `MW_FILL_BUFFER` is in the protocol enum but has no wire structure in v1, so the
  host fails it as `BAD_COMMAND` rather than inventing one.
* **Pixels still cross the socket.** Like glwire, the picture bytes travel over TCP; the translation
  is Metal command → Vulkan command, and the host-side cost includes the real GPU round trip. It is
  not a zero-copy display path.
* **v1 is one connection at a time, one frame in flight.** The host accepts one client, answers
  synchronously; pipelining is allowed by the protocol but not exercised.
* **Buffer contents are applied at execution time.** `MW_UPLOAD_BUFFER` copies into host-visible
  staging when it arrives; a frame with several uploads and copies would not reproduce Metal's
  copy-time snapshot. The v1 flow (one upload, one copy per frame) does not need it.
* **Measurements are host-side.** The guest's own timing is a separate log line; the ack carries
  `host_t_done_ns` so the guest can compute its round trip.
* **The first frame is an outlier** (allocation, command-pool and page-cache warm-up); use the
  median for the typical frame.
* `--out` PNGs are valid but uncompressed (~3.6 MB each); they are diagnostic, not a recording.
* The process is a measurement tool: it does not leak by design and cleans up, but it is not
  hardened against a hostile peer (message length is capped at 512 MiB, payloads are validated
  before use).

Two bugs found and fixed by measuring, recorded here because they would otherwise be repeated:

1. The first version preferred `DEVICE_LOCAL` for host-visible buffers. On this driver that is a
   BAR window into VRAM: reading the 4.87 MB read-back from it took ~1.1 s per frame (`readback`
   1123 ms, `publish` 147 ms). Preferring `HOST_CACHED` (type 9, `HOST_VISIBLE|HOST_COHERENT|
   HOST_CACHED`) brought `readback` to 2.4 ms and `publish` to 0.7 ms. The image itself stays in
   device-local memory.
2. Without `TCP_NODELAY` the synchronous guest's small command packets waited on delayed ACKs and
   each frame gained a spurious ~40 ms (`recv` 42 ms). Both ends set `TCP_NODELAY`; the measured
   `recv` is now 2–6 ms.

## License

MIT. See `LICENSE` at the repository root.

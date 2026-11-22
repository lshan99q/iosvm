# metalwire host — executing a guest Metal frame's blits on Vulkan

Status: experimental, branch `experiment/metal-translation`. This is the host half of the
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

## Files

| file | what it is |
|---|---|
| [`metalwire_host.c`](metalwire_host.c) | the server: TCP, Vulkan, SHM publication, CSV/timing, PPM/PNG dumps |
| [`metalwire_gen.py`](metalwire_gen.py) | a synthetic wire client; sends deterministic frames so the host can be tested without the guest |
| [`metalwire_test.py`](metalwire_test.py) | the test behind `make test`; runs the host on lavapipe and checks frames, CRC, CSV, SHM and image files |
| [`Makefile`](Makefile) | `make`, `make test`, `make clean` |
| [`metalwire_protocol.h`](metalwire_protocol.h), [`PROTOCOL.md`](PROTOCOL.md) | the wire contract, shared with the guest |

## Build

Needs a C compiler, the Vulkan loader and headers (`libvulkan-dev`) and Python 3 (standard library
only) for the tools:

```sh
make            # builds ./metalwire_host
make test       # builds, then runs the lavapipe test (skips cleanly without lavapipe)
make clean
```

The host links `-lvulkan`; it uses no Vulkan device extension. `VK_EXT_pci_bus_info` is only queried
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
  size.

No GPU is needed. If the lavapipe ICD is absent the test prints `SKIP` and exits 0, so `make test`
stays green on a machine with only a proprietary driver or no Vulkan at all.

The CI test exercises the BGRA8 (Metal 80) format. The RGBA8 (Metal 70) path was checked by hand
on lavapipe: the same 100-frame generator with `--format rgba8` produced `crc_ok=1` frames, the SHM
picture was the expected RGBA→BGRA swap, and the PPM held the right RGB order.

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

## Limits, and things that are not true

* **This is not CoreAnimation.** The test client is a synthetic Metal program (device, queue,
  buffer, texture, one blit, present, wait). The port's CoreAnimation has no Metal renderer here (no
  ARM shader compiler / AIR → SPIR-V), so these numbers say nothing about coverage of the system UI.
  They measure the smallest end-to-end version of the planned path.
* **No shaders, no render passes.** Only Metal blit copy buffer → texture, one presented texture and
  the read-back. There is no `MTLRenderCommandEncoder`, pipeline, or draw. `MW_FILL_BUFFER` is in
  the protocol enum but has no wire structure in v1, so the host fails it as `BAD_COMMAND` rather
  than inventing one.
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

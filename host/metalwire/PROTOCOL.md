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

# guest/metal-plugin — the metalwire guest side

Status: experimental, branch `experiment/metal-translation`. This directory is L3 of
`docs/architecture.md` and the first real implementation of `docs/adr/ADR-002-api-translation.md`:
a Metal device that lives in the guest and turns Metal commands into `metalwire` protocol messages
for the host's Vulkan translator (`host/metalwire/`). The wire contract is
`host/metalwire/metalwire_protocol.h`; the conventions are `host/metalwire/PROTOCOL.md`.

The device is a stand-in, not an emulator: it executes nothing. Buffers are guest memory, textures
are descriptors (the pixels are the host's), and a frame's picture leaves the guest as the bytes of
one buffer plus one blit command. That is the whole translation this experiment measures; the
shaders are not compiled and nothing is drawn with them; since stage 30 a render encoder is here
that records CoreAnimation's draws (see "CoreAnimation on this device" and "Deliberately
unimplemented").

## Files

| File | What it is |
|---|---|
| `HakiosMetal.m` | the bundle: registers `HakiosMetalDevice` (`_MTLDevice` subclass, `MTLDevice` + `MTLDeviceSPI`) and its object classes; the wire encoder; the ack path |
| `mw_wire.c`, `mw_wire.h` | the socket client, message framing, timed connect, ack read, CRC-32; shared by the bundle only |
| `mw_metal_extras.h` | the one extension: `-newDrawableWithTexture:` / `MWRemoteDrawable.texture`, because there is no `CAMetalLayer` to get a drawable from in this port |
| `metal_frame.m` | the Metal test client: `metal_frame [-n FRAMES] [-s SECONDS]` |
| `metal_probe.m` | the diagnostic: creates the device and a queue and prints what Metal answered; sends nothing |
| `Info.plist` | the bundle's Info.plist (`CFBundleExecutable=HakiosMetal`, `NSPrincipalClass=HakiosMetalDevice`) |
| `build.sh` | builds all of it with `xnu-clang` and the iPhoneOS 16.5 SDK; prints `file`/`otool -l` |

Build products go to `build/` (git-ignored): `build/HakiosMetal.bundle/` plus `build/metal_frame`,
`build/metal_probe` and `build/HakiosMetal.clients`.

## Build

The HakiOS cross toolchain is the only hard dependency; it is read, never written:

```sh
cd guest/metal-plugin
./build.sh
```

`build.sh` sources `$HAKIOS_DIR/xnu-env.sh` (default
`/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel`), exports
`LD64_LLD=/usr/bin/ld64.lld-18`, and compiles with
`xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1`. Override `HAKIOS_DIR`, `SDK`,
`CC`, `LD64_LLD` or `THEOS` if the layout differs. The wire header is not copied: `-I
host/metalwire` makes both ends include the repository's `metalwire_protocol.h`.

What it prints, and must keep printing (this is the build proof):

```
build/HakiosMetal.bundle/HakiosMetal: Mach-O 64-bit arm64 bundle, flags:<NOUNDEFS|DYLDLINK|TWOLEVEL>
build/metal_frame:                    Mach-O 64-bit arm64 executable, flags:<NOUNDEFS|DYLDLINK|TWOLEVEL|PIE>
build/metal_probe:                    Mach-O 64-bit arm64 executable, flags:<NOUNDEFS|DYLDLINK|TWOLEVEL|PIE>
...
platform 2        # iOS
sdk 16.5
minos 16.5
LC_LOAD_DYLIB  Foundation, CoreFoundation, libobjc, Metal, libSystem
```

The bundle's `LC_BUILD_VERSION` is platform 2 (iOS), minos 16.5; its module initializer
(`__mod_init_func`) is where `mw_plugin_load`, the constructor, is called from.

## Installing into a root image

The kernel's `IOAcceleratorES` service names `MetalPluginName=HakiosMetal` and
`MetalPluginClassName=HakiosMetalDevice` when boot-arg `metal=N` (N ≠ 0); Metal then loads
`/System/Library/Extensions/HakiosMetal.bundle`. So the image needs:

```sh
# the bundle (binary + Info.plist), beside where the port keeps its libraries
mkdir -p /redwood/lib/HakiosMetal.bundle
cp build/HakiosMetal.bundle/HakiosMetal   /redwood/lib/HakiosMetal.bundle/
cp build/HakiosMetal.bundle/Info.plist    /redwood/lib/HakiosMetal.bundle/
ln -sfn /redwood/lib/HakiosMetal.bundle /System/Library/Extensions/HakiosMetal.bundle

# the process gate: this experiment's client (one name per line, "*" for every process)
cp build/HakiosMetal.clients /redwood/lib/HakiosMetal.clients

# the clients themselves
cp build/metal_frame build/metal_probe /redwood/bin/
```

This replaces the HakiOS tree's `HakiosMetal.bundle`; do not install both. The clients file is
looked for beside the bundle executable as opened (`/System/Library/Extensions/HakiosMetal.clients`)
and, through `realpath`, where the bundle really is (`/redwood/lib/HakiosMetal.clients`) — either
location works. `metal_frame` and `metal_probe` are also allowed by name outright, so the
experiment runs even if the clients file is missing.

Boot with `metal=1` (only enough that the accelerator names the plug-in; the level controls which
processes see the device) and start the host translator first:

```sh
# on the Linux host
host/metalwire/metalwire_host --gpu 1 --port 9111 --shm /dev/shm/metalwire-frame --frames 0
# then in the guest
/redwood/bin/metal_probe
/redwood/bin/metal_frame -n 100
```

Environment overrides, read by the bundle once per process: `METALWIRE_HOST` (default `10.0.2.2`),
`METALWIRE_PORT` (default `9111`), `METALWIRE_OFF=1` (no socket at all), `METALWIRE_CONNECT_MS`
(default 2000), `METALWIRE_ACK_MS` (default 15000).

## The object graph, and exactly which Metal API is covered

Runtime classes (all made with `objc_allocateClassPair`; see "Private classes and protocols"):

```
HakiosMetalDevice        subclass of _MTLDevice     MTLDevice, MTLDeviceSPI
 └─ newCommandQueue
HakiosMetalQueue         subclass of _MTLCommandQueue  MTLCommandQueue (+ every MTLCommandQueue* SPI)
 └─ commandBuffer
HakiosMetalCommandBuffer subclass of _MTLCommandBuffer  MTLCommandBuffer (+ its SPI family)
 └─ blitCommandEncoder
HakiosMetalBlitEncoder   subclass of _MTLCommandEncoder MTLBlitCommandEncoder, MTLCommandEncoder
 └─ copyFromBuffer:...toTexture:...           one rectangle per frame
 └─ presentDrawable:                           one texture per frame
HakiosMetalBuffer        subclass of _MTLResource   MTLBuffer, MTLResource (+ SPI)
HakiosMetalTexture       subclass of _MTLResource when this system has one, else NSObject
                         MTLTexture, MTLResource (+ SPI)
HakiosMetalLibrary       subclass of _MTLLibrary    MTLLibrary (+ SPI)
HakiosMetalFunction      subclass of _MTLFunction   MTLFunction (+ SPI)
 └─ newSpecializedFunctionWithDescriptor:...          placeholders, constants logged
HakiosMetalPipelineLibrary subclass of _MTLPipelineLibrary  MTLPipelineLibrary (+ SPI)
 └─ newRenderPipelineStateWithName:options:reflection:error:   placeholder, name logged
HakiosMetalRenderPipelineState  subclass of _MTLRenderPipelineState  MTLRenderPipelineState
HakiosMetalDepthStencilState    subclass of _MTLDepthStencilState
HakiosMetalSamplerState         subclass of _MTLSamplerState
HakiosMetalRenderCommandEncoder subclass of _MTLRenderCommandEncoder when present, else NSObject
 └─ 29 setters + drawPrimitives:/drawIndexedPrimitives: record the calls; serialized at commit
    (protocol 2, stage C)
MWDrawable               plain NSObject  MTLDrawable + MWRemoteDrawable (the extension)
```

Implemented (anything else a real Metal runtime asks is logged once and answered with zero/nil):

- device: `initWithAcceleratorPort:`, `name`, `registryID`, `maxBufferLength`,
  `hasUnifiedMemory`, `supportsFeatureSet:`, `supportsFamily:`, `isLowPower`/`isRemovable`/
  `isHeadless` (NO — CoreImage reads them), `dedicatedMemorySize` (0), `sharedMemorySize`,
  `newCommandQueue`, `newCommandQueueWithMaxCommandBufferCount:`,
  `newBufferWithLength:options:`, `newBufferWithBytes:length:options:`,
  `newTextureWithDescriptor:`, `newDrawableWithTexture:` (ours).
- queue: `device`, `label`/`setLabel:`, `setCompletionQueue:` (no-op), `commandBuffer`,
  `commandBufferWithUnretainedReferences`.
- command buffer: `commit`, `enqueue` (no-op), `waitUntilCompleted`, `waitUntilScheduled` (no-op),
  `status`, `error`, `commandQueue`, `blitCommandEncoder`, `presentDrawable:`,
  `presentDrawable:atTime:`, `presentDrawable:afterMinimumDuration:`, `addCompletedHandler:`,
  `addScheduledHandler:`, `GPUStartTime`/`GPUEndTime` (0), `label`/`setLabel:`.
- blit encoder:
  `copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:destinationSlice:destinationLevel:destinationOrigin:`
  (and its `options:` form), `endEncoding`, `synchronizeResource:` (no-op),
  `label`/`setLabel:`.
- buffer: `contents`, `length`, `resourceOptions`, `storageMode`, `cpuCacheMode`,
  `hazardTrackingMode`, `allocatedSize`, `gpuAddress` (0), `didModifyRange:`, `setPurgeableState:`,
  `device`, `label`/`setLabel:`.
- texture: `width`, `height`, `depth`, `arrayLength`, `mipmapLevelCount`, `sampleCount`,
  `pixelFormat`, `textureType`, `usage`, `storageMode`, `resourceOptions`, `allocatedSize`,
  `isFramebufferOnly` (NO), `isShareable` (NO), `device`, `label`/`setLabel:`.
- drawable: `present`, `presentAtTime:`, `presentAfterMinimumDuration:`, `addPresentedHandler:`,
  `presentedTime` (0), `drawableID`, `texture` (ours).
- libraries/functions (stage 30): `newLibraryWithURL:/Data:/File:`, `newDefaultLibrary`,
  `newFunctionWithName:`, `newFunctionWithDescriptor:error:` (the descriptor API CoreAnimation
  uses), `newSpecializedFunctionWithDescriptor:...` including the `…DescriptorInternal:…` spelling,
  `functionNames`, `name`, `functionType`, `type`, `installName`, labels.
- pipeline library (stage 30): `newPipelineLibraryWithFilePath:error:` (the file is never read),
  `setDisableRunTimeCompilation:` (stored), `newRenderPipelineStateWithName:options:reflection:error:`
  (every name accepted and logged, one placeholder state per name).
- pipeline states (stage 30): `newRenderPipelineStateWithDescriptor:error:` and its options/
  reflection variants; every descriptor is answered with a placeholder, the vertex/fragment
  function names are logged. `newDepthStencilStateWithDescriptor:` and
  `newSamplerStateWithDescriptor:` are placeholders too.
- render encoder (stage 30, wired in stage 31): `renderCommandEncoderWithDescriptor:`, 29 setters
  (`setRenderPipelineState:`, `setVertexBuffer:offset:atIndex:`, `setFragmentTexture:atIndex:`,
  `setViewport:`, …), `drawPrimitives:…`, `drawIndexedPrimitives:…`, `endEncoding`. Each call is
  copied into a plain C record; at commit the command buffer serializes the render pass, every set
  and every draw as protocol-2 render messages (`host/metalwire/PROTOCOL.md`). The host decodes
  them; it does not paint yet (WP-2).
- dealloc: classes whose superclass is a Metal private base free their own state and skip the
  base's `-dealloc` (it dereferences dispatch objects these stubs never created — `_MTLCommandQueue`
  `_submittedGroup`, `_MTLLibrary` `_cacheEntry`); that base allocation is deliberately leaked.
  Two stage-4.1 additions soften that: the command buffer, the two encoder classes and the buffer
  class are handed out from fixed instance pools (the pool holds the last reference and the
  per-frame state is rebuilt; buffers match on their length and are handed out again once the
  frame that used them has been acknowledged -- CoreAnimation keeps its own references to the
  per-frame 256 KiB buffers and never releases them (boot wp4-soak-fix2: 750
  `newBufferWithLength:262144` calls, one per frame), so the pool cannot wait for `-retainCount`
  to drop and tracks frame availability instead; `MW_BUFFER_LOG=1` prints rate-limited hit/miss
  lines), and the 256 KiB render buffer is one host-side buffer refreshed by the upload instead of
  one per frame),
  and the command queue's leak has a *safe release* probe — `/redwood/etc/metal-base-dealloc`
  creates the `_submittedGroup` dispatch group and releases it balanced, logging pre/post, without
  ever calling the inherited `-dealloc` chain (boot wp4-soak-fix ran that chain once: the guest
  stopped rendering, 0 frames in 1,500 s, so it is recorded as unusable). The marker is off by
  default. Both decisions are counted per class (`alloc`/`dealloc`/`base_dealloc`/`base_skipped`/
  `reused`/`safe_release`) and dumped to the serial console on SIGTERM/SIGINT, at exit and every
  600 committed frames, so a short soak can read the accounting too.
- census (`/redwood/etc/metal-census` in the root image): every method the stub classes inherit
  from their private base is trapped and logged once, answered with zero; the device and the queue
  are excluded, because trapping `_MTLDevice` breaks Metal's own device registration. Diagnostic
  only, off by default.

Limits on purpose: one 2D BGRA8Unorm/RGBA8Unorm texture per frame, one buffer→texture copy per
frame, one present per frame, byte-for-byte `copyFromBuffer:...toTexture:` only. A second copy, a
`fillBuffer:range:value:` or a `copyFromBuffer:...toBuffer:` sets the command buffer's error (the
frame is not sent) and logs why.

## The wire, one frame at a time

The connection is lazy: nothing is opened when the device is made (so `metal_probe` works with the
host down), and a connect is bounded by 2 s and retried at most every 5 s, so a missing host can
never hang a process. If the host is unreachable the frame is a no-op that reports an `NSError`; it
is never a crash and never a block beyond the timeouts.

```
(first frame that needs it)  TCP connect, MW_HELLO{version, pid, "metal_frame"}
MW_FRAME_BEGIN{seq, guest_t_begin_ns}          guest_t_begin = when the command buffer was made
  MW_CREATE_BUFFER{id, options, length}        once per object per connection, first frame using it
  MW_CREATE_TEXTURE{id, w, h, format, usage}   ditto
  MW_UPLOAD_BUFFER{id, offset=0, length} + bytes   the buffer as it is at commit
  MW_COPY_BUFFER_TO_TEXTURE{buffer, texture, offset, bytesPerRow, ..., width, height, origin}
  MW_PRESENT{texture_id}
MW_FRAME_END{seq, encoded_ns, crc, width, height, bytes_per_row}
                                          → host copies on the V100, reads back, publishes, checks CRC
                                          ← MW_ACK{seq, status, crc_ok, ...}
```

An `MW_UPLOAD_BUFFER` is one message whose `mw_msg.length` covers the structure **and** the bytes
after it. The host reads exactly `length` bytes before looking for the next header, so a body sent
after a short header desynchronises the stream and the host closes the connection — the first guest
boot's trans1 run found exactly that (the host answered `Broken pipe`, then the client spent 200
frames failing to reconnect); the framing was corrected and trans2/demo1 then acknowledged every
frame. The ack is the other direction and is read tolerantly (below).

`encoded_ns` is frame-begin → commit (the recording cost). `waitUntilCompleted` blocks for the ack
(15 s timeout); the ack's `status`/`crc_ok` become the command buffer's `error`. The guest CRC is
computed over the bytes the blit reads — `height` rows of `width × 4` bytes, skipping
`sourceBytesPerRow` padding — so it can be compared with the host's read-back of the picture.

Frame ownership is manual: `newCommandQueue`, `newBufferWith...`, `newTextureWith...`,
`newDrawableWithTexture:` return +1 objects (Metal's "new" convention; the ARC clients release
them), `commandBuffer`/`blitCommandEncoder` return autoreleased ones, and a frame at commit holds
one reference to its command buffer until its ack is read.

The ack is read tolerantly. The protocol note says every message is a `mw_msg` frame, while
`struct mw_ack` carries its own magic; the host may send either a raw `struct mw_ack` or a
`mw_msg` frame whose payload is one, and `mw_recv_ack` accepts both. (The guest→host direction is
unambiguous: everything above is `mw_msg`-framed.)

## Private classes and protocols, and why

Metal's discovery chain (worked out in the HakiOS tree's `docs/ru/metal.md` and implemented by its
`userland/src/HakiosMetal.m`, which this file uses as a reference but does not copy) requires the
class it instantiates to be a **subclass of the private `_MTLDevice`**: `MTLAddDevice` checks
`isSubclassOfClass:_MTLDevice`, and then asserts conformance to `@protocol(MTLDevice)` and to the
private `@protocol(MTLDeviceSPI)`. Neither protocol is declared by `_MTLDevice` itself, and
`_MTLDevice` is not in the SDK's `Metal.tbd`, so:

1. the constructor that runs at bundle load (only in an allowed process) looks `_MTLDevice` up with
   `objc_getClass`, makes `HakiosMetalDevice` with `objc_allocateClassPair`, declares `MTLDevice`
   and `MTLDeviceSPI`, adds the methods above, and registers the class. A public protocol is taken
   from `objc_getProtocol(name)` first — the running framework's own object, which is what the
   first boot registered the working device with — and only if that returns NULL (Darwin's
   optimized shared cache leaves some protocols unrealized until a compiled reference exists) the
   compile-time `@protocol(...)` reference is used; `MTLDeviceSPI` has no SDK declaration and is
   found by name. The log says which one a class got, and which private bases the system has;
2. every other object class is made the same way on Metal's private base where the running system
   has one — `_MTLCommandQueue`, `_MTLCommandBuffer`, `_MTLCommandEncoder`, `_MTLResource`,
   `_MTLTexture` — and on `NSObject` where it does not. The class registered is logged, so the boot
   log says which bases this system actually has (`the class HakiosMetalBuffer is registered (a
   subclass of _MTLResource)` or "... NSObject: this system has no _MTLResource");
3. all classes declare the public protocol of the object plus its SPI family when present
   (`MTLCommandQueueSPI` is the one that carries `setCompletionQueue:`, which the window server
   sends in the HakiOS runs);
4. a message that is in no protocol the class declares is looked up in every protocol loaded in the
   process, and if a signature is found the message is answered with a zero of the right size and
   logged once ("asked and not implemented: -foo"). Only a selector with no signature anywhere
   falls through to `_MTLDevice`'s/`NSObject`'s `methodSignatureForSelector:`, and that case is
   logged too. This is what makes it safe for a live Metal runtime to ask a stub device things the
   test client never will.

Difference from the HakiOS reference: the reference only needed a device that exists; this bundle
implements the one remote path end to end, so it has real state per object, real command recording,
a synchronous wire and an ack. The reference's `HakiosMetal.clients` lookup uses the bundle's
`dladdr` path only; this one also tries the `realpath`, because this port installs the bundle as a
symlink at `/System/Library/Extensions` pointing into `/redwood/lib`. And the reference never made a
drawable; `-newDrawableWithTexture:` is ours, not Metal's.

## Deliberately unimplemented

- **Compilation and execution.** Libraries, functions and pipeline states are placeholders: no
  AIR module is read and no shader exists on the host. Compute encoders, heaps, events, fences,
  argument buffers and indirect command buffers are not made. A render encoder now records and
  serializes (protocol 2); the host decodes the stream and acknowledges the frame, but executes
  nothing until stage C's WP-2 shaders exist.
- **A real render target.** `newTextureWithDescriptor:iosurface:plane:` accepts the display's
  IOSurface and returns a placeholder texture that does not wrap it; pixel formats other than
  2D BGRA8Unorm/RGBA8Unorm are metadata. This is why CoreAnimation's Metal frames contain no
  pixels yet (stage 30, §6.3).
- **Async/completion-handler variants** of the library/function/pipeline APIs beyond the ones listed
  above.
- **The base-dealloc leak** described above: the per-frame objects no longer multiply (command
  buffer, encoders and buffers come from instance pools) and the queue's dispatch group has a
  verified safe-release probe, but the inherited `-dealloc` chain of every class stays uncalled —
  running the queue's chain once stopped the guest from rendering (t30), so each class still keeps
  Metal's own allocation for its objects. The lifecycle counters are the soak's evidence of what is
  still growing.
- `newBufferWithBytesNoCopy:length:options:deallocator:`, IOSurface-backed textures, texture
  read/write (`getBytes:`/`replaceRegion:`) — nil.
- Buffers are plain anonymous memory; there is no GPU address and no guest-side GPU work.
- One connection, one frame in flight; no pipelining across frames.
- No swapchain/surface: the host publishes the read-back picture in the glwire shm format and
  `tools/glwire/webview.py` serves it.

## CoreAnimation on this device (stage 30)

`docs/stage30-ca-metal.md` and `docs/stage31-ca-metal-wire.md` are the records: with
CoreAnimation's Metal renderer shown the device, the plug-in gets past the renderer-library assert
(the URL is answered by the injected library in `guest/ca-metal-inject/`), past the inherited
`_MTLDevice`/`_MTLFunction`/`_MTLPipelineLibrary` implementations, records CoreAnimation's own
draw calls (three indexed triangles per frame) and, since stage 31, serializes the whole render
encoder on protocol 2: the render pass's target/load/store/clear, every set, every draw, and the
pipeline states' base functions. The host decodes and validates them and acknowledges the frame.
It is not a picture yet: no shader is compiled, the IOSurface is not wrapped, and nothing is
painted (WP-2).

## The test client and its picture

`metal_frame` fills a 4,872,000-byte buffer with a deterministic picture each frame (horizontal
red ramp, two moving colour channels, a dark ruler every 128 rows, a 16-block binary frame counter,
a 4-pixel white border — documented at the top of `metal_frame.m`), records the blit and the
present, commits and waits, and prints one line per frame plus a summary:

```
metal_frame: frame=0 fill_us=... encode_us=... commit_us=... wait_us=... total_us=... status=4 error=-
...
metal_frame: summary: frames=100 ok=100 failed=0 elapsed_us=... fps=... encoded_us_mean=... round_trip_us_mean=...
```

The picture is deterministic, so the frame CRCs can be checked without a guest: fill a
4,872,000-byte BGRA8 buffer as the comment in `metal_frame.m` says and take zlib's CRC-32; frame 0
is `0x16b0a1b3`, frame 1 `0xadece4e2`, frame 2 `0x252d8e2c`. Those are exactly the values the guest
sent in the first boot and the V100 host read back (`mt-work/logs/demo1/host.log`, `crc ok`), which
is what ties the documented picture, the guest's CRC and the host's read-back together.

`metal_probe` prints the device's answers (`name`, `registryID`, `maxBufferLength`,
`hasUnifiedMemory`, feature sets, whether the drawable extension is there, a queue's label) and does
not touch the wire — it is the first thing to run when `metal_frame` says
`MTLCreateSystemDefaultDevice() returned nil`.

## What the first guest boot showed

The lead booted a root image carrying this bundle and the two clients (QEMU instance 3, boot-arg
`metal=1`, clients file naming `metal_frame`; logs under `mt-work/logs/`). The device chain worked
first time:

```
metalwire: metal_frame is this experiment's client: the device is shown to it
metalwire: the class HakiosMetalDevice is registered (a subclass of _MTLDevice; declares MTLDevice and MTLDeviceSPI)
metalwire: a device for the accelerator port 3075 is made; it translates Metal to 10.0.2.2:9111
metalwire: asked and not implemented: -featureProfile (returns 8 bytes, type Q; answered with zero)
metalwire: newCommandQueue: a queue that records frames for the host
metalwire: newBufferWithLength:4872000 options:0x0: buffer 2 (4872000 bytes, sent on first use)
metalwire: newTextureWithDescriptor: texture 3 (750x1624 format 80 usage 0x5; pixels on the host)
metalwire: connected to 10.0.2.2:9111
metalwire: frame 1 seq 1: encoded 1109 us, wait 557188 us, host 750x1624 crc 0x16b0a1b3 matches
...
metalwire: summary: 60 frames, 60 ok, 0 failed (0 crc mismatches), 292320000 bytes sent;
           encoded mean/min/max 126/53/1109 us; wait mean/min/max 495337/472798/557188 us
metal_frame: summary: frames=60 ok=60 failed=0 ... fps=2.00
```

`metal_probe` printed `ok` with the host down (0 frames, no wire traffic), and the host translator
(the other half of the experiment) acknowledged every frame with `crc_ok=1` on the V100. What the
log also settled: this system **has** `_MTLResource`, `_MTLCommandQueue`, `_MTLCommandBuffer` and
`_MTLCommandEncoder` (the classes say so), and **has no** `_MTLTexture` (the texture class is on
`NSObject`, as the HakiOS notes said); Metal asks the device nothing but `-featureProfile`,
`-name`, `-registryID`, `-maxBufferLength`, `-newCommandQueue`, `-newBufferWith...`,
`-newTextureWithDescriptor:` and our drawable/queue methods — the unknown-selector stub was needed
for exactly one selector. The frame cost breakdown (guest `commit_us` ≈ 480 ms) says the 4.87 MB
upload over QEMU's user-mode TCP is the bottleneck, not the recording (`encoded_us` ≈ 0.1 ms) and
not the host's execution (≈ 6-12 ms per frame on the V100).

## Remaining risks

1. **The upload framing is the wire's one historical trap.** A body inside or outside the message
   length is ambiguous in `host/metalwire/PROTOCOL.md`; both ends now read it the same way (inside), and the
   guest→host direction has been booted. Any change to either end must keep that.
2. **Protocol conformance objects.** The device registration uses the framework's `MTLDevice`
   object when it exists (the tested path) and a compiled `@protocol()` copy otherwise; Darwin's
   unrealized protocols mean `objc_getProtocol` cannot be the only route for the public protocols
   (MTLCommandQueue and MTLBlitCommandEncoder were the two that failed by name in the first boot).
3. **The host must be up before the guest's first frame.** A missing host is a visible failure in
   `metal_frame`'s output (200 no-op frames in trans1's first attempt), never a hang; the connect
   is bounded and retried every 5 s.
4. **The process gate.** The device is registered only when `getprogname()` is `metal_frame`,
   `metal_probe`, `metal_test`, or listed in the clients file, or when the boot-arg level shows it
   to the process. Boot processes are not shown it, so the port's OpenGL path is untouched.
5. **The picture's meaning.** Even when the CRC matches, the frame proves Metal→Vulkan translation
   of *this* tiny API subset, not CoreAnimation. The experiment's honest claim is the channel and
   its cost, not the desktop.

License: MIT (see the SPDX line at the top of every file).

# stage 30: CoreAnimation's Metal renderer — the walls, and the first recorded draws (experiment)

> Status: **measured. CoreAnimation initialises a Metal renderer and records its first draw calls
> through this port's device plug-in; no picture is produced yet.** Branch `experiment/metal-translation`
> (this stage's work is also on `experiment/ca-metal-device`), 2026-11-22. Every number and log line
> below was taken on this host in this session. The design it continues is
> [`docs/research/metal-translation-feasibility.md`](research/metal-translation-feasibility.md),
> the measurement it extends is [`docs/stage29-metal-translation.md`](stage29-metal-translation.md),
> and the static analysis it leans on is
> [`docs/research/ca-metallib-url.md`](research/ca-metallib-url.md).

## 1. The question, and what the branch already had

The engineering plan this stage was asked to carry out (`ENG-METAL-001`, draft) begins from "the
port has no `_MTLDevice` plug-in yet". That premise is stale for this branch: stage 29 wrote the
plug-in, and it already answers `-initWithAcceleratorPort:`, a queue, command buffers, buffers,
textures, a blit encoder and the metalwire wire. What the port did **not** have was any CoreAnimation
call reaching that plug-in: with the window server shown the device (`metal=3`), CoreAnimation's
Metal renderer stops before its first Metal call, on

```
Assertion failed: (metallib_url != nil), function MetalContext, file ogl-metal.mm, line 3308.
```

So stage 30's question was narrower than the plan's Stage A: **can CoreAnimation's Metal renderer be
walked past that assertion — and past the walls behind it — until it encodes draws on this device,
without touching Apple's code and without shipping an Apple binary?** It can, at "record only": the
calls are captured, no pixels are produced. That is Stage B of the plan; Stage C is not attempted
(§6).

Configuration (all of it, because a result without it is not reproducible):

| | |
|---|---|
| branch / worktree | `experiment/metal-translation`, `/home/lshan/DSH-desktop/iosvm-metal` |
| guest run tree | `/home/lshan/DSH-desktop/mt-work` (own ram.img, own root image) |
| QEMU | instance 3 (monitor 4474, QMP 4475), `QEMU_SMP=1 QEMU_NET=1 QEMU_RAM_GB=7 QEMU_TB_MB=1536`, one vCPU |
| guest cmdline | `cpus=1 fb_beat=0 fbscale=2 metal=1` with `/redwood/lib/HakiosMetal.clients` = `metal_frame`, `metal_probe`, **`backboardd`** (the last boot; early boots used `metal=3`) |
| why `metal=1` + clients | with `metal=3` the device is shown to every process: `mediaserverd` then dies on `-[CMMTLCommandQueue setBackgroundGPUPriority:]`, which is noise and a second restart loop. The clients file shows it to the window server only. The kernel still advertises the plug-in for any non-zero `metal=` |
| injected library | `guest/ca-metal-inject/libcainject.dylib` into `backboardd` (the root image's `etc/inject.conf`, the port's own insertion mechanism) |
| placeholder resource | `/redwood/share/ca-default.metallib`, 64 bytes of this project's own; never parsed |
| host GPU | **none in this path.** The host Vulkan translator is not involved; the guest records calls and nothing executes. The other experiment on this host had the other V100 bound to its VM; this stage therefore did not touch a GPU at all |
| build | `guest/metal-plugin/build.sh` (HakiOS cross toolchain, read-only); `guest/ca-metal-inject/build.sh`; both exit 0 with no warnings |

## 2. The walls, in the order CoreAnimation hit them

Each row is one boot; the log is named. "Fix" is what the plug-in or the injected dylib answers;
none of it patches Apple code.

| # | what CA did | what happened | evidence | fix |
|---|---|---|---|---|
| W1 | `CA::OGL::new_metal_context()` asks `QuartzCore.framework` for resource `default`/`metallib` | assert line 3308, `backboardd` aborts and restarts every ~30 s | `ca-metal3-baseline`: `Assertion failed: (metallib_url != nil) ...` | injected `-[NSBundle URLForResource:withExtension:]` hooks (the two- and three-argument forms) return `/redwood/share/ca-default.metallib` |
| W2 | hands that URL to `-[MTLDevice newLibraryWithURL:error:]` | Apple's `_MTLDevice` implementation reads our placeholder, `[Metal] Metal failed to load library: Invalid library file`, abort | `ca-metal3-inject1`: the NSBundle line above + `Metal failed to load library: Invalid library file` | plug-in overrides `newLibraryWithURL:error:` (and data/file/default variants) and returns a placeholder library; the URL's bytes are never read |
| W3 | asks for `default`/`pipelinelib` (exists on disk) then `newPipelineLibraryWithFilePath:error:` | Apple's implementation deserialises the pipelinelib on a fake device, SIGSEGV at 0x10; `pc 0x181a3ca3c` = Metal +0x2aa3c | `ca-metal3-lib1`: signal 11 line | plug-in overrides `newPipelineLibraryWithFilePath:error:` |
| W4 | sends `-[MTLPipelineLibrary setDisableRunTimeCompilation:1]` on that placeholder | Apple's implementation stores the flag and dereferences a real ivar; SIGSEGV at 0x10, `pc 0x181ab9f7c` = that method (Metal +0xa7f7c) | `ca-metal3-pipelib1` signal 11; resolved statically in `docs/research/ca-metallib-url.md` §10 | plug-in implements `setDisableRunTimeCompilation:` |
| W5 | first frame teardown: an autorelease pool releases the command queue | Apple's `-[_MTLCommandQueue dealloc]` releases `_submittedGroup` at ivar 0xb0 **without a nil check**; `dispatch_release(NULL)` faults in libdispatch (`pc 0x18db56628`) | `ca-metal3-shim1`: register dump `x1 = -dealloc`, and the static resolution of Metal +0x25930 = `_MTLCommandQueue dealloc` | plug-in's dealloc rule: for classes whose superclass is a Metal private base, free our state and `object_dispose(self)` **without** sending the base's `-dealloc` (recorded limitation: the base's own allocation is leaked; one boot's worth is kilobytes) |
| W6 | loads the base shader functions from the library and specialises them: `fixed_frag_lpf_cpf`, `fixed_vert_lpf_spc`, … | `-[HakiosMetalFunction newSpecializedFunctionWithDescriptor:destinationArchive:functionCache:error:]`: unrecognised selector → NSException → abort | `ca-metal3-dealloc1`: `Terminating app due to uncaught exception ... unrecognized selector` | plug-in implements the specialization methods on the function object (logs the base name and the descriptor's constants, returns a placeholder) |
| W7 | builds an `MTLFunctionDescriptor` and calls `-[library newFunctionWithDescriptor:error:]` | Apple's `_MTLLibrary` implementation looks the function up in `_cacheEntry` (ivar 0x30), nil on the stub → SIGSEGV at 0x10; `pc 0x181aec43c` = Metal `_MTLLibrary` entry 36, called from QuartzCore 0x188479b8c | `ca-metal3-spec4`, `ca-metal3-int1`: same fault + register dump; resolved statically in `docs/research/ca-metallib-url.md` §11 | plug-in overrides `newFunctionWithDescriptor:error:` and the exact `newSpecializedFunctionWithDescriptorInternal:destinationArchive:functionCache:error:` spelling on the **library** class |
| W8 (not a wall) | — | after W7 CA creates pipeline states and encodes draws | `ca-metal3-int2` (§3) | — |

Two hypotheses were tried and killed on the way, kept here so they are not repeated:

- **The internal device pipeline selector**: the 16.5 cache carries
  `newRenderPipelineStateWithDescriptorInternal:options:reflection:destinationBinaryArchive:error:completionHandler:`,
  and the device does get asked for pipeline states eventually. But the boot log shows
  `_MTLDevice` does not implement that selector at all, and the fault of W7 happens before any
  pipeline state is made. A census boot that traps every inherited method of every stub class also
  broke Metal's own device registration (`_MTLDevice` was trapped: crash inside
  `CAMetalContextCreate` before any CA line), so the census excludes the device and the queue.
- **Hooking `MTLRenderPipelineDescriptor`'s function setters**: the public class carries no
  implementation; the private `MTLRenderPipelineDescriptorInternal` does. Hooking it is harmless
  but was not needed for anything this stage measured.

The general lesson, and the one that contradicts the draft plan's "return an empty library and a
tolerant object": **the subclass does not isolate us from Apple's `_MTLDevice`/`_MTLLibrary`/
`_MTLFunction`/`_MTLPipelineLibrary` implementations.** Every Metal object CoreAnimation touches has
to be overridden on our class, because the private base's implementation will otherwise run inside a
process that has the shared cache and will dereference ivars these stubs never set. Seven such
overrides were needed before the first draw; the log is the work list.

## 3. What CoreAnimation asks, and the first draws

Boot `ca-metal3-int2` (the last one): the window server is shown the device only (`metal=1` +
clients file). Sequence, de-duplicated against the serial console's double-printed lines:

- The device, queue, library and pipeline library are made; the **pipeline library** is asked for
  **21 distinct pipeline names** in this boot, repeated per window-server start:
  `P0081Xm_A2Xghfc`, `PBGRABsovXm_Tds4A2Xhf_Isrc`, …, `PRGhAXm_Tn23A2Xhf_Isrc`.
- The **function descriptor API** asks for `fixed_frag_lpf_cpf`, `fixed_vert_lpf_spc`,
  `luma_boost_lpf`, `std_vert1_lpf` — the `lpf_*` base functions that the pipelinelib does *not*
  carry (the pipelinelib has the 451 specialised `lph_*` modules; see
  `docs/research/ca-metallib-url.md` §4.4). This is CoreAnimation's runtime-specialisation path:
  the missing `default.metallib` would have carried those base functions.
- It wraps the display's **IOSurface 750×1624 BGRA8** into a texture
  (`newTextureWithDescriptor:iosurface:plane:`; our texture ignores the surface — the Stage C gap),
  makes the window-server textures and buffers, and creates render pipeline states:
  `newRenderPipelineStateWithDescriptor:error:: vertex fixed_vert_lpf_spc, fragment fixed_frag_lpf_cpf`
  and `… vertex std_vert1_lpf, fragment luma_boost_lpf`.
- It encodes and the command buffer is committed:

```
metalwire: CA draw: pipeline com.apple.coreanimation.draw.PBGRA_Xfc prim 3 indexed (vertices 0, indices 6, instances 1; record only)
metalwire: CA draw: pipeline com.apple.coreanimation.draw.PBGRABsov_TimgXfc_Isrc prim 3 indexed (vertices 0, indices 6, instances 1; record only)
metalwire: CA draw: pipeline com.apple.coreanimation.draw.PBGRA_TlubXf_Isrc prim 3 indexed (vertices 0, indices 6, instances 1; record only)
metalwire: CA render encoder: 3 draws, 13 sets, pipeline com.apple.coreanimation.draw.PBGRA_TlubXf_Isrc (first: prim 3, 0 vertices, 6 indices, 1 instances; record only)
metalwire: CA frame: 1 render encoders, 3 draws (record only; not on the wire)
```

- The window server's own frame path reports the presentation: three `swap` lines
  (`MiniIOMobileFramebuffer: swap 3 (#2): surfaces 2 …`) and
  `3 frames in 6018 ms … 75064 us each in the driver, after 31 us waiting for the GPU` during the
  BootUI phase. `prim 3` is `MTLPrimitiveTypeTriangle`; the draws are six-indexed triangles, i.e.
  the full-screen quad of CoreAnimation's display pass. No `userabort`, no panic, no restart loop in
  the boot until our own timeout.

What this establishes, and what it does not:

| | |
|---|---|
| **established** | CoreAnimation's Metal renderer is reachable through this port's device plug-in: it gets past the renderer-library assert, builds its pipeline library, asks for its pipelines and functions, creates pipeline states and encodes indexed draws on the `MTLCommandBuffer` this experiment records. The plan's Stage B acceptance line — "`MTLRenderCommandEncoder` `drawPrimitives:`/`drawIndexedPrimitives:` in the log; CA does not crash" — is met at the recording boundary. |
| **not established** | a picture. No shader is compiled (the functions are placeholders), no render target is real (`newTextureWithDescriptor:iosurface:plane:` returns a texture that ignores the IOSurface), nothing is sent on the metalwire wire, and there is no host execution. The three frames are CoreAnimation frames by the window server's own accounting; they contain no drawn pixels from the Metal path. |
| **also not** | Replay-path behaviour. The delivered path is not touched; the experiment runs only in the branch's own image with the injected dylib. |

## 4. Cost of the injection, and why it is a separate library

The first two walls happen **before any plug-in code runs**: the resource lookup and the library
load are inside `new_metal_context`, which uses the Metal framework's own object before the client's
plugin object is asked anything. A plug-in cannot answer them; a process-level insertion can. That
is all `guest/ca-metal-inject/` is: NSBundle resource hooks, a 64-byte placeholder file, and
diagnostics (resource-lookup log, fault register census, frame-pointer walk). It is not needed to
build the port, and it is off in the delivered image (the root image is the branch's own, built by
`tools/metalwire/prepare-guest.sh`). Without it the port behaves exactly as stage 29 left it.

The fault census was the tool that paid for itself: it records `x0–x3`, the selector in `x1` and a
frame walk at the first fault, which named W5 (`-dealloc` on a fake command queue) in one boot
instead of a static-analysis session. The static analysis then confirmed each one with function
starts and class method lists (`docs/research/ca-metallib-url.md`).

## 5. Self-corrections and dead ends

- **The draft plan's premise.** `ENG-METAL-001` §1 says the missing piece is the `_MTLDevice`
  plugin; this branch has had it since stage 29, and the real first wall is an NSBundle assertion
  inside QuartzCore. The plan's Stage A is therefore already satisfied for the branch; our own
  re-verification is in §6.1.
- **"An empty library is enough"** (plan §3.2, step 4). It is enough for the *assert's URL*, not for
  the load: Apple's inherited `newLibraryWithURL:` parses the file and aborts. The plug-in
  override, not the file, is what makes it work.
- **"A tolerant object that answers every selector"** (plan §5 risk row). Returning zero is what
  the plug-in did; it is not enough, because the selectors that hurt are *implemented on the
  private base classes*, so the forwarding path is never reached. They have to be overridden by
  name (W2–W7).
- **The `setDisableRunTimeCompilation:` wall** was first worked around in the injected dylib (a
  one-line no-op shim). The plug-in now implements it; the shim exists only as a gated diagnostic
  (`/redwood/etc/ca-inject-shims`, off by default).
- **The dealloc wall** was first worked around by replacing every Metal base `-dealloc` in the
  injected dylib. The plug-in now does it properly (skip the base dealloc, `object_dispose`);
  the gated shim remains only to make an A/B boot possible.
- **The internal device pipeline selector** and **the descriptor-setter hook** were both wrong
  turns for W7 and are recorded above so they are not repeated.
- **`metal=3` is not the right level for this experiment.** It shows the device to `mediaserverd`,
  which then aborts on a private CoreMedia selector twice a second; `metal=1` + a clients file
  naming `backboardd` is enough for CoreAnimation and leaves the rest of the system on its GL path.
- **GPU renumbering on the shared host.** The second V100 this project calls "GPU 1" can be bound
  to the other experiment's VM; when that happens `nvidia-smi` shows a single V100 as index 0. This
  stage used no host GPU at all, which is one way not to collide; a host-execution stage must pick
  the device by PCI id, not by the index it had in stage 29.

## 6. Relation to the plan's stages

### 6.1 Stage A (device loads, `MTLCreateSystemDefaultDevice()` non-nil)

Already satisfied on this branch by stage 29 (`docs/stage29-metal-translation.md` T3: three probe
boots, device + queue made, no panic), and re-verified in this session with a fresh probe boot
(`ca-metal3-probe1`: `prepare-guest.sh probe`, `metal=1`, 120 s). It printed

```
metal_probe: MTLCreateSystemDefaultDevice()...
metal_probe: device "HakiOS metalwire device (Metal on the host's Vulkan)"
metal_probe: registryID 6659
metal_probe: maxBufferLength 268435456
metal_probe: hasUnifiedMemory 1
metal_probe: newDrawableWithTexture: yes
metal_probe: queue "metalwire queue"
metal_probe: ok
hakios: exit pid=10 comm=metal_probe status=0
```

so `MTLCreateSystemDefaultDevice()` is non-nil and a queue is made in a process of the experiment's
own. Every CA boot also logs the same device path in the window server (`a device for the
accelerator port … is made`, the class registrations, `newCommandQueue`,
`newDepthStencilStateWithDescriptor:` before W1).

### 6.2 Stage B (CoreAnimation's Metal calls reach the wire) — recorded draws, no wire

The plan's acceptance is CA's `drawIndexedPrimitives:` in the log and no crash; that is §3. What is
**not** done is the plan's other half, "these calls go out on the metalwire protocol". The wire is
still the v1 blit-only contract (`host/metalwire/metalwire_protocol.h`); a render encoding is
recorded into the plug-in's counters and logged, not serialised. Extending the wire is Stage C
work, because a render command without its shaders is not executable on the host.

### 6.3 Stage C (a picture) — why it is not in this stage

The evidence for the estimate is now much better than the feasibility note's:

- **The shaders exist in the guest image.** `QuartzCore.framework/default.pipelinelib/functions` is
  an LPPM container of **451 single-function AIR modules** (444 fragment, 7 vertex:
  `fixed_frag_lph_cpf` ×305, `fixed_frag_lph_cph` ×139, `fixed_vert_lph_spc` ×6,
  `fixed_vert_lph_gen` ×1), all ordinary LLVM bitcode, all disassemblable. `pipelines.desc` names
  **1,079 render pipelines** `com.apple.coreanimation.draw.<spec>` and their per-specialisation
  constants; 986 of the 1,079 names map onto one of the 451 fragment modules
  (`docs/research/ca-metallib-url.md` §4). This is not the "no shaders available" picture the
  feasibility note had; it is a bounded corpus that a host translator could consume.
- **What is missing is still the translator, not the files.** HakiOS's AIR reader already lowers
  34 of the 35 `air.*` operations these modules use (one module, the generic vertex shader, uses
  `air.unpack.unorm4x8.v4f16` which it lacks); but it emits Mesa NIR, and this host's Vulkan needs
  SPIR-V. Writing that emitter (or rewriting the fixed shader set in Vulkan GLSL) is the Stage C
  cost centre; no off-the-shelf Metal→Vulkan *graphics* translator exists
  (`docs/research/metal-translation-feasibility.md` §3).
- **The present path is not two calls away from a picture.** In this boot the display texture wraps
  the real IOSurface only as a placeholder (the surface is ignored), the render target formats are
  metadata, no shader is compiled, and the wire has no render commands. Stage C has to replace
  three of those four, and only the fourth (CoreAnimation's call sequence) is now known.
- **The 21 pipeline names this boot asks for** are the window-server's BootUI set; the 1,079-name
  table plus the 93 names not covered by the pipelinelib are the full surface a real translation
  would have to cover. The 93 are the ones the runtime-specialisation path (the missing
  `default.metallib`) would have served — they are exactly the `lpf_*`/`lut*` families that W6–W7
  made visible (`fixed_frag_lpf_cpf`, `fixed_vert_lpf_spc`, `luma_boost_lpf`, `std_vert1_lpf`).

So the honest conclusion for the plan's Stage C is **needs more information on the cost, not
blocked on access to the shaders**: the corpus is measurable now, and the missing half is an
AIR→SPIR-V emitter (or a hand-written Vulkan GLSL set) plus a render-command wire. This stage does
not claim a frame.

## 7. Reproduce

```sh
# 1. build the plug-in and the injected library (HakiOS tree read-only)
guest/metal-plugin/build.sh
guest/ca-metal-inject/build.sh

# 2. the private run tree (once), then the image with the injection and the window-server-only gate
tools/metalwire/make-worktree.sh
guest/ca-metal-inject/install.sh on
tools/metalwire/prepare-guest.sh ui
printf 'metal_frame\nmetal_probe\nbackboardd\n' | sudo tee \
  /home/lshan/DSH-desktop/mt-work/staging/lib/HakiosMetal.clients >/dev/null
# rebuild the root image after editing the clients file (prepare-guest does this itself before the edit)
sudo python3 "$HAKIOS_DIR/tools/apfs/mkapfs.py" -z --plain fw -o /tmp/ca-root.apfs \
  "redwood=/home/lshan/DSH-desktop/mt-work/staging"
sudo chown "$(id -u):$(id -g)" /tmp/ca-root.apfs
cp /tmp/ca-root.apfs /home/lshan/DSH-desktop/mt-work/hakios/userland/root.apfs

# 3. boot with the window server shown the device and nothing else
MW_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1" tools/metalwire/run-guest.sh ca-metal3 300
grep -E 'CA draw:|CA frame:|swap [0-9]+ \(' \
  /home/lshan/DSH-desktop/mt-work/logs/ca-metal3/serial.log
```

The wire (`host/metalwire/`) is not started: this stage records only. The evidence logs behind the
tables are `mt-work/logs/ca-metal3-{baseline,inject1,lib1,pipelib1,shim1,dealloc1,spec4,int1,int2}/`.

## 8. Deliberate limitations

- **Record only.** No render command is serialised or executed; `CA frame: … (record only; not on
  the wire)` is the boundary.
- **Placeholder resources.** Textures that wrap an IOSurface do not wrap it; their pixels are never
  read by anyone. Vertex/index buffers are guest memory with no GPU counterpart.
- **No shaders.** The library and function objects are stubs; `newFunctionWithDescriptor:` answers
  a placeholder for every name. No AIR module is read, compiled or translated.
- **Base dealloc leak.** For classes on Metal's private bases, the base's `-dealloc` is not called;
  its allocation leaks (one command buffer per frame, command queues per start). A long run would
  have to replace this with creating the missing dispatch objects instead of skipping the dealloc
  (the analyst's §11 notes `_MTLCommandQueue._submittedGroup` and `_MTLLibrary._cacheEntry` as the
  two known cases).
- **No picture, no fps claim.** The three `swap` frames are the window server's accounting for the
  BootUI phase; nothing measured here is a claim about the delivered port's frame rate.

## 9. Files this stage adds or changes

- `guest/metal-plugin/HakiosMetal.m` — the object graph grows to: library, function, render
  pipeline state, depth-stencil state, sampler state, pipeline library, render command encoder,
  the descriptor-based function API, the specialization API, the base-dealloc rule, and the
  record-only render-encoder counters (plus the census mode gated by `/redwood/etc/metal-census`).
- `guest/ca-metal-inject/` — the injected dylib, its build/install scripts and README (§4).
- `docs/research/ca-metallib-url.md` — the static analysis of the NSBundle lookup, the LPPM/AIR
  corpus, the pipeline database, and the resolution of every fault address in this stage.
- `Makefile` — `ca-inject`, `ca-inject-install`.
- `README.md`, `docs/state-of-the-port.md` — the one-line state, updated from "CA is not on Metal"
  to what is now true.

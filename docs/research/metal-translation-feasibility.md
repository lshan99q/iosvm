# Metal → Vulkan translation: what is feasible, and what this experiment can prove

> Status: M1 research note, draft for review, 2026-11-21, branch `experiment/metal-translation`.
> This note bounds the question; the decision it supports is
> [`docs/adr/ADR-013-metal-translation.md`](../adr/ADR-013-metal-translation.md), the measurements it
> schedules are [`docs/stage29-metal-translation.md`](../stage29-metal-translation.md).

## Labels used in this note

| label | meaning |
|---|---|
| **verified here** | read in this repository, in the guest tree's source, or in a log/record on this host |
| **measured upstream** | a number recorded by the HakiOS tree (its own runs, read here but not re-run) |
| **reported** | an outside project's own documentation (URL given), not re-run here |
| **speculation** | reasoning, labelled as such |

No number in this note is estimated. Where the experiment has no number yet it says `TBD (M5)`.

Paths in backticks such as `docs/ru/metal.md` or `docs/air.md` belong to the **HakiOS guest tree**
(`/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel`), which the port is built on and which this
note reads as a primary source; they are not files in this repository.

## Summary

- **The interception seam already exists and is the right one.** The port publishes an empty
  `IOAcceleratorES` service on purpose, and boot-arg `metal=N` makes it name
  `HakiosMetal`/`HakiosMetalDevice`; Metal itself then loads
  `/System/Library/Extensions/<name>.bundle` and instantiates the `_MTLDevice` subclass inside
  (measured upstream in the HakiOS kernel and its bundle; §1). The experiment catches calls at
  `MTLCommandBuffer commit` — the boundary the design expects CoreAnimation to submit a frame at
  — not at a draw.
- **CoreAnimation does not use Metal here for two independent reasons, and neither is the wire.**
  First the device is deliberately not advertised (so CA falls back to its OpenGL ES renderer);
  second, with the device advertised, CA's Metal renderer stops on a missing `.metallib` and,
  behind it, the AIR compiler (measured upstream; §2). Full CA coverage is three subsystems —
  device object graph, AIR→SPIR-V, present/IOSurface path — spread over six sequential gates
  (§2.5), each of them measured in the thousands of lines in the systems that do it, against 3,685
  lines for the whole working GL path here.
  **This experiment cannot run CA's Metal renderer and does not claim to.**
- **Nothing off the shelf translates Metal to Vulkan graphics.** The closest prior art is UniXNU's
  `metal2vulkan`/AdrenoMetal (Metal→Vulkan in the guest, Adreno backend); MoltenVK is the inverse
  direction; SPIRV-LLVM-Translator is compute-flavoured; gfxstream/vkd3d are shape-analogues, not
  reuse (§3). The minimal subset this experiment implements — blit and present — is the smallest
  thing that exercises every seam (discovery, object lifetime, wire, host Vulkan execution,
  read-back, publication) with **zero shader dependency**, i.e. it isolates M2 from M3.
- **The transport is the socket for the calls, and guest RAM for the picture.** Stage 23
  implemented virtio-gpu end to end and measured zero frame-rate gain (1.80 → 1.85 fps, +2.8 %,
  inside the noise; 7.3–8.3 ms a push, ~3 % of a 245 ms frame): pipes do not move this port's
  frame time. The first version of the experiment still sent the 4.87 MB picture inline on the
  socket and measured **1.94 fps** — 490 ms a frame — which is a statement about a picture-carrying
  wire, not about the translation (§4). The final version borrows glwire's by-reference trick
  (`hakios_vtop` physical segments + QEMU's RAM file mapped on the host): the socket carries the
  calls and ≈ 1.5 KB of segments, and the same client then runs at **58.3 fps median** with
  2,000/2,000 frames CRC-clean (§6.6).
- **What this experiment can prove:** Metal's own loader loads a third-party device in this guest;
  a pure-Metal client can commit, wait and present; translating at commit and executing on the
  host's Vulkan (physical device 1) reproduces the client's picture byte for byte (CRC-32); the
  translated frame is visible in the existing browser viewer; and the whole host path is testable
  on lavapipe with no GPU (§5.3).
- **What it cannot prove:** a single CoreAnimation frame, any shader compilation, any render
  encoder, IOSurface sharing, `MTLSharedEvent`, multi-process behaviour, or that the delivered
  13.9 fps moves at all. It also cannot prove the host GPU executed the copy: a host `memcpy`
  would pass the same CRC — the lesson [`docs/phases.md`](../phases.md) already records for the
  vkbridge probe (§6.3).
- **Verdict, in the spike vocabulary** ([`CONTRIBUTING.md`](../../CONTRIBUTING.md)): the
  *mechanism* is **feasible**; *CoreAnimation through Metal inside this experiment* is **not
  feasible**; whether the translation path can ever beat the Replay baseline is **needs more
  information**, and the information requires M3 and M4, not M2 (§5).

## 1. Question 1 — where a Metal call can be intercepted, and what each choice costs

### 1.1 The discovery chain, as this port has it

Metal finds a device through IOKit, not through a file path. The chain is five steps (the HakiOS
tree's reading of Metal 306.6.4 and of the kernel source it quotes; the reading is theirs, so it is
labelled measured upstream):

1. `MTLRegisterDevices` matches `IOServiceMatching("IOAcceleratorES")`. This class name is the whole
   of GPU discovery; there is no second way to ask.
2. For each service, `getMetalPluginClassForService` reads the IORegistry property
   `MetalPluginName` and requires a `CFString`.
3. It builds `/System/Library/Extensions` + that name + `.bundle` and opens the bundle with
   `NSBundle bundleWithPath:`.
4. It reads `MetalPluginClassName`, takes the class (or the principal class), and **rejects any
   class that is not a subclass of Metal's private `_MTLDevice`**.
5. `[[class alloc] initWithAcceleratorPort:service]` returns the device. Metal then asserts the
   class conforms to the `MTLDevice` protocol and to the private `MTLDeviceSPI` protocol — neither
   is declared by `_MTLDevice`, so the subclass must declare both (measured upstream, HakiOS
   `docs/metal-plan.md`, "What steps 1 and 2 found", items 1–2).

The port's kernel already publishes an `IOAcceleratorES` service named `sgx`, **empty on purpose**:
its own comment says a service of this class exists so CoreAnimation does not wait half a minute
for one, and the two properties that would name a Metal or GLES driver are deliberately absent, so
both renderers pass it by (measured upstream, `docs/ru/metal.md` §2, which quotes
`MiniIOMobileFramebuffer.cpp`). `metal_test` prints the state exactly:

```
metal_test:   IOAcceleratorES "sgx": MetalPluginName (none), MetalPluginClassName (none)
```

With boot-arg `metal=N` the service gains `MetalPluginName = HakiosMetal` and
`MetalPluginClassName = HakiosMetalDevice`, and a bundle at
`/System/Library/Extensions/HakiosMetal.bundle` (a symlink in the experiment's root image to
`/redwood/lib/HakiosMetal.bundle`) supplies the class. `N` gates **which processes are shown the
device**: `metal=1` only a program named `metal_test`, `metal=2` every process but the window
server, `metal=3` every process; a `/redwood/lib/HakiosMetal.clients` file beside the bundle names
further programs, one per line, `*` for all (HakiOS tree; measured upstream for the `metal=N` levels).

Two consequences matter for an interposition choice:

- **The device list is answered once per process and never again.** `MTLCreateSystemDefaultDevice`
  builds the list under `dispatch_once` and caches it; an empty list is a silent `nil` with no log
  line and no error. `MTLCopyAllDevices` is `API_UNAVAILABLE(darwin)` and absent from the 16.5
  binary (measured upstream, `docs/ru/metal.md` §1). A device that is not advertised before the
  process first asks is not reachable afterwards.
- **The gate is per process and must be right the first time.** This is why the experiment uses a
  clients file naming `metal_frame` rather than `metal=3`: showing the device to the window server
  is known to stop it on the library it cannot find (§2.2), and showing it to `mediaserverd` makes
  that process end and restart (measured upstream, `docs/metal-plan.md`, "What steps 1 and 2 found",
  item 7).

### 1.2 Five seams, and what each costs

| seam | where the call is caught | what it costs | verdict |
|---|---|---|---|
| **A. Device plug-in through `IOAcceleratorES` (chosen)** | inside our `_MTLDevice` subclass: the client's own API calls, e.g. `commit` | the Metal object graph we choose to implement; no new kernel change (the service and the boot-arg already exist); no Apple file; the class must subclass `_MTLDevice` and declare `MTLDevice`/`MTLDeviceSPI`; unknown selectors must be absorbed, not crash | **chosen**: it is Metal's own supported seam, per-process gated, and the only one that can be tested in QEMU today |
| B. Kernel driver (provide a real accelerator and IOGPU submit ABI) | kernel / IOKit | the `IOGPU` layer is 36 classes and 711 ObjC methods whose ABI the kernel would have to satisfy (measured upstream, `docs/ru/metal.md` §4); memory, fences, submit; and on an x86 host there is no AGX to submit to | not for this experiment; ADR-002's "no GPU emulation" stands |
| C. User-space interposition of Metal.framework (dyld interpose, swizzle `MTLCreateSystemDefaultDevice` / `_MTLDevice`) | the process's call to Metal | private and version-fragile; Metal lives in the dyld shared cache; no per-process gate as clean as the IORegistry one; **and the same object graph is still needed** afterwards | no advantage over A; a diagnostic fallback at best |
| D. Translate to the existing GLES in the guest | the same commit point | the same object graph **plus** AIR→GLSL; GLES cannot express compute, argument buffers or much else; strictly more work than A | rejected (HakiOS's own `docs/ru/metal.md` §4 reaches the same conclusion for its system) |
| E. Translate the GL replay stream to Vulkan on the host | CoreAnimation's GL calls | a different project (desktop GL 4.5 → Vulkan replay), and it still does not run a Metal client; answers a portability question, not the Metal question | out of scope here; noted as the cheaper alternative if the goal is only host-side GPU portability |

### 1.3 Why the plug-in is the choice, and where the boundary sits

A is chosen because the seam is **Metal's own**: the loader, the bundle path, the class check and
the accelerator port are all defined by the system, so nothing is guessed and nothing is patched.
The port has already paid the kernel-side cost (the service and the boot-arg exist); the remaining
cost is the object graph, which is exactly the L3 work ADR-002 assigned to
`guest/metal-plugin` ([`ADR-002`](../adr/ADR-002-api-translation.md)).

The translation boundary inside the plug-in is **`MTLCommandBuffer commit`**, not an encoder and
not a draw:

- it is where a Metal client says "this group is complete and ordered", and where CA's renderer
  submits a frame — the same boundary the GL path uses today (`eagl.c` ends a frame on
  `glFlush`/`glFinish`, and ADR-012 records that this is emphatically *not* a CA pass boundary, a
  trap this design avoids by recording whole command buffers rather than passes);
- it is the smallest unit with a defined completion: `waitUntilCompleted` blocks until one ack,
  so the v1 guest loop is synchronous and no double buffering is invented;
- encoder-level translation would need pipeline-state lifetime and render-pass state the v1 subset
  does not have; draw-level is finer than any synchronisation we can test.

## 2. Question 2 — why CoreAnimation does not take a Metal path today, and what full coverage needs

### 2.1 The device is not advertised, by design

With `metal=0` (the default) the accelerator service has no `MetalPluginName`, Metal's step 2 fails,
`MTLCreateSystemDefaultDevice()` returns `nil` silently, and CoreAnimation's renderer takes its
OpenGL ES path. The port deliberately keeps it that way: a Metal device that exists but cannot draw
is worse than no Metal device, because CA prefers Metal when it sees one. The kernel comment quoted
in §1.1 is the decision; `docs/state-of-the-port.md` records the boot-arg's effect as "nothing loads
it yet, so it changes nothing today".

### 2.2 With the device shown, CA's Metal renderer stops on its library

CoreAnimation asks in a fixed order: `CA::WindowServer::EAGLServer::renderer()` tries
`CAMetalContextCreate()` and `CA::OGL::new_metal_context()` **first**, and only if neither yields a
renderer does it take the `EAGLContext`/OpenGL ES path (measured upstream, `docs/ru/metal.md` §3, with
cache addresses). With `metal=3` the renderer gets a device and then stops at:

```
Assertion failed: (metallib_url != nil), function MetalContext, file ogl-metal.mm, line 3308.
```

It needs the URL of a `.metallib`. The HakiOS record bounds this in two directions and **does not
settle which file it wanted**: it says the system carries
`AppleMetalGLRenderer.bundle/default.metallib` (274 shader modules), and it also says
`QuartzCore.framework` has a `default.pipelinelib` naming a `default.metallib` that is not in the
extracted 16.5 image — "where CoreAnimation takes that URL from is not known yet". Either way the
plug-in cannot supply it: the library is Apple's, and shipping it would break this repository's
first veto ([`PROVENANCE.md`](../../PROVENANCE.md), ADR-006).

### 2.3 The shader wall, as measured

`.metallib` is AIR: ordinary LLVM bitcode in a container. Mesa's GPU compilers take NIR, Vulkan
takes SPIR-V; the port's host is Vulkan. The HakiOS tree built the frontend in the NIR direction
and measured it on the device's own system libraries (measured upstream, `docs/air.md` and its
corpus report):

| stage | modules | of 456 |
|---|---:|---:|
| container read | 456 | 100 % |
| read by their own bitcode reader, byte for byte as `llvm-dis` reads them | 456 | 100 % |
| translated to NIR, passing `nir_validate` | 321 | 70 % |
| compiled by `ir3` (Adreno) | 321 | 70 % |
| numerically identical to LLVM's execution, and drawn | 255 | 56 % |
| run on the phone's GPU | 54 | 12 % |

And the breakdown that matters here: 321 is **every entry point** of the corpus (271 fragment
shaders, 47 compute kernels, 3 vertex shaders); the other 135 modules are `air.visible` library
functions, not entry points. The one library CA's renderer is known to want,
`AppleMetalGLRenderer.bundle/default.metallib`, is 274 of the 456 and **all 274 translate and
compile** in that corpus.

What that means, precisely: **AIR is readable and translatable — to NIR, on Adreno.** It does not
mean AIR→SPIR-V exists. The 274 modules are read; the emitter to the host's shader language is the
unbuilt half, and the HakiOS cost estimate for "what the system's shaders need" is months and
~5–10 k lines for one GPU family (measured upstream, `docs/ru/metal.md` §9.4), with a warning that
their NIR path is not a SPIR-V path. The port's own host backend is Vulkan, so the target must be
SPIR-V (§3).

### 2.4 The object graph, as measured

| what | size | source |
|---|---|---|
| `AGXMetalA11` — the driver a full Metal implementation replaces | 2,578,880 B, 5,798 symbols, **53 classes, 1,158 methods** | measured upstream, `docs/ru/metal.md` §4 |
| `IOGPU` — the layer between `AGX*` and `_MTLDevice`, and the kernel ABI it needs | 461,664 B, **36 classes, 711 methods** | same |
| `_MTLDevice` — the private base a plug-in subclasses | **549 methods, 36 ivars** | same |
| `AGXA11FamilyDevice` — how much of the base the real driver overrides | 236 methods | same |
| `MTLCompiler` + `AGXCompilerCore` — the shader front and back ends | 983,544 B + 2,680,736 B | same |
| what the port's working GL path cost, for scale | `eagl.c` 3,685 lines; 62 of 409 EAGL slots used; 30 written by hand; 6 ObjC methods replaced | same |

The number to keep is not "1,158 methods" as a target — a plug-in implements only what its clients
send — but the shape: a working GL replacement that reaches the home screen is 3,685 lines and 62
entry points; the Metal object graph the system's own clients touch is of a different order, and
`_MTLDevice`'s 36 private ivars mean a subclass that over-answers can corrupt memory inside Apple's
code with no diagnostic (reported by the same note).

### 2.5 What full CoreAnimation coverage would require

Sequential gates, each of which must pass before the next is testable:

| gate | requirement | state | cost |
|---|---|---|---|
| 1 | a device Metal accepts (subclass, protocols, `initWithAcceleratorPort:`, command queue) | **done upstream**: device + queue + buffers + libraries in QEMU, checked on a Mac | days–weeks; this experiment re-proves the device part |
| 2 | CA's Metal renderer finds its library | **blocked**: `metallib_url != nil`; the file is Apple's and not in the image; which URL it wants is not settled | legal + engineering; **cannot ship the file** |
| 3 | a shader compiler from AIR to the host's target | **not built for SPIR-V**; AIR→NIR measured 456/321/255 | months, ~5–10 k lines (HakiOS estimate) |
| 4 | render encoder + pipeline states + argument buffers + depth/stencil | stub classes exist; nothing draws | months |
| 5 | present path: drawable, `IOSurface` sharing, `MTLSharedEvent`/fences, colour management (P3) | not started; the port's IOSurface sharing across the VM boundary is an M1 unknown in [`docs/phases.md`](../phases.md) | months, and the ABI is private |
| 6 | enough performance to beat 13.9 fps | unknown | not measurable before 2–5 |

**Conclusion for this question:** CA is not on Metal because it has never been given a device that
can draw, and the first thing that stops it once given one is a library the port cannot legally
ship. The shader compiler behind that library is the second. Neither blocker is the discovery seam,
which is why this experiment can still test the seam end to end with a client of our own.

### 2.6 What the experiment tests instead

A controlled Metal program — `metal_frame` — that creates a device through
`MTLCreateSystemDefaultDevice()`, a queue, a 4.87 MB buffer (750 × 1624 BGRA8), a texture and an
internal drawable (the device exposes `-newDrawableWithTexture:`, because this user space has no
`CAMetalLayer` to make a stock one); fills the buffer with a deterministic picture; records one
`copyFromBuffer:…toTexture:` blit and a `presentDrawable:`; commits and waits; and prints per-frame
timings and a summary. It is **blit-shaped**, which is deliberate: a blit is the one Metal command
whose Vulkan equivalent is exact (`vkCmdCopyBufferToImage`), whose result can be checked byte for
byte, and which needs no shader — so a failure is a failure of the seam, transport, objects or
synchronisation, never of a compiler that does not exist.

## 3. Question 3 — which translator to build on

### 3.1 Survey

| project | direction | what it is | usable here? |
|---|---|---|---|
| **UniXNU AdrenoMetal / metal2vulkan** | Metal → Vulkan, **in the guest** | a `_MTLDevice` plug-in plus a runtime that lowers Metal objects/commands to Turnip (Mesa's Adreno Vulkan driver) over the kernel's GPU interface; composes the system UI on a Xiaomi Pad 6S Pro | **prior art, not adopted.** It is the same seam and the same API-level idea at production size; its backend is an in-guest Vulkan driver for one GPU family, not a wire to a host V100, and its runtime is the opposite of this experiment's minimum. License is MIT per the HakiOS note (*reported*) |
| **MoltenVK** | Vulkan → Metal | layered Vulkan 1.4 subset on Metal, with a SPIR-V→MSL shader converter; Khronos deprecated it in favour of KosmicKrisp for Apple Silicon, community-maintained now | **not our direction.** Its value here is the cautionary measurement: the *inverse* translator is a multi-year, foundation-scale project, and it converts a representation designed for translation (SPIR-V) into the one shader language it can emit (MSL) |
| **SPIRV-LLVM-Translator** | LLVM IR ↔ SPIR-V | Khronos tool/library for bi-directional translation | **partly usable, one axis.** HakiOS's survey reads it as supporting the OpenCL/compute flavour of SPIR-V, so `AIR → SPIR-V → spirv_to_nir` does not work head-on for vertex/fragment (*measured upstream*, not re-run here). For compute kernels it is a candidate starting point; for graphics the SPIR-V side would have to be written |
| **gfxstream** | Vulkan/GLES API streaming, guest → host | generated per-entry-point marshalling, host Vulkan/GLES backend; the closest *shape* to this experiment's wire | **no code reuse, useful model.** It confirms the architecture: generated marshalling + host-side Vulkan + guest object ids. It is not a Metal frontend |
| **vkd3d** (Direct3D → Vulkan) | D3D12 → Vulkan | Wine's translation library, with its own shader compiler to SPIR-V | **analogue, no reuse.** Shows an API-translation layer is viable and maintained, and that the shader compiler is the expensive half |
| **Mesa's Dozen** (Vulkan → D3D12) | inverse | Vulkan driver on D3D12 | analogue only |
| **Naga, SPIRV-Cross, ANGLE, Apple Metal Shader Converter, HLSL→Metal** | anything → Metal/MSL | mature "into Metal" or MSL-emitting tools | **not usable**: none reads AIR |
| **HakiOS AIR→NIR** | AIR → Mesa NIR | the measured frontend above; AIR reader, 51 operations, 2D textures | **not usable as a host-Vulkan target** (NIR is Mesa-internal and the host is not Mesa), but it is the reusable asset for M3: its AIR reader is proven on 456/456 modules; what M3 needs is a SPIR-V emitter, not another bitcode reader |
| **Asahi Linux** | — | deliberately no AIR reader; NIR backend | confirms nobody has shipped an AIR frontend |

### 3.2 Reading of the survey

Two facts decide the translator choice:

1. **There is no Metal→Vulkan graphics translator to adopt.** The one that exists (UniXNU's) is a
   complete object graph aimed at one mobile GPU and its kernel ABI; adopting it would mean
   adopting its mesa fork and its backend, and it still would not put the frame on this host's
   Vulkan device. MoltenVK is the opposite arrow. The LLVM translator is compute-only-flavoured
   for this purpose.
2. **The shader question can be deferred without faking the result.** A blit/present client needs
   no shader at all, and the wire, object lifetime, Vulkan execution and read-back/publication
   seams have the same shape for a blit as they would for a frame — while the CA-specific present
   path (IOSurface, the real drawable) is exactly what v1 does not claim to cover. So the
   experiment can test the seam now, and the shader frontend stays a separate gate (M3) with its
   own kill criterion, instead of being smuggled in as "not yet implemented".

### 3.3 The minimal subset, and why it is the minimum rather than a shortcut

v1 implements: device, command queue, command buffer, `MTLBlitCommandEncoder` with one
buffer→texture copy, buffer (host-visible staging), texture (optimal tiling, BGRA8/RGBA8), an
internal drawable, `presentDrawable:`, `commit`, `waitUntilCompleted`, and the object lifetime
around them. The wire carries exactly those operations (`MW_CREATE_BUFFER`, `MW_UPLOAD_BUFFER`,
`MW_CREATE_TEXTURE`, `MW_COPY_BUFFER_TO_TEXTURE`, `MW_PRESENT`, `MW_FRAME_BEGIN/END`), and the host
executes the copy on a real Vulkan device, reads the result back, verifies a CRC-32 and publishes
the picture in glwire's shared-memory format.

Why this and not more: every additional operation (render encoder, pipeline state, library, draw)
brings an AIR dependency or a private path; every operation removed below a buffer→texture copy
leaves a seam untested (no texture means no `VkImage` lifecycle; no present means no publication;
no wait means no acknowledgement). One copy spans all of them. Why not less: a host-side `memcpy`
would pass without Vulkan, which is precisely the failure mode [`docs/phases.md`](../phases.md)
records for the vkbridge probe — and which this experiment must therefore name as a limit, not
hide (§6.3).

## 4. Question 4 — transport: glwire's socket or virtio-gpu

### 4.1 What the socket is

TCP, one connection, guest → host `10.0.2.2:9111` (QEMU user networking maps that to host
loopback), one acknowledgement per committed frame, the same shape as glwire: the guest records,
the host executes and answers once. On the host the picture is published into
`/dev/shm/metalwire-frame` in glwire's format — magic `GLFW`, sequence number, dimensions,
`CLOCK_MONOTONIC` timestamp, BGRA bytes — so `tools/glwire/webview.py --shm` serves a translated
frame **unmodified** (verified here, `host/metalwire/PROTOCOL.md` and `metalwire_protocol.h`).

### 4.2 virtio-gpu: the stage-23 measurement

virtio-gpu was implemented end to end (device discovery, feature negotiation, control queue,
`GET_DISPLAY_INFO`, `RESOURCE_CREATE_2D`, `ATTACH_BACKING` of 298 pages, `SET_SCANOUT`,
`TRANSFER_TO_HOST_2D`, `RESOURCE_FLUSH`; 1,001 pushes, zero failures) and measured on a same-root
A/B over a 124.6 s scroll:

| | A: TCP/wire | B: + GPU push |
|---|---:|---:|
| scroll frames | 224 | 230 |
| **scroll fps** | **1.80** | **1.85** |
| push cost | — | 584 pushes, 7.3–8.3 ms each |

+2.8 %, inside the noise: **the transport was not the bottleneck** (`docs/stage23-verdict.md`,
verified here). The guest census explains it: of ~245 ms a frame, CoreAnimation's own work was
~184 ms, read-back 60 ms, flush 0.6 ms, and the guest's **wait for the transport was 5 µs**.

### 4.3 What that does and does not imply

It implies:

- A faster or more direct transport cannot raise the replay path's frame rate, because the frame
  time is guest CPU, not wire time. The same argument applies to a translation path: moving
  commands over virtio-gpu instead of TCP will not make translation cheap; **the win has to come
  from removing guest work** (CA's draw work would move to the host), and no transport change
  moves it.
- Transport should therefore be chosen for *testability and failure isolation*, not for speed, in
  an experiment whose question is translation.

It does **not** imply:

- that TCP plus inline pictures is fast enough for a production translation path. The first
  version carried the **4.87 MB picture** over the socket every frame and measured 1.94 fps
  (`trans2`, 490 ms a frame): under TCG, a picture on the wire is the frame. The final version
  passes it by reference as the Replay path does (`hakios_vtop` + the host's mapping of QEMU's RAM
  file) and keeps the socket for the calls (≈ 1.5 KB a frame); that is the only change between the
  1.94 fps run and the 58.3 fps median of §6.6. The lesson is not "the socket is slow" but "the
  picture does not belong on the socket" — and it had to be measured to be said.
- that virtio-gpu was wasted work. It is the groundwork for a 3D path (ADR-004), and it is where a
  future zero-copy picture path would live. It is simply not this experiment's transport.

### 4.4 The experiment's transport, stated narrowly

One TCP connection on 9111 carries the Metal calls, one raw acknowledgement per frame (status,
timings, CRC), and — when the buffer is resident and the kernel answers `hakios_vtop` — a list of
physical segments instead of the picture bytes; the host maps QEMU's RAM file and reads the same
memory (the Replay path's own trick, `tools/glwire`). The SHM publication is host-local. This is
glwire's shape with a smaller op set; the fallback carries the picture inline and is kept and
measured, not deleted. virtio-gpu is still not this experiment's transport, for the reasons in
§4.3; a production path would put the same by-reference scheme on a device with proper fences and
shared memory (ADR-004), and the numbers here say the transport choice is worth one experiment,
not the architecture.

## 5. Question 5 — risks, kill criteria, and the phased plan

### 5.1 Risks

| id | risk | likelihood | impact | mitigation | measurement that would show it |
|---|---|---|---|---|---|
| R1 | **AIR→SPIR-V** never reaches a usable graphics subset | high | blocks all CA Metal paths (M3/M4) | keep M2 shader-free; M3 is a separate spike with a one-shader gate | `clearFragment01` translates and draws a matching pixel |
| R2 | the Metal object graph CA uses is wider than estimated (private `MTLDeviceSPI` paths, render encoder, argument buffers) | high | months; possibly unbounded | do not promise CA coverage; measure what CA actually sends only in M4 | the plugin's "unanswered selector" log during a CA boot |
| R3 | CA's renderer library is Apple's and cannot be shipped; which URL it wants is unresolved | high | M4 blocked on constraints, not effort | record it as a legal/product question, not an engineering one | the `metallib_url` assertion and any substitute library's failure |
| R4 | present/synchronisation semantics (`MTLSharedEvent`, drawable, IOSurface, fences) do not map to a headless read-back | medium | a translated CA frame cannot be presented through the existing browser path | v1 uses an internal drawable and a read-back; treat the real present path as M4 work | differences between the guest's expected fence order and the ack order |
| R5 | the translated path is slower than the replay path | medium | the path is correct but pointless as a speed-up | measure cost separation honestly; the comparison is only meaningful after M4 | paired guest CPU/frame and wall-clock in the same load band |
| R6 | host contention: one V100 (device 1) shared with the GL replay experiment; wall clock moves ±30% with load | certain | numbers unattributable if not paired | separate QEMU instance/ports/GPU/SHM; paired runs; report load | load-banded paired table (§6.4) |
| R7 | the experiment branch rots: `experiment/…` is outside the documented branch prefixes and spikes are time-boxed at 5 days | medium | stale code, unclear ownership | M6 decision or delete; the ADR records the status | age of the branch vs the M6 decision |
| R8 | **correctness illusion**: the CRC proves the host read back the guest's bytes, not that the GPU executed the copy — a host `memcpy` passes it | certain (by construction) | overclaiming "executed on the GPU" | state it; plan a device-side oracle (compute checksum) as future work, mirroring the vkbridge lesson in `docs/phases.md` | a patched host with the Vulkan calls replaced still yields `crc_ok=1` |
| R9 | boot damage: a device that exists but cannot draw crashes/restarts backboardd and mediaserverd | medium | lost boots, misleading logs | keep `metal=1` + clients file (never `metal=2/3` in a UI boot); absorb unknown selectors; never hang in `waitUntilCompleted` | a boot with the device shown to CA stops at the library assertion, as upstream measured |

### 5.2 Kill criteria

A criterion is a stop, not a prompt to try "one more day" ([`CONTRIBUTING.md`](../../CONTRIBUTING.md)
§Spikes). Each names the evidence that triggers it.

| id | if this happens | then stop / do this |
|---|---|---|
| K1 | the host path cannot execute one frame on lavapipe with `crc_ok=1`, or `--gpu 1` cannot publish and read back a frame | the wire/executor shape is wrong; write up the failure, do not patch around it |
| K2 | `metal_probe` gets `nil` from `MTLCreateSystemDefaultDevice()` in 3 boots with `metal=1` and the clients file naming it | the `_MTLDevice` route is closed on 16.5; M2 cannot proceed as designed |
| K3 | with the device shown to CA, the library cannot be supplied without an Apple file, and no substitute can satisfy it | M4 is blocked by constraints; record it, do not manufacture a workaround |
| K4 | `clearFragment01` (four LLVM instructions, no `air.*`) cannot be translated and drawn matching LLVM inside the M3 box | AIR→SPIR-V for graphics is beyond this project's budget; stop M3 |
| K5 | the translated client's per-frame cost (guest encode + wire + host execution and read-back, excluding its own picture generation) exceeds the replay path's 75 ms frame budget for the same picture size | the mechanism cannot carry the delivered frame rate even before CA's work is added; keep it as a correctness demonstrator only, never as a speed claim |
| K6 | the replay baseline cannot be reproduced on this host in the same load band | no comparison is claimed; M5 reports mechanism only |
| K7 | any step requires an Apple binary, library or image | stop; legal evaluation (the same rule as D2 in [`docs/phases.md`](../phases.md)) |

### 5.3 The phased plan (M1–M6)

Milestones are this experiment's own; they are **not** the project milestones M0–M4 in
[`docs/phases.md`](../phases.md) (the project's M2 "GPU channel" is the parent of this work).

| # | what | deliverable | gate | in this experiment? |
|---|---|---|---|---|
| **M1** | feasibility and decision (this note + ADR-013) | `docs/research/metal-translation-feasibility.md`, `docs/adr/ADR-013-metal-translation.md`, `docs/stage29-metal-translation.md` | the report answers the six questions and names what it cannot prove; the lead decides to build M2 | **yes — this document** |
| **M2** | the smallest real path: plug-in, wire, host Vulkan executor, browser frame | `guest/metal-plugin/`, `host/metalwire/`, `tools/metalwire/` | lavapipe test green; GPU 1 frame read back with `crc_ok=1`; picture visible in `webview.py`; client runs in the guest | **yes — in progress** (other roles own the code) |
| **M3** | AIR→SPIR-V for one system shader | a spike report, not code on `main` | `clearFragment01` from a real `.metallib` draws the expected pixel on the host GPU | **no** — separate time-boxed spike; this experiment must not attempt it |
| **M4** | CA's Metal renderer on the translated device | nothing shippable until the library question is answered | a CA frame through the device, on screen | **no** — blocked by R1–R3; requires a legal answer first |
| **M5** | measurements | results filled into `docs/stage29-metal-translation.md` | paired runs with CRC and byte-identity, load-banded, numbers not adjectives | **partly** — the protocol and CSV fields exist; the numbers are the lead's runs (`TBD (M5)`) |
| **M6** | decision | ADR-013 status updated (accept as direction / keep as experiment / stop) | M2–M5 evidence reviewed | **no** — after M5 |

### 5.4 Explicitly out of scope

- CoreAnimation, `WindowServer`, SpringBoard or any Apple client on Metal (M4, blocked).
- AIR, `.metallib`, Metal libraries, pipeline states, any shader (M3).
- `MTLRenderCommandEncoder`, argument buffers, compute encoders, tessellation.
- `IOSurface`, `MTLSharedEvent`, cross-process sharing, swapchains, the real present path.
- Any frame-rate claim about the delivered port; any change to the Replay path or its defaults.
- Any Apple binary, any downloaded Apple material, any credential (the three vetoes).

### 5.5 Decision table

| option | what it would give | what it costs | decision |
|---|---|---|---|
| Keep the Replay path as delivered, change nothing | 13.9 fps, a working port, no new risk | leaves ADR-002 unproven and the Metal question open | **baseline, unchanged** |
| **Build M2: plug-in + wire + host Vulkan, synthetic client (chosen)** | proof of the mechanism; a CI-testable host path; the seam CA will need | weeks of two roles; no delivered fps gain; limited coverage | **do it, as an experiment** |
| Attempt AIR→SPIR-V now (M3) | the first real shader on the host GPU | months, a separate compiler project; no CA frame without M4 anyway | **not in this experiment; spike only** |
| Attempt CA's Metal renderer now (M4) | the only route to CA-on-Metal | blocked by the library (legal) and the compiler (M3) | **blocked; record, do not attempt** |
| virtio-gpu 3D / Venus as the transport | a standard 3D path | stage 23 measured zero transport gain; another device project (ADR-004) | **not for this experiment** |
| Emulate AGX | guest runs Apple's drivers unmodified | instruction set is secret; years (ADR-002) | **rejected** |
| Translate Metal to GLES instead | reuses the existing GLES | same object graph + AIR→GLSL, less expressive (HakiOS §4) | **rejected** |

## 6. Question 6 — the measuring plan: what the numbers will mean

### 6.1 What is measured, and where

Per frame, the guest prints `fill_us` (generating the deterministic picture), `encode_us` (the
Metal calls), `commit_us`, `wait_us` (`commit` → ack) and `total_us`, and a summary line (frames,
ok, failed, fps, encode mean/min/max, round-trip mean/min/max). The plug-in computes the CRC-32 of
the picture bytes it sends, so correctness is reported by the host's `crc_ok`, not by the client.
The host writes a CSV row per frame with: `seq`, `bytes`, `recv_us` (frame begin → frame end
arrival), `exec_us`, `gpu_us`, `readback_us`, `publish_us`, `total_us`, `crc_ok`, and a rolling 1 s
`fps`; it also saves frames as PPM/PNG and publishes to SHM (`host/metalwire/PROTOCOL.md`,
`metalwire_protocol.h`, and the CSV header in `host/metalwire/metalwire_host.c`). The guest's
serial log is the only window into the guest: one line per frame, one summary line.

### 6.2 What the numbers will mean

- **Correctness first.** `crc_ok=1` on every frame, plus a full byte comparison between the saved
  frame and the deterministic picture the client meant to send, is the gate. A performance number
  from a run with one CRC mismatch is discarded.
- **Where the time goes.** The split says which side owns the frame: guest encode vs guest wait vs
  wire (recv) vs host execute/read-back/publication. The replay path is the prior: its 75 ms a
  frame is guest CPU, its wait was 1 µs, and its host replay work was ~1.4 ms — so if the
  translated path shows the same shape, the conclusion is that **guest work, not translation, is
  the ceiling**, and a translation path only pays off if it removes guest work.
- **The cost of the picture copy, if there is one.** The first wire uploaded 4.87 MB a frame and
  the cost was the frame (1.94 fps); the final wire passes the picture by reference and the cost
  disappears (58.3 fps median). Both are measured and both are in §6.6.
- **Paired runs only.** The wall clock moves ±30% with host load (README, and every stage record
  since stage 23 says the same). The comparison protocol is fixed: one QEMU run at a time; the
  same picture size on both sides; translated and replay runs paired back to back in the same load
  band; medians over windows; the load average reported with every table; never a single shot.

### 6.3 What they will not mean

- **Not a CA number.** The client is synthetic (see 6.5).
- **Not proof that the GPU executed the copy.** `crc_ok=1` proves the host read back the guest's
  bytes. A host that replaced `vkCmdCopyBufferToImage`/`vkCmdCopyImageToBuffer` with a `memcpy`
  would pass the same check. [`docs/phases.md`](../phases.md) records exactly this trap for the
  vkbridge probe ("a patched copy printed 8/8"), and the honest statement here is the
  same: the result is correct, the code does use Vulkan, and the probe alone does not establish the
  device. A device-side oracle (a compute-shader checksum, a non-host-visible staging path) is the
  fix and is future work, not an M2 claim.
- **Not a transport verdict.** The by-reference path is the Replay path's own mechanism, applied
  at a different seam; the number says this mechanism works at this seam, not that TCP would be
  adequate in production.
- **Not a delivered-fps claim.** The delivered port stays at 13.9 fps / 75 ms (one vCPU, the
  delivered setting) until something changes it; this experiment changes nothing on that path.

### 6.4 The paired-measurement discipline (as the README states it)

> The wall-clock figure moves ±30% with host load, so compare paired runs, never single shots.
> ([`README.md`](../../README.md))

The experiment adds: one run of ours at a time on this host (the other experiment keeps running,
and its load is exactly what the load average is there to report); the experiment's QEMU instance (3,
ports 4474/4475) pinned to its own cores (26–28 by default); the replay baseline rebuilt as a
separate root-image mode and pointed at a different replay port (9210) so it can never talk to the
delivered replayer; the load average sampled with the run; and a median-of-windows comparison
rather than an endpoint-to-endpoint one.

### 6.5 The caveat, stated plainly: the client is synthetic

The test client is a Metal program **we wrote**: it creates a device, a queue, a buffer, a texture
and an internal drawable, blits a deterministic 750 × 1624 BGRA8 picture and presents it. It is
not CoreAnimation, it is not SpringBoard, and it does not draw the iOS UI. Therefore:

- the experiment measures the **mechanism** (discovery, serialisation, transport, host execution,
  publication) and **cannot** measure the rendered UI, CA's command mix, blur/compute passes,
  colour management or the delivered frame rate;
- a green result does **not** mean "Metal works for the port"; it means "the seam this port would
  use for a Metal device is real, and the host side of it works";
- the gap between this client and CoreAnimation is exactly M3 (shaders) and M4 (CA's renderer and
  present path) — the two gates this experiment deliberately does not claim.

This is also why the stage record ([`docs/stage29-metal-translation.md`](../stage29-metal-translation.md))
says, in its own words, that the client is synthetic: so that no later reader can quote a green
M2 as a CA result.

### 6.6 What was measured (M5, 2026-11-21/22)

Five translate runs of 400 frames each, one host, one vCPU pinned, GPU 1, 750 × 1624 BGRA8, host
load 1.2–2.0 on 32 CPUs, a host read-back checksum every 8th row:

| | measured |
|---|---|
| frames | 2,000 / 2,000 `crc_ok=1`, 0 failed, 0 panics |
| median fps | **58.3** (range 57.4–60.1) |
| guest, per frame | fill 2.6 ms, Metal encode 0.19 ms, commit ≈ 7.1 ms, wait ≈ 13.7 ms |
| host, per frame | total 4.2 ms (recv 6.1–6.4 ms is the guest's record+send window; GPU 0.94 ms; read-back+sampled CRC 0.31 ms; publish 2.9 ms) |
| wire | 0 picture bytes on the socket; 1.95 GB per run read from guest RAM by reference |
| picture | full `memcmp` source vs read-back identical; saved frame documented in the stage record |
| control | Replay home screen, same host picture size, measured in the same session (stage 29 §5.6) |

The cost separation is the finding: the Metal API work is 0.2 ms, the host's Vulkan work is 4.2 ms,
and what remains is the guest's own picture generation and the checksum the experiment uses to
prove correctness. The naive picture-on-the-socket wire cost 490 ms. Nothing about the translation
is the bottleneck. What this still does not prove is in §6.3 and §6.5.

## 7. Workspace and branch isolation, and the baseline that does not change

The experiment shares a host with the running GL replay experiment and the HakiOS guest tree, so
isolation is part of the design, not housekeeping (verified here in `tools/metalwire/*.sh`):

| item | experiment | other work on this host | why |
|---|---|---|---|
| source tree | git worktree `/home/lshan/DSH-desktop/iosvm-metal`, branch `experiment/metal-translation` | main checkout `iosvm` | no branch switching, no uncommitted work disturbed |
| guest run tree | `/home/lshan/DSH-desktop/mt-work` — symlinks for immutable parts (kernel, loader, roms, 2.4 GB system image), copies for scripts that write, a private 18 MB root staging | HakiOS tree used **read-only** | our boots cannot disturb another experiment's tree, and their boots cannot disturb ours |
| QEMU | instance 3: monitor **4474**, QMP **4475** | instance 1: 4454/4455 | one QEMU per experiment; no shared monitor/QMP |
| host replay | — | `host_glwire` on **9110** | not run by this experiment; the baseline guest points at **9210** instead, so a metal boot can never reach the delivered replayer; the file's only branch edit is the opt-in selector below |
| translator | TCP **9111**; SHM `/dev/shm/metalwire-frame` | — | port 9111 and its own SHM file; `webview.py` can read either |
| GPU | Vulkan physical device **1** — the V100 in slot 1; `--gpu 1` is the default, `METALWIRE_GPU` overrides | device 0 belongs to the GL replay experiment | one experiment per GPU; the host prints the chosen device and refuses one it was not asked for |
| vCPU | one vCPU, `QEMU_SMP=1`, pinned `taskset -c 26-28`, `cpus=1 fb_beat=0 fbscale=2 metal=1` | other runs pinned elsewhere | reproducible single-core guest numbers; `metal=1` keeps the device away from CA and mediaserverd |
| commits | none from the research role; the lead reviews and commits | — | the work tree is left dirty for review |

**The Replay path keeps its delivered behaviour as the baseline.** The delivered boot
configuration keeps its own root images and its own port (9110); the baseline for comparison is a
separate root-image mode (`prepare-guest.sh replay`) that starts launchd and glwire but no metalwire
client, and points its glremote file at 9210. The translated path is an addition beside the
baseline, not a replacement for it.

One self-correction, made while this note was being written: the branch did edit one file under
`tools/glwire/`, `host_glwire.c`, adding an opt-in `GLWIRE_EGL_DEVICE` selector (default `-1`,
which keeps the old "first EGL device that initialises") so the replay control can be pinned to the
other card on this two-GPU host. It changes nothing unless the variable is set, and the delivered
path never sets it; "untouched" is therefore true of behaviour and defaults, not of the file list.

## 8. Open questions for the lead

1. **M5 baseline re-run.** Is the 13.9 fps / 75 ms figure from 2026-11-17 accepted as the control,
   or does M5 require a fresh replay run on this host in the same load band (T6)? Paired
   measurement implies the latter; it costs a boot and a run.
2. **Picture size.** The synthetic client is 750 × 1624 (the layer size, 4.87 MB). Should the
   published frame be panel-native 1125 × 2436 (what the browser viewer crops), or stay at the
   layer size and let `webview.py --crop` scale it?
3. **M2 acceptance shape.** Given that the client is synthetic and v1 carries the picture, should
   stage 29 gate on correctness only (CRC + byte-identity + visibility), with performance reported
   but not gated? As written, K5 gates on the replay budget (75 ms a frame) rather than on a speed
   claim; is that the right bar for a mechanism experiment?
4. **Branch policy.** `experiment/metal-translation` is outside the documented prefixes
   (`feat/`, `spike/`, …) and spikes are time-boxed at 5 days. Does this branch stay as
   `experiment/…`, or should it be a `spike/` with a hard end date and a `docs/adr/` report?
5. **The library question (M4).** Is M4 blocked by definition — the `.metallib` CA's renderer asks
   for is Apple's and cannot be shipped — or is writing our own library with the same entry-point
   names a path the project is willing to consider? That is a legal/product call, not an
   engineering one, and it decides whether M3 is worth starting.
6. **GPU index stability.** `--gpu 1` assumes the second V100 is always physical-device index 1.
   Should the host select by PCI bus/device (`VK_EXT_pci_bus_info`) and treat the index as a
   fallback, so a driver reordering cannot silently move the experiment onto device 0?
7. **Documentation indexes.** The report is not linked from `docs/research/README.md` and not in
   the README's Documentation table, because those are lead-owned files. Should the lead add the
   two rows on commit?

## Sources

Read for this note (files in this repository are verified here; HakiOS-tree and web sources are labelled in place):

- [`README.md`](../../README.md) — 13.9 fps / 75 ms, ±30% wall clock, the 12.65 KB/frame records,
  the 9.96 MB → 13 KB by-reference history, the "not fast / 24 fps is a target" section.
- [`docs/architecture.md`](../architecture.md), [`docs/phases.md`](../phases.md),
  [`docs/state-of-the-port.md`](../state-of-the-port.md), [`CONTRIBUTING.md`](../../CONTRIBUTING.md),
  [`PROVENANCE.md`](../../PROVENANCE.md).
- [`docs/adr/ADR-002-api-translation.md`](../adr/ADR-002-api-translation.md),
  [`ADR-004`](../adr/ADR-004-virtio-device.md), [`ADR-011`](../adr/ADR-011-gl-context-in-the-wire.md),
  [`ADR-012`](../adr/ADR-012-frame-boundary-not-a-pass-boundary.md).
- [`docs/stage23-verdict.md`](../stage23-verdict.md), [`docs/stage24-blitfast.md`](../stage24-blitfast.md),
  [`docs/stage25-multicore.md`](../stage25-multicore.md), [`docs/stage27-multicore.md`](../stage27-multicore.md),
  [`docs/stage28-boot.md`](../stage28-boot.md), [`tools/glwire/README.md`](../../tools/glwire/README.md).
- [`docs/research/linux-kernel-build.md`](linux-kernel-build.md) — the boot story and the glwire/CA
  measurements this note builds on.
- [`host/metalwire/PROTOCOL.md`](../../host/metalwire/PROTOCOL.md),
  [`host/metalwire/metalwire_protocol.h`](../../host/metalwire/metalwire_protocol.h),
  [`tools/metalwire/`](../../tools/metalwire/).
- HakiOS guest tree (read-only): `docs/metal-plan.md`, `docs/air.md`, `docs/air-report.md`,
  `docs/ru/metal.md`, `docs/boot-args.md`, and the kernel source (`MiniIOMobileFramebuffer.cpp`,
  `userland/src/HakiosMetal.m`) those notes quote.
- Web, for prior art (reported): [UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal),
  [MoltenVK](https://github.com/KhronosGroup/MoltenVK),
  [SPIRV-LLVM-Translator](https://github.com/KhronosGroup/SPIRV-LLVM-Translator),
  [gfxstream](https://github.com/google/gfxstream),
  [vkd3d](https://gitlab.winehq.org/wine/vkd3d),
  [Mesa's Dozen](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/microsoft/vulkan).

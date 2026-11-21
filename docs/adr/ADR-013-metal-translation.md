# ADR-013: intercept Metal in a device plug-in, translate at commit, execute on the host's Vulkan

- Status: **Proposed** on branch `experiment/metal-translation`; this is not a `main` decision,
  and the accept/reject decision is M6.
- Date: 2026-11-21
- Supersedes: —
- Superseded by: —
- Related: [`ADR-002`](ADR-002-api-translation.md) (the design this makes concrete),
  [`ADR-004`](ADR-004-virtio-device.md) (the long-term transport),
  [`ADR-011`](ADR-011-gl-context-in-the-wire.md) and
  [`ADR-012`](ADR-012-frame-boundary-not-a-pass-boundary.md) (what the GL wire taught about
  identity and frame boundaries), `docs/research/metal-translation-feasibility.md`,
  `host/metalwire/PROTOCOL.md`, `tools/metalwire/`, `guest/metal-plugin/`.

## Background

The port draws SpringBoard through a host GL replay path (`tools/glwire/`): the guest records
CoreAnimation's OpenGL ES calls, the host replays them on a real GL device and publishes the frame
to the browser. It runs at **13.9 fps / 75 ms a frame** in the delivered one-vCPU setting, and the
measurements say the ceiling is the guest's CPU, not the host and not the wire (records are
12.65 KB a frame; host replay work is ~1.4 ms; virtio-gpu moved the transport and changed nothing,
1.80 → 1.85 fps).

ADR-002 chose a different long-term route: replace the Metal driver at the API level — a guest Metal
device (L3), a transport (L2), a host Vulkan backend (L1) — so that the guest records commands
instead of executing them, and the host's GPU does the work. That decision has been the project's
direction since 2026-10-31, but the L3 half (`guest/metal-plugin`) never existed in this repository,
and Metal has never been exercised in this guest.

Three facts make a smallest real version possible now (all measured; sources in Evidence):

1. The port already publishes an empty `IOAcceleratorES` service, and boot-arg `metal=N` makes it
   name `HakiosMetal`/`HakiosMetalDevice`; Metal's own loader then opens
   `/System/Library/Extensions/HakiosMetal.bundle`, requires a `_MTLDevice` subclass that declares
   `MTLDevice` and `MTLDeviceSPI`, and instantiates it with the accelerator port.
2. A pure-Metal client can therefore get a device, a queue and a command buffer without any Apple
   driver, and the smallest interesting command — a buffer→texture blit followed by a present — has
   an exact Vulkan equivalent and needs no shader.
3. CoreAnimation cannot be that client here: with the device advertised, its Metal renderer stops
   on a missing `.metallib`, and behind that sits the AIR→SPIR-V compiler, which does not exist in
   this repository. That is M3/M4 work and is out of scope for this decision.

## Decision

Take the ADR-002 shape and build the smallest real version of it end to end, as an **experiment**
on its own branch, with the Replay path's delivered behaviour unchanged.

1. **Interception point.** A standalone Metal device bundle
   (`guest/metal-plugin/`), registered through the port's existing discovery path: the kernel's
   `IOAcceleratorES` service names it (`MetalPluginName`), the class subclasses Metal's private
   `_MTLDevice` and declares `MTLDevice` + `MTLDeviceSPI`, and it is instantiated with
   `-initWithAcceleratorPort:`. It is installed at `/System/Library/Extensions/HakiosMetal.bundle`
   (a symlink to `/redwood/lib/HakiosMetal.bundle`) in the experiment's root image only. Process
   visibility stays on the port's own gate: boot-arg `metal=1` plus a
   `/redwood/lib/HakiosMetal.clients` file naming `metal_frame` (and `metal_probe`), **never**
   `metal=2/3` in a boot that also starts the window server.
2. **Translation boundary.** Calls are recorded, not translated, in the encoders; the unit of
   translation is the **`MTLCommandBuffer` at `commit`**, and `waitUntilCompleted` blocks for one
   acknowledgement per frame. No Vulkan header, library or object is linked into the guest.
3. **Subset v1.** Device, command queue, command buffer, `MTLBlitCommandEncoder` with one
   `copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:`
   copy, `presentDrawable:` / the present, buffers (host-visible staging), textures (optimal
   tiling, BGRA8/RGBA8), an internal `<MTLDrawable>` (the device exposes `-newDrawableWithTexture:`
   because this user space has no `CAMetalLayer`), `commit`, `waitUntilCompleted`, and object
   lifetime. Deliberately absent: render/compute encoders, libraries, pipeline states, shaders,
   `IOSurface`, `MTLSharedEvent`, swapchains.
4. **Translator source.** Our own recorder and host executor, written for this experiment. No
   third-party translator is imported: nothing off the shelf translates Metal to Vulkan graphics
   (see Options weighed); the bundle is our own implementation, and the HakiOS tree is a reference
   for the runtime registration technique and the AIR work; the closest prior art (UniXNU's
   `metal2vulkan`/AdrenoMetal) is cited, not vendored.
5. **Transport.** Reuse glwire's shape for the calls: TCP, one connection, guest → host
   `10.0.2.2:9111`, one acknowledgement per frame, contract in
   `host/metalwire/metalwire_protocol.h` (shared by both ends, never duplicated). The picture is
   **not** carried inline: the plug-in asks the port's kernel for the physical segments behind the
   buffer (`hakios_vtop`, syscall 556 — the call `tools/glwire` already uses) and the host maps
   QEMU's RAM file and reads the same bytes (`metalwire_host --ram`). The inline-body message stays
   as the fallback for a buffer that is not resident. The host publishes each frame into glwire's
   shared-memory format, so `tools/glwire/webview.py --shm /dev/shm/metalwire-frame` serves the
   translated frame unmodified. **Not** virtio-gpu in this experiment: stage 23 measured zero gain
   from it, and the transport is not the bottleneck this decision is trying to move. The decision
   records the by-reference choice because its absence was measured: the inline version of this
   same wire ran at 1.94 fps and the by-reference one at a 58.3 fps median (Evidence).
6. **Host executor.** Headless Vulkan on an explicitly chosen physical device
   (**device 1**, the V100 in slot 1; device 0 belongs to the GL replay experiment), transfer-capable
   queue, `vkCmdCopyBufferToImage` → `vkCmdCopyImageToBuffer`, fence wait, read-back, CRC-32
   comparison with the guest's, per-frame CSV, SHM publication, no window and no swapchain.
7. **Client.** A controlled Metal program (`metal_frame`) that creates the device through
   `MTLCreateSystemDefaultDevice()`, fills a deterministic 750 × 1624 BGRA8 picture, blits it into a
   texture, presents and waits. It is **synthetic on purpose** and is not CoreAnimation; the
   experiment's claims are bounded accordingly (see Consequences).
8. **Baseline and isolation.** The Replay path keeps its default behaviour: no change to the
   delivered boot configuration, its port (9110) or its root images. The only edit on the branch
   under `tools/glwire/` is an opt-in `GLWIRE_EGL_DEVICE` selector (default `-1` = the old "first
   EGL device that initialises"), so a baseline run can be pinned to the other card; it is never
   set on the delivered path. The experiment lives on branch `experiment/metal-translation` in the
   worktree `/home/lshan/DSH-desktop/iosvm-metal`, boots its own QEMU instance 3 (monitor 4474,
   QMP 4475) with its own guest-run tree `/home/lshan/DSH-desktop/mt-work`, selects GPU 1, listens
   on 9111 and its own SHM file, and points a replay-baseline guest at 9210 so the two experiments
   cannot reach each other's services. The research role commits nothing; the lead reviews and
   commits.

## Options weighed

| Option | Good | Bad | Verdict |
|---|---|---|---|
| **Device plug-in through `IOAcceleratorES` + commit-level translation (chosen)** | Metal's own supported seam; per-process gate; no kernel change (the service and boot-arg exist); no Apple file; testable in QEMU; isolates transport, object lifetime and Vulkan execution from shaders | implements only the object graph a client asks for; says nothing about CA coverage until M3/M4 | **chosen for M2** |
| Status quo: keep only the GL replay path | 13.9 fps delivered; no new risk; no new code | leaves ADR-002 unproven and the Metal question unasked; host GL replay is the only GPU use | keep as the **baseline, unchanged** |
| Full Metal device + AIR→SPIR-V + CA renderer now | the only route to CoreAnimation on Metal | three subsystems, months each; blocked first by Apple's missing `.metallib`, then by a compiler that does not exist; an experiment cannot carry it | out of scope (M3/M4 gates) |
| Kernel driver: implement the accelerator and IOGPU submit ABI | guest drivers could submit directly | `IOGPU` alone is 36 classes/711 methods of ABI; no AGX on x86; not needed to move commands to a host GPU | rejected (ADR-002's no-emulation stands) |
| Translate Metal to the existing GLES in the guest | reuses the working GLES | same object graph plus AIR→GLSL; GLES cannot express compute or argument buffers; strictly more work | rejected |
| virtio-gpu (2D, or 3D/Venus) as the transport now | the long-term shape (ADR-004); one transport for the port | stage 23: zero gain, the guest never waits; Venus is a Vulkan driver the guest does not have | deferred to ADR-004's own work |
| Import UniXNU's `metal2vulkan` runtime | a complete Metal→Vulkan object graph, demonstrated composing a tablet's system UI | in-guest Turnip/Adreno backend and their kernel ABI; a full runtime where the experiment needs a minimum; does not put a frame on this host's V100 | cited as prior art, not adopted |
| Translate the GL replay stream to Vulkan instead | no Metal needed; a portability win for the existing path | does not run any Metal client; answers a different question | out of scope |

## Consequences

**Positive**

- The seam ADR-002 assumed is demonstrated upstream (HakiOS booted its own device this way) and is
  re-proven where it matters for us: Metal's own loader, in this guest, with our class, no Apple
  driver and no Apple binary — as an M2 result, not an assumption.
- The experiment separates the three questions that used to be one: *can commands cross and execute
  on the host* (M2), *can shaders be compiled* (M3), *can CoreAnimation's renderer be satisfied*
  (M4). A failure in M2 is now diagnosable without a shader compiler in the way.
- The host side is testable on **lavapipe** with no GPU and no QEMU, so it can be a CI gate; the
  guest side is testable in QEMU with no GPU, as the HakiOS tree already did for its device.
- The translated picture appears in the existing browser viewer without changes to `webview.py`,
  because the SHM format is glwire's.
- **The measured end-to-end result (2026-11-21/22):** 5 × 400 frames, 2,000/2,000 `crc_ok=1`, 0
  panics, median **58.3 fps** for the synthetic 750 × 1624 client (guest 14 ms a frame, host
  4.2 ms), with 0 picture bytes on the socket. The same client over the inline-picture wire is
  1.94 fps, and the by-reference move is the difference — the transport decision above is
  evidence-driven, not stylistic.

**Negative**

- The coverage is small and must never be described as "Metal works": it is a blit and a present.
  CoreAnimation is not exercised, and the delivered 13.9 fps is unchanged.
- The by-reference upload depends on the port's `hakios_vtop` syscall and on the host knowing the
  RAM file and its physical base; a guest kernel without the call falls back to the inline picture
  path (measured: 1.94 fps). It also means the host must be started with `--ram` in translate
  boots: the plug-in cannot know, and its frames fail loudly rather than silently copying.
- The checksum that proves correctness costs guest CPU (a full byte-wise CRC was 48 ms a frame; a
  slice-by-8 CRC 24 ms; sampling every 8th row 6 ms). The reported 58.3 fps uses the sampled
  checksum; the full-picture proof is one `--byte-check` run (`memcmp`), not every frame. The
  stage record states which stride each number used.
- The CRC-32 check proves byte-identity, **not** GPU execution: a host `memcpy` would pass it. The
  device-side oracle that would fix this is future work; the claim is worded accordingly.
- A plugin that answers wrongly can crash Apple's clients (the upstream log shows the window server
  and `mediaserverd` failing when shown a device that cannot draw), so unknown selectors are
  absorbed and `metal=2/3` is avoided in UI boots.
- The experiment branch is outside the documented branch prefixes and can rot; M6 must resolve it.

**Neutral**

- The protocol is versioned and private (v1, `MW_VERSION`), not a public ABI. Both ends include the
  same header; a wire change rebuilds both.
- GPU 1 is shared with the other experiment on this host; numbers are paired and load-banded.

## Evidence

Measured, with the sources the research note cites:

- **Delivered baseline:** 13.9 fps / 75 ms a frame (one vCPU, `gkbf=1` + `ack_only` + `fb_noblit`;
  picture byte-identical to the 9.09 fps baseline), and ±30% wall-clock movement with host load —
  therefore paired runs only (`README.md`; `docs/stage24-blitfast.md`).
- **Transport is not the bottleneck:** stage 23's same-root A/B, 1.80 → 1.85 fps (+2.8 %, inside
  noise), 584 pushes at 7.3–8.3 ms each, guest wait 5 µs of a ~245 ms frame
  (`docs/stage23-verdict.md`).
- **Discovery works upstream:** with `metal=1` the class registers in `backboardd` and
  `mediaserverd`, `-initWithAcceleratorPort:` returns an object, then Metal asks
  `-featureProfile`, `-maxBufferLength`, and a command queue; the window server then stops on
  `Assertion failed: (metallib_url != nil), function MetalContext, file ogl-metal.mm, line 3308`
  (HakiOS `docs/metal-plan.md`, "What steps 1 and 2 found").
- **The shader wall, measured:** 456 system modules read, 321 translated to NIR (271 fragment,
  47 kernel, 3 vertex) and compiled, 255 numerically identical to LLVM and drawn; 135
  `air.visible` library functions are not entry points; the 274 modules of
  `AppleMetalGLRenderer.bundle/default.metallib` are all translated (HakiOS `docs/air.md`,
  `docs/air-report.md`).
- **The object graph, measured:** `AGXMetalA11` 53 classes/1,158 methods; `IOGPU` 36 classes/711
  methods; `_MTLDevice` 549 methods/36 ivars; `AGXA11FamilyDevice` overrides 236 (HakiOS
  `docs/ru/metal.md` §4).
- **The glwire precedent for the wire:** ten megabytes of records a frame became 13.6 KB with the
  picture passed by reference, handover 99 µs, host dispatch 0.03 ms (`tools/glwire/README.md`);
  the replay path's records are 12.65 KB a frame, 0.26 % of the picture (`README.md`).
- **The trap this ADR refuses to repeat:** the vkbridge probe printed 8/8 `[ok]` with its blit
  replaced by a host `memcpy`, so "the bytes are right" is not "the mechanism is established"
  (`docs/phases.md` §M2, review findings).
- **Measured on the experiment branch (2026-11-21/22), stage 29:** the seam loads in this guest
  (`metal=1`, clients file, a device for accelerator port 2819); 2,000/2,000 frames `crc_ok=1` on
  the V100 in slot 1 (PCI `0000:05:00.0`); 5 × 400 frames, median 58.3 fps (57.4–60.1), guest
  fill 2.6 ms + Metal encode 0.19 ms + commit 7.1 ms + wait 13.7 ms, host total 4.2 ms; 0 picture
  bytes on the socket; full `memcmp` between the uploaded source and the read-back identical; the
  same client with the inline picture at 1.94 fps; the Replay control measured in the same session
  (§5.6 of the stage record). What these numbers do not prove is in the stage record §5.5 and §7.

## Follow-up

- M2 lands the code; its evidence is appended to the stage record, and a premise this ADR got
  wrong is corrected with an **Amendment** section (ADR-011's practice), never a silent edit.
- M3 (AIR→SPIR-V for one system shader) and M4 (CA's renderer library) are separate gates; this
  ADR does not authorise them, and M4 cannot start before the "which `.metallib`, and may we
  supply it" question is answered.
- M6 updates this ADR's status to Accepted/Superseded with the M2–M5 evidence.
- Open questions for the lead are listed in `docs/research/metal-translation-feasibility.md` §8
  (baseline re-run, picture size, branch policy, GPU selection by PCI id, documentation indexes).

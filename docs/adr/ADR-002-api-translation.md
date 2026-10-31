# ADR-002: translate Metal at the API level; do not emulate Apple's GPU

- Status: Accepted
- Date: 2026-10-31
- Supersedes: —
- Superseded by: —

## Background

SpringBoard's first frame needs a GPU. Two ways to provide one: emulate Apple's GPU (AGX) well
enough that Apple's own user-space driver drives it, or replace the driver's API surface so that
Metal calls go somewhere else.

Apple's GPU instruction set is not public and has no community reverse-engineering effort
comparable to what Qualcomm's Adreno has (Turnip, freedreno, `ir3`). AGX is the single most
expensive asset Corellium owns, and it took a team years.

## Decision

Translate **Metal at the API level**, in the guest's user space, and execute the translated
commands on the host's GPU through Vulkan.

Three layers, each of which knows nothing about the others' language:

- **L3** `guest/metal-plugin` — implements `MTLDevice`/`MTLCommandQueue`/`MTLCommandBuffer`/
  `MTLBlitCommandEncoder` and, later, the render encoder. Turns Metal calls into either encoded
  Vulkan commands or an abstract command stream.
- **L2** the transport — moves that stream across the VM boundary. It does not translate.
- **L1** `host/vkbridge` — owns Vulkan objects, replays commands, presents, signals fences. It
  does not know what a Metal command buffer is.

## Options weighed

| Option | Good | Bad |
|---|---|---|
| Emulate AGX MMIO + command stream | guest runs unmodified Apple drivers | instruction set is secret; years of work; Corellium-class effort |
| Translate Metal → Vulkan in guest user space | uses two documented APIs; host backend is replaceable (Vulkan, D3D12, Metal-on-Mac) | must cover the Metal surface CoreAnimation actually uses; AIR shaders still need compiling |
| Translate Metal → Adreno directly (what HakiOS does) | no Vulkan in the middle | ties the project to one GPU vendor; on x86 hosts there is no Adreno |
| Fork Corellium's approach | proven | not available |

## Consequences

**Positive**

- The hard part (a GPU) becomes a compiler problem (a shader translation) plus a protocol
  problem (moving commands), both of which can be unit-tested without a GPU at all.
- `host/vkbridge` runs against **lavapipe**, so most of the project is testable on a CI runner
  with no GPU.

**Negative**

- Coverage is the risk. CoreAnimation's private paths (`MTLSharedEvent`, `IOSurface` sharing,
  the present path itself) are the unknowns, and they are what M2's first deliverable
  (`--screenshot` of a guest-drawn red surface) is designed to falsify early.
- Shaders. Metal's `.metallib` carries AIR bitcode; something has to turn that into SPIR-V.
  That is M3 and it is the second pivot point.

## Evidence

- HakiOS proves the API-replacement approach can reach a rendered home screen on a real device:
  a device Metal loads, buffers and libraries work; drawing is "not yet" (`README.md` capability
  table). What is missing there is the object graph and the submit ABI — the same work we do, with
  a different backend.
- HakiOS's `docs/air.md` measures the compiler third on real system `.metallib`s: 456 modules
  read, 321 translated to NIR, 255 numerically identical to LLVM. A shader frontend at this
  quality is a solved shape of problem, not an unknown one. We would need SPIR-V rather than NIR.

# ADR-004: a custom virtio device, not standard virtio-gpu

- Status: Accepted
- Date: 2026-10-31
- Supersedes: —
- Superseded by: —

## Background

The guest needs to hand GPU work to the host. There is an existing answer: `virtio-gpu`, with
Venus (Vulkan) or VirGL (OpenGL) on top. Both are designed for a guest that runs a **standard
driver** speaking a **standard protocol** to a host that understands it.

Our guest does not. The guest side is `guest/metal-plugin`, written by us, talking to a device we
also write. There is no third-party guest driver to keep compatible with, and no third-party host
implementation to stay bug-compatible with.

## Decision

Define `virtio-iosvm-gpu`: a virtio-compatible device with our own feature bits and our own
shared-memory ring, plus a doorbell. Protocol in `proto/iosvm_gpu.h`, versioned and frozen per
milestone.

- Synchronisation: single-producer/single-consumer ring, plus `eventfd` for the doorbell
  (guest → host kick, host → guest completion). No fine-grained locking.
- Memory: `dma-buf` where the kernel offers it (Linux 5.4+), otherwise a guest-allocated region
  mapped on the host via `memory-backend-memfd` plus an offset table.
- Ops (v0.1): `CREATE_BUFFER`, `CREATE_IMAGE`, `FREE`, `WRITE`, `SUBMIT`, `FENCE_WAIT`,
  `PRESENT`, `SCREENSHOT`.

## Options weighed

| Option | Good | Bad |
|---|---|---|
| Standard `virtio-gpu` + Venus | reuse a maintained protocol; possible future interop | Venus means a Vulkan *guest driver*; our guest has Metal, not Vulkan. The protocol is a poor fit and we would be constrained by its error cases |
| Standard `virtio-gpu` + VirGL | mature | OpenGL-shaped, and we need Vulkan semantics (barriers, descriptors) |
| Our own device | exact fit, ~2–3 k lines, fully controllable, easy to version | we own the protocol, its bugs and its documentation |
| Shared memory with no device (purely a side channel) | simplest | no doorbell discipline, no fence model, nothing to test against, host/guest coupling becomes implicit |

## Consequences

**Positive**

- The protocol can be exactly what the translation layer needs, including a `SCREENSHOT` op — a
  composite, synchronous read-back that no standard GPU protocol offers and that our acceptance
  criteria depend on.
- Because the protocol does not try to be a GPU, `tests/fake-gpu/` can implement it in a few
  hundred lines and validate every byte without QEMU, a kernel or a GPU.

**Negative**

- We must defend the protocol: version negotiation, unknown-op rejection, bounds checks on every
  descriptor, and test coverage for each. A protocol bug in a VM boundary is a security bug in
  both directions.
- No ecosystem. If someone later wants to run a real Vulkan driver in the guest, this device does
  not help them. That is out of scope and recorded as such.

## Follow-up

- Version comparison happens **before** any payload is read; an old guest must reject a new host
  cleanly (see `docs/zh/design-baseline.md` §5.4).
- Every struct is packed with explicit padding; `sizeof` assertions live in the unit tests.
- The `SCREENSHOT` op is the acceptance path for M2 and M3; it must never be asynchronous.

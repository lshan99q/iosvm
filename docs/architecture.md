# Architecture

> Status: M0. This document describes the shape we are building toward; the milestone that
> delivers each piece is in `docs/phases.md`. Decisions are in `docs/adr/`.

## The whole thing in one diagram

```
┌───────────────────────────────────────────────────────────────────┐
│ Guest (inside QEMU)                                               │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │ iOS user space — supplied by the contributor, never here     │  │
│  │   launchd · SpringBoard · CoreAnimation · Metal.framework    │  │
│  └───────────────┬──────────────────────────┬──────────────────┘  │
│                  │ Metal API                │ IOKit / syscall     │
│  ┌───────────────▼──────────────┐  ┌────────▼──────────────────┐  │
│  │ guest/metal-plugin           │  │ XNU built from source     │  │
│  │  MTLDevice, MTLCommandQueue  │  │  + our drivers            │  │
│  │  → command serialisation     │  │  (ADR-003)                │  │
│  └───────────────┬──────────────┘  └────────┬──────────────────┘  │
│                  │ shared-memory ring + doorbell                │
│  ┌───────────────▼──────────────────────────▼──────────────────┐  │
│  │ QEMU fork, stock machine model + hw/virtio-iosvm-gpu        │  │
│  │   virtio-blk / virtio-input / virtio-net / virtio-rng        │  │
│  └──────────────────────────┬──────────────────────────────────┘  │
└─────────────────────────────┼─────────────────────────────────────┘
                              │ shared memory / fd passing
┌─────────────────────────────▼─────────────────────────────────────┐
│ Host (Linux)                                                      │
│  host/vkbridge — Vulkan backend                                   │
│    object tables (buffer→VkBuffer, texture→VkImage)               │
│    command replay, fence signalling, present                      │
│  GPU: radv / nvidia / lavapipe                                    │
└───────────────────────────────────────────────────────────────────┘
```

## Layers and what each one refuses to know

| Layer | Owns | Must not know |
|---|---|---|
| L0 host GPU | executing Vulkan | anything about Metal |
| L1 `host/vkbridge` | Vulkan objects, replay, present, fences | Metal semantics |
| L2 transport (`proto/`) | serialisation, rings, memory sharing | what the commands mean |
| L3 `guest/metal-plugin` | the Metal API surface → commands | how they are executed |
| L4 user space | SpringBoard, CoreAnimation | — |
| L5 XNU + our drivers | scheduling, memory, devices | GPU semantics |
| L6 QEMU | virtual hardware | GPU semantics |

The L1/L3 split is the load-bearing decision: because neither end speaks the other's language,
`vkbridge` can be replaced (D3D12, Metal-on-Mac) without touching the guest, and the guest can
be replaced without touching the backend.

## A frame's life

```
1. SpringBoard submits a CA::Render::Update
2. CoreAnimation encodes draws through Metal                      [L4 guest]
3. metal-plugin intercepts MTLCommandBuffer::commit               [L3 guest]
4. translates the buffer into a Vulkan command stream             [L3 guest]
5. writes it into the shared ring, kicks the doorbell             [L2 guest]
6. QEMU sees the kick, forwards to vkbridge                       [L6]
7. vkbridge replays vkCmd*, submits to the queue, waits the fence  [L1 host]
8. fence completion signals an eventfd back into the guest        [L1 → guest]
9. vkbridge blits the finished image into the swapchain           [L1 host]
10. tools/screendump.sh grabs the window                          [tooling]
```

## Why this shape and not another

- **No GPU emulation** (ADR-002): Apple's instruction set is not public; translation at the API
  level is the only route that a small team can test without a device.
- **Our own kernel, not the IPSW's kernelcache** (ADR-003): a closed kext that refuses to bind is
  a dead end with no stack trace; our own driver is a bug we can fix.
- **A custom virtio device, not standard virtio-gpu** (ADR-004): the guest side is our own plugin,
  so we do not need to satisfy a guest driver ecosystem's protocol.
- **HakiOS's QEMU path as the baseline** (ADR-008): it already boots a self-compiled XNU with its
  own drivers on a stock `virt` machine, and reaches a home screen with a user-supplied userland.
- **No activation code, ever** (baseline ADR-006): the boot path never enters recovery mode, so
  there is nothing to bypass. Adding a bypass would be both illegal and pointless.

## The three hard walls

Recorded here so they are not rediscovered as surprises:

| Wall | Consequence | Where it is handled |
|---|---|---|
| No Secure Enclave | no keychain, no DRM, no App Store, no Apple services | out of scope, ADR-005 baseline |
| FairPlay-encrypted `.ipa` | App Store apps cannot be run, ever | documented in `docs/phases.md` non-goals |
| Apple GPU instruction set | no hardware emulation | ADR-002 translates at the API level |

The fourth wall, the one that decides whether v1.0 ships, is **AIR → SPIR-V**, and it is M3.

# Phases

Every acceptance criterion is a command, not an opinion. A phase is done when its command exits
0 on a clean machine, and `docs/demo/` holds the evidence.

Estimated for one person, part-time; the ranges are wide because the last third of this has no
precedent. See `docs/adr/ADR-008-hakios-baseline.md` for why M1 is narrower than the original
design assumed.

| Milestone | Name | One-line criterion | Estimate | Public action |
|---|---|---|---|---|
| **M0** | foundation | `make test-open` green with no Apple file, and a boot harness that reproduces the upstream kernel start | 2–3 weeks | repo + README |
| **M1** | user space boots | `ps aux` inside the guest shows `launchd`, then `WindowServer` | 2–3 months | blog ① |
| **M2** | GPU channel | `iosvm-probe` fully green; a 512 KiB blit is byte-identical on the host | 2–4 months | **v0.1.0-alpha.1** + blog ② |
| **M3** | first frame | a captured PNG with ≥ 64 unique colours | 3–6 months | **v0.1.0** + blog ③ |
| **M4** | usable home screen | can unlock, scroll, open Settings | 3–6 months | **v1.0.0** |

```
M0 ──→ M1 ──┬──→ M2 ──→ M3 ──→ M4
             │      ↑
             └── D1' (does iOS 26 arm64e user space boot?)
```

## M0 — foundation

Goal: prove the machine can run a real XNU before writing a line of our own kernel code, and put
the compliance machinery in place before the first feature.

Deliverables — `[x]` means it exists and was exercised on 2026-10-31, not that it is perfect
- [x] Repository skeleton, three licenses, `PROVENANCE.md`, `NOTICE`
- [x] `tools/provenance-check.py` + `tools/check-spdx.py` + `tools/doctor.sh` in CI
- [x] `tools/unpack-ipsw.sh`: IPSW → `firmware/` + `userland-raw/` (Linux only, ADR-009)
- [ ] `tools/make-images.sh`: builds `userland/system.apfs` — **blocked**: needs the APFS writer (M1)
- [ ] `qemu/` submodule pinned — **not vendored yet** (M2); `make submodule` fails rather than lie
- [x] `tools/qemu-open.sh` + `make qemu-open-check`: a boot smoke test with no Apple file
- [ ] `make run` reaches a root shell — **not built yet** (M0 tail)
- [x] README states what the contributor must supply and what they must not commit
- [x] `docs/m0-host-report.md` — measured; regenerating it from a script is `tools/qemu-open.sh`
      (M0 tail). `tools/doctor.sh --json` reports the host, not boot timings, and does not own it.

Acceptance

```sh
make test-open                                   # exit 0, no Apple file present
make unpack IPSW=~/your.ipsw                     # writes firmware/ and userland-raw/
tools/unpack-ipsw.sh ~/your.ipsw                 # or dry-run it first
make run
# expected: Darwin Kernel Version ... ; root shell prompt
```

`make run` is the part that does not exist yet; it exits 1 and says so. The upstream equivalent
(HakiOS `make qemu-open`) was run by hand instead — see the status line below.

Exit condition: three consecutive clean boots to a prompt, plus `make test-open` green in CI.

Unknowns
1. Whether the QEMU fork builds cleanly on Ubuntu 24.04 with gcc 13 (upstream documents macOS).
2. Whether the pinned XNU release and the contributor's IPSW agree on user-space ABI.

Status 2026-10-31: the boot is now a **script, not an anecdote** —
`make qemu-open-check HAKIOS=~/src/hakios` asserts five markers and passes in ~8 s
(`tools/qemu-open.sh`, pinned upstream commit in `.github/workflows/upstream-boot.yml`). The
upstream boot is **reproduced on the host** — HakiOS's prebuilt kernel
(`xnu-8796.121.2`, built by that project from XNU source) reaches BSD initialisation in 6.4 s of
emulated boot and then panics on `launchd`, because the test ramdisk carries no user space
(`docs/m0-host-report.md`). Building that kernel here failed for lack of the theos toolchain, so
"self-compiled on this host" is not yet true. The kernel half of M0 is de-risked; the remaining
work is the harness, the image tooling, and a kernel build of our own.

## M1 — user space boots

Goal: get a real user space to `WindowServer`, and settle the remaining research question: does
iOS 26 arm64e boot where Darwin 16.5 does?

Deliverables
- [ ] `tools/check-licenses.py`: every file under `qemu/` keeps its original header, and nothing
      under `xnu/` is anything but a `*.patch` (the promise made in ADR-005)
- [ ] Device tree self-consistent enough that `launchd` does not panic on platform expert
- [ ] APFS images mount with our block driver
- [ ] `virtio-net` + SSH into the guest (observability before optimisation)
- [ ] `docs/adr/D1-prime-userland-version.md`: 16.5 vs 26, with measurements
- [ ] Failing screenshot of `ps aux` in the guest

Acceptance

```sh
make run
ssh -p 2222 root@127.0.0.1 'ps aux | grep -c WindowServer'   # >= 1
```

Exit condition: `WindowServer` exists as a process (it may crash immediately).

Unknowns
1. What minimum set of device-tree nodes satisfies the platform expert? No published answer.
2. What breaks first between Darwin 16.5 and iOS 26 arm64e — PAC, SPTM-era boot, or userland
   size?
3. Can an `IOSurface` be shared across the VM boundary without a copy?

Risks
- **R1'**: iOS 26 arm64e does not boot with our kernel at all. Mitigation: keep the 16.5 path
  green so there is always a working demo, and time-box the 26 investigation to 5 days per round.
- **R5**: a contributor without an IPSW can do M0 but not M1. Mitigation: `tools/` exports logs
  and a reproducible recipe so a contributor with an IPSW can help someone without one.

### Decision gate D1' (replaces the original D1)

| Question | Test | Yes | No |
|---|---|---|---|
| Does HakiOS's QEMU path reproduce here, with no Apple file? | `make qemu-open` equivalent | M0 continues | fix the host; this is not research |
| Does the same path boot **iOS 26 arm64e**? | your own iOS 26 IPSW through the same tooling | M1 is a port | 16.5 stays the demo; 26 gets a 5-day spike with a written report either way |

The original D1 (reverse-engineer `AppleDisplayPipe` vs hook the present path vs our own driver) is
**closed by ADR-008**: our own driver is the route, and HakiOS already proved it.

## M2 — GPU channel

Goal: falsify or confirm the central assumption — that a Metal command stream can cross the VM
boundary and produce byte-identical results on the host GPU. ⭐ This milestone is publishable even
if M3 never lands.

Deliverables
- [x] `proto/iosvm_gpu.h` v0.1 (ring, op set, fence semantics) — in `iosvm-proto`, pinned as `proto/`
- [x] `host/vkbridge`: object tables, replay, fences — in `iosvm-vkbridge`, pinned as `host/vkbridge/`;
      8/8 probe checks on lavapipe and on a V100, and the 512 KiB blit is byte-identical
- [ ] `hw/virtio-iosvm-gpu`: doorbell → vkbridge
- [ ] `guest/metal-plugin`: `MTLDevice`, `MTLCommandQueue`, `MTLCommandBuffer`,
      `MTLBlitCommandEncoder` only — **the only part of M2 still open**, and the only one that needs
      a Mac (`ci/mac.md`)
- [ ] `iosvm-probe`, one line per check
- [ ] `tests/fake-gpu`: strict in-process interpreter (out of bounds, unknown opcode, fence
      reordering → fail)
- [ ] `v0.1.0-alpha.1`

Acceptance

```sh
make test                # unit + fake-gpu, green, no Apple file
ssh -p 2222 root@127.0.0.1 'iosvm-probe'
# [ok] proto handshake
# [ok] create buffer 1 MiB
# [ok] write 1 MiB, readback identical
# [ok] blit copy 512 KiB
# [ok] blit fill 0..999996
# [ok] 40 command buffers in order
# [ok] fence signal/wait ordering
```

Exit condition: probe green on lavapipe **and** on a real GPU.

Unknowns
1. Does CoreAnimation present through `MTLBlitCommandEncoder` at all? If it uses a private path,
   a green probe can still be irrelevant to the desktop — which is why M2 ends with a
   `--screenshot` of a guest-drawn red surface.
2. Can `MTLSharedEvent`'s cross-process semantics be approximated with an `eventfd`?
3. Does `dma-buf` work inside QEMU without `-object memory-backend-memfd`?

Risks
- **R3 (synchronisation)**: single queue, single thread first. Every performance compromise needs a
  `fake-gpu` test behind it.
- **R2 (shaders)**: deliberately untouched here, so "does the channel work" and "is translation
  complete" stay separate questions.

## M3 — first frame

Goal: CoreAnimation draws *any* frame through Metal onto the screen.

Deliverables
- [ ] `.metallib` → AIR → SPIR-V (or construct Vulkan pipelines directly)
- [ ] `MTLRenderCommandEncoder` subset: pipelines, vertex/index buffers, texture binding
- [ ] Stable `--screenshot` → PNG
- [ ] `docs/demo/first-frame.png`
- [ ] `v0.1.0` + a 30-second recording

Acceptance

```sh
make run
./tools/screendump.sh
python3 tools/check-frame.py logs/screen.png --min-unique-colors 64   # exit 0
```

Exit condition: three consecutive boots produce a non-uniform frame that a human recognises as
iOS, not noise.

Unknowns
1. Is AIR signed or otherwise checked in our path? **D2: if a check is found, stop and get a legal
   evaluation. Do not route around it.** `docs/air.md` in HakiOS reports no signing step in its
   (Adreno) path — evidence, not proof, for ours.
2. Liquid Glass relies heavily on compute; if CoreAnimation's blur forces compute, M4's shader work
   moves into M3.
3. Colour management: CA expects P3; an sRGB blit will look wrong.

## M4 — usable home screen

Deliverables
- [ ] `virtio-input`: absolute-coordinate touch, power and volume keys
- [ ] Compute shader support
- [ ] Home screen icons, Dock, status bar
- [ ] Gestures
- [ ] Recorded, measured responsiveness (numbers, not adjectives)
- [ ] `v1.0.0`

Acceptance

```sh
make run
# by hand: wake, swipe, open Settings, go back
./tools/screendump.sh
```

Exit condition: five minutes of interaction without a crash; `docs/demo/demo.mp4` recorded.

## Non-goals for v1.0

Recorded so they are not quietly added:

| Not doing | Why |
|---|---|
| Activation, Apple ID, iCloud, App Store | no Secure Enclave; physically impossible |
| FairPlay `.ipa` from the App Store | key derived from device DRM identity |
| Cellular baseband, cameras, Bluetooth fidelity | unrelated to the first frame |
| Beating Corellium's feature set | no SLA, no multi-tenancy, no support contracts |
| Booting a stock Apple `kernelcache` | ADR-003: we boot our own kernel |

## Cadence

| When | What |
|---|---|
| Before every commit | `make test-open` green |
| Every day | one small, demonstrable step; update the issue with the command that proves it |
| Every week | one new `iosvm-probe` line, or one flaky test fixed |
| Every phase | a probe line, a demo capture, one ADR |
| Every milestone | tag + blog post |

The blog cadence is a survival strategy for the one-person-project risk R6 ("builder runs out of
steam", `docs/zh/engineering.md` §7): M2's probe output is a publishable result on its own — "the
first open VM to push a Metal command stream to a host GPU".

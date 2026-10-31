# Phases

Every acceptance criterion is a command, not an opinion. A phase is done when its command exits
0 on a clean machine, and `docs/demo/` holds the evidence.

Estimated for one person, part-time; the ranges are wide because the last third of this has no
precedent. See `docs/adr/ADR-008-hakios-baseline.md` for why M1 is narrower than the original
design assumed.

| Milestone | Name | One-line criterion | Estimate | Public action |
|---|---|---|---|---|
| **M0** | foundation | `make test-open` green with no Apple file, and a boot harness that reproduces the upstream kernel start | 2–3 weeks | repo + README |
| **M1** | user space boots | `ps aux` inside the guest shows `launchd`, then `WindowServer` — **Darwin 16.5 user space** | 2–3 months | blog ① |
| **M2** | GPU channel | `iosvm-probe` fully green; a 512 KiB blit is byte-identical on the host | 2–4 months | **v0.1.0-alpha.1** + blog ② |
| **M3** | first frame | a captured PNG with ≥ 64 unique colours | 3–6 months | **v0.1.0** + blog ③ |
| **M4** | usable home screen | can unlock, scroll, open Settings | 3–6 months | **v1.0.0** |

```
M0 ──→ M1 ──┬──→ M2 ──→ M3 ──→ M4
             │      ↑
             └── D1' closed: target is the Darwin 16.5 user space (docs/adr/D1-prime-userland-version.md)
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

Goal: get the **Darwin 16.5 user space** to `WindowServer`. The version question is closed:
`docs/adr/D1-prime-userland-version.md` targets 16.5 and defers iOS 26 on licensing grounds, not
engineering ones.

Deliverables
- [ ] `tools/check-licenses.py`: every file under `qemu/` keeps its original header, and nothing
      under `xnu/` is anything but a `*.patch` (the promise made in ADR-005)
- [ ] Device tree self-consistent enough that `launchd` does not panic on platform expert
- [ ] APFS images mount with our block driver
- [ ] `virtio-net` + SSH into the guest (observability before optimisation)
- [x] `docs/adr/D1-prime-userland-version.md`: 16.5 chosen, 26 deferred with the reason recorded
- [ ] `tools/make-images.sh` builds `userland/system.apfs` from a 16.5 IPSW (`iPhone10,3`/`iPhone10,6`,
      `20F66`) — the APFS writer, which is the real work here
- [ ] Failing screenshot of `ps aux` in the guest

Acceptance

```sh
make run
ssh -p 2222 root@127.0.0.1 'ps aux | grep -c WindowServer'   # >= 1
```

Exit condition: `WindowServer` exists as a process (it may crash immediately).

Status 2026-10-31: **the data path is proven end to end on Linux, except one piece.**

| Step | State |
|---|---|
| Get the IPSW | **done** — `iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`, 6,165,698,198 bytes, the exact size upstream's unpacker refuses to run without, downloaded from Apple's public CDN |
| Extract it on Linux | **done** — `tools/unpack-ipsw.sh --apply`, 51 s, 48 MB of firmware and 7.2 GB of images; no Mac, no key service, this repository's first run against a real IPSW |
| Read the system volume | **done** — 7-Zip 23.01 opens `096-04088-077.dmg` and yields the APFS volume `SydneyF20F66.D22D221OS.apfs` and its full Darwin tree (`Applications`, `System`, `usr`, `private`, `Library`, …) |
| Turn that tree into `system.apfs` | **done** — 4.7 GB container, `apfsck` clean |
| Get the dyld shared cache out | **done** — from the OS cryptex, with the kernel's own APFS reader (details below) |
| Compose the volume the userland expects | **done** — 6.0 GB: root + cryptex + cache, `apfsck` clean |

The writer was not research: upstream ships one in its `apfs/` tool directory, MIT-licensed, and
`tools/apfs/` now carries `apfs.py` and `mkapfs.py` verbatim with an attribution header and a `NOTICE`
row. `tools/make-images.sh --apply` is a real implementation on top of it:

- the root image's name comes **from the BuildManifest** (`OS`), not from a hardcoded file number;
- it extracts the volume with 7-Zip and repairs the four symbolic links that 7-Zip's "dangerous path"
  policy declines to create — normal on a Darwin volume (`AVFoundation.framework/Frameworks/
  AVFAudio.framework` → `../../AVFAudio.framework`, inside the volume) — while still failing on any
  other extraction error;
- it stores files the way Apple does (`mkapfs.py -z`): small ones whole in the `decmpfs` attribute,
  larger ones in 64 KiB zlib pieces in the resource fork;
- it checks the result twice, because a check that cannot fail is worth nothing: `apfsck` (built from
  `linux-apfs/apfsprogs`) and a 7-Zip read-back through a completely different parser.

Measured on the real archive: **179,361 files, 136,413 directories, 2,324 symbolic links and
1,187,306 data blocks — 15,735 compressed into resource forks, 155,371 in their attribute — into a
4.7 GB container in 125 s.** `apfsck: clean`; the volume reads back as `System.apfs`.

### The cryptex, and why 7-Zip could not do it alone

launchd does not run without the **dyld shared cache**, which on a device lives in a second volume --
the OS cryptex -- grafted at `/System/Cryptexes/OS`. Two things had to be solved:

- **Reading it.** 7-Zip extracts 2.5 GB of the 2.7 GB cryptex and refuses 984 files with
  `Unsupported Method`: those are stored with APFS compression it does not implement, and they include
  a `dyld_shared_cache_arm64` for DriverKit. The kernel's own `apfs_lib` does implement it, so the
  HakiOS tree's `apfs-tool` (which is that code compiled for the host) is used to write back **exactly
  those 984 files, and only those**. `make-images.sh` parses 7-Zip's log and does this automatically
  when `APFS_TOOL` is set; without it, it says the cache may be incomplete instead of pretending.
- **Placing it.** `/System/Cryptexes/OS` in the boot volume is a **symbolic link** to
  `../../private/preboot/Cryptexes/OS`, so the cryptex is copied to the link's *target*, and the image
  keeps the layout the userland expects.

The cache itself is the real thing: 44 files, 3.1 GB, every one carrying `dyld_v1   arm64` —
`dyld_shared_cache_arm64` plus numbered subcaches, `.dylddata`, `.dyldlinkedit` and `.symbols`.

Result on the real archive: **182,786 files, 137,049 directories, 2,326 symbolic links, 1,505,081 data
blocks into a 6.0 GB container in 261 s**, `apfsck: clean`, volume reads back as `System.apfs`, and the
image contains `/sbin/launchd` and `private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld`.

So the data half of M1 is finished — the image now holds everything launchd needs to start. What
remains is the guest half: a kernel that mounts this volume and an init that executes from it.

Unknowns
1. What minimum set of device-tree nodes satisfies the platform expert? No published answer.
2. How much of `userland/build.sh`'s APFS assembly has to be reimplemented to stay Linux-only?
   Upstream's script is the specification.
3. Can an `IOSurface` be shared across the VM boundary without a copy?

Risks
- **R1'** (was: iOS 26 does not boot): **retired.** The version question is settled by D1'; the
  remaining M1 risk is ordinary porting risk, not a research gate.
- **R5**: a contributor without an IPSW can do M0 but not M1. Mitigation: `tools/` exports logs
  and a reproducible recipe so a contributor with an IPSW can help someone without one.

### Decision gates (both closed)

| Gate | Outcome |
|---|---|
| **D1** display path (C1/C2/C3) | closed by **ADR-008**: our own driver, which HakiOS already proved |
| **D1'** which userland version | closed by **`docs/adr/D1-prime-userland-version.md`**: Darwin 16.5; iOS 26 deferred because its OS and System-cryptex images are delivered through Apple's key service |

The iOS 26 path is not lost — `docs/research/d1-prime-arm64e.md` and `docs/research/e2-kernel-port-surface.md`
record what it would take, and the ADR lists the four conditions under which it reopens.

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

Status 2026-10-31: **two of the three halves exist, one of them needs a correction.** `iosvm-proto`
(pinned as `proto/`) passes its cases with no GPU and no Apple file. `iosvm-vkbridge` (pinned as
`host/vkbridge/`) passes eight probe checks on **lavapipe and on a Tesla V100**, and its blit does
go through `vkCmdCopyBuffer`/`vkCmdFillBuffer`.

But an adversarial review showed the probe **proves less than this document first claimed**: a copy
of the repository with the blit replaced by a host `memcpy` still printed all eight `[ok]` lines.
The bytes are right; the mechanism is not established by the comparison. The fix in flight is a
device-side oracle (a shader the host cannot emulate writing a checksum the host then verifies),
plus an assertion that the blitted buffers are not host-visible. Until that lands, the honest
statement is: *the result is correct on two devices, and the code at `src/replay.cpp` does use the
device — but the probe alone does not prove it.*

What is left is the guest: `guest/metal-plugin` and the QEMU device that carries the ring. That
is also the only part of M2 that needs a Mac (`ci/mac.md`).

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

Goal: CoreAnimation draws *any* frame through Metal onto the screen — for the **Darwin 16.5** UI,
which is the flat design of that era. There is no Liquid Glass on this path: no real-time refraction
and blur pass, and a much smaller compute-shader surface. That is the single biggest reason D1'
chose 16.5 (`docs/adr/D1-prime-userland-version.md`).

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
2. Which CoreAnimation blurs this era does reach the GPU, and whether they arrive as compute
   dispatches. Bounded, unlike the Liquid Glass question it replaces: the 16.5 UI uses blur in a few
   places, not as the foundation of the whole design.
3. Colour management: CA expects P3; an sRGB blit will look wrong.

## M4 — usable home screen

Deliverables
- [ ] `virtio-input`: absolute-coordinate touch, power and volume keys
- [ ] Compute shader support, if M3 finds any dispatch in the path (the 16.5 UI needs far less)
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

## Review findings that changed the plan

Two adversarial reviews (2026-10-31) were run over the components the day they were written, and
both found something that mattered:

| Where | Finding | Consequence |
|---|---|---|
| `iosvm-proto` | An **empty ring after a wrap could refuse a record that fits**, permanently: counters were indices in `[0,size)`, so any record larger than the tail gap was rejected even with the whole body free | The ring representation is being changed to cumulative offsets with an explicit padding skip. A protocol that can wedge on a legal sequence is not shippable |
| `iosvm-vkbridge` | The probe **could not tell a GPU blit from a host memcpy** — a patched copy printed 8/8 | A device-side oracle is being added, and the claim in this document was corrected above rather than left standing |
| `iosvm-vkbridge` | The screenshot's error path destroyed a fence, image and command buffer that were still in flight | Fixed with a wait-idle before cleanup |
| both | Several tests could not fail (mutation-tested): a wrap case that never wrapped, an ordering assertion fed already-sorted values, a "refuses and changes nothing" case that skipped the field that changed | Each is being replaced with one that fails when the property is removed |

The lesson is the same one ADR-008 taught: a check that has never been seen to fail is not evidence.
Both reviews ran the "delete the guard and see if the suite notices" experiment, which is now the
standard this project holds its own tests to.

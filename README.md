# IOSVM

**Boot iOS on Linux in QEMU, and render SpringBoard's first frame — all of it open, all of it
reproducible.**

Status: **M0 — foundation.** The VM half is proven upstream (see `docs/adr/ADR-008-hakios-baseline.md`);
this repository is the project that carries it from a framebuffer to a Metal command stream on the
host GPU.

**Target: the Darwin 16.5 user space.** iOS 26 arm64e is deferred, and not for engineering reasons —
that release delivers its OS and System-cryptex images through Apple's key service, so obtaining its
user space would mean asking that service for a key. This project does not do that.
`docs/adr/D1-prime-userland-version.md` records the evidence and the four conditions under which the
decision reopens.

```
$ make run
...
$ ./tools/screendump.sh
$ file logs/screen.png
logs/screen.png: PNG image data, 1179 x 2556, 8-bit/color RGB, non-interlaced
```

That is the v1.0 definition of done. It is not there yet. What is there is written down in
`docs/phases.md`, one acceptance command per milestone.

## You bring the IPSW

This repository **ships nothing of Apple's**: no kernel, no system image, no dyld cache, no
firmware, no font. You supply an IPSW you obtained yourself; `tools/unpack-ipsw.sh` lays it out
locally. Nothing is uploaded, nothing is downloaded for you — `tools/` refuses to fetch Apple
content by design.

Read `PROVENANCE.md` before your first commit and `CONTRIBUTING.md` before your first PR. Three
rules are one-strike vetoes: no Apple binaries in the tree, no code that impersonates a real
device to Apple's servers, no credentials.

## Quick start

```sh
# 1. Host prerequisites (Linux x86_64)                      — works today
sudo apt install build-essential meson ninja pkg-config \
                 qemu-system-arm qemu-efi-aarch64 lld lz4 7zip \
                 mesa-vulkan-drivers vulkan-tools libvulkan-dev libdrm-dev

# 2. The thing you must keep green                          — works today
make test-open          # compliance + SPDX + protocol units + fake-gpu, no Apple file

# 3. Look at your host before blaming the code              — works today
make doctor             # what is missing here, and the apt line for it

# 4. Take the parts out of YOUR IPSW                       — works today (dry run first)
./tools/unpack-ipsw.sh ~/iPhone10,3_16.5_20F66_Restore.ipsw       # dry run: prints, writes nothing
./tools/unpack-ipsw.sh ~/iPhone10,3_16.5_20F66_Restore.ipsw --apply
./tools/make-images.sh                              # dry run; --apply needs the M1 APFS writer

# 5. Boot a guest                                          — NOT YET (M0 tail / M1)
make run
```

Step 5 is the one that does not exist. `make run`, `make host`, `make guest`, `make qemu` and
`make test-gpu` are stubs that say so; `hw/virtio-iosvm-gpu` is M2 work. What *is* proven is
upstream, not here: a self-compiled XNU reaches BSD initialisation in 6.4 s of emulated boot on the
reference host (`docs/m0-host-report.md`).

What works today, exactly: `make test-open` (compliance plus the protocol cases when `proto/` is
fetched), `make proto`, `make test-gpu` (eight probe checks on lavapipe), `make provenance`,
`make style`, `make doctor`,
`tools/unpack-ipsw.sh` (verified end to end against a synthetic IPSW), `tools/check-frame.py`
(verified both ways), the CI gate on every push, and — the interesting one — this:

```sh
# an open-source XNU reaching BSD initialisation under QEMU, no Apple file anywhere.
# needs a checkout of the upstream tree that builds it: make qemu-open-check HAKIOS=~/src/hakios
make qemu-open-check HAKIOS=~/src/hakios
#   [ok] loader banner   [ok] kernel banner   [ok] paged memory
#   [ok] MAC Framework   [ok] BSD half reached pid 1 (expected panic on launchd)
#   qemu-open: PASS
```

Two dependencies are pinned as submodules and are not in a bare clone: `proto/` (the wire protocol,
testable with a C compiler alone) and `host/vkbridge/` (the Vulkan backend). `make submodule`
fetches both at their commits; `make components` builds and tests them.

An adversarial review of the backend found that its probe alone **does not prove the GPU did the
work** — a patched copy with a host `memcpy` still printed every check as `[ok]`. The bytes are
right and the code does use `vkCmdCopyBuffer`; the proof is what is missing, and a device-side
oracle is in flight. `docs/phases.md` records the same thing, because a claim that survives in
only one document is still a claim.

And this is what the guest will mount — a real Darwin 16.5 system volume, built on Linux from a
user-supplied IPSW, checked by `apfsck` and read back through a different parser:

```
$ ./tools/make-images.sh --apply
  tree: 318098 entries, 8.8G
  mkapfs.py: volume 0 "System": 179361 files, 136413 directories, 2324 symbolic links,
             1187306 blocks of data (15735 compressed in their resource fork,
             155371 whole in their attribute)
  mkapfs.py: userland/system.apfs: 1233652 blocks of 4096 bytes, 256 free
  built in 125s, 4.7G
  apfsck: clean
  7z read-back: volume "System.apfs"
```

It takes about eight seconds of wall clock on a 2014 Xeon: the kernel reaches the interrupt
controller at 6.3 s of emulated boot and the expected `launchd` panic ends the run. The kernel is
the upstream project's build, not ours — `tools/qemu-open.sh` says so, and that distinction is
the point of the exercise.

## Repositories

The components have different toolchains and different gates, so they are separate repositories
pinned by this one (ADR-010). A contributor with a C compiler can improve the protocol; one with a
GPU can work on the backend; neither needs a Mac or an iPhone.

| Repository | What | Builds with | Gate |
|---|---|---|---|
| `iosvm` (this one) | docs, ADRs, tools, the compliance gate, the plan | python3, make | `make test-open` |
| `iosvm-proto` → `proto/` | the wire protocol: packed structs, ring, codec, strict fake GPU | a C compiler | `make proto` |
| `iosvm-vkbridge` → `host/vkbridge/` | the host Vulkan backend: object tables, replay, fences, probe | C++20 + Vulkan | `make test-gpu` |

```sh
make submodule     # fetch both components at their pinned commits, and report drift
make components    # build and test both
```

`hw/` and `guest/` stay in this repository until they need their own toolchain (ADR-010 §rules).

## What lives where — planned layout

Only `tools/` and `docs/` exist today; the rest is where the work will go, and `docs/phases.md`
says which milestone creates each one.

| Path | What |
|---|---|
| `hw/` | QEMU devices: `virtio-iosvm-gpu` and the display path |
| `proto/` | The GPU transport protocol (versioned, packed, little-endian) |
| `host/vkbridge/` | Vulkan backend: object tables, command replay, present |
| `guest/metal-plugin/` | Metal API → serialized Vulkan commands, inside the guest |
| `tools/` | Unpack, image building, screendump, provenance check |
| `tests/fake-gpu/` | In-process command interpreter with strict validation |
| `docs/adr/` | Every non-obvious decision, one file each, with evidence |

## Design, in three sentences

Do not emulate Apple's GPU: translate **Metal to Vulkan in the guest's user space**, and carry
encoded commands across the VM boundary over a `virtio` device of our own. Activation is not
bypassed — it is **never entered**, because the system boots flat from a laid-out IPSW and never
sees recovery mode. The hard wall is not activation but the GPU, and M2 (a correct blit through
the whole channel) is a publishable result even if M3 never lands.

Full reasoning: `docs/architecture.md`, `docs/adr/`, and the Chinese design baseline under
`docs/zh/` — `docs/zh/engineering.md` for the architecture and ADRs, `docs/zh/design-baseline.md`
for the feasibility study, `docs/zh/README.md` for what has since been overturned.

## License

MIT for this project's original code (`LICENSE`). The QEMU fork is GPL-2.0 (`LICENSE.GPL2`),
XNU patches are APSL-2.0 (`LICENSE.APSL`). Third-party reuse is recorded in `NOTICE`.

Apple, iPhone, iOS, Metal and XNU are trademarks of Apple Inc. This project is not affiliated
with, endorsed by, or supported by Apple.

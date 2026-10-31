# IOSVM

**Boot iOS on Linux in QEMU, and render SpringBoard's first frame — all of it open, all of it reproducible.**

Status: **M0 — foundation.** The VM half is proven upstream (see `docs/adr/ADR-008-hakios-baseline.md`);
this repository is the project that carries it from Darwin 16.5 to iOS 26 arm64e and from a
framebuffer to a Metal command stream on the host GPU.

```
$ make run
...
$ ./tools/screendump.sh
$ file logs/screen.png
logs/screen.png: PNG image data, 1179 x 2556
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
./tools/unpack-ipsw.sh ~/iPhone17,3_26.x.ipsw       # dry run: prints, writes nothing
./tools/unpack-ipsw.sh ~/iPhone17,3_26.x.ipsw --apply
./tools/make-images.sh                              # dry run; --apply needs the M1 APFS writer

# 5. Boot a guest                                          — NOT YET (M0 tail / M1)
make run
```

Step 5 is the one that does not exist. `make run`, `make host`, `make guest`, `make qemu` and
`make test-gpu` are stubs that say so; `hw/virtio-iosvm-gpu` is M2 work. What *is* proven is
upstream, not here: a self-compiled XNU reaches BSD initialisation in 6.4 s of emulated boot on the
reference host (`docs/m0-host-report.md`).

What works today, exactly: `make test-open`, `make provenance`, `make style`, `make doctor`,
`tools/unpack-ipsw.sh` (verified end to end against a synthetic IPSW), `tools/check-frame.py`
(verified both ways), and the CI gate on every push.

## What lives where

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

Full reasoning: `docs/architecture.md`, `docs/adr/`, `docs/zh/design-baseline.md`.

## License

MIT for this project's original code (`LICENSE`). The QEMU fork is GPL-2.0 (`LICENSE.GPL2`),
XNU patches are APSL-2.0 (`LICENSE.APSL`). Third-party reuse is recorded in `NOTICE`.

Apple, iPhone, iOS, Metal and XNU are trademarks of Apple Inc. This project is not affiliated
with, endorsed by, or supported by Apple.

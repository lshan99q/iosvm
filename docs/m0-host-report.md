# M0 host report — 2026-10-31

Measured, not estimated. This is the "can this machine carry the project" answer that
`docs/phases.md` M0 asks for before any of our own code exists.

## Host

| | |
|---|---|
| OS | Ubuntu 24.04.4 LTS |
| Kernel | 7.0.0-38-generic |
| CPU | Intel Xeon E5-2698B v3 @ 2.00 GHz, 32 threads (Haswell, 2014) |
| RAM | 62 GiB |
| Disk free | 95 GiB (an APFS image set is ~8 GB) |
| GPU | 2 × NVIDIA Tesla V100-SXM2-16GB, driver 580.178.04 |
| Vulkan | `libvulkan1` 1.3.275, `mesa-vulkan-drivers` 25.2.8 (lavapipe, radv, intel, nouveau, virtio ICDs present) |
| KVM | `/dev/kvm` present and group-accessible, **not used** — the guest is ARM64 on x86, so it is TCG |

## What was installed for this report

`qemu-system-arm` (8.2.2), `qemu-efi-aarch64`, `lld`, `lz4`, `clang` 18.1.3, `flex`, `bison`,
`m4`, `autoconf`, `automake`. `meson`, `ninja`, `7-Zip 23.01`, `gcc` 13.3.0, Python 3.12.3 were
already present.

## Test 1 — HakiOS open kernel under QEMU, no Apple file

Built from `oosexgang/hakios-xnu-kernel` at `main` (2026-10-30), using its `prebuilt/hakios-kernel`
and a payload assembled with `xnuloader/mkpayload.py`. Command:

```sh
python3 xnuloader/mkpayload.py -o qemu/payload.bin \
    kernel=prebuilt/hakios-kernel ramdisk=prebuilt/userland/hello --cmdline ""
QEMU_EFI=... QEMU_VARS=... qemu/run.sh 100
```

Result: **the kernel boots.** The relevant lines, in order:

```
BdsDxe: starting Boot0001 "UEFI Misc Device"
== XNULOADER v3 ==
platform qemu-virt
payload entry 'kernel' size 1007c98
kernel loaded at pa f7004000..f8408000
jumping to f74424a8
Darwin Kernel Version 22.5.0: ... xnu-8796.121.2
pmap_startup() init/release time: 44993 microsec
vm_page_bootstrap: 199608 free pages, 5192 wired pages
VM bootstrap done: 90 maps, 163 entries and 500 holes left
MAC Framework successfully initialized
ifnet_attach: All kernel threads ... lo0 ... Proceeding.
MiniIOMobileFramebuffer: no usable boot framebuffer (0x0, depth 0)
Added memory device md0/rmd0 (01000000/0D000000) ...
pe_arm_map_interrupt_controller: failed to find the interrupt-controller
panic(cpu 0 caller 0xfffffff007ae8240): initproc failed to start
```

Time to `pe_arm_map_interrupt_controller`, the first line carrying a timestamp: **6.44 s** wall
clock inside a 100 s `timeout`, single vCPU, `cortex-a76`, TCG.

### Why it panics, and why that is the expected result

The payload's ramdisk is `prebuilt/userland/hello`, a 16 KB freestanding program, not a user
space. `launchd` is what the kernel tries to exec as pid 1, finds nothing to exec, and panics on.
**The panic is evidence that the BSD half initialised far enough to try.** A boot with a real
Darwin userland is a different test and needs the contributor's own IPSW (Test 2, not run here).

### Two host-specific gotchas, both fixed by the harness

1. Ubuntu's `qemu-efi-aarch64` ships a **2 MiB** `QEMU_EFI.fd`; QEMU's pflash device demands a
   64 MiB image. Pad it: `cp QEMU_EFI.fd efi64.fd && truncate -s 64M efi64.fd`. Debian/Arch
   `edk2-aarch64` and `AAVMF` ship the padded one.
2. `qemu/run.sh` looks for `QEMU_VARS`; with only `qemu-efi-aarch64` installed there is none to
   copy. An all-zero 64 MiB `vars.fd` works.

## Test 2 — Darwin userland in QEMU (not run, needs an IPSW)

Blocked on the contributor's own `iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw` (6 165 698 198
bytes). Nothing that follows can be measured until that file exists; it is not, and will not be,
in this repository.

## What this means for the schedule

- The QEMU half of M0 is **de-risked on this host**: a self-compiled XNU reaches BSD
  initialisation in ~6.4 s of emulated boot. That is fast enough to iterate on (a full user-space
  boot is documented upstream as ~5 minutes on a fast desktop; expect more on a 2.0 GHz Haswell,
  and treat 15 minutes as the planning number until measured).
- The display gap is visible in the boot log before any of our code exists:
  `MiniIOMobileFramebuffer: no usable boot framebuffer`. That is the exact seam
  `virtio-iosvm-gpu` sits on (ADR-004), and it is the first place M1 will look.
- `interrupt-controller` not found and `cpu-debug-interface` not found are cosmetic warnings from
  the generic ARM platform expert on `-M virt`; the kernel proceeds past both.
- Nothing here needs KVM, and nothing here uses it. Do not budget for hardware acceleration on
  this host: x86 hosting ARM64 is TCG by definition, and this CPU is from 2014.

## Reproduce

The commands above reference the upstream tree, not this repository, because M0 has no boot
harness of its own yet. When `tools/qemu-open.sh` lands, this report is regenerated from it and
these numbers are replaced with `--json` output.

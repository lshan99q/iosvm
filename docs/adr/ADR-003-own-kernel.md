# ADR-003: the guest kernel is one we compile, not the IPSW's kernelcache

- Status: Accepted
- Date: 2026-10-31
- Supersedes: ADR-003 of `docs/zh/engineering.md` ("use the IPSW kernelcache as the main path")
- Amended by: ADR-008 (HakiOS's port is the starting driver set)
- Superseded by: —

## Background

The design baseline chose the IPSW's `kernelcache` as the primary kernel, on the argument that it
comes with Apple's own kexts and therefore matches the user space's ABI without reimplementation.
The fallback was "compile XNU ourselves and write our own drivers", described as the expensive
route taken only if the display path spikes failed.

That argument inverts the cost. Using the `kernelcache` means every driver in it must recognise
virtual hardware: Apple's display kext must bind to something in QEMU, AppleAPFS must accept our
block device, and the platform expert must accept our device tree. Each of those is
reverse-engineering closed code, and one failure blocks everything after it. Compiling XNU
ourselves means writing open drivers against open headers, where a failure is a bug with a stack
trace.

## Evidence

- HakiOS does exactly this and reaches a home screen on real hardware **and** under QEMU `virt`
  with a stock machine model (`docs/getting-started/qemu.md`, README capability table). Its
  drivers are its own, built from `configs/defconfig`.
- XNU is APSL-2.0 open source. The kernel's interfaces to a driver (IOKit, `IOPlatformExpert`,
  `IODMACommand`, the block and network stacks) are open, unlike Apple's kexts.
- The design baseline's own risk table already noted that R1 (C1/C2 fail) is "high probability,
  high impact" and that C3 is "known possible (HakiOS did it on a phone)". Writing that down as
  the fallback and then making it the fallback is what ADR-008 fixes.

## Decision

Build XNU from source with our own drivers, and boot that. Specifically:

- The kernel tree is patched by files in `xnu/*.patch` (APSL-2.0), applied to a pinned XNU
  release.
- Drivers live behind configuration options so a build carries exactly the drivers its target
  has, the way HakiOS does with `CONFIG_HAKIOS_*`.
- The user space still comes from the contributor's IPSW. We do not ship it and do not modify it
  beyond what a signed-out system needs to start.

## Consequences

**Positive**

- No closed kext has to be fooled. The display path becomes "our driver, our device" instead of
  C1's reverse engineering.
- A panic is debuggable: we have symbols for the kernel we built.
- The v1.0 scope no longer includes making a stock `kernelcache` boot, which removes the entire
  C1/C2 branch of the design.

**Negative**

- APSL-2.0 obliges us to publish modifications as source. They are in `xnu/*.patch`, in the open.
- We own the driver set. Mice, keyboards, block, network, framebuffer, audio: each is ours to
  write and to keep working. This is the real cost, and it is why ADR-008 exists — HakiOS's set
  is a starting point, not a blank page.
- The user space must tolerate a kernel that is functionally equivalent but not byte-identical to
  Apple's. That is what the M1 acceptance test measures.

## Follow-up

- `xnu/` must contain only patches, never a copy of the tree (size, license hygiene, reviewing).
- Every patch file carries an `APSL-2.0` header naming the upstream release it applies to.

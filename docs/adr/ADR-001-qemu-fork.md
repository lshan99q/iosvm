# ADR-001: fork QEMU, do not write a VMM

- Status: Accepted
- Date: 2026-10-31
- Supersedes: —
- Amended by: ADR-008 (the base is no longer `qemu-sptm`)
- Superseded by: —

## Background

Two ways to get an ARM64 guest running on an x86 host: write a hypervisor, or extend one. A
hypervisor that boots XNU correctly has to model the Apple platform closely enough that the
kernel's early boot path, interrupt controllers, timers and MMIO layout all behave — and QEMU
has two decades of exactly that, plus a device model for every block, input and network device we
need.

## Decision

Fork QEMU. Add our devices under `hw/` and keep changes to QEMU's core at zero. When the fork
exists it is a submodule pinned at a commit, never edited in place (see
`docs/adr/ADR-004-virtio-device.md` and the submodule rules in `docs/zh/git-rules.md` §8).
**The fork is not vendored yet**: there is no `.gitmodules`, and `make submodule` fails rather than
pretending, until M2 adds it.

Amendment (ADR-008): the base changes from `jprx/qemu-sptm` to an upstream-based fork that carries
HakiOS's `virt` machine configuration and our `hw/` devices. `qemu-sptm` models SPTM/TXM so that an
Apple `kernelcache` can boot; we boot a kernel we compile ourselves, so we do not need SPTM
modeling. Keeping the fork close to upstream QEMU is worth more than the SPTM work.

## Options weighed

| Option | Good | Bad |
|---|---|---|
| Write a VMM from scratch (KVM on ARM host, or a custom TCG) | total control | a decade of device models to rebuild; loses every contributor who knows QEMU |
| Fork `qemu-sptm` | SPTM/TXM already modeled | we do not boot an Apple kernelcache, so the model is dead weight; upstream drift |
| Fork upstream QEMU, add `hw/` only | smallest diff, reusable, contributors know it | must build the Apple-ish machine config ourselves (ADR-008: HakiOS already has it) |

## Consequences

- The diff against upstream must stay reviewable: anything that can live in `hw/` lives there.
- License: QEMU's GPL-2.0 covers the fork (`LICENSE.GPL2`). Our own code stays MIT.
- A contributor must build QEMU to test devices, so `make test-open` must never depend on that.
  That is why `tests/fake-gpu/` interprets commands in-process.
- Upstream drift is a standing risk (R7). Mitigation: pin, bump deliberately in its own PR, and
  never carry a patch that upstream would take.

## Evidence

- HakiOS boots its own XNU under unmodified `qemu-system-aarch64 -M virt` (ADR-008 evidence 1).
  A stock machine model is enough for the kernel we build, which settles the "do we need a
  custom machine" question for v1.0.
- The `hw/` surface we actually need is small: one device (`virtio-iosvm-gpu`) plus a display
  path. That is a fork diff of a few thousand lines, not a platform.

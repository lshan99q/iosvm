# ADR-008: HakiOS on QEMU `virt` is the baseline, not darwin-vm plus reverse engineering

- Status: Accepted
- Date: 2026-10-31
- Supersedes: parts of ADR-001, ADR-003, ADR-007, and the D1 decision gate as written in
  `docs/zh/engineering.md` (ADR-001..007 at §3, the C1/C2/C3 gate at §4.5)
- Superseded by: —

## Background

The design baseline (`docs/zh/engineering.md` for the architecture and the ADRs,
`docs/zh/design-baseline.md` for the feasibility study) was written from the assumption that the
QEMU half of the problem was unsolved: `darwin-vm` reaches a root shell and nothing more, so
display has to be obtained either by reverse-engineering Apple's closed display kext (C1), by
hooking the user-space present path (C2), or by giving up on the Apple kernelcache and writing
our own drivers (C3, called a fallback). Under that assumption D1 was the project's only go/no-go
gate, and it was gated on two 3–4 day spikes against closed code.

That assumption is wrong, and the baseline's own reference list contains the counter-evidence:
HakiOS (`oosexgang/hakios-xnu-kernel`, 1997 stars, MIT/APSL-2.0).

## Evidence

All of the following is from the HakiOS tree at `main` (commit of 2026-10-30) and from running
its `make doctor` on this host on 2026-10-31.

1. **HakiOS runs its own XNU build in QEMU with nothing of Apple's and nothing of a phone's.**
   `make qemu-open` builds the kernel, a placeholder loader and a tiny raw-syscall user space and
   boots them under `qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76`. The machine is
   an ordinary `virt` board; the drivers are HakiOS's own, built from its `configs/defconfig`.
   This is C3 — and it is not a fallback, it is a shipped feature.

2. **With a user-supplied Darwin userland, the same QEMU path reaches the home screen.**
   `docs/getting-started/qemu.md`: `sysimage/unpack.sh <your IPSW>`, `REST=1 userland/build.sh`,
   `make loader`, `qemu/run-darwin.sh 1200 -display gtk`. "the Apple logo at about one minute, the
   home screen at about five". The README's capability table marks lock screen / home screen /
   Control Center / stock apps as **yes** for `QEMU virt`, annotated "(slow: the CPU is
   emulated)". Target of that path: Darwin userland 16.5, `20F66`, `iPhone10,3` (iPhone X).

3. **IPSW extraction is pure Linux.** `sysimage/unpack.sh` needs `python3`, `7z >= 23` and `cc`;
   it reads the `.dmg` members with 7-Zip and finishes LZBITMAP-compressed files with its own
   `unzbm`. There is no `hdiutil`, `ipsw`, `otool` or `codesign` in the path. **The Mac
   dependency in ADR-007 does not apply to the boot and unpack half.**

4. **AIR is parsed and translated without touching any signature check.** HakiOS's air.md (under its docs/): of the
   456 modules in the device's system `.metallib` files, 456 are read byte-for-byte as `llvm-dis`
   reads them, 321 translate to NIR and compile with `ir3`, 255 are numerically identical to
   LLVM's own execution. HakiOS's own `air/check.sh` (under its `tools/`) does this from the files; the document lists no
   signing or encryption step as an obstacle. That answers D2's *first* half (is AIR
   encryptable/verifiable) with "no, not in the path we would use" — see the caveat in §Consequences.

5. **The Metal work HakiOS has not done is exactly the work this project exists to do.** HakiOS's
   AIR document states plainly: "The other two thirds are not here and are not started: the
   `_MTLDevice`/`IOGPUMetalDevice` object graph (AGXMetalA11 alone is 53 classes and 1158 methods,
   IOGPU.framework 711 more), and the kernel ABI that submits work." The target of that work is
   Adreno via `ir3`. Our target is Vulkan on the host, which is a *different* third — and a
   strictly better one to stand on, because Vulkan is a documented API.

## Decision

**Build IOSVM on the HakiOS `QEMU virt` baseline.** Concretely:

- Guest kernel: a build of XNU from source with our drivers, following HakiOS's port and its
  config system — **not** the IPSW's `kernelcache`, and **not** a fork of `qemu-sptm`.
- Guest user space: the contributor's own IPSW, laid out by a Linux-only unpack tool in this
  repository, starting from the Darwin 16.5 path HakiOS documents. (Moving to iOS 26 was the plan
here; `docs/adr/D1-prime-userland-version.md` later closed that door on licensing grounds.)
- Display: `virtio-iosvm-gpu` as designed in ADR-004, on top of the framebuffer path HakiOS
  already proves. C1 (reverse-engineering `AppleDisplayPipe`) is **dropped from v1.0 scope**;
  it is only interesting for booting an unmodified Apple `kernelcache`, which we are no longer
  doing.
- The D1 gate is **closed by evidence**, not by a spike. D1's replacement gate is D1' below.

## Consequences

**Positive**

- The project's largest stated risk (R1: "C1/C2 both fail → pivot to C3, schedule doubles") is
  gone. C3 is the starting point, and it is a working, documented, MIT-licensed one.
- M1's two 3–4 day reverse-engineering spikes are no longer on the critical path. M1 becomes
  "reproduce the upstream QEMU + userland path here, then move it from Darwin 16.5 to iOS 26".
- The Mac dependency drops out of M0 and M1 entirely. A contributor with a Linux box and their
  own IPSW can reach a home screen without owning a Mac (ADR-009 keeps the remaining Mac need
  for `guest/` only).
- We inherit a proven driver set, a proven QEMU machine configuration and a tested `unpack.sh`.

**Negative / new risk**

- **Windows-of-motion risk:** iOS 26 arm64e is not what HakiOS's QEMU path targets today. The
  jump from Darwin 16.5 / arm64 to iOS 26 / arm64e involves PAC, the newer SPTM-era boot
  requirements, and a much larger userland. This replaces R1 with a real question — see D1'.
- We now depend on a fast-moving upstream (HakiOS is ~1 week old at the time of writing). R7
  (upstream drift) gets worse, not better.
- AIR: "no signing step in our path" is evidence about *HakiOS's Adreno path*, which reads
  `.metallib` files. It is **not** evidence that iOS 26's `.metallib`s behave the same, and it is
  not evidence about any check that happens at compile/install time on the guest. D2 stays open
  as a legal-evaluation gate. If a check is found: **stop, do not route around it.**
- C1 removal means that a future desire to boot a stock Apple `kernelcache` would reopen the
  whole problem. That is out of scope for v1.0 and is recorded here so it is not rediscovered.

## New decision gate D1'

Instead of "C1 vs C2 vs C3", the first gate is:

| Question | Test | If yes | If no |
|---|---|---|---|
| Does the HakiOS QEMU path reproduce here (kernel banner, user space lines)? | `make qemu-open`, no Apple files | proceed to M0 | fix the host first; this is a build problem, not a research problem |
| Does the same path boot **iOS 26 arm64e** user space? | user's own iOS 26 IPSW, same `unpack`/`build` flow | M1 is a port, not a rewrite | keep 16.5 working; treat 26 as a research spike with a 3-day box, and record what blocks it |

The second row is the real research question now. It is bounded, it is measurable, and it is
much cheaper to answer than C1/C2 ever were.

# xlt — cross-architecture translator

xlt is the project's own AArch64 → x86-64 translator. Capstone 4 decodes a region of guest code, the
lifter emits LLVM IR, LLVM 18's LLJIT compiles it once, and a direct-mapped cache runs the host code
on every later entry to that guest PC. It exists to find out how fast a translator built for this one
guest and this one machine can be, with QEMU's TCG as the reference to beat.

Status: **M1 complete and independently verified** (2026-11-28) — 10.08× TCG on the benchmark,
every checksum matching — and **M2/P1 delivered**: `-accel xlt` plus the CPU-less `-M xlt` machine
run bare-metal images with byte-identical TCG parity. The full note is
[`../docs/xlt-translator.md`](../docs/xlt-translator.md); the verification reports are
[`../reports/xlt-m1-report.md`](../reports/xlt-m1-report.md) and
[`../reports/xlt-m2-p1-report.md`](../reports/xlt-m2-p1-report.md).

## Layout

| path | what |
|---|---|
| `src/xlt.h` | public engine API: `CpuRegs`, `BlockFn`, `Engine`, `RunOptions`, `Stats` |
| `src/libxlt.h`, `src/libxlt_c.h` | host-neutral engine facade and the C ABI the QEMU accelerator links against |
| `src/xlt_insn.h`, `src/decode.*` | the neutral instruction model and the capstone-backed decoder |
| `src/lift.*`, `src/jit.*` | region lifter to LLVM IR; LLJIT wrapper (`-O2` default) |
| `src/engine.cpp`, `src/main.cpp` | ELF loader and dispatcher; the `xlt` CLI |
| `tests/` | decode unit tests and the `*_test` runner |
| `tests/fixtures/p1/`, `tests/fixtures/qemu-smoke/` | freestanding AArch64 images for the accelerator |
| `bench/` | bare-metal AArch64 benchmark image, host harness, recorded TCG baselines |
| `qemu/` | the QEMU work (M2): design, P1 quick start, patches, build scripts, P2 plan |

## Build dependencies

LLVM 18 development files (`llvm-config-18`), capstone 4, and a C++17 compiler. Both are host
dependencies and are never vendored into this tree.

## Build, run, test

```
make -C xlt                        # builds xlt/build/xlt
xlt/build/xlt run <elf> [--stats]  # execute a static AArch64 image
make -C xlt test                   # decode tests (481 checks) + every tests/*_test
cd bench && ./build.sh && ./run.sh # benchmark: TCG baseline, then xlt
```

`make -C xlt clean` removes `xlt/build/`, including a built benchmark image.

## Scope

M1 is the in-tree engine only: ELF loader, AArch64 decoder, region lifter, JIT block cache,
`xlt run`, tests and the benchmark. M2 puts the engine behind QEMU: P1 is delivered as
`-accel xlt` + `-M xlt` (bare-metal images, TCG parity) and the remaining machine work — MMU,
exceptions, interrupts, pflash/virtio, FDT — is staged in [`qemu/p2/PLAN.md`](qemu/p2/PLAN.md).
Design and quick start: [`qemu/README.md`](qemu/README.md) and
[`qemu/INTEGRATION.md`](qemu/INTEGRATION.md); P1 limits are in [`qemu/p1/NOTES.md`](qemu/p1/NOTES.md).

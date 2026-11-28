# XLT: the project's own cross-architecture translator

> Status: **M1 complete and independently verified** (2026-11-28, `feat/xlt-translator`); the
> numbers in [Benchmark results](#benchmark-results) are from
> [`reports/xlt-m1-report.md`](../reports/xlt-m1-report.md) (verdict PASS). M2 — the QEMU
> accelerator — is designed in [`xlt/qemu/INTEGRATION.md`](../xlt/qemu/INTEGRATION.md), not built.

XLT — `xlt` on the command line and in the tree — is our own dynamic translator. It executes
AArch64 guest code on an x86-64 host by translating it a region at a time, then caching the host
code so repeated execution runs as native code. TCG is QEMU's translator and stays the reference
implementation: XLT exists to find out how much faster a translator can be when it is built for
this one guest — one CPU model, one machine, one boot path — instead of for every target QEMU
supports.

M1 is the in-tree engine, its tests and its measurements. M2 puts the engine behind `-accel xlt`
in QEMU; the design for that, including the hooks it needs and the metrics it must meet, is the
[accelerator design](../xlt/qemu/INTEGRATION.md).

## Why: where TCG's time goes

The thing to beat is not TCG's translator, it is TCG's dispatch. A `perf` profile of QEMU 8.2.2
during the real boot (two 60 s windows, the boot takes about two minutes and reaches the `lsd`
stage) attributes host CPU like this:

| host CPU | symbol | what it is |
|---:|---|---|
| 6.8–7.9% | `helper_lookup_tb_ptr` | TB dispatch / chaining lookup |
| 4.1–6.6% | `qht_lookup_custom` | TB hash-table lookup |
| 2.8% | `cpu_get_tb_cpu_state` | per-TB CPU state / environment computation |
| 1.3–1.8% | `tlb_set_page_full`, `arm_cpu_tlb_fill` | soft-TLB fill |
| 1.6% | `get_page_addr_code_hostp` | code-page address translation |
| 1.0% | `qemu_ram_block_from_host` | RAMBlock lookup |
| 0.7% | `g_hash_table_lookup` | generic hash lookup |
| 0.6% | `probe_access_full_mmu` | memory-access probe |
| 0.5% | `object_dynamic_cast_assert` | QOM runtime type check |
| **3.4%** | `helper_simd_tblx` | NEON `TBL` via helper (Apple libc's SIMD memcpy/strlen) |
| 0.5–1.0% | `helper_neon_*`, `helper_ld*_mmu`, `helper_dc_zva` | NEON, memory, cache-maintenance helpers |
| **0.44%** | `tcg_gen_code` | the translator itself |

The summary of a longer analysis in the full design document: translation is **not** the boot
bottleneck (0.44%); the cost is that every guest instruction runs behind TCG's dispatch, address
translation and helpers — the design document puts dispatch and memory machinery at **20–25%** of
host CPU together, with the dispatch path alone (`tb_find`/`lookup_tb_ptr`/`qht`/
`cpu_get_tb_cpu_state`) at **11–15%** and NEON helpers at **3–4%**. Summing the memory rows above
(soft-TLB fill, code-page translation, RAMBlock and hash lookups, probe and load helpers) gives
roughly 6–7%. Another 2.8–3.0% is the host kernel writing back dirty pages of the guest RAM file,
a memory-backend choice rather than a translator cost.

So the useful moves are the ones XLT makes: a region is entered with no table lookup, guest memory
access is one host access, and translation happens once per region instead of once per basic block.
Accelerating TCG's translation loop by 10× would not touch the other 20%.

## M1 scope

In scope, and what this document covers:

- an ELF loader for static AArch64 images (`xlt run <elf>`), with semihosting input/output;
- the decode → lift → JIT pipeline: capstone 4 → an architecture-neutral instruction model →
  LLVM IR → host code through LLVM 18's LLJIT at `-O2`;
- a block cache keyed by guest PC, statistics counters, and unit tests;
- a bare-metal benchmark image (`xlt/bench/`) that runs unchanged under TCG and under XLT, so the
  two engines can be compared instruction for instruction and cycle for cycle.

## Architecture

```
 guest ELF ──► loader ──► flat 256 MiB RAM window at 0x40000000
                              │
              pc ─────────────┤
                              ▼
 dispatcher (engine.cpp) ──► block cache ──hit──► host function (BlockFn)
                              │ miss                    │
                              ▼                         │ returns next guest PC
                    decoder (capstone 4)                │ or sets CpuRegs::halted
                              ▼                         │
                    lifter (LLVM IR)                    │
                              ▼                         │
                    LLJIT (LLVM 18) ────────────────────┘
```

- **ELF loader** (`engine.cpp`): accepts `ELF64`, `EM_AARCH64`, `ET_EXEC` only. Every `PT_LOAD`
  segment is copied into a zeroed 256 MiB host allocation, so BSS is zero by construction; the
  initial stack sits 16-byte aligned near the top of the window, the loader refuses an image that
  would overlap it, and `RunOptions::entry` defaults to the ELF entry point.
- **Decoder** (`decode.cpp`): one instruction word plus its guest address becomes one `xlt::Insn`
  (`xlt_insn.h`). PC-relative operands are normalized to absolute guest addresses; an instruction
  the M1 lifter does not handle is still decoded, with `unsupported` set, so the engine can
  diagnose it instead of looping. No capstone type escapes this layer.
- **Lifter** (`lift.cpp`): a region becomes one LLVM function `g<entry_pc>`. The guest register
  file lives in allocas promoted by mem2reg; `sp`, `pc` and the packed `nzcv` word live in
  `CpuRegs` at field offsets the lifter computes with `offsetof`. Host helpers called from
  generated code are `xlt_bad_mem`, `xlt_unsupported`, `xlt_semihost` and `xlt_cntvct`.
- **JIT** (`jit.cpp`): an LLJIT wrapper, `-O2` by default (`XLT_OPT=0|1|3` overrides). Every module
  gets a fresh analysis-manager set and `PassBuilder`, because a long-lived `ModuleAnalysisManager`
  caches results keyed by pointers that ORC recycles — the crash the prototype hit. `get()`
  reports translate and opt/JIT times.
- **Dispatcher** (`engine.cpp`): a direct-mapped cache of 8,192 entries from guest PC to `BlockFn`
  (slot `(pc ^ (pc >> 12)) & 0x1fff`). A hit calls the block; a miss translates and installs the
  region, then calls it. The loop stops when the guest halts (semihosting `SYS_EXIT`, or
  `CpuRegs::halted`) or when `RunOptions::max_instructions` is reached; `Stats` counts blocks and
  instructions translated and executed.
- **Semihosting**: `HLT #0xf000` calls the host's Arm semihosting implementation. M1 implements
  `SYS_WRITE0` (0x04, to stdout) and `SYS_EXIT` (0x18, sets `halted` and `exit_code`); any other
  call halts with exit code 1. `cntvct_el0` is emulated from `CLOCK_MONOTONIC` at the 62.5 MHz the
  `virt` machine advertises.

The public surface is deliberately small: `Engine::load()` maps the image, `Engine::run()`
translates and executes it, and `regs()`/`stats()` expose the result (`xlt/src/xlt.h`). The CLI
adds `--stats`; `--version` prints `xlt 0.1`.

## Regions and lazy flags

A **region** is a straight-line run of at most 256 instructions with internal basic blocks for
direct branches. It ends at the first instruction that cannot fall through — `BL`, `BLR`, `BR`,
`RET`, `HLT` — or at the region-size cap. The generated function returns the next guest PC; the
dispatcher decides whether to call it again, so control flow never leaves the translated world
except through the return value or a helper.

The region is the unit of translation and caching for a reason: it is large enough that
translation, IR verification and LLVM optimisation are amortised over hundreds of guest
instructions, and small enough that a miss costs milliseconds and a retranslation after an
invalidation touches little code. In the benchmark the whole image is 11 regions and 509 translated
instructions, executed 343.9 million times.

**Flags are lazy inside a region.** `NZCV` is not read back from the guest word on every use. The
lifter keeps four `i1` values (`N`, `Z`, `C`, `V`) for the current flag state: a flag-writing
instruction computes them and packs them into the guest `nzcv` word (`setFlags`), while a
conditional or a later flag read consumes the four `i1`s directly; a region that reads flags
before writing them unpacks them from the word once, on demand (`ensureFlags`). The packed layout
is the architectural one — bit 31 `N`, 30 `Z`, 29 `C`, 28 `V` — the same constants `kN`…`kV` in
`xlt.h`, so the boundary to the dispatcher and to tests needs no conversion. A guest that samples
the word (a debugger, or the next region) always sees the architectural value, because every
flag-writing instruction updates it.

Flags cross region boundaries through that packed word. Carrying the four `i1`s across a boundary
would need a second entry point in `BlockFn`; M1 does not plan one.

## Memory fast path

Guest memory is one flat 256 MiB window mapped at `0x40000000` (`kRamBase`/`kRamSize` in
`lift.h`). The loader copies the ELF's `PT_LOAD` segments into a host allocation, zero-fills BSS by
construction, and places the initial stack near the top of the window. The generated code embeds
the host pointer to that allocation at translation time, so a guest load or store is one host load
or store plus a bounds check against the window; a miss calls the noreturn `xlt_bad_mem` helper
(exit code 70) instead of corrupting host memory.

This is exactly the "memory fast path" the accelerator design needs, in its simplest possible
form: guest virtual address = host address, no MMU, no TLB, no fault path. M1 can afford that
because the benchmark image is freestanding and static. M2 replaces the flat window with QEMU's
`virt` RAM region, a software TLB built on `get_phys_addr()`, and `address_space_rw()` for
everything that is not RAM — H4 and H15 in the [accelerator design](../xlt/qemu/INTEGRATION.md).

## Build, run, test

Host dependencies: LLVM 18 development files (`llvm-config-18`), capstone 4, and a C++17 compiler
(`make -C xlt` links `-lLLVM-18 -lcapstone -lpthread -ldl -lz -ltinfo`). Nothing is vendored.

```
make -C xlt                       # builds xlt/build/xlt
make -C xlt test                  # decode tests + xlt/tests/run_tests.sh; zero tests is a pass
xlt/build/xlt run <elf> [--stats] # execute a static AArch64 image
xlt/build/xlt --version           # prints "xlt 0.1"
```

Exit status: the guest's semihosting exit code on success; 1 when the engine fails (bad ELF,
unsupported instruction); 2 on a command-line mistake. `make -C xlt clean` removes `xlt/build/`,
including a built benchmark image.

`make -C xlt test` runs `xlt/tests/decode_test` (481 checks over the instruction forms the lifter
supports) and then `xlt/tests/run_tests.sh`, which executes every `*_test` binary in `tests/` and
`build/tests/`.

The benchmark is a separate, self-checking harness:

```
cd xlt/bench
./build.sh                        # clang --target=aarch64-unknown-elf, freestanding, lld
./run.sh                          # TCG baseline, then XLT if xlt/build/xlt works
./run.sh --skip-xlt               # baseline only
```

`run.sh` runs the same ELF under `qemu-system-aarch64 -M virt -accel tcg` and under `xlt run`,
compares every checksum, and prints per-test ticks, milliseconds and the speedup
(`tcg_ticks / xlt_ticks`). Its dependencies and exact commands are in
[`xlt/bench/README.md`](../xlt/bench/README.md).

## Benchmark results

Five workloads — `alu64`, `memcpy`, `hash`, `ptrchase`, `indcall` — each a fixed amount of work
with an architecture-independent checksum. Guest time is measured with `cntvct_el0`, 62.5 MHz on
this `virt` machine, so one tick is 16 ns and `ms = ticks / 62500`; XLT emulates the counter from
the host's monotonic clock, so both engines are timed with the same counter. A run is valid only
if every checksum matches.

Median of three runs per engine, the same image (`bench.a64.elf`, 69,224 bytes, sha256
`d28c71ecd46ccd5ed9b0a24ddccdf407044900dc45d1618f52afd5e3fd643113`), TCG at
`-accel tcg,tb-size=1536`, on the shared 32-thread build host (load average 3.6–6.5 across the
runs):

| test | TCG ticks | TCG ms | XLT ticks | XLT ms | speedup | checksums |
|---|---:|---:|---:|---:|---:|---|
| `alu64` | 11,471,737 | 183.55 | 5,913,344 | 94.61 | 1.94× | match |
| `memcpy` | 146,777,774 | 2,348.44 | 963,724 | 15.42 | 152.30× | match |
| `hash` | 135,556,594 | 2,168.91 | 14,030,879 | 224.49 | 9.66× | match |
| `ptrchase` | 131,371,870 | 2,101.95 | 12,565,482 | 201.05 | 10.45× | match |
| `indcall` | 15,914,586 | 254.63 | 9,977,442 | 159.64 | 1.60× | match |
| **total** | **441,092,561** | **7,057.48** | **43,760,955** | **700.17** | **10.08×** | **5/5** |

The three TCG/XLT pairings give 9.83×, 10.19× and 10.42× overall; the table is the median-based
pairing. Every individual test is at least 1.57× faster in every pairing, and every checksum
matched in every run of both engines (3 TCG + 3 XLT): `alu64` `0x067a3bf250fa984e`, `memcpy`
`0x13fbe40bf4180000`, `hash` `0x0000006a1365d0c6`, `ptrchase` `0x000003d094fd9544`, `indcall`
`0xc2a86d1e196c073f`.

The engine counters from the same runs explain the shape: wall clock 867–875 ms, of which 4.1 ms
was translating and 194–195 ms was LLVM optimisation and JIT of the 11 regions; 16,000,014
dispatches executed 343,894,262 guest instructions. The CPU-time speedup is 10.08× while the
wall-clock speedup is 7.87–8.33×, because XLT pays the compile before it runs — the trade the
accelerator design removes with caching and background compilation (P3).

TCG's translation-cache size does not move the comparison: a separate panel over
`tb-size` 384 / 1536 / 4096 MB put TCG's mean total at 6.93 / 6.85 / 6.97 s, inside the
run-to-run spread, with identical checksums. An earlier three-run TCG baseline is recorded in
`xlt/bench/RESULTS.md`; it agrees with the verification run within about 1%.

## What M1 does not do

- **No QEMU integration.** There is no `-accel xlt` yet; the engine runs ELF images as a host
  process. That is M2 ([accelerator design](../xlt/qemu/INTEGRATION.md)).
- **No MMU, exceptions, interrupts, system registers or MMIO.** A guest that touches any of them
  stops at the unsupported path. This is why the benchmark is a freestanding image and not
  firmware.
- **No NEON/SIMD, no floating point, no atomics/LSE.** AArch64 integer code only.
- **No persistence or AOT.** The block cache lives for one process and is rebuilt every run; there
  is no on-disk translation cache in M1.
- **No self-modifying-code detection**, no multi-threaded execution, no AArch32.
- **Static images only** (`ELF64`, `ET_EXEC`; no shared objects, no loader), AArch64 little-endian
  only, x86-64 host only.

## Limitations

- **Coverage is a moving line.** Every instruction the benchmark and tests execute is lifted;
  anything else is an `unsupported` hard stop (`exit 71`). Adding instructions is adding cases to
  the lifter, not plumbing.
- **The 256 MiB window is a fixed map.** The image must fit below it, the stack lives at the top,
  and the benchmark's memory accesses are within it.
- **One host thread.** There is no concurrent translation and no parallel execution; `SMP` is not
  modelled.
- **The JIT is not a product.** There is no recompilation policy, no hotness tracking and no
  adaptive tier — one region is lifted once at `-O2`.
- **Numbers are noisy on a shared host.** The benchmark host also runs other work; the report
  records load averages and medians, and the comparison is pairing-based for that reason.
- **`unsupported` diagnostics are author-facing**, not a guest-visible fault model.

## Roadmap: from M1 to the accelerator

M1 is the design document's P0 delivered in-tree: ELF loading, region translation, lazy flags,
flat addressing, semihosting, LLJIT, and the benchmark comparison. The remaining phases, with the
design document's acceptance targets:

| phase | work | acceptance |
|---|---|---|
| **P0 — prototype, in-tree** (done) | this milestone: the engine, the tests, the benchmark, this document | checksums match TCG/native; ≥1.5× on every test; measured in [the verification report](../reports/xlt-m1-report.md) |
| **P1 — kernel boot** | table-driven decoder; MMU and soft TLB, exceptions, sysregs, generic timer, GICv3 CPU interface, PSCI, `HLT`; T1 template-JIT skeleton; boot to `lsd` | serial aligned with TCG to `lsd` under `cpus=4 fb_beat=0`; single-vCPU boot ≤ 6 min |
| **P2 — userland to home screen** | NEON/FP translation (`tbl`/`zip`/`widen`/`fmla`), atomics/LSE, SMC invalidation, VM integrity, multi page table/ASID; QEMU accel integration; glwire/metalwire unchanged | home screen reachable, ≥ TCG success rate; 13.9 fps path loses no frames; D1/D2 no regression |
| **P3 — tiering, cache, AOT** | T2 background compile and hot swap; on-disk translation cache; boot-trace record/replay; parallel pretranslation; metrics/QMP | second boot translates ≈0; boot ≤ 4 min; cache restore < 100 µs |
| **P4 — SMP** | multi-vCPU threads, TLB shootdown, GIC IPI, spinlock backoff; stage-44 scenario regression | 2–4 vCPUs, 30 min without panic (or a clearly new root cause); 2-vCPU fps ≥ 1.5× |
| **P5 — tuning and delivery** | SIMD/AES, memory backend (hugetlbfs/tmpfs/memfd), cache eviction, boot arguments, docs/CI | all hard targets met; a one-switch `run-darwin.sh` |

The numbers P1–P5 will be measured against are the TCG baselines this repository already has:
about eight minutes from power-on to a drawn SpringBoard frame, and a steady-state median around
13.9 fps at 75 ms per frame at load 5.5–8 (the low 11.7 fps / 87 ms figure is the high-load check).
See [`handover.md`](handover.md). The QEMU-side plan — phases, the 16 hooks, acceptance metrics and
risks — is [`xlt/qemu/INTEGRATION.md`](../xlt/qemu/INTEGRATION.md).

## Where things live

| path | what |
|---|---|
| [`xlt/README.md`](../xlt/README.md) | the tree: build, run, test, dependencies |
| `xlt/src/xlt.h` | the public engine interface: `CpuRegs`, `BlockFn`, `Engine`, `Stats` |
| `xlt/src/xlt_insn.h`, `xlt/src/decode.h` | the neutral instruction model and the decoder interface |
| `xlt/src/lift.h`, `xlt/src/jit.h` | the lifter and JIT interfaces |
| [`xlt/bench/README.md`](../xlt/bench/README.md) | the benchmark image: workloads, checksums, harness |
| [`xlt/bench/RESULTS.md`](../xlt/bench/RESULTS.md) | the recorded TCG baseline runs |
| [`xlt/qemu/README.md`](../xlt/qemu/README.md) | the accelerator's design: threading, exits, memory, cache |
| [`xlt/qemu/INTEGRATION.md`](../xlt/qemu/INTEGRATION.md) | phases, QEMU hooks, acceptance metrics, risks |
| [`reports/xlt-m1-report.md`](../reports/xlt-m1-report.md) | the independent M1 verification report |
| [`docs/phases.md`](phases.md) | the milestone list and its acceptance commands |
| [`docs/adr/ADR-001-qemu-fork.md`](adr/ADR-001-qemu-fork.md) | the QEMU fork decision, and the "core changes at zero" rule M2 has to negotiate |

The full design — the three-tier execution model (T0 interpreter, T1 copy-and-patch templates, T2
LLVM hot regions), the memory and SIMD plans, the cache format and the phase estimates — lives
outside this repository in `/home/lshan/dsh.MD/iosvm-新转译架构.md` (Chinese); this document and the
accelerator design are the in-tree subset that M1 and M2 need.

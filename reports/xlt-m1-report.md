# xlt M1 - independent verification report

Verifier: member `exp` (t6, attempt 2). Date: 2026-11-28.
Tree: `feat/xlt-translator`, HEAD `8b27c15`, uncommitted work tree (commit is t8's job).
Scope: independently rebuild the in-tree xlt M1 engine, re-run its tests and the bare-metal
AArch64 benchmark, and compare QEMU TCG against xlt. The implementer's numbers (t5) were not
trusted; every number below was produced by this verification.

## Environment

| item | value |
|---|---|
| host CPU | 2x Intel(R) Xeon(R) CPU E5-2698B v3 @ 2.00 GHz, 32 threads |
| kernel | Linux 7.0.0-42-generic |
| clang / lld | 18.1.3 / 18.1.3 |
| LLVM | 18.1.3 (`llvm-config-18`) |
| capstone | 4.0.2 (`libcapstone-dev 4.0.2-5.1build1`, `pkg-config --modversion capstone` = 4.0.2) |
| QEMU | 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18) |
| benchmark image | `xlt/build/bench/bench.a64.elf`, 69,224 bytes, sha256 `d28c71ecd46ccd5ed9b0a24ddccdf407044900dc45d1618f52afd5e3fd643113` |
| host load during runs | 1-minute load average 3.63-5.93 on 32 threads (recorded per run) |
| guest counter | `cntvct_el0`, 62.5 MHz = 16 ns/tick; `ms = ticks / 62500` |

## Exact commands

```sh
cd /home/lshan/iosvm&o

# build and unit tests
make -C xlt clean && make -C xlt 2>&1 | tee /tmp/t6-build.log
make -C xlt test

# benchmark image
cd xlt/bench && ./build.sh bench && file ../build/bench/bench.a64.elf

# TCG baseline, twice (tb-size 1536 is what run.sh hard-codes)
./run.sh --skip-xlt 2>&1 | tee /tmp/t6-tcg-1.log
./run.sh --skip-xlt 2>&1 | tee /tmp/t6-tcg-2.log

# engine, twice, with translation statistics
cd ../.. && for i in 1 2; do
  timeout 60 xlt/build/xlt run xlt/build/bench/bench.a64.elf --stats \
    >/tmp/t6-xlt-$i.out 2>/tmp/t6-xlt-$i.err
done

# both engines in one harness invocation (checksum-gated table)
cd xlt/bench && ./run.sh --xlt "$PWD/../build/xlt" 2>&1 | tee /tmp/t6-integrated.log

# strict warning proof (LLVM headers as system includes, -Werror)
make -C xlt clean && make -C xlt CXXFLAGS="-O2 -g -std=c++17 -Wall -Wextra -Wpedantic \
  -Isrc -isystem /usr/lib/llvm-18/include -Werror -fexceptions \
  -D_GNU_SOURCE -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS -D__STDC_LIMIT_MACROS" \
  2>&1 | tee /tmp/t6-build-strict.log

# quality audit
clang-format-18 --dry-run --Werror xlt/src/*.cpp xlt/src/*.h xlt/tests/*.cpp
git status --short && git status --ignored --short xlt
git ls-files -o --exclude-standard xlt
```

Note: the CLI is `xlt run <elf> [--stats]`; the task text's `xlt <elf> --stats` form would hit
the unknown-command path.

## Build and unit tests

- `make -C xlt clean && make -C xlt`: exit 0, binary `xlt/build/xlt` 9,843,640 bytes.
  The build prints 159 `warning:` lines, all from LLVM 18 headers under
  `/usr/lib/llvm-18/include` (19 from `llvm/ADT/DenseMap.h`, 18 from `llvm/IR/Instructions.h`,
  15 from `llvm/Target/TargetMachine.h`, plus `SimplePackedSerialization.h`, `OrcABISupport.h`,
  ...); **0 warnings come from `xlt/src` or `xlt/tests`**. The cause is the scaffold Makefile
  passing the LLVM include directory with `-I` rather than `-isystem`, so LLVM's headers are
  subject to `-Wpedantic`.
- Strict proof: rebuilding with the LLVM include dir as `-isystem` and adding `-Werror` gives
  exit 0 and zero warning/error lines (`/tmp/t6-build-strict.log`). xlt's own code is
  warning-free; the 159 lines are toolchain-header noise, not engine code. Fixing the `-I`
  hygiene in `xlt/Makefile` is a follow-up, outside this task's write scope.
- `make -C xlt test`: exit 0. `decode_test` reports **481 checks passed**, runner reports
  `xlt tests: pass (1 test binary(ies))`.
- `xlt/bench/build.sh bench`: exit 0; `file` says
  `ELF 64-bit LSB executable, ARM aarch64, version 1 (SYSV), statically linked, not stripped`;
  sha256 matches the asset from t3.

## TCG baseline (QEMU 8.2.2, `-accel tcg,tb-size=1536`, 60 s timeout)

Ticks per test, three independent invocations (ms = ticks / 62500):

| run | alu64 | memcpy | hash | ptrchase | indcall | total ticks | total ms | load 1m before/after |
|---|---:|---:|---:|---:|---:|---:|---:|---|
| R1 | 12,181,560 | 145,688,056 | 134,616,192 | 122,837,370 | 15,545,987 | 430,869,165 | 6893.90 | 4.96 / 5.93 |
| R2 | 11,471,737 | 146,777,774 | 135,556,594 | 131,371,870 | 15,914,586 | 441,092,561 | 7057.48 | 5.33 / 5.45 |
| R3 (integrated) | 11,145,881 | 152,638,694 | 140,434,066 | 133,191,874 | 18,580,356 | 455,990,871 | 7295.85 | 4.97 / - |
| median | 11,471,737 | 146,777,774 | 135,556,594 | 131,371,870 | 15,914,586 | 441,092,561 | 7057.48 | - |

TCG per-test ms: R1 194.90 / 2331.01 / 2153.86 / 1965.40 / 248.74; R2 183.55 / 2348.44 /
2168.91 / 2101.95 / 254.63; R3 178.33 / 2442.22 / 2246.95 / 2131.07 / 297.29.
This agrees with the t3 baseline (`xlt/bench/RESULTS.md`: 445,026,940 ticks / 7120.43 ms median
at load ~7.4); t6 ran at load 4.96-5.93 and lands in the same range. QEMU exits with status 1
on the successful semihosting `SYS_EXIT` path; the harness keys on the parsed rows, not on the
QEMU exit status.

Checksums: all five expected values matched in every TCG run (3/3).

## xlt engine

Command: `xlt/build/xlt run xlt/build/bench/bench.a64.elf --stats`, three runs (two direct,
one through `run.sh`). Ticks / ms per test:

| run | alu64 | memcpy | hash | ptrchase | indcall | total ticks | total ms |
|---|---:|---:|---:|---:|---:|---:|---:|
| X1 (direct) | 7,409,554 / 118.55 | 1,080,339 / 17.29 | 13,757,357 / 220.12 | 11,661,434 / 186.58 | 9,916,500 / 158.66 | 43,825,184 | 701.20 |
| X2 (direct) | 5,776,998 / 92.43 | 913,886 / 14.62 | 14,030,879 / 224.49 | 12,593,026 / 201.49 | 9,977,442 / 159.64 | 43,292,231 | 692.68 |
| X3 (integrated) | 5,913,344 / 94.61 | 963,724 / 15.42 | 14,074,730 / 225.20 | 12,565,482 / 201.05 | 10,243,675 / 163.90 | 43,760,955 | 700.17 |
| median | 5,913,344 | 963,724 | 14,030,879 | 12,565,482 | 9,977,442 | 43,760,955 | 700.17 |

(Per-column medians; the total column is the median of the three totals, not the sum of the
per-test medians.)

Engine-reported statistics (`--stats` plus the run summary line), identical counters in both
direct runs:

| field | run 1 | run 2 |
|---|---:|---:|
| wall | 875,493,996 ns | 867,230,023 ns |
| translate | 4,110,183 ns | 4,094,997 ns |
| opt+jit | 193,750,857 ns | 194,979,385 ns |
| regions / blocks-translated | 11 / 11 | 11 / 11 |
| blocks-executed | 16,000,015 | 16,000,015 |
| insns-translated | 509 | 509 |
| insns-executed | 343,894,262 | 343,894,262 |
| dispatches | 16,000,014 | 16,000,014 |
| guest exit code | 0 | 0 |

Interpretation: 11 regions cover the whole benchmark; each region is translated lazily once
(509 guest instructions in total, 343.9 M executed). `translate` (LLVM IR build) is ~4 ms;
`opt+jit` (LLVM optimization and ORC JIT) is ~194 ms, i.e. ~22% of the ~870 ms process wall
time and a real one-off cost that a steady-state engine would amortize.

## QEMU TCG vs xlt

Speedup = TCG ticks / xlt ticks. Pairs R1/X1, R2/X2 and the integrated R3/X3 invocation
(same harness run, same host conditions):

| test | R1/X1 | R2/X2 | R3/X3 (integrated) |
|---|---:|---:|---:|
| alu64 | 1.64x | 1.99x | 1.88x |
| memcpy | 134.85x | 160.61x | 158.38x |
| hash | 9.79x | 9.66x | 9.98x |
| ptrchase | 10.53x | 10.43x | 10.60x |
| indcall | 1.57x | 1.60x | 1.81x |
| **overall** | **9.83x** | **10.19x** | **10.42x** |

Using medians over the three runs: 441,092,561 / 43,760,955 = **10.08x**.
The acceptance threshold (overall >= 1.5x over TCG) is met by a wide margin; no test is below
1.57x in any pairing.

Wall-clock cross-check: engine process wall time was 875.49 ms and 867.23 ms for X1/X2, so
including process startup, ELF load and JIT compilation the wall-clock speedup is
7.87x-8.33x rather than 10.08x-10.42x. The 10.x figures are the guest-counter ratios printed
by the benchmark (same metric on both engines).

## Checksum evidence

`run.sh` compares each row against the expected checksum and exits nonzero on any mismatch.
All five values are byte-identical between the TCG guest and xlt across every run:

| test | expected (t3) | TCG seen (3/3) | xlt seen (3/3) |
|---|---|---|---|
| alu64 | `0x067a3bf250fa984e` | `0x067a3bf250fa984e` | `0x067a3bf250fa984e` |
| memcpy | `0x13fbe40bf4180000` | `0x13fbe40bf4180000` | `0x13fbe40bf4180000` |
| hash | `0x0000006a1365d0c6` | `0x0000006a1365d0c6` | `0x0000006a1365d0c6` |
| ptrchase | `0x000003d094fd9544` | `0x000003d094fd9544` | `0x000003d094fd9544` |
| indcall | `0xc2a86d1e196c073f` | `0xc2a86d1e196c073f` | `0xc2a86d1e196c073f` |

The integrated run printed `run.sh: all 5 checksums match` and exited 0. No checksum mismatch,
no missing row, no unsupported-instruction abort, no bad-memory abort in any run.

## Determinism

- Two direct engine runs (X1, X2) plus the integrated run (X3): all checksums identical.
- Engine counters identical between X1 and X2 (regions 11, blocks-executed 16,000,015,
  insns-executed 343,894,262, dispatches 16,000,014).
- Total engine ticks vary by 1.23% across the three runs (43,292,231-43,825,184 ticks);
  the short tests are noisier per-test (alu64 5.78-7.41 M, memcpy 0.91-1.08 M) because they
  are 14-226 ms and the host is shared. Checksums, not timings, are the correctness gate.

## Quality audit

- SPDX: every new C++/C/header/S file has `SPDX-License-Identifier: MIT` on its first line
  (`xlt/src/*.cpp`, `xlt/src/*.h`, `xlt/tests/decode_test.cpp`, `xlt/bench/bench.c`,
  `xlt/bench/start.S`); `xlt/bench/link.ld` line 1 is `/* SPDX-License-Identifier: MIT */`;
  the shell scripts (`xlt/bench/build.sh`, `xlt/bench/run.sh`, `xlt/tests/run_tests.sh`) carry
  the SPDX line on line 2 after the shebang.
- `clang-format-18 --dry-run --Werror` over `xlt/src/*.cpp`, `xlt/src/*.h`,
  `xlt/tests/*.cpp`: exit 0.
- `grep -rnE '/home/lshan|/tmp/|FIXME|XXX|HACK|TODO|#if 0'` over `xlt/`: no matches. No
  debug leftovers, no hardcoded absolute host paths.
- Git state: `M .gitignore`, `?? .agent-teams/`, `?? docs/xlt-translator.md`, `?? xlt/`,
  `?? reports/` (this report). `xlt/build/` is ignored by `.gitignore:65`; `git check-ignore`
  confirms `xlt/build/xlt`, `xlt/build/bench/bench.a64.elf` and `xlt/build/obj/main.o` are
  ignored. `git ls-files -o --exclude-standard xlt` lists 26 files, all source/doc (10 bench
  files, `xlt/Makefile`, `xlt/README.md`, `xlt/qemu/INTEGRATION.md`, `xlt/qemu/README.md`,
  10 `xlt/src` files, 2 `xlt/tests` files); no ELF or build artifact would be committed.
- The verifier changed no engine, decoder, benchmark or documentation source; the only file
  written is this report.
- Repo-wide `tools/check-links.py` still reports the same pre-existing 57 problems that are red
  on `main` as well (now over 376 references with this report); this report adds references but
  no new link problem, and `xlt/` has none. `tools/provenance-check.py --root .` is clean
  (495 tracked files; the report and the rest of `xlt/` are not staged yet).

## tb-size sensitivity panel (prepared for t6 evidence)

iosvm runs QEMU with `-accel tcg,tb-size=384`; the t3 harness hard-codes `tb-size=1536`.
Same ELF, same QEMU flags, two runs per size (direct QEMU because `run.sh` hard-codes 1536):

| tb-size | run | load 1m | alu64 | memcpy | hash | ptrchase | indcall | total ticks | total ms |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 384 | 1 | 4.17 | 11,115,310 | 147,804,131 | 135,276,083 | 118,640,204 | 15,748,796 | 428,584,524 | 6857.35 |
| 384 | 2 | 4.00 | 11,668,616 | 148,075,752 | 133,709,969 | 127,403,558 | 16,696,936 | 437,554,831 | 7000.88 |
| 1536 | 1 | 4.24 | 11,066,177 | 148,317,322 | 133,796,856 | 125,416,221 | 15,196,709 | 433,793,285 | 6940.69 |
| 1536 | 2 | 3.98 | 11,068,904 | 145,646,013 | 131,487,983 | 119,070,709 | 15,642,343 | 422,915,952 | 6766.66 |
| 4096 | 1 | 4.22 | 10,619,393 | 151,086,209 | 136,667,868 | 132,570,657 | 15,243,339 | 446,187,466 | 7139.00 |
| 4096 | 2 | 4.95 | 10,956,653 | 145,874,753 | 135,434,008 | 117,881,278 | 15,439,906 | 425,586,598 | 6809.39 |

| tb-size | mean total ms | range | spread |
|---:|---:|---|---:|
| 384 | 6929.11 | 6857.35-7000.88 | +2.1% |
| 1536 | 6853.67 | 6766.66-6940.69 | +2.6% |
| 4096 | 6974.19 | 6809.39-7139.00 | +4.8% |

Finding: tb-size is not a measurable factor on this image. 384 is +1.10% on the mean vs 1536,
4096 is +1.76%, and both are smaller than the run-to-run spread of one size (up to 4.8%).
The workloads are a few hot loops, so the translation buffer is reused and flushes are rare;
tb-size bounds memory/cold-start, not steady-state speed. Host load moves totals by a similar
amount (t3 baseline 7120.43 ms at load ~7.4 vs this panel 6853-6974 ms at load 4-5).
Recommendation: state the tb-size in every headline. Use `tcg,tb-size=384` for the iosvm-parity
panel (what users actually run), and 1536/4096 as sensitivity; checksums are independent of
tb-size (6/6). This report's headline tables use the harness default 1536.

Raw panel evidence: `/tmp/t6-tb/panel.tsv` and `/tmp/t6-tb/SUMMARY.md` plus per-run `.log` and
`.meta` files (the .meta files record date, full `/proc/loadavg`, sha256, QEMU rc, wall time and
the exact command). During the panel a concurrent `make -C xlt clean` from the t5 build removed
`xlt/build/`; one 4096 run was retried against a same-sha256 copy at `/tmp/t6-tb/bench.a64.elf`.
That does not affect the numbers and is recorded so the two paths are not mistaken for
different images.

## Limitations and caveats

- **M1 engine scope**: a flat 256 MiB guest RAM window at a fixed base, a single vCPU, static
  `ET_EXEC` AArch64 images only; no MMU, no dynamic loader, no threads, no self-modifying code.
  The benchmark image is exactly in scope (one PT_LOAD pair inside the window). Generality is
  not claimed.
- **QEMU accelerator integration is design-only**: `xlt/qemu/INTEGRATION.md` and
  `xlt/qemu/README.md` describe it; nothing in this report exercises or verifies it.
- **Timing metric**: xlt emulates `cntvct_el0` from host `CLOCK_MONOTONIC` scaled to 62.5 MHz;
  QEMU's virt counter likewise tracks host time. The ratios are elapsed-time ratios on one
  shared host, not guest instruction-cycle counts; absolute ticks depend on host speed.
- **Translation overhead**: xlt translates lazily, so the ~194 ms LLVM opt+JIT and ~4 ms IR
  translation happen inside the benchmark's measured window on first execution. TCG pays its
  own translation cost inside the same window, but wall-clock speedup (7.87x-8.33x) is lower
  than the printed guest-counter ratio (10.08x-10.42x); both numbers are reported above.
- **Host noise**: this is a shared 32-thread host at load 3.6-5.9; totals move by ~4-6% between
  runs. Checksums are deterministic; timings are a sample, not a threshold.
- **Build warnings**: the standard build emits 159 warnings from LLVM headers through `-I`
  (see Build and unit tests). No xlt source warning; a strict `-isystem -Werror` build is
  clean. The Makefile hygiene fix was left to the owner of `xlt/Makefile`.
- **Benchmark representativeness**: five micro-workloads in one freestanding image (ALU mix,
  byte-wise memcpy, open-addressing hash, pointer chase, indirect calls). It measures these
  patterns, not arbitrary AArch64 software.
- **`run.sh` check**: it invokes the engine as `xlt run <elf>` and merges stderr into the
  captured output; the 60 s timeout applies to both engines. The engine's stderr statistics
  line is not part of the parsed checksum rows.

## Verdict

PASS. Independent rebuild, unit tests (481 checks) and benchmark image build succeeded. Across
three TCG runs and three xlt runs every checksum is byte-identical to the expected AArch64
results; the engine is deterministic in its result and translation counters. Overall speedup is
9.83x-10.42x by guest counter (10.08x on medians) and 7.87x-8.33x by wall clock, comfortably
above the 1.5x acceptance threshold. Quality audit is clean: SPDX present, clang-format clean,
no debug leftovers or hardcoded paths, no binaries staged, benchmark build products ignored.
Caveats (M1 scope, design-only QEMU integration, translation overhead, shared-host noise,
LLVM-header warning noise, tb-size) are documented above.

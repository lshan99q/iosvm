# Bare-metal AArch64 benchmark

`xlt/bench/` holds the freestanding AArch64 image that drives the QEMU-TCG vs xlt
comparison for M1. It is not a unit test: it is a guest program that runs unchanged under
`qemu-system-aarch64 -M virt` (the TCG baseline) and under `xlt run` once the translator can
execute it, so both engines execute the same instructions and must produce the same checksums.
QEMU is the reference; the checksums below are what "the engine is correct" means, and the
cycle counts are what "the engine is fast" is measured against.

## Files

| path | role |
|---|---|
| `bench.c` | the five workloads and semihosting output; the only image in the checksum gate |
| `start.S` | `_start`: stack pointer, `.bss` zeroing, `bl main`, then `SYS_EXIT` |
| `link.ld` | image at `0x40080000`, 1 MiB stack, `__bss_start`/`__bss_end` |
| `memprobe.c` | diagnostic: load/store cost per access width (not in the gate) |
| `memcopy2.c` | diagnostic: 4 MiB copy isolation, 16 repetitions (not in the gate) |
| `widthprobe.c` | diagnostic: 1/2/4/8-byte and compiler-unrolled copy widths (not in the gate) |
| `build.sh` | builds the images into `../build/bench/` (git-ignored) |
| `run.sh` | runs TCG and xlt, checks checksums, prints the per-test speedup table |
| `RESULTS.md` | the measured TCG baseline, three runs |

No build product is ever committed: `xlt/build/` is in `.gitignore`, and the harness
rebuilds the ELF on demand.

## Design

The image is freestanding and has no C library: `bench.c` supplies `memcpy`, `memset`,
`memmove` and `memcmp`, and every buffer is a static arena. The only external interfaces are
the CPU's virtual counter and the semihosting calls, both of which are part of the QEMU
`-M virt` contract that xlt has to reproduce.

**Semihosting.** Output goes through `hlt #0xf000` with the call number in `x0` and a single
argument in `x1`:

| call | number | use here |
|---|---|---|
| `SYS_WRITE0` | `0x04` | write a NUL-terminated string (`x1` = string) |
| `SYS_EXIT` | `0x18` | terminate the image (`x1` = `0`, application exit) |

`run.sh` starts QEMU with `-semihosting-config enable=on,target=native`, which routes
`SYS_WRITE0` to the process's stderr; the harness merges the streams and parses the rows.
QEMU's own exit status is 1 on the `SYS_EXIT` path even when the workload completed, so the
harness deliberately does not use it: it uses the parsed checksum rows instead.

**Clock.** The measurement is `mrs cntvct_el0` around each workload. On this `virt` machine
the generic counter is 62.5 MHz, i.e. one tick is 16 ns, and

    milliseconds = ticks / 62.5e6 * 1e3 = ticks / 62500

The image does not calibrate the counter; the 62.5 MHz rate is a fixed platform contract
(`run.sh` prints it, `RESULTS.md` uses it). A tick is therefore comparable across TCG and
xlt, and a tick ratio is a time ratio.

**Memory map.** `link.ld` places the image at `0x40080000`, which is 512 KiB above the base
of QEMU `virt` RAM (`-m 512M`), and reserves a 1 MiB stack. Everything is in `.bss`, so the
ELF loaded by `-kernel` is under 70 KiB and the data never leaves the guest's RAM.

## The five workloads

`bench.c` runs them in this order and prints one line per test:

```
alu64: 0x067a3bf250fa984e cycles=...
memcpy: 0x13fbe40bf4180000 cycles=...
hash: 0x0000006a1365d0c6 cycles=...
ptrchase: 0x000003d094fd9544 cycles=...
indcall: 0xc2a86d1e196c073f cycles=...
sink=0x0000006a1365d0c6
```

| test | work | expected checksum |
|---|---|---|
| `alu64` | 30,000,000 iterations of a dependent 64-bit mix: xorshift64, adds, shifts, rotate, multiply-add, and an `if (x & 1)` branch | `0x067a3bf250fa984e` |
| `memcpy` | two 4 MiB buffers, 16 copies (64 MiB moved), a 64-byte-stride XOR after each copy, then one 4 MiB buffer summed as 524,288 `u64` words | `0x13fbe40bf4180000` |
| `hash` | open-addressing table of 2^20 entries, linear probing, 1,000,000 inserts followed by 1,000,000 lookups of xorshift-derived keys | `0x0000006a1365d0c6` |
| `ptrchase` | 2^20-node linked list laid out as a Fisher-Yates permutation of indices, then 8,000,000 dependent `next[p]` hops | `0x000003d094fd9544` |
| `indcall` | 8,000,000 indirect calls chosen from a 5-entry constant function table (`h = tab[h % 5](h)`) | `0xc2a86d1e196c073f` |

`out_hex` always prints 16 hex digits, so the shorter checksums above appear with leading
zeros. Compare values, not string lengths: `0x6a1365d0c6` is `0x0000006a1365d0c6`.

The checksums are architecture-independent by construction: the code uses fixed-width
integers and array indices, never pointers or host addresses, so the same build produces
the same results on any AArch64 implementation. `-mgeneral-regs-only` and the absence of
libc keep the instruction set small enough that the M1 lifter has to cover all of it.

## Build

    cd xlt/bench
    ./build.sh

This writes `xlt/build/bench/bench.a64.elf` (git-ignored). The exact compile line is:

    clang --target=aarch64-unknown-elf -O2 -ffreestanding -nostdlib \
      -fno-pic -fno-stack-protector -mgeneral-regs-only -mno-outline-atomics \
      -fno-asynchronous-unwind-tables -fuse-ld=lld \
      -Wl,-T,link.ld -Wl,--build-id=none \
      bench.c start.S -o ../build/bench/bench.a64.elf

The flags matter: no libc, no PIC, no stack protector, no outline atomics, no floating
point, no unwind tables, no build id. Requirements: clang with the AArch64 and lld backends
(clang 18 and lld 18 are what this tree uses) and `qemu-system-aarch64` (8.2 here).

The three diagnostic probes build the same way and are never part of the checksum gate:

    ./build.sh memprobe
    ./build.sh memcopy2
    ./build.sh widthprobe
    ./build.sh all          # every image
    ./build.sh clean        # remove xlt/build/bench/

## Run

    cd xlt/bench
    ./run.sh                # TCG, plus xlt when xlt/build/xlt works

`run.sh` builds the ELF if it is missing, runs the TCG baseline with

    timeout 60 qemu-system-aarch64 -M virt -cpu cortex-a76 -m 512M -smp 1 \
      -accel tcg,tb-size=1536 -kernel bench.a64.elf \
      -semihosting-config enable=on,target=native -nographic -monitor none

then, when `xlt/build/xlt` exists and produces benchmark rows, runs

    timeout 60 xlt/build/xlt run bench.a64.elf

and prints a table with per-test TCG ticks/ms, xlt ticks/ms, the speedup
(`tcg_ticks / xlt_ticks`) and the checksum verdict for each engine, plus totals.

Options: `--skip-xlt` (baseline only), `--elf PATH`, `--xlt PATH`, `--build` (force a
rebuild), `-h`/`--help`. While the M1 engine is a stub it exits 1 without printing rows;
`run.sh` reports the TCG baseline, leaves the xlt columns as `-`, and still exits 0.

Exit codes:

| code | meaning |
|---|---|
| 0 | every result row present and every checksum matches |
| 1 | a checksum is wrong, or a result row is missing for an engine that produced rows |
| 2 | the harness could not run at all (no QEMU/clang, QEMU timeout, bad option) |

Timings are host-noise sensitive - the machine is shared - so treat the cycle columns as a
baseline sample, not a threshold. Checksums are deterministic and are the gate.
`RESULTS.md` records the three baseline runs.

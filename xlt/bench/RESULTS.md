# TCG baseline results

Measured on 2026-11-28 (+08:00) on the shared x86-64 build host:

| item | value |
|---|---|
| CPU | 2x Intel Xeon E5-2698B v3 @ 2.00 GHz, 32 threads |
| kernel | Linux 7.0.0-42-generic |
| load average at measurement | 7.42, 7.67, 7.44 on 32 threads (the machine is busy) |
| QEMU | 8.2.2 (Debian 1:8.2.2+ds-0ubuntu1.18) |
| clang / lld | 18.1.3 / 18.1.3 |
| image | `xlt/build/bench/bench.a64.elf`, sha256 `d28c71ecd46ccd5ed9b0a24ddccdf407044900dc45d1618f52afd5e3fd643113` |

The three runs are independent invocations of the same harness:

    cd xlt/bench
    ./build.sh
    ./run.sh --skip-xlt        # run three times; --skip-xlt because M1's xlt is still a stub

Each run executes the TCG command documented in `README.md`
(`qemu-system-aarch64 -M virt -cpu cortex-a76 -m 512M -smp 1 -accel tcg,tb-size=1536 ...
-semihosting-config enable=on,target=native -nographic -monitor none`, 60 s timeout) and
parses the guest's `cntvct_el0` rows. The counter is 62.5 MHz on this machine, so
`ms = ticks / 62500` (one tick = 16 ns).

## Ticks per test

| test | run 1 | run 2 | run 3 | median |
|---|---:|---:|---:|---:|
| `alu64` | 11,757,227 | 10,980,928 | 11,356,939 | 11,356,939 |
| `memcpy` | 153,068,084 | 148,578,898 | 152,087,943 | 152,087,943 |
| `hash` | 138,969,757 | 133,602,313 | 135,655,612 | 135,655,612 |
| `ptrchase` | 141,787,781 | 131,460,746 | 130,451,169 | 131,460,746 |
| `indcall` | 15,768,979 | 15,454,644 | 15,475,277 | 15,475,277 |
| **total** | **461,351,828** | **440,077,529** | **445,026,940** | **445,026,940** |

## Milliseconds per test

| test | run 1 | run 2 | run 3 | median | spread (max/min - 1) |
|---|---:|---:|---:|---:|---:|
| `alu64` | 188.11 | 175.69 | 181.71 | 181.71 | +7.1% |
| `memcpy` | 2449.08 | 2377.26 | 2433.40 | 2433.40 | +3.0% |
| `hash` | 2223.51 | 2137.63 | 2170.48 | 2170.48 | +4.0% |
| `ptrchase` | 2268.60 | 2103.37 | 2087.21 | 2103.37 | +8.7% |
| `indcall` | 252.30 | 247.27 | 247.60 | 247.60 | +2.0% |
| **total** | **7381.62** | **7041.24** | **7120.43** | **7120.43** | **+4.8%** |

The whole image takes about 7.0-7.4 s of wall time; the three long workloads
(`memcpy`, `hash`, `ptrchase`) account for ~94% of the total, and `alu64` + `indcall` are a
couple of hundred milliseconds each. Spread is 2-9% per test across the three runs, which is the
noise floor on this shared host: use the medians as the baseline, not the extremes. The
checksums must be identical regardless of load; they were, in all three runs.

## Checksums

Every run printed these values and `run.sh` compared them against the expected set:

| test | printed by `out_hex` | expected | runs |
|---|---|---|---|
| `alu64` | `0x067a3bf250fa984e` | `0x067a3bf250fa984e` | 3/3 match |
| `memcpy` | `0x13fbe40bf4180000` | `0x13fbe40bf4180000` | 3/3 match |
| `hash` | `0x0000006a1365d0c6` | `0x0000006a1365d0c6` | 3/3 match |
| `ptrchase` | `0x000003d094fd9544` | `0x000003d094fd9544` | 3/3 match |
| `indcall` | `0xc2a86d1e196c073f` | `0xc2a86d1e196c073f` | 3/3 match |

Raw output of run 2:

```
XLT-BENCH v1
alu64: 0x067a3bf250fa984e cycles=10980928
memcpy: 0x13fbe40bf4180000 cycles=148578898
hash: 0x0000006a1365d0c6 cycles=133602313
ptrchase: 0x000003d094fd9544 cycles=131460746
indcall: 0xc2a86d1e196c073f cycles=15454644
sink=0x0000006a1365d0c6
```

QEMU itself exits with status 1 on the semihosting `SYS_EXIT` path even on success; that is
why `run.sh` keys on the parsed rows and `RESULTS.md` does not treat QEMU's status as a
failure. All three harness invocations exited 0.

## xlt column

Not measured yet. `xlt/build/xlt` is the M1 scaffold: it prints "the ELF loader and the
translator land in M1 after this scaffold" and exits 1 without running guest code, so
`run.sh` reports `-` for the xlt columns and warns that the TCG baseline is all it can show.
When the engine produces `XLT-BENCH` rows, the same command prints the per-test and total
speedups, which is the actual QEMU-TCG-vs-xlt comparison for M1.

## Probe context (not part of the checksum gate)

The three diagnostic images isolate the long workloads on the same host and QEMU. Single
run each, so treat these as context, not as a second baseline.

`widthprobe` moves the same 64 MiB (4 MiB x 16) with different element widths:

| variant | ticks |
|---|---:|
| 1-byte loop (`copy1`) | 148,080,603 |
| 2-byte loop (`copy2`) | 74,768,063 |
| 4-byte loop (`copy4`) | 39,649,234 |
| 8-byte loop (`copy8`) | 18,817,830 |
| constant-size `__builtin_memcpy` (`builtin`, 16-byte `ldp`/`stp`) | 17,356,704 |

`memcopy2` copies 4 MiB sixteen times and reports 9.0-9.5 M ticks per copy, which reconciles
with `widthprobe copy1`: `bench.c`'s `memcpy` workload is a byte-at-a-time loop (it has no
libc, and `-mgeneral-regs-only` leaves no vector copy), so each 4 MiB copy costs ~9 M ticks
and the 16 copies are the ~2.4 s baseline. A compiler-generated 16-byte `ldp`/`stp` copy is
~8.5x cheaper per byte; that gap is TCG's per-guest-instruction overhead, not memory
bandwidth.

`memprobe`, 8M accesses per line, for load/store-width costs visible to the lifter:

| st1 | ld1 | st2 | ld2 | st4 | ld4 | st8 | ld8 | rdwr8 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,359,348 | 1,430,993 | 1,350,985 | 1,511,312 | 1,310,507 | 1,345,145 | 1,622,015 | 1,520,405 | 22,257,318 |

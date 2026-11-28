# P2.1: stage-1 MMU, synchronous aborts and ERET

P2.1 keeps the P1 contract (`-M xlt` only with `-accel xlt`, CPU-less machine, PL011, PSCI
SYSTEM_OFF/RESET) and adds the minimum EL1 architectural state and translation path needed by the
P2 fixtures. The implementation lives in `xlt/src/p2.cpp`; the QEMU machine only owns the
authoritative sysreg block and MMIO/semihosting callbacks.

## What is implemented

- `ArmSysRegs` in `hw/arm/xltm.c`: SCTLR_EL1, TTBR0/1_EL1, TCR_EL1, MAIR_EL1, VBAR_EL1, ESR/FAR/
  ELR/SPSR_EL1, SP_EL0/SP_EL1, MDSCR_EL1, DAIF, CurrentEL. The engine mirrors every write through
  `HostOps::write_sysreg`, so machine state and engine state stay identical.
- 4 KiB granule stage-1 EL1 translation: T0SZ/T1SZ and TTBR selection, L0-L3 walks, 4 KiB pages and
  2 MiB/1 GiB blocks, MAIR normal/device attributes, AF, AP[2:1] and UXN/PXN checks. MMIO is never
  cached; RAM uses the direct host pointer when translation permits it.
- 512-entry direct-mapped software TLB, indexed by `(va >> 12) & 511`; a generation counter is
  bumped on `tlbi`, SCTLR/TTBR/TCR/MAIR/VBAR writes and `ic ivau`.
- Synchronous instruction/data aborts (EC 0x21/0x25 for the current EL), including the IL bit and
  the TCG-compatible `0x96000006` WnR/translation-level encoding. The vector is `VBAR_EL1 + 0x200`
  (current EL, SPx), with ELR/SPSR/FAR/ESR/DAIF/SPSel/SP_EL1 set for EL1h.
- `eret` restores PC, NZCV, DAIF, SPSel and the SP bank; `msr spsel/daifset/daifclr` update the same
  state. `smc`, `hlT #0xf000` semihosting and PSCI `hvc #0` remain available.
- QEMU semihosting: `SYS_WRITE0` writes the guest string to the PL011 chardev; `SYS_EXIT` exits with
  its argument; any other operation is a clean fatal error instead of a silent no-op.

## Fixtures and oracle

`xlt/tests/fixtures/p2/mmu/` is the t17 P2.1 image (SHA-256
`92b291eaff109277d61b0dea6beb9eace5a35052aa39ccd1cba7d86a970991fe`). It installs L1/L2/L3 tables,
aliases `0x80080000` to `0x40200000`, takes exactly one data abort at `0x70000000`, repairs the
table, executes `dsb`/`tlbi vmalle1`/`dsb`/`isb`, and returns through `eret`.

```text
xlt-p2.1-start
alias: pa=0x0000000040200000 va=0x0000000080080000 val=0x1122334455667788
abort: esr=0x0000000096000006 far=0x0000000070000000 elr=0x0000000040081550 elr_off=0x0000000000001550 recovered=0x00000000600df00d faults=0x0000000000000001
xlt-p2.1-ok
```

`xlt/tests/fixtures/p2/semihost/` exercises `SYS_WRITE0`/`SYS_EXIT` through `hlt #0xf000` and
prints `xlt-p2-semihost-ok`.

Run both engines and compare bytes:

```sh
xlt/qemu/p2/run.sh
# ok: p2-mmu: TCG/xlt identical (259 bytes)
# ok: p2-semihost: TCG/xlt identical (19 bytes)
# run.sh: PASS (P2.1 MMU/fault/ERET and semihosting parity)
```

The CLI path (`xlt run <elf>`) models the PL011 FR/DR registers and PSCI SYSTEM_OFF/RESET, so the
same MMU image runs without QEMU.

## Known limits / next stage

- Single vCPU, no GIC, no IRQ delivery, no WFI wake-up, no virtual timer interrupt, no
  `ICC_*` system registers. P2.3 adds GICD/GICR MMIO, ICC sysregs, IRQ vector 0x280 and PPI30;
  the engine loop's slice boundary is where that poll hook belongs.
- The xlt accelerator now runs the guest without holding the BQL; MMIO callbacks take it only
  around `address_space_rw`. Timer/IRQ/QMP progress must be re-checked when P2.3 adds interrupts.
- `-M xlt` remains a CPU-less machine, not `-M virt`: the t15 spike showed that making `virt`
  legal without TCG needs CPUARMState/libxlt state synchronisation and is not a bounded change.

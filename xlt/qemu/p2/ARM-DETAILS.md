# P2.2/P2.3 implementation reference: exceptions, ESR, PSCI, timer, GICv3

This is a reference for the code that P2.2 (exceptions, EL0/EL1, VBAR, PSCI) and P2.3
(timers, GICv3, PL011 RX) will write. It is not a plan: the plan, milestones, exit
criteria and risks stay in [PLAN.md](PLAN.md); the accelerator contract and hook IDs
stay in [INTEGRATION.md](../INTEGRATION.md).

The machine introduced in P2 is the CPU-less `-M xlt` machine. There is no
`CPUARMState`, no `ARMCPU`, no TCG and no QEMU CP-register table: every state element in
this document is owned by the engine (libxlt) or by the machine model, and the tables
below say which bits that state must hold and how to compute them.

## Verification basis

Every table and formula is taken from one of these sources:

- QEMU 8.2.2 source, pristine tree extracted at `/home/lshan/xlt/qemu-8.2.2`
  (`hw/intc/arm_gicv3_*.c`, `target/arm/helper.c`, `target/arm/syndrome.h`,
  `target/arm/internals.h`, `target/arm/cpu.h`, `target/arm/tcg/helper-a64.c`,
  `include/hw/arm/bsa.h`, `include/hw/intc/gicv3_internal.h`). Line numbers are from
  that tree, which is the same release the accelerator patches apply to.
- Two TCG oracles produced with upstream QEMU 8.2.2 on this host:
  - P2.1 (`/home/lshan/xlt/p2-fixtures/NOTES.md`): EL1 MMU, alias mapping, one recovered
    data abort, serial output sha256
    `53c45c140060a81b5806de54686cef69827c04d4aec8711f306b09d684e9d4c6`.
  - P2.2 (`/home/lshan/xlt/p2-fixtures/p2.2/NOTES.md`): EL0 `svc #0x42`, lower-EL
    permission fault, DAIF round trip, ERET with an invalid SPSR, serial output sha256
    `b9eef31443d4fb3e2ef5e9b9cd0ac422ae72bcf369b7687f2124650bf42b6e62`.
  The oracle values in this document are copied from those two runs, not recomputed.
- ARM ARM (DDI 0487) for architectural rules. Section names are given; revision letter
  depends on the PDF revision, so re-check a specific number before quoting it
  normatively.

Marked **unverified** below: anything about EDK2 or XNU behaviour, because no EDK2 or
XNU source tree is vendored in this repository and the local AAVMF firmware image is
built from compressed firmware volumes (a byte search for the PSCI function IDs found
nothing, so it cannot be used as evidence).

## Register key convention

xlt's decoder reports `MRS`/`MSR` system registers as the raw capstone `sys` operand: a
16-bit key built from the five architectural fields,

```
key = (op0 << 14) | (op1 << 11) | (CRn << 7) | (CRm << 3) | op2
```

For example `SPSR_EL1` is `S3_3_0_C4_C0_0` → `0xC200`, `DAIF` is `0xDA11` (bit 15 set
because `op0 = 3` is encoded in bits 15:14). The engine's existing table in
`xlt/src/p2.cpp` already uses this form; every table below gives both forms so a new
register can be added without decoding the ARM ARM again.

---

## 1. Exception entry and return

### 1.1 Vector table

`VBAR_EL1` holds the vector base. It must be 2 KiB aligned; bits [10:0] of the register
are RES0 and read as zero. The table has 16 entries, 128 bytes apart, four exception
types per source:

| Offset | Source | Exception |
| --- | --- | --- |
| `0x000` | current EL with SP_EL0 (EL1t) | synchronous |
| `0x080` | current EL with SP_EL0 | IRQ |
| `0x100` | current EL with SP_EL0 | FIQ |
| `0x180` | current EL with SP_EL0 | SError |
| `0x200` | current EL with SP_ELx (EL1h) | synchronous |
| `0x280` | current EL with SP_ELx | IRQ |
| `0x300` | current EL with SP_ELx | FIQ |
| `0x380` | current EL with SP_ELx | SError |
| `0x400` | lower EL, AArch64 | synchronous |
| `0x480` | lower EL, AArch64 | IRQ |
| `0x500` | lower EL, AArch64 | FIQ |
| `0x580` | lower EL, AArch64 | SError |
| `0x600` | lower EL, AArch32 | synchronous |
| `0x680` | lower EL, AArch32 | IRQ |
| `0x700` | lower EL, AArch32 | FIQ |
| `0x780` | lower EL, AArch32 | SError |

P2.2 needs only three of these: `0x200` (synchronous taken at EL1h), `0x400`
(synchronous taken from EL0 into EL1) and, from P2.3, `0x280` (IRQ taken at EL1h). The
AArch32 rows are unreachable because the engine is AArch64-only, and the SP_EL0 rows are
unreachable because nothing runs EL1 with `PSTATE.SP == 0`.

QEMU computes the address in `arm_cpu_do_interrupt_aarch64` (`target/arm/helper.c:11093`):

1. `base = env->cp15.vbar_el[new_el]`.
2. If `cur_el < new_el`: add `0x400` when the immediately lower EL is AArch64, `0x600`
   when it is AArch32 (`is_a64()` for EL1, `HCR_EL2.RW` for EL2, `SCR_EL3.RW` for EL3).
3. Otherwise, if `PSTATE.SP` is set: add `0x200`.
4. Add the exception-type offset: IRQ and VIRQ `0x80`, FIQ and VFIQ `0x100`, VSERR
   `0x180`; synchronous exceptions keep the offset chosen in steps 2–4.
5. Abort entries also write `FAR_EL1 = env->exception.vaddress` (`helper.c:11178`-ish in
   the same function).

The engine needs steps 1–4 only; `FAR_EL1` is written by the trap code, not by the entry
code, because the engine knows the fault address at trap time.

### 1.2 PSTATE fields

PSTATE bits used by the engine (`target/arm/cpu.h:1560-1589`):

| Field | Bit(s) | Value | Meaning |
| --- | --- | --- | --- |
| `M` | 3:0 | `0b0000`-`0b1111` | EL and stack selector: `EL0t=0`, `EL1t=4`, `EL1h=5`, `EL2t=8`, `EL2h=9`, `EL3t=12`, `EL3h=13` |
| `nRW` | 4 | 1 = AArch32 | must be 0 for this port |
| `F` | 6 | 1 = masked | FIQ mask |
| `I` | 7 | 1 = masked | IRQ mask |
| `A` | 8 | 1 = masked | SError mask |
| `D` | 9 | 1 = masked | debug mask |
| `BTYPE` | 11:10 | - | branch type (BTI); no BTI on this model |
| `SSBS` | 12 | - | speculation barrier; no effect in xlt |
| `IL` | 20 | 1 = illegal execution state | see 1.5 |
| `SS` | 21 | - | single step; not modelled |
| `PAN` | 22 | - | privileged access never; not modelled |
| `UAO` | 23 | - | user access override; not modelled |
| `DIT` | 24 | - | data independent timing; not modelled |
| `TCO` | 25 | - | tag check override; no MTE |
| `NZCV` | 31:28 | N=31, Z=30, C=29, V=28 | condition flags |

`PSTATE.DAIF` is `D|A|I|F = 0x3C0`. Mode values are `(EL << 2) | handler`, so
`aarch64_pstate_mode(1, true)` = `0x5` (`EL1h`) and `0x3C5` is "EL1h with everything
masked" — the value the P2.2 oracle prints for `SPSR_EL1` after a trap to EL1.

### 1.3 SPSR_EL1 layout

`SPSR_EL1` is the saved PSTATE of the interrupted context, in PSTATE bit positions:

| Bits | Field |
| --- | --- |
| 31:28 | NZCV |
| 25 | TCO |
| 24 | DIT |
| 23 | UAO |
| 22 | PAN |
| 21 | SS |
| 20 | IL |
| 12 | SSBS |
| 11:10 | BTYPE |
| 9:6 | DAIF |
| 4 | nRW (M[4]) |
| 3:0 | M[3:0] |

Values seen in the oracle: EL0t with DAIF masked = `0x00000000000003C0`; EL1h with DAIF
masked = `0x00000000000003C5`; the illegal-return SPSR shows `IL=1` on top of EL1h:
`0x00000000001003C5`.

### 1.4 SP banking

There is one active stack pointer (`xregs[31]` in QEMU, `regs->sp` in xlt) plus four
banks `SP_EL0..SP_EL3` (`target/arm/internals.h:220-256`):

| Situation | Active SP lives in |
| --- | --- |
| `PSTATE.SP == 0` (EL0, or EL1t/EL2t) | `SP_EL0` |
| `PSTATE.SP == 1` | `SP_EL<current EL>` |

Rules the engine must implement:

- `save_active_sp(el)`: if `PSTATE.SP` then `SP_EL[el] = SP` else `SP_EL[0] = SP`.
- `restore_active_sp(el)`: the inverse — load `SP` from `SP_EL[el]` or `SP_EL[0]`.
- `MSR SPSel, #imm` (`p2.cpp` already handles `PSTATE` register `0x05`): save the old
  active SP, flip `PSTATE.SP`, load the new active SP.
- Exception entry: save the outgoing active SP for `cur_el`, set `PSTATE.SP = 1` (so the
  new EL runs on its own bank), then restore the active SP for `new_el`.
- Exception return: restore `PSTATE` from `SPSR_ELx`, then restore the active SP for the
  EL named in `SPSR.M`.

The P2.2 fixture sets `SP_EL1` and `SP_EL0` at boot, enters EL0 with `SP_EL0`, traps back
to EL1 on the `SP_EL1` stack, and returns to EL0 with `SP_EL0` again; a handler that
clobbers `SP_EL0` or forgets to save `x0-x30` fails the oracle.

### 1.5 Exception entry sequence

For a trap taken from EL0/AArch64 into EL1/AArch64 the engine must do exactly this
(mirrors `helper.c:11093-11230`):

1. `save_active_sp(cur_el)` — stash EL0's `xregs[31]` into `SP_EL0`.
2. `ELR_EL1 = trap_pc` — for a synchronous trap this is the address of the faulting
   instruction, except for `SVC`/`HVC`/`SMC`, where it is the following instruction.
3. `SPSR_EL1 = pstate` (the old PSTATE, including NZCV and DAIF).
4. `pstate = PSTATE_DAIF | EL1h` — all of D, A, I, F masked, `M = 0x5`.
5. `restore_active_sp(new_el)` — set `xregs[31] = SP_EL1`.
6. `ESR_EL1 = <syndrome>` and, for aborts, `FAR_EL1 = fault address`.
7. `PC = VBAR_EL1 + <vector offset>`.

Steps 2–4 and 7 are exactly what QEMU's `arm_cpu_do_interrupt_aarch64` does; the engine
keeps PSTATE in `ArmSysRegs` instead of `CPUARMState::pstate`.

### 1.6 Exception return and the illegal-state rule

`ERET` restores PSTATE from `SPSR_EL1` and jumps to `ELR_EL1`. It is an *illegal return*
when any of the following holds (QEMU's predicate list,
`target/arm/tcg/helper-a64.c:760-911`; the checks that involve EL2/EL3 are unreachable in
this port):

1. The target EL encoded in `SPSR_EL1.M` is not implemented or is higher than the
   current EL. In this port `new_el > cur_el` is illegal (there is no EL2; the guest runs
   at EL1 and may only return to EL0), and so is `new_el == 2` even when `M` names it.
2. `SPSR_EL1.M[1]` is set (reserved bit).
3. `SPSR_EL1.M == 1` (EL0 with `M[0]` set).
4. The target EL would be AArch32 while the returning EL is AArch64 (`nRW == 1` here).

Architectural behaviour of an illegal return (QEMU `illegal_return:` label,
`helper-a64.c:887-911`; matches the P2.2 oracle):

- set `PSTATE.IL = 1`,
- set `PC = ELR_EL1` (the bad return address is kept, the instruction there is not
  executed),
- restore only NZCV and DAIF from `SPSR_EL1` (other PSTATE fields, the EL and the stack
  pointer do not change),
- the next instruction fetch traps: `EC = 0x0E` (illegal execution state), `IL = 1`,
  `ISS = 0`, `ESR_EL1 = 0x3A000000`, `ELR_EL1 = <that instruction>`,
  `SPSR_EL1 = <PSTATE with IL set>`. In the oracle: `ERET` with `SPSR_EL1 = 0x3C9`
  (`M = EL2h`) produced `ESR_EL1 = 0x3A000000`, `ELR_EL1 = 0x40082654`,
  `SPSR_EL1 = 0x1003C5`, and `PSTATE.IL == 1`.

So the engine's `ERET` implementation is: compute the target EL; if legal, save the
active SP, `pstate = SPSR_EL1` (mask to the implemented fields), restore the active SP,
`PC = ELR_EL1`; if illegal, set `PSTATE.IL`, `PC = ELR_EL1`, copy only NZCV and DAIF.
`PSTATE.IL` must then be checked before executing the next instruction (QEMU does this at
translate time, `target/arm/tcg/translate-a64.c:14106-14112`: illegal-state has priority
over BTI, but comes after instruction abort), and cleared only by an `ERET`/exception
return that writes a clean PSTATE (the oracle clears it by returning with
`SPSR_EL1 = 0x3C5`).

### 1.7 Handler save/restore contract

A P2.2/P2.3 handler written by us or by the payload must be able to run on the engine and
on TCG unchanged:

- On entry `x0-x30`, `ELR_EL1`, `SPSR_EL1`, `SP_EL0` are live and `DAIF` is masked; the
  handler must save `x0-x30` (16-byte aligned frame) and `ELR_EL1`/`SPSR_EL1` if it wants
  to return.
- Synchronous traps leave `ELR_EL1` at the faulting instruction (`SVC`/`HVC` excepted),
  so a handler that fixes a mapping and returns retries the instruction; a handler for
  `SVC` must not add 4.
- `ERET` is the only way back; it consumes `SPSR_EL1` and `ELR_EL1`.
- The IRQ handler (P2.3) is entered at `0x280` with `DAIF` masked; it must not use
  `ERET` with a stale `SPSR_EL1`.

### 1.8 What the engine needs in `ArmSysRegs`

Fields implied by this section: `pstate` (or `M`, `DAIF`, `IL`, NZCV split out),
`spsr_el1`, `elr_el1`, `sp_el[0..1]`, `vbar_el1`, `esr_el1`, `far_el1`, and
`xregs[0..30]` plus the active `sp`. `current_el()` is `pstate.M[3:2]`; EL0/EL1 is the
only legal pair.

---

## 2. ESR_EL1 encodings

### 2.1 Layout

`ESR_EL1` is 64-bit, but only the low 32 bits are used:

| Bits | Field |
| --- | --- |
| 31:26 | `EC` — exception class |
| 25 | `IL` — instruction length: 1 = 32-bit instruction |
| 24 | `ISV` — ISS valid (data aborts only) |
| 24:0 | `ISS` — class-specific syndrome |

`IL` is 1 for every AArch64 instruction and for exceptions raised without a specific
instruction. It is 0 only for exceptions from 16-bit (T32) instructions and for some
external aborts; this port never executes AArch32, so set `IL = 1` everywhere except
where a rule below says otherwise.

### 2.2 Exception classes used by P2.2/P2.3

| EC | Name | Raised on |
| --- | --- | --- |
| `0x00` | unknown | never from this port |
| `0x0E` | illegal execution state | next instruction after an illegal `ERET` |
| `0x15` | SVC from AArch64 | `svc #imm16` at EL0 or EL1 |
| `0x16` | HVC from AArch64 | `hvc #imm16` (PSCI conduit) |
| `0x18` | trapped MSR/MRS | unimplemented system register |
| `0x20` | instruction abort from lower EL | fetch fault while `cur_el < target_el` |
| `0x21` | instruction abort from same EL | fetch fault at EL1 |
| `0x22` | PC alignment fault | not modelled (no alignment checking) |
| `0x24` | data abort from lower EL | load/store fault while at EL0 |
| `0x25` | data abort from same EL | load/store fault while at EL1 |
| `0x26` | SP alignment fault | not modelled |
| `0x2C` | FP/SIMD trap | not modelled (FP always enabled) |
| `0x2F` | SError | P2.3 only, if RAS is advertised (it is not) |
| `0x3C` | BRK from AArch64 | `brk #imm16` |

(Values from `target/arm/syndrome.h:31-79`.)

### 2.3 SVC64, EC `0x15`

`ESR_EL1 = (0x15 << 26) | (1 << 25) | (imm16 & 0xffff)`; ISS is the 16-bit immediate.
Oracle example: `svc #0x42` → `0x56000042`, `ELR_EL1` = address of the instruction after
the `SVC`. `HVC` uses the same shape with `EC = 0x16`.

### 2.4 Data abort, EC `0x24`/`0x25`

`EC = 0x24` when taken from a lower EL (EL0), `0x25` when taken at the current EL. ISS is
only valid when `ISV = 1` (see the note at the end of this subsection):

| Bits | Field | Meaning |
| --- | --- | --- |
| 24 | ISV | ISS holds the access syndrome |
| 23:22 | SAS | access size: 0 = byte, 1 = halfword, 2 = word, 3 = doubleword |
| 21 | SSE | sign-extended load |
| 20:16 | SRT | transfer register (Rt) |
| 15 | SF | access was 64-bit |
| 14 | AR | acquire/release semantics |
| 13 | VNCR | access to an `VNCR` register (FEAT_NV2) |
| 12:11 | SET | FEAT_LS64 |
| 10 | FnV | `FAR_EL1` is not valid |
| 9 | EA | external abort |
| 8 | CM | cache maintenance |
| 7 | S1PTW | fault on a stage-2 walk for a stage-1 translation |
| 6 | WnR | 1 = write, 0 = read |
| 5:0 | DFSC | data fault status code |

With `ISV = 0`, bits 23:14 are RES0 and only FnV/EA/CM/S1PTW/WnR/DFSC are meaningful.
QEMU's `merge_syn_data_abort()` (`target/arm/tcg/tlb_helper.c:29-60`) sets `ISV` only for
stage-2 aborts routed to EL2 and never for stage-1 faults, so a TCG-run payload sees
`ISV = 0` for the P2.1/P2.2 faults. The engine should do the same in P2.2 (stage-1 only,
no `SAS/SRT/SF` fill-in) and keep the option to fill `ISV` later for its own disassembly
or gdbstub use; filling it does not change the oracle output because the fixture does not
inspect ISS. **Unverified:** whether EDK2's abort handler ever looks at `SAS/SRT`; the
P2.4 boot is the test.

### 2.5 Instruction abort, EC `0x20`/`0x21`

ISS layout: FnV[10], EA[9], S1PTW[7], IFSC[5:0]. `EC = 0x20` when `cur_el < target_el`,
`0x21` when taken at the same EL. `FAR_EL1` is the faulting instruction address unless
`FnV` is set. `ELR_EL1` is the faulting address (not the following instruction), so
installing a mapping and returning retries the fetch.

### 2.6 Trapped MSR/MRS, EC `0x18`

ISS: `Op0[21:20]`, `Op2[19:17]`, `Op1[16:14]`, `CRn[13:10]`, `Rt[9:5]`, `CRm[4:1]`,
`Direction[0]` (0 = write/MSR, 1 = read/MRS). `IL = 1`. The engine raises this instead of
treating an unknown register as an unsupported-instruction exit, which lets a payload
handler emulate it. P2.2 should raise it for any `MRS`/`MSR` not in `ArmSysRegs` (the
current engine calls `xlt_unsupported`, exit 71).

### 2.7 Illegal execution state, EC `0x0E`

`ESR_EL1 = (0x0E << 26) | (1 << 25) = 0x3A000000`; ISS is RES0 and `FAR_EL1` is UNKNOWN.
Raised when the instruction is about to execute with `PSTATE.IL == 1` — QEMU checks this
at translate time *after* the instruction fetch (so an instruction abort at the same
address wins, `translate-a64.c:14106-14112`). Oracle: `0x3A000000` with
`SPSR_EL1 = 0x1003C5`.

### 2.8 Fault status codes (DFSC/IFSC, low 6 bits)

| Value | Name | Level |
| --- | --- | --- |
| `0x00`-`0x03` | address size fault, level 0-3 | walk level |
| `0x04`-`0x07` | translation fault, level 0-3 | walk level |
| `0x08`-`0x0B` | access flag fault, level 0-3 | walk level |
| `0x0C`-`0x0F` | permission fault, level 0-3 | walk level |
| `0x10` | synchronous external abort | - |
| `0x11` | asynchronous external abort | - |
| `0x14`-`0x17` | synchronous external abort on translation table walk, level 0-3 | walk level |
| `0x18` | synchronous parity or ECC error | - |
| `0x21` | alignment fault | - |
| `0x22` | debug event | - |
| `0x30` | TLB conflict abort | - |
| `0x31` | unsupported atomic hardware update fault | - |

(QEMU encoding: `arm_fi_to_lfsc()` in `target/arm/internals.h:496-560`; translation
`0b000100|level`, access flag `0b001000|level`, permission `0b001100|level`,
`ARMFault_Alignment` = `0x21`. The table lists the 6-bit FSC field; the long-format FSR
also carries LPAE bit 9, which the engine does not need if it only writes `ESR_EL1`.)

The level is the level of the *faulting* descriptor: an invalid L2 entry gives
translation fault level 2 (`0x06`), an L3 page whose AP bits deny the access gives
permission fault level 3 (`0x0F`). Oracle values: P2.1 `0x96000006` (EC `0x25`, DFSC
`0x06`), P2.2 `0x9200000F` (EC `0x24`, DFSC `0x0F`).

### 2.9 Trap → ESR mapping for the engine

| Trap | EC | ISS fields | FAR/ELR | Vector |
| --- | --- | --- | --- | --- |
| `svc #imm` | `0x15` | `imm16` | ELR = next insn | `0x200` from EL1h, `0x400` from EL0 |
| `hvc #imm` | `0x16` | `imm16` | ELR = next insn | `0x200` (PSCI handled by machine) |
| `mrs/msr` unknown reg | `0x18` | op0/op1/op2/CRn/CRm/Rt/dir | ELR = insn | `0x200`/`0x400` |
| data fault | `0x24`/`0x25` | FnV=0, WnR, DFSC | FAR = address, ELR = insn | `0x400`/`0x200` |
| fetch fault | `0x20`/`0x21` | FnV=0, IFSC | FAR = address, ELR = address | `0x400`/`0x200` |
| illegal state | `0x0E` | 0 | FAR unknown, ELR = insn | `0x200`/`0x400` |
| `brk #imm` | `0x3C` | `imm16` | ELR = next insn | `0x200`/`0x400` |

`0x24`/`0x20` are used when `cur_el < target_el` (EL0 → EL1), `0x25`/`0x21` when the
fault is taken at the same EL (EL1 → EL1). With no EL2, `target_el` is always 1.

---

## 3. PSCI

### 3.1 Conduit

The machine uses `HVC #0` as the PSCI conduit (the accelerator machine has no EL3 and no
secure firmware, so `arm_cpu_do_interrupt`'s PSCI branch would be disabled anyway).
`HVC` from AArch64 traps with `EC = 0x16`; the machine's trap dispatcher runs the PSCI
handler *instead of* delivering an exception, and writes the result into `x0`.

### 3.2 Function IDs and calling convention

Function ID in `x0`, arguments in `x1`-`x3`, result returned in `x0`. AArch64 uses the
64-bit IDs (`0xC4...`); the 32-bit IDs (`0x84...`) are for AArch32 callers. QEMU's table
(`target/arm/kvm-consts.h:43-125`) and switch (`target/arm/tcg/psci.c:28-160`):

| Function | ID (AArch64) | Arguments | Return |
| --- | --- | --- | --- |
| `PSCI_VERSION` | `0x84000000` | - | `0x00010001` (PSCI 1.1) |
| `CPU_SUSPEND` | `0x84000001` | power state, entry, context | `SUCCESS` (single vCPU, no low-power states) |
| `CPU_OFF` | `0x84000002` | - | never returns |
| `CPU_ON` | `0xC4000003` | target MPIDR, entry, context | `SUCCESS` / `ALREADY_ON` / `INVALID_PARAMS` |
| `AFFINITY_INFO` | `0xC4000004` | target MPIDR, lowest affinity level | `0` running, `1` off, `2` on+pending |
| `MIGRATE` | `0x84000005` | - | `NOT_SUPPORTED` |
| `MIGRATE_INFO_TYPE` | `0x84000006` | - | `2` (no TOS) |
| `MIGRATE_INFO_UP_CPU` | `0xC4000007` | - | `NOT_SUPPORTED` |
| `SYSTEM_OFF` | `0x84000008` | - | never returns |
| `SYSTEM_RESET` | `0x84000009` | - | never returns |
| `PSCI_FEATURES` | `0x8400000A` | function ID | `0` if supported, `NOT_SUPPORTED` otherwise |

Return codes (`kvm-consts.h:107-115`): `0 SUCCESS`, `-1 NOT_SUPPORTED`,
`-2 INVALID_PARAMETERS`, `-3 DENIED`, `-4 ALREADY_ON`, `-5 ON_PENDING`,
`-6 INTERNAL_FAILURE`, `-7 NOT_PRESENT`, `-8 DISABLED`.

### 3.3 Behaviour for `-M xlt`

| Call | Behaviour |
| --- | --- |
| `VERSION` | return `0x00010001` |
| `FEATURES` | `0` for VERSION/SYSTEM_OFF/SYSTEM_RESET/CPU_ON/AFFINITY_INFO/FEATURES, `-1` otherwise |
| `SYSTEM_OFF` | flush serial, exit QEMU with status 0; must not return to the guest |
| `SYSTEM_RESET` | exit with status 0 (there is no firmware to restart); must not return |
| `CPU_ON` | single vCPU: `-4 ALREADY_ON` for the only MPIDR, `-2 INVALID_PARAMETERS` for any other MPIDR |
| `AFFINITY_INFO` | `0` (running) for MPIDR 0 at level 0, `-2` otherwise |
| `MIGRATE_INFO_TYPE` | `2` (no TOS) |
| `CPU_OFF` | exit with status 0 (single vCPU cannot be offlined) |
| unknown | `-1 NOT_SUPPORTED` |

P1 already implements `SYSTEM_OFF`/`SYSTEM_RESET` in the machine's `hvc` callback
(`xlt/qemu/patches/0002-hw-arm-add-xlt-machine.patch`); P2.2 adds the rest of the table
and moves the handler into the engine's trap dispatcher so it works from EL0 as well.

### 3.4 What the firmware and kernel call

**Unverified** (no EDK2/XNU source in this repo): from the PSCI specification and from
what a UEFI payload in a `virt`-style machine normally does, expect `VERSION`,
`MIGRATE_INFO_TYPE` (often during PSCI init), `FEATURES` probes, `AFFINITY_INFO` and
`CPU_ON` (MP core info), and `SYSTEM_RESET`/`SYSTEM_OFF` at shutdown. XNU's own
bring-up: `docs/stage27-multicore.md:17` records that the port's PSCI bring-up "may" be
buggy, and there is no XNU source in the tree, so the kernel side stays unverified until
the P2.4 boot.

---

## 4. Generic timer

### 4.1 Registers

| Register | Key `S3_op0_op1_CRn_CRm_op2` | Raw | Access | Notes |
| --- | --- | --- | --- | --- |
| `CNTFRQ_EL0` | `S3_3_3_C14_C0_0` | `0xDF00` | RW | frequency in Hz; reset 62,500,000 |
| `CNTPCT_EL0` | `S3_3_3_C14_C0_1` | `0xDF01` | RO | physical count |
| `CNTVCT_EL0` | `S3_3_3_C14_C0_2` | `0xDF02` | RO | virtual count (`CNTVOFF_EL2` = 0 here) |
| `CNTP_TVAL_EL0` | `S3_3_3_C14_C2_0` | `0xDF10` | RW | signed 32-bit delta to `CVAL` |
| `CNTP_CTL_EL0` | `S3_3_3_C14_C2_1` | `0xDF11` | RW | enable/mask/status |
| `CNTP_CVAL_EL0` | `S3_3_3_C14_C2_2` | `0xDF12` | RW | 64-bit compare value |
| `CNTV_TVAL_EL0` | `S3_3_3_C14_C3_0` | `0xDF18` | RW | virtual counterpart |
| `CNTV_CTL_EL0` | `S3_3_3_C14_C3_1` | `0xDF19` | RW | virtual counterpart |
| `CNTV_CVAL_EL0` | `S3_3_3_C14_C3_2` | `0xDF1A` | RW | virtual counterpart |
| `CNTKCTL_EL1` | `S3_3_0_C14_C1_0` | `0xC708` | RW | EL0 access enables; return 0 |

### 4.2 Frequency and the virtual clock

QEMU's ARM CPU exports a 62.5 MHz counter by default: `GTIMER_SCALE` is 16 and
`NANOSECONDS_PER_SECOND / GTIMER_SCALE = 62,500,000`
(`target/arm/cpu.c:1368-1369`, `:1456-1457`), and `CNTFRQ_EL0` resets to that value
(`target/arm/helper.c:3349`). One count is therefore exactly 16 ns.

QEMU derives the count from the virtual clock:

```c
/* target/arm/helper.c:2598-2603 */
return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / gt_cntfrq_period_ns(cpu);
```

with `gt_cntfrq_period_ns()` = `1e9 / CNTFRQ` (16 with the default). The machine model
must expose the same virtual clock it already uses for `CNTVCT_EL0` (the P1 machine
divides `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)` by 16) and the engine must implement
`CNTPCT/CNTVCT` from it, so a payload that measures time across a trap sees a monotonic
count.

### 4.3 Control/compare semantics

`CNTP_CTL_EL0`: bit 0 `ENABLE`, bit 1 `IMASK`, bit 2 `ISTATUS` (read-only). The timer's
interrupt condition is `ISTATUS == 1 && IMASK == 0`, i.e. `(CTL & 6) == 4` in QEMU's
formulation (`target/arm/helper.c:2605-2625`). `ISTATUS` is
`(count - offset) >= CVAL` (unsigned 64-bit; `offset` is 0 for the physical timer,
`CNTVOFF_EL2` for the virtual timer, which is 0 here — `helper.c:2638-2660`).

Write semantics to implement (`helper.c:2741-2805`):

- `CVAL` write: store, then recompute `ISTATUS` and the IRQ line.
- `TVAL` write: `CVAL = count + sign_extend32(value)` (the register is 32-bit signed),
  then recompute.
- `TVAL` read: `(uint32_t)(CVAL - count)`.
- `CTL` write: only bits 1:0 are writable; `ISTATUS` is computed. If `ENABLE` changed,
  recompute `ISTATUS`/IRQ; if only `IMASK` changed, just recompute the IRQ line.
- `CNTPCT`/`CNTVCT` reads return the live count; P2.3 should update `ISTATUS` when the
  counter passes `CVAL`, which for the engine means: compute a deadline
  (`(CVAL - count) * 16 ns`) and run the slice until then (or until the instruction
  budget), then raise PPI 30 if the condition holds.

### 4.4 Interrupt wiring

Architectural INTIDs (`include/hw/arm/bsa.h:23-30`):

| INTID | Source |
| --- | --- |
| 23 | virtual PMU |
| 25 | GICv3 maintenance |
| 26 | EL2 physical timer (not implemented) |
| 27 | virtual timer |
| 29 | secure EL1 physical timer (not implemented) |
| 30 | non-secure EL1 physical timer (`CNTP`) |

The physical timer PPI 30 is the one an EL1 payload uses. It is a level-sensitive PPI
owned by the redistributor: raise it while `ISTATUS && !IMASK`, and make it pending in
the SGI/PPI frame (section 5.3). `CNTP_CTL_EL0` reset value is `IMASK` set with
`ENABLE` clear, so nothing fires until the payload programs it.

### 4.5 What the payload needs

- `CNTFRQ_EL0` must read 62,500,000 and `CNTPCT_EL0`/`CNTVCT_EL0` must advance
  monotonically; delay loops in firmware spin on these.
- The firmware programs `CNTP_TVAL_EL0`/`CNTP_CTL_EL0` and expects PPI 30 to arrive after
  roughly that many counts, through the GICv3 CPU interface, then acknowledges it through
  `ICC_IAR1_EL1` and returns through `ICC_EOIR1_EL1`.
- `CNTVOFF_EL2` is not implemented and no EL2 exists, so `CNTV*` can be aliased to
  `CNTP*` (QEMU's `gt_virt_cnt_offset()` returns 0 when EL2 is disabled).
- Precision: the engine's slice boundary decides when the PPI is raised, so a 10 ms
  firmware timeout can be late by one slice. Keep the slice budget small (the P2.1
  prototype uses a bounded instruction count) and document the latency.

---

## 5. GICv3 minimum register map

### 5.1 Topology and addresses

For the `-M xlt` machine (virt-compatible aliases):

| Region | Address | Size | Notes |
| --- | --- | --- | --- |
| GICD | `0x08000000` | 64 KiB | distributor |
| GICR | `0x080A0000` | 128 KiB per redistributor | one redistributor (one vCPU) |
| GIC CPU interface | `ICC_*` system registers | - | engine sysreg model, not MMIO |
| PL011 | `0x09000000` | 4 KiB | SPI 1 → INTID 33 |
| virtio-mmio | `0x0A000000` + `i * 0x200` | 0x200 each | SPI `16 + i` → INTID `48 + i` |
| RTC | `0x09010000` | 4 KiB | SPI 2 → INTID 34 |

The spi-to-INTID mapping is QEMU's: the GIC's qdev input line `L` is INTID `32 + L`
(`hw/intc/arm_gicv3.c:324-344`), and virt wires `VIRT_UART` to line 1 and
`VIRT_MMIO + i` to line `16 + i` (`hw/arm/virt.c:862-880`, `:1085-1090`), which matches
the `interrupts = <GIC_SPI n ...>` cells in the generated device tree.

All registers below are little-endian MMIO; the model must accept 1/2/4/8-byte accesses
where the architecture allows them, and at minimum the 4-byte accesses a firmware
actually uses. With one vCPU and `DS = 1` (single security state) the model can ignore
security banking, group 0, LPIs and ITS.

### 5.2 Distributor (GICD) registers

Offsets from `include/hw/intc/gicv3_internal.h:22-58`. The bitmap registers cover SPIs
only: bits for INTID < 32 are RAZ/WI in the distributor (SGIs and PPIs live in the
redistributor frame), and `GICD_IROUTER` exists only for INTID >= 32.

| Offset | Register | Width | Access | Model notes |
| --- | --- | --- | --- | --- |
| `0x0000` | `GICD_CTLR` | 32 | RW | bits: `EnableGrp0[0]`, `EnableGrp1NS[1]`, `ARE[4]` (with `DS=1`), `DS[6]`, `RWP[31]` (always 0) |
| `0x0004` | `GICD_TYPER` | 32 | RO | see formula below |
| `0x0008` | `GICD_IIDR` | 32 | RO | constant, e.g. `0x0000043B` |
| `0x0010` | `GICD_STATUSR` | 32 | RW | RAZ/WI |
| `0x0080` | `GICD_IGROUPR<n>` | 32 | RW | bit per INTID; with `DS=1` set all implemented bits to 1 (Group 1) |
| `0x0100` | `GICD_ISENABLER<n>` | 32 | RW | write 1 sets enable |
| `0x0180` | `GICD_ICENABLER<n>` | 32 | RW | write 1 clears enable |
| `0x0200` | `GICD_ISPENDR<n>` | 32 | RW | write 1 sets pending (SPI only) |
| `0x0280` | `GICD_ICPENDR<n>` | 32 | RW | write 1 clears pending |
| `0x0300` | `GICD_ISACTIVER<n>` | 32 | RW | write 1 sets active |
| `0x0380` | `GICD_ICACTIVER<n>` | 32 | RW | write 1 clears active |
| `0x0400` | `GICD_IPRIORITYR<n>` | 8 | RW | one byte per INTID, `n = INTID` |
| `0x0C00` | `GICD_ICFGR<n>` | 32 | RW | two bits per INTID: bit 0 = 1 edge, 0 level; bit 1 RES0 |
| `0x0D00` | `GICD_IGRPMODR<n>` | 32 | RW | RAZ/WI with `DS=1` |
| `0x0E00` | `GICD_NSACR<n>` | 32 | RW | RAZ/WI with `DS=1` |
| `0x6000` | `GICD_IROUTER<n>` | 64 | RW | one per SPI, see layout below |
| `0xFFD0` | `GICD_IDREGS` | - | RO | implementation-defined ID registers |

Index arithmetic: `ISENABLER/ISPENDR/...` register `n` covers INTIDs `32n .. 32n+31`, bit
`INTID % 32`, offset `base + 4n`; `IPRIORITYR` is byte-addressed at
`0x0400 + INTID`; `ICFGR` at `0x0C00 + 4 * (INTID / 16)`; `IROUTER` at
`0x6000 + 8 * INTID` with `INTID >= 32`.

`GICD_TYPER` (QEMU `hw/intc/arm_gicv3_dist.c:380-402`): `ITLinesNumber = num_irq/32 - 1`
in bits 4:0, `CPUNumber = 0` in bits 7:5 (ARE always on), `SecurityExtn = 0` with `DS=1`
(bit 10), `IDbits = 0xF` (bits 23:19), `A3V = 1` (bit 24), `No1N = 1` (bit 25),
`DVIS = 0`. For 256 INTIDs: `0x03780007`. Pick `num_irq` from the interrupt map, not the
other way round.

`GICD_IROUTER<n>` layout: `Aff3[63:56]`, `Aff2[55:48]`, `Aff1[47:40]`, `Aff0[39:32]`,
`IRM[31]` (1 = any PE), rest RES0. QEMU stores the full 64-bit value and re-routes on
write (`hw/intc/arm_gicv3_dist.c:241-263`). For one vCPU accept `IRM=1` and
`Affinity=0`; store and ignore otherwise.

`GICD_CTLR` with `DS=1`: write `DS | ARE | EnableGrp1NS` (`0x52`) to start delivery;
`RWP` reads 0 because the model is synchronous.

### 5.3 Redistributor (GICR) frames

Frame `RD_base = 0x080A0000` for the single vCPU; frame size `0x20000` (GICv3; QEMU's
`GICR_VLPI_OFFSET` = `0x20000` only exists on GICv4). The SGI/PPI registers live in a
second 64 KiB block at `RD_base + 0x10000`, so `GICR_ISENABLER0` is at `0x10100` from
`RD_base`. Offsets from `include/hw/intc/gicv3_internal.h:75-135`:

| Offset from `RD_base` | Register | Notes |
| --- | --- | --- |
| `0x0000` | `GICR_CTLR` | bit 0 `EnableLPIs`; RAZ/WI if no LPIs |
| `0x0004` | `GICR_IIDR` | RO constant |
| `0x0008` | `GICR_TYPER` | 64-bit: `Affinity[63:32]`, `PROCNUM[23:8]`, `LAST[4]`, `DPGS[5]`, `DirectLPI[3]`, `VLPIS[1]`, `PLPIS[0]` |
| `0x0010` | `GICR_STATUSR` | RAZ/WI |
| `0x0014` | `GICR_WAKER` | `ProcessorSleep[1]`, `ChildrenAsleep[2]` |
| `0x0070` | `GICR_PROPBASER` | LPI config; RAZ/WI without LPIs |
| `0x0078` | `GICR_PENDBASER` | LPI pending; RAZ/WI without LPIs |
| `0x10080` | `GICR_IGROUPR0` | bit per SGI/PPI (INTID 0-31) |
| `0x10100` | `GICR_ISENABLER0` | write 1 sets |
| `0x10180` | `GICR_ICENABLER0` | write 1 clears |
| `0x10200` | `GICR_ISPENDR0` | write 1 sets |
| `0x10280` | `GICR_ICPENDR0` | write 1 clears |
| `0x10300` | `GICR_ISACTIVER0` | write 1 sets |
| `0x10380` | `GICR_ICACTIVER0` | write 1 clears |
| `0x10400` + `INTID` | `GICR_IPRIORITYR` | one byte per INTID 0-31 |
| `0x10C00`/`0x10C04` | `GICR_ICFGR0`/`1` | two bits per INTID; SGIs are edge, PPIs level |
| `0x10D00` | `GICR_IGRPMODR0` | RAZ/WI with `DS=1` |
| `0x10E00` | `GICR_NSACR` | RAZ/WI with `DS=1` |

`GICR_TYPER` for the only redistributor: affinity 0, `PROCNUM = 0`, `LAST = 1`,
`PLPIS = VLPIS = DirectLPI = 0` → `0x00000010`. `GICR_WAKER` resets with
`ProcessorSleep|ChildrenAsleep` (`arm_gicv3_common.c:470`); a guest writes 0 and polls
until `ChildrenAsleep` clears, so the model should clear it immediately or after a
bounded delay.

### 5.4 CPU interface (`ICC_*` system registers)

All `ICC_*` registers are `op0 = 3, op1 = 0` unless shown otherwise, and are accessible at
EL1 when `ICC_SRE_EL1.SRE == 1` (`target/arm/cpu.h`/`cpregs` equivalents; QEMU table
`hw/intc/arm_gicv3_cpuif.c:2240-2400`):

| Register | Key | Raw | Access | Fields / behaviour |
| --- | --- | --- | --- | --- |
| `ICC_PMR_EL1` | `S3_3_0_C4_C6_0` | `0xC230` | RW | priority mask `[7:0]`; interrupts with priority numerically lower (higher urgency) pass |
| `ICC_BPR1_EL1` | `S3_3_0_C12_C12_3` | `0xC663` | RW | binary point `[2:0]`, group 1 priority split |
| `ICC_CTLR_EL1` | `S3_3_0_C12_C12_4` | `0xC664` | RW | `CBPR[0]`, `EOImode[1]`, `PMHE[6]`, `PRIBITS[10:8]`, `IDBITS[15:11]`; reset `0x8C00` (A3V=`1`, IDbits=1, PRIbits=4). Model as read-only `0x8C00` for `EOImode=0` |
| `ICC_SRE_EL1` | `S3_3_0_C12_C12_5` | `0xC665` | RW | reads `0x7` (SRE, DIB, DFB); writes ignored |
| `ICC_IGRPEN0_EL1` | `S3_3_0_C12_C12_6` | `0xC666` | RW | group 0 enable; unused with `DS=1` group 1 |
| `ICC_IGRPEN1_EL1` | `S3_3_0_C12_C12_7` | `0xC667` | RW | `EnableGrp1[0]`; must be 1 for IRQs |
| `ICC_IAR0_EL1` | `S3_3_0_C12_C8_0` | `0xC640` | RO | group 0 ack; unused |
| `ICC_EOIR0_EL1` | `S3_3_0_C12_C8_1` | `0xC641` | WO | group 0 EOI; unused |
| `ICC_HPPIR0_EL1` | `S3_3_0_C12_C8_2` | `0xC642` | RO | group 0 highest pending; unused |
| `ICC_BPR0_EL1` | `S3_3_0_C12_C8_3` | `0xC643` | RW | group 0 binary point |
| `ICC_AP0R0_EL1` | `S3_3_0_C12_C8_4` | `0xC644` | RW | group 0 active priorities |
| `ICC_AP1R0_EL1` | `S3_3_0_C12_C9_0` | `0xC648` | RW | group 1 active priorities |
| `ICC_DIR_EL1` | `S3_3_0_C12_C11_1` | `0xC659` | WO | deactivate INTID (only meaningful with `EOImode=1`) |
| `ICC_RPR_EL1` | `S3_3_0_C12_C11_3` | `0xC65B` | RO | running priority |
| `ICC_SGI1R_EL1` | `S3_3_0_C12_C11_5` | `0xC65D` | WO | generate group 1 SGI, see layout |
| `ICC_ASGI1R_EL1` | `S3_3_0_C12_C11_6` | `0xC65E` | WO | SGI to the other security state; unused |
| `ICC_SGI0R_EL1` | `S3_3_0_C12_C11_7` | `0xC65F` | WO | group 0 SGI; unused |
| `ICC_IAR1_EL1` | `S3_3_0_C12_C12_0` | `0xC660` | RO | returns `INTID[23:0]`, spurious = `1023` |
| `ICC_EOIR1_EL1` | `S3_3_0_C12_C12_1` | `0xC661` | WO | write the INTID from `IAR1` |
| `ICC_HPPIR1_EL1` | `S3_3_0_C12_C12_2` | `0xC662` | RO | highest priority pending INTID |

`ICC_SGI1R_EL1` layout (QEMU `hw/intc/arm_gicv3_cpuif.c:1810-1861`): `Aff3[55:48]`,
`IRM[40]`, `Aff2[39:32]`, `INTID[27:24]`, `Aff1[23:16]`, `TargetList[15:0]`. With one
vCPU, `IRM=1` targets nobody (all PEs except self), `IRM=0` targets this CPU when
`Aff1/2/3 == 0` and bit 0 of `TargetList` is set; the SGI then becomes pending.

Priority model to implement: an interrupt is signalled while `enable` is set, group
enabled, `pending`, not active, and its priority is less than `ICC_PMR_EL1` (0 is highest).
`ICC_BPR1_EL1` splits preemption; with one vCPU and no nested interrupts it is enough to
implement "running priority" and `EOImode = 0` (EOIR1 deactivates). QEMU's
`icc_hppi_can_preempt()` (`arm_gicv3_cpuif.c:806` onwards) is the reference for the
priority comparison.

### 5.5 Interrupt state machine (minimum)

- Level-sensitive SPI/PPI (`GICD_ICFGR`/`GICR_ICFGR` bit 0 = 0): pending while the device
  line is asserted and the interrupt is not active; when the line drops and the interrupt
  is not active, pending clears.
- Edge-sensitive (SGI always, SPI if configured): a write to `ISPENDR`, a device edge or
  `ICC_SGI1R` sets pending until acknowledged.
- `IAR1` read: return the highest-priority pending INTID, clear its pending bit, set its
  active bit, set running priority; return `1023` if none.
- `EOIR1` write with `EOImode=0`: clear the active bit (priority drop is implied);
  `EOImode=1`: priority drop only, `DIR` clears the active bit.
- SPIs only from `GICD_ISPENDR`/devices; SGIs/PPIs only from `GICR_*0`/`ICC_SGI1R`.

### 5.6 Wiring summary

| Source | INTID | Trigger | GICD/GICR config |
| --- | --- | --- | --- |
| PL011 | 33 (SPI 1) | level high | `ICFGR` level, `IGROUPR` group 1, `ISENABLER` bit 1 |
| RTC | 34 (SPI 2) | level high | as above |
| virtio-mmio `i` | `48 + i` (SPI `16 + i`) | level high | as above |
| generic timer | 30 (PPI) | level | `GICR_ICFGR1` level, `GICR_ISENABLER0` bit 30 |
| SGI | 0-15 | edge | `ICC_SGI1R`/`GICR_ISPENDR0` |

Device-tree cells for these are `interrupts = <GIC_SPI n LEVEL_HIGH>` with
`n = INTID - 32` for SPIs and `GIC_PPI` with `n = INTID - 16` for PPIs; the numbers above
are the INTIDs the payload programs into the GIC, which is what the model sees.

### 5.7 Reusable QEMU code as a behavioural reference

QEMU's GICv3 cannot be instantiated in this machine (its CPU interface is built from
`ARMCPU` objects and CP-register tables, and `gicv3_init_cpuif()` loops over
`qemu_get_cpu(i)`), but three files are a precise reference for the model and the tests:

| QEMU file | What to mirror |
| --- | --- |
| `hw/intc/arm_gicv3_dist.c` | GICD read/write semantics, bitmap register indexing, `GICD_TYPER` composition, `IROUTER` routing |
| `hw/intc/arm_gicv3_redist.c` | `GICR_*` register behaviour, `WAKER` handshake, `TYPER` fields |
| `hw/intc/arm_gicv3_cpuif.c` | `ICC_*` field widths, priority comparison (`icc_hppi_can_preempt`), `IAR`/`EOIR` state transitions, `SGI1R` routing |

Reimplementing the register file is roughly 600-900 lines of C in the machine; the
engine-side `ICC_*` model is a sysreg table plus a pending/active bitmap.

### 5.8 Deliberately not modelled

LPIs, ITS, GICv4, the GICv3 security/Group 0 banking, EL2 virtual interface (`ICH_*`),
`GICD_CTLR.E1NWF`, and message-based SPIs. `DS = 1` is the single-security-state mode:
`GICD_IGROUPR`/`IGRPMODR`/`NSACR` are mostly RAZ/WI, and group 1 is the only group used.

---

## 6. Worked end-to-end trace: EL0 `svc #0x42`

Values below are the P2.2 oracle run (`/home/lshan/xlt/p2-fixtures/p2.2/NOTES.md`), with
`VBAR_EL1 = V` standing for the fixture's vector base. The fixture builds the table with
only the `0x200` and `0x400` synchronous slots populated and both handlers save
`x0-x30`, `ELR_EL1` and `SPSR_EL1` in a 256-byte frame.

### 6.1 Drop to EL0

EL1 executes `ERET` with `SPSR_EL1 = 0x00000000000003C0` (`M = EL0t`, `DAIF` masked) and
`ELR_EL1 = 0x0000000040200000` (`user_entry`). Result: `PSTATE = 0x3C0`, active SP becomes
`SP_EL0`, `PC = 0x40200000`.

### 6.2 Trap

| Step | What happens | State after |
| --- | --- | --- |
| EL0 runs `mov x0, #0x42` at `0x40200000` | - | `x0 = 0x42` |
| EL0 runs `svc #0x42` at `0x40200004` | trap to EL1, target = `V + 0x400` | - |
| entry | save `SP_EL0`, `ELR_EL1 = 0x40200008` (next instruction) | `ELR_EL1 = 0x40200008` |
| entry | `SPSR_EL1 = 0x3C0` (old PSTATE) | `SPSR_EL1 = 0x3C0` |
| entry | `PSTATE = DAIF | EL1h` | `PSTATE = 0x3C5`, active SP = `SP_EL1` |
| entry | `ESR_EL1 = (0x15 << 26) | IL | 0x42` | `ESR_EL1 = 0x56000042` |
| entry | `PC = V + 0x400` | `PC = V + 0x400` |
| handler | `MRS x1, ESR_EL1` → `0x56000042`; `EC = 0x15`, `IL = 1`, `imm16 = 0x42` | - |
| handler | `ERET` with the saved `ELR/SPSR` | `PSTATE = 0x3C0`, `PC = 0x40200008` |
| EL0 runs `svc #1` at `0x40200008` | second trap, `EC = 0x15`, `imm16 = 1`; the handler switches `ELR`/`SPSR` to return to `el1_report` | `PSTATE = 0x3C5`, `PC = el1_report` |

The two values a test should assert here are the printed `esr=0x0000000056000042`,
`elr=0x0000000040200008`, `spsr=0x00000000000003c0`.

### 6.3 Variant: lower-EL permission fault

EL0 executes `ldr x1, [x2]` with `x2 = 0x40203000`, a page mapped EL1-only in the EL1&0
translation regime:

| Item | Value |
| --- | --- |
| `ESR_EL1` | `0x9200000F` (`EC = 0x24` data abort from lower EL, `IL = 1`, `DFSC = 0xF` permission fault level 3) |
| `FAR_EL1` | `0x40203000` |
| `ELR_EL1` | faulting `ldr` (retry address) |
| Handler fix | rewrite the L3 descriptor to user-RW, `DSB ISH`, `TLBI VMALLE1`, `DSB ISH`, `ISB`, then `ERET` |
| Retried load | succeeds and returns `0xfeedfacecafebeef` |

`ELR_EL1` is the faulting instruction, not the next one: a data-abort handler that
installs a mapping and returns retries the access. This is the same rule the P2.1 oracle
uses for its translation fault (`ESR 0x96000006`, `FAR 0x70000000`).

### 6.4 Variant: illegal `ERET`

EL1 executes `ERET` with `SPSR_EL1 = 0x3C9` (`M = EL2h`, `DAIF` masked) and
`ELR_EL1 = illegal_target` (`0x40082654`). EL2 does not exist in this machine:

| Item | Value |
| --- | --- |
| PSTATE after `ERET` | `IL = 1`, still `EL1h`, `DAIF` masked |
| Next instruction | traps before executing: `ESR_EL1 = 0x3A000000` (`EC = 0x0E`, `IL = 1`, `ISS = 0`) |
| `ELR_EL1` | `0x40082654` (the instruction that was about to execute) |
| `SPSR_EL1` | `0x00000000001003C5` (`IL = 1`, `M = EL1h`) |
| Recovery | handler sets `ELR_EL1 = el1_after_illegal`, `SPSR_EL1 = 0x3C5` (IL clear), `ERET` |

The engine therefore treats "return to an unimplemented EL" exactly like QEMU's
`illegal_return` path (section 1.6), not as an unsupported-instruction error.

### 6.5 What the engine must reproduce to match the oracle

1. Vector dispatch at `0x200`/`0x400` from `VBAR_EL1`, with the saved `ELR`/`SPSR` and
   the `DAIF`-masked `EL1h` entry state.
2. `SVC` keeps `ELR` at the following instruction; aborts keep it at the faulting one.
3. `FAR_EL1` written for both instruction and data aborts, `FnV = 0`.
4. `PSTATE.IL` set by an illegal `ERET` and trapped as `EC = 0x0E` on the next
   instruction; cleared by the return that restores a valid `SPSR`.
5. `SP_EL0`/`SP_EL1` banking across every entry/`ERET` pair.
6. `DAIF` round-trip through `MSR DAIFClr/DAIFSet` (`PSTATE 0x1F`/`0x1E` in `p2.cpp`)
   and through `SPSR_EL1`.

The exact byte-for-byte expected output and sha256 are in the P2.2 oracle notes at
`/home/lshan/xlt/p2-fixtures/p2.2/NOTES.md`; the fixture
must run unchanged on the engine, and `make test` style comparison is what P2.2's exit
criterion asks for.

---

## 7. Coverage and open items

### Tables in this document

| Section | Table | Rows |
| --- | --- | --- |
| 1 | vector offsets | 16 |
| 1 | PSTATE bits | 17 |
| 1 | SPSR layout | 12 |
| 1 | SP banking rules | 3 |
| 1 | illegal-return predicates | 4 |
| 2 | ESR layout | 4 |
| 2 | exception classes | 14 |
| 2 | data-abort ISS | 13 |
| 2 | FSC codes | 12 |
| 2 | trap-to-ESR mapping | 7 |
| 3 | PSCI functions | 11 |
| 3 | PSCI return codes | 8 |
| 4 | timer registers | 10 |
| 4 | timer PPIs | 6 |
| 5 | GICD registers | 16 |
| 5 | GICR registers | 18 |
| 5 | ICC registers | 20 |
| 5 | interrupt wiring | 5 |

### Implementable now, per milestone

- P2.2: sections 1, 2 and the PSCI table (3); no GIC or timer register file needed beyond
  the sysreg traps that already exist.
- P2.3: sections 4 and 5 plus the IRQ entry at `0x280`; the PPI 30 path needs the engine
  slice deadline and the machine's GIC pending bit.

### Unverified

- EDK2's exact PSCI call sequence and whether it uses `FEATURES`/`AFFINITY_INFO` at all;
  no EDK2 source is vendored and the local AAVMF image is compressed.
- XNU's CPU bring-up: `docs/stage27-multicore.md:17` only says the port's PSCI bring-up
  "may" be buggy; no kernel source in the repo.
- Whether the payload's abort handler ever relies on `ISV`/`SAS`/`SRT` (section 2.4).
- `ICC_CTLR_EL1.EOImode` choice: this document recommends read-only `EOImode = 0`; if a
  payload expects `EOImode = 1` and uses `ICC_DIR_EL1`, the model needs the extra state.
  Unverified until P2.3 runs the payload.
- The ARM ARM revision behind the field names in sections 1-2; the encodings were
  cross-checked against QEMU 8.2.2, which is the executable reference for this port.
- Whether CNTFRQ should stay 62.5 MHz on a `-M xlt` machine that wants a different
  frequency: the P1 machine already reports 62,500,000, and virt does the same, so this
  is a constant until a payload complains.

## 8. References

- [PLAN.md](PLAN.md) - P2 plan (path A, milestones, exit criteria, risks).
- [INTEGRATION.md](../INTEGRATION.md) - accelerator design and hook IDs.
- `xlt/qemu/p1/NOTES.md` - P1 machine and sysreg subset that P2.2 extends.
- QEMU 8.2.2: `target/arm/syndrome.h`, `target/arm/internals.h`,
  `target/arm/helper.c`, `target/arm/tcg/helper-a64.c`,
  `target/arm/tcg/translate-a64.c`, `target/arm/tcg/tlb_helper.c`,
  `target/arm/tcg/psci.c`, `include/hw/arm/bsa.h`,
  `include/hw/intc/gicv3_internal.h`, `hw/intc/arm_gicv3_{dist,redist,cpuif}.c`,
  `hw/arm/virt.c`.
- TCG oracles: `/home/lshan/xlt/p2-fixtures/NOTES.md` (P2.1),
  `/home/lshan/xlt/p2-fixtures/p2.2/NOTES.md` (P2.2).

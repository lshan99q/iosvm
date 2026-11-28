# P2 plan — growing the custom `-M xlt` machine to payload boot

Status: plan, written for t14. No source or patch is changed by this task; this file is the
deliverable.

Decision: **path A** — keep growing the custom, CPU-less `-M xlt` machine (its own ARM system
model, MMU, exceptions, timers, GICv3, pflash, virtio-blk) until it can boot the project
payload. `-M virt -accel xlt` is not the target. The decision is fixed by the t15 spike
(`/home/lshan/xlt/p2-spike/NOTES.md`, verdict C); this plan does not re-open it and stages the
work that follows from it.

Evidence base: QEMU 8.2.2 sources read at `/home/lshan/xlt/qemu-8.2.2` (upstream v8.2.2,
commit `03e36c72c2c44f9afb116858d5e7c92be66a32b1`); repo paths and lines refer to that tree.
Items marked **unverified** have not been checked against sources or measured.

## 0. Why path A: the t15 spike result

The spike asked whether the standard `virt` machine plus an ordinary `cortex-a76` ARMCPU could
run with `-accel xlt` instead of TCG, using bounded patches. Two attempts were made in a spike
tree outside the repository:

| Attempt | Changes | Result |
| --- | --- | --- |
| 1 | accept `virt*` in `xlt_init_machine`; accept `xlt` in virt's GIC gate; dummy `create_vcpu_thread` that allocates `halt_cond`/`thread`, signals `cpu_thread_signal_created` | machine and `cortex-a76` realize, then QEMU idles; `rc=124`, empty stdout; the same build with `-accel tcg` prints `xlt-p1-fixture1` and exits 0 |
| 1a (before the dummy thread) | only patches 1+2 | crash in `qemu_cpu_kick()` from the preconfig resume path (`cpu->halt_cond` NULL), `rc=139` |
| 2 | add an empty `xlt-accel-arm` AccelCPUClass | identical to attempt 1: `rc=124`, empty stdout |

Source pointers for the mechanics (all verified in the tree):

- `arm_cpu_realizefn()` target/arm/cpu.c:1789, the `!tcg_enabled() && !qtest_enabled()` feature
  gates at :1833, `cpu_exec_realizefn()` at :1890; `arm_tcg_ops` and
  `cc->tcg_ops = &arm_tcg_ops` under `CONFIG_TCG` at target/arm/cpu.c:2458, :2511-2513.
- `qemu_init_vcpu()` system/cpus.c:630, the mandatory-ops assert at :648, the
  `create_vcpu_thread` call at :649 and the `while (!cpu->created)` wait at :651-654.
- Optional AccelCPUClass lookup `accel/accel-target.c:94-100`: QEMU 8.2.2 has no ARM
  AccelCPUClass at all (ARM TCG and KVM do not use one), and its absence is tolerated.
- virt's accelerator gate: hw/arm/virt.c:1912 `finalize_gic_version()`, `else` at :1948
  (`Unsupported accelerator, can not determine GIC support`).
- There is no other hard TCG gate in hw/arm/virt.c; the machine constructs successfully.

The spike's conclusion, which this plan adopts: those patches can make QEMU *accept* the
combination, but nothing executes; the accelerator's only execution hook is
`create_vcpu_thread`, and an empty thread satisfies `qemu_init_vcpu()` and then does nothing.
Making it execute would mean bridging `CPUARMState` and libxlt (registers, PSTATE/DAIF, SP
banks, exceptions, interrupts, PSCI, MMIO), which is the hybrid design the spike calls C — a
material accel/CPU integration with its own QEMU-internal couplings (the code-dirty
`assert(tcg_enabled())` at system/physmem.c:2590-2593, the cpreg exports, and GICv3's CPU
interface being registered on `ARMCPU`, hw/intc/arm_gicv3_cpuif.c:2799-2840). Bounded patches
therefore do not deliver the M2 target.

Path A keeps everything the P1 machine already owns (RAM window, PL011, ELF loader, host
callbacks, PSCI) and grows the missing pieces in our own code, with no dependency on QEMU's ARM
CPU/accelerator coupling. Its cost is honest: we re-implement a small ARM system model
(system registers, MMU, exceptions, generic timer) and two devices (GICv3, virtio-mmio wiring,
pflash wiring, a minimal FDT) inside `hw/arm/xltm.c` and the engine.

## 1. What P2 starts from

| Piece | State at the end of P1 |
| --- | --- |
| Engine | In-tree C++ engine (decoder, lifter, LLVM ORC JIT, dispatcher), 10.08x TCG on the benchmark set; integer/load-store/branch/MRS/MSR/HLT/SVC/HVC only; no MMU, no exceptions, no FP/SIMD, no exclusives |
| Accel | `xlt` accelerator and `xlt-accel-ops` (`xlt-accel` / `xlt-accel-ops` QOM types); the accelerator owns guest startup (`setup_post` → machine `xlt_machine_start_guest`); `-M xlt` without `-accel xlt` fails with `-M xlt requires -accel xlt` |
| Machine | 256 MiB RAM at `0x40000000`, PL011 at `0x09000000`, ELF loader, host callbacks, PSCI SYSTEM_OFF/RESET, no CPU objects (one device thread) |
| Proved | fixtures and the smoke image produce byte-identical serial under `-accel xlt` and `-accel tcg`; `-M xlt` alone is rejected cleanly |
| Missing | MMU, exceptions, EL0/EL1, VBAR, ERET, generic timer, GICv3, interrupts, FP/SIMD, exclusives/LSE, cache maintenance, pflash, virtio-blk, FDT |

The P1 machine's address map was chosen to match virt (`0x40000000` RAM, `0x09000000` PL011),
which is a useful property: a fixture written for P2 can also be run on `-M virt -accel tcg` to
produce the reference serial output.

## 2. Shape of path A

### 2.1 ARM system model moves into the machine/engine

Under A there is no `ARMCPU` and no `CPUARMState`, so the machine must own the architectural
state that the engine and the devices share. The model is deliberately narrow — the payload
runs at EL1 with EL0 available and no EL2/EL3:

| State | Owner | Notes |
| --- | --- | --- |
| x0-x30, SP, PC | engine `CpuRegs` | slice-local, checkpointed into the machine's `ArmSysRegs` at traps |
| NZCV, DAIF, SPSel, current EL | `ArmSysRegs.pstate` | packed in the existing `CpuRegs.nzcv` layout for NZCV; DAIF/EL/SPSel set by traps and exceptions |
| SP banks | `ArmSysRegs.sp_el[0..1]` | `aarch64_save_sp`/`restore_sp` semantics (EL0/EL1 banks), mirroring target/arm/internals.h:220-237 |
| SCTLR_EL1, TTBR0/1_EL1, TCR_EL1, MAIR_EL1, CONTEXTIDR_EL1 | `ArmSysRegs.mmu` | drives the engine's page-table walk |
| VBAR_EL1, ESR/FAR/ELR/SPSR_EL1 | `ArmSysRegs.exc` | exception entry/return |
| CNTFRQ/CNTPCT/CNTVCT/CNTP_*/CNTV_* | `ArmSysRegs.timer` | backed by `QEMU_CLOCK_VIRTUAL` |
| ICC_* (GICv3 CPU interface) | machine GIC model | see §2.3 |
| MPIDR, MIDR, CTR, DCZID, ID registers | machine constants | values taken from the cortex-a76 model (target/arm/tcg/cpu64.c:349) |

The engine's existing sysreg callbacks (`read_sysreg`/`write_sysreg` in `HostOps`) already route
MRS/MSR to the machine; P2 grows that dispatch from P1's five-register switch to the table
above. The machine, not QEMU's target/arm, is the source of truth — no `CPUARMState` sync, no
`arm_rebuild_hflags`, no `tcg_ops`.

### 2.2 MMU and traps (P2.1)

The engine gains a stage-1 EL1-only page-table walk (4 KiB granule; 2 MiB blocks allowed):
TTBR0/TTBR1 selection by VA range and TCR, table walk through `ArmSysRegs.mmu`, MAIR
attributes (normal write-back for RAM, device for MMIO), AP/UXN/PXN permission checks,
AF update, and a small software TLB keyed by VA page plus a generation counter.

Faults leave the engine through the same trap record the P1 fatal path used:

- data abort: `{vaddress, is_write, size, WnR, FnV, CM, S1PTW}` → `ESR_EL1` with EC=0x25,
- prefetch abort: `{vaddress}` → `ESR_EL1` with EC=0x21,
- the dispatcher fills `ArmSysRegs.exc` (ESR_EL1, FAR_EL1, ELR_EL1, SPSR_EL1), sets
  PSTATE to EL1h with DAIF masked, loads PC from `VBAR_EL1 + offset` and resumes.

The decoder must also accept `eret` (P2.2), `tlbi` (flush the engine TLB), `dc cvau`/`ic ivau`
(flush translated code for the range), `dc zva` (zero a 64-byte block), `msr spsel/daif`, and
`isb/dsb/dmb` (already present as no-ops).

Foreign-accurate fault VPN/FSC encoding is specified from the ARM ARM and cross-checked against
the TCG reference run of the same fixture on `-M virt` (exp's P2.1 baseline, t17), not
hand-computed and assumed.

### 2.3 GICv3 and timers (P2.3)

The machine grows its own GICv3 model, because QEMU's `arm-gicv3` cannot be used without CPUs
(`gicv3_init_cpuif()` walks `qemu_get_cpu(i)` and calls `define_arm_cp_regs()` on each,
hw/intc/arm_gicv3_cpuif.c:2799-2840). Scope for P2:

- distributor at `0x08000000` (64 KiB): CTLR (ARE_NS, EnableGrp1NS), TYPER, IIDR, IGROUPR,
  ISENABLER/ICENABLER, ISPENDR/ICPENDR, ISACTIVER, IPRIORITYR, ICFGR, IROUTER;
- one redistributor at `0x080A0000` (SGI frame + RD frame): CTLR, TYPER, IIDR, WAKER,
  IGROUPR0, ISENABLER0, IPRIORITYR0, ICFGR0/1, ISACTIVER0;
- CPU interface sysregs, reached through the engine's sysreg traps: ICC_SRE_EL1, ICC_CTLR_EL1,
  ICC_PMR_EL1, ICC_BPR1_EL1, ICC_IGRPEN1_EL1, ICC_IAR1_EL1, ICC_EOIR1_EL1, ICC_DIR_EL1,
  ICC_HPPIR1_EL1, ICC_AP0R/AP1R, ICC_SGI1R_EL1;
- lines: SPI 33 (PL011 UART0, same number virt uses), PPI 30 (`ARCH_TIMER_NS_EL1_IRQ`), SGI 0-15
  for software interrupts; group 1 only, no priorities beyond PMR/BPR modelling, no security
  states, no ITS, no LPIs.
- interrupt delivery: the accel loop polls pending IRQs after each slice, picks the highest
  priority group-1 interrupt, sets IAR semantics (pending→active), and enters the vector; EOI
  writes deactivate.

Generic timer (P2.3): CNTFRQ_EL0 = 62,500,000 (already P1's value), CNTPCT/CNTVCT from
`QEMU_CLOCK_VIRTUAL` divided by 16, CNTP_CTL/CNTP_TVAL/CNTP_CVAL and the virtual counterparts,
compare→PPI 30. `CNTVOFF_EL2` is out of scope (no EL2).

### 2.4 Machine devices for the payload (P2.4)

The payload boot line uses pflash (EDK2), an ESP FAT disk (virtio-blk), a raw payload loaded at
`0xF0000000`, and a device tree. Path A grows each of these in `hw/arm/xltm.c`:

- **pflash**: two `TYPE_PFLASH_CFI01` devices with virt's properties (sector 64 KiB, width 4,
  device-width 2, id bytes `89 18 00 00`, `name=pflash0/1`, drive aliases `pflash0`/`pflash1`),
  mapped at `0x0` and `0x04000000` (64 MiB each) as in hw/arm/virt.c:1121-1186. This is copied
  from virt and is small.
- **virtio-mmio + virtio-blk**: create `virtio-mmio` transports (the same
  `sysbus_create_simple("virtio-mmio", base, irq)` pattern as hw/arm/virt.c:1052-1087) at a
  `0x0a000000`-style window so `-device virtio-blk-device,drive=esp0` attaches unchanged.
- **FDT**: EDK2 is started from pflash at `0x0` and expects a device tree in `x0`. The machine
  builds a minimal flattened DT with libfdt: RAM, `/cpus/cpu@0`, GICv3 (`arm,gic-v3` dist +
  gicr), `arm,armv8-timer`, PL011, two `cfi-flash` nodes, `virtio_mmio` nodes, `psci` with
  `method = "hvc"`, and `/chosen`. Whether XNU's boot path consumes this DT directly or relies
  on the payload loader's own tables is an open question (§6).
- **RAM size**: the P1 256 MiB window becomes `-m`-driven (12 GiB for parity), because the
  payload address `0xF0000000` must be inside RAM.
- **loader**: the generic `-device loader,file=payload.bin,addr=0xF0000000,force-raw=on` works
  with any machine; no machine code needed.

Machine reset state: PC = 0 (pflash), x0 = FDT address, PSTATE = EL1h with DAIF masked, and a
flat identity map is *not* pre-enabled — EDK2 sets up its own MMU.

### 2.5 Engine coverage the payload will demand

The decoder order follows the boot, not a wish list:

1. FP/SIMD: `ldr/str q`, `ldp/stp q`, `mov/fmov/movi`, `add/sub/mul/fdiv`, conversions,
   `tbl/tbx` (EDK2 and XNU startup, libc memcpy/strlen),
2. exclusives `ldxr/stxr/ldaxr/stlxr` with a single-vCPU monitor (LDXR arms, STXR succeeds
   unless a store to the same 16-byte granule or an exception intervened),
3. LSE atomics (`ldadd/ldclr/ldset/ldeOR`, `swp`, `cas`) because the a76 model advertises
   ATOMICS (`id_aa64isar1` bit 20 at target/arm/tcg/cpu64.c:349),
4. system instructions: `eret`, `tlbi`, `dc cvau`, `ic ivau`, `dc zva`, `msr spsel/daif`,
5. `wfi/wfe` semantics: end the slice, wait for an interrupt (the standalone CLI keeps the P1
   halt behaviour behind a host flag).

## 3. Milestones

Reference runs: every fixture that does not use a P2-only device is also run under
`-M virt -accel tcg` (same PL011/RAM base) to produce expected serial output; exp's P2.1
baseline (t17) supplies that reference for the MMU fixture, and code's scratch prototype (t18)
is the first engine implementation.

| Milestone | Goal | Fixture / command | Exit criteria |
| --- | --- | --- | --- |
| P2.1 | EL1 MMU, page-table walk, data/prefetch aborts, fault recovery | `p2-mmu.elf` | serial byte-identical to the TCG reference; handler prints ESR/FAR/ELR; retry after mapping succeeds; XN fetch faults; no `xlt_bad_mem` exit |
| P2.2 | Exceptions, EL0/EL1, VBAR, ERET, PSCI | `p2-exc.elf` | SVC from EL0 handled (EC=0x15); illegal eret sets PSTATE.IL; DAIF masking honoured; PSCI SYSTEM_OFF/RESET work |
| P2.3 | Generic timer + GICv3 + PL011 RX interrupt | `p2-irq.elf` + serial socket | PL011 IRQ handled and EOI'd; timer PPI fires; counts and order match TCG; interrupt latency measured |
| P2.4 | pflash + virtio-blk + FDT + payload boot attempt | EDK2 pair, esp disk, loader, 12 GiB | boot reaches at least `== XNULOADER v3 ==`, or a precise documented blocker (PC/ESR/insn/device) |
| P2.5 | Boot parity to SpringBoard | payload, 12 GiB, single vCPU | 5/5 markers, screenshot parity, image md5 unchanged, boot ≤ TCG baseline, no panic |

Cross-cutting work that lands before or across milestones:

- machine-side `ArmSysRegs` table + engine sync (P2.1),
- trap record and exception dispatcher (P2.1),
- `dcache`/epoch moved out of `LibXlt::run()`'s function-local static (P2.1; every slice
  currently re-initialises the dispatch cache, xlt/src/libxlt.cpp),
- IRQ polling at slice boundary and kick handling (P2.3),
- invalidation hooks for guest stores, `ic ivau`, TLBI and device DMA (P2.3/P2.4).

### P2.1 — MMU and faults

Fixture `p2-mmu.elf` (bare metal, EL1, UART-only):

1. build TTBR0_EL1 tables (identity map of RAM with 2 MiB blocks, MAIR normal/device),
2. enable SCTLR_EL1.M,
3. store to an unmapped VA → data abort: print ESR_EL1/FAR_EL1/ELR_EL1 and decode EC/FSC/WnR,
4. handler maps the page, `eret` back, store succeeds,
5. jump to a page marked UXN → prefetch abort, same reporting path,
6. PSCI SYSTEM_OFF.

Exit criteria: serial byte-identical to the TCG reference on `-M virt`; ESR/FAR/ELR values
themselves are compared, not just "it printed"; engine TLB invalidation after mapping is
observed (step 4 only passes if the handler's flush works).

Stop rule: if the engine's walk disagrees with the TCG fixture on any level/attribute case,
stop and simplify to a flush-all TLB and 4 KiB-only mapping until the fixture is green; do not
special-case the fixture.

### P2.2 — exceptions and ELs

Fixture `p2-exc.elf`: install VBAR_EL1; drop to EL0 via ERET; `svc #0x42`; the EL1 handler
prints ESR EC=0x15 and ISS, reads SPSR/ELR, returns with ERET; then an illegal ERET (SPSR to
EL2) must set PSTATE.IL and return to the instruction after the ERET; then PSCI SYSTEM_OFF.

Work: ERET, vector-offset rules (same-EL with SP0/SPx, lower-EL A64), PSTATE save/restore,
SPSel bank switch, DAIF set on entry and restore on return, SVC/HVC/BRK/UNDEF syndrome
encoding.

Stop rule: any register/state dump mismatch (PSTATE, SPSR, SP bank, ELR) → stop; the exception
model is the foundation for booting anything.

### P2.3 — timers and GICv3

Fixture `p2-irq.elf`: program CNTP_TVAL and enable PPI 30; initialise the GICv3 model
(ICC_SRE_EL1, ICC_PMR_EL1, ICC_IGRPEN1_EL1, GICD/GICR enables); enable SPI 33 for the UART;
the harness writes a byte to the serial socket; handler reads IAR, prints ID, EOIs, counts three
interrupts, powers off.

Exit criteria: serial matches the TCG reference (same interrupt order/IDs on virt); timer and
UART interrupts each delivered; no lost EOI; measured worst-case interrupt latency recorded
with the slice budget.

Stop rule: if the GIC model cannot deliver an interrupt within 5 ms of the device raising it,
stop and instrument the model; never disable interrupts to make a test pass.

### P2.4 — payload devices and first boot

Work, in this order: pflash pair with virt-compatible drive aliases; virtio-mmio transport and
virtio-blk attach; minimal FDT + `x0` at reset; `-m`-driven RAM; `-device loader` payload at
`0xF0000000`; EDK2 boot from pflash. Then triage: first exception, first missing instruction,
first unhandled sysreg, first unsupported device access.

Exit criteria: serial reaches `== XNULOADER v3 ==` or further; otherwise the blocker is
documented with PC, instruction encoding, ESR, and the missing engine/machine feature.

Stop rule: three consecutive boot attempts failing on different ISA/device classes → stop and
re-scope the engine's ISA coverage; do not grow the decoder one instruction per attempt.

### P2.5 — parity

Exit criteria in §5. Stop rule: markers pass but the frame differs → fix correctness before any
performance work.

## 4. Risk register

| ID | Risk | Unknown / trigger | Stop rule |
| --- | --- | --- | --- |
| R1 | Engine MMU walk | TTBR0/TTBR1 selection, TCR walks, block/page levels, AF/AP/UXN/PXN, MAIR attributes | Any fixture divergence from the TCG reference → simplify (flush-all TLB, 4 KiB pages) and re-land features one at a time |
| R2 | GICv3 model | register semantics EDK2/XNU depend on (CTLR/ARE, WAKER, IAR/EOIR, groups/priorities, SGI) | An interrupt that the TCG run delivers but xlt loses, twice → stop and diff the GIC register trace against the TCG reference |
| R3 | FDT / boot ABI | EDK2 needs the DT in `x0`; XNU's bootloader may need more (chosen/initrd/memory reservations) | EDK2 rejects the DT or stops before banner → stop and compare the DT against the virt DT for the same devices |
| R4 | ISA coverage | FP/SIMD, exclusives, LSE, cache maintenance, system ops | Two consecutive "unsupported encoding" stops on new classes → re-scope before continuing |
| R5 | DMA/code invalidation | virtio writes guest RAM; guest may modify code; pflash vars writes | Any stale-code fault → stop and implement the page bitmap + device-write hook rather than guessing |
| R6 | Timer fidelity | 62.5 MHz virtual clock, TVAL/CVAL/CTL semantics, compare crossing | Boot time or delay loops diverge from TCG → stop and compare timer register reads against TCG |
| R7 | EL/PSTATE model | SP banking, DAIF, SPSR/ELR, illegal ERET | Any state-dump mismatch in P2.2 → stop |
| R8 | Single vCPU | delivery is `-smp 1`; payload boot args unchanged | If boot needs more than one vCPU, stop: that is P4, not P2 |
| R9 | Device wiring | pflash aliases, virtio-mmio bus order, IRQ numbers | EDK2 does not find the disk/flash → stop and compare the QEMU device tree and `-device` ordering against the TCG boot line |
| R10 | Measurement | TCG baselines are load-sensitive (handover reports require load) | Any fps/boot comparison without recorded load is invalid; re-run |

Out of scope for P2: EL2/EL3, SVE/PAuth/BTI (the a76 model we mirror advertises none), PMU
counters beyond reads, gdbstub, migration, icount, SMP.

## 5. Acceptance metrics

Baselines (TCG, single vCPU, from the repo):

| Metric | Baseline | Source |
| --- | --- | --- |
| Boot to SpringBoard | ≈8 min | docs/gpu-acceleration-stage13-plan.md:29 |
| Frame rate | 13.9 fps / 75 ms median at load 5.5-8; 11.7 fps / 87 ms at load 10-15.5 | docs/handover.md:9, :13, :16 |
| CPU per frame | 51 ms ≈ 19.6 fps ceiling at `fb_hz=15` | docs/handover.md:12 |
| Serial markers | `== XNULOADER v3 ==`, `Darwin Kernel Version`, `vm_page_bootstrap`, `MAC Framework successfully`, `initproc failed to start` | tools/qemu-open.sh:166-174 |
| Image md5 | root `7c7da1a5e4dc5cb6044e4e813f001ae3`, kernel `c153053e3b6e374ae9c5f05c6ca1f508`, system `6be494f16775402cab1fa2120ca24112`, lib `40463969cfb9cb7be14a20ded6042a05` | docs/handover.md:21-23 |
| Screen | captured PNG byte-identical (sha256) to the TCG capture | docs/handover.md |

Consequence of path A: the M2 line is no longer "the TCG command line with only `-accel`
changed". P2 parity is defined as **same payload, same RAM size, same devices, same guest
visible behaviour**, with `-M xlt -accel xlt` in place of `-M virt,gic-version=3 -accel tcg`.
The guest boot arguments (including `cpus=4`/`fb_beat=0`, docs/state-of-the-port.md) stay
byte-identical, and the host stays single-vCPU.

P2.5 gates:

1. five markers, same order as TCG,
2. screenshot sha256 equal to the TCG capture (`tools/screendump.sh` and the frame comparison),
3. image md5 list unchanged,
4. boot wall time ≤ the TCG baseline, same host load (record `loadavg`; median of 3 boots),
5. zero panics, zero QEMU assertions, `-smp 1`.

FPS is reported at P2.5 but gated at P3: P2's job is correctness and boot parity, and the frame
rate is capped by `fb_hz=15`.

## 6. Test harness and open questions

Fixtures live under `xlt/tests/fixtures/p2/` with a `build.sh` (clang aarch64, freestanding, no
libc), mirroring P1. For each fixture the runner captures serial output under
`-M xlt -accel xlt` and under `-M virt -accel tcg` and diffs them. The runner is opt-in; `make
test-open` must never require a built QEMU.

Open questions (unverified):

1. Whether XNU consumes the FDT through EDK2/loader unchanged, or whether the payload path
   needs additional `/chosen` properties (boot args, memory reservations). Compare against the
   TCG boot's DT.
2. Whether the payload's loader expects the payload at `0xF0000000` there rather than in a
   device node; the TCG line uses `-device loader,addr=0xF0000000`, so path A keeps it.
3. Exact GICv3 register set EDK2 touches at the version it ships (vendor builds differ);
   instrument the TCG run's GIC accesses before freezing the model.
4. Whether pflash var writes (`vars.fd`) need write-protection or a separate read-only region
   at P2.4 (virt maps flash0 into the secure address space only; path A has one address space).
5. Boot-time performance of the engine's page-table walk and trap rate; measure in P2.4 and
   apply the design-doc P3 targets afterwards.

## 7. File layout delta

```
xlt/src/libxlt.h              HostOps: + fault/trap record, + machine sysreg set, wfi_sleeps
xlt/src/libxlt.cpp            slice exit reasons, trap record, dcache/epoch in Impl
xlt/src/lift.cpp              MMU-aware load/store/fetch, trap returns, exclusives/LSE, FP/SIMD
xlt/src/xlt_insn.h            + eret, tlbi, dc/ic, dc zva, msr immediate, exclusives/LSE, FP/SIMD
xlt/src/armsys.{h,cpp}        ArmSysRegs, PSTATE/SP banking, MMU walk (new)
xlt/qemu/patches/0001..0003   accel: single-machine model, slice loop, IRQ polling, trap dispatch
xlt/qemu/p2/                  PLAN.md (this file), fixtures runner, NOTES.md
xlt/tests/fixtures/p2/        p2-mmu, p2-exc, p2-irq + build.sh
```

The QEMU-side surface stays in `accel/xlt/` (class/ops, slice loop, trap dispatch) and
`hw/arm/xltm.c` (machine, ARM system model, GICv3, timer, pflash, virtio-mmio, FDT). No QEMU
core file is modified unless R5 forces the physmem invalidation hook (system/physmem.c:
2590-2593, the code-dirty `assert(tcg_enabled())`), which would be a small tracked patch.

## 8. References

- `/home/lshan/xlt/p2-spike/NOTES.md` — t15 spike verdict C, both attempts and source pointers.
- `xlt/qemu/README.md`, `xlt/qemu/INTEGRATION.md` — accel design and hook inventory H1-H16.
- `xlt/qemu/p1/NOTES.md` — P1 machine, fixtures and known limits.
- `xlt/qemu/patches/` — current patch series.
- `docs/xlt-translator.md` — M1 engine, benchmarks, P0-P5 roadmap.
- `docs/adr/ADR-001-qemu-fork.md` — fork/core-change policy.
- `reports/xlt-m2-p1-report.md` — P1 evidence.
- `tools/qemu-open.sh` — TCG boot line and serial markers.

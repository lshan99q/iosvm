# Integrating xlt with QEMU: phases, hooks and acceptance

This is the execution plan behind [README.md](README.md). It answers three questions: what the
accelerator has to achieve in each phase, which QEMU internals it has to hook, and how each phase
is judged. Everything about QEMU 8.2.2 was verified against the upstream sources of the target
release (commit `11aa0b1ff115b86160c4d37e7c37e6a6b13b77ea`), as were the repository baselines used
for acceptance; statements that were not checked are labelled *(unverified)*. M1 is the standalone
engine (`xlt/README.md`); M2 is this accelerator. P1 — the accelerator skeleton — is implemented
and verified; the status section below records what that means, and
[`p2/PLAN.md`](p2/PLAN.md) carries the plan for the next stage.

## Status: P1 delivered and verified

P1 is **implemented and independently verified** at the working-tree state on top of `9267e0f`
(`feat(xlt): add in-tree AArch64-to-x86_64 translator`); the commit that lands it is a separate
step. The verification report is [`../../reports/xlt-m2-p1-report.md`](../../reports/xlt-m2-p1-report.md)
(PASS, including the t16 re-verification; the committed patch files were then whitespace-normalized with unchanged code, see the report addendum).

| artifact | what |
| --- | --- |
| [`patches/0001-accel-add-xlt-accelerator.patch`](patches/) | 5,197 B, sha256 `bdfeb19f…`: `xlt-accel` / `xlt-accel-ops`, `CONFIG_XLT` wiring, accelerator ownership of guest startup through `AccelClass::setup_post` |
| [`patches/0002-hw-arm-add-xlt-machine.patch`](patches/) | 6,927 B, sha256 `5efbec75…`: `hw/arm/xltm.c`, the CPU-less `-M xlt` machine |
| [`build-qemu.sh`](build-qemu.sh), [`build-xlt.sh`](build-xlt.sh) | pristine QEMU 8.2.2 tree; patched tree with `libxlt.a` and `qemu-system-aarch64` (92 s, 1,971/1,971 targets, 0 warnings on the 32-thread host) |
| [`p1/run.sh`](p1/run.sh) | parity, determinism and failure-path suite (`run.sh: PASS`, ≈1 s) |
| `xlt/tests/fixtures/p1/`, `xlt/tests/fixtures/qemu-smoke/` | fixtures and the unpatched-tree baseline image |
| [`p1/NOTES.md`](p1/NOTES.md) | P1 limits, host-emulated registers, evidence |

Verified behaviour: both fixtures and the qemu-smoke image produce byte-identical serial output
between `-M virt -accel tcg -cpu cortex-a76` and `-M xlt -accel xlt -cpu cortex-a76` (exit 0 through
PSCI), twice per side (deterministic); `-M xlt` without `-accel xlt` exits 1 with
`-M xlt requires -accel xlt`; the M1 engine is unchanged (481 decode checks, 5/5 benchmark
checksums, overall 9.68–10.02× across the two verification runs).

Deviations accepted for P1: a custom `-M xlt` machine instead of reusing `-M virt`; no ARM
CPU/`CPUState`, therefore no MMU/page tables/exceptions/interrupts/GIC/SMMU and one guest vCPU;
only four host-emulated system registers (`CurrentEL`, `CNTFRQ_EL0`, `CNTVCT_EL0`, `MPIDR_EL1`)
with writes ignored; `hvc #0` as the PSCI conduit with `SYSTEM_OFF`/`SYSTEM_RESET` only; PL011 as
the only device with synchronous MMIO through `address_space_rw()` under the BQL; no pflash, DMA or
self-modifying-code invalidation; the semihosting hook is a no-op. These are scope choices, not
verification failures; the report lists them in its §7.

The P2 phase text below was written before the t15 spike. The spike's verdict (path A, recorded
with its evidence in [`p2/PLAN.md`](p2/PLAN.md) §0) fixed the direction: grow the custom `-M xlt`
machine instead of making `-M virt -accel xlt` execute. Where the text below assumes a `virt`
machine or a `CPUState`, read it as the historical design input; the P2 rows and the threading
section are annotated accordingly.

## What boots today, and what xlt has to run

The TCG path the project measures is one command (`tools/qemu-open.sh:145-155`):

```
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 12G \
  -accel tcg,tb-size=384 \
  -drive if=pflash,...,file=efi.fd -drive if=pflash,...,file=vars.fd -net none \
  -drive file=fat:rw:$WORK/esp,if=none,id=esp0 -device virtio-blk-device,drive=esp0 \
  -device loader,file=payload.bin,addr=0xF0000000,force-raw=on \
  -display none -serial stdio -monitor none -no-reboot
```

So there are two guest programs, not one:

1. **Firmware.** EDK2 executes from the two pflash regions at guest physical `0x0` (64 MiB each,
   `hw/arm/virt.c` maps `VIRT_FLASH` at 0), reads the FAT ESP over `virtio-blk-device`, and jumps
   to `BOOTAA64.EFI` (the project's loader) in RAM.
2. **Payload.** `xnuloader` is loaded by the loader device at `0xF0000000` (3.75 GiB, inside the
   RAM window), then jumps into the XNU image that `mkpayload.py` assembled. Everything from that
   point on is the OS the project already measures.

`-accel xlt` replaces only the translation of these instructions. On the rejected path B, boot
essentials — pflash, the virtio transport, the GIC, the UART, RAM, the loader device — would have
stayed exactly as under TCG, which is what makes parity measurable. Path A keeps the same
guest-visible map (RAM at `0x40000000`, PL011 at `0x09000000`, GICv3 dist/redist at
`0x08000000`/`0x080A0000`, pflash at `0x0`/`0x04000000`, virtio-mmio at `0x0a000000`), but those
devices are modelled by the machine, so parity means same payload, RAM size, devices and guest
behaviour rather than the same QEMU machine code. Note two consequences: code executes from
read-only flash as well as from RAM (P2 must fetch instruction bytes from non-RAM regions), and the
delivery configuration is one vCPU (`QEMU_SMP=1`), because multi-core XNU panics in the scheduler
lock (`docs/stage25-multicore.md`, section 12) — so P2 acceptance is measured on one vCPU.

## Phase plan

| Phase | Scope | Exit criterion | How it is judged |
| --- | --- | --- | --- |
| P0 (M1 scope) | Standalone engine: ELF loader, decoder, lifter, JIT, block cache, CLI | `xlt run <elf>` executes a test ELF and exits with the guest's exit code | `xlt/tests/run_tests.sh` |
| P1 | `-accel xlt` skeleton: QOM types, vCPU thread, dispatcher, RAM fast path, MMIO exit, sysreg read, semihosting, PSCI shutdown | **Done.** A bare-metal payload prints over the PL011 UART and powers the machine off through PSCI; no translation of firmware yet | [`p1/run.sh`](p1/run.sh), plus the M1 tests |
| P2 | Grow the custom `-M xlt` machine (path A, [`p2/PLAN.md`](p2/PLAN.md)): EL1-only MMU and faults, exceptions/ELs, machine-owned GICv3 and generic timer, pflash/virtio-blk/FDT, flash instruction fetch, device-write invalidation, ISA coverage | Payload brought up by the TCG boot line reaches the five boot markers and a SpringBoard frame under `-M xlt -accel xlt`, with the same image checksums and screenshot; parity is defined as same payload/RAM/devices/behaviour, since the machine name differs | Boot wall time and serial markers vs TCG; `md5` of the built images; screenshot sha256 parity; `p2/PLAN.md` §5 |
| P3 | Performance and parity hardening: cache warm path, hot-block chaining, N host threads (gated), profiling | Guest CPU time per frame below TCG's; no behavioural divergence in the gate scripts | `glwire` fps at matched load, CPU/frame, `tools/` gate scripts |

P1 was deliberately "until the first MMIO" plus semihosting: it proved the ABI, the dispatcher and
the register bridge without needing page tables, exceptions or the firmware. It is delivered; P2 is
the first phase whose success number is meaningful, because it is the first full boot, and its work
is staged in [`p2/PLAN.md`](p2/PLAN.md).

## Required hooks

"Existing" means xlt calls a public function and the QEMU tree needs no change. "New export" means
a symbol becomes visible outside its current translation unit without behaviour change. "Core
change" means a real edit and is called out separately, because [ADR-001](../../docs/adr/ADR-001-qemu-fork.md)
requires core changes to stay at zero; the M2 patch series must justify each one.

| # | Hook | Where (v8.2.2) | Type |
| --- | --- | --- | --- |
| H1 | Accelerator QOM types `xlt-accel`, `xlt-accel-ops`; `init_machine`; `create_vcpu_thread` | `accel/accel-target.c:44` (`accel_find`), `accel/accel-system.c:65` (`accel_init_ops_interfaces`), `system/cpus.c:648` | New files `accel/xlt/`, `accel/meson.build`, `accel/Kconfig` |
| H2 | vCPU thread loop: RCU/thread registration, BQL, `cpu->can_do_io`, `cpu_thread_signal_created`, kick handling | `include/sysemu/cpus.h:23,26,29,30`; `system/cpus.c:244` (`generic_handle_interrupt`); `accel/tcg/tcg-accel-ops-rr.c` as the model | Existing API, copied from TCG's rr loop |
| H3 | "CPU is executing" bracket around guest code, needed so `cpu_exit`/exclusive sections can stop the guest | `include/hw/core/cpu.h:1000-1015`, implemented in `cpu-common.c:246,286`; KVM's use `accel/kvm/kvm-all.c:2832,2991` | Existing API |
| H4 | Guest RAM identity window | `hw/arm/virt.c:2263` (RAM attached at `0x40000000`, `VIRT_MEM` base at `:167`); `include/hw/boards.h:379` (`MachineState::ram`); `include/exec/memory.h:1988` (`memory_region_get_ram_ptr`) | Existing API |
| H5 | Translation-fault invalidation on device/DMA writes to RAM | `system/physmem.c:2576` (`invalidate_and_set_dirty`), `:2590-2593` (`assert(tcg_enabled()); tb_invalidate_phys_range(...)`) | **Core change** (see below) |
| H6 | System-register table lookup and read | `target/arm/cpregs.h:983` (`get_arm_cp_reginfo`), `:1048` (`read_raw_cp_reg`), `:167` (`ENCODE_AA64_CP_REG`) | Existing API |
| H7 | System-register writes | `target/arm/helper.c:77` (`write_raw_cp_reg` is static) | New export (one wrapper) |
| H8 | System-register permission checks (`AccessFlag`/`SCTLR` traps) | `target/arm/tcg/op_helper.c` (`access_check_cp_reg`) | New export, P2 only |
| H9 | Synchronous exception delivery | `target/arm/cpu.h:1176` (`arm_cpu_do_interrupt`), `target/arm/cpu.h:37-51` (`EXCP_*`), `env->exception` fields | Existing API; BQL required |
| H10 | ESR/FSC encoding for page faults | `target/arm/tcg/tlb_helper.c:163` (`arm_deliver_fault`), `:73` (`compute_fsr_fsc`, static) | New export or move next to `arm_cpu_do_interrupt` |
| H11 | Interrupt masking test | `target/arm/cpu.c:656` (`arm_excp_unmasked`, static inline) | New export, P2; P1 uses a local DAIF check |
| H12 | Interrupt target EL | `target/arm/helper.c:10315` (`arm_phys_excp_target_el`; `:10212` is the user-mode stub) | Existing API |
| H13 | PSCI calls (`SMC`/`HVC`) | `target/arm/internals.h:319-327` (`arm_is_psci_call`, `arm_handle_psci_call`) | Existing API, but see risk R3 |
| H14 | Semihosting | `include/semihosting/common-semi.h` (`do_common_semihosting`), trigger decoded by xlt (`HLT #0xf000`, `target/arm/tcg/translate-a64.c:2401`) | Existing API; do not go through `arm_cpu_do_interrupt` |
| H15 | MMIO and non-RAM accesses | `include/exec/memory.h:2642` (`address_space_rw`), `system/physmem.c:2869` | Existing API |
| H16 | Machine memory region for the identity window and for flash decode cache | `hw/arm/virt.c` has no accel-specific code; xlt reads `ms->ram` in `init_machine` | No change to `hw/arm/virt.c` |

### What P1 actually used

| hook | P1 state |
| --- | --- |
| H1 | Implemented: the `xlt-accel` / `xlt-accel-ops` pair, `init_machine` requires the `xlt` machine, `create_vcpu_thread` is a no-op |
| H2 | Partial: one detached RCU/BQL thread instead of TCG's rr loop — there is no `CPUState`, so no `cpu_thread_signal_created`, `can_do_io` or kick path |
| H3 | Not used: no `CPUState` to bracket. P2 (path A) stays CPU-less, so this is not on the P2 path either |
| H4 | Not used yet: the P1 machine creates its own 256 MiB region; growing it to `-m`-driven RAM is a P2 item (`p2/PLAN.md` §2.4) |
| H5 | Not needed in P1: fixtures never enable code-dirty logging; P2 adds the hook only if device DMA writes code |
| H6, H7 | Not used: `MRS`/`MSR` go through the machine's four-register host callback table |
| H8–H12 | Not used: no exceptions, no interrupts, no permission model |
| H13 | Partial: `hvc #0` is handled by the machine callback (`SYSTEM_OFF`/`SYSTEM_RESET`), not `arm_handle_psci_call()` |
| H14 | Not used: the semihosting callback is a no-op |
| H15 | Implemented: `address_space_rw()` under the BQL |
| H16 | Implemented in the P1 form: the machine owns its RAM region (no `hw/arm/virt.c` change) |

Three files a TCG-shaped design would expect to change are deliberately untouched:

- `accel/tcg/cpu-exec.c` — TCG's basic-block loop is not reused. xlt's dispatcher replaces it,
  called from H2's thread function. There is nothing to hook: `tcg_enabled()` is false under
  `-accel xlt` (`include/sysemu/tcg.h:15` defines it as `tcg_allowed`, which is only set in
  `tcg_init_machine()`, `accel/tcg/tcg-all.c:112`), so no TCG code path runs.
- `accel/tcg/translate-all.c` — its translation-block cache, `tb_flush()` and
  `cpu_io_recompile()` exist for TCG's TBs; xlt has its own block table and its own
  return-to-dispatcher path. Nothing is shared. What is worth borrowing is the *policy* (invalidate
  by physical page). P3 may revisit `cpu_io_recompile()`-style re-execution for MMIO inside a
  block, but the M2 ABI already exits at the MMIO instruction.
- `system/physmem.c` — `address_space_rw()` is used as is. But see H5: the write path has a TCG
  assumption that has to change for P2.

### H5 in detail: the write path assumes TCG

This finding changed the first draft of the design. QEMU's RAM write path calls
`invalidate_and_set_dirty()` for every write (`system/physmem.c:2576`). If code-dirty logging is
enabled on the region, it executes:

```c
if (dirty_log_mask & (1 << DIRTY_MEMORY_CODE)) {
    assert(tcg_enabled());
    tb_invalidate_phys_range(addr, addr + length - 1);
    dirty_log_mask &= ~(1 << DIRTY_MEMORY_CODE);
}
```

Under `-accel xlt`, `tcg_enabled()` is false, so a guest that enables code-dirty logging would
abort the process. Two consequences:

- xlt must **not** call `memory_region_set_log(mr, true, DIRTY_MEMORY_CODE)` on the RAM region
  (`include/exec/memory.h:2032`) while relying on this path.
- P2 needs a replacement. The smallest one is a new `AccelOpsClass::invalidate_phys_range(start,
  end)` hook: TCG registers `tb_invalidate_phys_range`, xlt registers its block invalidation, and
  `invalidate_and_set_dirty()` calls it instead of testing `tcg_enabled()`. That is a ~10-line core
  change plus one TCG registration; it also fixes the same assumption in
  `cpu_physical_memory_test_and_clear_dirty()` (`system/physmem.c:884`), which calls
  `tlb_reset_dirty_range_all()` under `tcg_enabled()`.

P1 does not need H5: its payloads are loaded before any block is translated and never modify code.
Guest stores to executable pages are still detected (see the self-modifying-code section of
[README.md](README.md)); what P1 cannot see is a device writing code bytes behind xlt's back.

H5 is the only hook in this table that edits shared QEMU code, which collides with
[ADR-001](../../docs/adr/ADR-001-qemu-fork.md)'s "core changes at zero" rule. The plan's position
is that the rule needs a narrow, written exception for it (or the edit must live in the fork as a
patch kept on top of upstream and re-based every release); it is not worth claiming zero core
changes when one is plainly needed, and hiding it would only move the surprise to review time.

## Threading and the dispatcher

P1 runs one detached host thread that owns guest execution: it registers with RCU, takes the BQL,
and loops over the guest one slice at a time, dropping the BQL only around translated code. The
accelerator starts it from `AccelClass::setup_post`; the machine never starts a guest. Because the
custom machine has no `CPUState`, none of TCG's per-CPU plumbing applies — no
`cpu_thread_signal_created`, no kick, no `can_do_io`. P2 keeps that model (path A in
[`p2/PLAN.md`](p2/PLAN.md)): interrupts come from the machine's own GICv3 model and are polled at
slice boundaries, not injected through `cpu_interrupt()`.

The snippet below is the TCG-shaped loop this design would use if it ever owned a real `CPUState`
(and the reason path B was rejected is that making that state work is the larger integration):

```
qemu_mutex_unlock_iothread();
cpu_exec_start(cpu);                 /* H3: guest code is about to run */
exit = xlt_run_slice(vcpu, budget);  /* translated code, returns via the ABI */
cpu_exec_end(cpu);
qemu_mutex_lock_iothread();
xlt_handle_exit(vcpu, exit);         /* MMIO, sysreg, exception, IRQ, ... */
```

`cpu_exec_start()` is what tells the rest of QEMU that this CPU can be interrupted with
`cpu_exit()` and that an exclusive section has to wait (`include/hw/core/cpu.h:1000-1030`), and
`cpu_exec_end()` is what makes `qemu_cpu_kick()` effective. TCG calls the pair around
`tcg_cpus_exec()` (`accel/tcg/tcg-accel-ops.c:75-77`); KVM does the same around `ioctl(KVM_RUN)`
(`accel/kvm/kvm-all.c:2832,2991`). Skipping them is safe only while no `CPUState` exists, which is
P1/P2's situation.

N host threads remain a P3 option (`-accel xlt,thread=multi`): the block table, the software TLB
and the persistent cache would all need explicit locking, and the guest that would benefit is the
multi-core XNU configuration the project has already judged unsafe (`docs/stage25-multicore.md`).
The design keeps the hook so this is an added backend, not a rewrite.

## Acceptance metrics

All numbers below are the project's own TCG baselines; "same load" means the host load average is
recorded with the measurement (`docs/handover.md` requires it).

| Metric | TCG baseline | P2 acceptance | P3 goal |
| --- | --- | --- | --- |
| Boot to SpringBoard (wall clock, 1 vCPU) | ≈8 min (`docs/gpu-acceleration-stage13-plan.md:29` records this as an accepted cost) | ≤ baseline median of 3 runs; no regression on the five serial markers | ≤ 4 min *(unverified projection)* |
| `glwire` steady-state fps | 13.9 fps / 75 ms at load 5.5–8; 11.7 fps / 87 ms at load 10–15.5 (`docs/handover.md:9,16`) | ≥ 13.9 fps at the same load, 0 panics | guest CPU time per frame below TCG's 51 ms at `fb_hz=15` (`docs/handover.md:12`) |
| Image parity | md5 root `7c7da1a5…`, kernel `c153053e…`, system `6be494f1…`, lib `40463969…` (`docs/handover.md:21-23`) | identical | identical |
| Screen parity | screenshot sha256 equal to the TCG baseline | equal (byte-identical) | equal |
| Serial boot markers | 5 of 5 (`tools/qemu-open.sh:166-174`) | 5 of 5 | 5 of 5 |

The fps number alone is a weak acceptance criterion, because the delivery configuration is
frame-paced (`fb_hz=15`, CPU/frame 51 ms ≈ 19.6 fps ceiling): a faster engine cannot show more than
the pacer allows. P2 therefore reports boot wall time and CPU/frame as the primary numbers, and fps
as a guard against regressions.

A P1 exit is simpler but strict: `xlt/tests/run_tests.sh` gains a test that boots a test payload
under `-accel xlt` and compares the UART log, the semihosting exit code and the QEMU exit status
against expectations. The payload exercises: PL011 output, one `HLT #0xf000` call, `CNTPCT_EL0`
reads (twice, to check the counter advances), a deliberate unaligned access, and `PSCI SYSTEM_OFF`
so the process exits 0.

## Risks and mitigations

| id | Risk | Why it matters | Mitigation / state |
| --- | --- | --- | --- |
| R1 | `assert(tcg_enabled())` in the write path | Aborts QEMU the moment code-dirty logging is on (H5) | P1 avoids code-dirty logging; P2 adds the accel hook (H5) |
| R2 | The write path invalidates TCG TBs only | Stale translated blocks after device writes | Same hook as R1; guest stores are handled by xlt itself |
| R3 | PSCI and semihosting are gated on TCG or `#ifdef CONFIG_TCG` | `arm_cpu_do_interrupt()` calls `arm_handle_psci_call()` only when `tcg_enabled()` (`target/arm/helper.c:11351`); `arm_is_psci_call`/`arm_handle_psci_call` are stubs outside TCG builds (`target/arm/internals.h:319-327`); semihosting (`helper.c:11362`) is under `#ifdef CONFIG_TCG` | xlt calls `arm_handle_psci_call()` itself in its `SMC`/`HVC` handler and `do_common_semihosting()` itself, so the accel does not depend on either gate; a `--disable-tcg` build needs a local PSCI handler (P3, unverified effort) |
| R4 | Instruction fetch from pflash/ROM | The identity-window fast path covers RAM only; firmware executes from flash at `0x0` | Fetch bytes via `address_space_read()` into a per-page decode cache for non-RAM regions; those blocks are immutable and never invalidated. Cost is boot-only *(unverified: measured flash byte volume)* |
| R5 | MMIO trap rate | Every UART byte, virtio descriptor and GIC access leaves translated code; a block can contain many | Exit at the access (no speculative pre-execution), so correctness is trivial; P3 may add per-page MMIO fast paths for the GIC distributor *(unverified benefit)* |
| R6 | GICv3 CPU-interface registers | `ICC_*` are system registers owned by the GIC model, not the CPU | Path A: the engine traps `ICC_*` and calls the machine's GICv3 model, so there are no cpregs to reach. On the rejected path B this would have needed `gicv3_cpuif_reginfo[]` (`hw/intc/arm_gicv3_cpuif.c:2240`, registered into `cpu->cp_regs` at `:2829`) and `arm_cpu_set_irq()` (`target/arm/cpu.c:916`) |
| R7 | SMP memory ordering / multi-core XNU | The guest already panics in the scheduler lock with more than one CPU | Delivery is one vCPU; multi-thread accel is P3 and gated on the guest bug, not on xlt |
| R8 | Self-modifying code and TLBI | XNU and SpringBoard patch code and flush caches; stale blocks corrupt execution | Guest-store detection, cache-op exits and TLBI-driven TLB flush in P1/P2; device writes need H5 |
| R9 | Unaligned and exclusive/atomic accesses | AArch64 allows unaligned normal-memory accesses; `LDXR/STXR` need monitor semantics | Alignment-safe loads/stores plus split at page and device boundaries; at first, exclusive sequences exit to QEMU (slow, correct) *(unverified: how often XNU uses exclusives in hot paths)* |
| R10 | Translation-cache staleness/corruption | A wrong cache hit is a silent miscompile | Cache key = image hash + ABI hash; per-block guest-byte hash re-checked on load; checksummed header and atomic rename (see README) |
| R11 | gdbstub, record/replay, live migration, `-icount` | These expect TCG semantics or accel hooks | Out of scope for M2; `-accel xlt` documents them as unsupported, and QEMU's generic checks still apply (`icount_enabled() && !tcg_enabled()` already errors, `system/vl.c:2409`) |
| R12 | Not building under `--disable-tcg` | Some hooks are TCG-only (R3), and `tcg_ops` is NULL | M2 targets a TCG-enabled build (`--enable-tcg` default); a TCG-free build is a P3 todo |
| R13 | `qemu_ram_ptr_length()` cannot be called | Older notes named it; it is static to `system/physmem.c:2185` | Use `memory_region_get_ram_ptr()` / `RAMBlock::host` (H4) |

## Test matrix

| Level | Test | Runs where |
| --- | --- | --- |
| Unit | Decoder/lifter/JIT tests as they land in `xlt/tests/run_tests.sh` | host, `make -C xlt test` |
| P1 integration | `qemu-system-aarch64 -M xlt -accel xlt` + bare-metal images (PL011, RAM, CurrentEL, PSCI off), TCG vs xlt byte comparison, determinism and four failure paths | host, [`p1/run.sh`](p1/run.sh) (opt-in; needs a QEMU tree with the patches) |
| P1 negative | Missing `-kernel`, unknown accelerator, `-M xlt` without `-accel xlt`, `-accel xlt` on `-M virt`: each must fail with a diagnostic, exit 1, no hang | host, [`p1/run.sh`](p1/run.sh) |
| P2 boot | The TCG boot line's payload/devices under `-M xlt -accel xlt`; five markers + screenshot + image md5 | host, manual until the machine work lands ([`p2/PLAN.md`](p2/PLAN.md) §5) |
| P3 parity | `md5` of images, screenshot sha256, fps at matched load, CPU/frame | host, `tools/` gate scripts |

The M2 tests cannot run in CI until a QEMU tree with the xlt patches exists; the plan keeps
`make test-open` independent of building QEMU (ADR-001), so the P1/QEMU tests are opt-in and skip
with a clear message when no patched binary is available (`QEMU=` overrides the default path).

## Patch series order

The P1 series landed as steps 1–4 in a CPU-less form (no `CPUState`, no exception delivery); the
plan below keeps the original order so a reader can map hooks to commits. Steps 5–7 are P2/P3 work
restated in [`p2/PLAN.md`](p2/PLAN.md). Each step is independently reviewable and keeps the TCG path
byte-for-byte unchanged:

1. `accel/xlt/` skeleton: QOM types, Kconfig/meson entries, dispatcher thread. `-accel xlt`
   selects it and `-accel tcg` is untouched. **Landed (P1).** (H1, H2)
2. Dispatcher + register bridge + exit ABI; PSCI `SYSTEM_OFF`/`SYSTEM_RESET` through the machine
   callback. **Landed (P1).** (H13)
3. Machine RAM region, MMIO through `address_space_rw()`, host-emulated sysreg reads.
   **Landed (P1).** (H15, H16)
4. Semihosting hook and the ELF loader. **Landed as a no-op hook (P1).** (H14)
5. Page tables, faults and ESR encoding. **P2** (H8, H10, H11, H12)
6. Device-write invalidation and cache maintenance. **P2** (H5, R8)
7. Interrupts/GIC and timers, then performance work and the optional multi-thread backend.
   **P2/P3** (no ABI changes)

## Unverified items

- `MemTxAttrs` fields a machine is expected to pass for guest accesses, and whether secure
  versus non-secure EL2/EL3 state needs more than `MEMTXATTRS_UNSPECIFIED` (P2 dependency).
- Whether EDK2 on the TCG boot executes from pflash in place, and how many flash bytes it executes
  (affects the fetch-cache cost in `p2/PLAN.md` §2.4/§4 only).
- The P1/P2 performance projections (boot ≤4 min, MMIO fast-path benefit); only TCG baselines are
  measured.
- P1's hook coverage is the "What P1 actually used" table above: H1, H2 (partial), H13 (partial),
  H15 and H16 are implemented and tested; the remaining hooks are P2 design. H5, H7, H8, H10 and
  H11 remain design-only, and no QEMU tree is vendored.
- Exclusive/atomic instruction frequency and the cost of exiting on every exclusive sequence.
- Behaviour of the P2 machine's GICv3 model against devices other than PL011 and virtio-blk.

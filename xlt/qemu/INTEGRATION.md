# Integrating xlt with QEMU: phases, hooks and acceptance

This is the execution plan behind [README.md](README.md). It answers three questions: what the
accelerator has to achieve in each phase, which QEMU internals it has to hook, and how each phase
is judged. Everything about QEMU 8.2.2 was verified against the upstream sources of the target
release (commit `11aa0b1ff115b86160c4d37e7c37e6a6b13b77ea`), as were the repository baselines used
for acceptance; statements that were not checked are labelled *(unverified)*. Nothing here is
implemented yet: M1 is the standalone engine (`xlt/README.md`), M2 is this accelerator.

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

`-accel xlt` replaces only the translation of these instructions. Boot essentials — pflash, the
virtio transport, the GIC, the UART, RAM, the loader device — stay exactly as they are under TCG,
which is the property that makes parity measurable. Note two consequences: code executes from
read-only flash as well as from RAM (P2 must fetch instruction bytes from non-RAM regions), and the
delivery configuration is one vCPU (`QEMU_SMP=1`), because multi-core XNU panics in the scheduler
lock (`docs/stage25-multicore.md`, section 12) — so P2 acceptance is measured on one vCPU.

## Phase plan

| Phase | Scope | Exit criterion | How it is judged |
| --- | --- | --- | --- |
| P0 (M1 scope) | Standalone engine: ELF loader, decoder, lifter, JIT, block cache, CLI | `xlt run <elf>` executes a test ELF and exits with the guest's exit code | `xlt/tests/run_tests.sh` |
| P1 | `-accel xlt` skeleton: QOM types, vCPU thread, dispatcher, RAM fast path, MMIO exit, sysreg read, semihosting, PSCI shutdown | A bare-metal payload prints over the PL011 UART and powers the machine off through PSCI; no translation of firmware yet | Joins `xlt/tests/run_tests.sh` as an integration test that launches `qemu-system-aarch64 -accel xlt` and diffs the serial log |
| P2 | Page tables and exceptions (software TLB, data/prefetch aborts), flash instruction fetch, device-write invalidation, cache/TLB maintenance, interrupts | Same command line as TCG (only `-accel` differs) reaches the five boot markers in `tools/qemu-open.sh:166-174` and gets a SpringBoard frame, with the same image checksums | Boot wall time and serial markers vs TCG; `md5` of the built images; screenshot sha256 parity |
| P3 | Performance and parity hardening: cache warm path, hot-block chaining, N host threads (gated), profiling | Guest CPU time per frame below TCG's; no behavioural divergence in the gate scripts | `glwire` fps at matched load, CPU/frame, `tools/` gate scripts |

P1 is deliberately "until the first MMIO" plus semihosting: it proves the ABI, the dispatcher and
the register bridge in days, without needing page tables, exceptions or the firmware. P2 is the
first phase whose success number is meaningful, because it is the first full boot.

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

P1/P2 use one host thread for all vCPUs, as TCG's rr mode does. The thread function from H2 does
the RCU/thread registration and the BQL discipline, and calls, per CPU slice:

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
(`accel/kvm/kvm-all.c:2832,2991`). Skipping them compiles and even boots, but stop/reset/monitor
commands would race with translated code.

N host threads remain a P3 option (`-accel xlt,thread=multi`): the block table, the software TLB
and the persistent cache would all need explicit locking, and the guest that would benefit is the
multi-core XNU configuration the project has already judged unsafe (`docs/stage25-multicore.md`).
The design keeps the hook (`cpu_thread_is_idle`, `kick_vcpu_thread`) so this is an added backend,
not a rewrite.

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
| R6 | GICv3 CPU-interface registers | `ICC_*` are system registers owned by the GIC model, not the CPU | Verified: `hw/intc/arm_gicv3_cpuif.c:2240` defines `gicv3_cpuif_reginfo[]` and registers it into `cpu->cp_regs` at `:2829`, so H6/H7 reach them; IRQ lines come through `arm_cpu_set_irq()` (`target/arm/cpu.c:916`) unchanged |
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
| P1 integration | `qemu-system-aarch64 -accel xlt` + bare-metal payload (UART, HLT, CNTPCT, fault, PSCI off) | host, added to `xlt/tests/run_tests.sh` once a qemu tree with the patch exists |
| P1 negative | Payload with an unassigned MMIO access must produce a data abort, not a hang | host |
| P2 boot | Same command as `tools/qemu-open.sh` with `-accel xlt`; five markers + screenshot | host, manual until the guest work lands |
| P3 parity | `md5` of images, screenshot sha256, fps at matched load, CPU/frame | host, `tools/` gate scripts |

The M2 tests cannot run in CI until a QEMU tree with the xlt patch exists; the plan keeps
`make test-open` independent of building QEMU (ADR-001), so the P1/QEMU tests are opt-in and skip
with a clear message when `QEMU_XLT` is not set.

## Patch series order

Each step is independently reviewable and keeps the TCG path byte-for-byte unchanged:

1. `accel/xlt/` skeleton: QOM types, Kconfig/meson entries, dispatcher thread that stops with
   "not implemented"; `-accel xlt` selects it and `-accel tcg` is untouched. (H1, H2)
2. Dispatcher + register bridge + exit ABI; a slice runs an empty payload (no blocks) and PSCI
   `SYSTEM_OFF` works. (H3, H9, H13)
3. RAM identity window, MMIO through `address_space_rw()`, sysreg reads. (H4, H6, H15)
4. Semihosting and the sysreg write export. (H7, H14)
5. Page tables, faults and ESR encoding. (H8, H10, H11, H12)
6. Device-write invalidation and cache maintenance. (H5, R8)
7. Performance work and the optional multi-thread backend: no ABI changes. (P3)

## Unverified items

- `MemTxAttrs` fields an accelerator is expected to pass for guest accesses, and whether secure
  versus non-secure EL2/EL3 state needs more than `MEMTXATTRS_UNSPECIFIED` (P2 dependency).
- Whether EDK2 on `virt` executes from pflash in place, and how many flash bytes it executes
  (affects R4's cost only).
- The P1/P2 performance projections (boot ≤4 min, MMIO fast-path benefit); only TCG baselines are
  measured.
- The small patches (H5, H7, H8, H10, H11) are described, not written; no QEMU tree is vendored,
  so none of them has been compiled.
- Exclusive/atomic instruction frequency and the cost of exiting on every exclusive sequence.
- Behaviour of `-accel xlt` with `-M virt` variants other than `gic-version=3` and `cortex-a76`.

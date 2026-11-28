# The xlt accelerator: design for QEMU 8.2.2

`xlt/README.md` describes the M1 engine: a standalone program that loads an AArch64 ELF and runs
it on this x86-64 host. This directory describes the next step, M2: putting the same engine behind
a QEMU accelerator so `-accel xlt` replaces TCG's translator while QEMU keeps its machine model,
devices and CPU state. P1 is implemented as a small text patch series in
[`patches/`](patches/); no modified QEMU tree is vendored (see
[ADR-001](../../docs/adr/ADR-001-qemu-fork.md)).

## Building the unpatched tree

[`build-qemu.sh`](build-qemu.sh) fetches QEMU 8.2.2, verifies the tarball sha256, and builds
`qemu-system-aarch64` out of tree under `/home/lshan/xlt` (override with `QEMU_WORK_DIR`); nothing
is written inside the repository. The P1 baseline image that proves the unpatched tree works is the
[`qemu-smoke` fixture](../tests/fixtures/qemu-smoke/README.md): it must print `xlt-p1-ok` over the
PL011 UART and exit through PSCI before any accelerator work starts.

## Quick start (P1)

P1 is the first executable milestone: an `xlt` accelerator and a CPU-less `-M xlt` machine that run
bare-metal AArch64 images with byte-for-byte TCG parity on the P1 fixtures. These commands are the
whole path from a clean checkout to `run.sh: PASS`; the expected output follows the commands. Host
dependencies: QEMU's usual build tools (gcc/g++, ninja, GNU patch, rsync), `llvm-config-18`,
capstone 4, and about 2 GB free under `/home/lshan/xlt` (override with `QEMU_WORK_DIR`).

```sh
# 1. Pristine QEMU 8.2.2 tree and the unpatched baseline image (also run automatically
#    by build-xlt.sh when the tree is missing). Verifies the tarball sha256.
xlt/qemu/build-qemu.sh

# 2. Patched tree: copies the pristine source, applies xlt/qemu/patches/*.patch in order,
#    builds the PIC libxlt.a, configures and builds qemu-system-aarch64 only.
#    On the 32-thread build host this takes about 92 s (1,971/1,971 targets, 0 warnings).
xlt/qemu/build-xlt.sh

# 3. The binary and the two registration checks
QEMU=${QEMU_WORK_DIR:-/home/lshan/xlt}/qemu-8.2.2-xlt-build/qemu-system-aarch64
$QEMU -M help | grep xlt          # xlt   xlt AArch64-to-x86-64 translator (P1)
$QEMU -accel help                 # xlt

# 4. Build the fixtures and run one under the accelerator, plus its TCG reference
xlt/tests/fixtures/p1/build.sh
$QEMU -M xlt -cpu cortex-a76 -smp 1 -m 256M -accel xlt \
      -kernel xlt/build/fixtures/p1/fixture1.elf \
      -display none -serial stdio -monitor none -no-reboot
$QEMU -M virt -cpu cortex-a76 -smp 1 -m 256M -accel tcg \
      -kernel xlt/build/fixtures/p1/fixture1.elf \
      -display none -serial stdio -monitor none -no-reboot

# 5. Reproducible parity suite: rebuilds the fixtures, runs each image twice per side,
#    compares both within-side runs and TCG vs xlt byte-for-byte, checks the failure
#    paths, prints "run.sh: PASS". Override the binary with QEMU=/path/to/qemu-system-aarch64.
xlt/qemu/p1/run.sh
```

Expected serial output (exit status 0 in both cases, through PSCI `SYSTEM_OFF`):

```text
xlt-p1-fixture1
xlt-p1-fixture2 fr=0x0000000000000090 ram=0x1122334455667788 el=0x0000000000000004
```

The `qemu-smoke` image from t9 prints `xlt-p1-ok` with the same flags. The failure paths are part of
the contract and are checked by `run.sh`:

```text
$QEMU -M xlt … -kernel fixture1.elf            # -M xlt requires -accel xlt            (exit 1)
$QEMU -M xlt -accel xlt …                      # xlt: -kernel <elf> is required        (exit 1)
$QEMU -M xlt -accel nope …                     # invalid accelerator nope              (exit 1)
$QEMU -M virt -accel xlt …                     # -accel xlt is only supported by -M xlt  (exit 1)
```

Measured on this host: parity suite ≈1 s, accelerator fixture run 0.05–0.09 s (TCG 0.05 s — the
micro-fixtures pay JIT startup), `make -C xlt` 25 s. The two patches are plain text files applied
with `patch -p1` (also `git apply`-able), so they can be checked against the untouched upstream tree;
no modified QEMU tree is vendored (see [ADR-001](../../docs/adr/ADR-001-qemu-fork.md)).

Under the hood, step 2 is exactly this (from the repository root; the upstream tree is never
modified — the patched copy lives beside it):

```sh
root=$PWD
QEMU_WORK_DIR=${QEMU_WORK_DIR:-/home/lshan/xlt}
src=$QEMU_WORK_DIR/qemu-8.2.2 patched=$QEMU_WORK_DIR/qemu-8.2.2-xlt
build=$QEMU_WORK_DIR/qemu-8.2.2-xlt-build lib=$root/xlt/build/qemu

rm -rf "$patched" && mkdir -p "$patched" && rsync -a "$src/" "$patched/"
for p in xlt/qemu/patches/*.patch; do
  patch -p1 -d "$patched" --batch --forward -i "$root/$p"
done

# PIC archive that QEMU links against
mkdir -p "$lib/obj"
for f in decode jit libxlt libxlt_c lift; do
  g++ -O2 -g -std=c++17 -fPIC -I"$root/xlt/src" $(llvm-config-18 --cxxflags) \
      -c "$root/xlt/src/$f.cpp" -o "$lib/obj/$f.o"
done
ar rcs "$lib/libxlt.a" "$lib"/obj/*.o

mkdir -p "$build" && cd "$build"
"$patched/configure" --target-list=aarch64-softmmu --disable-docs --disable-werror \
  --disable-gtk --disable-sdl --disable-vnc --disable-opengl --disable-spice \
  --disable-libssh --disable-guest-agent \
  --extra-cflags="-I$root/xlt/src -I/usr/lib/llvm-18/include" \
  --extra-ldflags="-L$lib -lxlt -lLLVM-18 -lcapstone -lpthread -ldl -lz -ltinfo -lstdc++ -lm \
  -Wl,-rpath,/usr/lib/llvm-18/lib"
ninja -j16 qemu-system-aarch64
```

P1 is deliberately small: no `CPUState`, no ARM CPU/MMU model, no interrupts or GIC, no pflash, no
semihosting beyond the no-op hook; `hvc #0` is the PSCI conduit and only `SYSTEM_OFF`/`SYSTEM_RESET`
are implemented. The exact limits, the host-emulated system registers and the evidence table are in
[`p1/NOTES.md`](p1/NOTES.md); the plan for the next machine generation is
[`p2/PLAN.md`](p2/PLAN.md).

Every statement below was checked against the QEMU v8.2.2 sources
(commit `11aa0b1ff115b86160c4d37e7c37e6a6b13b77ea`), the release this plan targets and the one
installed on the development host (`docs/stage48-h-a-crosscheck-v4.md:38`). Statements marked
*(unverified)* were not confirmed in that tree. Two paths in older design notes are wrong for 8.2.2
and are corrected here:

- 8.2.2 renamed `softmmu/` to `system/`; the file is `system/physmem.c`.
- `qemu_ram_ptr_length()` is static to `system/physmem.c`, so an accelerator cannot call it. The
  public way to get the RAM host pointer is described under "Guest memory and MMIO" below.

## What the accelerator replaces

TCG is two things glued together: a translator (`accel/tcg/translate-all.c`, `tcg/`) that turns
guest basic blocks into host code and caches them by guest PC, and an execution loop
(`accel/tcg/cpu-exec.c`) that runs those blocks and re-enters QEMU for every trap. xlt takes over
the first role only. The CPU model, `CPUARMState`, the GIC, the UART, the loader and the machine
keep working unchanged, because xlt reads and writes the same `env` TCG would.

Consequently an xlt "block" is called by xlt's own dispatcher, not by `cpu_exec()`, and the
contract between a block and the dispatcher is the piece this document pins down. The M1
`BlockFn` already carries the register state:

```c++
/* xlt/src/xlt.h */
struct CpuRegs;
using BlockFn = uint64_t (*)(CpuRegs *regs);
```

## Invocation and coexistence with TCG

The accelerator is selected exactly like any other QEMU accelerator, so both work side by side in
one binary:

```
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 12G \
                    -accel xlt -device loader,file=payload.bin,addr=0x40000000,force-raw=on \
                    -display none -serial stdio
qemu-system-aarch64 ... -accel tcg,tb-size=1536          # unchanged TCG path
```

- `-accel xlt` and `-machine accel=xlt` are both accepted; QEMU rejects mixing the two spellings
  ("-accel and \"-machine accel=\" options are incompatible", `system/vl.c`). Each name is
  validated with `accel_find()` (`system/vl.c:2292`), so a typo fails at startup with
  "invalid accelerator %s".
- `-accel tcg,...` keeps its meaning; xlt adds no properties to, and changes no behaviour of, the
  TCG accelerator. `tools/glwire/harness/README.md` documents `QEMU_EXTRA` and
  `-accel tcg,thread=single`; neither interacts with xlt.
- The project's delivery configuration is one vCPU (`QEMU_SMP=1`) because multi-core XNU panics in
  the scheduler lock (`docs/stage25-multicore.md`, section 12; `docs/handover.md:13`). The xlt
  threading model below therefore has to support one vCPU perfectly before it matters for more.

## Registration: two QOM types and one mandatory method

An accelerator is a QOM object named `<name>-accel` plus an operations object named
`<name>-accel-ops`. The macros are in `include/qemu/accel.h` (`TYPE_ACCEL "accel"`,
`ACCEL_CLASS_SUFFIX "-accel"`) and `include/sysemu/accel-ops.h` (`ACCEL_OPS_SUFFIX "-ops"`), so for
xlt the two type names are `xlt-accel` and `xlt-accel-ops`.

The init path, verified in `accel/accel-target.c`, `accel/accel-system.c` and `system/cpus.c`:

1. `configure_accelerators()` (`system/vl.c:2343`) resolves each `-accel` name with
   `accel_find()` (`accel/accel-target.c:44`), which builds `ACCEL_CLASS_NAME(name)` and looks the
   class up with `module_object_class_by_name()`.
2. `accel_init_interfaces()` (`accel/accel-target.c:104`) initialises the ops object through
   `accel_init_ops_interfaces()` (`accel/accel-system.c:65`), which appends `-ops` to the class
   name; if that type is missing QEMU prints "fatal: could not load module for type" and exits.
   The comment there is explicit: *all accelerators need to define ops, providing at least a
   mandatory non-NULL `create_vcpu_thread`*.
3. `accel_init_machine()` (`accel/accel-system.c`) stores the accelerator in
   `MachineState::accelerator`, sets `*(ac->allowed) = true` and calls `ac->init_machine(ms)`.
4. `qemu_init_vcpu()` (`system/cpus.c:648`) asserts that `cpus_accel->create_vcpu_thread` is
   non-NULL and calls it.

The `AccelClass` fields xlt fills are small: `name = "xlt"`, `init_machine` (return 0; optionally
reject CPU models xlt cannot translate), and `allowed = &xlt_allowed`. The `AccelOpsClass`
instance sets `create_vcpu_thread`, `kick_vcpu_thread` and `cpu_thread_is_idle`; the rest of the
struct (reset/pause synchronisation, `handle_interrupt`, `get_virtual_clock`) may stay NULL and
QEMU behaves like the generic case (`system/cpus.c:244`, `generic_handle_interrupt()`).

TCG's own registration is the pattern to copy: `TYPE_TCG_ACCEL` in `accel/tcg/tcg-all.c:53`,
`module_obj(TYPE_TCG_ACCEL)` at `accel/tcg/tcg-all.c:264`, and the ops assignment in
`accel/tcg/tcg-accel-ops.c:200-204`. New files go under `accel/xlt/` with a
`accel/xlt/meson.build` added from `accel/meson.build` under `if have_system`, plus a `config XLT`
entry in `accel/Kconfig`. A `module_obj()` line is required for modular (`--enable-modules`)
builds; a built-in QEMU does not need it but tolerates it.

## vCPU threading and lifecycle

The first implementation copies TCG's round-robin (rr) model: one host thread runs every guest
CPU, switching `current_cpu` only between blocks. The design point to support is `-smp 4` — four
guest CPUs time-shared on one host thread, exactly TCG's rr behaviour — even though the delivery
configuration runs one. This is the simplest model that is already exercised by TCG
(`accel/tcg/tcg-accel-ops-rr.c`, `rr_cpu_thread_fn()`, `rr_start_vcpu_thread()`), and it is the
only one the project can validate today, because the delivery guest is a single vCPU.

The thread function does, in order:

1. `rcu_register_thread()` and the accel's own thread registration; take the BQL with
   `qemu_mutex_lock_iothread()`.
2. Set `cpu->thread_id = qemu_get_thread_id()` and `cpu->neg.can_do_io = true`, then
   `cpu_thread_signal_created(cpu)` (`include/sysemu/cpus.h:29`).
3. Wait until the CPU is released (`first_cpu->stopped`, `qemu_cond_wait_iothread()`), then loop:
   for each CPU whose work list is empty and that has no `exit_request`, set the current CPU and,
   if `cpu_can_run(cpu)` (`include/sysemu/cpus.h:26`) is true, drop the BQL,
   run xlt's dispatcher for a slice, and retake the BQL.
4. On exit, clear the current CPU, consume `cpu->exit_request` if it was a kick, call
   `rr_wait_io_event()` to service I/O, then advance with `CPU_NEXT(cpu)`.
5. `cpu_thread_signal_destroyed(cpu)` (`include/sysemu/cpus.h:30`) on teardown.

Kicks arrive through the generic path: `cpu_interrupt()` (`include/hw/core/cpu.h:944`; the
`CPU_INTERRUPT_*` masks it takes are defined in `include/exec/cpu-all.h:284`) sets
`cs->interrupt_request` and, if the caller is not the vCPU thread, calls `qemu_cpu_kick(cpu)`
(`include/hw/core/cpu.h:835`). Because xlt leaves `AccelOpsClass::handle_interrupt` NULL,
`system/cpus.c:244` uses `generic_handle_interrupt()`, which is exactly the behaviour above. The
dispatcher re-reads `interrupt_request` between blocks.

The BQL is held whenever QEMU code runs and dropped only around translated guest code, matching
TCG's rr loop. Every trap handler in this document therefore starts with the BQL held.

Multi-threaded xlt (one host thread per vCPU, TCG's `mttcg`) is deliberately out of scope for
P1/P2: it needs a thread-safety review of the block cache and the translation cache, and the guest
it would help is the one that panics (`docs/stage25-multicore.md`, section 12). The
`AccelOpsClass` hook is the place to add it later (`-accel xlt,thread=multi`), not a second code
path.

## Execution model and the return-to-dispatcher ABI

A slice is "run translated blocks until a block asks QEMU for something". Within a slice the
dispatcher stays out of the way: a block ends with a direct jump to its successor block when the
successor is already translated, and returns only when it must. The M1 ABI returns the next guest
PC, which is enough for the standalone runner; the accelerator needs three more answers, so the
return value is split:

- bit 63 clear: the value is the next guest PC (guest physical addresses are far below 2^63).
- bit 63 set: an exit. Bits 62..56 carry `XltExitReason`, bits 55..0 a small payload (usually 0).

```
enum XltExitReason : uint8_t {
    XLT_EXIT_NEXT = 0,    /* not encoded as an exit; kept for logs */
    XLT_EXIT_MMIO,        /* load/store to a non-RAM address */
    XLT_EXIT_MSR,         /* MRS/MSR of a system register */
    XLT_EXIT_SEMIHOST,    /* HLT #0xf000 with semihosting enabled */
    XLT_EXIT_IRQ,         /* interrupt_request became non-zero */
    XLT_EXIT_FAULT,       /* synchronous data/prefetch abort */
    XLT_EXIT_ATOMIC,      /* exclusive/atomic sequence needs QEMU */
    XLT_EXIT_HALT,        /* WFI/WFE or guest requested stop */
    XLT_EXIT_TRANSLATE,   /* successor is not translated yet */
};
```

Details that the slow paths fill in live in a per-vCPU `XltVcpu` (M2's accel-only structure):
`CpuRegs` at offset 0, a trap record (faulting PC, virtual address, size, is-write, syndrome
inputs), the software TLB and the block index. The dispatcher sets a thread-local pointer to the
current `XltVcpu` before calling a block; a slow path may then fill the trap record without
changing the M1 `BlockFn` signature. This is a design choice, not a QEMU requirement.

Guest registers are copied between `CPUARMState` and `CpuRegs` at slice boundaries only:

- `xregs[0..30]` map one to one to `CpuRegs::x[0..30]`.
- `sp` is not `xregs[31]`; QEMU banks the stack pointers in `env->sp_el[0..3]` and which bank is
  active depends on the current EL and PSTATE.SP. Use QEMU's helpers and
  `pstate_read()`/`pstate_write()` (`target/arm/cpu.h:1614`/`:1624`) rather than open-coding the
  selection.
- Condition flags are not stored packed: `CPUARMState` keeps them in `env->NF`, `ZF`, `CF`, `VF`
  (top-bit encoding) and `pstate_read()` packs them into N/Z/C/V at bits 31/30/29/28, which is
  exactly `CpuRegs::nzcv`. `pstate_read()`/`pstate_write()` handle DAIF, SPSel, BTYPE and the rest
  as well.
- `pc` is `env->pc`.

After an exit, the dispatcher handles it with the BQL held, updates `env` (or the trap record) and
either chains to the next block or calls back into QEMU, then loops.

## Guest memory and MMIO

**The RAM fast path.** `virt` attaches one RAM region at guest physical address 1 GiB:
`memory_region_add_subregion(sysmem, vms->memmap[VIRT_MEM].base, machine->ram)`
(`hw/arm/virt.c:2263`; `VIRT_MEM` base is `GiB`, `hw/arm/virt.c:167`). The region covers the whole
`ram_size` and spills over the 4 GiB boundary, and no MMIO lives inside it; the floating high-IO
regions start at 256 GiB or above (`virt_set_memmap()`, `hw/arm/virt.c:1766`). With `-m 12G` the
guest RAM is `0x40000000..0x340000000`, contiguous.

That gives an identity window. At machine init xlt asks the machine for its RAM region, gets the
host pointer with `memory_region_get_ram_ptr()` (`include/exec/memory.h:1988`) — the underlying
field is `RAMBlock::host` (`include/exec/ramblock.h`), and `ramblock_ptr()`
(`include/exec/ram_addr.h:83`) is the inline arithmetic — and remembers
`[gpa_base, gpa_base + size) -> host_base`. A guest physical address inside the window becomes
`host_base + (gpa - gpa_base)` with one subtraction and one bounds check; that is the whole fast
path. `memory_region_is_ram()` (`include/exec/memory.h:1668`) answers "is this RAM?" when the
region list is not yet known.

With the MMU off (P1's bare-metal payloads) guest virtual addresses are physical addresses, so
the identity window is used directly. With the MMU on (P2, firmware and XNU) xlt keeps its own
software TLB: on a miss it calls `get_phys_addr()` (`target/arm/internals.h:1220`) with
`GetPhysAddrResult` (`target/arm/internals.h:1193`) and caches `(vpage -> host page, permissions,
attributes)` per vCPU. This is the same page-table walk TCG's `arm_cpu_tlb_fill()`
(`target/arm/tcg/tlb_helper.c:298`) performs; xlt caches the result in its own structure so it
does not depend on `CPUTLBEntry` layout. Translation faults are delivered as exceptions (below).

**MMIO.** A load or store outside the RAM window is an exit. The dispatcher calls

```c++
/* include/exec/memory.h:2642, implementation system/physmem.c:2869 */
MemTxResult address_space_rw(AddressSpace *as, hwaddr addr, MemTxAttrs attrs,
                             void *buf, hwaddr len, bool is_write);
```

with `as = cpu->as`, the guest physical address, the size and a buffer; the return value decides
success, device error or unassigned memory (the latter two become a data abort). Each access is
dispatched individually, in program order, because devices depend on it; there is no batching and
no speculation across an MMIO access. The initial attributes are `MEMTXATTRS_UNSPECIFIED`; P2 must
carry the security state and the access type (NS/secure, instruction fetch) *(unverified: which
`MemTxAttrs` fields QEMU expects from a non-TCG accelerator)*.

`address_space_rw()` requires the BQL, which is why MMIO exits are handled inside the locked
section and the lock is dropped only for pure translated code.

Byte order and unaligned accesses: AArch64 guests may access unaligned memory, and device memory
may reject it. xlt emits alignment-safe host accesses for normal memory and splits page-crossing
accesses; device accesses go through MMIO one access at a time, so the device's own `access_size`
checks apply.

## Traps: system registers, exceptions, semihosting, interrupts

**System registers.** An MRS/MSR is an exit carrying the encoded register. QEMU's table is the
source of truth: build the key with `ENCODE_AA64_CP_REG(CP_REG_ARM64_SYSREG_CP, crn, crm, op0, op1,
op2)` (`target/arm/cpregs.h:167`; the `CP_REG_ARM64_SYSREG_CP` constant is
`target/arm/kvm-consts.h:164`), look it up with `get_arm_cp_reginfo(cpu->cp_regs, key)`
(`target/arm/cpregs.h:983`), and read the value with `read_raw_cp_reg()`
(`target/arm/cpregs.h:1048`). Writes are the one gap: `write_raw_cp_reg()` exists but is static to
`target/arm/helper.c:77`, so the M2 patch adds a one-line exported wrapper (or calls
`ri->writefn` when the register is not raw). Registers marked `ARM_CP_NO_RAW`, special or RAZ/WI
need their `readfn`/`writefn` instead. Permission checks (`ri->accessfn` and the trap logic around
it) live in TCG's `access_check_cp_reg()` (`target/arm/tcg/op_helper.c`) and are not exported;
P1 handles only registers whose access is unconditional, and P2 adds an exported check.

The dispatcher caches `key -> ARMCPRegInfo *` in a small hash table, so the second access to
`CNTPCT_EL0` or `TPIDR_EL0` costs one lookup and one indirect call, not a hash of the key.

**Exceptions.** Synchronous exceptions are delivered by filling the public fault state and calling
the architectural entry code:

```c++
/* target/arm/cpu.h:1176; implementation target/arm/helper.c:11332 */
cs->exception_index = EXCP_DATA_ABORT;   /* or EXCP_PREFETCH_ABORT, EXCP_UDEF, ... */
env->exception.vaddress = fault_va;
env->exception.fsr = fsr;
env->exception.syndrome = syndrome;
env->exception.target_el = target_el;
arm_cpu_do_interrupt(cs);
```

`arm_cpu_do_interrupt()` asserts that the BQL is held. The `EXCP_*` constants are in
`target/arm/cpu.h:37-51`. `arm_deliver_fault()` (`target/arm/tcg/tlb_helper.c:163`) builds the
ESR/FSC encoding for a page fault from a `GetPhysAddrResult`; it is non-static but declared in no
header, and its helpers (`compute_fsr_fsc()`, `merge_syn_data_abort()`) are static to that file.
The M2 hook moves this fault encoding next to `arm_cpu_do_interrupt()` (or exports it) so xlt does
not have to re-implement ESR construction; the function only touches public `env` fields and
helpers such as `exception_target_el()` (`target/arm/internals.h:1482`).

**Semihosting.** QEMU's AArch64 trigger is `HLT #0xf000` with semihosting enabled
(`target/arm/tcg/translate-a64.c:2401-2402`); the M1 decoder must treat it as an exit, not as an
undefined instruction. The dispatcher then does what the static `tcg_handle_semihosting()`
(`target/arm/helper.c:11301`) does: call `do_common_semihosting(cs)` (declared in
`include/semihosting/`, implemented in `semihosting/arm-compat-semi.c`) with `env->xregs[0..3]`
already holding the call number and arguments, then `env->pc += 4`.

**Interrupts and the GIC.** The GICv3 model is unchanged; its CPU-interface IRQ/FIQ lines reach
`arm_cpu_set_irq()` (`target/arm/cpu.c:916`), which calls `cpu_interrupt()` with
`CPU_INTERRUPT_HARD`, `CPU_INTERRUPT_FIQ`, `CPU_INTERRUPT_VIRQ`, `CPU_INTERRUPT_VFIQ` or
`CPU_INTERRUPT_VSERR` (`target/arm/cpu.c:954`). With no `handle_interrupt` hook the generic path
sets `cs->interrupt_request` and kicks the vCPU thread (`system/cpus.c:244-258`). Before a slice
the dispatcher checks the pending mask; if an interrupt is deliverable it sets
`cs->exception_index = EXCP_IRQ` (or `EXCP_FIQ`), computes the target EL with
`arm_phys_excp_target_el()` (`target/arm/helper.c:10315`; the `:10212` definition is the
`CONFIG_USER_ONLY` stub) and calls `arm_cpu_do_interrupt(cs)`.

The full masking test is TCG's static `arm_excp_unmasked()` (`target/arm/cpu.c:656`); P1 uses a
simplified check (PSTATE.I/F for the current EL) and P2 should export the real one. Until then,
P1 only promises correct UART output for interrupt-free payloads.

## Self-modifying code and cache maintenance

Guest code can be overwritten, and devices can write guest RAM (DMA, loader). A block translated
from stale bytes is a correctness bug, so invalidation must cover both writers:

- Guest stores: the store slow path checks whether the target page contains translated blocks. The
  M2 engine keeps a per-page bitmap of code pages; P1's simpler variant write-protects translated
  guest pages with `mprotect(PROT_READ)` and handles the host `SIGSEGV` by invalidating the page,
  restoring write permission and returning to the dispatcher to re-execute the store.
- Device/DMA writes: these bypass translated code and go through QEMU's physical-memory helpers
  (`address_space_write()`/`address_space_write_rom()`, `system/physmem.c:2853`/`:2952`). QEMU's
  code-dirty logging is the right mechanism, but it is TCG-shaped: with
  `memory_region_set_log(mr, true, DIRTY_MEMORY_CODE)` (`include/exec/memory.h:2032`) enabled, the
  write path runs `assert(tcg_enabled()); tb_invalidate_phys_range(...)`
  (`system/physmem.c:2590-2593`) and aborts under `-accel xlt`. P1 therefore does not enable it
  (its payloads never write code), and P2 adds an accelerator hook that replaces the TCG test with
  a call into xlt's block invalidation; the exact edit is H5 in
  [INTEGRATION.md](INTEGRATION.md). Guest stores do not depend on this and must still be detected
  by the store path above.
- Cache maintenance and TLBI: `IC IVAU`/`DC IVAC`/`DC CVAU` are architecturally cache operations
  and can be treated as invalidation hints (or no-ops when the host is coherent); `TLBI *` flushes
  the software TLB. Both are exits, so they are cheap to get right.

## Translation cache on disk

Re-translating the kernel and SpringBoard on every boot is the largest avoidable cost of a boot,
so xlt's block cache has a persistent form. The key is a pair of hashes, never a path:

```
cache_root/<sha256(payload) : 16 bytes hex>/<abi_hash : 16 bytes hex>/
    index.bin     header + block directory
    blocks.bin    code bytes, one record per block
```

- `abi_hash` covers everything that changes generated code without changing guest bytes: xlt
  version, LLVM major version, host CPU features used, guest page size, the exit-ABI number, and
  the set of reserved host registers.
- Each block record stores the guest physical address and flags (EL, single-step) it was
  translated for, the length and a `sha256` of the guest bytes it covers, the host code bytes, and
  a relocation list. The record is valid only if re-hashing the guest bytes at the recorded address
  matches; otherwise it is discarded and retranslated.
- Relocations exist because M1 codegen embeds absolute host addresses (RAM base, helper function
  pointers, block-table entries). Kinds: `REL_HOST_IMM64` (patch the host pointer),
  `REL_RAM_OFFSET` (host = `ram_host + offset`), `REL_BLOCK_SLOT` (patch a block-table slot). The
  loader patches them at map time; a future PIC codegen using a reserved base register would drop
  the first and second kinds.
- Writes are `write(tmp) + fsync + rename`, and a header checksum protects against a truncated
  file. A corrupt or unreadable cache is a warning, never an error: xlt deletes the entry and
  translates.
- Location: `$XLT_CACHE_DIR`, else `$XDG_CACHE_HOME/xlt`, else `~/.cache/xlt`. `XLT_CACHE=0`
  disables reads and writes. The guest image hash comes from the bytes the loader consumed, so a
  changed kernel or ramdisk invalidates the cache by construction.

Self-modifying code invalidates both levels (memory and disk) per page.

## Limits and non-goals

- xlt translates AArch64 only. AArch32 guests, big-endian guests and KVM are out of scope.
- Live migration, gdbstub single-stepping and record/replay are out of scope; `-accel xlt` is a
  development and measurement accel, not a drop-in KVM replacement.
- The guest CPU model must be one whose QEMU implementation xlt can follow. The project uses
  `cortex-a76`; other models are unverified.
- P1 (bare-metal payload to first MMIO) needs no MMU support; P2 (firmware and XNU) does. The
  phase plan, acceptance metrics and the exact QEMU hooks are in [INTEGRATION.md](INTEGRATION.md),
  and the milestone landing plan — what ships in M1, what waits for M2 — lives in
  `docs/xlt-translator.md`.
- No QEMU tree is vendored. P1 ships patches 0001 (accelerator) and 0002 (machine) in
  [`patches/`](patches/), built by `build-xlt.sh` from a pristine 8.2.2 tarball. The hook inventory
  above was written for path B (the accelerator inside `-M virt`); under the chosen path A (see
  [`p2/PLAN.md`](p2/PLAN.md)) most of it becomes unnecessary — the machine owns the GIC and timers,
  the engine owns exceptions, and no shared QEMU file is modified unless the DMA-invalidation risk
  (R5) forces the H5 hook in `system/physmem.c`. [ADR-001](../../docs/adr/ADR-001-qemu-fork.md) asks
  for core QEMU changes to stay at zero, so if R5 fires that edit needs an explicit exception
  recorded in the ADR (or has to live as a tracked patch in the fork); pretending otherwise would
  make the plan look cleaner than it is.

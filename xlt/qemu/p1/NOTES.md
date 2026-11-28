# P1 QEMU integration: `-M xlt` + `-accel xlt`

Status: implemented and verified against QEMU 8.2.2 on 2026-11-28. The upstream
tree is not modified in place and no QEMU tree is vendored. The changes are a
two-patch text series in [`../patches/`](../patches/).

## Components

- `accel/xlt/xlt-accel.c`, `accel/xlt/xlt-accel-ops.c`: the QOM
  `xlt-accel` class and its mandatory `xlt-accel-ops` object. The accelerator
  owns guest startup: `AccelClass::setup_post` calls the machine's
  `xlt_machine_start_guest()` hook after machine init, and that hook starts a
  detached QEMU thread which registers with RCU, takes the BQL and runs the
  guest through libxlt. The machine itself never starts the guest.
- `hw/arm/xltm.c`: `xlt-machine`, a CPU-less board with 256 MiB of RAM at
  `0x40000000`, a PL011 at `0x09000000`, the QEMU ELF loader and the host
  callbacks used by translated code.
- `xlt/src/libxlt_c.{h,cpp}`: the C ABI bridge between QEMU (C) and the
  host-neutral `xlt::LibXlt` (C++). QEMU build flags link
  `xlt/build/qemu/libxlt.a` and LLVM/capstone.
- `xlt/tests/fixtures/p1/`: two freestanding AArch64 images. Fixture 1 prints
  one line; fixture 2 reads PL011 FR, stores/loads a 64-bit word in RAM and
  reads CurrentEL before shutting down through PSCI `SYSTEM_OFF`.

## Build

```sh
xlt/qemu/build-xlt.sh
xlt/tests/fixtures/p1/build.sh
```

`build-xlt.sh` copies the pristine source to `$QEMU_WORK_DIR/qemu-8.2.2-xlt`,
applies `0001-accel-add-xlt-accelerator.patch` and
`0002-hw-arm-add-xlt-machine.patch`, builds the PIC libxlt archive, then
configures and builds only `qemu-system-aarch64` in
`$QEMU_WORK_DIR/qemu-8.2.2-xlt-build`. `QEMU_WORK_DIR` defaults to
`/home/lshan/xlt`.

## Run

```sh
QEMU=/home/lshan/xlt/qemu-8.2.2-xlt-build/qemu-system-aarch64
$QEMU -M xlt -cpu cortex-a76 -smp 1 -m 256M -accel xlt \
      -kernel xlt/build/fixtures/p1/fixture1.elf \
      -display none -serial stdio -monitor none -no-reboot
```

Observed output, exit 0:

```text
xlt-p1-fixture1
```

```text
xlt-p1-fixture2 fr=0x0000000000000090 ram=0x1122334455667788 el=0x0000000000000004
```

The `qemu-smoke` image from t9 also prints `xlt-p1-ok` with the same flags.

## P1 limits

- One guest vCPU. The machine creates no `CPUState`; `xlt_create_vcpu_thread`
  is a no-op and the dispatcher is started from `AccelClass::setup_post`
  (`xlt_setup_post()` → `xlt_machine_start_guest()`, which starts the detached
  libxlt thread). The MachineClass still advertises `cortex-a57` as its default
  CPU type because QEMU's board init expects a valid type, but no CPU object is
  instantiated.
- No interrupts, MMU, GIC, SMMU or second-stage translation. The fixtures are
  interrupt-free bare-metal images linked at `0x40080000`.
- MMIO is synchronous: generated code calls `xlt_host_read`/`xlt_host_write`,
  which use `address_space_rw()` in the guest thread while holding the BQL.
- `HVC` is an exit. `xltm_hvc` implements PSCI `SYSTEM_OFF` and
  `SYSTEM_RESET`; other calls terminate with the unsupported exit code. The
  lifter flushes the register file before the HVC call.
- Self-modifying guest code and device/DMA code invalidation are not part of
  P1; see `xlt/qemu/INTEGRATION.md` for the P2 hooks.
- `-M xlt` without `-accel xlt` is rejected during machine init with
  `-M xlt requires -accel xlt` (exit 1); the machine no longer starts a guest
  thread on its own.

## Evidence

- `xlt/qemu/build-xlt.sh` link succeeded against QEMU 8.2.2.
- Both fixture ELFs and the `qemu-smoke` image produce the exact strings above
  under `-accel xlt`, rc 0.
- `-M xlt` without `-accel xlt` prints `qemu-system-aarch64: -M xlt requires
  -accel xlt` and exits 1.
- `xlt/tests/fixtures/p1/build.sh` uses clang's AArch64 target and lld; the
  images are ignored build artifacts, never committed.

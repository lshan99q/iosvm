# FDT notes for P2.4 — what `-M xlt` must expose, and what goes in the device tree

This is a research note, not a plan. [PLAN.md](PLAN.md) says what P2.4 must build; this file
records what the firmware and the loader actually read, with source references, so the machine
model can be written once instead of guessed at. Register-level detail for exceptions, timers
and the GIC lives in [ARM-DETAILS.md](ARM-DETAILS.md); the accelerator side is described in
[../INTEGRATION.md](../INTEGRATION.md).

Everything below was checked against:

* QEMU 8.2.2, `/home/lshan/xlt/qemu-8.2.2` (the tree the P1 patch series applies to).
* EDK2 `edk2-stable202402`, files fetched from the pinned tag and cited as
  `ArmVirtPkg/...:line` (the local firmware is Ubuntu's `AAVMF_CODE.fd`, version string
  `2024.02-2ubuntu0.10`, i.e. the same stable tag plus distribution patches; the line numbers
  are from the upstream tag, not from the Debian build).
* Boot experiments on this host with the project's real payload
  (`qemu/run.sh` recipes from the XNU tree, `/tmp/xltboot/{esp,payload.bin}`, 7 GiB, TCG).

Claims that could not be checked against a primary source are marked **hypothesis** together
with the experiment that would settle them.

## 1. The boot chain as it exists today

With the real recipe (`-M virt -drive if=pflash,...code -drive if=pflash,...vars
-drive file=fat:...,if=virtio -device loader,file=payload.bin,addr=0xF0000000`):

1. The CPU resets to `PC = 0x0`, the reset vector inside the first pflash bank
   ([hw/arm/boot.c:682-778](https://gitlab.com/qemu-project/qemu/-/blob/v8.2.2/hw/arm/boot.c):
   `arm_load_kernel()` only redirects the PC for a direct-kernel boot; with firmware the
   boot_info is left `NULL`).
2. EDK2's PrePi reads the DTB from the fixed address `PcdDeviceTreeInitialBaseAddress`
   = `0x40000000` (`ArmVirtPkg/ArmVirtQemu.dsc:204`) — QEMU writes it there in
   `virt_machine_done()` ([hw/arm/virt.c:1668-1686](https://gitlab.com/qemu-project/qemu/-/blob/v8.2.2/hw/arm/virt.c)).
   No register carries the DTB pointer; the address is a contract between QEMU's `virt` and
   EDK2, not something the firmware can discover.
3. `QemuVirtMemInfoPeiLib` walks the DTB for `device_type = "memory"`, takes the lowest node,
   and **asserts** that its base equals `PcdSystemMemoryBase` (`0x40000000`) and its size is at
   least 128 MiB (`ArmVirtPkg/Library/QemuVirtMemInfoLib/QemuVirtMemInfoPeiLibConstructor.c:39`).
4. DXE drivers take the GIC, timer, UART, NOR-flash (UEFI variables) and virtio transports from
   the DTB (§2), enumerate the ESP and start `\EFI\BOOT\BOOTAA64.EFI` — the xnuloader.
5. The xnuloader does **not** look at QEMU's DTB. It has a compiled platform table
   (`platform_qemu_virt`: `io_base 0x08000000`, `gic_dist 0x08000000`, `gic_redist 0x080A0000`,
   `uart 0x09000000`, `psci_hvc = 1`), finds the payload at `0xF0000000` by its `XNUP` magic,
   optionally programs ramfb through fw_cfg's `etc/ramfb` file (base `0x09020000` hard-coded in
   `xnuloader/loader.c`), builds the Apple device tree XNU wants, and jumps to the kernel.

So the DTB `-M xlt` emits matters to **EDK2 only**. The payload's own view of the platform is
hard-coded in the loader and must match the machine's memory map, not its DTB.

## 2. What EDK2 reads from the DTB

| Node / property | Consumer | What happens if it is missing |
|---|---|---|
| `/memory@40000000` `reg` | `QemuVirtMemInfoPeiLib` | `ASSERT (fdt_check_header(...) == 0)` / `ASSERT (base == 0x40000000)` / `ASSERT (size >= 128 MiB)` fire; no RAM at all. Additional `/memory` nodes above 4 GiB are added to the GCD by `HighMemDxe` (`OvmfPkg/Fdt/HighMemDxe/HighMemDxe.c:103`). |
| `/intc` compatible `arm,gic-v3`, `reg` = distributor + redistributor | `ArmVirtGicArchLib` | `ASSERT (RegSize >= 32)` at `:86`, `PcdGicDistributorBase`/`PcdGicRedistributorsBase` stay 0; the GIC driver cannot start. The library also toggles `ICC_SRE_EL2.SRE` (`:114-124`) and asserts it reads back set — the machine's GIC must offer the system-register interface. |
| `/timer` compatible `arm,armv7-timer`/`arm,armv8-timer`, `interrupts` (4×3 cells) | `ArmVirtTimerFdtClientLib` | `ASSERT (PropSize >= 36)`, PPI numbers stay 0 → timer interrupts never fire. `INTID = Number + (Type ? 16 : 0)` (`:76-84`), so QEMU's `<1 13>`, `<1 14>`, `<1 11>`, `<1 10>` become INTIDs 29, 30, 27, 26. |
| `/pl011@9000000` compatible `arm,pl011`, `reg`; `/chosen/stdout-path` | `FdtSerialPortAddressLib` → `FdtPL011SerialPortLib` | serial output stops; the library reads `stdout-path` to pick the node (`OvmfPkg/Library/FdtSerialPortAddressLib/FdtSerialPortAddressLib.c:168`, `:54`). The PL011 clock is a fixed PCD, so `clocks`/`clock-frequency` are optional. |
| `/flash@0` compatible `cfi-flash`, `reg` (two banks), `bank-width` | `NorFlashQemuLib` + `VirtNorFlashDxe` | no non-volatile UEFI variables. The library reads up to four `(base, size)` pairs, uses 256 KiB blocks, skips the bank overlapping the firmware FV, and sets `status = "disabled"` on the node (`ArmVirtPkg/Library/NorFlashQemuLib/NorFlashQemuLib.c:106`, `:123`). |
| `/virtio_mmio@...` compatible `virtio,mmio`, `reg` = 16 bytes | `VirtioFdtDxe` | no virtio transports → the ESP (and any virtio disk) is invisible. The driver asserts `RegSize == 16` and calls `VirtioMmioInstallDevice(base, handle)` per node (`OvmfPkg/Fdt/VirtioFdtDxe/VirtioFdtDxe.c:84`, `:127`); `interrupts` is ignored. |
| `/psci` compatible `arm,psci-0.2`, `method = "hvc"` | `ArmVirtPsciResetSystemLib` | the reset/shutdown library finds no method and does nothing (`ArmVirtPkg/Library/ArmVirtPsciResetSystemLib/ArmVirtPsciResetSystemLib.c:51-67`, `:95-108`). With `method = "hvc"` the driver issues `PSCI SYSTEM_RESET`(0x84000009) / `SYSTEM_OFF`(0x84000008) as HVC calls. QEMU's node lists `"arm,psci-1.0", "arm,psci-0.2", "arm,psci"`, so the `0.2` match succeeds. |
| `/chosen/stdout-path` | `FdtSerialPortAddressLib` | see the PL011 row; without it the firmware picks no console. |
| `/fw-cfg@9020000` | — | not required for boot. EDK2's `QemuFwCfgLib` talks to the fixed MMIO base (`0x09020000`); `QemuRamfbDxe` needs the **device**, not the node (`OvmfPkg/QemuRamfbDxe/QemuRamfb.c:260` looks up the `etc/ramfb` file through fw_cfg). |
| `/pcie@...` | PCIe host bridge enumeration | with `virt`'s default `-drive if=virtio`, the ESP is a **virtio-pci** disk; without a `/pcie` node EDK2 never enumerates it and boots to the UEFI Shell instead. Verified empirically (§4, T1 vs A). |
| `/cpus`, `/cpus/cpu@0`, optional `cpu-map` | MP services / topology | boot works with a single `cpu@0` node; `cpu-map` is optional (T3 removed it and the stack still booted). |
| `/pmu`, `/its`, `/platform-bus`, `/pl031` | optional drivers | none needed for the payload: T3 removed all four and the stack still reached the loader. |

Two practical consequences:

* The `/memory` node must be **one region starting at 0x40000000** with exactly the `-m` size,
  even when it crosses 4 GiB (QEMU emits `reg = <0x0 0x40000000 0x1 0xc0000000>` for `-m 7G`);
  EDK2 takes the size from the node and hand the >4 GiB part to `HighMemDxe`.
* The DTB has to be at `0x40000000`, i.e. exactly the start of guest RAM, before the CPU
  starts. Writing it as a "rom" blob during machine init is enough (see §3 for the ordering).

## 3. pflash, the loader device, and `-kernel`/`-bios`

### pflash pair

`virt` creates two `pflash_cfi01` devices with 256 KiB sectors, width 4 and device-width 2, maps
bank 0 at `0x0` and bank 1 at `0x04000000`, and connects them to `-drive if=pflash`
([hw/arm/virt.c:1121-1187](https://gitlab.com/qemu-project/qemu/-/blob/v8.2.2/hw/arm/virt.c)).
Its DT node is a single `cfi-flash` covering both banks (`/flash@0`,
`reg = <0 0x4000000 0 0x4000000>`, `bank-width = 4`, `:1189-1235`); the secure variant splits it
into `/secflash@0` + `/flash@4000000`. No `qemu,uefi-*` property exists in QEMU 8.2.2 — the task
brief that mentions one is wrong for this tree.

`-M xlt` needs the same pair, the same addresses and the same DT shape: the code bank holds the
firmware (read-only drive) and the vars bank holds the UEFI variable store (writable drive), and
EDK2 only keeps variables if it can see the second bank through the `cfi-flash` node.

### `-device loader`

`-device loader,file=payload.bin,addr=0xF0000000,force-raw=on` is a `TYPE_LOADER` device that
registers a ROM blob; `rom_reset()` writes it with `address_space_write_rom()` and calls
`cpu_flush_icache_range()` ([hw/core/loader.c:1250-1300](https://gitlab.com/qemu-project/qemu/-/blob/v8.2.2/hw/core/loader.c)).
No CPU object is involved, so it works on a CPU-less machine as long as the address is inside a
RAM region. Ordering, verified in `system/vl.c`:

```
machine_run_board_init()      vl.c:2621   -> qemu_system_reset() inside it (machine.c:1569)
qemu_create_cli_devices()     vl.c:2713   -> realizes -device loader / virtio / ramfb
accel_setup_post()            vl.c:3764   -> P2.3/P2.4 accel starts the guest here
```

The payload is therefore in guest RAM before the accelerator starts executing, with no extra
synchronisation: a `-M xlt` that starts the guest from `setup_post` gets the same ordering as
`virt`. With `-m 7G` the RAM window is `0x40000000..0x200000000`, so `0xF0000000` and the
loader's ramdisk address `0xFC000000` are both inside it; with the 12 GiB harness setting they
obviously are too.

### `-kernel` / `-bios` for `-M xlt`

P1's machine requires `-kernel <elf>` and runs it with its own loader. P2.4 has to support the
firmware path instead, and the two must not fight:

* **Firmware boots from pflash.** Keep `-drive if=pflash,...` as the way to supply EDK2; do not
  invent a `-bios` semantics that loads at `0x0` into a RAM region, because bank 0 is a device,
  not RAM. A `-bios <file>` option can be accepted as a convenience that stages the file into a
  pflash-like device, but the honest contract is "pflash drives carry the firmware".
* **`-kernel <elf>` keeps its P1 meaning** for the fixtures: load the ELF, set `PC` to its entry,
  start the guest from `setup_post`. In firmware mode the DTB is written at `0x40000000` and the
  CPU starts at `0x0`; in `-kernel` mode the DTB (if any) must go somewhere that cannot collide
  with the loaded segments.
* **`-kernel` together with pflash** is what `virt` resolves through fw_cfg
  ([hw/arm/boot.c:1145-1200](https://gitlab.com/qemu-project/qemu/-/blob/v8.2.2/hw/arm/boot.c):
  `arm_setup_firmware_boot()` exposes the kernel as a fw_cfg item, or refuses with
  "This machine type does not support loading both a guest firmware/BIOS image and a guest
  kernel at the same time"). P2.4 does not need that combination; either reject it with a clear
  message or implement the fw_cfg handoff later (the EDK2 path would need
  `QemuKernelLoaderFsDxe`).

## 4. What the experiments showed

All runs: `-M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 7G -accel tcg,tb-size=384`, real
pflash pair, ESP and payload from the project, serial captured to a file, 240 s timeout.
"Booted" means the log contains `XNULOADER` and then `Darwin Kernel Version`.

| Run | DTB | ESP transport | Result |
|---|---|---|---|
| A | QEMU-generated (no `-dtb`) | `-drive if=virtio` | boots; first boot entry is `"UEFI Misc Device"` at `PciRoot(0x0)/Pci(0x2,0x0)` — under stock `virt` the ESP really is virtio-pci |
| T1 | `minf` = dump minus `/pcie`, `/pmu`, `/platform-bus`, `/its`, `/fw-cfg` | `-drive if=virtio` | UEFI Shell, "map: No mapping found" — no PCIe → the virtio-pci disk does not exist |
| T2 | `minf` | `-drive file=fat:...,if=none,id=esp0` + `-device virtio-blk-device,drive=esp0` | boots — the reduced DT is enough when the ESP is virtio-mmio |
| T3 | `minf` minus `/pl031`, `/chosen`, `/apb-pclk`, `/cpus/cpu-map` | virtio-mmio | boots — those nodes are not needed for the payload |

Two traps worth recording:

* **Stale NVRAM.** Early `-dtb` runs fell to the UEFI Shell even with a correct DTB because the
  reused `vars.fd` still held a boot order from a previous device topology. Always start DT
  experiments from a fresh `AAVMF_VARS.fd` copy, or the result says nothing about the DTB.
* **The `if=virtio` shorthand binds to PCIe on `virt`.** `-drive if=virtio` does not mean
  virtio-mmio there; to exercise the virtio-mmio path the disk must be attached explicitly with
  `-device virtio-blk-device,drive=...`.

Consequence for P2.4: either the machine grows a PCIe host bridge and a virtio-pci model (large,
and outside what P2 promised), or the `-M xlt` recipe attaches the ESP as virtio-mmio and the
machine provides virtio-mmio transports plus their DT nodes. The second option is what §5
assumes; the first can be revisited in P3 if a device needs PCIe.

## 5. Minimal FDT for `-M xlt`

Build it with the `qemu_fdt_*` helpers (`include/sysemu/device_tree.h`), backed by the in-tree
libfdt (`subprojects/dtc/libfdt`, pulled in by `meson.build:3129-3132`); there is no system
libfdt on the host. Emit, in this order:

```dts
/ {
    compatible = "linux,dummy-virt";            /* cosmetic; EDK2 does not match on it */
    #address-cells = <0x2>;
    #size-cells = <0x2>;

    psci {
        compatible = "arm,psci-1.0", "arm,psci-0.2", "arm,psci";
        method = "hvc";
        cpu_suspend = <0xc4000001>;
        cpu_off     = <0x84000002>;
        cpu_on      = <0xc4000003>;
        migrate     = <0xc4000005>;             /* AArch64 */
    };

    memory@40000000 {
        device_type = "memory";
        reg = <0x0 0x40000000 0x1 0xc0000000>;  /* -m size; crosses 4 GiB for -m >= 4G */
    };

    cpus {
        #address-cells = <0x1>;
        #size-cells = <0x0>;
        cpu@0 {
            device_type = "cpu";
            compatible = "arm,cortex-a76";
            reg = <0x0>;
        };
    };

    timer {
        compatible = "arm,armv8-timer", "arm,armv7-timer";
        always-on;
        interrupts = <0x1 0xd 0x4>, <0x1 0xe 0x4>, <0x1 0xb 0x4>, <0x1 0xa 0x4>;
    };                                          /* INTIDs 29, 30, 27, 26 */

    intc@8000000 {
        compatible = "arm,gic-v3";
        #interrupt-cells = <0x3>;
        interrupt-controller;
        #address-cells = <0x2>;
        #size-cells = <0x2>;
        reg = <0x0 0x08000000 0x0 0x10000>,     /* GICD */
              <0x0 0x080a0000 0x0 0xf60000>;    /* GICR */
        #redistributor-regions = <0x1>;
    };

    pl011@9000000 {
        compatible = "arm,pl011", "arm,primecell";
        reg = <0x0 0x09000000 0x0 0x1000>;
        interrupts = <0x0 0x1 0x4>;             /* INTID 33 */
    };

    flash@0 {
        compatible = "cfi-flash";
        reg = <0x0 0x0 0x0 0x4000000>,          /* two banks, one node */
              <0x0 0x4000000 0x0 0x4000000>;
        bank-width = <0x4>;
    };

    virtio_mmio@a000000 {                       /* one node per transport */
        compatible = "virtio,mmio";
        reg = <0x0 0xa000000 0x0 0x200>;
        interrupts = <0x0 0x30 0x1>;            /* SPI 16 = INTID 48, edge */
        dma-coherent;
    };

    chosen {
        stdout-path = "/pl011@9000000";
    };
};
```

Notes on the table:

* `interrupts` cells are `<type number flags>`: type 0 = SPI (number + 32), type 1 = PPI
  (number + 16), per the GICv3 binding. The values above are QEMU's own, so they are already
  in INTID space after the firmware adds the offset.
* Every `virtio_mmio` node that exists in the machine should have one, at
  `0x0a000000 + i * 0x200`, with INTID `48 + i`; EDK2 only needs the transport that carries the
  disk, but attaching disks to later slots is a normal user move.
* `/fw-cfg@9020000`, `/pcie`, `/pmu`, `/its`, `/platform-bus`, `/pl031`, `/apb-pclk` and
  `cpu-map` are **not** required by the payload path (T3). Add `/fw-cfg` and create the fw_cfg
  device only if display is wanted (§6).
* The DTB itself must be written at `0x40000000` before the guest starts; a 512 KiB region is
  plenty. `qemu_fdt_dumpdtb()` is the debug hook that produced the reference tree in §4.

## 6. Display: fw_cfg + ramfb

`-M xlt` does not need a display to boot to the kernel, but P2.5 (SpringBoard, screenshots)
does. The path is entirely fw_cfg-based:

* The machine creates an fw_cfg device at `0x09020000` with DMA, exactly like `virt`:
  `fw_cfg_init_mem_wide(base + 8, base, 8, base + 16, &address_space_memory)`
  ([hw/arm/virt.c:1283-1302](https://gitlab.com/qemu-project/qemu/-/blob/v8.2.2/hw/arm/virt.c)).
* QEMU's `ramfb` device then finds it through `fw_cfg_find()`, requires DMA to be enabled, and
  publishes the `etc/ramfb` file plus the scanout
  ([hw/display/ramfb.c:140-155](https://gitlab.com/qemu-project/qemu/-/blob/v8.2.2/hw/display/ramfb.c)).
  `-device ramfb` therefore works on any machine that has a DMA-capable fw_cfg.
* EDK2's `QemuRamfbDxe` uses that file to install a GOP
  (`OvmfPkg/QemuRamfbDxe/QemuRamfb.c:255-260`), and the xnuloader instead writes its own
  `etc/ramfb` configuration (1125×2436, XR24, stride = width×4) to make the window match the
  iPhone panel (`xnuloader/loader.c`, `ramfb_phone_screen()`).
* If either half is missing, the loader prints `no display (QEMU without -device ramfb)` and
  the kernel still boots; SpringBoard then draws into memory nobody scans out.

Adding fw_cfg + `-device ramfb` to `-M xlt` is cheap (both are device models that already exist
in QEMU); the DT node for fw_cfg is optional, but harmless and matches `virt`.

## 7. PSCI

| Caller | IDs used | Conduit |
|---|---|---|
| EDK2 `ArmVirtPsciResetSystemLib` | `SYSTEM_RESET` 0x84000009, `SYSTEM_OFF` 0x84000008 | HVC (chosen from `/psci/method`) |
| xnuloader | `CPU_OFF` 0x84000002, `CPU_ON` 0xC4000003, `AFFINITY_INFO` 0xC4000004, returns 0 / 1 / -1 | HVC via `hvc #0` |
| XNU | `PSCI_VERSION` 0x84000000, `AFFINITY_INFO` 0xC4000004, `CPU_ON` 0xC4000003 (**hypothesis**) | HVC from the Apple DT's `psci-method` |

So the machine must accept **both** the 32-bit and the 64-bit function-ID forms, and answer:

* `VERSION` (0x84000000) → `0x00010001` (PSCI 1.1; anything ≥ 0.2 satisfies the reset library
  and the loader; 1.1 is what QEMU's TCG returns through `arm_handle_psci_call`).
* `FEATURES` (0x8400000A) → 0 for functions implemented, -1 (`NOT_SUPPORTED`) otherwise. EDK2
  does not call it; XNU may probe it (**hypothesis**).
* `SYSTEM_OFF` / `SYSTEM_RESET` → terminate the machine (exit 0 in the P1 convention); never
  return.
* `CPU_ON` for the single vCPU → `ALREADY_ON` (-4) when the target is MPIDR 0, `INVALID_PARAMS`
  (-2) for any other MPIDR; `AFFINITY_INFO` → 0 (ON) for MPIDR 0, -2 otherwise.

The EDK2 side is settled by the source; the XNU side is the one place where a wrong answer will
show up late (secondary-CPU bring-up during boot). The confirming experiment is: boot the
payload with a `CPU_ON` that returns SUCCESS but never starts a core, and watch whether Darwin
hangs in `cpu_init` or falls back to `cpus=1`. Keep `-smp 1` until then.

## 8. virt vs `-M xlt` today vs P2.4

| Feature | `virt` (8.2.2) | `-M xlt` in P1 | P2.4 must add |
|---|---|---|---|
| RAM base / size | `0x40000000`, `-m`; one `/memory` region | `0x40000000`, fixed 256 MiB | `-m`-driven size, `/memory` node with the full size |
| Firmware | two pflash banks at `0x0` / `0x04000000`, `-drive if=pflash` | none (ELF only) | same pflash pair + `cfi-flash` node; CPU resets to `0x0` |
| DTB | written at `0x40000000` (`virt_machine_done`) | none | emit §5, write at `0x40000000` before `setup_post` |
| CPU / MPIDR | real `ARMCPU`s, cortex-a76, `cpu@0` node | no CPU object | one `cpu@0` node; machine model keeps its own register file |
| GICv3 | `arm,gic-v3` dist+redist, ICC_* sysregs | none | GICD/GICR model + ICC_* sysregs + IRQ lines (ARM-DETAILS §5) |
| Generic timer | 62.5 MHz, PPIs 29/30/27/26 | CNTVCT/CNTFRQ stubs only | `arm,armv8-timer` node + CVAL/TVAL/CTL + PPI injection |
| UART | PL011 at `0x09000000`, SPI 1 → INTID 33 | PL011 present, no interrupts | same + `/pl011` node + `stdout-path` |
| Virtio | 32 virtio-mmio transports **and** virtio-pci via PCIe | none | 32 virtio-mmio transports + nodes (or PCIe+virtio-pci, deferred) |
| PCIe | ECAM at `0x10000000` (low) or `0x4010000000` (high with >3 GiB RAM) | none | not in P2.4; document the virtio-mmio ESP recipe |
| fw_cfg | at `0x09020000`, DMA, always created | none | create it; needed for ramfb/GOP |
| ramfb / display | `-device ramfb` + fw_cfg | none | fw_cfg + ramfb; no DT requirement |
| PSCI | `arm,psci-1.0/0.2`, HVC, 0x840000xx and 0xC40000xx | HVC 0x84000008/09 only | full ID set of §7 |
| RTC / PMU / ITS / platform-bus | optional devices | none | not needed (T3) |

## 9. Hypotheses and the experiment that settles each

| Hypothesis | Experiment |
|---|---|
| XNU's arm64 PSCI stub uses VERSION/AFFINITY_INFO/CPU_ON exactly as listed in §7 | Trace HVC arguments during a payload boot (QEMU `-d int` or a temporary print in the xlt HVC handler); compare with the kernel's `cpus=1`/`cpus=4` behaviour. |
| EDK2 enumerates PCIe from the `/pcie` node (not from ACPI or a PCD) | Boot T1 again with a minimal `/pcie` node restored; if the disk appears, the node is the trigger. Already consistent with T1/A. |
| A 512 KiB DTB region at `0x40000000` is enough for the machine's own tree | Emit the tree, check `fdt_totalsize`, and boot EDK2; the memory node is a few hundred bytes. |
| The loader needs fw_cfg at exactly `0x09020000` | Change the machine's fw_cfg base in a scratch build; the loader's hard-coded `FW_CFG_BASE` should stop finding `etc/ramfb`. |
| `-machine memory-backend=ram` works once RAM is a single memory region | Run the run-darwin.sh recipe with `-M xlt` once RAM is `-m`-sized; check that the backend maps the whole region. |

## 10. Summary for the machine patch

Five load-bearing facts:

1. EDK2 reads the DTB from the fixed address `0x40000000` and asserts
   `/memory` base == `0x40000000`; the DTB must be there before the CPU starts.
2. `virt`'s ESP is virtio-**pci** (`if=virtio`), and without a `/pcie` node EDK2 does not boot
   it; with a virtio-mmio ESP and a reduced DT it does (T1 vs T2/T3).
3. EDK2 needs `/intc` (GICv3 dist+redist), `/timer` (four PPIs), `/pl011`, `/flash@0`,
   `/psci` with `arm,psci-0.2` + `method=hvc`, and one `/virtio_mmio` node per transport.
4. `-device loader` writes guest RAM during machine init, before `accel_setup_post`; starting
   the guest from `setup_post` needs no extra ordering.
5. Display needs fw_cfg (DMA) at `0x09020000` plus `-device ramfb`; the DTB node is optional.

Top P2.4 risk: **the ESP transport**. The project's recipe uses `if=virtio`, which on `virt` is
virtio-pci; `-M xlt` in P2.4 has no PCIe model. Either the recipe for `-M xlt` attaches the ESP
with `-device virtio-blk-device,drive=esp0` (virtio-mmio; verified to boot), or P2.4 grows a
PCIe ECAM host bridge and a virtio-pci device — a much larger change that also drags in an
`msi-map`/ITS discussion. Decide this before writing the machine patch.

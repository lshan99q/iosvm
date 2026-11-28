# `-M xlt` machine specification for P2.4

This is the implementation contract for growing the custom `-M xlt` machine from P2.3 (fixtures,
no firmware) to a payload boot with EDK2 and the xnuloader. It turns the research in
[FDT-NOTES.md](FDT-NOTES.md) into concrete addresses, sizes, interrupts and device-tree values so
the machine can be written and reviewed against a fixed target. [PLAN.md](PLAN.md) is the phase
plan; [ARM-DETAILS.md](ARM-DETAILS.md) is the register-level reference for exceptions, the GIC and
the timer.

Every choice below is either copied from QEMU 8.2.2 `virt` (`/home/lshan/xlt/qemu-8.2.2`) or was
verified with the boot experiments recorded in FDT-NOTES.md §4. Values that differ from the task
brief are called out explicitly.

## 0. Decisions at a glance

* The machine is CPU-less from QEMU's point of view (no `ARMCPU`, no TCG): it owns the guest state
  and starts the engine from the accelerator's `setup_post`, as P1/P2.3 already do.
* RAM starts at `0x40000000` and its size comes from `-m`. The `/memory` node must carry exactly
  that base and size; EDK2 asserts both.
* The device tree is written at `0x40000000` (start of RAM) before the guest starts, matching
  `PcdDeviceTreeInitialBaseAddress`.
* Firmware comes from **two pflash banks**: code at `0x00000000`, variables at `0x04000000`,
  64 MiB each, `-drive if=pflash` attached through `pflash0`/`pflash1` object aliases.
* The ESP disk is **virtio-mmio** (`-device virtio-blk-device,drive=esp0`), not PCIe. Decision in
  §9; no PCIe host bridge is modelled in P2.4.
* PSCI is the `HVC` conduit; the machine answers the 32-bit and 64-bit function IDs listed in §7.
* Display is not required for P2.4: serial-only (`-display none`) is the acceptance path, and the
  fw_cfg device is created so that `-device ramfb` can be enabled in P2.5 without a machine change.

## 1. Address map

| Region | Base | Size | Notes |
|---|---|---|---|
| pflash bank 0 (code) | `0x00000000` | 64 MiB | `-drive if=pflash,readonly=on,file=$AAVMF_CODE.fd`; reset vector and EDK2 code live here |
| pflash bank 1 (vars) | `0x04000000` | 64 MiB | `-drive if=pflash,format=raw,file=$vars.fd`; UEFI variable store |
| GIC distributor | `0x08000000` | 64 KiB | `arm,gic-v3` first reg entry; mirrors `virt`'s `VIRT_GIC_DIST` |
| GIC redistributor | `0x080A0000` | 128 KiB per vCPU | one vCPU in P2.4, so 128 KiB; `virt` reserves `0xF60000` for 123 vCPUs, xlt does not need that |
| PL011 UART | `0x09000000` | 4 KiB | serial console, `-serial stdio` |
| fw_cfg | `0x09020000` | 0x18 | data `+0x00`, selector `+0x08`, DMA `+0x10` (`virt` uses the same layout, hw/arm/virt.c:1283-1302: `fw_cfg_init_mem_wide(base + 8, base, 8, base + 16)`) |
| virtio-mmio transports | `0x0A000000` | 32 × `0x200` | slot *i* at `0x0A000000 + i*0x200`, same as `virt` (hw/arm/virt.c:1052-1090) |
| loader payload | `0xF0000000` | payload size | written by `-device loader`, see §6 |
| RAM | `0x40000000` | `-m` bytes | 12 GiB ⇒ `0x40000000..0x340000000`; 7 GiB ⇒ `..0x200000000` |
| DTB | `0x40000000` | ≤ 512 KiB | inside RAM at its base, written before the guest starts |
| stack (fixture mode only) | RAM top − 8 MiB | 8 MiB | P1's `-kernel` fixtures; not used in firmware mode |

Ordering matters: the pflash banks are below the GIC, the GIC and UART sit in the first 256 MiB of
the address space, and RAM starts at 1 GiB exactly as on `virt`. Keeping the same map is what makes
the xnuloader's compiled platform table (`gic_dist 0x08000000`, `gic_redist 0x080A0000`,
`uart 0x09000000`) work unchanged.

### The DTB-vs-RAM-base conflict

EDK2 reads the device tree from the fixed address `0x40000000`
(`PcdDeviceTreeInitialBaseAddress`, ArmVirtQemu.dsc:204) and separately requires the `/memory`
node's base to equal `0x40000000` (QemuVirtMemInfoPeiLibConstructor.c:33-89). So the DTB cannot be
moved to a "safe" address outside RAM without breaking the firmware, and RAM cannot start anywhere
else without breaking the memory assertion.

`virt` resolves this by writing the FDT into the first bytes of RAM
(`virt_machine_done()` → `arm_load_dtb(info->dtb_start, ...)`, hw/arm/virt.c:1668-1686) and
letting EDK2 consume it during PEI, before general-purpose memory allocation starts. `-M xlt`
must do the same:

1. build the FDT after RAM and devices exist (machine init), with `qemu_fdt_*` helpers;
2. write it to `0x40000000` (RAM base) and keep the total size ≤ 512 KiB;
3. start the guest only afterwards — starting from `accel_setup_post` guarantees this ordering
   (system/vl.c:2621 machine init → 2713 CLI devices realize → 3764 `accel_setup_post`);
4. never place machine-owned data (payload copies, framebuffer, stacks) in the first 512 KiB of
   RAM; everything the machine itself needs goes above, e.g. the loader payload at `0xF0000000`
   and the fixture stack at the top of RAM.

Where EDK2 moves its working copy of the DTB is an EDK2 internal; xlt only has to honour the
placement contract above. (Unverified detail, not load-bearing: whether ArmVirt PrePi copies the
DTB or the firmware simply never reuses the first pages. The empirical evidence is that `virt`
boots with the DTB inside RAM, and so do the reduced-DT runs T2/T3.)

## 2. Device wiring

| Device | MMIO | IRQ (DT form) | INTID | GIC line (QEMU convention) |
|---|---|---|---|---|
| PL011 | `0x09000000` 4 KiB | `<0 1 4>` (SPI 1, level-high) | 33 | line 1 (SPI index) |
| virtio-mmio slot *i* | `0x0A000000 + i*0x200` | `<0 16+i 1>` (SPI 16+i, edge) | 48+i | line 16+i |
| generic timer, secure EL1 physical | — | `<1 13 4>` (PPI 13) | 29 | `(num_irq-32) + 29` |
| generic timer, NS EL1 physical | — | `<1 14 4>` (PPI 14) | 30 | `(num_irq-32) + 30` |
| generic timer, virtual | — | `<1 11 4>` (PPI 11) | 27 | `(num_irq-32) + 27` |
| generic timer, NS EL2 physical | — | `<1 10 4>` (PPI 10) | 26 | `(num_irq-32) + 26` |

Notes:

* DT `interrupts` cells are `<type number flags>`: type 0 = SPI (INTID = number + 32), type 1 = PPI
  (INTID = number + 16); flags 4 = level-high, 1 = edge-rising. This is the standard GIC binding
  and matches QEMU's own nodes (hw/arm/virt.c:862-905 and 1052-1120).
* Inside QEMU, GIC input lines are **not** INTIDs: `gicv3_set_irq()` maps line *L* <
  `num_irq - 32` to SPI `L + 32`, and the remaining lines to per-CPU PPIs (hw/intc/arm_gicv3.c:324-350).
  So a device is connected with the SPI index (UART → line 1), while the DT records SPI 1 for it.
  xlt's own GIC model should keep INTIDs internally and translate at the edge.
* The four timer PPIs are exactly what EDK2's `ArmVirtTimerFdtClientLib` expects: it reads four
  `<type number flags>` triples and sets `PcdArmArchTimerSecIntrNum/IntrNum/VirtIntrNum/HypIntrNum`
  (ArmVirtPkg/Library/ArmVirtTimerFdtClientLib/ArmVirtTimerFdtClientLib.c:46-104). The virtual timer
  is **PPI 27**, not 30 — PPI 30 is the non-secure EL1 physical timer. (The task brief's "PPI 30
  virtual timer" is off by three; keep the mapping in the table above.)
* PSCI needs no interrupt. fw_cfg needs no interrupt.

### pflash properties

Both banks are `cfi.pflash01` devices, created and mapped exactly as `virt` does
(hw/arm/virt.c:1119-1187):

```c
DeviceState *dev = qdev_new(TYPE_PFLASH_CFI01);
qdev_prop_set_uint64(dev, "sector-length", 256 * KiB);   /* VIRT_FLASH_SECTOR_SIZE */
qdev_prop_set_uint8 (dev, "width", 4);
qdev_prop_set_uint8 (dev, "device-width", 2);
qdev_prop_set_bit (dev, "big-endian", false);
qdev_prop_set_uint16(dev, "id0", 0x89);
qdev_prop_set_uint16(dev, "id1", 0x18);
qdev_prop_set_uint16(dev, "id2", 0x00);
qdev_prop_set_uint16(dev, "id3", 0x00);
qdev_prop_set_string(dev, "name", "xlt.flash0");         /* or virt.flash0 */
qdev_prop_set_uint32(dev, "num-blocks", bank_size / (256 * KiB));
object_property_add_child(OBJECT(machine), name, OBJECT(dev));
object_property_add_alias(OBJECT(machine), "pflash0", OBJECT(dev), "drive");
```

with `bank_size = 64 MiB` → `num-blocks = 256`, bank 0 mapped at `0x0` and bank 1 at
`0x04000000` (nonsecure address space). The `pflash0`/`pflash1` aliases are what makes
`-drive if=pflash` attach; without them the drives are rejected.

**Correction to the brief:** `virt` uses 256 KiB sectors, not 64 KiB. EDK2's `NorFlashQemuLib`
hard-codes `QEMU_NOR_BLOCK_SIZE` to 256 KiB and builds the `VirtNorFlash` device from the DT node,
so the machine and the firmware must agree. Use 256 KiB; the DT node (`bank-width = 4`, both banks
in one `reg`) is unchanged by this choice.

The firmware code bank must be mapped **read-only**; QEMU's pflash honours the drive's `readonly`
flag, and EDK2 will not touch it. The vars bank is writable; a fresh copy of `AAVMF_VARS.fd` is
mandatory for reproducible tests (§8).

## 3. fw_cfg and display

fw_cfg is created unconditionally during machine init, like `virt`:

```c
FWCfgState *fw_cfg = fw_cfg_init_mem_wide(base + 8, base, 8, base + 16,
                                          &address_space_memory);
fw_cfg_add_i16(fw_cfg, FW_CFG_NB_CPUS, 1);
```

with `base = 0x09020000` (data at +0, selector at +8, 8-byte data port, DMA at +16). This is
cheap, matches `virt`, and makes the display path work later without touching the machine:

* QEMU's `ramfb` device finds the fw_cfg through `fw_cfg_find()`, requires DMA to be enabled, and
  publishes the `etc/ramfb` file (hw/display/ramfb.c:140-155). `-device ramfb` therefore attaches
  to `-M xlt` with no additional machine code.
* EDK2's `QemuRamfbDxe` turns that file into a GOP (OvmfPkg/QemuRamfbDxe/QemuRamfb.c:255-260), and
  the xnuloader writes its own 1125×2436 configuration through the same file.
* P2.4's acceptance run is `-display none` + serial. If `-device ramfb` is absent the loader prints
  `no display (QEMU without -device ramfb)` and the kernel still boots; SpringBoard rendering is a
  P2.5 concern (screenshot parity).

Do not add the fw_cfg DT node unless it is wanted for cosmetic parity with `virt`; EDK2's
`QemuFwCfgLib` uses the fixed MMIO base, and the reduced-DT boots in FDT-NOTES §4 ran without it.

## 4. Boot handoff

The machine starts the guest from `AccelClass::setup_post` (P1 behaviour, kept), i.e. after machine
init and after all `-device` options have realized. That ordering is load-bearing; see §6.

### CPU reset state for the firmware path

There is no `ARMCPU` in xlt, so the machine itself must present the state a real reset would:

| State | Value | Why |
|---|---|---|
| `PC` | `0x00000000` | reset vector in pflash bank 0; EDK2's entry point |
| `PSTATE` | `EL1h`, `DAIF` all masked, `SPSel=1` | reset EL; `virt` with no EL3/EL2 starts at EL1h |
| `MMU` | off (`SCTLR_EL1.M = 0`) | firmware enables it itself |
| `x0` | `0` | `virt` does **not** pass the DTB in x0; EDK2 reads the fixed address. Leave 0 (a future firmware following the arm64 boot protocol may expect `x0 = 0x40000000`, but EDK2 2024.02 does not) |
| `VBAR_EL1` | `0` | firmware installs its own vectors |
| `CNTFRQ_EL0` | `62500000` | 62.5 MHz, matches `virt` and QEMU's virtual-clock scaling |

With `-kernel <elf>` (fixture mode) the machine keeps P1's semantics instead: load the ELF, set
`PC` to its entry, put the stack at the top of RAM, and start from `setup_post`. `-kernel` is not
combined with pflash in P2.4; if both are given, fail with a clear message rather than silently
picking one (see FDT-NOTES §3 for what `virt` does with fw_cfg in that case).

### `-bios`

Do not implement `-bios` as a RAM load at `0x0`: bank 0 is a pflash device and EDK2 needs its CFI
semantics (and the `/flash` node) to keep variables. If a `-bios` convenience is wanted, map it to
"load this file into pflash bank 0's backing store" and document it; the supported contract is and
stays `-drive if=pflash`.

### `-device loader`

`-device loader,file=payload.bin,addr=0xF0000000,force-raw=on` is a `TYPE_LOADER` device that
registers a ROM blob; the blob is written into guest RAM by `rom_reset()` with
`address_space_write_rom()` (hw/core/loader.c:1250-1300) as part of the initial machine reset. No
CPU object is involved, so nothing extra is needed on a CPU-less machine except that `0xF0000000`
must be inside RAM — which it is for every `-m ≥ 7G` (`0x40000000 + 7 GiB = 0x200000000`). The
machine must not reserve the payload area or copy over it afterwards.

## 5. Device tree for `-M xlt`

Build the tree in this order with the `qemu_fdt_*` helpers (`include/sysemu/device_tree.h`); the
values are the ones the reduced-DT experiments validated. `RAMSIZE` is the `-m` value and
`RAMEND = 0x40000000 + RAMSIZE`.

```dts
/ {
    compatible = "linux,dummy-virt";
    #address-cells = <0x2>;
    #size-cells = <0x2>;

    psci {
        compatible = "arm,psci-1.0", "arm,psci-0.2", "arm,psci";
        method = "hvc";
        cpu_suspend = <0xc4000001>;
        cpu_off     = <0x84000002>;
        cpu_on      = <0xc4000003>;
        migrate     = <0xc4000005>;
    };

    memory@40000000 {
        device_type = "memory";
        reg = <0x0 0x40000000 RAMSIZE_HI RAMSIZE_LO>;   /* one node, even across 4 GiB */
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
    };

    intc@8000000 {
        compatible = "arm,gic-v3";
        #interrupt-cells = <0x3>;
        interrupt-controller;
        #address-cells = <0x2>;
        #size-cells = <0x2>;
        reg = <0x0 0x08000000 0x0 0x10000>,
              <0x0 0x080a0000 0x0 0x20000>;   /* 128 KiB, one vCPU */
        #redistributor-regions = <0x1>;
    };

    pl011@9000000 {
        compatible = "arm,pl011", "arm,primecell";
        reg = <0x0 0x09000000 0x0 0x1000>;
        interrupts = <0x0 0x1 0x4>;
    };

    flash@0 {
        compatible = "cfi-flash";
        reg = <0x0 0x0 0x0 0x4000000>,
              <0x0 0x4000000 0x0 0x4000000>;
        bank-width = <0x4>;
    };

    virtio_mmio@a000000 {        /* one node per transport, 0..31 */
        compatible = "virtio,mmio";
        reg = <0x0 0xa000000 0x0 0x200>;
        interrupts = <0x0 0x30 0x1>;    /* SPI 16 = INTID 48, edge-rising */
        dma-coherent;
    };

    chosen {
        stdout-path = "/pl011@9000000";
    };
};
```

Mandatory for the payload path (each one is read by an EDK2 driver; see FDT-NOTES §2):
`/memory`, `/intc`, `/timer`, `/pl011` (+ `/chosen/stdout-path`), `/flash`, `/psci` with
`arm,psci-0.2` and `method = "hvc"`, and at least the `/virtio_mmio` node that carries the ESP.

Optional / omitted on purpose: `/pmu`, `/its`, `/platform-bus`, `/pl031`, `/apb-pclk`,
`/chosen/bootargs`, `cpu-map`. The reduced-DT boots in FDT-NOTES §4 removed all of them without
losing the payload. `/chosen/bootargs` in particular is not consumed by the xnuloader — it builds
XNU's command line itself — so emit it only if `-append` is given, and do not rely on it.

`/chosen/stdout-path` is not decorative: `FdtSerialPortAddressLib` uses it to choose the serial
node (OvmfPkg/Library/FdtSerialPortAddressLib/FdtSerialPortAddressLib.c:168), and without it the
firmware has no console.

## 6. PSCI

The machine answers ``HVC`` calls from EL1 with the ARM PSCI 0.2/1.1 encodings (see ARM-DETAILS §3
for the conventions):

| Function | ID | Answer |
|---|---|---|
| `PSCI_VERSION` | `0x84000000` | `0x00010001` (1.1) |
| `PSCI_FEATURES` | `0x8400000A` | `0` for VERSION/SYSTEM_OFF/SYSTEM_RESET/CPU_ON/AFFINITY_INFO, `-1` otherwise |
| `SYSTEM_OFF` | `0x84000008` | machine exit, code 0; never returns |
| `SYSTEM_RESET` | `0x84000009` | machine reset/exit, code 0; never returns |
| `CPU_ON` | `0xC4000003` | `-4` (`ALREADY_ON`) for MPIDR 0, `-2` (`INVALID_PARAMS`) otherwise |
| `AFFINITY_INFO` | `0xC4000004` | `0` (ON) for MPIDR 0 / level 0, `-2` otherwise |
| `MIGRATE_INFO_TYPE` | `0x84000006` | `2` (no trusted OS); not required by the EDK2 sources cited here — answer it for completeness (hypothesis: firmware that probes migration support expects `2`) |

Both the 32-bit and the 64-bit forms must be accepted: EDK2's reset library sends the 32-bit
`SYSTEM_RESET`/`SYSTEM_OFF` IDs, while the loader and (hypothesis) XNU use the 64-bit
`CPU_ON`/`AFFINITY_INFO` forms. Unknown IDs return `-1` and must not crash the machine.

## 7. Test procedure

Run every step with a **fresh copy** of `AAVMF_VARS.fd`. A reused vars file keeps a boot order from
a previous device topology and silently falls back to the UEFI Shell; that produced false negatives
during the t26 research and will do the same here.

(a) **EDK2 boots with the reduced DT, virt + TCG.**

```
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/vars.fd
qemu-system-aarch64 -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m 7G \
  -accel tcg,tb-size=384 -dtb /tmp/xlt-m24.dtb \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/vars.fd \
  -drive file=fat:rw:/tmp/esp,format=raw,if=none,id=esp0 \
  -device virtio-blk-device,drive=esp0 \
  -device loader,file=/tmp/payload.bin,addr=0xF0000000,force-raw=on \
  -display none -serial file:/tmp/serial.log -monitor none -no-reboot
```

Pass when the log shows the UEFI banner and the mapping table with `FS0:` (or the loader); fail if
it drops to `Shell>` with `map: No mapping found` (that means the DT lost the virtio transport or
`/pcie` was needed, i.e. the ESP was not virtio-mmio).

(b) **The same DT boots the payload.** Add `-device ramfb` only in P2.5. Pass when the log contains
`XNULOADER` and `Darwin Kernel Version` (the five-markers list in PLAN.md §5 is the P2.5 target).

(c) **Parity checklist for `-M xlt` once the machine exists.** Compare against a `virt` run with the
same payload, RAM and devices: same UEFI boot path, same `FS0:`/disk discovery, same loader output
up to the first XNU print, serial output byte-identical up to that point, exit code equal for
`SYSTEM_OFF`. Record any divergence in the order it appears.

## 8. ESP transport decision

**Decision:** the ESP is virtio-mmio. The `-M xlt` recipe attaches it with
`-drive file=fat:rw:esp,format=raw,if=none,id=esp0 -device virtio-blk-device,drive=esp0`.

Evidence: with a reduced DT (no `/pcie`), `-M virt` cannot see a `-drive if=virtio` disk (UEFI
Shell, no mapping), but boots the same payload when the disk is virtio-mmio (FDT-NOTES §4, T1 vs
T2/T3). `if=virtio` binds to virtio-pci on `virt`; xlt has no PCIe model.

If PCIe is ever needed (e.g. a future virtio-gpu-pci or NVMe path with the stock recipe), it would
drag in: an ECAM host bridge (low ECAM `0x10000000`, moving to `0x4010000000` once RAM passes
3 GiB), the PCIe memory window and PIO window, legacy INTx routing or a `msi-map` + ITS model for
MSI, `pci-host-ecam-generic` DT node, and a virtio-pci device model on top. That is a P3-scale
change and is **out of scope for P2.4**; the project recipe for `-M xlt` must use the virtio-mmio
form until then.

## 9. Three things most likely to be wrong

1. **DTB placement vs RAM.** Writing the FDT after the guest starts, above RAM, or at an address
   other than `0x40000000` breaks EDK2 twice (header read and memory-base assertion). Build it in
   machine init, write it at RAM base, and start the guest only in `setup_post`.
2. **pflash details.** 256 KiB sectors (not 64 KiB), `width 4`, `device-width 2`, `num-blocks =
   bank_size / 256 KiB`, the four id bytes, and the `pflash0`/`pflash1` object aliases. Getting the
   aliases wrong makes `-drive if=pflash` fail at startup; getting the sector size wrong makes
   EDK2's `VirtNorFlash` disagree with the device and corrupt variables.
3. **Interrupt numbering.** The DT uses SPI/PPI numbers with the type cell, the GIC's internal line
   numbering is different, and the virtual timer is PPI 27 (not 30). A wrong `interrupts` cell or a
   missing `/chosen/stdout-path` shows up as "firmware runs but nothing works" rather than a clear
   error.

Acceptance for P2.4 itself, beyond the payload: `make test-open` must not depend on building QEMU
(INTEGRATION.md rule); the machine must still boot the P1 fixtures unchanged under `-M xlt -accel
xlt`; and the `-M xlt` help string should name the firmware recipe.

## 10. References

* QEMU 8.2.2: `hw/arm/virt.c` (memmap 136-167, GIC node 536-612, timer 323-379, PL011 862-905,
  virtio 1052-1120, pflash 1119-1187, fw_cfg 1283-1302, machine_done 1668-1686, bootinfo
  2329-2336), `hw/arm/boot.c` (firmware path 1145-1200, PSCI node 435-508, memory node 412-437),
  `hw/core/loader.c` (rom_reset 1250-1300), `system/vl.c` (ordering 2621/2713/3764),
  `hw/intc/arm_gicv3.c` (input-line convention 324-350), `include/hw/intc/arm_gicv3_common.h:45`
  (`GICV3_REDIST_SIZE 0x20000`).
* EDK2 `edk2-stable202402`: `ArmVirtQemu.dsc:204` (DTB address), `QemuVirtMemInfoPeiLibConstructor.c`
  (memory node asserts), `ArmVirtTimerFdtClientLib.c`, `ArmVirtGicArchLib.c`,
  `ArmVirtPsciResetSystemLib.c`, `NorFlashQemuLib.c`, `VirtioFdtDxe.c`,
  `FdtSerialPortAddressLib.c`, `QemuRamfbDxe/QemuRamfb.c`.
* Local: [PLAN.md](PLAN.md) phases P2.4/P2.5, [FDT-NOTES.md](FDT-NOTES.md) experiments and
  per-node table, [ARM-DETAILS.md](ARM-DETAILS.md) GIC/timer/PSCI encodings,
  [../INTEGRATION.md](../INTEGRATION.md) accelerator hooks, `xlt/qemu/p1/NOTES.md` for the current
  machine, `xnuloader/loader.c` (payload magic, platform table, ramfb path) in the XNU tree.

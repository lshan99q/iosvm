# P2.5 acceptance: booting the real payload on `-M xlt -accel xlt`

This is the checklist a verifier follows to accept — or reject — the first boot of the real
payload (EDK2 → xnuloader → XNU → SpringBoard) on the custom machine. It is derived from
[PLAN.md](PLAN.md) §5, [MACHINE-SPEC.md](MACHINE-SPEC.md) and [FDT-NOTES.md](FDT-NOTES.md); it
does not add requirements, it makes the existing ones runnable and evidence-producing.

Scope: P2.5 only. P2.4's exit gate is narrower (the boot reaches `== XNULOADER v3 ==` or stops
with a precise documented blocker), and P2.3's interrupt/timer oracles are regression inputs here,
not part of this acceptance.

## 0. What "accepted" means

All of the following, on one machine, with one command line and a fresh variable store:

1. the five serial markers appear in the same order as the TCG baseline;
2. a screenshot of the guest frame is byte-identical (sha256) to the TCG capture;
3. the image md5 list is unchanged;
4. cold boot wall time is ≤ the TCG baseline, same host load, median of three runs;
5. zero guest panics, zero QEMU assertions/aborts, `-smp 1` throughout.

Anything else — frame rate, cache behaviour, SMP — is reported but not gated until P3/P4.

## 1. Boot markers and expected serial lines

The markers are the ones the TCG harness already uses (`tools/qemu-open.sh:166-174`):

| # | Marker | Emitted by | Meaning |
|---|---|---|---|
| 1 | `== XNULOADER v3 ==` | xnuloader | firmware ran the ESP's `BOOTAA64.EFI` and the loader started |
| 2 | `Darwin Kernel Version` | XNU | the loader built `boot_args`, jumped, and the kernel's early console is up |
| 3 | `vm_page_bootstrap` | XNU | kernel memory management initialised |
| 4 | `MAC Framework successfully` | XNU | security framework init passed (late boot) |
| 5 | `initproc failed to start` | XNU/launchd | userspace reached the point the port's baseline records |

For a full P2.5 diff, the log must additionally contain the EDK2 and loader preamble in the same
order as the baseline. Expected sequence, from QEMU start:

```text
UEFI firmware (version ... )          # EDK2 banner on the PL011
BdsDxe: loading Boot....              # boot manager selects the ESP's BOOTAA64.EFI
BdsDxe: starting ...                  # (a fresh vars store discovers \EFI\BOOT\BOOTAA64.EFI)
<loader output, begins == XNULOADER v3 ==>
<Darwin banner, then the kernel's boot lines>
```

The exact text between those anchors is not part of the gate; the anchors and their order are.
Keep the baseline log under `reports/` (or the harness's `logs/`) and diff against it with the
anchor script below rather than eyeballing.

```sh
# markers, in order; exits 0 only if all five appear exactly once and ordered
last=0
for m in "== XNULOADER v3 ==" "Darwin Kernel Version" "vm_page_bootstrap" \
         "MAC Framework successfully" "initproc failed to start"; do
  n=$(grep -cF -- "$m" "$LOG")
  [ "$n" -ge 1 ] || { echo "MISSING: $m"; exit 1; }
  ln=$(grep -nF -- "$m" "$LOG" | head -1 | cut -d: -f1)
  [ "$ln" -gt "$last" ] || { echo "OUT OF ORDER: $m"; exit 1; }
  last=$ln
done
echo "markers: 5/5 in order"
```

**TODO(baseline):** exp's formal boot baseline (full serial log, its sha256, and the exact
per-line expectations) is not in the tree yet. When it lands, replace the "preamble" paragraph
above with a link to it and add its sha256 next to each run under test; until then the marker
check above plus the five-marker order is the gate.

### Timing measurement

Wall clock from `qemu-system-aarch64` start to the moment the last marker is flushed (or to the
screenshot, whichever the team standardises). Record `uptime`/load average for each run. Use the
median of three; a single run is not a measurement on a shared host.

| Metric | TCG baseline | P2.5 requirement |
|---|---|---|
| cold boot to the 5th marker | ≈8 min (`docs/gpu-acceleration-stage13-plan.md:29`) | ≤ baseline, median of 3, same load band |
| stretch target | — | < 50% of baseline (≤4 min) |
| boot to the loader marker | not recorded yet | record it; it isolates firmware+loader cost from XNU |
| steady-state fps (reported, not gated) | 13.9 fps / 75 ms at load 5.5–8; 11.7 fps / 87 ms at load 10–15.5 | ≥ 13.9 fps at comparable load, zero panics (frame pacing is P3) |

The TCG baseline must be re-measured on the same host and load band as the `-M xlt` run; the
recorded 8 min is the reference, not a substitute for a paired run. Record the kernel's own
timestamps too (`[ <sec> ]:` lines) so the comparison does not depend on host scheduling.

## 2. Screenshot and scanout parity

Two capture modes exist and they mean different things:

* **Serial-only acceptance (`-display none`)** — the mode P2.4/P2.5 use until ramfb is enabled.
  It gates the five markers and boot time; it cannot check the frame.
* **ramfb mode (`-device ramfb` + fw_cfg)** — the mode P2.5 needs for screenshot parity. The
  machine spec already creates fw_cfg with DMA (MACHINE-SPEC §3), so `-device ramfb` attaches
  without machine changes; EDK2's `QemuRamfbDxe` exposes a GOP and the loader repoints it at
  1125×2436 XR24.

Capture with the existing harness, not by hand:

```sh
# start QEMU with a monitor socket (the run-darwin recipe uses -monitor tcp:127.0.0.1:4444+10n)
qemu-system-aarch64 ... -monitor tcp:127.0.0.1:4454,server,nowait ...
tools/screendump.sh -o shots/xlt-p25.png --ppm /tmp
```

**Parity definition.** Byte-for-byte: same PNG dimensions, same pixel bytes, same sha256 as the
TCG capture taken from the same guest state (same marker crossed, same wait time). This is the
gate; the repo has done screenshot sha256 equivalence before (docs/handover.md). A semantic check
(same size, identical non-black pixel fraction within 0.1%, mean absolute difference < 1 LSB) is a
*diagnostic* for narrowing a diff, never a substitute for the byte comparison.

Before capturing, verify the scanout path: the loader must not print `no display (QEMU without
-device ramfb)`, and the QEMU console must not warn about a missing ramfb/fw_cfg file. If either
appears, the frame is black or stale and parity is meaningless.

## 3. Device checklist

Each row is a thing the machine must have for the payload; "verify" is the observable that proves
it on a real run.

| Device | Address / IRQ | Verify |
|---|---|---|
| pflash code bank | `0x0`, 64 MiB, 256 KiB sectors | EDK2 banner appears at all; without it the CPU executes nothing |
| pflash vars bank | `0x04000000`, 64 MiB | EDK2 boot menu stores/keeps a boot entry; a second boot with the same vars file skips rediscovery |
| GICv3 distributor | `0x08000000` + 64 KiB | firmware proceeds past `ArmGicDxe`; no GIC assert |
| GICv3 redistributor | `0x080A0000` + 128 KiB (1 vCPU) | ICC_* sysregs present; `ArmGicArchLib` does not error |
| PL011 | `0x09000000`, SPI 1 → INTID 33 | all serial output; RX needed only when the guest reads input |
| generic timer | PPIs 26/27/29/30 | XNU's timer calibration and the `[ <sec> ]:` timestamps advance |
| virtio-mmio ESP | `0x0A000000 + 0x200*i`, SPI 16+i → INTID 48+i | `FS0:` in EDK2's mapping table, then `BOOTAA64.EFI` starts |
| payload loader | RAM at `0xF0000000` | loader finds the `XNUP` payload; without it prints its "no payload" error |
| fw_cfg + ramfb | `0x09020000`, fw_cfg DMA | GOP installed, `-device ramfb` window shows the frame, screenshot non-black |
| PSCI (HVC) | n/a | reset/shutdown behave; no `unknown PSCI` panic |

The IRQ numbers are the DT values from MACHINE-SPEC §2; the GIC's internal line indices differ and
are not what this checklist verifies.

## 4. Regression gate (run before declaring P2.5)

All of these must pass on the candidate build, in this order:

| Check | Command | Expected |
|---|---|---|
| M1 engine | `make -C xlt clean && make -C xlt && make -C xlt test` | build ok; decode tests 481+ checks; no new warnings |
| M1 benchmark | `xlt/bench/build.sh`, `xlt/bench/run.sh` | 5/5 checksums, overall ≈10× vs TCG |
| P1 accelerator | `xlt/qemu/p1/run.sh` | `run.sh: PASS`; fixtures `xlt-p1-fixture*` byte-identical |
| P2.1 MMU | `xlt/qemu/p2/run.sh` | `p2-mmu: TCG/xlt identical (259 bytes)` |
| P2.1 semihosting | same | `p2-semihost: ... (19 bytes)` |
| P2.2 exceptions | same | `p2-exc: ... (419 bytes)`, sha256 `b9eef314…` for the xlt output too |
| P2.3 oracles | same script once the fixtures land | **TODO(P2.3):** fill in fixture name, byte count and sha256 when the GIC/timer oracle is committed |
| fixtures under `-M xlt` | p1 + p2 runs | exit 0, no QEMU assertion, `-M xlt` alone still rc 1 |
| provenance | `python3 tools/provenance-check.py --root .` | clean on tracked files; no new SPDX gaps |
| links | `python3 tools/check-links.py` | no new FAIL lines from touched files (repo baseline is already red) |
| formatting | `clang-format --dry-run --Werror` on changed C/C++, `bash -n` on changed scripts | clean |

If any oracle changes, the acceptance entry is "no": fix the engine's behaviour, do not update the
oracle to match, unless the oracle itself is being corrected — then record old/new bytes and the
reason in the report.

## 5. Failure triage

Symptom → first suspicion → first check. This table assumes the `-M xlt` machine from
MACHINE-SPEC and the virtio-mmio ESP (MACHINE-SPEC §8).

| Symptom | Likely cause | Check |
|---|---|---|
| No serial output at all | PL011 not wired to the chardev, wrong PC, or the guest never starts | QEMU starts? any output on `-serial stdio`? PC/exception dump from the accel |
| EDK2 banner then hang with no further output | DTB missing/invalid at `0x40000000`; EDK2 asserts on `fdt_check_header` | dump the first 64 KiB of RAM at `0x40000000`; compare `fdt_totalsize`; check the FDT magic |
| EDK2 runs, then `Shell>` with `map: No mapping found` | ESP not virtio-mmio, or `/virtio_mmio` nodes missing/mismatched | the DT node regs vs the transport addresses; the `-device virtio-blk-device` form |
| EDK2 sees no boot option with a fresh vars file | ESP has no `\EFI\BOOT\BOOTAA64.EFI` | list the ESP contents; the loader copy step |
| EDK2 boots an old entry or falls back to the Shell with a reused vars file | stale NVRAM boot order | refresh `AAVMF_VARS.fd`; never reuse across device-topology changes |
| `XNULOADER` never appears but the ESP is found | wrong file on the ESP (not the loader) or a loader crash | compare the ESP's `BOOTAA64.EFI` sha256 with the loader build |
| Loader prints a payload/`XNUP` error | payload absent at `0xF0000000`, or RAM does not cover `0xF0000000` | `-m` size; the `-device loader,...,addr=` value; RAM end ≥ `0xF0000000` |
| Kernel banner then panic with an ESR/ELR dump | an unimplemented instruction or a wrong syndrome/vector | map the ESR/EC to ARM-DETAILS §2; check the faulting instruction against the lifter's coverage |
| Kernel boots, then hangs before `initproc` | timer/IRQ not delivered, or PSCI CPU_ON mismatch | PPI 27/30 delivery, DAIF state, the loader's `psci-method`, the HVC answers in MACHINE-SPEC §6 |
| QEMU aborts/asserts during boot | machine bug (e.g. ROM/MMIO overlap, pflash aliases, buffer overrun) | QEMU's stderr and the assertion line; reproduce with `-d guest_errors,int` |
| Display black or stale | ramfb/fw_cfg missing, or GOP absent | loader's `no display` line; fw_cfg DMA enabled; `-device ramfb` present |
| Boot time ≫ TCG | unbounded engine loop, missing fast path, or a trap storm | slice timing/exit counters; compare with the P2.4 profiling run |

Record the symptom, the first failing marker and the evidence in the report; "it hangs" is not a
finding.

## 6. Reproduction and evidence

A verifier reproduces the run with the command below (values from MACHINE-SPEC §7 and the project
recipe; substitute the payload paths used by the build):

```sh
cp /usr/share/AAVMF/AAVMF_VARS.fd /tmp/vars.fd     # fresh every time
QEMU=/home/lshan/xlt/qemu-8.2.2-xlt-build/qemu-system-aarch64
$QEMU -M xlt -accel xlt -cpu cortex-a76 -smp 1 -m 12G \
  -drive if=pflash,format=raw,readonly=on,file=/usr/share/AAVMF/AAVMF_CODE.fd \
  -drive if=pflash,format=raw,file=/tmp/vars.fd \
  -drive file=fat:rw:/tmp/esp,format=raw,if=none,id=esp0 \
  -device virtio-blk-device,drive=esp0 \
  -device loader,file=/tmp/payload.bin,addr=0xF0000000,force-raw=on \
  -device ramfb -display none -serial file:/tmp/p25.log \
  -monitor tcp:127.0.0.1:4454,server,nowait -no-reboot
```

Evidence to attach to the acceptance report:

* `git rev-parse HEAD`, the two patch files' sha256, and the QEMU binary's size/sha256;
* the full serial log, plus the marker script's output;
* paired TCG and xlt timings (three runs each) with load averages;
* the TCG and xlt screenshots (or their sha256s) and the dimensions;
* the image md5 list before/after (unchanged expected);
* the regression table from §4 with pass/fail per row;
* any deviation from MACHINE-SPEC/FDT-NOTES, with the observed behaviour that justified it.

`-smp 1` is part of the acceptance, not a convenience: multi-core XNU panics and P4 owns SMP. A run
with `-smp 2` is a diagnostic, never an acceptance run.

## 7. Sign-off

| Item | Owner | Result |
|---|---|---|
| machine + devices per MACHINE-SPEC | code | ☐ |
| markers 5/5 in order | verifier | ☐ |
| boot time ≤ TCG (median 3, same load) | verifier | ☐ |
| screenshot sha256 equal | verifier | ☐ |
| image md5 unchanged | verifier | ☐ |
| regression gate §4 green | verifier | ☐ |
| evidence attached per §6 | verifier | ☐ |

Verdict is PASS only when every box is ticked; otherwise it is a documented blocker with the
triage row that applies and the next experiment.

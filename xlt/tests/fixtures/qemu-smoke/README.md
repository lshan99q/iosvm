# QEMU P1 smoke payload

A tiny freestanding AArch64 image that prints one line on the `virt` machine's PL011 UART and
powers the machine off through PSCI. It is the unpatched-QEMU baseline for the M2 accelerator
(`-accel tcg` today, `-accel xlt` later): same image, same command line, only the accelerator
changes.

| file | what |
| --- | --- |
| `start.S` | `_start` at `0x40080000`: byte-by-byte PL011 output, then `PSCI SYSTEM_OFF` via `hvc #0` |
| `link.ld` | links the payload at the start of guest RAM (`0x40080000`), discards unwind/note sections |
| `build.sh` | builds `xlt/build/qemu-smoke/qemu-smoke.elf` (gitignored output, nothing written here) |

## Build

```
xlt/qemu/build-qemu.sh                 # once: upstream QEMU 8.2.2, out of tree
xlt/tests/fixtures/qemu-smoke/build.sh # the payload
```

`build-qemu.sh` downloads and verifies `qemu-8.2.2.tar.xz`, builds `qemu-system-aarch64` under
`/home/lshan/xlt` (override with `QEMU_WORK_DIR`, jobs with `QEMU_JOBS`) and prints the binary
path. If `https_proxy` is set it is used for the download.

The payload is assembled with:

```
clang --target=aarch64-unknown-elf -O2 -nostdlib -ffreestanding -fuse-ld=lld \
      -Wl,-T,link.ld -Wl,--build-id=none start.S -o xlt/build/qemu-smoke/qemu-smoke.elf
```

## The exact TCG baseline command

From the repository root (`/home/lshan/iosvm&o`), with the QEMU build produced by
`xlt/qemu/build-qemu.sh`:

```
QEMU=/home/lshan/xlt/qemu-8.2.2-build/qemu-system-aarch64
"$QEMU" -M virt -cpu cortex-a76 -smp 1 -m 256M -accel tcg \
    -kernel xlt/build/qemu-smoke/qemu-smoke.elf \
    -display none -serial stdio -monitor none -no-reboot < /dev/null
```

Captured result (QEMU 8.2.2, this host):

```
stdout: xlt-p1-ok
stderr: (empty)
exit code: 0
```

The payload writes each byte of `xlt-p1-ok\n` to UARTDR (`0x09000000`) after polling UARTFR.TXFF
(bit 5), then calls `PSCI SYSTEM_OFF` (`0x84000008`) through the HVC conduit that `virt` uses for
`-kernel` boots. `-no-reboot` makes the shutdown end QEMU with status 0; `-serial stdio` keeps the
UART on the terminal and `-monitor none` keeps the QEMU monitor off that same terminal.

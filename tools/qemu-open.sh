#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# qemu-open.sh — boot an XNU built from open source under QEMU with **nothing of Apple's** in the
# payload, and assert that it reached BSD initialisation. This is the M0 smoke test that needs no IPSW.
#
#   tools/qemu-open.sh --help
#   tools/qemu-open.sh --hakios ~/src/hakios            # boot and print the log tail
#   tools/qemu-open.sh --hakios ~/src/hakios --check    # exit 0 only if the markers are present
#
# The kernel is not in this repository and never will be (16 MB of it), so this script takes a
# checkout of the upstream project that builds it. Everything it boots is open source; the
# ramdisk is a 16 KB freestanding "hello" that has no Apple code in it either.
#
# What "pass" means: the loader ran, the kernel printed its Darwin banner, paged memory came up,
# and the BSD half got far enough to try to exec pid 1. It then panics on launchd, because the
# payload has no user space — **that panic is the expected outcome**, and this script asserts it
# rather than treating it as failure. A boot with a real userland is a different test (M1).
#
# Requires: qemu-system-aarch64, an aarch64 UEFI firmware, and an upstream checkout.
# Exit codes: 0 reached BSD init, 1 did not, 2 usage/tooling error, 3 missing upstream checkout.

set -euo pipefail

PROG=${0##*/}
HAKIOS=${HAKIOS:-}
SECONDS_RUN=${SECONDS_RUN:-120}
MEM=${QEMU_MEM:-6G}
CHECK=0
WORK=${WORK:-build/qemu-open}

usage() {
	sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
	cat <<'EOF'

Options:
  --hakios DIR        checkout of the upstream kernel tree (default: $HAKIOS)
  --seconds N         how long to let QEMU run before killing it (default: 120)
  --mem SIZE          guest RAM (default: 6G; 6G is what the kernel asks for)
  --check             assert the markers, print a verdict, exit 0/1
  --keep              keep the log even on success (default: kept, path is printed)
  -h, --help          this text
EOF
}

die() { local m=$1 c=${2:-2}; printf '%s: %s\n' "$PROG" "$m" >&2; exit "$c"; }
say() { printf '%s\n' "$*"; }

while [ $# -gt 0 ]; do
	case $1 in
		--hakios) HAKIOS=${2:?--hakios needs a directory}; shift ;;
		--seconds) SECONDS_RUN=${2:?--seconds needs a number}; shift ;;
		--mem) MEM=${2:?--mem needs a size}; shift ;;
		--check) CHECK=1 ;;
		--keep) : ;;
		-h|--help) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
	esac
	shift
done

[ -n "$HAKIOS" ] || die "no upstream checkout: pass --hakios DIR or set HAKIOS (see --help)" 2
[ -d "$HAKIOS" ] || die "no such directory: $HAKIOS" 3
[ -x "$HAKIOS/xnuloader/mkpayload.py" ] || die "$HAKIOS does not look like the upstream tree (no xnuloader/mkpayload.py)" 3

# --- tooling ---------------------------------------------------------------------------------

command -v qemu-system-aarch64 >/dev/null || die "qemu-system-aarch64 is missing (apt install qemu-system-arm)"
command -v python3 >/dev/null || die "python3 is missing"

KERNEL=$HAKIOS/prebuilt/hakios-kernel
LOADER=$HAKIOS/prebuilt/loader.efi
HELLO=$HAKIOS/prebuilt/userland/hello
for f in "$KERNEL" "$LOADER" "$HELLO"; do
	[ -f "$f" ] || die "upstream artifact missing: $f (run the upstream build, or use PREBUILT=1)" 3
done

# UEFI firmware: edk2, AAVMF, or Ubuntu's qemu-efi-aarch64 (the last is 2 MiB and needs padding).
EFI_SRC=${QEMU_EFI:-}
if [ -z "$EFI_SRC" ]; then
	for f in /usr/share/edk2/aarch64/QEMU_EFI.fd /usr/share/AAVMF/AAVMF_CODE.fd \
	         /usr/share/qemu-efi-aarch64/QEMU_EFI.fd; do
		[ -f "$f" ] && { EFI_SRC=$f; break; }
	done
fi
[ -n "$EFI_SRC" ] || die "no aarch64 UEFI firmware found (apt install qemu-efi-aarch64; or set QEMU_EFI)"

VARS_SRC=${QEMU_VARS:-}
if [ -z "$VARS_SRC" ]; then
	for f in /usr/share/edk2/aarch64/QEMU_VARS.fd /usr/share/AAVMF/AAVMF_VARS.fd; do
		[ -f "$f" ] && { VARS_SRC=$f; break; }
	done
fi

say "IOSVM open-kernel boot (no Apple content in the payload)"
say "  upstream  $HAKIOS"
say "  kernel    $KERNEL ($(stat -c %s "$KERNEL") bytes)"
say "  firmware  $EFI_SRC"
say "  work      $WORK"
say "  limits    ${SECONDS_RUN}s wall clock, ${MEM} guest RAM"
say

# --- stage -----------------------------------------------------------------------------------

rm -rf "$WORK"
mkdir -p "$WORK/esp/EFI/BOOT" "$WORK/logs"

# QEMU's pflash device insists on a 64 MiB image; Ubuntu ships 2 MiB.
cp "$EFI_SRC" "$WORK/efi.fd"
truncate -s 64M "$WORK/efi.fd"
if [ -n "$VARS_SRC" ]; then
	cp "$VARS_SRC" "$WORK/vars.fd"
	truncate -s 64M "$WORK/vars.fd"
else
	head -c 67108864 /dev/zero >"$WORK/vars.fd"
fi

cp "$LOADER" "$WORK/esp/EFI/BOOT/BOOTAA64.EFI"

say "building payload …"
python3 "$HAKIOS/xnuloader/mkpayload.py" -o "$WORK/payload.bin" \
	kernel="$KERNEL" ramdisk="$HELLO" --cmdline "" >"$WORK/logs/mkpayload.log" 2>&1 ||
	{ cat "$WORK/logs/mkpayload.log" >&2; die "mkpayload.py failed"; }
say "  payload.bin $(stat -c %s "$WORK/payload.bin") bytes"

# --- run -------------------------------------------------------------------------------------

LOG=$WORK/logs/qemu.log
say
say "booting for ${SECONDS_RUN}s (the window is closed: serial only) …"

set +e
timeout --signal=TERM "$SECONDS_RUN" qemu-system-aarch64 \
	-M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m "$MEM" \
	-accel tcg,tb-size=384 \
	-drive if=pflash,format=raw,readonly=on,file="$WORK/efi.fd" \
	-drive if=pflash,format=raw,file="$WORK/vars.fd" \
	-drive file=fat:rw:"$WORK/esp",format=raw,if=virtio \
	-device loader,file="$WORK/payload.bin",addr=0xF0000000,force-raw=on \
	-display none -serial stdio -monitor none -no-reboot \
	>"$LOG" 2>&1
rc=$?
set -e
# timeout(1) exits 124 when it had to kill QEMU, which is the normal end of a bounded run.
case $rc in
	0|124) : ;;
	*) say "warning: QEMU exited with $rc" ;;
esac

# --- verdict ---------------------------------------------------------------------------------

mark() { grep -qF -- "$1" "$LOG"; }

reached=0
mark "== XNULOADER v3 =="            && loader=1 || loader=0
mark "Darwin Kernel Version"         && banner=1 || banner=0
mark "vm_page_bootstrap"             && paged=1  || paged=0
mark "MAC Framework successfully"    && mac=1    || mac=0
mark "initproc failed to start"      && launchd=1 || launchd=0
[ $((loader + banner + paged + mac + launchd)) -eq 5 ] && reached=1 || true

# The first timestamped kernel line is the closest thing to "how long did boot take".
t_int=$(sed -n 's/^\[ *\([0-9][0-9]*\.[0-9][0-9]*\)\]:.*interrupt-controller.*/\1/p' "$LOG" | head -1)

verdict() { [ "$1" = 1 ] && printf '[ok]  ' || printf '[FAIL]'; }
say
say "markers:"
printf '  %s loader banner (XNULOADER v3)\n' "$(verdict "$loader")"
printf '  %s kernel banner (Darwin Kernel Version)\n' "$(verdict "$banner")"
printf '  %s paged memory (vm_page_bootstrap)\n' "$(verdict "$paged")"
printf '  %s security framework (MAC Framework)\n' "$(verdict "$mac")"
printf '  %s BSD half reached pid 1 (expected panic on launchd)\n' "$(verdict "$launchd")"
[ -n "$t_int" ] && say "  [i]   kernel reached the interrupt controller at ${t_int}s"
say
say "log: $LOG ($(grep -c . "$LOG" || true) non-empty lines)"

if [ "$CHECK" = 1 ]; then
	if [ "$reached" = 1 ]; then
		say "qemu-open: PASS — the payload has no Apple content and XNU reached BSD initialisation"
		say "                (kernel: the upstream tree's build; this script does not compile it)"
		exit 0
	fi
	say "qemu-open: FAIL — see the log above; the first missing marker is the one to chase" >&2
	grep -nE "panic|error|failed|not found" "$LOG" | head -10 >&2 || true
	exit 1
fi

say
say "tail of the log:"
tail -n 15 "$LOG" | sed 's/^/  /'

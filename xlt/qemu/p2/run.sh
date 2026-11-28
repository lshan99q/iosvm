#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Verify P2.1/P2.2 fixtures: TCG is the serial reference, xlt must match byte-for-byte.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
tcg="${XLT_TCG_QEMU:-/home/lshan/xlt/qemu-8.2.2-build/qemu-system-aarch64}"
xlt="${XLT_XLT_QEMU:-/home/lshan/xlt/qemu-8.2.2-xlt-build/qemu-system-aarch64}"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

die() { echo "run.sh: $*" >&2; exit 1; }
[ -x "$tcg" ] || die "missing TCG QEMU: $tcg"
[ -x "$xlt" ] || die "missing xlt QEMU: $xlt"

mmu="$repo/xlt/build/fixtures/p2/mmu.elf"
semihost="$repo/xlt/build/fixtures/p2/semihost.elf"
exc="$repo/xlt/build/fixtures/p2/exc.elf"
[ -f "$mmu" ] || "$repo/xlt/tests/fixtures/p2/mmu/build.sh" >/dev/null
[ -f "$semihost" ] || "$repo/xlt/tests/fixtures/p2/semihost/build.sh" >/dev/null
[ -f "$exc" ] || "$repo/xlt/tests/fixtures/p2/exc/build.sh" >/dev/null

# run_qemu <out> <err> <qemu> <machine> <accel> <elf> [extra...]
run_qemu() {
    local out="$1" err="$2" qemu="$3" machine="$4" accel="$5" elf="$6"
    shift 6
    local rc=0
    timeout 30 "$qemu" -M "$machine" -accel "$accel" "$@" \
        -cpu cortex-a76 -smp 1 -m 256M -kernel "$elf" -display none \
        -serial stdio -monitor none -no-reboot >"$out" 2>"$err" </dev/null || rc=$?
    echo "$rc"
}

check_xlt() {
    local name="$1" elf="$2" reference="$3"
    local out="$tmp/$name.xlt" err="$tmp/$name.xlt.err"
    local rc
    rc="$(run_qemu "$out" "$err" "$xlt" xlt xlt "$elf")"
    [ "$rc" -eq 0 ] || die "$name under xlt failed (rc=$rc)"
    [ ! -s "$err" ] || { cat "$err" >&2; die "$name under xlt wrote to stderr"; }
    cmp -s "$reference" "$out" || {
        diff -u "$reference" "$out" >&2 || true
        die "$name: TCG and xlt output differ"
    }
    echo "ok: $name: TCG/xlt identical ($(wc -c <"$reference") bytes)"
}

# MMU fixture: serial output and rc 0 from both engines.
mmu_tcg_out="$tmp/p2-mmu.tcg"
mmu_tcg_err="$tmp/p2-mmu.tcg.err"
mmu_rc="$(run_qemu "$mmu_tcg_out" "$mmu_tcg_err" "$tcg" virt tcg "$mmu")"
[ "$mmu_rc" -eq 0 ] || die "p2-mmu under TCG failed (rc=$mmu_rc)"
[ ! -s "$mmu_tcg_err" ] || { cat "$mmu_tcg_err" >&2; die "p2-mmu under TCG wrote to stderr"; }
check_xlt p2-mmu "$mmu" "$mmu_tcg_out"

# Semihosting fixture: TCG's HLT semihosting writes to stderr and exits 1;
# compare its semihosting text with the xlt machine's PL011/semihost output.
semi_tcg_out="$tmp/p2-semihost.tcg"
semi_tcg_err="$tmp/p2-semihost.tcg.err"
semi_rc="$(run_qemu "$semi_tcg_out" "$semi_tcg_err" "$tcg" virt tcg "$semihost" -semihosting)"
[ "$semi_rc" -eq 1 ] || die "p2-semihost under TCG expected rc=1, got $semi_rc"
[ ! -s "$semi_tcg_out" ] || die "p2-semihost under TCG unexpectedly wrote stdout"
check_xlt p2-semihost "$semihost" "$semi_tcg_err"

# P2.2 exception fixture: EL0/EL1 SVC, lower-EL data abort, illegal ERET and DAIF.
exc_tcg_out="$tmp/p2-exc.tcg"
exc_tcg_err="$tmp/p2-exc.tcg.err"
exc_rc="$(run_qemu "$exc_tcg_out" "$exc_tcg_err" "$tcg" virt tcg "$exc")"
[ "$exc_rc" -eq 0 ] || die "p2-exc under TCG failed (rc=$exc_rc)"
[ ! -s "$exc_tcg_err" ] || { cat "$exc_tcg_err" >&2; die "p2-exc under TCG wrote to stderr"; }
check_xlt p2-exc "$exc" "$exc_tcg_out"

echo "run.sh: PASS (P2.1 MMU/fault/ERET + semihosting and P2.2 exception parity)"

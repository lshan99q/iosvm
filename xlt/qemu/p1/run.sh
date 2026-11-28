#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Reproducible P1 parity check for the xlt accelerator.
#
# Builds the two freestanding P1 fixtures (xlt/tests/fixtures/p1) and the t9
# qemu-smoke image, then runs each of them twice through QEMU: once on the TCG
# baseline (-M virt -accel tcg -cpu cortex-a76) and once on the xlt accelerator
# (-M xlt -accel xlt -cpu cortex-a76). The serial output must be byte-for-byte
# identical between TCG and xlt and identical across the two runs of each side.
#
# The script also checks the failure paths: a missing -kernel, an unknown
# accelerator, -M xlt without -accel xlt (the accelerator owns guest startup and
# the machine rejects the missing accel) and -accel xlt on -M virt. Every one of
# them must fail cleanly with a diagnostic instead of hanging.
#
# The QEMU binary defaults to the tree built by xlt/qemu/build-xlt.sh; override
# it with QEMU=/path/to/qemu-system-aarch64. Scratch files go to a mktemp dir.
#
# Exit status: 0 if every check passes, 1 on any mismatch/failure, 2 on usage
# or setup errors.

set -euo pipefail

here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "${here}/../../.." && pwd)
qemu=${QEMU:-/home/lshan/xlt/qemu-8.2.2-xlt-build/qemu-system-aarch64}
timeout_s=${QEMU_TIMEOUT:-20}
p1_build=${repo}/xlt/tests/fixtures/p1/build.sh
smoke_build=${repo}/xlt/tests/fixtures/qemu-smoke/build.sh
fixture_root=${repo}/xlt/build/fixtures/p1
smoke_elf=${repo}/xlt/build/qemu-smoke/qemu-smoke.elf

die() {
    echo "run.sh: $*" >&2
    exit 2
}

usage() {
    cat <<'EOF'
Usage: ./run.sh [--qemu PATH]

  --qemu PATH   qemu-system-aarch64 binary (default: $QEMU or the
                xlt/qemu/build-xlt.sh output path)
  -h, --help    show this help

Environment: QEMU, QEMU_TIMEOUT (seconds per run, default 20).
EOF
}

while (( $# > 0 )); do
    case "$1" in
        --qemu)
            (( $# >= 2 )) || die "--qemu needs a path"
            qemu=$2
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown argument: $1"
            ;;
    esac
done

[[ -x "$qemu" ]] || die "QEMU binary not found or not executable: $qemu (build it with xlt/qemu/build-xlt.sh, or pass --qemu PATH)"
[[ -x "$p1_build" ]] || die "fixture build script not found: $p1_build"
[[ -x "$smoke_build" ]] || die "fixture build script not found: $smoke_build"

scratch=$(mktemp -d "${TMPDIR:-/tmp}/xlt-p1-run.XXXXXX")
trap 'rm -rf "$scratch"' EXIT

echo "QEMU:     $qemu"
echo "fixtures: $fixture_root, $smoke_elf"
echo

run_qemu() { # out err prefix args...
    local out=$1 err=$2
    shift 2
    timeout "$timeout_s" "$qemu" "$@" >"$out" 2>"$err"
}

check_failure() { # name pattern args...
    local name=$1 pattern=$2
    shift 2
    local out=$scratch/fail-${name}.out err=$scratch/fail-${name}.err
    local rc=0
    run_qemu "$out" "$err" "$@" || rc=$?
    if (( rc == 0 )); then
        echo "FAIL: ${name}: expected a non-zero exit, got 0"
        return 1
    fi
    if (( rc == 124 )); then
        echo "FAIL: ${name}: timed out after ${timeout_s}s (possible hang)"
        return 1
    fi
    if ! grep -q -- "$pattern" "$err"; then
        echo "FAIL: ${name}: stderr does not contain: $pattern"
        sed -n '1,5p' "$err" >&2
        return 1
    fi
    echo "ok: ${name} (rc=${rc})"
}

check_fixture() { # name elf expected
    local name=$1 elf=$2 expected=$3
    local ok=1
    local run side
    local common=(-cpu cortex-a76 -smp 1 -m 256M -kernel "$elf" -display none -serial stdio -monitor none -no-reboot)

    if [[ ! -f "$elf" ]]; then
        echo "FAIL: ${name}: fixture not built: $elf"
        return 1
    fi

    for run in 1 2; do
        if ! run_qemu "$scratch/${name}.tcg.${run}.out" "$scratch/${name}.tcg.${run}.err" \
                -M virt -accel tcg "${common[@]}"; then
            echo "FAIL: ${name}: TCG baseline run ${run} exited non-zero"
            ok=0
        fi
        if ! run_qemu "$scratch/${name}.xlt.${run}.out" "$scratch/${name}.xlt.${run}.err" \
                -M xlt -accel xlt "${common[@]}"; then
            echo "FAIL: ${name}: xlt accelerator run ${run} exited non-zero"
            ok=0
        fi
    done

    for side in tcg xlt; do
        if ! cmp -s "$scratch/${name}.${side}.1.out" "$scratch/${name}.${side}.2.out"; then
            echo "FAIL: ${name}: ${side} output differs between run 1 and run 2" >&2
            ok=0
        fi
    done

    if ! cmp -s "$scratch/${name}.tcg.1.out" "$scratch/${name}.xlt.1.out"; then
        echo "FAIL: ${name}: serial output differs between TCG and xlt" >&2
        diff -u "$scratch/${name}.tcg.1.out" "$scratch/${name}.xlt.1.out" >&2 || true
        ok=0
    fi

    if ! grep -qxF -- "$expected" "$scratch/${name}.tcg.1.out"; then
        echo "FAIL: ${name}: expected line not found: $expected" >&2
        sed -n '1,5p' "$scratch/${name}.tcg.1.out" >&2
        ok=0
    fi

    if (( ok )); then
        echo "ok: ${name}: TCG/xlt identical twice: ${expected}"
        return 0
    fi
    return 1
}

echo "building fixtures"
"$p1_build" >/dev/null
"$smoke_build" >/dev/null
echo

failed=0
check_fixture fixture1 "${fixture_root}/fixture1.elf" "xlt-p1-fixture1" || failed=1
check_fixture fixture2 "${fixture_root}/fixture2.elf" \
    "xlt-p1-fixture2 fr=0x0000000000000090 ram=0x1122334455667788 el=0x0000000000000004" || failed=1
check_fixture qemu-smoke "${smoke_elf}" "xlt-p1-ok" || failed=1

echo
check_failure "no-kernel" "xlt: -kernel <elf> is required" \
    -M xlt -accel xlt -display none -serial stdio -monitor none -no-reboot || failed=1
check_failure "unknown-accel" "invalid accelerator" \
    -M xlt -accel xlt-does-not-exist -display none -serial stdio -monitor none -no-reboot || failed=1
check_failure "machine-needs-accel" "-M xlt requires -accel xlt" \
    -M xlt -cpu cortex-a76 -smp 1 -m 256M -kernel "${fixture_root}/fixture1.elf" \
    -display none -serial stdio -monitor none -no-reboot || failed=1
check_failure "accel-needs-machine" "-accel xlt is only supported by -M xlt" \
    -M virt -accel xlt -cpu cortex-a76 -smp 1 -m 256M -kernel "${fixture_root}/fixture1.elf" \
    -display none -serial stdio -monitor none -no-reboot || failed=1

echo
if (( failed )); then
    echo "run.sh: FAIL"
    exit 1
fi
echo "run.sh: PASS (P1 parity, determinism and failure behaviour verified)"

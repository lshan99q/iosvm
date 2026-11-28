#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# run.sh — compare QEMU TCG and xlt on the bare-metal AArch64 benchmark.
#
#   ./run.sh                 TCG baseline and, when built, xlt; per-test table
#   ./run.sh --skip-xlt      TCG baseline only
#   ./run.sh --elf PATH      benchmark ELF (default xlt/build/bench/bench.a64.elf)
#   ./run.sh --xlt PATH      xlt binary (default xlt/build/xlt)
#   ./run.sh --build         rebuild the ELF with build.sh first
#
# The ELF is built automatically when it does not exist. Each engine gets
# `timeout 60`. QEMU's semihosting SYS_EXIT path makes qemu itself exit with
# status 1 even when the workload completed, so the harness trusts the parsed
# result rows, not QEMU's status. Exit codes: 0 every checksum matches, 1 a
# checksum or result row is wrong or missing, 2 the harness could not run.

set -euo pipefail

prog=${0##*/}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
xlt_dir=$(cd -- "$here/.." && pwd)
default_elf=$xlt_dir/build/bench/bench.a64.elf
default_xlt=$xlt_dir/build/xlt
elf=$default_elf
xlt_bin=$default_xlt
run_xlt=auto
xlt_explicit=0
do_build=0
timeout_s=${TIMEOUT:-60}

# The five workloads bench.c runs, in output order.
tests=(alu64 memcpy hash ptrchase indcall)

# Expected results as produced by bench.c. Values are compared as zero-padded
# 16-digit lowercase hex because that is what out_hex() prints.
declare -A expect=(
	[alu64]=067a3bf250fa984e
	[memcpy]=13fbe40bf4180000
	[hash]=6a1365d0c6
	[ptrchase]=3d094fd9544
	[indcall]=c2a86d1e196c073f
)

usage() { sed -n '4,16p' "$0" | sed -E 's/^# ?//'; }
die() { echo "$prog: $*" >&2; exit 2; }

while [[ $# -gt 0 ]]; do
	case $1 in
		-h | --help | help) usage; exit 0 ;;
		--skip-xlt) run_xlt=no; shift ;;
		--build) do_build=1; shift ;;
		--elf)
			[[ $# -ge 2 ]] || die "--elf needs a path"
			elf=$2
			shift 2
			;;
		--xlt)
			[[ $# -ge 2 ]] || die "--xlt needs a path"
			xlt_bin=$2
			xlt_explicit=1
			run_xlt=yes
			shift 2
			;;
		-*) die "unknown option '$1' (see --help)" ;;
		*) die "unexpected argument '$1' (see --help)" ;;
	esac
done

command -v qemu-system-aarch64 >/dev/null 2>&1 ||
	die "qemu-system-aarch64 not found; install QEMU to run the TCG baseline"
command -v timeout >/dev/null 2>&1 || die "timeout not found"

if ((do_build)); then
	echo "== building bench.a64.elf"
	"$here/build.sh" bench
fi
if [[ ! -f $elf ]]; then
	[[ $elf == "$default_elf" ]] || die "no benchmark ELF at $elf (build it first)"
	echo "== building bench.a64.elf"
	"$here/build.sh" bench
	[[ -f $elf ]] || die "build did not produce $elf"
fi

hex_norm() {
	local h=${1#0x}
	h=${h,,}
	[[ $h =~ ^[0-9a-f]{1,16}$ ]] || return 1
	printf '%016s' "$h" | tr ' ' 0
}

fmt_ms() {
	local c=$1
	printf '%d.%02d' $((c / 62500)) $(((c % 62500) * 100 / 62500))
}

fmt_speedup() {
	local a=$1 b=$2 v
	((b > 0)) || { printf '%s' '-'; return; }
	v=$((a * 100 / b))
	printf '%d.%02dx' $((v / 100)) $((v % 100))
}

declare -A tcg_sum=() tcg_cyc=() xlt_sum=() xlt_cyc=()

collect() {
	local tag=$1 raw=$2 name sum cyc
	while read -r name sum cyc; do
		[[ -n $name ]] || continue
		if [[ $tag == tcg ]]; then
			tcg_sum[$name]=$sum
			tcg_cyc[$name]=$cyc
		else
			xlt_sum[$name]=$sum
			xlt_cyc[$name]=$cyc
		fi
	done < <(sed -nE \
		's/^(alu64|memcpy|hash|ptrchase|indcall): (0x[0-9a-fA-F]+) cycles=([0-9]+)$/\1 \2 \3/p' \
		<<<"$raw")
}

echo "benchmark: $elf"
echo "clock:     cntvct_el0, 62.5 MHz (16 ns/tick; ms = ticks / 62500)"
echo

echo "== qemu-system-aarch64 -M virt -cpu cortex-a76 -accel tcg (timeout ${timeout_s}s)"
tcg_raw=
tcg_rc=0
tcg_raw=$(timeout "$timeout_s" qemu-system-aarch64 \
	-M virt -cpu cortex-a76 -m 512M -smp 1 -accel tcg,tb-size=1536 \
	-kernel "$elf" -semihosting-config enable=on,target=native \
	-nographic -monitor none 2>&1) || tcg_rc=$?
((tcg_rc != 124)) || die "QEMU timed out after ${timeout_s}s"
grep -q 'XLT-BENCH v1' <<<"$tcg_raw" ||
	die "QEMU produced no benchmark output (exit $tcg_rc)"
collect tcg "$tcg_raw"
echo "   qemu exit status $tcg_rc (semihosting SYS_EXIT path; result rows are what count)"
echo

xlt_ready=0
xlt_note=
if [[ $run_xlt != no ]]; then
	if ((xlt_explicit)); then
		[[ -x $xlt_bin ]] || die "--xlt $xlt_bin is not executable"
		xlt_ready=1
	elif [[ -x $xlt_bin ]]; then
		xlt_ready=1
	elif [[ -e $xlt_bin ]]; then
		xlt_note="$xlt_bin exists but is not executable"
	else
		xlt_note="no xlt binary at $xlt_bin (build it with: make -C $xlt_dir)"
	fi
fi

if ((xlt_ready)); then
	echo "== $xlt_bin run $elf (timeout ${timeout_s}s)"
	xlt_raw=
	xlt_rc=0
	xlt_raw=$(timeout "$timeout_s" "$xlt_bin" run "$elf" 2>&1) || xlt_rc=$?
	collect xlt "$xlt_raw"
	if ((${#xlt_sum[@]} == 0)); then
		xlt_ready=0
		msg=$(head -n 1 <<<"$xlt_raw")
		xlt_note="xlt produced no benchmark rows (exit $xlt_rc${msg:+: $msg})"
	else
		echo "   xlt exit status $xlt_rc"
	fi
elif [[ -n $xlt_note ]]; then
	echo "   xlt skipped: $xlt_note"
fi
echo

printf '%-9s %12s %9s %12s %9s %9s  %-3s %-3s\n' \
	test 'TCG ticks' 'TCG ms' 'xlt ticks' 'xlt ms' speedup TCG xlt
printf '%s\n' "------------------------------------------------------------------------------"

fail=0
total_tcg=0
total_xlt=0
have_xlt_total=1
for name in "${tests[@]}"; do
	tc=${tcg_cyc[$name]:-}
	ts=${tcg_sum[$name]:-}
	xc=${xlt_cyc[$name]:-}
	xs=${xlt_sum[$name]:-}
	tcg_state='-'
	xlt_state='-'
	if [[ -z $tc || -z $ts ]]; then
		tcg_state='?'
		fail=1
	else
		total_tcg=$((total_tcg + tc))
		if [[ $(hex_norm "$ts") == "$(hex_norm "${expect[$name]}")" ]]; then
			tcg_state='ok'
		else
			tcg_state='BAD'
			fail=1
			printf '%s: TCG checksum 0x%s != expected 0x%s\n' \
				"$prog" "$(hex_norm "$ts")" "$(hex_norm "${expect[$name]}")" >&2
		fi
	fi

	if ((xlt_ready)); then
		if [[ -z $xc || -z $xs ]]; then
			xlt_state='?'
			fail=1
		else
			total_xlt=$((total_xlt + xc))
			if [[ $(hex_norm "$xs") == "$(hex_norm "${expect[$name]}")" ]]; then
				xlt_state='ok'
			else
				xlt_state='BAD'
				fail=1
				printf '%s: xlt checksum 0x%s != expected 0x%s\n' \
					"$prog" "$(hex_norm "$xs")" "$(hex_norm "${expect[$name]}")" >&2
			fi
		fi
	else
		have_xlt_total=0
	fi

	if [[ -n $tc && -n $xc ]]; then
		sp=$(fmt_speedup "$tc" "$xc")
	else
		sp='-'
	fi
	printf '%-9s %12s %9s %12s %9s %9s  %-3s %-3s\n' \
		"$name" "${tc:--}" "$([[ -n $tc ]] && fmt_ms "$tc" || echo -)" \
		"${xc:--}" "$([[ -n $xc ]] && fmt_ms "$xc" || echo -)" \
		"$sp" "$tcg_state" "$xlt_state"
done

tcg_total_ms=$(fmt_ms "$total_tcg")
if ((have_xlt_total)); then
	printf '%-9s %12s %9s %12s %9s %9s\n' total "$total_tcg" "$tcg_total_ms" \
		"$total_xlt" "$(fmt_ms "$total_xlt")" "$(fmt_speedup "$total_tcg" "$total_xlt")"
else
	printf '%-9s %12s %9s %12s %9s %9s\n' total "$total_tcg" "$tcg_total_ms" - - -
fi

echo
if ((fail)); then
	echo "$prog: FAIL: checksum or result row mismatch" >&2
	exit 1
fi
echo "$prog: all ${#tests[@]} checksums match"
if ((!have_xlt_total)); then
	echo "$prog: TCG baseline only (xlt not run: ${xlt_note:-disabled})"
fi

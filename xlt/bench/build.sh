#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build.sh — build the bare-metal AArch64 benchmark images for QEMU TCG and xlt.
#
#   ./build.sh              build bench.a64.elf (the five-workload benchmark)
#   ./build.sh memprobe     build the memory-access probe
#   ./build.sh memcopy2     build the 4 MiB copy isolation probe
#   ./build.sh widthprobe   build the copy-width probe
#   ./build.sh all          build every image
#   ./build.sh clean        remove xlt/build/bench/
#
# The flags are fixed on purpose: a freestanding AArch64 ELF with no libc, no
# PIC, no outline atomics, no floating point, no unwind tables and no build id,
# linked at 0x40080000 by link.ld so that both
# `qemu-system-aarch64 -M virt -kernel` and xlt can start it directly.
# All build products land in xlt/build/bench/, which git ignores; a binary is
# never committed.

set -euo pipefail

prog=${0##*/}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out_dir=$(cd -- "$here/.." && pwd)/build/bench
cc=${CC:-clang}

usage() { sed -n '4,18p' "$0" | sed -E 's/^# ?//'; }
die() { echo "$prog: $*" >&2; exit 2; }

target_src() {
	case $1 in
		bench)      echo 'bench.c bench.a64.elf' ;;
		memprobe)   echo 'memprobe.c memprobe.a64.elf' ;;
		memcopy2)   echo 'memcopy2.c memcopy2.a64.elf' ;;
		widthprobe) echo 'widthprobe.c widthprobe.a64.elf' ;;
		*) return 1 ;;
	esac
}

build_one() {
	local name=$1 src out
	read -r src out <<<"$(target_src "$name")" || die "unknown target '$name' (see --help)"
	[[ -f $here/$src ]] || die "missing source $here/$src"

	mkdir -p "$out_dir"
	# shellcheck disable=SC2054 # the commas belong to the -Wl option, not to an array
	local -a cmd=(
		"$cc" --target=aarch64-unknown-elf -O2 -ffreestanding -nostdlib
		-fno-pic -fno-stack-protector -mgeneral-regs-only -mno-outline-atomics
		-fno-asynchronous-unwind-tables -fuse-ld=lld
		-Wl,-T,"$here/link.ld" -Wl,--build-id=none
		"$here/$src" "$here/start.S" -o "$out_dir/$out"
	)
	echo "== $name"
	printf '   %s\n' "$(printf '%q ' "${cmd[@]}")"
	"${cmd[@]}"
	printf '%s: built %s (%s bytes)\n' "$prog" "$out_dir/$out" "$(wc -c <"$out_dir/$out")"
}

clean() {
	case $out_dir in
		*/xlt/build/bench) ;;
		*) die "refusing to remove unexpected directory: $out_dir" ;;
	esac
	if [[ -d $out_dir ]]; then
		rm -rf -- "$out_dir"
		echo "$prog: removed $out_dir"
	else
		echo "$prog: $out_dir already absent"
	fi
}

command -v "$cc" >/dev/null 2>&1 ||
	die "$cc not found; clang with the AArch64 and lld backends is required"

[[ $# -eq 0 ]] && set -- bench
for arg in "$@"; do
	case $arg in
		-h | --help | help) usage; exit 0 ;;
		clean) clean ;;
		all)
			for t in bench memprobe memcopy2 widthprobe; do build_one "$t"; done
			;;
		-*) die "unknown option '$arg' (see --help)" ;;
		*) build_one "$arg" ;;
	esac
done

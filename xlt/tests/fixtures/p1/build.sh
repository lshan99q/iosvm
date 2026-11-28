#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build the two P1 boot fixtures as freestanding AArch64 ELFs. Products go to
# xlt/build/fixtures/p1/, which is ignored by git.

set -euo pipefail

prog=${0##*/}
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
out_dir=$(cd -- "$here/../../.." && pwd)/build/fixtures/p1
cc=${CC:-clang}

die() { echo "$prog: $*" >&2; exit 2; }

command -v "$cc" >/dev/null 2>&1 || die "$cc not found"

build_one() {
	local name=$1
	mkdir -p "$out_dir"
	# shellcheck disable=SC2054 # the commas belong to the -Wl option, not to an array
	local -a cmd=(
		"$cc" --target=aarch64-unknown-elf -O2 -ffreestanding -nostdlib
		-fno-pic -fno-stack-protector -mgeneral-regs-only -mno-outline-atomics
		-fno-asynchronous-unwind-tables -fuse-ld=lld
		-Wl,-T,"$here/link.ld" -Wl,--build-id=none
		"$here/$name.c" "$here/start.S" -o "$out_dir/$name.elf"
	)
	echo "== $name"
	"${cmd[@]}"
	printf '%s: built %s (%s bytes)\n' "$prog" "$out_dir/$name.elf" "$(wc -c <"$out_dir/$name.elf")"
}

for name in fixture1 fixture2; do
	build_one "$name"
done

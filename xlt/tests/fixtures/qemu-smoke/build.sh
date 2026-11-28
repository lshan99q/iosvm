#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the P1 QEMU smoke payload: freestanding AArch64, linked at 0x40080000.
# The output lives under xlt/build/ (gitignored); nothing is written into the
# fixture directory itself.
set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$here/../../../.." && pwd)
out_dir="$repo/xlt/build/qemu-smoke"
out="$out_dir/qemu-smoke.elf"

mkdir -p "$out_dir"

clang \
    --target=aarch64-unknown-elf \
    -O2 \
    -nostdlib \
    -ffreestanding \
    -fuse-ld=lld \
    -Wl,-T,"$here/link.ld" \
    -Wl,--build-id=none \
    "$here/start.S" \
    -o "$out"

echo "$out"

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Build the P2.2 EL0/SVC/data-abort/illegal-ERET fixture for aarch64-unknown-elf.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../../.." && pwd)"
out="${1:-$repo/build/fixtures/p2/exc.elf}"
mkdir -p "$(dirname "$out")"

clang --target=aarch64-unknown-elf -O2 -ffreestanding -nostdlib -fno-pic \
    -fno-stack-protector -mgeneral-regs-only -mno-outline-atomics \
    -fno-asynchronous-unwind-tables -fuse-ld=lld \
    -Wl,-T,"$here/link.ld" -Wl,--build-id=none \
    "$here/start.S" "$here/main.c" -o "$out"

echo "$out"
sha256sum "$out"

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build a QEMU 8.2.2 tree with the P1 xlt accelerator and machine patches.
#
# The upstream source stays untouched in $QEMU_WORK_DIR/qemu-8.2.2. This
# script rsyncs a disposable copy to qemu-8.2.2-xlt, applies every patch in
# xlt/qemu/patches/, builds the host-neutral libxlt archive, and configures
# QEMU with -M xlt support. Products go to qemu-8.2.2-xlt-build/.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/../.." && pwd)
work=${QEMU_WORK_DIR:-/home/lshan/xlt}
jobs=${QEMU_JOBS:-16}
src=$work/qemu-8.2.2
patched=$work/qemu-8.2.2-xlt
build=$work/qemu-8.2.2-xlt-build
patch_dir=$here/patches
lib_dir=$root/xlt/build/qemu

die() { echo "build-xlt.sh: $*" >&2; exit 2; }

[[ -x $src/configure ]] || {
	"$here/build-qemu.sh" >/dev/null ||
		die "cannot build upstream QEMU; run xlt/qemu/build-qemu.sh first"
}
[[ -f $src/VERSION ]] || die "not a QEMU source tree: $src"
[[ $(cat "$src/VERSION") == 8.2.2 ]] || die "expected QEMU 8.2.2 in $src"
[[ -x $(command -v llvm-config-18) ]] || die "llvm-config-18 not found"
[[ -x $(command -v g++) ]] || die "g++ not found"
[[ -x $(command -v rsync) ]] || die "rsync not found"
[[ -d $patch_dir ]] || die "missing patch directory $patch_dir"

echo "== syncing $src -> $patched" >&2
rm -rf -- "$patched"
mkdir -p "$patched"
rsync -a "$src/" "$patched/"

echo "== applying xlt patches" >&2
for p in "$patch_dir"/*.patch; do
	[[ -e $p ]] || die "no patches in $patch_dir"
	echo "   $(basename "$p")" >&2
	patch -p1 -d "$patched" --batch --forward -i "$p"
done

echo "== building libxlt (PIC, C++17)" >&2
mkdir -p "$lib_dir/obj"
llvm_flags=()
read -r -a llvm_flags <<<"$(llvm-config-18 --cxxflags)"
cxxflags=(-O2 -g -std=c++17 -fPIC -I"$root/xlt/src" "${llvm_flags[@]}")
for srcfile in decode.cpp jit.cpp libxlt.cpp libxlt_c.cpp lift.cpp; do
	echo "   $srcfile" >&2
	g++ "${cxxflags[@]}" -c "$root/xlt/src/$srcfile" -o "$lib_dir/obj/${srcfile%.cpp}.o"
done
ar rcs "$lib_dir/libxlt.a" "$lib_dir"/obj/*.o

echo "== configuring QEMU in $build" >&2
mkdir -p "$build"
if [[ ! -f $build/build.ninja ]]; then
	(
		cd "$build"
		"$patched/configure" \
			--target-list=aarch64-softmmu \
			--disable-docs \
			--disable-werror \
			--disable-gtk \
			--disable-sdl \
			--disable-vnc \
			--disable-opengl \
			--disable-spice \
			--disable-libssh \
			--disable-guest-agent \
			--extra-cflags="-I$root/xlt/src -I/usr/lib/llvm-18/include" \
			--extra-ldflags="-L$lib_dir -lxlt -lLLVM-18 -lcapstone -lpthread -ldl -lz -ltinfo -lstdc++ -lm -Wl,-rpath,/usr/lib/llvm-18/lib"
	)
fi

echo "== building qemu-system-aarch64" >&2
ninja -C "$build" -j"$jobs" qemu-system-aarch64

[[ -x $build/qemu-system-aarch64 ]] || die "build finished without qemu-system-aarch64"
echo "$build/qemu-system-aarch64"

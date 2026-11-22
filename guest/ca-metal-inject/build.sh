#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Build libcainject.dylib for the CoreAnimation-on-Metal experiment:
#   build/libcainject.dylib   the injected instrumentation (see ca_inject.m)
#
# The toolchain is the HakiOS cross toolchain (xnu-env.sh + the iPhoneOS 16.5 SDK in
# $THEOS/sdks); the HakiOS tree is read, never written. Nothing else is required.
#
#   HAKIOS_DIR  default /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
#   SDK         default $THEOS/sdks/iPhoneOS16.5.sdk
#   CC          default xnu-clang
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
HAKIOS_DIR=${HAKIOS_DIR:-/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel}

if [ ! -f "$HAKIOS_DIR/xnu-env.sh" ]; then
	echo "build.sh: no xnu-env.sh under HAKIOS_DIR=$HAKIOS_DIR" >&2
	exit 1
fi
# shellcheck disable=SC1090,SC1091
source "$HAKIOS_DIR/xnu-env.sh"
export LD64_LLD=${LD64_LLD:-/usr/bin/ld64.lld-18}
SDK=${SDK:-$THEOS/sdks/iPhoneOS16.5.sdk}
CC=${CC:-xnu-clang}

[ -d "$SDK" ] || { echo "build.sh: no iPhoneOS SDK at $SDK" >&2; exit 1; }
command -v "$CC" >/dev/null 2>&1 || { echo "build.sh: $CC is not on PATH" >&2; exit 1; }

OUT=$HERE/build
mkdir -p "$OUT"

echo "build.sh: CC=$CC SDK=$SDK"
"$CC" -arch arm64 -isysroot "$SDK" -O1 -Wall -Wextra \
	-dynamiclib -install_name /redwood/lib/libcainject.dylib \
	-framework Foundation -framework CoreFoundation -lobjc \
	-o "$OUT/libcainject.dylib" "$HERE/ca_inject.m"

file "$OUT/libcainject.dylib"
if command -v llvm-otool-18 >/dev/null 2>&1; then
	llvm-otool-18 -l "$OUT/libcainject.dylib" |
		grep -E 'cmd LC_BUILD_VERSION|platform |sdk |minos |LC_LOAD_DYLIB|      name ' | head -12
fi
echo "build: $OUT/libcainject.dylib"

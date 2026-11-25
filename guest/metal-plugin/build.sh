#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# build.sh — build the metalwire guest side with the HakiOS cross toolchain:
#
#   build/HakiosMetal.bundle/HakiosMetal        the Metal device bundle (the host's client)
#   build/HakiosMetal.bundle/Info.plist
#   build/metal_frame                            the Metal test client (drives the wire)
#   build/metal_probe                            the device diagnostic (no wire traffic)
#   build/HakiosMetal.clients                    the process list to install beside the bundle
#
# The toolchain is the one the HakiOS tree provides (xnu-env.sh + the iPhoneOS 16.5 SDK in
# $THEOS/sdks); nothing is modified in that tree. Override HAKIOS_DIR, SDK, CC, LD64_LLD or THEOS
# if your layout differs. The toolchain is Linux-hosted clang with a Darwin target; the result is
# an arm64 Mach-O bundle and two arm64 Mach-O executables, checked with `file` and `otool -l`.
#
# The wire header is shared, not copied: -I host/metalwire makes `#include "metalwire_protocol.h"`
# resolve to the repository's own contract, so the guest and the host cannot disagree about a
# struct by accident.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
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

if [ ! -d "$SDK" ]; then
	echo "build.sh: no iPhoneOS SDK at $SDK (THEOS=$THEOS); set SDK=" >&2
	exit 1
fi
if ! command -v "$CC" >/dev/null 2>&1; then
	echo "build.sh: $CC is not on PATH; source $HAKIOS_DIR/xnu-env.sh first?" >&2
	exit 1
fi

PROTOCOL_DIR=$REPO/host/metalwire
if [ ! -f "$PROTOCOL_DIR/metalwire_protocol.h" ]; then
	echo "build.sh: no $PROTOCOL_DIR/metalwire_protocol.h; is this a worktree with host/?" >&2
	exit 1
fi

OUT=$HERE/build
BUNDLE=$OUT/HakiosMetal.bundle
COMMON=(-arch arm64 -isysroot "$SDK" -O1 -Wall -Wextra -Wno-unused-parameter -fblocks
	-I"$HERE" -I"$PROTOCOL_DIR")
LIBS=(-framework Foundation -framework Metal -lobjc)

rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"

echo "build.sh: CC=$CC"
echo "build.sh: SDK=$SDK"
echo "build.sh: protocol=$PROTOCOL_DIR/metalwire_protocol.h"

# The bundle: the device, its object classes and the wire.
"$CC" "${COMMON[@]}" -bundle "${LIBS[@]}" \
	-o "$BUNDLE/HakiosMetal" "$HERE/HakiosMetal.m" "$HERE/mw_wire.c"
cp "$HERE/Info.plist" "$BUNDLE/Info.plist"
chmod 644 "$BUNDLE/Info.plist"

# The experiment's two clients. They speak Metal only; the bundle owns the wire.
"$CC" "${COMMON[@]}" "${LIBS[@]}" -o "$OUT/metal_frame" "$HERE/metal_frame.m"
"$CC" "${COMMON[@]}" "${LIBS[@]}" -o "$OUT/metal_probe" "$HERE/metal_probe.m"

# What the lead copies to /redwood/lib/HakiosMetal.clients (beside the bundle directory).
printf 'metal_frame\nmetal_probe\n' > "$OUT/HakiosMetal.clients"
chmod 644 "$OUT/HakiosMetal.clients"

echo
echo "build.sh: files"
file "$BUNDLE/HakiosMetal" "$BUNDLE/Info.plist" "$OUT/metal_frame" "$OUT/metal_probe"

OTOOL=
for candidate in llvm-otool-18 llvm-otool otool; do
	if command -v "$candidate" >/dev/null 2>&1; then
		OTOOL=$candidate
		break
	fi
done
if [ -n "$OTOOL" ]; then
	echo
	echo "build.sh: load commands ($OTOOL)"
	for binary in "$BUNDLE/HakiosMetal" "$OUT/metal_frame" "$OUT/metal_probe"; do
		echo "--- ${binary#"$HERE"/}"
		"$OTOOL" -l "$binary" | grep -E 'cmd LC_BUILD_VERSION|platform |sdk |minos |cmd LC_LOAD_DYLIB|cmd LC_MAIN|      name ' |
			sed 's/^ *//' | head -24
	done
fi

echo
echo "bundle:  $BUNDLE"
echo "clients: $OUT/metal_frame $OUT/metal_probe; install as /redwood/lib/HakiosMetal.clients"

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Install libcainject.dylib and its placeholder resource into the experiment's private guest
# root staging, so that the next tools/metalwire/prepare-guest.sh run puts them in the root image.
#
#   guest/ca-metal-inject/install.sh [MODE]
#
# MODE=on (default)  add /redwood/lib/libcainject.dylib for backboardd to etc/inject.conf
# MODE=off           remove that line again
#
# env: MT_WORK (default /home/lshan/DSH-desktop/mt-work)
#
# The image has to be rebuilt after this: prepare-guest.sh <mode> does that (its mkapfs step copies
# the staging tree as it is at that moment), then run-guest.sh boots it.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MT=${MT_WORK:-/home/lshan/DSH-desktop/mt-work}
STAGING=$MT/staging
MODE=${1:-on}
LINE='/redwood/lib/libcainject.dylib backboardd'

say() { printf 'ca-inject-install: %s\n' "$*"; }
die() { printf 'ca-inject-install: %s\n' "$*" >&2; exit 1; }

[ -f "$HERE/build/libcainject.dylib" ] || die "no $HERE/build/libcainject.dylib: run $HERE/build.sh first"
[ -d "$STAGING" ] || die "no $STAGING: run tools/metalwire/make-worktree.sh first"

sudo install -d "$STAGING/lib" "$STAGING/share"
sudo install -m 0644 "$HERE/build/libcainject.dylib" "$STAGING/lib/libcainject.dylib"
# The URL the injected library returns for a nil QuartzCore metallib lookup. It is our own file:
# 64 bytes that claim to be an MTLB container and contain no function. Nothing Apple's.
sudo python3 - "$STAGING/share/ca-default.metallib" <<'PY'
import struct, sys
header = b'MTLB' + struct.pack('<I', 0x0102) + b'\0' * 56
open(sys.argv[1], 'wb').write(header)
PY

case "$MODE" in
on)
	sudo python3 - "$STAGING/etc/inject.conf" "$LINE" <<'PY'
import sys
path, line = sys.argv[1], sys.argv[2]
lines = [l.rstrip('\n') for l in open(path)] if __import__('os').path.exists(path) else []
lines = [l for l in lines if l.strip()]
if line not in lines:
    lines.append(line)
open(path, 'w').write('\n'.join(lines) + '\n')
PY
	say "installed; etc/inject.conf now:"
	sudo cat "$STAGING/etc/inject.conf"
	;;
off)
	sudo python3 - "$STAGING/etc/inject.conf" "$LINE" <<'PY'
import sys
path, line = sys.argv[1], sys.argv[2]
try:
    lines = [l.rstrip('\n') for l in open(path)]
except FileNotFoundError:
    sys.exit(0)
lines = [l for l in lines if l.strip() and l.strip() != line]
open(path, 'w').write('\n'.join(lines) + '\n')
PY
	say "removed; rebuild the image with prepare-guest.sh to take effect"
	;;
*)
	die "unknown mode $MODE (on|off)"
	;;
esac

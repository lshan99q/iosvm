#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# screendump.sh — capture the guest's screen as a PNG.
#
#   tools/screendump.sh                      # logs/screen.png from the running VM's monitor socket
#   tools/screendump.sh -o shots/a.png       # explicit output
#   tools/screendump.sh --ppm /tmp --from logs/qemu.log   # keep the raw PPM too
#
# Talks to QEMU's monitor over a unix socket (run the VM with -monitor unix:logs/mon.sock,server,nowait).
# Convert a PPM to PNG with any of: python3 (built in below), ImageMagick, netpbm.

set -euo pipefail

PROG=${0##*/}
OUT=logs/screen.png
MON=logs/mon.sock
PPM_DIR=
KEEP_PPM=0

usage() { sed -n '2,11p' "$0" | sed 's/^# \{0,1\}//'; }
die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case $1 in
		-o) OUT=${2:?}; shift ;;
		--monitor) MON=${2:?}; shift ;;
		--ppm) PPM_DIR=$2; KEEP_PPM=1; shift ;;
		-h|--help) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
	esac
	shift
done

[ -S "$MON" ] || die "no monitor socket at $MON — start the VM with -monitor unix:$MON,server,nowait"

mkdir -p "$(dirname "$OUT")" "${PPM_DIR:-$(dirname "$OUT")}"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# QEMU writes the PPM at the path we give it.
target=${PPM_DIR:-$tmp}/screen.ppm
printf 'screendump %s\n' "$target" | socat - "UNIX-CONNECT:$MON" >/dev/null 2>&1 ||
	die "could not talk to the monitor (socat missing, or the monitor is not there)"
for _ in $(seq 20); do [ -s "$target" ] && break; sleep 0.1; done
[ -s "$target" ] || die "QEMU did not produce $target"

python3 - "$target" "$OUT" <<'PY'
import sys, zlib, struct
ppm, png = sys.argv[1], sys.argv[2]
data = open(ppm, "rb").read()
# P6 <w> <h> <max>\n then binary RGB
fields, pos = [], 0
while len(fields) < 4:
    while data[pos:pos + 1].isspace():
        pos += 1
    start = pos
    while not data[pos:pos + 1].isspace():
        pos += 1
    fields.append(data[start:pos])
pos += 1
magic, w, h, maxv = fields[0], int(fields[1]), int(fields[2]), int(fields[3])
assert magic == b"P6", f"expected a binary PPM (P6), got {magic!r}"
assert maxv == 255, f"only 8-bit PPM is handled, got maxval {maxv}"
raw = data[pos:pos + w * h * 3]
assert len(raw) == w * h * 3, f"truncated PPM: {len(raw)} of {w*h*3} bytes"
stride = w * 3
rows = b"".join(b"\x00" + raw[y * stride:(y + 1) * stride] for y in range(h))
def chunk(tag, payload):
    return struct.pack(">I", len(payload)) + tag + payload + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)
out = (b"\x89PNG\r\n\x1a\n"
       + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
       + chunk(b"IDAT", zlib.compress(rows, 6))
       + chunk(b"IEND", b""))
open(png, "wb").write(out)
print(f"{png}: {w}x{h}, PNG")
PY

[ "$KEEP_PPM" = 1 ] && printf '%s\n' "kept $target"
printf '%s\n' "$OUT"

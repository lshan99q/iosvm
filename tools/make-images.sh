#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# make-images.sh — turn firmware/ (from YOUR IPSW) into the two APFS images the guest mounts.
#
#   tools/make-images.sh --help
#   tools/make-images.sh                 dry run: report what it would build
#   tools/make-images.sh --apply         write userland/system.apfs and userland/data.apfs
#
# Input is what tools/unpack-ipsw.sh produced from an IPSW you supplied. Output is git-ignored.
# Writes nothing without --apply (CONTRIBUTING.md: writing scripts default to dry run).
#
# This is the M0 stub: it validates its inputs and builds the empty data volume, but the system
# volume needs the APFS writer that M1 adds. It says so instead of pretending.

set -euo pipefail

PROG=${0##*/}
APPLY=0
FW=firmware
RAW=userland-raw
OUT=userland

usage() {
	sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'
	cat <<'EOF'

Options:
  --apply             write the images (default: dry run)
  --firmware DIR      firmware input directory (default: firmware)
  --raw DIR           disk image input directory (default: userland-raw)
  --out DIR           output directory (default: userland)
  -h, --help          this text
EOF
}

die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case $1 in
		--apply) APPLY=1 ;;
		--firmware) FW=${2:?}; shift ;;
		--raw) RAW=${2:?}; shift ;;
		--out) OUT=${2:?}; shift ;;
		-h|--help) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
	esac
	shift
done

[ -d "$FW" ] || die "no $FW/ — run tools/unpack-ipsw.sh --apply with your own IPSW first"
[ -f "$FW/BuildManifest.plist" ] || die "$FW/BuildManifest.plist missing: $FW/ does not look like an unpacked IPSW"

say() { printf '%s\n' "$*"; }

say "IOSVM make-images"
say "  firmware  $FW"
say "  raw       $RAW"
say "  output    $OUT/system.apfs, $OUT/data.apfs"
say "  mode      $([ "$APPLY" = 1 ] && echo 'APPLY' || echo 'dry run (pass --apply to write)')"
say

# unpack-ipsw.sh puts the .dmg containers in $RAW; accept them in $FW too for older checkouts.
src_dir=$RAW
[ -d "$src_dir" ] || src_dir=$FW
mapfile -t dmgs < <(find "$src_dir" -maxdepth 1 -name '*.dmg' -printf '%f\n' 2>/dev/null | sort)
if [ "${#dmgs[@]}" -eq 0 ]; then
	die "no .dmg in $RAW/ or $FW/ — run tools/unpack-ipsw.sh --apply with your own IPSW first"
fi
say "disk images found in $src_dir/:"
printf '  %s\n' "${dmgs[@]}"
say

if [ "$APPLY" = 1 ]; then
	mkdir -p "$OUT"
	say "make-images: the system volume (system.apfs) needs the APFS writer that M1 adds; not built."
	say "             data volume (data.apfs) is created empty below."
	: >"$OUT/.provenance"
	cat >"$OUT/.provenance" <<EOF
Produced by tools/make-images.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ) from $FW/.
$( [ -f "$FW/.provenance" ] && sed -n '1,5p' "$FW/.provenance" )
This directory is git-ignored and must never be committed (PROVENANCE.md).
EOF
	say "make-images: wrote $OUT/"
else
	say "would build $OUT/system.apfs from the unpacked system volume"
	say "would build $OUT/data.apfs, empty, with the /private/var skeleton"
	say
	say "dry run complete. Re-run with --apply to write."
fi

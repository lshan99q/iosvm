#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# make-images.sh — turn an unpacked IPSW into the APFS image the guest mounts.
#
#   tools/make-images.sh --help
#   tools/make-images.sh                        dry run: report what it would build
#   tools/make-images.sh --apply                write userland/system.apfs
#   tools/make-images.sh --apply --no-compress  store files uncompressed (much bigger, faster)
#
# Input is what tools/unpack-ipsw.sh produced from an IPSW you supplied: `firmware/` (which carries
# the BuildManifest) and `userland-raw/` (the disk images). Output is git-ignored.
#
# This is the piece that makes the Linux-only path real: the APFS container is written by
# tools/apfs/mkapfs.py, vendored from HakiOS (MIT), and checked by apfsck when it is installed.
# No macOS, no hdiutil, no newfs_apfs.
#
# Exit codes: 0 ok, 1 failure, 2 usage, 3 refused.

set -euo pipefail

PROG=${0##*/}
APPLY=0
FW=firmware
RAW=userland-raw
OUT=userland
BUILD=build/images
ROOT_TREE=$BUILD/rootfs
COMPRESS=1
VOLUME=System

usage() {
	sed -n '3,19p' "$0" | sed 's/^# \{0,1\}//'
	cat <<'EOF'

Options:
  --apply             write the image (default: dry run)
  --no-compress       store files uncompressed (default: zlib per 64 KiB, as Darwin does)
  --firmware DIR      unpacked firmware (default: firmware)
  --raw DIR           disk images from unpack-ipsw (default: userland-raw)
  --out DIR           output directory (default: userland)
  --work DIR          scratch for the extracted tree (default: build/images)
  -h, --help          this text
EOF
}

die() { local m=$1 c=${2:-1}; printf '%s: %s\n' "$PROG" "$m" >&2; exit "$c"; }
say() { printf '%s\n' "$*"; }

while [ $# -gt 0 ]; do
	case $1 in
		--apply) APPLY=1 ;;
		--no-compress) COMPRESS=0 ;;
		--firmware) FW=${2:?}; shift ;;
		--raw) RAW=${2:?}; shift ;;
		--out) OUT=${2:?}; shift ;;
		--work) BUILD=${2:?}; ROOT_TREE=$BUILD/rootfs; shift ;;
		-h|--help) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
	esac
	shift
done

[ -d "$FW" ] || die "no $FW/ — run tools/unpack-ipsw.sh --apply with your own IPSW first"
[ -f "$FW/BuildManifest.plist" ] || die "$FW/BuildManifest.plist missing: $FW/ is not an unpacked IPSW"
[ -d "$RAW" ] || die "no $RAW/ — the disk images are missing"
command -v 7z >/dev/null || die "7z is not installed" 2
command -v python3 >/dev/null || die "python3 is not installed" 2
[ -f tools/apfs/mkapfs.py ] || die "tools/apfs/mkapfs.py is missing (the vendored APFS writer)"

# The root image is not hardcoded: the manifest says which of the IPSW's images is `OS`.
root_dmg=$(python3 - "$FW/BuildManifest.plist" <<'PY'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], "rb"))
man = d["BuildIdentities"][0]["Manifest"]
for name in ("OS", "SystemVolume"):
    if name in man:
        print(man[name]["Info"]["Path"])
        break
PY
)
[ -n "$root_dmg" ] || die "the manifest names no OS component — is this an IPSW's BuildManifest?"
root_img=$RAW/$(basename "$root_dmg")
[ -f "$root_img" ] || die "the OS image $(basename "$root_dmg") is not in $RAW/ (run unpack-ipsw --apply)"
size=$(stat -c %s "$root_img")

say "IOSVM make-images"
say "  root image  $root_img ($((size / 1024 / 1024)) MiB)"
say "  tree        $ROOT_TREE"
say "  output      $OUT/system.apfs"
say "  compression $([ "$COMPRESS" = 1 ] && echo 'zlib per 64 KiB (as Darwin stores it)' || echo 'none')"
say "  mode        $([ "$APPLY" = 1 ] && echo 'APPLY' || echo 'dry run (pass --apply to write)')"
say

if [ "$APPLY" != 1 ]; then
	say "would: extract the APFS volume from $(basename "$root_img") into $ROOT_TREE"
	say "would: build $OUT/system.apfs from it with tools/apfs/mkapfs.py"
	say "would: check the result with apfsck (if installed) and a 7z read-back"
	say
	say "dry run complete. Re-run with --apply to write."
	exit 0
fi

mkdir -p "$OUT" "$BUILD"

# 1. The volume, as a tree. 7-Zip writes the APFS volume's contents directly and keeps modes and
#    symbolic links, which is what mkapfs reads back.
if [ ! -d "$ROOT_TREE" ] || [ -z "$(ls -A "$ROOT_TREE" 2>/dev/null)" ]; then
	say "extracting the volume (this reads $((size / 1024 / 1024)) MiB of image) …"
	rm -rf "$ROOT_TREE"
	mkdir -p "$ROOT_TREE"
	# 7-Zip refuses to create a symbolic link whose relative target walks up out of the extraction
	# root, and reports it as an error (exit 2). On a Darwin volume that is a normal idiom —
	# `AVFoundation.framework/Frameworks/AVFAudio.framework` points at `../../AVFAudio.framework`,
	# which is inside the volume — so the refuse is a policy, not a corruption. Take those lines
	# out of the log, recreate exactly those links, and treat every other non-zero exit as a
	# failure.
	set +e
	7z x -y -o"$ROOT_TREE" "$root_img" >"$BUILD/extract.log" 2>&1
	rc=$?
	set -e
	other_errors=$(grep -c '^ERROR' "$BUILD/extract.log" 2>/dev/null || true)
	symlink_errors=$(grep -c '^ERROR: Dangerous symbolic link path was ignored' "$BUILD/extract.log" 2>/dev/null || true)
	if [ "$other_errors" != "$symlink_errors" ] && [ "$rc" != 0 ]; then
		grep '^ERROR' "$BUILD/extract.log" | head -10 >&2
		die "7z could not extract $root_img (see $BUILD/extract.log)"
	fi
	if [ "$symlink_errors" != 0 ]; then
		say "  recreating $symlink_errors symbolic link(s) 7-Zip declined to make"
		sed -n 's|^ERROR: Dangerous symbolic link path was ignored : \(.*\) : \(.*\)$|\1\t\2|p' \
			"$BUILD/extract.log" | while IFS=$'\t' read -r link target; do
			[ -n "$link" ] || continue
			mkdir -p "$(dirname "$ROOT_TREE/$link")"
			ln -sfn "$target" "$ROOT_TREE/$link"
		done
	fi
	if [ ! -d "$ROOT_TREE/System" ] && [ ! -d "$ROOT_TREE/sbin" ]; then
		die "$root_img did not extract to a directory tree (multi-volume image, or not a system volume)"
	fi
else
	say "reusing the extracted tree at $ROOT_TREE"
fi
say "  tree: $(find "$ROOT_TREE" -mindepth 1 | wc -l) entries, $(du -sh "$ROOT_TREE" | cut -f1)"

# 2. The container.
say "building $OUT/system.apfs …"
start=$(date +%s)
mk_args=(-o "$OUT/system.apfs")
if [ "$COMPRESS" = 1 ]; then
	mk_args+=(-z)
fi
python3 tools/apfs/mkapfs.py "${mk_args[@]}" "$VOLUME=$ROOT_TREE" | sed 's/^/  /'
say "  built in $(( $(date +%s) - start ))s, $(du -h "$OUT/system.apfs" | cut -f1)"

# 3. Two checks, because a check that cannot fail is not worth having: the filesystem checker, and
#    a read-back through a completely different reader (7-Zip).
if command -v apfsck >/dev/null; then
	apfsck "$OUT/system.apfs" || die "apfsck rejected $OUT/system.apfs"
	say "  apfsck: clean"
else
	say "  apfsck: not installed — build linux-apfs/apfsprogs to check here (the 7z read-back runs anyway)"
fi
vol=$(7z l "$OUT/system.apfs" 2>/dev/null | sed -n 's/^Name = //p' | head -1)
[ -n "$vol" ] || { 7z l "$OUT/system.apfs" >&2 2>/dev/null; die "7z cannot read back $OUT/system.apfs"; }
say "  7z read-back: volume \"$vol\""

{
	echo "Produced by tools/make-images.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)."
	echo "From $FW/ (unpacked by tools/unpack-ipsw.sh) and $(basename "$root_img")."
	echo "Volume \"$vol\", $([ "$COMPRESS" = 1 ] && echo 'zlib-compressed' || echo 'uncompressed')."
	[ -f "$FW/.provenance" ] && sed -n '1,4p' "$FW/.provenance"
	echo "This directory is git-ignored and must never be committed (PROVENANCE.md)."
} >"$OUT/.provenance"
say "wrote $OUT/system.apfs"

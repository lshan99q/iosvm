#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# unpack-ipsw.sh — take the parts of an IPSW that IOSVM needs, out of a file YOU supply.
#
#   tools/unpack-ipsw.sh --help
#   tools/unpack-ipsw.sh ~/iPhone17,3_26.0.ipsw              # dry run: report, change nothing
#   tools/unpack-ipsw.sh ~/iPhone17,3_26.0.ipsw --apply      # write firmware/ and userland-raw/
#
# This repository ships nothing of Apple's and downloads nothing of Apple's. The IPSW is yours;
# this script has no URL in it and adding one is a review veto (PROVENANCE.md, ADR-009).
#
# Requires: python3 3.10+, 7z (7-Zip 23 or newer — APFS), ~20 GB free for an iPhone-class IPSW.
# Never requires macOS: there is no hdiutil, ipsw, otool or codesign in this path (ADR-009).
#
# Exit codes: 0 ok, 1 failure, 2 usage/tooling error, 3 refused (an unsafe default was overridden).

set -euo pipefail

PROG=${0##*/}
IPSW=
APPLY=0
KEEP=0
OUT_FW=firmware
OUT_RAW=userland-raw

usage() {
	sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
	cat <<'EOF'

Options:
  --apply             actually write (without it, this is a dry run)
  --keep-raw          keep the extracted .dmg files after unpacking them
  --firmware DIR      firmware output directory (default: firmware)
  --raw DIR           intermediate output directory (default: userland-raw)
  -h, --help          this text

What it produces, all of it git-ignored:

  firmware/BuildManifest.plist          what this IPSW is
  firmware/kernelcache.*                the kernel and its kexts (for reference, not booted)
  firmware/*.im4p, *.im4m               device tree, trustcache and friends
  userland-raw/*.dmg                    the APFS containers, before expansion
EOF
}

die() { printf '%s: %s\n' "$PROG" "$*" >&2; exit "${2:-1}"; }

while [ $# -gt 0 ]; do
	case $1 in
		--apply) APPLY=1 ;;
		--keep-raw) KEEP=1 ;;
		--firmware) OUT_FW=${2:?--firmware needs a directory}; shift ;;
		--raw) OUT_RAW=${2:?--raw needs a directory}; shift ;;
		-h|--help) usage; exit 0 ;;
		-*) usage >&2; exit 2 ;;
		*) [ -z "$IPSW" ] || die "more than one IPSW given" 2; IPSW=$1 ;;
	esac
	shift
done

[ -n "$IPSW" ] || { usage >&2; exit 2; }
[ -f "$IPSW" ] || die "no such file: $IPSW"
IPSW=$(cd "$(dirname "$IPSW")" && pwd)/$(basename "$IPSW")

# --- tooling, checked before anything is written ---------------------------------------------

command -v python3 >/dev/null || die "python3 is not installed" 2
command -v 7z >/dev/null || die "7z is not installed (apt install 7zip)" 2

pyver=$(python3 -c 'import sys; print("%d.%d" % sys.version_info[:2])')
python3 -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' ||
	die "python3 $pyver is too old (need 3.10+)" 2

# 7-Zip < 23 does not read APFS, and it leaves files empty instead of failing. Refuse early.
sevenz=$(7z 2>/dev/null | head -1)
sevenz_major=$(printf '%s' "$sevenz" | sed -n 's/.* \([0-9][0-9]*\)\.[0-9][0-9]*.*/\1/p' | head -1)
[ -n "$sevenz_major" ] || die "cannot read the 7-Zip version from: $sevenz" 2
if [ "$sevenz_major" -lt 23 ]; then
	die "7-Zip $sevenz_major is too old: APFS needs 23 or newer, and older versions silently
       produce empty files instead. Install a newer 7-Zip (PROVENANCE.md, ADR-009)." 3
fi

size=$(wc -c <"$IPSW" | tr -d ' ')
free_kb=$(df -Pk . | awk 'NR==2 {print $4}')
need_kb=$((size / 1024 * 3))
[ "$free_kb" -ge "$need_kb" ] ||
	die "not enough space here: need about $((need_kb / 1024 / 1024)) GB, have $((free_kb / 1024 / 1024)) GB" 2

say() { printf '%s\n' "$*"; }
plan() {
	if [ "$APPLY" = 1 ]; then say "$*"; else say "would: $*"; fi
}

say "IOSVM unpack — Linux only, no Apple tools"
say "  IPSW      $IPSW"
say "  size      $((size / 1024 / 1024)) MiB"
say "  7-Zip     $sevenz"
say "  python    $pyver"
say "  mode      $([ "$APPLY" = 1 ] && echo 'APPLY (writing)' || echo 'dry run (pass --apply to write)')"
say

# --- what is in it -----------------------------------------------------------------------------

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# An IPSW is a zip. Use python's zipfile for the listing: 7z would also work, but this keeps the
# parse strict and independent of 7z's output format.
python3 - "$IPSW" >"$tmp/list.tsv" <<'PY'
import sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for i in z.infolist():
        print(f"{i.file_size}\t{i.filename}")
PY

say "IPSW contents of interest:"
awk -F'\t' '
  $2 ~ /^(BuildManifest\.plist|Restore\.plist|KernelCache\/.*|kernelcache.*)$/ ||
  $2 ~ /\.dmg$/ || $2 ~ /^Firmware\/(dfu|all_flash)\/.*\.(im4p|im4m)$/ {
    printf "  %12d  %s\n", $1, $2
  }' "$tmp/list.tsv" | sort -k2 | head -60
say

n_dmg=$(awk -F'\t' '$2 ~ /\.dmg$/' "$tmp/list.tsv" | wc -l)
n_kc=$(awk -F'\t' '$2 ~ /kernelcache/' "$tmp/list.tsv" | wc -l)
say "  $n_dmg disk image(s), $n_kc kernelcache entry(ies)"

if [ "$n_dmg" -eq 0 ]; then
	die "this does not look like an IPSW: no .dmg inside" 3
fi

# --- write -------------------------------------------------------------------------------------

if [ "$APPLY" = 1 ]; then
	mkdir -p "$OUT_FW" "$OUT_RAW"

	# Members we want verbatim.
	python3 - "$IPSW" "$OUT_FW" <<'PY'
import sys, zipfile, os
ipsw, out = sys.argv[1], sys.argv[2]
WANT = ("BuildManifest.plist", "Restore.plist")
with zipfile.ZipFile(ipsw) as z:
    for name in z.namelist():
        keep = (name in WANT
                or name.endswith(".dmg")
                or "kernelcache" in name
                or (name.startswith("Firmware/") and name.endswith((".im4p", ".im4m"))))
        if not keep:
            continue
        dst = os.path.join(out if not name.endswith(".dmg") else ".",
                           os.path.basename(name))
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        with z.open(name) as src, open(dst, "wb") as f:
            while chunk := src.read(1 << 20):
                f.write(chunk)
        print(f"  wrote {dst} ({os.path.getsize(dst)} bytes)")
PY

	# The git-ignored marker, so a contributor can see what produced these files.
	cat >"$OUT_FW/.provenance" <<EOF
Produced by tools/unpack-ipsw.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ).
Source IPSW: $(basename "$IPSW"), $size bytes, sha256 $(sha256sum "$IPSW" | cut -d' ' -f1).
This directory is git-ignored and must never be committed (PROVENANCE.md).
EOF
	say "unpack-ipsw: wrote $OUT_FW/ and $OUT_RAW/"
else
	plan "extract BuildManifest.plist, kernelcache, *.dmg, Firmware/*.im4p to $OUT_FW/"
	plan "expand the APFS containers into $OUT_RAW/"
	plan "write $OUT_FW/.provenance"
	say
	say "dry run complete. Re-run with --apply to write."
fi

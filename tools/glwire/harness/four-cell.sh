#!/bin/bash
# four-cell.sh -- P4: the four builds, one 20 s scroll window each, one table.
#
# This is a *skeleton*: it does not build the four images and it does not run by itself
# unless each image is given.  It drives the existing drive-scroll.sh, so the window is the
# same "eight swipes, about 20 s" the earlier P2''' table used, measured *after* the
# interface has settled (WINDOW_AFTER).  Same QEMU_SMP=1 and same touch script for all four.
#
# The four cells (docs/gpu-acceleration-stage19-plan.md P4):
#
#   cell1  pure local     no recorder in the image
#   cell2  by-reference   GLWIRE_LOCAL=1 (guest draws; surfaces uploaded by reference)
#   cell3  W1.5           remote_only, host --no-panel replays
#   cell4  W1.5 + W3      same build as W1.5, W3 (in-place row write) on
#
# The images are built outside this script.  Give each one explicitly:
#
#   CELL1_IMAGE=/tmp/root-local.apfs \
#   CELL2_IMAGE=/tmp/root-byref.apfs \
#   CELL3_IMAGE=/tmp/root-w15.apfs \
#   CELL4_IMAGE=/tmp/root-w15w3.apfs \
#   ./four-cell.sh
#
# --dry-run reads existing /tmp/<label>-{host.log,log,shot.png,drive.txt} and prints the same
# table without touching QEMU or the GPU.  Useful for checking the arithmetic on a previous run.
#
# env: CELL1_IMAGE..CELL4_IMAGE  mandatory in run mode
#      CELL1_LABEL..CELL4_LABEL  default cell1..cell4
#      CELL1_XCMD..CELL4_XCMD    optional per-cell guest boot args (e.g. "glwire_local=1")
#      CELL1_HOST_ENV..CELL4_HOST_ENV  optional per-cell host env
#      CELL1_HOST_ARGS..CELL4_HOST_ARGS optional per-cell extra host_glwire args
#      WINDOW_AFTER=60 MAX_SECS=1800 MINSHOT=200000 NOUI_BY=1800
#      OUT=/tmp/four-cell
#      IOSVM=... GLWIRE=...
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
GLWIRE=${GLWIRE:-$(cd "$HERE/.." && pwd)}
IOSVM=${IOSVM:-/host/iosvm-work/hakios-xnu-kernel}
OUT=${OUT:-/tmp/four-cell}
WINDOW_AFTER=${WINDOW_AFTER:-60}
MAX_SECS=${MAX_SECS:-1800}
MINSHOT=${MINSHOT:-200000}
NOUI_BY=${NOUI_BY:-1800}
SMP=${QEMU_SMP:-1}
DRY=0
if [ "${1:-}" = "--dry-run" ]; then
	DRY=1
	shift
fi

mkdir -p "$OUT"
TABLE_MD="$OUT/four-cell.md"
TABLE_CSV="$OUT/four-cell.csv"

cell_image() { eval "printf '%s' \"\${CELL$1_IMAGE:-}\""; }
cell_label() {
	case $1 in
	1) printf '%s' "${CELL1_LABEL:-pure-local}" ;;
	2) printf '%s' "${CELL2_LABEL:-by-reference}" ;;
	3) printf '%s' "${CELL3_LABEL:-w15}" ;;
	4) printf '%s' "${CELL4_LABEL:-w15-w3}" ;;
	esac
}
cell_xcmd()  { eval "printf '%s' \"\${CELL$1_XCMD:-}\""; }
cell_henv()  { eval "printf '%s' \"\${CELL$1_HOST_ENV:-}\""; }
cell_hargs() { eval "printf '%s' \"\${CELL$1_HOST_ARGS:-}\""; }

# One row from an existing set of logs.  Prints the markdown row (without the leading |).
row_from_logs() {
	local label=$1 image=$2
	local hlog="$OUT/$label-host.log" glog="$OUT/$label-guest.log"
	local drive="$OUT/$label-drive.txt" shot="$OUT/$label-shot.png"
	# drive-scroll writes /tmp/<label>-*; accept either location.
	[ -f "$hlog" ] || hlog="/tmp/$label-host.log"
	[ -f "$glog" ] || glog="/tmp/$label.log"
	[ -f "$drive" ] || drive="/tmp/$label-drive.txt"
	[ -f "$shot" ] || shot="/tmp/$label-shot.png"

	if [ ! -f "$hlog" ] || [ ! -f "$drive" ]; then
		printf '%s | SKIP | - | - | - | - | no /tmp/%s-*.log' "$label" "$label"
		return
	fi
	local line ui fps frames span why
	line=$(grep -E '^drive: ui=[0-9]+s (window|frame-window)' "$drive" | tail -1)
	if [ -z "$line" ]; then
		line=$(grep -E '^drive: (no interface|panic)' "$drive" | tail -1)
		printf '%s | %s | - | - | - | - | -' "$label" "${line:-no drive line}"
		return
	fi
	# (the line continues with a "; guest ..." half that has its own fps: cut it off first)
	local headline=${line%%;*}
	ui=$(printf '%s\n' "$line" | sed -n 's/^drive: ui=\([0-9]*\)s.*/\1/p')
	frames=$(printf '%s\n' "$headline" | sed -n 's/.*frames \([0-9]*\) ->.*/\1/p')
	fps=$(printf '%s\n' "$headline" | sed -n 's/.*-> \([0-9.]*\) fps.*/\1/p')
	span=$(printf '%s\n' "$headline" | sed -n 's/.*window \([0-9.]*\)s.*/\1/p')
	local shot_bytes=0 errors=0 end=""
	[ -f "$shot" ] && shot_bytes=$(stat -Lc%s "$shot" 2>/dev/null || echo 0)
	errors=$(grep -c 'glerror 0x502' "$hlog" 2>/dev/null || true)
	end=$(grep -E '^xb: ' "$glog" 2>/dev/null | tail -1 | sed 's/.*end=//; s/ .*//')
	printf '%s | %s | %s | %s | %s | %s | %s' "$label" "${ui:--}s" "${frames:--}" \
	    "${fps:--}" "${span:--}s" "$shot_bytes" "$errors"
}

{
	echo "# P4 four-cell table (20 s scroll; QEMU_SMP=$SMP, WINDOW_AFTER=${WINDOW_AFTER}s)"
	echo
	echo "| cell | ui | frames | fps | span | shot B | host 0x502 |"
	echo "|---|---|---|---|---|---|---|"
} > "$TABLE_MD"
echo "cell,ui_s,frames,fps,span_s,shot_bytes,host_0x502" > "$TABLE_CSV"

for n in 1 2 3 4; do
	label=$(cell_label "$n")
	image=$(cell_image "$n")
	xcmd=$(cell_xcmd "$n")
	henv=$(cell_henv "$n")
	hargs=$(cell_hargs "$n")

	if [ "$DRY" = 0 ]; then
		if [ -z "$image" ] || [ ! -f "$image" ]; then
			echo "four-cell: cell$n ($label): no image (set CELL${n}_IMAGE); skipped" >&2
			row="$label | SKIP | - | - | - | - | no image"
			echo "| $row |" >> "$TABLE_MD"
			echo "$label,,,,,,no image" >> "$TABLE_CSV"
			continue
		fi
		root="$image"
		echo "four-cell: cell$n ($label) $root" >&2
		# Same touch script and same 20 s window for every cell: drive-scroll's default
		# WINDOW_FRAMES=0 is the eight-swipe ~20 s window.
		env QEMU_SMP="$SMP" ZERO_RAM=1 MINSHOT="$MINSHOT" NOUI_BY="$NOUI_BY" \
		    WINDOW_AFTER="$WINDOW_AFTER" WINDOW_FRAMES=0 \
		    XCMD="$xcmd" HOST_ENV="$henv" \
		    IOSVM="$IOSVM" GLWIRE="$GLWIRE" \
		    "$HERE/drive-scroll.sh" "$label" "$root" "$MAX_SECS" $hargs \
		    >"$OUT/$label-drive.log" 2>&1 || echo "four-cell: cell$n failed, row will say why" >&2
		# drive-scroll writes to /tmp; copy the four artifacts beside the table.
		cp -f "/tmp/$label.log" "$OUT/$label-guest.log" 2>/dev/null || true
		cp -f "/tmp/$label-host.log" "$OUT/$label-host.log" 2>/dev/null || true
		cp -f "/tmp/$label-shot.png" "$OUT/$label-shot.png" 2>/dev/null || true
		cp -f "/tmp/$label-drive.txt" "$OUT/$label-drive.txt" 2>/dev/null || true
	fi
	row=$(row_from_logs "$label" "$image")
	echo "| $row |" >> "$TABLE_MD"
	# CSV: split on the same pipes, trim spaces.
	printf '%s\n' "$row" | awk -F' *\\| *' '{print $1","$2","$3","$4","$5","$6","$7}' >> "$TABLE_CSV"
done

{
	echo
	echo "0x502 is counted in the *host* log ('glerror 0x502'); the P4 criterion is 0."
	echo "The four images are built outside this script; the table only measures them."
} >> "$TABLE_MD"

echo "four-cell: wrote $TABLE_MD and $TABLE_CSV"
cat "$TABLE_MD"

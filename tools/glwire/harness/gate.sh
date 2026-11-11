#!/bin/bash
# gate.sh -- one cold boot, one PASS/FAIL line.
#
# usage: gate.sh --root <root.apfs> [--system <md5>] [--window 20|120]
#                [--out <dir>] [--minshot <bytes|10M>]
#
# Checks:
#   1. screenshot >= --minshot (default 300000 = 300 KB; MINSHOT env also works)
#   2. readable-name heuristic >= 5 (gate_names.py: white text lines; no OCR in the image)
#   3. host GL errors "glerror 0x502" == 0
#   4. guest "panic(" == 0 and "exited due to SIGSEGV" == 0
#   5. artifacts written: shot.png, guest.log, host.log, boot.log, md5s.txt, gate.md
#
# PASS line: GATE: PASS  (checks: shot=302K names=7 0x502=0 panic=0)
# FAIL prints that line plus one "GATE: FAIL reason: ..." line per failed check.
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
GLWIRE=$(cd "$HERE/.." && pwd)
IOSVM=${IOSVM:-/host/iosvm-work/hakios-xnu-kernel}
QEMU_EFI=${QEMU_EFI:-$IOSVM/qemu/efi64.fd}
QEMU_VARS=${QEMU_VARS:-$IOSVM/qemu/vars.fd}
QEMU_SMP=${QEMU_SMP:-1}
ROOT=
SYSTEM_MD5=
WINDOW=20
OUT=
MINSHOT=${MINSHOT:-300000}
BOOTMINSHOT=${BOOTMINSHOT:-300000}
MAXSECS=${MAXSECS:-1050}

usage() {
	cat <<'EOF'
usage: gate.sh --root <root.apfs> [--system <md5>] [--window 20|120]
               [--out <dir>] [--minshot <bytes|300K|10M>]

  --root      guest root APFS to boot (required)
  --system    expected md5 of userland/system.apfs (optional; mismatch is a failed check)
  --window    UI settle window after the first shot: 20 (default) or 120 seconds
  --out       artifact directory (default: /tmp/gate-<UTC timestamp>)
  --minshot   minimum screenshot bytes (default 300000; env MINSHOT also accepted)

Env: IOSVM=/host/iosvm-work/hakios-xnu-kernel, HOST_ENV=..., XCMD=..., MAXSECS=...
     BOOTMINSHOT=300000 (UI detection; default 300000, independent of --minshot check)
     IOSVM=/host/iosvm-work/hakios-xnu-kernel
     QEMU_EFI=$IOSVM/qemu/efi64.fd, QEMU_VARS=$IOSVM/qemu/vars.fd
     QEMU_SMP=1 (cell1 historical baseline is one vCPU; override for multi-core runs)
Exit: 0 PASS, 1 FAIL, 2 usage.
EOF
}

die_usage() { echo "gate.sh: $*" >&2; usage >&2; exit 2; }

while [ $# -gt 0 ]; do
	case "$1" in
		--root) [ $# -ge 2 ] || die_usage "--root needs a path"; ROOT=$2; shift 2 ;;
		--system) [ $# -ge 2 ] || die_usage "--system needs an md5"; SYSTEM_MD5=$2; shift 2 ;;
		--window) [ $# -ge 2 ] || die_usage "--window needs 20 or 120"; WINDOW=$2; shift 2 ;;
		--out) [ $# -ge 2 ] || die_usage "--out needs a directory"; OUT=$2; shift 2 ;;
		--minshot) [ $# -ge 2 ] || die_usage "--minshot needs bytes"; MINSHOT=$2; shift 2 ;;
		-h|--help) usage; exit 0 ;;
		*) die_usage "unknown argument $1" ;;
	esac
done
[ -n "$ROOT" ] || die_usage "--root is required"
[ -f "$ROOT" ] || { echo "gate.sh: root not found: $ROOT" >&2; exit 1; }
[ -f "$QEMU_EFI" ] || { echo "gate.sh: QEMU_EFI not found: $QEMU_EFI (set QEMU_EFI)" >&2; exit 1; }
[ -f "$QEMU_VARS" ] || { echo "gate.sh: QEMU_VARS not found: $QEMU_VARS (set QEMU_VARS)" >&2; exit 1; }
export IOSVM QEMU_EFI QEMU_VARS QEMU_SMP
case "$WINDOW" in 20|120) ;; *) die_usage "--window must be 20 or 120" ;; esac

# 300K / 10M / 300000
case "$MINSHOT" in
	*KB|*kb) MINSHOT=$(( ${MINSHOT%[Kk][Bb]} * 1024 )) ;;
	*MB|*mb) MINSHOT=$(( ${MINSHOT%[Mm][Bb]} * 1024 * 1024 )) ;;
	*K|*k) MINSHOT=$(( ${MINSHOT%[Kk]} * 1024 )) ;;
	*M|*m) MINSHOT=$(( ${MINSHOT%[Mm]} * 1024 * 1024 )) ;;
	''|*[!0-9]*) die_usage "--minshot must be bytes or N K/M" ;;
esac

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
LABEL=gate-$STAMP-$$
[ -n "$OUT" ] || OUT=/tmp/gate-$STAMP
mkdir -p "$OUT" || { echo "gate.sh: cannot create $OUT" >&2; exit 1; }
OUT=$(cd "$OUT" && pwd)
SHOT="$OUT/shot.png"
GUEST="$OUT/guest.log"
HOST="$OUT/host.log"
BOOT="$OUT/boot.log"
MD5S="$OUT/md5s.txt"
REPORT="$OUT/gate.md"
BOOTSHOT=/tmp/$LABEL-shot.png
BOOTGUEST=/tmp/$LABEL.log
BOOTHOST=/tmp/$LABEL-host.log

HOST_ENV=${HOST_ENV:-"GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16"}
export HOST_ENV
{ echo "gate: label=$LABEL"; echo "gate: root=$ROOT"; echo "gate: window=$WINDOW minshot=$MINSHOT";
  echo "gate: host_env=$HOST_ENV"; echo "gate: xcmd=${XCMD:-}";
  echo "gate: iosvm=$IOSVM"; echo "gate: qemu_efi=$QEMU_EFI"; echo "gate: qemu_vars=$QEMU_VARS";
  echo "gate: qemu_smp=$QEMU_SMP"; } > "$BOOT"
echo "gate: booting $ROOT (window ${WINDOW}s, minshot $MINSHOT), artifacts $OUT"
(
	cd "$GLWIRE" || exit 1
	MINFRAMES=0 REQUIRE_HOST_FRAMES=0 ZERO_RAM=1 MINSHOT="$BOOTMINSHOT" UI_GRACE="$WINDOW" \
		NOUI_BY=${NOUI_BY:-900} MAXSECS="$MAXSECS" XCMD="${XCMD:-}" HOST_ENV="$HOST_ENV" \
		./harness/boot-measure.sh "$LABEL" "$ROOT" "$MAXSECS" >> "$BOOT" 2>&1
)
BOOT_RC=$?

# boot-measure writes /tmp/<label>-shot.png (the biggest screenshot), /tmp/<label>.log,
# /tmp/<label>-host.log.  Copy whatever exists, then judge the copies.
[ -f "$BOOTSHOT" ] && cp -f "$BOOTSHOT" "$SHOT"
[ -f "$BOOTGUEST" ] && cp -f "$BOOTGUEST" "$GUEST"
[ -f "$BOOTHOST" ] && cp -f "$BOOTHOST" "$HOST"

SHOT_BYTES=$(stat -c '%s' "$SHOT" 2>/dev/null || echo 0)
SHOT_K=$(( (SHOT_BYTES + 1023) / 1024 ))
NAMES=0
if [ -s "$SHOT" ] && [ -x "$HERE/gate_names.py" ]; then
	NAMES=$(python3 "$HERE/gate_names.py" "$SHOT" 2>"$OUT/names.err" || true)
	case "$NAMES" in ''|*[!0-9]*) NAMES=0 ;; esac
fi
if [ -s "$HOST" ]; then
	GL502=$(grep -ac 'glerror 0x502' "$HOST" 2>/dev/null || true)
else
	GL502=0
fi
PANIC=0; SIGSEGV=0
if [ -s "$GUEST" ]; then
	PANIC=$(grep -ac 'panic(' "$GUEST" 2>/dev/null || true)
	SIGSEGV=$(grep -ac 'exited due to SIGSEGV' "$GUEST" 2>/dev/null || true)
fi
[ -n "${GL502:-}" ] || GL502=0
[ -n "${PANIC:-}" ] || PANIC=0
[ -n "${SIGSEGV:-}" ] || SIGSEGV=0

FAILS=()
[ "$SHOT_BYTES" -ge "$MINSHOT" ] || FAILS+=("screenshot ${SHOT_BYTES} B < ${MINSHOT} B")
[ "$NAMES" -ge 5 ] || FAILS+=("readable names ${NAMES} < 5 (gate_names.py heuristic)")
[ "$GL502" -eq 0 ] || FAILS+=("host glerror 0x502 x${GL502}")
[ "$PANIC" -eq 0 ] || FAILS+=("guest panic x${PANIC}")
[ "$SIGSEGV" -eq 0 ] || FAILS+=("guest SIGSEGV x${SIGSEGV}")
[ "$BOOT_RC" -eq 0 ] || FAILS+=("boot-measure exit ${BOOT_RC} (see boot.log)")
for f in "$SHOT" "$GUEST" "$HOST" "$BOOT"; do [ -s "$f" ] || FAILS+=("missing artifact $(basename "$f")"); done

if [ -f "$ROOT" ]; then
	ROOT_MD5=$(md5sum "$ROOT" | awk '{print $1}')
else
	ROOT_MD5=missing
fi
SYSTEM_FILE="$IOSVM/userland/system.apfs"
SYSTEM_ACTUAL=missing
[ -f "$SYSTEM_FILE" ] && SYSTEM_ACTUAL=$(md5sum "$SYSTEM_FILE" | awk '{print $1}')
if [ -n "$SYSTEM_MD5" ] && [ "$SYSTEM_ACTUAL" != "$SYSTEM_MD5" ]; then
	FAILS+=("system md5 ${SYSTEM_ACTUAL:0:12} != expected ${SYSTEM_MD5:0:12}")
fi
{
	echo "root_md5=$ROOT_MD5  $ROOT"
	echo "system_md5=$SYSTEM_ACTUAL  $SYSTEM_FILE"
	echo "shot_md5=$( [ -s "$SHOT" ] && md5sum "$SHOT" | awk '{print $1}' || echo missing )  shot.png"
	echo "guest_log_md5=$( [ -s "$GUEST" ] && md5sum "$GUEST" | awk '{print $1}' || echo missing )  guest.log"
	echo "host_log_md5=$( [ -s "$HOST" ] && md5sum "$HOST" | awk '{print $1}' || echo missing )  host.log"
} > "$MD5S"

RESULT=PASS
[ ${#FAILS[@]} -eq 0 ] || RESULT=FAIL
{
	echo "# GATE: $RESULT"
	echo
	echo "- root: \`$ROOT\`"
	echo "- system: \`$SYSTEM_ACTUAL\`"$([ -n "$SYSTEM_MD5" ] && echo " (expected \`$SYSTEM_MD5\`)")
	echo "- window: ${WINDOW}s; minshot: ${MINSHOT} B"
	echo "- boot-measure exit: ${BOOT_RC}"
	echo
	echo "| check | result | detail |"
	echo "|---|---|---|"
	echo "| screenshot >= ${MINSHOT} B | $([ "$SHOT_BYTES" -ge "$MINSHOT" ] && echo PASS || echo FAIL) | ${SHOT_BYTES} B (${SHOT_K}K) |"
	echo "| readable names >= 5 | $([ "$NAMES" -ge 5 ] && echo PASS || echo FAIL) | ${NAMES} (gate_names.py white-text line heuristic) |"
	echo "| host glerror 0x502 | $([ "$GL502" -eq 0 ] && echo PASS || echo FAIL) | ${GL502} |"
	echo "| guest panic | $([ "$PANIC" -eq 0 ] && echo PASS || echo FAIL) | ${PANIC} |"
	echo "| guest SIGSEGV | $([ "$SIGSEGV" -eq 0 ] && echo PASS || echo FAIL) | ${SIGSEGV} |"
	echo "| artifacts | $([ ${#FAILS[@]} -eq 0 ] && echo PASS || echo "see failures") | shot.png guest.log host.log boot.log md5s.txt |"
	echo
	if [ ${#FAILS[@]} -ne 0 ]; then
		echo "## Failures"
		for f in "${FAILS[@]}"; do echo "- $f"; done
	fi
	if [ -s "$OUT/names.err" ]; then
		echo
		echo "## names stderr"
		sed 's/^/    /' "$OUT/names.err"
	fi
	echo
	echo "Artifacts: \`$OUT\`"
} > "$REPORT"

echo "GATE: $RESULT  (checks: shot=${SHOT_K}K names=${NAMES} 0x502=${GL502} panic=${PANIC})"
if [ ${#FAILS[@]} -ne 0 ]; then
	for f in "${FAILS[@]}"; do echo "GATE: FAIL reason: $f"; done
	exit 1
fi
exit 0

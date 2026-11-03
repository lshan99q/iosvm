#!/bin/bash
# boots.sh -- P0's retry protocol: cold boots until WANT of them reach the interface, or TRIES run out.
#
# One boot is one sample and one sample proves nothing (the pass rate has been 14-67%).  This runs the
# boots back to back, classifies each one with boot-measure's own end condition, and prints the numbers
# the stage is judged by: how many reached the interface, how many of those stayed alive, and every
# failure attributed to its lock.
#
#   IMAGE=/tmp/root-fixed-net.apfs WANT=8 TRIES=12 ./harness/boots.sh
#   sudo -n nice -n -20 env IMAGE=/tmp/root-fixed-net.apfs WANT=3 TRIES=3 TAG=prio ./harness/boots.sh
#
# env:
#   WANT=8        how many passes count as done
#   TRIES=12      how many attempts are allowed (a panic costs an attempt)
#   IMAGE=...     the root.apfs to boot (default /tmp/root-fixed-net.apfs)
#   TAG=boots     label prefix: attempt N is /tmp/<TAG><N>.log, /tmp/<TAG><N>-host.log
#   BOOTS_LOG=/tmp/boots.log
#   MAXSECS=1200  passed to boot-measure as its hard timeout
#   UI_GRACE=300  after the first screenshot >= MINSHOT bytes, keep the boot running this many seconds
#                 (P0's sample: "reached the interface and stayed alive").  UI_GRACE=0 falls back to
#                 boot-measure's classic acceptance (MINFRAMES frames + MINSHOT bytes).
#   ZERO_RAM=1    recreate guest RAM for every attempt (a leftover frame would otherwise be read as
#                 "the interface is up" in the first seconds; see boot-measure.sh)
#   plus everything boot-measure reads: MINFRAMES, MINSHOT, QEMU_SMP, QEMU_EXTRA, XCMD, HOST_ENV
#
# A pass is a run that ended at the interface (end=ui) or at the classic acceptance, with a real
# interface seen (ui>0) and no SIGSEGV, no usertrap, no panic in the same run.  Anything else is a
# failure, and the line written for it says which one and why.
set -u
cd "$(dirname "$(readlink -f "$0")")/.." || exit 1     # tools/glwire, where boot-measure's paths are relative
WANT=${WANT:-8}
TRIES=${TRIES:-12}
IMAGE=${IMAGE:-/tmp/root-fixed-net.apfs}
TAG=${TAG:-boots}
BOOTS_LOG=${BOOTS_LOG:-/tmp/boots.log}
MAXSECS=${MAXSECS:-1200}
UI_GRACE=${UI_GRACE:-300}
ZERO_RAM=${ZERO_RAM:-1}
[ -f "$IMAGE" ] || { echo "boots: no such image: $IMAGE" >&2; exit 1; }

rm -f "$BOOTS_LOG"; : > "$BOOTS_LOG"
pass=0
try=0
uiseen=0
panics=0
declare -a failures=()
declare -a uisecs=()
echo "boots: tag=$TAG image=$IMAGE want=$WANT tries=$TRIES max=$MAXSECS ui_grace=$UI_GRACE smp=${QEMU_SMP:-4} extra=[${QEMU_EXTRA:-}] xcmd=[${XCMD:-}] env=[${HOST_ENV:-}] $(date -Is)" | tee -a "$BOOTS_LOG"

while [ "$pass" -lt "$WANT" ] && [ "$try" -lt "$TRIES" ]; do
	try=$((try + 1))
	label="$TAG$try"
	echo "=== attempt $try ($label), passes $pass/$WANT, $(date -Is)" | tee -a "$BOOTS_LOG"
	MINFRAMES=${MINFRAMES:-200} MINSHOT=${MINSHOT:-300000} UI_GRACE="$UI_GRACE" ZERO_RAM="$ZERO_RAM" \
		harness/boot-measure.sh "$label" "$IMAGE" "$MAXSECS" "$@" > "/tmp/$label.run" 2>&1
	line=$(grep -a "xb: $label end=" "/tmp/$label.run" | tail -1)
	classify=$(echo "$line" | sed -n 's/.*end=\([a-z-]*\).*/\1/p')
	uis=$(echo "$line" | sed -n 's/.* ui=\([0-9]*\)s.*/\1/p')
	pan=$(echo "$line" | sed -n 's/.* panic=\([0-9]*\).*/\1/p')
	sig=$(echo "$line" | sed -n 's/.*SIGSEGV=\([0-9]*\).*/\1/p')
	trap=$(echo "$line" | sed -n 's/.* usertrap=\([0-9]*\).*/\1/p')
	[ -n "$pan" ] && [ "$pan" -gt 0 ] && panics=$((panics + 1))
	[ -n "$uis" ] && [ "$uis" -gt 0 ] && { uiseen=$((uiseen + 1)); uisecs+=("$uis"); }
	if { [ "$classify" = ui ] || [ "$classify" = acceptance ]; } && [ "${uis:-0}" -gt 0 ] \
		&& [ "${pan:-1}" -eq 0 ] && [ "${sig:-1}" -eq 0 ] && [ "${trap:-1}" -eq 0 ]; then
		pass=$((pass + 1))
		echo "=== PASS $pass: $line" | tee -a "$BOOTS_LOG"
	else
		# what the failure was: the panic's lock, or the end line itself
		why=$(grep -a "panic(cpu" "/tmp/$label.log" 2>/dev/null | head -1 | tr -d '\000' | cut -c1-220)
		[ -z "$why" ] && why=$(echo "$line" | cut -c1-220)
		failures+=("$label:$classify")
		echo "=== FAIL $label: $line" | tee -a "$BOOTS_LOG"
		[ -n "$why" ] && echo "     why: $why" | tee -a "$BOOTS_LOG"
	fi
done

rate=$(( (pass * 100) / try ))
uirate=$(( (uiseen * 100) / try ))
{
	echo "=== boots done: pass $pass/$WANT after $try attempts ($rate%), interface seen $uiseen/$try ($uirate%), $panics panics, $(date -Is)"
	echo "=== ui seconds: ${uisecs[*]:-none}"
	echo "=== failures: ${failures[*]:-none}"
} | tee -a "$BOOTS_LOG"
[ -n "${SUDO_UID:-}" ] && chown "$SUDO_UID:${SUDO_GID:-$SUDO_UID}" /tmp/"$TAG"*.log /tmp/"$TAG"*.run /tmp/"$TAG"*.png "$BOOTS_LOG" 2>/dev/null
grep -a "=== boots done" "$BOOTS_LOG" | tail -1

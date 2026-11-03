#!/bin/bash
# boot-measure.sh LABEL ROOT_APFS MAX_SECONDS [HOST_ARGS...]
#
# One cold boot, one host renderer, one pair of logs, and an end condition that is the acceptance
# itself.  Two ways to end, and the stage's protocol is the one it sets:
#
#   UI_GRACE=300   the interface is a screenshot of at least MINSHOT bytes (an interface is 400 KB and
#                  more; a black screen is 8 KB).  When the first one is taken, keep the boot running
#                  for UI_GRACE more seconds and end with reason=ui if nothing crashed in that window.
#                  This is P0's per-boot sample: did it reach the interface, and did it stay alive.
#                  (Without UI_GRACE the old condition stands.)
#
#   MINFRAMES/MINSHOT  at least MINFRAMES frames from the host and a screenshot bigger than MINSHOT
#                  bytes: the classic acceptance, and what the R1/R2 runs were judged by.
#
# Nothing else may run: payload.bin, ram.img and port 9110 are shared, and a second virtual machine
# or a build during a measurement is what turns a boot into a ticket-spinlock panic.
#
#   /tmp/<label>.log        the guest's serial log
#   /tmp/<label>-host.log   the host renderer
#   /tmp/<label>-last.png   the last screenshot taken
#   /tmp/<label>-shot.png   the biggest one
#
# env: MINFRAMES=200 MINSHOT=300000 UI_GRACE=0 NOFRAMES_BY=900 NOUI_BY=780 MINUI=100000
#      ZERO_RAM=1   recreate qemu/ram.img first: guest RAM is a private mapping of that file, so the
#                   frame the previous boot left in memory is still there at power-on and a screendump
#                   taken in the first seconds is the old home screen, not this boot's (measured
#                   2026-11-03: two boots in a row reported "ui at 30s" with 448,195 B while launchd
#                   was still at UEFI).  The interface counts only once the host has frames too.
set -u
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
GLWIRE=/home/lshan/DSH-desktop/iosvm/tools/glwire
LABEL=$1
ROOT=$2
MAXSECS=${3:-1800}
shift 3 || true
HOSTARGS="$*"
ZERO_RAM=${ZERO_RAM:-0}
MINFRAMES=${MINFRAMES:-200}
MINSHOT=${MINSHOT:-300000}
UI_GRACE=${UI_GRACE:-0}
NOFRAMES_BY=${NOFRAMES_BY:-900}
NOUI_BY=${NOUI_BY:-780}
LOG=/tmp/$LABEL.log
HLOG=/tmp/$LABEL-host.log
SHOT=/tmp/$LABEL-last.png

for pid in $(ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'); do
	echo "xb: killing a leftover qemu $pid" | tee -a "$LOG"
	kill -9 "$pid" 2>/dev/null
done
for pid in $(ps -eo pid,comm | awk '$2 ~ /^host_glwire/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
sleep 1
if [ "$ZERO_RAM" = 1 ]; then
	rm -f "$IOSVM/qemu/ram.img"     # (run-darwin.sh puts the system image back at the loader's address)
fi

# every file this run writes is removed first: with fs.protected_regular=2 a root-run script cannot even
# truncate a /tmp file that a previous (non-root) run left behind -- that is how the sweeps of 2026-11-03
# kept reading a screenshot from 15:57 as "the interface is up" in the first seconds of a boot.
rm -f "$LOG" "$HLOG" "$SHOT" "/tmp/$LABEL-shot.png" "/tmp/$LABEL-final.png" "/tmp/$LABEL-crash.png"
: > "$LOG"; : > "$HLOG"
cd "$IOSVM" || exit 1
echo "===== $(date -Is) $LABEL root=$ROOT hostargs=[$HOSTARGS] minframes=$MINFRAMES minshot=$MINSHOT ui_grace=$UI_GRACE =====" >> "$LOG"
QEMU_SMP=${QEMU_SMP:-4} QEMU_TB_MB=${QEMU_TB_MB:-1536} QEMU_RAM_GB=12 QEMU_NET=1 QEMU_ESP_IMAGE=$IOSVM/qemu/esp.img \
	IMG=$IOSVM/userland/system.apfs ROOT=$ROOT \
	CMDLINE="cpus=${QEMU_SMP:-4} fb_beat=0 fbscale=2 metal=1 ${XCMD:-}" \
	timeout 7500 bash qemu/run-darwin.sh "$MAXSECS" ${QEMU_EXTRA:-} >> "$LOG" 2>&1 &
qpid=$!

# the host maps the RAM file, so QEMU has to have opened it first
for _ in $(seq 1 90); do
	pid=$(ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}' | head -1)
	if [ -n "$pid" ] && ls -l /proc/$pid/fd 2>/dev/null | grep -q 'ram.img'; then break; fi
	sleep 2
done
sleep 2
( cd "$GLWIRE" && env ${HOST_ENV:-} ${HOST_BIN:-./host_glwire} $HOSTARGS 9110 "$IOSVM/qemu/ram.img" > "$HLOG" 2>&1 ) &
hpid=$!
echo "xb: qemu=$qpid host=$hpid started $(date -Is)" >> "$LOG"

biggest=0
SHOTIN=/tmp/xb-$LABEL-$$.png
rm -f "$SHOTIN"
reason=timeout
best=/tmp/$LABEL-shot.png
ui_at=0
ui_bytes=0
for tick in $(seq 1 $((MAXSECS / 10))); do
	sleep 10
	kill -0 "$qpid" 2>/dev/null || { reason=qemu-exited; break; }
	if grep -aq "exited due to SIGSEGV\|panic(" "$LOG" 2>/dev/null; then
		reason=crash
		rm -f "/tmp/$LABEL-crash.png"; bash qemu/screendump.sh "/tmp/$LABEL-crash.png" >/dev/null 2>&1
		sleep 15
		break
	fi
	if [ $((tick % 3)) -eq 0 ]; then
		rm -f "$SHOTIN"
		bash qemu/screendump.sh "$SHOTIN" >/dev/null 2>&1
		if [ -f "$SHOTIN" ]; then
			size=$(stat -c %s "$SHOTIN")
			cp "$SHOTIN" "$SHOT"
			if [ "$size" -gt "$biggest" ]; then biggest=$size; cp "$SHOTIN" "$best"; fi
			if [ "$ui_at" -eq 0 ] && [ "$size" -ge "$MINSHOT" ] && [ "$(grep -ac "host_glwire: frame " "$HLOG" 2>/dev/null)" -gt 0 ]; then
				ui_at=$((tick * 10))
				ui_bytes=$size
				echo "xb: $LABEL ui at ${ui_at}s ($size B) $(date -Is)" >> "$LOG"
			fi
		fi
	fi
	hostframes=$(grep -ac "host_glwire: frame " "$HLOG" 2>/dev/null)
	if [ "$ui_at" -eq 0 ] && [ "$tick" -ge $((NOFRAMES_BY / 10)) ] && [ "$hostframes" -eq 0 ]; then
		reason=noframes
		break
	fi
	if [ "$ui_at" -eq 0 ] && [ "$tick" -ge $((NOUI_BY / 10)) ] && [ "$biggest" -lt "${MINUI:-100000}" ]; then
		reason=noui
		break
	fi
	# P0's per-boot sample: the interface is up and the boot has stayed alive UI_GRACE seconds after
	if [ "$UI_GRACE" -gt 0 ] && [ "$ui_at" -gt 0 ] && [ $((tick * 10)) -ge $((ui_at + UI_GRACE)) ]; then
		reason=ui
		break
	fi
	if [ "$UI_GRACE" -eq 0 ] && [ "$hostframes" -ge "$MINFRAMES" ] && [ "$biggest" -ge "$MINSHOT" ]; then
		reason=acceptance
		break
	fi
done

rm -f "/tmp/$LABEL-final.png"; bash qemu/screendump.sh "/tmp/$LABEL-final.png" >/dev/null 2>&1
fsize=$(stat -c %s /tmp/$LABEL-final.png 2>/dev/null || echo 0)
[ "$fsize" -gt "$biggest" ] && { biggest=$fsize; cp /tmp/$LABEL-final.png "$best"; }
if [ "$ui_at" -eq 0 ] && [ "$biggest" -ge "$MINSHOT" ] && [ "$(grep -ac "host_glwire: frame " "$HLOG" 2>/dev/null)" -gt 0 ]; then
	ui_at=$((tick * 10))
	ui_bytes=$biggest
	echo "xb: $LABEL ui at ${ui_at}s ($biggest B, final) $(date -Is)" >> "$LOG"
fi

for pid in $(ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
kill "$qpid" 2>/dev/null
for pid in $(ps -eo pid,comm | awk '$2 ~ /^host_glwire/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
kill "$hpid" 2>/dev/null
wait "$qpid" 2>/dev/null

frames=$(grep -ac "hakios-gl: frame " "$LOG")
hostframes=$(grep -ac "host_glwire: frame " "$HLOG")
match=$(grep -ac "MATCH" "$HLOG")
diff=$(grep -ac "DIFF" "$HLOG")
sig=$(grep -ac "exited due to SIGSEGV" "$LOG")
pan=$(grep -ac "panic(" "$LOG")
xtrap=$(grep -ac "usertrap:" "$LOG")
echo "xb: $LABEL end=$reason ui=${ui_at}s ui_bytes=$ui_bytes $(date -Is) guestframes=$((frames / 2)) hostframes=$hostframes MATCH=$match DIFF=$diff SIGSEGV=$sig usertrap=$xtrap panic=$pan biggest=$biggest" >> "$LOG"
echo "xb: $LABEL end=$reason ui=${ui_at}s ui_bytes=$ui_bytes guestframes=$((frames / 2)) hostframes=$hostframes MATCH=$match DIFF=$diff SIGSEGV=$sig usertrap=$xtrap panic=$pan biggest=$biggest"

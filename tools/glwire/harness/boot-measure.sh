#!/bin/bash
# xbrep.sh LABEL ROOT_APFS MAX_SECONDS [HOST_ARGS...]
#
# One cold boot, one host renderer, one pair of logs, and an end condition that is the acceptance
# itself: at least MINFRAMES frames from the host and a screenshot bigger than MINSHOT bytes (a
# screen with a real interface is 400 KB and more; a black one is 8 KB).
#
# Nothing else may run: payload.bin, ram.img and port 9110 are shared, and a second virtual machine
# or a build during a measurement is what turns a boot into a ticket-spinlock panic.
#
#   /tmp/<label>.log        the guest's serial log
#   /tmp/<label>-host.log   the host renderer
#   /tmp/<label>-last.png   the last screenshot taken
#   /tmp/<label>-shot.png   the biggest one
set -u
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
GLWIRE=/home/lshan/DSH-desktop/iosvm/tools/glwire
LABEL=$1
ROOT=$2
MAXSECS=${3:-1800}
shift 3 || true
HOSTARGS="$*"
MINFRAMES=${MINFRAMES:-200}
MINSHOT=${MINSHOT:-300000}
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

: > "$LOG"; : > "$HLOG"
cd "$IOSVM" || exit 1
echo "===== $(date -Is) $LABEL root=$ROOT hostargs=[$HOSTARGS] minframes=$MINFRAMES minshot=$MINSHOT =====" >> "$LOG"
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
reason=timeout
best=/tmp/$LABEL-shot.png
for tick in $(seq 1 $((MAXSECS / 10))); do
	sleep 10
	kill -0 "$qpid" 2>/dev/null || { reason=qemu-exited; break; }
	if grep -aq "exited due to SIGSEGV\|panic(" "$LOG" 2>/dev/null; then
		reason=crash
		bash qemu/screendump.sh "/tmp/$LABEL-crash.png" >/dev/null 2>&1
		sleep 15
		break
	fi
	if [ $((tick % 3)) -eq 0 ]; then
		bash qemu/screendump.sh /tmp/.xb-shot.png >/dev/null 2>&1
		if [ -f /tmp/.xb-shot.png ]; then
			size=$(stat -c %s /tmp/.xb-shot.png)
			cp /tmp/.xb-shot.png "$SHOT"
			if [ "$size" -gt "$biggest" ]; then biggest=$size; cp /tmp/.xb-shot.png "$best"; fi
		fi
	fi
	hostframes=$(grep -ac "host_glwire: frame " "$HLOG" 2>/dev/null)
	if [ "$tick" -ge $((NOFRAMES_BY / 10)) ] && [ "$hostframes" -eq 0 ]; then
		reason=noframes
		break
	fi
	if [ "$tick" -ge $((NOUI_BY / 10)) ] && [ "$biggest" -lt "${MINUI:-100000}" ]; then
		reason=noui
		break
	fi
	if [ "$hostframes" -ge "$MINFRAMES" ] && [ "$biggest" -ge "$MINSHOT" ]; then
		reason=acceptance
		break
	fi
done

bash qemu/screendump.sh "/tmp/$LABEL-final.png" >/dev/null 2>&1
fsize=$(stat -c %s /tmp/$LABEL-final.png 2>/dev/null || echo 0)
[ "$fsize" -gt "$biggest" ] && { biggest=$fsize; cp /tmp/$LABEL-final.png "$best"; }

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
echo "xb: $LABEL end=$reason $(date -Is) guestframes=$((frames / 2)) hostframes=$hostframes MATCH=$match DIFF=$diff SIGSEGV=$sig usertrap=$xtrap panic=$pan biggest=$biggest" >> "$LOG"
echo "xb: $LABEL end=$reason guestframes=$((frames / 2)) hostframes=$hostframes MATCH=$match DIFF=$diff SIGSEGV=$sig usertrap=$xtrap panic=$pan biggest=$biggest"

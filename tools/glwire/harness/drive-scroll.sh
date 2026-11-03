#!/bin/bash
# drive-scroll.sh LABEL ROOT.apfs [MAX_SECONDS] [HOST_ARGS...] -- P3': a 20 s scroll AFTER the interface
#
# rc.touch1's touch is at 600 s and the interface comes up between 420 and 660 s, so its window is
# driven on whatever the boot was doing.  This drives instead: it waits for the interface itself
# (a screenshot of at least MINSHOT bytes), opens Settings, swipes for 20 s, and reports the frames
# the host received in that window and the time the window took.  Beside the host's count it prints
# the guest's own (the display driver's "N frames in M ms" lines inside the window), which is the
# only frame count the pure-local column has: with no recorder in the image no records leave the
# guest, so "host frames" is 0 there -- REQUIRE_HOST_FRAMES=0 is what lets that column find the
# interface at all (a screenshot alone is the sign).
#
# env: MINSHOT=300000 NOUI_BY=780 REQUIRE_HOST_FRAMES=1 ZERO_RAM=1 QEMU_SMP=4 QEMU_EXTRA= XCMD= HOST_ENV=
#   /tmp/<label>.log / -host.log / -shot.png as boot-measure.sh; plus /tmp/<label>-drive.txt
set -u
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
GLWIRE=/home/lshan/DSH-desktop/iosvm/tools/glwire
LABEL=$1; ROOT=$2; MAXSECS=${3:-900}; shift 3 || true
HOSTARGS="$*"
MINSHOT=${MINSHOT:-300000}
NOUI_BY=${NOUI_BY:-780}
REQUIRE_HOST_FRAMES=${REQUIRE_HOST_FRAMES:-1}
ZERO_RAM=${ZERO_RAM:-1}
LOG=/tmp/$LABEL.log; HLOG=/tmp/$LABEL-host.log; DRIVE=/tmp/$LABEL-drive.txt
FRAMES_IN='MiniIOMobileFramebuffer: [0-9]+ frames in [0-9]+ ms'
rm -f "$LOG" "$HLOG" /tmp/xb-$LABEL-$$.png "$DRIVE"
for pid in $(ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
for pid in $(ps -eo pid,comm | awk '$2 ~ /^host_glwire/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
sleep 1
[ "$ZERO_RAM" = 1 ] && rm -f "$IOSVM/qemu/ram.img"
cd "$IOSVM" || exit 1
QEMU_SMP=${QEMU_SMP:-4} QEMU_TB_MB=${QEMU_TB_MB:-1536} QEMU_RAM_GB=12 QEMU_NET=1 QEMU_ESP_IMAGE=$IOSVM/qemu/esp.img \
	IMG=$IOSVM/userland/system.apfs ROOT=$ROOT CMDLINE="cpus=${QEMU_SMP:-4} fb_beat=0 fbscale=2 metal=1 ${XCMD:-}" \
	timeout 7500 bash qemu/run-darwin.sh "$MAXSECS" ${QEMU_EXTRA:-} >> "$LOG" 2>&1 &
qpid=$!
for _ in $(seq 1 90); do
	pid=$(ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}' | head -1)
	[ -n "$pid" ] && ls -l /proc/$pid/fd 2>/dev/null | grep -q 'ram.img' && break
	sleep 2
done
sleep 2
( cd "$GLWIRE" && env ${HOST_ENV:-} ${HOST_BIN:-./host_glwire} $HOSTARGS 9110 "$IOSVM/qemu/ram.img" > "$HLOG" 2>&1 ) &
hpid=$!
SHOTIN=/tmp/xb-$LABEL-$$.png
ui=0
for tick in $(seq 1 $((MAXSECS / 10))); do
	sleep 10
	kill -0 "$qpid" 2>/dev/null || break
	grep -aq "panic(" "$LOG" 2>/dev/null && { echo "drive: panic before the interface" >> "$DRIVE"; break; }
	[ $((tick % 2)) -eq 0 ] || continue
	rm -f "$SHOTIN"; bash qemu/screendump.sh "$SHOTIN" >/dev/null 2>&1
	size=$(stat -c %s "$SHOTIN" 2>/dev/null || echo 0)
	frames_now=$(grep -ac 'host_glwire: frame ' "$HLOG" 2>/dev/null)
	if [ "$size" -ge "$MINSHOT" ] && { [ "$REQUIRE_HOST_FRAMES" = 0 ] || [ "$frames_now" -gt 0 ]; }; then
		ui=$((tick * 10)); cp "$SHOTIN" "/tmp/$LABEL-shot.png"; break
	fi
	[ $((tick * 10)) -ge "$NOUI_BY" ] && break
done
if [ "$ui" -eq 0 ]; then
	echo "drive: no interface" >> "$DRIVE"; kill "$qpid" 2>/dev/null
	for pid in $(ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
	kill -9 "$hpid" 2>/dev/null; echo "drive: $LABEL ui=0"; exit 0
fi
sleep 4
python3 qemu/touch.py tap 0.656 0.729 >/dev/null 2>&1     # the Settings icon, as rc.touch1's first action
sleep 6
n0=$(grep -ac "host_glwire: frame " "$HLOG"); g0=$(grep -acE "$FRAMES_IN" "$LOG"); t0=$(date +%s.%N)
echo "drive: window begins $(date -Is) frames=$n0 guest_lines=$g0" >> "$DRIVE"
for i in 1 2 3 4 5 6 7 8; do
	python3 qemu/touch.py swipe 0.5 0.85 0.5 0.30 700 >/dev/null 2>&1
	sleep 1.6
done
t1=$(date +%s.%N); n1=$(grep -ac "host_glwire: frame " "$HLOG"); g1=$(grep -acE "$FRAMES_IN" "$LOG")
python3 - "$n0" "$n1" "$g0" "$g1" "$t0" "$t1" "$ui" "$LOG" >> "$DRIVE" <<'PY'
import re
import sys

n0, n1, g0, g1 = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
t0, t1, ui = float(sys.argv[5]), float(sys.argv[6]), int(sys.argv[7])
log = sys.argv[8]
pattern = re.compile(r"(\d+) frames in (\d+) ms")
rows = [line for line in open(log, errors="replace").read().splitlines()
        if "MiniIOMobileFramebuffer:" in line and pattern.search(line)][g0:g1]
frames = sum(int(pattern.search(line).group(1)) for line in rows)
millis = sum(int(pattern.search(line).group(2)) for line in rows)
span = t1 - t0
host = (n1 - n0) / span if span else 0.0
guest = 1000.0 * frames / millis if millis else 0.0
print(f"drive: ui={ui}s window {span:.2f}s frames {n1-n0} -> {host:.2f} fps "
      f"(host frames, 20 s scroll in Settings); guest {frames} frames in {millis} ms -> {guest:.2f} fps "
      f"(MiniIOMobileFramebuffer, {len(rows)} driver lines ~5 s apart)")
PY
kill "$qpid" 2>/dev/null
for pid in $(ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
for pid in $(ps -eo pid,comm | awk '$2 ~ /^host_glwire/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
kill -9 "$hpid" 2>/dev/null
tail -2 "$DRIVE"

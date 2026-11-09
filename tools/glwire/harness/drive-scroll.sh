#!/bin/bash
# drive-scroll.sh LABEL ROOT.apfs [MAX_SECONDS] [HOST_ARGS...] -- P3': a scroll AFTER the interface
#
# rc.touch1's touch is at 600 s and the interface comes up between 420 and 660 s, so its window is
# driven on whatever the boot was doing.  This drives instead: it waits for the interface itself
# (a screenshot of at least MINSHOT bytes), opens Settings, swipes, and reports the frames of the
# window and the time the window took.  Beside the host's count it prints the guest's own (the
# display driver's "N frames in M ms" lines inside the window), which is the only frame count the
# pure-local column has: with no recorder in the image no records leave the guest, so "host frames"
# is 0 there -- REQUIRE_HOST_FRAMES=0 is what lets that column find the interface at all (a
# screenshot alone is the sign).
#
# Two windows (P3' then P2'''):
#   WINDOW_FRAMES=0 (default)  the old one: eight swipes, about 20 s, and the time it took.
#   WINDOW_FRAMES=N            a window of WINDOW_SPAN frames (120), starting at the Nth frame
#                              after the interface: measure how long those 120 frames take, so
#                              fps = 120 / seconds.  A boot whose UI phase stalls for seconds still
#                              gets a comparable number -- the frames are counted, not the clock.
#                              WINDOW_CLOCK=host|guest (default host): which counter "frame N" is.
#                              host is the recorder's "host_glwire: frame" lines; guest is the sum
#                              of the display driver's "N frames in M ms" (the pure-local column,
#                              whose image has no recorder: WINDOW_CLOCK=guest).
#                              WINDOW_AFTER=S (default 0): wait S seconds after the interface
#                              before the window opens.  The interface's first seconds are the
#                              Today view being rebuilt -- P0's hot stack -- so a window
#                              counted there measures the rebuild, not the column; the same
#                              counter after it settles is 19.35 fps where the first seconds are
#                              0.05 (see the dry run of m8ww15on).
#
# --dry-run LABEL [FROM]  no VM: reads /tmp/LABEL.log and /tmp/LABEL-host.log and prints the same
#                          sum (frames FROM+1..FROM+WINDOW_SPAN, host clock from the intervals,
#                          guest clock from the driver lines), for checking the arithmetic on a
#                          log of an earlier boot.
#
# env: MINSHOT=300000 NOUI_BY=780 REQUIRE_HOST_FRAMES=1 ZERO_RAM=1 QEMU_SMP=4 QEMU_EXTRA= XCMD= HOST_ENV=
#      WINDOW_FRAMES=0 WINDOW_SPAN=120 WINDOW_CLOCK=host WINDOW_AFTER=0
#   /tmp/<label>.log / -host.log / -shot.png as boot-measure.sh; plus /tmp/<label>-drive.txt
set -u
IOSVM=${IOSVM:-/host/iosvm-work/hakios-xnu-kernel}
GLWIRE=${GLWIRE:-/host/iosvm/tools/glwire}
MINSHOT=${MINSHOT:-300000}
NOUI_BY=${NOUI_BY:-780}
REQUIRE_HOST_FRAMES=${REQUIRE_HOST_FRAMES:-1}
ZERO_RAM=${ZERO_RAM:-1}
WINDOW_FRAMES=${WINDOW_FRAMES:-0}
WINDOW_SPAN=${WINDOW_SPAN:-120}
WINDOW_CLOCK=${WINDOW_CLOCK:-host}
WINDOW_AFTER=${WINDOW_AFTER:-0}
FRAMES_IN='MiniIOMobileFramebuffer: [0-9]+ frames in [0-9]+ ms'

host_frames() { local n; n=$(grep -ac 'host_glwire: frame ' "${HLOG:-/dev/null}" 2>/dev/null); echo "${n:-0}"; }
guest_frames() { grep -aoE "$FRAMES_IN" "${LOG:-/dev/null}" 2>/dev/null | awk '{s += $2} END {print s + 0}'; }
frames_on() { if [ "${1:-host}" = guest ]; then guest_frames; else host_frames; fi; }

if [ "${1:-}" = "--dry-run" ]; then
	# --dry-run LOG [HOSTLOG]  the two files of an earlier boot (HOSTLOG defaults to the LOG's name
	#                          with -host.log before .log); --dry-run LABEL [FROM] is the shorthand
	#                          for /tmp/LABEL.log and /tmp/LABEL-host.log, starting at frame FROM.
	if [ $# -ge 2 ] && [ -f "${2:-}" ]; then
		LOG=$2
		HLOG=${3:-${2%.log}-host.log}
		FROM=${WINDOW_FROM:-0}
	else
		LABEL=${2:?--dry-run needs a log file or a label}
		LOG=/tmp/$LABEL.log; HLOG=/tmp/$LABEL-host.log
		FROM=${3:-0}
	fi
	python3 - "$HLOG" "$LOG" "$FROM" "$WINDOW_SPAN" <<'PY'
import re
import sys

host_path, guest_path, start, span = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
FRAME = re.compile(r"host_glwire: frame \d+.*?interval ([\d.]+) s")
FRAMES_IN = re.compile(r"MiniIOMobileFramebuffer: (\d+) frames in (\d+) ms")

intervals = []
try:
    for line in open(host_path, errors="replace"):
        m = FRAME.search(line)
        if m:
            intervals.append(float(m.group(1)))
except FileNotFoundError:
    pass
if intervals:
    n = len(intervals)
    total = sum(intervals)
    if n < start + span:
        print(f"dry-run: host clock: only {n} frames in {host_path}; cannot make {span} frames "
              f"from frame {start + 1} (whole log {total:.1f}s = {(n / total) if total else 0:.2f} fps)")
    else:
        took = sum(intervals[:start + span]) - sum(intervals[:start])
        print(f"dry-run: host clock: frames {start + 1}..{start + span} of {n} in {took:.2f}s "
              f"-> {(span / took) if took else 0:.2f} fps (sum of intervals; whole boot {total:.1f}s "
              f"= {(n / total) if total else 0:.2f} fps)")
else:
    print(f"dry-run: host clock: no 'host_glwire: frame' intervals in {host_path}")

rows = []                       # (frames before, ms before, frames in line, ms in line)
cum = 0
ms = 0
try:
    for line in open(guest_path, errors="replace"):
        m = FRAMES_IN.search(line)
        if not m:
            continue
        n_i, ms_i = int(m.group(1)), int(m.group(2))
        rows.append((cum, ms, n_i, ms_i))
        cum += n_i
        ms += ms_i
except FileNotFoundError:
    rows = []
if rows:
    def at(frame):
        if frame <= 0:
            return 0.0
        for before, ms_before, n_i, ms_i in rows:
            if before + n_i >= frame:
                return ms_before + (frame - before) * ms_i / n_i
        return float(ms)
    if cum < start + span:
        print(f"dry-run: guest clock: only {cum} frames in {guest_path}; cannot make {span} frames "
              f"from frame {start + 1} ({len(rows)} driver lines, {ms / 1000.0:.1f}s)")
    else:
        took = (at(start + span) - at(start)) / 1000.0
        print(f"dry-run: guest clock: {len(rows)} driver lines, {cum} frames, {ms / 1000.0:.1f}s "
              f"= {((cum / (ms / 1000.0)) if ms else 0):.2f} fps; frames {start + 1}..{start + span} in "
              f"{took:.2f}s -> {(span / took) if took else 0:.2f} fps (interpolated within ~5 s driver lines: coarse)")
else:
    print(f"dry-run: guest clock: no 'MiniIOMobileFramebuffer: N frames in M ms' lines in {guest_path}")
PY
	exit 0
fi

LABEL=$1; ROOT=$2; MAXSECS=${3:-900}; shift 3 || true
HOSTARGS="$*"
LOG=/tmp/$LABEL.log; HLOG=/tmp/$LABEL-host.log; DRIVE=/tmp/$LABEL-drive.txt

# Kill only this launch's process tree (outer timeout -> run-darwin -> run.sh -> timeout -> qemu);
# never a name-wide pkill: another cell may already be starting.
kill_tree() {
	local pid=$1 child
	for child in $(ps -o pid= --ppid "$pid" 2>/dev/null); do
		kill_tree "$child"
	done
	kill -9 "$pid" 2>/dev/null
}
rm -f "$LOG" "$HLOG" /tmp/xb-$LABEL-$$.png "$DRIVE"
if ps -eo comm= | grep -q '^qemu-system-aar'; then
	echo "drive: another qemu-system is already running; refusing to start a second VM" >&2
	exit 1
fi
if ps -eo comm= | grep -q '^host_glwire'; then
	echo "drive: another host_glwire is already running; refusing to start" >&2
	exit 1
fi
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
	frames_now=$(host_frames)
	if [ "$size" -ge "$MINSHOT" ] && { [ "$REQUIRE_HOST_FRAMES" = 0 ] || [ "$frames_now" -gt 0 ]; }; then
		ui=$((tick * 10)); cp "$SHOTIN" "/tmp/$LABEL-shot.png"; break
	fi
	[ $((tick * 10)) -ge "$NOUI_BY" ] && break
done
if [ "$ui" -eq 0 ]; then
	echo "drive: no interface" >> "$DRIVE"
	kill_tree "$qpid"
	kill -9 "$hpid" 2>/dev/null
	echo "drive: $LABEL ui=0"; exit 0
fi
sleep 4
python3 qemu/touch.py tap 0.656 0.729 >/dev/null 2>&1     # the Settings icon, as rc.touch1's first action
sleep 6
if [ "$WINDOW_AFTER" -gt 0 ]; then
	# Let the interface settle before the window: its first seconds are the Today view being
	# rebuilt, and a window that lands there measures the rebuild, not the column.
	echo "drive: window waits ${WINDOW_AFTER}s after the interface ($(date -Is))" >> "$DRIVE"
	sleep "$WINDOW_AFTER"
fi

if [ "$WINDOW_FRAMES" -gt 0 ]; then
	# ---- P2''': a window of WINDOW_SPAN frames, counted, not timed ------------------------------
	clock=$WINDOW_CLOCK
	base=$(frames_on "$clock")
	host0=$(host_frames); guest0=$(guest_frames)
	skip=$((WINDOW_FRAMES - 1))
	target0=$((base + skip))
	target1=$((target0 + WINDOW_SPAN))
	began=$(date +%s)
	echo "drive: frame-window begins $(date -Is) clock=$clock base=$base frames $((skip + 1))..$((skip + WINDOW_SPAN))" >> "$DRIVE"
	t0=""; t1=""; count=$base
	for _ in $(seq 1 2400); do
		kill -0 "$qpid" 2>/dev/null || break
		if [ -z "$t1" ]; then
			python3 qemu/touch.py swipe 0.5 0.85 0.5 0.30 700 >/dev/null 2>&1
		fi
		count=$(frames_on "$clock")
		if [ -z "$t0" ] && [ "$count" -ge "$target0" ]; then t0=$(date +%s.%N); fi
		if [ -n "$t0" ] && [ "$count" -ge "$target1" ]; then t1=$(date +%s.%N); break; fi
		[ $(( $(date +%s) - began )) -ge $((MAXSECS - 30)) ] && break
		sleep 0.5
	done
	host1=$(host_frames); guest1=$(guest_frames)
	python3 - "$t0" "$t1" "$count" "$target1" "$WINDOW_SPAN" "$skip" "$clock" "$ui" "$host0" "$host1" "$guest0" "$guest1" >> "$DRIVE" <<'PY'
import sys

t0, t1 = sys.argv[1], sys.argv[2]
count, target1, span, skip = int(sys.argv[3]), int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
clock, ui = sys.argv[7], int(sys.argv[8])
host0, host1, guest0, guest1 = (int(v) for v in sys.argv[9:13])
if not t0 or not t1:
    print(f"drive: ui={ui}s frame-window TIMEOUT: clock={clock} reached frame {count} of {target1} "
          f"(host {host0}->{host1}, guest {guest0}->{guest1})")
else:
    seconds = float(t1) - float(t0)
    fps = span / seconds if seconds else 0.0
    print(f"drive: ui={ui}s frame-window {span} frames (from frame {skip + 1} after the interface) "
          f"in {seconds:.2f}s -> {fps:.2f} fps (WINDOW_CLOCK={clock}; host {host0}->{host1}, "
          f"guest {guest0}->{guest1})")
PY
else
	# ---- P3': the old fixed window (eight swipes, about 20 s) -----------------------------------
	n0=$(host_frames); g0=$(guest_frames); t0=$(date +%s.%N)
	echo "drive: window begins $(date -Is) frames=$n0 guest_frames=$g0" >> "$DRIVE"
	for i in 1 2 3 4 5 6 7 8; do
		python3 qemu/touch.py swipe 0.5 0.85 0.5 0.30 700 >/dev/null 2>&1
		sleep 1.6
	done
	t1=$(date +%s.%N); n1=$(host_frames); g1=$(guest_frames)
	python3 - "$n0" "$n1" "$g0" "$g1" "$t0" "$t1" "$ui" "$LOG" >> "$DRIVE" <<'PY'
import re
import sys

n0, n1, g0, g1, t0, t1, ui, log = (sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4],
                                  float(sys.argv[5]), float(sys.argv[6]), int(sys.argv[7]), sys.argv[8])
pattern = re.compile(r"(\d+) frames in (\d+) ms")
rows = [line for line in open(log, errors="replace").read().splitlines()
        if "MiniIOMobileFramebuffer:" in line and pattern.search(line)][int(g0):int(g1)]
frames = sum(int(pattern.search(line).group(1)) for line in rows)
millis = sum(int(pattern.search(line).group(2)) for line in rows)
span = t1 - t0
host = (int(n1) - int(n0)) / span if span else 0.0
guest = 1000.0 * frames / millis if millis else 0.0
print(f"drive: ui={ui}s window {span:.2f}s frames {int(n1)-int(n0)} -> {host:.2f} fps "
      f"(host frames, 20 s scroll in Settings); guest {frames} frames in {millis} ms -> {guest:.2f} fps "
      f"(MiniIOMobileFramebuffer, {len(rows)} driver lines ~5 s apart)")
PY
fi

kill_tree "$qpid"
kill -9 "$hpid" 2>/dev/null
tail -2 "$DRIVE"

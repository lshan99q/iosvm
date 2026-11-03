#!/bin/bash
# Replay one frame from a live guest and compare it, byte for byte, with the panel the guest
# composed itself.
#
#   ./repro_replay.sh [ROOT_APFS] [FRAME]
#
# It boots one four-vCPU machine with the arguments docs/state-of-the-port.md requires, starts the
# host with --no-panel (the host draws from CoreAnimation's recorded calls, not from the guest's
# panel), and at FRAME writes:
#
#   $OUT/f-panel-WxH.rgba    the panel the guest uploaded (RGBA)
#   $OUT/f-picture-WxH.bgra  the frame the host replayed (BGRA)
#   $OUT/rec-FRAME-*.records the record stream of that frame
#   $OUT/tex-*               every host texture after the upload pass
#   $OUT/draw-*              the framebuffer after every draw of that frame
#
# then runs compare_frame.py. Exit status is compare_frame.py's: 0 only when the two pictures are
# identical byte for byte.
#
# A boot that stalls before the display paints is a known race (docs/research/linux-kernel-build.md,
# addendum 17); the script retries, which is the procedure the port already documents.
set -u
IOSVM=${IOSVM:-/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${1:-${ROOT_APFS:-/tmp/root-fixed-net.apfs}}
FRAME=${2:-${FRAME:-80}}
OUT=${OUT:-/tmp/glwire-replay}
TRIES=${TRIES:-4}
MAX_WAIT=${MAX_WAIT:-900}
QEMU=${QEMU:-$IOSVM/qemu/run-darwin.sh}
RAM=${RAM:-$IOSVM/qemu/ram.img}
HOST_BIN=${HOST_BIN:-./host_glwire}
QEMU_CPUS=${QEMU_CPUS:-0-7}
HOST_CPUS=${HOST_CPUS:-8-15}

if [ ! -f "$ROOT" ]; then
	echo "repro_replay: no root image at $ROOT (set ROOT_APFS=...)" >&2
	exit 2
fi
if [ ! -f "$RAM" ]; then
	echo "repro_replay: no RAM file at $RAM" >&2
	exit 2
fi
mkdir -p "$OUT"
rm -f "$OUT"/f-panel-*.rgba "$OUT"/f-picture-*.bgra

cleanup()
{
	for pid in $(ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
	for pid in $(ps -eo pid,comm | awk '$2 ~ /^host_glwire/ {print $1}'); do kill -9 "$pid" 2>/dev/null; done
}
trap cleanup EXIT

wait_for_ram()
{
	for _ in $(seq 1 90); do
		pid=$(ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}' | head -1)
		if [ -n "$pid" ] && ls -l /proc/$pid/fd 2>/dev/null | grep -q 'ram.img'; then
			return 0
		fi
		sleep 2
	done
	return 1
}

for attempt in $(seq 1 "$TRIES"); do
	cleanup
	sleep 1
	rm -f "$OUT"/f-panel-*.rgba "$OUT"/f-picture-*.bgra "$OUT"/rec-* "$OUT"/tex-* "$OUT"/draw-*
	LOG=$OUT/guest-$attempt.log
	HLOG=$OUT/host-$attempt.log
	echo "repro_replay: attempt $attempt/$TRIES, root $ROOT, frame $FRAME"
	(
		cd "$IOSVM" || exit 1
		QEMU_SMP=4 QEMU_TB_MB=1536 QEMU_RAM_GB=12 QEMU_NET=1 \
		QEMU_ESP_IMAGE=$IOSVM/qemu/esp.img IMG=$IOSVM/userland/system.apfs ROOT=$ROOT \
		CMDLINE="cpus=4 fb_beat=0 fbscale=2 metal=1" \
		taskset -c "$QEMU_CPUS" timeout $((MAX_WAIT + 120)) bash "$QEMU" "$MAX_WAIT"
	) > "$LOG" 2>&1 &
	qpid=$!
	if ! wait_for_ram; then
		echo "repro_replay: QEMU did not open $RAM"
		kill -9 "$qpid" 2>/dev/null
		continue
	fi
	sleep 2
	(
		cd "$HERE" || exit 1
		LP_NUM_THREADS=${LP_NUM_THREADS:-1} \
		GLWIRE_DUMP=$OUT/f GLWIRE_DUMP_FROM=$FRAME GLWIRE_DUMP_DRAWS=$OUT/draw \
		GLWIRE_DUMP_TEXTURES=$OUT/tex GLWIRE_SAVE_RECORDS=$OUT/rec GLWIRE_SAVE_AT=$FRAME \
		taskset -c "$HOST_CPUS" $HOST_BIN --no-panel 9110 "$RAM"
	) > "$HLOG" 2>&1 &
	hpid=$!
	echo "repro_replay: qemu $qpid host $hpid"

	last_size=0
	frozen=0
	for tick in $(seq 1 $((MAX_WAIT / 5))); do
		sleep 5
		kill -0 "$qpid" 2>/dev/null || { echo "repro_replay: QEMU exited"; break; }
		if ls "$OUT"/f-panel-*.rgba >/dev/null 2>&1 && ls "$OUT"/f-picture-*.bgra >/dev/null 2>&1; then
			break
		fi
		if grep -aq "panic(\|exited due to SIGSEGV" "$LOG" 2>/dev/null; then
			echo "repro_replay: guest crashed before frame $FRAME"
			break
		fi
		# A boot that is going to stall stops writing its own log about two minutes in; one that
		# is going to work keeps writing. Retry instead of waiting the whole timeout.
		size=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
		if [ "$size" -eq "$last_size" ]; then
			frozen=$((frozen + 1))
		else
			frozen=0
			last_size=$size
		fi
		if [ "$frozen" -ge 24 ]; then
			echo "repro_replay: the boot stopped writing two minutes ago"
			break
		fi
	done
	if ls "$OUT"/f-panel-*.rgba >/dev/null 2>&1 && ls "$OUT"/f-picture-*.bgra >/dev/null 2>&1; then
		cleanup
		echo "repro_replay: comparing frame $FRAME"
		python3 "$HERE/compare_frame.py" "$OUT"/f-panel-*.rgba "$OUT"/f-picture-*.bgra --diff "$OUT/diff.png"
		exit $?
	fi
	cleanup
	kill -9 "$qpid" "$hpid" 2>/dev/null
done
echo "repro_replay: no boot reached frame $FRAME" >&2
exit 1

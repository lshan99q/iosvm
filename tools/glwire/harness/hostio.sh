#!/bin/bash
# hostio.sh OUT [SECONDS] -- what the host was doing while a boot ran (P1': is the stall host I/O?)
#
# One line a second:  epoch  io_some  io_full  disk_reads  disk_writes  free_mb
# Run it next to boot-measure.sh; stalls.py puts the stalls and these numbers on one clock.
#
#   ./harness/hostio.sh /tmp/m8/hostio.log 1200 &
set -u
OUT=${1:-/tmp/hostio.log}
SECS=${2:-1200}
: > "$OUT"
echo "# epoch io_some io_full disk_reads disk_writes free_mb" >> "$OUT"
dev=$(ls /sys/block/nvme*/stat 2>/dev/null | head -1)
prev_r=0 prev_w=0
while [ "$SECS" -gt 0 ]; do
	now=$(date +%s.%N)
	read -r some full < <(awk '/^some/ {split($2,a,"="); s=a[2]} /^full/ {split($2,a,"="); f=a[2]} END {print s+0, f+0}' /proc/pressure/io)
	if [ -n "$dev" ] && [ -f "$dev" ]; then
		set -- $(cat "$dev"); r=$1; w=$5
	else
		r=0; w=0
	fi
	free=$(awk '/^MemFree:/ {print int($2/1024)}' /proc/meminfo)
	echo "$now $some $full $((r-prev_r)) $((w-prev_w)) $free" >> "$OUT"
	prev_r=$r; prev_w=$w
	sleep 1
	SECS=$((SECS-1))
done

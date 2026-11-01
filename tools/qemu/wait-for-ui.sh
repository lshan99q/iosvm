#!/bin/bash
# SPDX-License-Identifier: MIT
# Boot until the home screen is really on the panel, then save it and stop.
#
# Not every boot of this port reaches it: some stall a couple of minutes in and the display driver
# never reports a frame (docs/research/linux-kernel-build.md, addendum 17). Retrying is the honest
# procedure until the stall itself is understood, and this script is that procedure.
#
#   qemu/wait-for-ui.sh [OUT.png]        TRIES=8 boots at most; each waits MAX_WAIT seconds for frames
set -u
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/.." && pwd)
out=${1:-$root/logs/home-screen.png}
tries=${TRIES:-8}
wait=${MAX_WAIT:-600}
MIN_BYTES=${MIN_BYTES:-30000}   # a drawn screen, not a black panel or a logo
NO_FRAMES_BY=${NO_FRAMES_BY:-84}  # 84 x 5 s = 7 min without a frame: that boot is lost
export QEMU_SMP=${QEMU_SMP:-4} QEMU_TB_MB=${QEMU_TB_MB:-1536} QEMU_RAM_GB=${QEMU_RAM_GB:-12}
export IMG=${IMG:-$root/userland/system.apfs} ROOT=${ROOT:-$root/userland/root.apfs}
export CMDLINE="cpus=${CPUS:-4} fb_beat=0 ${CMDLINE_EXTRA:-}"

mkdir -p "$(dirname "$out")"
for attempt in $(seq 1 "$tries"); do
	echo "attempt $attempt of $tries"
	log=$(mktemp)
	( cd "$root" && timeout $((wait + 60)) bash qemu/run-darwin.sh "$wait" >"$log" 2>&1 ) &
	pid=$!
	last=-1; frozen=0
	for tick in $(seq 1 $((wait / 5))); do
		# A boot that is going to stall stops writing its own log about two minutes in
		# (addendum 17); a boot that is going to work keeps writing. Watching the file grow
		# turns that into an early reboot instead of a wait, which is most of the difference
		# between ten minutes and a minute to a working screen.
		size=$(stat -c %s "$log" 2>/dev/null || echo 0)
		if [ "$size" -eq "$last" ]; then
			frozen=$((frozen + 1))
		else
			frozen=0
			last=$size
		fi
		if [ "$frozen" -ge 24 ]; then          # two minutes without a byte: this boot is gone
			echo "  the boot stopped writing two minutes ago; booting again"
			break
		fi
		# A stalled boot keeps dribbling a line now and then (lsd's CPU complaints), so "the
		# log stopped" alone is not enough: if the display has not reported a frame by the time
		# a working boot certainly has (addendum 19: about six minutes), this one is lost.
		if [ "$tick" -gt "$NO_FRAMES_BY" ] && ! grep -aq "frames in " "$log" 2>/dev/null; then
			echo "  no frames after $((NO_FRAMES_BY * 5)) s; booting again"
			break
		fi
		if grep -aq "frames in .* ms (.* a second)" "$log" 2>/dev/null; then
			sleep 30                       # let the first frames settle into a screen
			if ( cd "$root" && bash qemu/screendump.sh "$out" >/dev/null 2>&1 ); then
				# Frames flowing is not the same as a screen with something on it: a black
				# panel is 8061 bytes, the boot logo 13161, and a drawn system screen is
				# hundreds of kilobytes. Content is what is being waited for.
				if [ "$(stat -c %s "$out")" -gt "$MIN_BYTES" ]; then
					echo "the panel shows the system: $out ($(stat -c %s "$out") bytes)"
					# AFTER_CMD: something to do with the system while it is running -- a tap,
					# another capture. The guest is left alive for it, and killed only after.
					if [ -n "${AFTER_CMD:-}" ]; then
						sh -c "$AFTER_CMD"
					fi
					# KEEP=1 leaves the machine running: the point of a boot that works is
					# sometimes to use it (tap something, launch an application) rather than to
					# photograph it and shut it down.
					if [ -n "${KEEP:-}" ]; then
						echo "left running (KEEP=1): monitor on tcp:127.0.0.1:4444"
						exit 0
					fi
					kill $pid 2>/dev/null
					exit 0
				fi
				echo "  frames are flowing but the panel is still blank ($(stat -c %s "$out") bytes)"
			fi
		fi
		sleep 5
	done
	echo "  no frames this boot ($(stat -c %s "$log") bytes of log); booting again"
	# the guest is gone: the QEMU child first, then its timeout wrapper -- and only then wait until
	# the ports it held are free, because a retry that starts while 4444 or the VNC port is still
	# bound fails at once with a 187-byte log and looks like a stalled guest.
	kill $(pgrep -P $pid 2>/dev/null) 2>/dev/null
	kill $pid 2>/dev/null
	for _ in $(seq 1 20); do
		if ! ss -ltn 2>/dev/null | grep -qE ":(4444|5909)\b"; then break; fi
		sleep 1
	done
done
echo "no boot of $tries reached the screen" >&2
exit 1

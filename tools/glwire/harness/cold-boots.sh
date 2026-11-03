#!/bin/bash
# r1loop.sh -- the cold boots R1 asks for: five of them, at least four with 200 frames and a screen.
#
# One boot at a time (the shared payload.bin and ram.img, and port 9110, take one virtual machine),
# each run ending on the acceptance itself or on "no interface by 780 s", which is the known way a
# boot of this port fails (40% of them stall on lsd).  A stalled attempt does not count and does not
# end the loop.
set -u
IMAGE=${IMAGE:-/tmp/root-fixed-net.apfs}
WANT=${WANT:-4}
MAXTRIES=${MAXTRIES:-9}
: > /tmp/r1loop.log
pass=0
try=0
while [ "$pass" -lt "$WANT" ] && [ "$try" -lt "$MAXTRIES" ]; do
	try=$((try + 1))
	label="r1t$try"
	echo "=== attempt $try ($label), passes so far $pass, $(date -Is)" >> /tmp/r1loop.log
	MINFRAMES=200 MINSHOT=300000 NOUI_BY=780 harness/boot-measure.sh "$label" "$IMAGE" 1200 >> /tmp/r1loop.log 2>&1
	line=$(grep -a "xb: $label end=" /tmp/r1loop.log | tail -1)
	echo "$line" >> /tmp/r1loop.log
	case "$line" in
	*end=acceptance*)
		# The acceptance itself: 200 frames from the host and a screen bigger than 300 KB.  The
		# crash counts are read as well -- a SIGSEGV or a panic in that run fails it.
		if echo "$line" | grep -q "SIGSEGV=0 usertrap=0 panic=0"; then
			pass=$((pass + 1))
			echo "=== PASS $pass: $line" >> /tmp/r1loop.log
		else
			echo "=== FAIL (crash in an accepted run): $line" >> /tmp/r1loop.log
		fi
		;;
	*)
		echo "=== skip (no interface): $line" >> /tmp/r1loop.log
		;;
	esac
done
echo "=== r1loop done: $pass/$WANT after $try attempts, $(date -Is)" >> /tmp/r1loop.log
tail -1 /tmp/r1loop.log

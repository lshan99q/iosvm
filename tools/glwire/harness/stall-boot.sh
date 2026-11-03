#!/bin/bash
# stall-boot.sh LABEL IMAGE [MAX_SECONDS] -- one boot with the host's I/O sampled for the same window (P1')
#
# Then stalls.py puts the two on one clock: are the multi-second stalls the host's I/O, or the guest's
# own stop?  Two conditions are meant to be compared:
#
#   ZERO_RAM=1   the RAM file is rebuilt for the boot (the default; writes 2.4 GB)
#   ZERO_RAM=0   the previous boot's RAM file is reused (nothing written; the frame it left is still
#                there, so the interface detection is meaningless -- UI_GRACE=0 and MINFRAMES huge,
#                the run goes to MAX_SECS and only the stalls are read)
#
#   ZERO_RAM=1 ./harness/stall-boot.sh m8s1a /tmp/root-fixed-net.apfs 800
#   ZERO_RAM=0 ./harness/stall-boot.sh m8s1b /tmp/root-fixed-net.apfs 800
#
# Writes /tmp/<label>.log, -host.log, -shot.png, /tmp/<label>-hostio.log and prints the stall table.
set -u
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
GLWIRE=/home/lshan/DSH-desktop/iosvm/tools/glwire
LABEL=$1; IMAGE=$2; SECS=${3:-800}
ZERO_RAM=${ZERO_RAM:-1}
cd "$GLWIRE" || exit 1
mkdir -p /tmp/m8
"$GLWIRE/harness/hostio.sh" "/tmp/$LABEL-hostio.log" "$((SECS + 60))" &
IOPID=$!
sleep 1
echo "== $LABEL zero_ram=$ZERO_RAM $(date -Is)"
ZERO_RAM="$ZERO_RAM" MINFRAMES=999999 MINSHOT=999999999 UI_GRACE=0 XCMD="tlto_us=30000000" \
	"$GLWIRE/harness/boot-measure.sh" "$LABEL" "$IMAGE" "$SECS" 2>&1 | tail -2
kill "$IOPID" 2>/dev/null
sleep 1
python3 "$GLWIRE/harness/stalls.py" "/tmp/$LABEL.log" "/tmp/$LABEL-host.log" "/tmp/$LABEL-hostio.log" --min 1.0 2>&1 | tail -18

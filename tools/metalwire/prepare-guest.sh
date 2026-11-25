#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Install the metalwire guest plug-in and client into the experiment's own root image.
#
# This experiment never touches the shared HakiOS work tree. It keeps a private copy of the
# guest's root staging directory (MT_WORK/staging, made once by tools/metalwire/make-worktree.sh)
# and rebuilds only the small APFS root image (6 MB) from it.
#
#   tools/metalwire/prepare-guest.sh probe       install the diagnostic client, no launchd
#   tools/metalwire/prepare-guest.sh translate   run metal_frame synchronously (measurement mode)
#   tools/metalwire/prepare-guest.sh replay      the Replay baseline: launchd + glwire to port 9210
#   tools/metalwire/prepare-guest.sh replay-idle the Replay baseline with the scripted touches removed,
#                                                so the home screen is idle for the whole window
#   tools/metalwire/prepare-guest.sh ui          boot normally to the home screen, no metalwire client
#
# env: MT_WORK (default /home/lshan/DSH-desktop/mt-work)
#      HAKIOS_DIR (the guest tree the cross toolchain lives in)
#      MW_FRAMES, MW_TIMEOUT (passed to metal_frame)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MT=${MT_WORK:-/home/lshan/DSH-desktop/mt-work}
HAKIOS=${HAKIOS_DIR:-/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel}
MODE=${1:-translate}
FRAMES=${MW_FRAMES:-120}
TIMEOUT=${MW_TIMEOUT:-300}
STAGING=$MT/staging
GUEST_BUILD=${GUEST_BUILD:-$ROOT/guest/metal-plugin/build}

say() { printf 'prepare-guest: %s\n' "$*"; }
die() { printf 'prepare-guest: %s\n' "$*" >&2; exit 1; }

[ -d "$STAGING" ] || die "no $STAGING: run tools/metalwire/make-worktree.sh first"
[ -f "$HAKIOS/xnu-env.sh" ] || die "no HakiOS tree at $HAKIOS (set HAKIOS_DIR)"

# Every mode starts from the pristine /etc/rc: the translate mode replaces the launchd hand-over,
# and a second prepare would otherwise patch a patched file (the first replay-idle boot halted at
# init because the hand-over was gone).
if [ -f "$STAGING/etc/rc.metalwire-pristine" ]; then
	sudo cp "$STAGING/etc/rc.metalwire-pristine" "$STAGING/etc/rc"
else
	sudo cp "$HAKIOS/userland/root/etc/rc" "$STAGING/etc/rc"
	sudo cp "$STAGING/etc/rc" "$STAGING/etc/rc.metalwire-pristine"
fi

say "building the guest side (HAKIOS_DIR=$HAKIOS)"
HAKIOS_DIR=$HAKIOS "$ROOT/guest/metal-plugin/build.sh"

for f in "$GUEST_BUILD/HakiosMetal.bundle/HakiosMetal" "$GUEST_BUILD/metal_frame" "$GUEST_BUILD/metal_probe"; do
	[ -f "$f" ] || die "the build did not produce $f"
done

say "installing into $STAGING (sudo: the staging tree is root's)"
sudo install -d "$STAGING/lib/HakiosMetal.bundle" "$STAGING/bin"
sudo install -m 0755 "$GUEST_BUILD/HakiosMetal.bundle/HakiosMetal" "$STAGING/lib/HakiosMetal.bundle/HakiosMetal"
sudo install -m 0644 "$ROOT/guest/metal-plugin/Info.plist" "$STAGING/lib/HakiosMetal.bundle/Info.plist"
sudo install -m 0755 "$GUEST_BUILD/metal_frame" "$STAGING/bin/metal_frame"
sudo install -m 0755 "$GUEST_BUILD/metal_probe" "$STAGING/bin/metal_probe"

# The process gate in the plug-in: these programs are allowed to see the Metal device. The name
# "metal_test" is what boot-arg metal=1 allows; the clients file is the port's own way to add more.
printf 'metal_frame\nmetal_probe\n' | sudo tee "$STAGING/lib/HakiosMetal.clients" >/dev/null

# Where the guest finds the translator (the port's own convention, as glwire's /redwood/etc/glremote)
printf '10.0.2.2 9111\n' | sudo tee "$STAGING/etc/metalwire" >/dev/null
# Where a replay-baseline guest finds host_glwire: NOT the delivered 9110, so a metal experiment
# can never talk to the other agent's replayer.
printf '10.0.2.2 9210\n' | sudo tee "$STAGING/etc/glremote" >/dev/null

case "$MODE" in
probe)
	BLOCK='/redwood/bin/metal_probe &'
	KEEP_LAUNCHD=1
	;;
translate)
	# Synchronous: init waits, launchd does not start, the one vCPU belongs to this client.
	BLOCK="/redwood/bin/metal_frame -n $FRAMES -s $TIMEOUT -c ${MW_CRC_STRIDE:-1}"
	KEEP_LAUNCHD=0
	sudo rm -f "$STAGING/bin/glremote_on"
	;;
replay | replay-idle)
	BLOCK=''
	KEEP_LAUNCHD=1
	sudo touch "$STAGING/bin/glremote_on"
	;;
ui)
	BLOCK=''
	KEEP_LAUNCHD=1
	sudo rm -f "$STAGING/bin/glremote_on"
	;;
*)
	die "unknown mode $MODE (probe|translate|replay|replay-idle|ui)"
	;;
esac

say "patching /etc/rc (mode $MODE)"
sudo python3 - "$STAGING/etc/rc" "$BLOCK" "$KEEP_LAUNCHD" <<'PY'
import sys
path, block, keep = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
lines = open(path).read().splitlines()
out, skipping = [], False
for line in lines:
	if line.startswith("# >>> metalwire"):
		skipping = True
		continue
	if line.startswith("# <<< metalwire"):
		skipping = False
		continue
	if skipping:
		continue
	out.append(line)
marker = "# >>> metalwire"
if keep:
	insert = [marker] + ([block] if block else []) + ["# <<< metalwire"]
	for i, line in enumerate(out):
		if line.strip() == "exec /sbin/launchd.darwin":
			out[i:i] = insert
			break
	else:
		out += insert
else:
	for i, line in enumerate(out):
		if line.strip() == "exec /sbin/launchd.darwin":
			out[i] = marker
			out.insert(i + 1, block)
			out.insert(i + 2, "# launchd deliberately not started: metalwire measurement boot")
			out.insert(i + 3, "# <<< metalwire")
			break
	else:
		out += [marker, block, "# <<< metalwire"]
open(path, "w").write("\n".join(out) + "\n")
PY

if [ "$MODE" = replay-idle ]; then
	# The delivered touch script drives the screen from 900 s on; a baseline window wants the home
	# screen to itself, so its lines are commented out of this image's rc.
	say "removing the scripted touches from /etc/rc"
	sudo python3 - "$STAGING/etc/rc" <<'PYIDLE'
import sys
path = sys.argv[1]
out = []
for line in open(path).read().splitlines():
    if line.lstrip().startswith("/redwood/bin/rwtouch"):
        line = "# " + line + "   # metalwire replay-idle: no scripted touches"
    out.append(line)
open(path, "w").write("\n".join(out) + "\n")
PYIDLE
fi

say "building the root image"
sudo rm -f /tmp/metalwire-root.apfs
sudo python3 "$HAKIOS/tools/apfs/mkapfs.py" -z --plain fw -o /tmp/metalwire-root.apfs "redwood=$STAGING"
sudo chown "$(id -u):$(id -g)" /tmp/metalwire-root.apfs
cp /tmp/metalwire-root.apfs "$MT/hakios/userland/root.apfs"
say "root image: $MT/hakios/userland/root.apfs ($(stat -c %s "$MT/hakios/userland/root.apfs") bytes, mode $MODE)"

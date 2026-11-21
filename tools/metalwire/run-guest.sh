#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Boot the experiment's guest in its own QEMU instance and write the serial log where the
# measurement harness looks for it.
#
#   tools/metalwire/run-guest.sh TAG [SECONDS]
#
# Every run is instance 3 (monitor 4474, QMP 4475), one vCPU pinned to host cores 26-28, the
# experiment's own ram.img and root image, and the shared HakiOS tree used read-only. The other
# agent's QEMU (instance 1, 4454/4455) and its replay (port 9110) are never touched.
#
# env: MT_WORK (default /home/lshan/DSH-desktop/mt-work)
#      MW_CORES (default 26,27,28)  MW_NET (default 1)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MT=${MT_WORK:-/home/lshan/DSH-desktop/mt-work}
HAKIOS=${HAKIOS_DIR:-/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel}
TAG=${1:-run}
SECS=${2:-240}
CORES=${MW_CORES:-26,27,28}
LOG=$MT/logs/$TAG

mkdir -p "$LOG"
cd "$MT/hakios"

# a stale guest from an earlier run of ours is not wanted (ours is instance 3: ports 4474/4475)
if ss -ltn 2>/dev/null | grep -q ':4474 '; then
	echo "run-guest: something already listens on 4474; stop it first" >&2
	exit 1
fi

say() { printf 'run-guest: %s\n' "$*"; }
say "tag=$TAG seconds=$SECS cores=$CORES log=$LOG/serial.log"
env IMG=userland/system.apfs ROOT=userland/root.apfs QEMU_RAM_GB=${QEMU_RAM_GB:-7} QEMU_INSTANCE=3 \
	QEMU_NET=${MW_NET:-1} QEMU_SMP=1 QEMU_TB_MB=1536 \
	QEMU_EFI="$HAKIOS/qemu/efi64.fd" QEMU_VARS="$HAKIOS/qemu/vars.fd" \
	CMDLINE="${MW_CMDLINE:-cpus=1 fb_beat=0 fbscale=2 metal=1}" \
	nohup taskset -c "$CORES" timeout $((SECS + 60)) bash qemu/run-darwin.sh "$SECS" -L "$MT/hakios/qemu-roms" \
	>"$LOG/serial.log" 2>&1 &
echo $! >"$LOG/qemu.pid"
say "launched pid $(cat "$LOG/qemu.pid")"

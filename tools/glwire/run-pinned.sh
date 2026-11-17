#!/bin/bash
# The stage-25 delivered run: remote-only replay, 2 vCPUs pinned to dedicated cores, no panel blit.
#
#   tools/glwire/run-pinned.sh [TAG] [SECONDS]
#
# GLWIRE_PATCH_MIN_COVERAGE=2 keeps the static patch always on: the replay alone leaves the lower half
# black (and some frames wholly wrong), and the patch is what makes the picture complete.
#
# Why the pinning matters: with `-smp 2` and no pinning the guest panics inside boot
# ("Ticket spinlock timeout after 60000047 ticks", CPU 1 waiting on a ticket owned by CPU 0):
# a vCPU thread that the host deschedules long enough makes the other CPU spin to the timeout.
# On dedicated cores it runs (measured: 15.8 fps median vs 9.9 fps on one vCPU, same host load).
set -e
TAG=${1:-run-pinned}
SECS=${2:-2700}
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
K=${KERNEL_DIR:-$ROOT/../iosvm-work/hakios-xnu-kernel}
OUT="$ROOT/logs/$TAG"
CORES=${CORES:-20,21,22,23}          # vCPUs and QEMU's own threads
HOST_CORES=${HOST_CORES:-24,25}      # the replayer
NCPU=${NCPU:-2}                      # 2 is the sweet spot: 4 vCPUs cost twice the CPU for no wall-clock gain
mkdir -p "$OUT"

ps -eo pid=,comm= | awk '$2=="qemu-system-aar"||$2=="host_glwire"{print $1}' | xargs -r kill -TERM
sleep 1
cp "$ROOT/logs/stage24/full-patch/host-picture.bgra" /tmp/patch-full.bgra
rm -f /dev/shm/glwire-frame
cd "$ROOT"
GLWIRE_NO_PANEL=1 GLWIRE_STATIC_PATCH=1 GLWIRE_STATIC_PATCH_FILE=/tmp/patch-full.bgra \
GLWIRE_ACK_ONLY=1 GLWIRE_SHM=/dev/shm/glwire-frame \
GLWIRE_PATCH_MIN_COVERAGE=2 \
  nohup taskset -c "$HOST_CORES" ./tools/glwire/host_glwire --no-panel 9110 "$K/qemu/ram.img" >"$OUT/host.log" 2>&1 &
sleep 2
cd "$K"
python3 -c "open('qemu/vars.fd','wb').write(b'\xff'*(64<<20))"
env QEMU_RAM_GB=12 QEMU_INSTANCE=1 QEMU_NET=1 QEMU_SMP="$NCPU" QEMU_TB_MB=1536 \
  QEMU_EFI="$K/qemu/efi64.fd" QEMU_VARS="$K/qemu/vars.fd" \
  CMDLINE="cpus=$NCPU virtio_gpu=2 virtio_sound=1 virtio_net=1 fb_beat=0 fbscale=2 metal=1 gkbf=1 glwire_ack_only=1 launchd_disable_all_timeouts=1" \
  nohup timeout "$SECS" taskset -c "$CORES" bash qemu/run-darwin.sh "$SECS" -L "$K/qemu-roms" >"$OUT/serial.log" 2>&1 &
echo "up: tag=$TAG vcpus=$NCPU cores=$CORES host=$HOST_CORES logs=$OUT"
echo "watch: python3 tools/glwire/webview.py --port 6083 --bind 0.0.0.0 --crop 0"

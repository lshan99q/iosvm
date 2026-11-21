#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Make the experiment's private guest-run tree. It is a small skeleton that points at the shared
# HakiOS checkout for everything immutable (kernel, loader, roms, the 2.4 GB system image) and
# owns everything that a boot writes (ram.img, payload.bin, vars.fd, the ESP directory, the root
# image). Nothing here writes into the shared tree, so the other experiments on this host cannot
# be disturbed and our boots cannot be disturbed by theirs.
#
#   tools/metalwire/make-worktree.sh
#
# env: MT_WORK (default /home/lshan/DSH-desktop/mt-work)
#      HAKIOS_DIR (default /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel)
#
# Layout made:
#   $MT_WORK/hakios/          the guest tree the port's own qemu/run-darwin.sh expects
#   $MT_WORK/staging/         a private copy of the guest root staging (the root image's content)
#   $MT_WORK/logs/            one directory per run
set -euo pipefail

MT=${MT_WORK:-/home/lshan/DSH-desktop/mt-work}
K=${HAKIOS_DIR:-/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel}

say() { printf 'make-worktree: %s\n' "$*"; }
die() { printf 'make-worktree: %s\n' "$*" >&2; exit 1; }

[ -f "$K/qemu/run-darwin.sh" ] || die "no HakiOS checkout at $K"
[ -f "$K/userland/system.apfs" ] || die "no $K/userland/system.apfs (the 2.4 GB system image)"

mkdir -p "$MT/hakios/qemu" "$MT/build" "$MT/logs" "$MT/frames"

say "immutable parts as symlinks"
for d in prebuilt qemu-roms tools xnu-deps docs; do
	ln -sfn "$K/$d" "$MT/hakios/$d"
done
ln -sfn "$K/hakios-kernel" "$MT/hakios/hakios-kernel"
ln -sfn "$K/xnuloader" "$MT/hakios/xnuloader"
# The 2.4 GB system image is used read-only. A hard link (not a symlink) matters: run-darwin.sh
# takes its size with `stat -c %s`, which does not follow a symlink. `ln` of a root-owned file
# needs sudo because of protected_hardlinks.
if [ ! -e "$MT/hakios/userland/system.apfs" ]; then
	sudo ln "$K/userland/system.apfs" "$MT/hakios/userland/system.apfs"
fi

say "scripts are copies: they write into qemu/ and must not touch the shared tree"
cp -f "$K/qemu/run.sh" "$K/qemu/run-darwin.sh" "$K/qemu/screendump.sh" \
	"$K/qemu/stop.sh" "$K/qemu/tap.py" "$MT/hakios/qemu/"

say "small immutable files as symlinks, mutable ones as ours"
for f in efi64.fd ipsw kernel.syms ba.bin bootargs.bin; do
	ln -sfn "$K/qemu/$f" "$MT/hakios/qemu/$f"
done
# ram.img is built from the system image by run-darwin.sh (a 12 GB sparse file, 2.4 GB of data).
# It must live on the same filesystem as the source: os.copy_file_range() refuses a cross-device
# copy, and /dev/shm is a different one.
[ -L "$MT/hakios/qemu/ram.img" ] && rm -f "$MT/hakios/qemu/ram.img"
[ -f "$MT/hakios/qemu/ram.img" ] || : >"$MT/hakios/qemu/ram.img"
# The ESP is a directory QEMU mounts read-write; run.sh copies the loader into it.
[ -d "$MT/hakios/qemu/esp" ] || cp -a "$K/qemu/esp" "$MT/hakios/qemu/esp"
[ -f "$MT/hakios/qemu/vars.fd" ] || sudo cp "$K/qemu/vars.fd" "$MT/hakios/qemu/vars.fd"

if [ ! -d "$MT/staging" ]; then
	say "copying the guest root staging (18 MB; root-owned, so with sudo)"
	sudo cp -a "$K/userland/root" "$MT/staging"
	# glwire's switch: the Replay baseline is a separate mode (prepare-guest.sh replay)
	sudo rm -f "$MT/staging/bin/glremote_on"
	sudo cp "$MT/staging/etc/rc" "$MT/staging/etc/rc.metalwire-pristine"
fi
sudo chown -R "$(id -u):$(id -g)" "$MT/logs" "$MT/frames" 2>/dev/null || true

say "done: $MT"
say "next: tools/metalwire/prepare-guest.sh translate && tools/metalwire/run-guest.sh translate 240"

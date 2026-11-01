#!/bin/bash
# SPDX-License-Identifier: MIT
# Boot the port, wait for a real screen, then operate it from the host -- and keep every picture.
#
#   qemu/demo.sh [OUTDIR]            (default logs/demo)
#
# What it does, in order, with the machine left running while that happens:
#
#   home screen -> tap the Settings icon -> tap a row inside Settings -> swipe back home
#               -> pull down the Control Center
#
# Every step is `qemu/tap.py`, the QMP tool: the human monitor does not deliver the tablet's
# coordinates to this guest (docs/research/linux-kernel-build.md addendum 28). The boot itself is
# `qemu/wait-for-ui.sh`, because not every boot reaches the screen (addendum 17).
set -u
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/.." && pwd)
out=${1:-$root/logs/demo}
mkdir -p "$out"

say() { printf '\n== %s\n' "$*"; }
shot() { ( cd "$root" && timeout 25 bash qemu/screendump.sh "$out/$1.png" >/dev/null 2>&1 ); }

say "booting, waiting for a screen (this can take a few attempts)"
( cd "$root" && TRIES=6 MAX_WAIT=1800 KEEP=1 bash qemu/wait-for-ui.sh "$out/00-home.png" ) || {
	echo "no boot reached the screen" >&2; exit 1; }
shot 00-home

say "tapping the Settings icon"
( cd "$root" && ./qemu/tap.py 683 1689 >/dev/null ); sleep 25; shot 01-settings

say "tapping a row inside Settings"
( cd "$root" && ./qemu/tap.py 562 1586 >/dev/null ); sleep 25; shot 02-inside

say "swiping back to the home screen"
( cd "$root" && ./qemu/tap.py --swipe 562 2425 562 500 20 >/dev/null ); sleep 20; shot 03-home

say "pulling down the Control Center"
( cd "$root" && ./qemu/tap.py --swipe 1050 20 1050 900 18 >/dev/null ); sleep 25; shot 04-control-center

say "the machine was left running; stop it with: kill \$(ss -ltnp | grep :4444 | grep -o 'pid=[0-9]*' | cut -d= -f2)"
ls -l "$out"/*.png | awk '{print $5, $9}'

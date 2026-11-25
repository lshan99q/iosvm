#!/bin/bash
# SPDX-License-Identifier: MIT
#
# Repeat the translation-path measurement N times: one host translator, one boot, one frame loop.
# Writes MT_WORK/logs/mw-t<N>/ with serial.log, host.log and host.csv, and prints one summary line
# per run (frames, fps, guest-visible means) for the stage document.
#
#   tools/metalwire/measure-translation.sh FIRST LAST [FRAMES]
#
# It expects the root image to be prepared for translate mode already
# (tools/metalwire/prepare-guest.sh translate with the same MW_FRAMES / MW_CRC_STRIDE).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
MT=${MT_WORK:-/home/lshan/DSH-desktop/mt-work}
HAKIOS=${HAKIOS_DIR:-/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel}
FIRST=${1:?first run number}
LAST=${2:?last run number}
FRAMES=${3:-${MW_FRAMES:-400}}
SECS=${MW_SECONDS:-300}
STRIDE=${MW_CRC_STRIDE:-8}

for n in $(seq "$FIRST" "$LAST"); do
	tag=mw-t$n
	dir=$MT/logs/$tag
	mkdir -p "$dir" "$MT/frames/$tag"
	# free our own instance 3 and wire port 9111 only (never 4454/4455/9110)
	for port in 4474 9111; do
		pid=$(ss -ltn 2>/dev/null | sed -n "s/.*:$port .*pid=\([0-9]*\).*/\1/p" | head -1)
		[ -n "${pid:-}" ] && kill "$pid" 2>/dev/null && sleep 1
	done
	rm -f /dev/shm/metalwire-frame
	( cd "$ROOT" && taskset -c 29,30 ./host/metalwire/metalwire_host --gpu 1 --port 9111 \
		--frames "$FRAMES" --crc-stride "$STRIDE" --ram "$MT/hakios/qemu/ram.img" \
		--shm /dev/shm/metalwire-frame --csv "$dir/host.csv" --out "$MT/frames/$tag" \
		>"$dir/host.log" 2>&1 ) &
	host_pid=$!
	sleep 1
	MT_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1" "$ROOT/tools/metalwire/run-guest.sh" "$tag" "$SECS" >/dev/null
	for _ in $(seq 1 $((SECS / 2))); do
		grep -q "summary:" "$dir/host.log" 2>/dev/null && break
		sleep 2
	done
	kill "$host_pid" 2>/dev/null || true
	for port in 9111 4474; do
		pid=$(ss -ltn 2>/dev/null | sed -n "s/.*:$port .*pid=\([0-9]*\).*/\1/p" | head -1)
		[ -n "${pid:-}" ] && kill "$pid" 2>/dev/null
	done
	for _ in $(seq 1 20); do
		ss -ltn 2>/dev/null | grep -q ":4474 " || break
		sleep 1
	done
	host_line=$(grep -a "summary:" "$dir/host.log" | tail -1 || true)
	guest_line=$(grep -a "metalwire: summary:" "$dir/serial.log" | tail -1 || true)
	echo "$tag | $host_line"
	echo "$tag | $guest_line"
done

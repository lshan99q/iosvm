#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# doctor.sh — report what this machine lacks for the IOSVM build, and what to install.
# Read-only: it inspects, it never changes anything.
#
#   tools/doctor.sh           human-readable report
#   tools/doctor.sh --json    machine-readable
#
# Exit codes: 0 ready, 1 something required is missing, 2 usage error.

set -euo pipefail

JSON=0
case "${1:-}" in
	--json) JSON=1 ;;
	-h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	"") ;;
	*) echo "doctor.sh: unknown argument: $1" >&2; exit 2 ;;
esac

fail=0
ok=0
warn=0
rows=()

# need LEVEL NAME COMMAND HINT
need() {
	local level=$1 name=$2 cmd=$3 hint=${4:-}
	if command -v "$cmd" >/dev/null 2>&1 || eval "$cmd" >/dev/null 2>&1; then
		rows+=("ok|$name|$(command -v "${cmd%% *}" 2>/dev/null || echo present)|")
		ok=$((ok + 1))
	else
		rows+=("$level|$name|$hint|")
		if [ "$level" = "fail" ]; then fail=$((fail + 1)); else warn=$((warn + 1)); fi
	fi
}

kernel_release=$(uname -r)
arch=$(uname -m)
mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)
mem_gb=$((mem_kb / 1024 / 1024))
disk_gb=$(df -BG --output=avail . 2>/dev/null | tail -1 | tr -dc '0-9' || echo 0)

need ok    "python3"           python3
need ok    "C compiler"        cc
need ok    "make"              make
need fail  "meson"             meson          "apt install meson"
need fail  "ninja"             ninja          "apt install ninja-build"
need ok    "pkg-config"        pkg-config

need warn  "qemu-system-aarch64" qemu-system-aarch64 "apt install qemu-system-arm (only the VM targets)"
need warn  "aarch64 UEFI firmware" "test -f /usr/share/qemu-efi-aarch64/QEMU_EFI.fd" \
	"apt install qemu-efi-aarch64"
need warn  "lld-link"          lld-link       "apt install lld (only to build a loader with a real identity)"
need warn  "7z"                7z             "apt install 7zip (APFS reading, >= 23)"
need warn  "lz4"               lz4            "apt install lz4"

need warn  "clang"             clang          "apt install clang (the guest compiler and clang-format)"
need warn  "clang-format"      clang-format   "apt install clang-format"
need warn  "flex"              flex           "apt install flex (QEMU fork)"
need warn  "bison"             bison          "apt install bison (QEMU fork)"

need warn  "vulkaninfo"        vulkaninfo     "apt install vulkan-tools"
need warn  "Vulkan ICD"        "test -n \"$(ls /usr/share/vulkan/icd.d/*.json 2>/dev/null)\"" \
	"apt install mesa-vulkan-drivers"

# Memory and disk: the guest wants 6 GB minimum (ADR-002), the build wants space.
if [ "$mem_gb" -lt 8 ]; then
	rows+=("fail|memory|${mem_gb} GB: the guest asks for 6 GB minimum, and QEMU needs its own||")
	fail=$((fail + 1))
else
	rows+=("ok|memory|${mem_gb} GB||")
	ok=$((ok + 1))
fi
if [ "${disk_gb:-0}" -lt 20 ]; then
	rows+=("warn|disk|${disk_gb} GB free: an APFS image set is ~8 GB||")
	warn=$((warn + 1))
else
	rows+=("ok|disk|${disk_gb} GB free||")
	ok=$((ok + 1))
fi

# The contributor-gate check must pass on every machine.
if python3 tools/provenance-check.py --root . --quiet >/dev/null 2>&1; then
	rows+=("ok|provenance|clean||")
	ok=$((ok + 1))
else
	rows+=("fail|provenance|run: make provenance||")
	fail=$((fail + 1))
fi

if [ "$JSON" = 1 ]; then
	printf '{"arch":"%s","kernel":"%s","mem_gb":%s,"disk_gb":%s,"ok":%d,"warn":%d,"fail":%d,"items":[' \
		"$arch" "$kernel_release" "$mem_gb" "${disk_gb:-0}" "$ok" "$warn" "$fail"
	first=1
	for row in "${rows[@]}"; do
		IFS='|' read -r level name detail _ <<<"$row"
		[ "$first" = 1 ] || printf ','
		first=0
		printf '{"level":"%s","name":"%s","detail":"%s"}' "$level" "$name" "$detail"
	done
	printf ']}\n'
else
	echo "IOSVM doctor — $(uname -s) $(uname -m), kernel $kernel_release"
	echo
	for row in "${rows[@]}"; do
		IFS='|' read -r level name detail _ <<<"$row"
		case "$level" in
			ok)   printf '  \033[32mok  \033[0m %-24s %s\n' "$name" "$detail" ;;
			warn) printf '  \033[33mwarn\033[0m %-24s %s\n' "$name" "$detail" ;;
			fail) printf '  \033[31mFAIL\033[0m %-24s %s\n' "$name" "$detail" ;;
		esac
	done
	echo
	echo "$ok ok, $warn warnings, $fail failures"
	echo
	echo "A contributor gate only needs: make test-open   (no IPSW, no GPU, no QEMU fork)"
	echo "To boot a guest you also supply your own IPSW: see README.md and ADR-009."
fi

[ "$fail" -eq 0 ]

#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# check-spdx.sh — run the SPDX header check over this repository and every pinned component.
#
#   tools/check-spdx.sh            check all
#   tools/check-spdx.sh --help
#
# Components are separate repositories with their own file sets, so the parent's python checker
# (which reads `git ls-files` in one tree) has to be run once per tree. This script is that loop.
# Exit codes: 0 clean, 1 violations, 3 nothing to check.

set -euo pipefail

PROG=${0##*/}
case "${1:-}" in
	-h|--help) sed -n '3,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	"") ;;
	*) echo "$PROG: unknown argument: $1" >&2; exit 2 ;;
esac

fail=0
checked=0
for tree in . proto host/vkbridge; do
	if [ ! -e "$tree/.git" ]; then
		# A component that was never fetched is not a violation, but it must not look checked.
		printf '  %-16s skipped (not fetched)\n' "$tree"
		continue
	fi
	if python3 tools/check-spdx.py --root "$tree" >/tmp/spdx-$$.out 2>&1; then
		printf '  %-16s %s\n' "$tree" "$(cat /tmp/spdx-$$.out)"
		checked=$((checked + 1))
	else
		printf '  %-16s FAIL\n' "$tree"
		cat /tmp/spdx-$$.out
		fail=1
	fi
done
rm -f /tmp/spdx-$$.out

[ "$checked" -gt 0 ] || { echo "$PROG: no trees checked" >&2; exit 3; }
[ "$fail" -eq 0 ] && echo "check-spdx: clean ($checked tree(s))"
exit "$fail"

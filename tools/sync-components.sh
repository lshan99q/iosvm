#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# sync-components.sh — fetch the component repositories this one pins, and report drift.
#
#   tools/sync-components.sh --help
#   tools/sync-components.sh              # fetch/check out every component at its pinned commit
#   tools/sync-components.sh --check      # also report components whose remote main is ahead of the pin
#   tools/sync-components.sh --update     # move the pins to the components' remote main (prints, does not commit)
#
# ADR-010 fixes each component at a commit. This script is how a contributor gets exactly those
# commits, and how a maintainer notices that a component has moved on. It never commits anything:
# a pin bump is its own pull request, with the submodule rules of docs/zh/git-rules.md §8 applied.
#
# Exit codes: 0 ok, 1 drift found with --check, 2 usage/tooling error, 3 nothing to sync.

set -euo pipefail

PROG=${0##*/}
MODE=fetch            # fetch | check | update

usage() {
	sed -n '3,16p' "$0" | sed 's/^# \{0,1\}//'
	cat <<'EOF'

Options:
  --check             report components whose remote main is ahead of the pinned commit
  --update            move the pins to the components' remote main (prints the new SHAs; does not commit)
  -h, --help          this text
EOF
}

die() { local m=$1 c=${2:-1}; printf '%s: %s\n' "$PROG" "$m" >&2; exit "$c"; }
say() { printf '%s\n' "$*"; }

while [ $# -gt 0 ]; do
	case $1 in
		--check) MODE=check ;;
		--update) MODE=update ;;
		-h|--help) usage; exit 0 ;;
		*) usage >&2; exit 2 ;;
	esac
	shift
done

git rev-parse --git-dir >/dev/null 2>&1 || die "not in a git work tree" 2

if [ ! -f .gitmodules ]; then
	die "no .gitmodules: this repository pins no components yet (ADR-010 explains the layout)" 3
fi

command -v git >/dev/null || die "git is missing" 2

# Parse path/url pairs out of .gitmodules without depending on `git config --file` output order.
mapfile -t paths < <(git config -f .gitmodules --get-regexp '^submodule\..*\.path$' | awk '{print $2}')
[ "${#paths[@]}" -gt 0 ] || die ".gitmodules lists no submodules" 3

say "IOSVM components: ${#paths[@]} pinned"
say

drift=0
for path in "${paths[@]}"; do
	[ -d "$path" ] || mkdir -p "$path"

	case $MODE in
		fetch|check)
			if ! git submodule update --init -- "$path" >/dev/null 2>&1; then
				say "  $path: FETCH FAILED"
				drift=1
				continue
			fi
			pin=$(git rev-parse "HEAD:$path" 2>/dev/null || echo "?")
			head=$(git -C "$path" rev-parse HEAD 2>/dev/null || echo "?")
			if [ "$pin" != "$head" ]; then
				say "  $path: work tree is at ${head:0:12}, pinned at ${pin:0:12} (run: git submodule update)"
				drift=1
			else
				say "  $(printf '%-18s' "$path") ${pin:0:12}"
			fi
			;;
		update)
			pin=$(git rev-parse "HEAD:$path" 2>/dev/null || echo "?")
			if ! git -C "$path" fetch -q origin 2>/dev/null; then
				say "  $path: FETCH FAILED (offline?)"
				drift=1
				continue
			fi
			remote=$(git -C "$path" rev-parse origin/HEAD 2>/dev/null || git -C "$path" rev-parse origin/main 2>/dev/null || echo "?")
			if [ "$pin" = "$remote" ]; then
				say "  $(printf '%-18s' "$path") already at origin/main (${pin:0:12})"
			else
				say "  $path: pin ${pin:0:12} -> ${remote:0:12}"
				say "      to move it: git -C $path checkout ${remote:0:12} && git add $path"
				drift=1
			fi
			;;
	esac
done

say
if [ "$MODE" = check ] && [ "$drift" = 1 ]; then
	say "sync-components: drift or fetch problem (see above)"
	exit 1
fi
if [ "$MODE" = update ] && [ "$drift" = 1 ]; then
	say "sync-components: pins can move; the commands above are printed, nothing was changed"
	exit 0
fi
say "sync-components: ok"

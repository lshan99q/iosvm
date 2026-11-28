#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# run_tests.sh — run every xlt test binary.
#
#   tests/run_tests.sh [name ...]
#
# A test is any executable named *_test under tests/ or build/tests/. With no test sources the
# run is a pass, not a failure: the scaffold must be green before the first test exists. Names on
# the command line select tests by substring, which is how a contributor runs one at a time.
#
# Exit codes: 0 every selected test passed (or nothing to run), 1 at least one failed.

set -euo pipefail

here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
root=$(cd -- "$here/.." && pwd)
export XLT_ROOT="$root"

tests=()
while IFS= read -r -d '' path; do
    tests+=("$path")
done < <(find "$root/tests" "$root/build/tests" -maxdepth 1 -type f -name '*_test' -perm -u+x \
    -print0 2>/dev/null | sort -z)

if [ "$#" -gt 0 ]; then
    selected=()
    for path in "${tests[@]}"; do
        for want in "$@"; do
            case "${path##*/}" in
            *"$want"*)
                selected+=("$path")
                break
                ;;
            esac
        done
    done
    tests=(${selected[@]+"${selected[@]}"})
fi

if [ "${#tests[@]}" -eq 0 ]; then
    echo "xlt tests: nothing to run (no *_test under $root/tests or $root/build/tests)"
    exit 0
fi

failed=0
for path in "${tests[@]}"; do
    name=${path#"$root"/}
    if "$path"; then
        printf '  PASS  %s\n' "$name"
    else
        status=$?
        printf '  FAIL  %s (exit %d)\n' "$name" "$status"
        failed=$((failed + 1))
    fi
done

if [ "$failed" -ne 0 ]; then
    printf 'xlt tests: %d of %d failed\n' "$failed" "${#tests[@]}"
    exit 1
fi
printf 'xlt tests: pass (%d test binary(ies))\n' "${#tests[@]}"

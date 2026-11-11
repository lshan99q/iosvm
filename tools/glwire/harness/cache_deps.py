#!/usr/bin/env python3
"""Check appex/binary dependencies against a dyld shared cache without ipsw.

The cache's path table is visible as NUL-terminated strings; every image path
(`/System/Library/...`, `/usr/lib/...`) appears there.  This is the same table
dyld uses for its imagesText entries, even though the cache is split into
`dyld_shared_cache_arm64` + `.01`, `.02`, ... files.

Usage:
  cache_deps.py <cache> <binary> [binary ...]

`<cache>` may be the main cache or a glob; all files are scanned.  Dependencies
come from `llvm-objdump --macho --dylibs-used`.  Exit 1 if any dependency is
missing from the cache path set.
"""
import glob
import os
import re
import subprocess
import sys

OBJDUMP = os.environ.get("LLVM_OBJDUMP")
if not OBJDUMP:
    for candidate in ("/usr/lib/llvm-19/bin/llvm-objdump", "llvm-objdump"):
        if candidate.startswith("/") and os.path.exists(candidate):
            OBJDUMP = candidate
            break
    else:
        OBJDUMP = "llvm-objdump"


def cache_paths(pattern):
    paths = set()
    files = sorted(glob.glob(pattern))
    if not files:
        sys.exit(f"cache_deps: no cache file matches {pattern}")
    for path in files:
        if not os.path.isfile(path):
            continue
        out = subprocess.run(["strings", "-a", path], check=True,
                             capture_output=True, text=True, errors="replace")
        for line in out.stdout.splitlines():
            line = line.strip()
            if line.startswith("/") and " " not in line:
                paths.add(line)
    return paths


def dependencies(binary):
    out = subprocess.run([OBJDUMP, "--macho", "--dylibs-used", binary],
                         check=True, capture_output=True, text=True,
                         errors="replace").stdout
    deps = []
    for line in out.splitlines():
        line = line.strip()
        if line.endswith(":"):
            continue
        m = re.match(r"^(/[^ ]+?)(?:\s+\(compatibility|$)", line)
        if m:
            deps.append(m.group(1))
    return deps


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    cache = sys.argv[1]
    bins = sys.argv[2:]
    known = cache_paths(cache)
    print(f"cache: {cache} ({len(known)} image paths)")
    bad = 0
    for binary in bins:
        deps = dependencies(binary)
        missing = [d for d in deps if d not in known]
        print(f"{binary}: {len(deps)} deps, {len(missing)} missing")
        for dep in missing:
            print(f"  MISSING {dep}")
        bad += len(missing)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())

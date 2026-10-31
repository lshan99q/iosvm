#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check that every source file carries an SPDX license identifier.

The rule (CONTRIBUTING.md, section Code): the first non-shebang, non-empty line of a source file
is a comment holding `SPDX-License-Identifier: <id>`. Generated files declare
`SPDX-License-Identifier: MIT` too, plus `SPDX-FileCopyrightText: GENERATED` or similar.

Exit codes: 0 clean, 1 violations, 2 usage error.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Extensions we own and therefore must label.
CHECKED = {
    ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh",
    ".m", ".mm", ".py", ".sh", ".bash",
}

# Files owned by others, or generated from a template that carries the header.
SKIP_NAMES = {"configure", "config.guess", "config.sub", "depcomp", "install-sh", "missing"}
SKIP_PREFIX = ("qemu/", "third_party/", "vendor/")

SPDX_RE = re.compile(r"SPDX-License-Identifier:\s*([A-Za-z0-9.+-]+)")
SHEBANG_RE = re.compile(r"^#!")


def tracked(root: Path) -> list[str]:
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                         capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8", "surrogateescape").split("\0") if p]


def first_code_line(path: Path) -> str | None:
    """The first line that is not a shebang, not blank, not an encoding cookie."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    for lineno, line in enumerate(text.splitlines()[:40]):
        s = line.strip()
        if not s:
            continue
        if lineno == 0 and SHEBANG_RE.match(s):
            continue
        if re.match(r"^#.*coding[:=]", s):
            continue
        return line
    return None


def main() -> int:
    ap = argparse.ArgumentParser(description="Check SPDX headers on source files.")
    ap.add_argument("--root", default=".", help="repository root (default: .)")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    if not (root / ".git").exists():
        print(f"check-spdx: {root} is not a git work tree", file=sys.stderr)
        return 2

    bad: list[str] = []
    checked = 0
    for rel in tracked(root):
        if rel.startswith(SKIP_PREFIX) or Path(rel).name in SKIP_NAMES:
            continue
        path = root / rel
        if path.suffix not in CHECKED or not path.is_file():
            continue
        checked += 1
        line = first_code_line(path)
        if line is None or not SPDX_RE.search(line):
            bad.append(rel)

    if bad:
        print("missing `SPDX-License-Identifier:` on the first code line:")
        for rel in bad:
            print(f"  {rel}")
        print(f"\ncheck-spdx: {len(bad)} of {checked} file(s) unlabelled")
        return 1

    print(f"check-spdx: clean ({checked} source files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

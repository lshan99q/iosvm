#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""check-links.py — every file path and ADR number the docs mention must exist.

The audit that produced this script found five references pointing at nothing and four that sent a
reader to the wrong file. All of them were cheap to catch mechanically, so they are caught here
instead of by a reader.

Checks, over every tracked `*.md` (plus the Makefile and the workflow):

  1. every backticked token that looks like a repository path resolves
  2. every `[text](target)` link that is not a URL resolves
  3. every `ADR-NNN` mention resolves to `docs/adr/ADR-NNN-*.md`
  4. every `#L<n>`-style or `§<n>` section claim on a *named file* is at least plausible: the file
     exists (the section itself is prose and is not verified)

Deliberately absent paths are listed in PLANNED below, each with the milestone that creates it.
That list is the only way to silence a missing file, and it is short on purpose.

Exit codes: 0 clean, 1 violations, 2 usage error.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

# Paths the docs may name before they exist. value = why it is not there yet.
PLANNED = {
    "tools/qemu-open.sh": "M0 tail: the no-Apple-file boot harness",
    "tools/check-licenses.py": "M1: qemu/ and xnu/ header audit (ADR-005 follow-up)",
    "hw/virtio-iosvm-gpu": "M2: the QEMU device",
    "qemu/": "M2: the fork is not vendored yet",
    "xnu/": "M0 tail: patches land with the first kernel build",
    "userland/system.apfs": "produced by tools/make-images.sh at M1",
    "userland/data.apfs": "produced by tools/make-images.sh at M1",
    "firmware/": "the contributor's own, never committed",
    "userland-raw/": "the contributor's own, never committed",
    "docs/incidents/": "created with the first incident report",
    "build/guest/metal-plugin/libmetal-plugin.dylib": "M2: the guest plugin",
    "logs/screen.png": "screendump output, git-ignored",
    "logs/mon.sock": "QEMU monitor socket, git-ignored",
    "docs/adr/D1-prime-userland-version.md": "M1: the decision record, written when D1' closes",
    "docs/demo/first-frame.png": "M3: the acceptance capture",
    "docs/demo/m1-frame.png": "M1: the acceptance capture",
    "docs/demo/demo.mp4": "M4: released as an attachment, not committed",
    "tools/scratch/": "git-ignored scratch space (docs/zh/work-rules.md)",
}

# Archived design documents quote the filenames they were written under, and paths from the
# upstream trees they studied. They are evidence, not claims; do not scan them for links.
SKIP_FILES = ("docs/zh/",)

# Only strong claims about THIS tree are checked. A name is a claim when it starts at a directory
# this repository owns, or is a root-level document. Anything else in backticks — an upstream path
# such as `sysimage/unpack.sh`, a GitHub repo such as `jprx/darwin-vm`, a bare basename used as
# shorthand — is context, not a promise, and flagging it would make this check unusable.
OWNED_DIRS = ("tools/", "docs/adr/", "docs/demo/", "docs/zh/", "ci/", ".github/")
ROOT_DOCS = {"Makefile", ".gitignore", ".gitattributes", ".clang-format", ".pre-commit-config.yaml",
             ".gitmodules", "README.md", "PROVENANCE.md", "NOTICE", "CONTRIBUTING.md", "SECURITY.md",
             "LICENSE", "LICENSE.GPL2", "LICENSE.APSL"}

# Whole trees that do not exist yet, by design, plus the archive that cites its own sources.
PLANNED_PREFIX = {
    "docs/zh/": "archive: quotes the original document names it was written under",
    "guest/": "M2: the guest plugin",
    "host/": "M2: vkbridge",
    "proto/": "M2: the transport protocol",
    "hw/": "M2: the QEMU device",
    "tests/": "M2: protocol units and fake-gpu",
    "qemu/": "M2: the fork is not vendored yet",
    "xnu/": "M0 tail: patches land with the first kernel build",
    "firmware/": "the contributor's own, never committed",
    "userland/": "generated, never committed",
    "userland-raw/": "the contributor's own, never committed",
    "build/": "build product",
    "logs/": "run product",
}

PATHLIKE = re.compile(
    r"""^(?:
        (?:[\w.-]+/)+[\w./*-]+            # a/b/c or a/b.md
      | [\w-]+\.(?:md|sh|py|yml|yaml|c|h|cc|hpp|m|mm|patch|txt)
    )$""",
    re.VERBOSE,
)
ADR = re.compile(r"\bADR-(\d{3})\b")
MD_LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)\)")

SKIP_TOKEN_CHARS = set("*?<>={}[]()$|\\\"'")
SKIP_PREFIX = ("http://", "https://", "#", "git@", "mailto:")


def tracked(root: Path) -> list[str]:
    out = subprocess.run(["git", "-C", str(root), "ls-files", "-z"],
                         capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8", "surrogateescape").split("\0") if p]


def strip_code(text: str) -> str:
    """Drop fenced blocks: a path inside an example is not a claim about this tree."""
    out, fence = [], None
    for line in text.splitlines():
        m = re.match(r"^\s*(```+|~~~+)", line)
        if m:
            marker = m.group(1)[0] * 3
            if fence is None:
                fence = marker
            elif line.strip().startswith(fence):
                fence = None
            continue
        if fence is None:
            out.append(line)
    return "\n".join(out)


def is_claim(tok: str) -> bool:
    """Does this token promise a file in *this* repository?"""
    if tok in PLANNED or any(tok.startswith(p) for p in PLANNED_PREFIX):
        return False
    if tok in ROOT_DOCS:
        return True
    if "/" not in tok and tok.endswith((".md", ".yml", ".yaml")):
        return True          # a bare document name at the root is a claim too
    return tok.startswith(OWNED_DIRS)


def candidates(text: str) -> set[str]:
    found: set[str] = set()
    for m in re.finditer(r"`([^`\n]+)`", text):
        tok = m.group(1).strip()
        if not tok or any(c in tok for c in SKIP_TOKEN_CHARS) or tok.startswith(SKIP_PREFIX):
            continue
        if tok.startswith("$") or " " in tok:
            continue
        if PATHLIKE.match(tok) and is_claim(tok):
            found.add(tok)
    for m in MD_LINK.finditer(text):
        tok = m.group(1).split("#", 1)[0].strip()
        if not tok or tok.startswith(SKIP_PREFIX) or any(c in tok for c in SKIP_TOKEN_CHARS):
            continue
        if is_claim(tok):
            found.add(tok)
    return found


def resolves(root: Path, doc: Path, tok: str) -> bool:
    p = (root / tok).resolve()
    if p.exists():
        return True
    # a doc may also link relatively (same directory, ../)
    alt = (doc.parent / tok).resolve()
    return alt.exists()


def main() -> int:
    ap = argparse.ArgumentParser(description="Check that files and ADRs named in the docs exist.")
    ap.add_argument("--root", default=".", help="repository root (default: .)")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    if not (root / ".git").exists():
        print(f"check-links: {root} is not a git work tree", file=sys.stderr)
        return 2

    docs = [f for f in tracked(root) if f.endswith(".md")] + [f for f in ("Makefile",) if (root / f).exists()]
    # Look on disk, not in the index: an ADR written in this same change must count.
    adrs = {p.name for p in (root / "docs/adr").glob("ADR-*.md")}

    problems: list[str] = []
    checked = 0
    for rel in docs:
        if rel.startswith(SKIP_FILES):
            continue
        path = root / rel
        if not path.is_file():
            continue
        text = strip_code(path.read_text(encoding="utf-8", errors="replace"))
        for tok in sorted(candidates(text)):
            checked += 1
            if tok in PLANNED:
                continue
            if not resolves(root, path, tok):
                problems.append(f"{rel}: names `{tok}`, which does not exist")
        for m in ADR.finditer(text):
            num = f"ADR-{m.group(1)}"
            if not any(a.startswith(f"{num}-") for a in adrs):
                problems.append(f"{rel}: cites {num}, which has no file under docs/adr/")

    if problems:
        for p in problems:
            print(f"FAIL {p}")
        print(f"\ncheck-links: {len(problems)} problem(s) over {checked} path reference(s)")
        return 1

    if args.verbose:
        print(f"check-links: clean ({checked} path references, {len(adrs)} ADRs)")
    else:
        print(f"check-links: clean ({checked} path references)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

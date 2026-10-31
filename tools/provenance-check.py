#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Provenance and secret check for the IOSVM tree.

Fails when the tree could get the project into trouble:

  1. a path under firmware/ or userland/ is tracked
  2. a tracked file has an Apple-artifact extension (.ipsw, .dmg, .apfs, .im4p, ...)
  3. a tracked file is not UTF-8 text and is not declared in PROVENANCE.md
  4. a tracked file's magic bytes say Mach-O / IMG4 / APFS / DMG
  5. a tracked file contains a credential-shaped string
  6. a tracked script fetches Apple content

Run with --help for the options. No third-party dependency: this must work on a bare
GitHub runner with the stock Python.

Exit codes: 0 clean, 1 violations, 2 usage error.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

# 2. Extensions that only ever hold Apple artifacts.
FORBIDDEN_SUFFIX = (
    ".ipsw", ".dmg", ".apfs", ".im4p", ".im4m", ".im4r", ".bundle", ".metallib",
    ".bitcode", ".kernecache", ".kernelcache", ".dsc", ".ats", ".gsb",
    ".tc", ".aml", ".nand", ".dtre", ".sepi",
)
FORBIDDEN_NAME = {"bootkc", "dtree", "devicetree", "ramdisk", "trustcache"}

# 4. Magic bytes. Checked on the first 4 KiB.
MAGIC = (
    (b"\xcf\xfa\xed\xfe", "Mach-O 64-bit (Apple binary)"),
    (b"\xce\xfa\xed\xfe", "Mach-O 32-bit (Apple binary)"),
    (b"\xca\xfe\xba\xbe", "Mach-O fat / Java class (Apple binary or not text)"),
    (b"IM4P", "IMG4 payload (Apple firmware container)"),
    (b"IM4M", "IMG4 manifest (Apple signature)"),
    (b"NXSB", "APFS container superblock"),
    (b"koly", "DMG trailer (Apple disk image)"),
    (b"\x1f\x8b\x08", "gzip stream"),
    (b"BZh", "bzip2 stream"),
    (b"\xfd7zXZ\x00", "xz stream"),
    (b"PK\x03\x04", "zip archive"),
    (b"7z\xbc\xaf\x27\x1c", "7z archive"),
    (b"\x00asm", "wasm module"),
    (b"\x7fELF", "ELF binary"),
    (b"MZ", "PE/COFF binary"),
)

# 5. Credential shapes. Deliberately narrow: a false positive costs a maintainer one review.
SECRET = (
    (re.compile(rb"gh[pousr]_[A-Za-z0-9]{36,}"), "GitHub token"),
    (re.compile(rb"github_pat_[A-Za-z0-9_]{50,}"), "GitHub fine-grained token"),
    (re.compile(rb"AKIA[0-9A-Z]{16}"), "AWS access key id"),
    (re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH |PGP )?PRIVATE KEY-----"), "private key"),
    (re.compile(rb"xox[abprs]-[0-9A-Za-z-]{10,}"), "Slack token"),
    (re.compile(rb"(?i)(?:api[_-]?key|secret|password|token)\s*[:=]\s*['\"][A-Za-z0-9/+_-]{20,}['\"]"),
     "assigned secret"),
)

# 6. Apple content fetched by a script. A path passed on the command line is fine; a URL is not.
APPLE_FETCH = (
    (re.compile(rb"https?://[^\s'\"]*\.(?:ipsw|dmg)\b"), "download URL for an Apple image"),
    (re.compile(rb"(?:updates|mesu|appldnld)\.cdn-apple\.com"), "Apple update server"),
    (re.compile(rb"gs\.apple\.com"), "Apple signing server"),
)

TEXT_SUFFIX_OK = {
    ".md", ".txt", ".py", ".sh", ".c", ".h", ".cc", ".cpp", ".hpp", ".m", ".mm",
    ".yml", ".yaml", ".json", ".toml", ".ini", ".cfg", ".conf", ".mk", ".cmake",
    ".xml", ".plist", ".patch", ".diff", ".gitignore", ".gitattributes",
    ".clang-format", ".gitmodules", ".editorconfig", ".license", "",
}
TEXT_NAME_OK = {
    "Makefile", "makefile", "GNUmakefile", "LICENSE", "LICENSE.GPL2", "LICENSE.APSL",
    "NOTICE", "PROVENANCE.md", "README", "Dockerfile", "Kconfig", "meson.build",
}

MAX_SCAN = 2 * 1024 * 1024  # do not read a 500 MB text file into memory to look for a magic number


class Report:
    def __init__(self) -> None:
        self.violations: list[tuple[str, str]] = []
        self.checked = 0

    def fail(self, path: str, why: str) -> None:
        self.violations.append((path, why))


def tracked_files(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files", "-z"],
        capture_output=True, check=True,
    ).stdout
    return [p for p in out.decode("utf-8", "surrogateescape").split("\0") if p]


def declared_binaries(root: Path) -> set[str]:
    """Paths listed in the PROVENANCE.md table of allowed binaries."""
    prov = root / "PROVENANCE.md"
    allowed: set[str] = set()
    if not prov.exists():
        return allowed
    for line in prov.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.match(r"^\|\s*`?([^`|\s][^|]*)`?\s*\|", line)
        if m:
            p = m.group(1).strip().strip("`")
            if p and not p.startswith(("Path", "—", "-", ":")):
                allowed.add(p)
    return allowed


def looks_text(path: Path) -> bool:
    if path.suffix in TEXT_SUFFIX_OK or path.name in TEXT_NAME_OK:
        return True
    if path.name.startswith((".git", "LICENSE", "NOTICE", "README")):
        return True
    return False


def scan(root: Path, report: Report, allow_binaries: set[str]) -> None:
    for rel in tracked_files(root):
        report.checked += 1
        path = root / rel
        if not path.is_file():
            continue

        # 1. Apple-derived directories.
        if rel.startswith(("firmware/", "userland/")):
            report.fail(rel, "tracked under firmware/ or userland/ (Apple-derived, never committed)")
            continue

        # 2. Forbidden extensions.
        low = rel.lower()
        if low.endswith(FORBIDDEN_SUFFIX) or Path(low).name in FORBIDDEN_NAME:
            report.fail(rel, "forbidden artifact extension/name")
            continue

        # 3. Binary not declared.
        if not looks_text(path):
            try:
                head = path.read_bytes()[:4096]
            except OSError as exc:  # noqa: BLE001
                report.fail(rel, f"unreadable: {exc}")
                continue
            if b"\0" in head:
                if rel not in allow_binaries:
                    report.fail(rel, "binary file not declared in PROVENANCE.md")
                continue
            try:
                head.decode("utf-8")
            except UnicodeDecodeError:
                if rel not in allow_binaries:
                    report.fail(rel, "not UTF-8 text and not declared in PROVENANCE.md")
                continue

        # 4. Magic bytes.
        try:
            head = path.read_bytes()[:4096]
        except OSError as exc:  # noqa: BLE001
            report.fail(rel, f"unreadable: {exc}")
            continue
        for magic, what in MAGIC:
            if head.startswith(magic):
                report.fail(rel, f"magic bytes: {what}")
                break

        # 5 + 6. Content scan.
        if path.stat().st_size > MAX_SCAN:
            continue
        try:
            blob = path.read_bytes()
        except OSError as exc:  # noqa: BLE001
            report.fail(rel, f"unreadable: {exc}")
            continue
        for pattern, what in SECRET:
            m = pattern.search(blob)
            if m:
                # Never echo the secret itself.
                report.fail(rel, f"credential-shaped string at byte {m.start()}: {what}")
        if rel.endswith((".sh", ".py", ".yml", ".yaml", ".mk")) or path.name == "Makefile":
            for pattern, what in APPLE_FETCH:
                m = pattern.search(blob)
                if m:
                    report.fail(rel, f"fetches Apple content at byte {m.start()}: {what}")


def scan_history(root: Path, report: Report) -> int:
    """Every line ever added to any branch or tag, scanned for credential shapes.

    A secret in history is worse than one in the work tree: deleting the file does not remove the
    value from clones, forks or GitHub's event cache. This walks `git log -p --all` and reports
    which commit introduced such a line, never the line itself.
    """
    proc = subprocess.run(
        ["git", "-C", str(root), "log", "--all", "-p", "--format=%H", "--no-color",
         "--no-ext-diff", "-U0"],
        capture_output=True, check=False,
    )
    if proc.returncode != 0:
        report.fail("<history>", "git log failed; history not verified")
        return 0

    commits = 0
    for raw in proc.stdout.split(b"\n"):
        if len(raw) == 40 and not raw.startswith((b"+", b"-", b"@", b" ")):
            commits += 1
            continue
        if not raw.startswith((b"+", b"-")) or raw.startswith((b"+++", b"---")):
            continue
        body = raw[1:]
        for pattern, what in SECRET:
            if pattern.search(body):
                report.fail(f"<history:commit {commits}>", f"credential ever added: {what}")
                break
    return commits


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Check the IOSVM tree for Apple artifacts, undeclared binaries and credentials.")
    ap.add_argument("--root", default=".", help="repository root (default: .)")
    ap.add_argument("--quiet", action="store_true", help="only print violations")
    ap.add_argument("--history", action="store_true",
                    help="also scan every commit ever made (slower; needs a full clone)")
    args = ap.parse_args()

    root = Path(args.root).resolve()
    if not (root / ".git").exists():
        print(f"provenance-check: {root} is not a git work tree", file=sys.stderr)
        return 2

    report = Report()
    scan(root, report, declared_binaries(root))

    commits = 0
    if args.history:
        commits = scan_history(root, report)

    if report.violations:
        for path, why in report.violations:
            print(f"FAIL {path}: {why}")
        msg = (f"\nprovenance-check: {len(report.violations)} violation(s) in "
               f"{report.checked} tracked file(s)")
        if args.history:
            msg += f", {commits} commit(s) of history"
        print(msg)
        return 1

    if not args.quiet:
        msg = f"provenance-check: clean ({report.checked} tracked files"
        msg += f", {commits} commits of history)" if args.history else ")"
        print(msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())

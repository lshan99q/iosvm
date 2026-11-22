#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""surface_flip_test.py -- the WP-4 item 1.1 orientation switch, offline.

host_glwire's in-place write-back puts a picture row into a guest surface row. With
GLWIRE_SURFACE_FLIP=1 the mapping is the other way round, because the guest's compositor reads
SpringBoard's GL surface with the opposite row order from the host's read-back. The switch is
experimental: unset (the delivered default) must keep the old mapping.

The C side has an offline self test (`GLWIRE_SURFACE_FLIP_TEST=1`) that builds a numbered 4x5
picture, writes it through the very same write_picture_rows() the in-place path uses into a
strided buffer split across two segments, and checks the mapping, the row padding and the
published (SHM) picture. This driver runs it three times -- variable unset, ="0", ="1" -- and
asserts the mapping, so both the delivered default and the experiment are pinned.

    ./surface_flip_test.py

Needs ./host_glwire built first (`make`).
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "host_glwire")

CASES = (
    (None, "off", "identity"),
    ("0", "off", "identity"),
    ("1", "on", "vertical"),
)


def run_case(value, mode, mapping):
    env = os.environ.copy()
    env.pop("GLWIRE_SURFACE_FLIP", None)
    if value is not None:
        env["GLWIRE_SURFACE_FLIP"] = value
    env["GLWIRE_SURFACE_FLIP_TEST"] = "1"
    env.pop("GLWIRE_SHM", None)
    run = subprocess.run([HOST], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                         timeout=60, env=env)
    lines = [line for line in run.stdout.splitlines() if line.startswith("surface-flip-test:")]
    if run.returncode != 0:
        raise SystemExit("surface_flip_test: GLWIRE_SURFACE_FLIP=%r exited %d\n%s" %
                         (value, run.returncode, run.stdout))
    if len(lines) != 1:
        raise SystemExit("surface_flip_test: GLWIRE_SURFACE_FLIP=%r printed %d self-test lines\n%s" %
                         (value, len(lines), run.stdout))
    line = lines[0]
    for wanted in ("mode=%s" % mode, "mapping=%s" % mapping, "padding=kept",
                   "published=unflipped", "PASS"):
        if wanted not in line:
            raise SystemExit("surface_flip_test: FLIP=%r: %r missing from %r" % (value, wanted, line))
    print("PASS surface-flip: GLWIRE_SURFACE_FLIP=%s -> %s mapping, padding kept, "
          "published picture unflipped" % ("unset" if value is None else value, mapping))


def main():
    if not os.path.exists(HOST):
        raise SystemExit("surface_flip_test: %s is not built (run make first)" % HOST)
    for value, mode, mapping in CASES:
        run_case(value, mode, mapping)
    print("surface_flip_test: PASS (unset and =0 are identity, =1 is vertical; default unchanged)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

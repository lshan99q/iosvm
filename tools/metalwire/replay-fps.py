#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure the Replay path's frame rate from host_glwire's own per-frame log lines.

    tools/metalwire/replay-fps.py HOST_LOG [--windows N] [--seconds S] [--out CSV]

`host_glwire` writes one `host_glwire: replay …` line per composited frame. Its SHM publication
increments the sequence more than once per frame (the panel and the picture are published
separately), so counting SHM sequence deltas overcounts; the log line is the ground truth. The
watcher waits for the first lines, then reports N windows of S seconds each:
t_rel, frames, fps. This is the Replay control's measurement for stage 29 T6.
"""
import argparse
import re
import subprocess
import sys
import time

REPLAY = re.compile(r"^host_glwire: replay ", re.MULTILINE)


def replay_lines(path):
    try:
        with open(path, "rb") as handle:
            return len(REPLAY.findall(handle.read().decode("utf-8", "replace")))
    except OSError:
        return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host_log")
    ap.add_argument("--windows", type=int, default=3)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--wait", type=float, default=1500.0, help="seconds to wait for the first frames")
    ap.add_argument("--out", default="")
    args = ap.parse_args()

    deadline = time.monotonic() + args.wait
    while time.monotonic() < deadline and replay_lines(args.host_log) < 3:
        time.sleep(5)
    first = replay_lines(args.host_log)
    if first < 3:
        print("replay-fps: no Replay frames within %.0f s (%d lines)" % (args.wait, first),
              file=sys.stderr)
        return 1
    out = open(args.out, "w") if args.out else None
    if out:
        out.write("t_rel,frames,fps\n")
    start = time.monotonic()
    previous = first
    previous_at = start
    print("replay-fps: first frames seen (%d lines) after %.0f s" % (first, args.wait -
          (deadline - time.monotonic())), file=sys.stderr)
    for window in range(args.windows):
        time.sleep(args.seconds)
        now = time.monotonic()
        lines = replay_lines(args.host_log)
        frames = lines - previous
        fps = frames / (now - previous_at)
        print("replay-fps: window %d: %d frames in %.1f s = %.2f fps"
              % (window + 1, frames, now - previous_at, fps), file=sys.stderr)
        if out:
            out.write("%.3f,%d,%.2f\n" % (now - start, frames, fps))
            out.flush()
        previous, previous_at = lines, now
    if out:
        out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""window_fps.py GUEST.LOG [HOST.LOG] -- the driven window's steady state, by position rather than by clock.

    window_fps.py /tmp/w2.log /tmp/w2-host.log

fps.py needs a time stamp on the marks, which it takes from the last kernel-stamped console line
before each one.  A boot whose log carries none of those lines (the fifth stage's did not: the console
was launchd's own stamped lines and unstamped process output) leaves the marks at None and the window
empty.  The marks still delimit what the display driver reported between them, so this reads the
window by where the two marks are in the file:

    rwtouch: mark scroll-start ... rwtouch: mark scroll-end
        -> every "MiniIOMobileFramebuffer: N frames in M ms" line in between, summed

That is the same number R5/S5 is about (the frames the display driver actually put up, over the time
it says they took), just without needing a clock in the log.  The host side of the same boot is
printed when the host log is given.
"""
import re
import sys

FRAMES_IN = re.compile(r"MiniIOMobileFramebuffer:\s+(\d+) frames in (\d+) ms")
HOST_FRAME = re.compile(r"host_glwire: frame (\d+), .*?interval ([\d.]+) s")


def main():
    guest = open(sys.argv[1], errors="replace").read().splitlines()
    host = open(sys.argv[2], errors="replace").read().splitlines() if len(sys.argv) > 2 else []
    marks = [(i, line.split("rwtouch: mark", 1)[1].strip())
             for i, line in enumerate(guest) if "rwtouch: mark" in line]
    print("marks: " + (", ".join(f"{text}@{at}" for at, text in marks) or "none"))
    windows, start = [], None
    for at, text in marks:
        if text.endswith("scroll-start"):
            start = at
        elif text.endswith("scroll-end") and start is not None:
            windows.append((start, at))
            start = None
    for begin, end in windows:
        rows = []
        for line in guest[begin:end + 1]:
            found = FRAMES_IN.search(line)
            if found:
                rows.append((int(found.group(1)), int(found.group(2))))
        frames = sum(n for n, _ in rows)
        millis = sum(ms for _, ms in rows)
        print(f"window lines {begin}..{end}: {len(rows)} driver lines")
        for n, ms in rows[-8:]:
            print(f"  {n:3d} frames in {ms:5d} ms = {1000.0 * n / ms:5.2f} fps")
        if millis:
            print(f"  -> {frames} frames in {millis} ms = {1000.0 * frames / millis:.2f} fps")
        else:
            print("  -> no driver lines inside the window (a boot that never drew)")
    if host:
        intervals = []
        for line in host:
            found = HOST_FRAME.search(line)
            if found:
                intervals.append(float(found.group(2)))
        if intervals:
            tail = [iv for iv in intervals[-40:] if iv > 0]
            print(f"host: {len(intervals)} frames, last interval {intervals[-1]:.2f} s, "
                  f"last {len(tail)} mean {(sum(tail) / len(tail)):.3f} s "
                  f"= {(len(tail) / sum(tail)):.2f} fps")


if __name__ == "__main__":
    main()

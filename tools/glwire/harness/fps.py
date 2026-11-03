#!/usr/bin/env python3
"""fps.py GUEST.LOG [HOST.LOG] -- what the driven windows measured.

The guest's console has two kinds of line: the kernel's own, stamped with the seconds since boot
("[  439.432034]: ..."), and user processes' own, not stamped.  rwtouch's marks are the second kind,
so a mark takes the time of the last stamped line before it.  Inside a window this prints the
driver's own steady state ("MiniIOMobileFramebuffer: N frames in M ms"), which is the number R5 is
about, and from the host log the frame intervals of the same window (by frame number, since the
host does not stamp wall time).
"""
import re
import sys

TS = re.compile(r"^\[\s*(\d+\.\d+)\]:")
FRAMES_IN = re.compile(r"MiniIOMobileFramebuffer:\s+(\d+) frames in (\d+) ms")
FRAME_LINE = re.compile(r"host_glwire: frame (\d+), .*?interval ([\d.]+) s")
GUEST_FRAME = re.compile(r"hakios-gl: frame (\d+): .*?(\d+) us \(")


def main():
    guest = sys.argv[1]
    host = sys.argv[2] if len(sys.argv) > 2 else None
    marks = []
    stamped = []
    last_ts = None
    for line in open(guest, errors="replace"):
        m = TS.match(line)
        if m:
            last_ts = float(m.group(1))
        if "rwtouch: mark" in line:
            marks.append((line.split("rwtouch: mark", 1)[1].strip(), last_ts))
        if last_ts is not None:
            f = FRAMES_IN.search(line)
            if f:
                stamped.append((last_ts, int(f.group(1)), int(f.group(2))))
    print("marks:")
    for text, ts in marks:
        print(f"  {ts!s:>10}  {text}")
    windows = []
    start = None
    for text, ts in marks:
        if text.endswith("scroll-start"):
            start = ts
        elif text.endswith("scroll-end") and start is not None and ts is not None:
            windows.append((start, ts))
            start = None
    for begin, end in windows:
        inside = [(t, n, ms) for t, n, ms in stamped if begin <= t <= end]
        total_frames = sum(n for _, n, _ in inside)
        total_ms = sum(ms for _, _, ms in inside)
        print(f"\nwindow {begin:.1f} s .. {end:.1f} s ({end - begin:.1f} s):")
        for t, n, ms in inside:
            print(f"  [ {t:9.1f} ] {n:3d} frames in {ms:6d} ms = {1000.0 * n / ms if ms else 0:5.2f} fps")
        if total_ms:
            print(f"  -> {total_frames} frames in {total_ms} ms = {1000.0 * total_frames / total_ms:.2f} fps")
    if host:
        intervals = []
        for line in open(host, errors="replace"):
            f = FRAME_LINE.search(line)
            if f:
                intervals.append((int(f.group(1)), float(f.group(2))))
        if intervals:
            last = intervals[-1]
            print(f"\nhost: {len(intervals)} frames, last frame {last[0]}, last interval {last[1]} s")
            tail = [iv for _, iv in intervals[-40:] if iv > 0]
            if tail:
                print(f"  last {len(tail)} intervals mean {sum(tail) / len(tail):.3f} s "
                      f"= {len(tail) / sum(tail):.2f} fps")
    frames = []
    for line in open(guest, errors="replace"):
        f = GUEST_FRAME.search(line)
        if f:
            frames.append((int(f.group(1)), int(f.group(2)) / 1000.0))
    if frames:
        uniq = {}
        for number, us in frames:
            uniq[number] = us
        recent = [uniq[k] for k in sorted(uniq)[-20:]]
        print(f"\nguest: {len(uniq)} frames reported, last {len(recent)} take "
              f"{sum(recent) / len(recent):.0f} ms each ({1000.0 * len(recent) / sum(recent):.2f} fps)")


if __name__ == "__main__":
    main()

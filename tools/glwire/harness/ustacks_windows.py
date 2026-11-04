#!/usr/bin/env python3
"""ustacks_windows.py GUEST.LOG [--gap 1.0] [--top N]

Answers one question about the stalls: **during a stall, was the machine busy?**

Run the boot with three things in the command line:

    XCMD="... glframes=1 ustacks=top ustacks_at=420 ustacks_ms=1000 ustacks_count=600"

so the same guest log carries (a) a line per frame (`hakios-gl: frame N: ...`), (b) the kernel's
top samples (`top: in the last N ms the processor was busy X% of the time` and its rows), and
(c) launchd's own timestamped lines.  The timestamps come from (c): every line inherits the last
wall-clock stamp seen above it, which is exact enough at the second scale these gaps live at.

A gap of more than --gap seconds between one frame's stamp and the next is a stall.  For each stall
the script reports the busy% the kernel printed *inside* it, the mean inside versus outside, and the
three processes that used the most inside.  busy% far below the outside mean means the machine waited
(nothing was running); at or above it means something kept the processors -- and then the rows name it.

Why the guest log and not the host's: the host's `host_glwire: frame` lines carry the interval but no
clock, and in a remote-only boot the guest's own frames are the ones the display would show.
"""
import re
import sys
from datetime import datetime

STAMP = re.compile(r"\|(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d+)")
FRAME = re.compile(r"hakios-gl: frame (\d+):")
BUSY = re.compile(r"top: in the last (\d+) ms the processor was busy (\d+)%")
ROW = re.compile(r"top: +(\d+)\.(\d+)% +(\S+)\[(\d+)\]")


def parse(path):
    frames, samples, stamps = [], [], []   # frames: (t, index); samples: (t, busy, rows); stamps: all
    now = None
    for line in open(path, errors="replace"):
        m = STAMP.search(line)
        if m:
            try:
                now = datetime.strptime(m.group(1), "%Y-%m-%d %H:%M:%S.%f").timestamp()
                stamps.append(now)
            except ValueError:
                pass
        m = FRAME.search(line)
        if m and now is not None:
            frames.append((now, int(m.group(1))))
            continue
        m = BUSY.search(line)
        if m and now is not None:
            samples.append((now, int(m.group(2)), []))
            continue
        m = ROW.search(line)
        if m and samples:
            samples[-1][2].append((m.group(3), int(m.group(1))))
    return frames, samples, stamps


def main():
    path = sys.argv[1]
    gap = 1.0
    topn = 3
    if "--gap" in sys.argv:
        gap = float(sys.argv[sys.argv.index("--gap") + 1])
    if "--top" in sys.argv:
        topn = int(sys.argv[sys.argv.index("--top") + 1])

    frames, samples, stamps = parse(path)
    # The frame lines are the better clock, but a remote-only boot does not print them (the guest's
    # e->frames does not move there); every timestamped line is the fallback -- and during a stall the
    # whole machine is frozen, so *nothing* is stamped, which is exactly the gap being looked for.
    clock = frames if len(frames) >= 10 else list(zip(stamps, range(len(stamps))))
    kind = "frame lines" if clock is frames else "timestamped lines"
    print(f"{path}: {len(frames)} frame lines, {len(stamps)} timestamped lines ({kind} as the clock), "
          f"{len(samples)} top samples")
    if len(clock) < 3 or not samples:
        print("  need a boot with ustacks=top (and preferably glframes=1)")
        return 1

    # the interval from the clock's first entry to the first frame of the UI is not a stall, it is
    # "nothing was drawn yet", so the pairs start at the second entry
    stalls = [(a, b) for a, b in zip(clock[1:], clock[2:]) if b[0] - a[0] > gap]
    # only the windows the sampling actually covered: before ustacks_at the kernel printed nothing,
    # so a boot-phase gap would read as "0% busy" when it simply has no evidence
    first_sample = samples[0][0] if samples else 0
    stalls = [(a, b) for a, b in stalls if a[0] >= first_sample]
    outside = [s for s in samples if not any(a[0] <= s[0] <= b[0] for a, b in stalls)]
    mean_out = sum(s[1] for s in outside) / len(outside) if outside else 0.0

    print(f"\n{'stall from':>12} {'to':>8} {'s':>6} {'busy% in':>8}  busiest inside")
    inside_all = []
    for (t0, n0), (t1, n1) in stalls:
        inside = [s for s in samples if t0 <= s[0] <= t1]
        inside_all.extend(s[1] for s in inside)
        procs = {}
        for _, _, rows in inside:
            for name, pct in rows:
                procs[name] = procs.get(name, 0) + pct
        worst = ", ".join(f"{k} {v}%" for k, v in sorted(procs.items(), key=lambda kv: -kv[1])[:topn])
        b = max((s[1] for s in inside), default=0)
        print(f"at {t0 - stamps[0]:>7.1f}s  -> {t1 - stamps[0]:>7.1f}s {t1 - t0:>6.1f} {b:>7}%  {worst}")
    if inside_all:
        mean_in = sum(inside_all) / len(inside_all)
        print(f"\nmean busy% inside stalls {mean_in:.0f}%   outside {mean_out:.0f}%   "
              f"({len(stalls)} windows, {len(samples)} samples)")
        print("-> waited (nothing ran)" if mean_in < mean_out / 2 else
              "-> running (something held the processors)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

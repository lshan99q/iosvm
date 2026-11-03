#!/usr/bin/env python3
"""stalls.py GUEST.LOG HOST.LOG HOSTIO.LOG -- were the stalls the host's I/O? (P1')

The boot log's first line has the wall clock the boot started at; the host log has one "interval" per
frame; the sampler has one line a second.  Reconstructed on one clock:

    frame N arrives at  start + (the sum of the intervals before it)

(approximate: the host starts a few seconds after the boot, and the first interval is 0).  Then every
gap of more than --min seconds is a stall, and the sampler says what the host was doing inside it.

    stalls.py /tmp/m8p1.log /tmp/m8p1-host.log /tmp/m8/hostio.log [--min 1.0]

Prints: the stalls with their windows and the I/O inside them, and the mean I/O in stall windows
against the mean outside them.  A stall is "the host's I/O" when the inside is much larger than the
outside; it is not otherwise.
"""
import re
import statistics
import sys
from datetime import datetime

BOOT = re.compile(r"===== (\S+) (\S+)")
FRAME = re.compile(r"host_glwire: frame \d+.*?interval ([\d.]+) s")


def read_io(path):
    rows = []
    for line in open(path, errors="replace"):
        if line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 6:
            continue
        try:
            rows.append((float(parts[0]), float(parts[1]), float(parts[2]), int(parts[3]), int(parts[4]),
                         int(parts[5])))
        except ValueError:
            continue
    return rows


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    guest, host, io = sys.argv[1], sys.argv[2], sys.argv[3]
    min_gap = 1.0
    if "--min" in sys.argv:
        min_gap = float(sys.argv[sys.argv.index("--min") + 1])

    start = None
    for line in open(guest, errors="replace"):
        m = BOOT.search(line)
        if m:
            start = datetime.fromisoformat(m.group(1)).timestamp()
            break
    if start is None:
        sys.exit("stalls: no '===== <time> <label>' line in the guest log")

    intervals = []
    for line in open(host, errors="replace"):
        m = FRAME.search(line)
        if m:
            intervals.append(float(m.group(1)))
    io_rows = read_io(io)
    if not intervals or not io_rows:
        sys.exit(f"stalls: {len(intervals)} frames, {len(io_rows)} sampler lines")

    # the host starts a few seconds after the boot; the sampler's first line is the best anchor we have
    host_start = io_rows[0][0]
    if host_start < start - 5 or host_start > start + 120:
        host_start = start + 5.0

    def io_in(begin, end):
        inside = [r for r in io_rows if begin <= r[0] <= end]
        if not inside:
            return None
        return (statistics.mean(r[1] for r in inside), statistics.mean(r[2] for r in inside),
                sum(r[3] for r in inside), sum(r[4] for r in inside))

    stalls = []
    at = host_start
    for i, interval in enumerate(intervals):
        begin, end = at, at + interval
        at = end
        if interval >= min_gap:
            stalls.append((i + 1, begin, end, interval, io_in(begin, end)))
    print(f"stalls: {len(intervals)} frames, {len(stalls)} gaps >= {min_gap}s, "
          f"{len(io_rows)} sampler seconds")
    if not stalls:
        return
    print(f"{'frame':>6} {'gap s':>7} {'io_some%':>8} {'io_full%':>8} {'reads':>8} {'writes':>8}")
    for number, begin, end, interval, inside in stalls[-20:]:
        if inside is None:
            print(f"{number:>6} {interval:>7.2f} {'---':>8}")
            continue
        print(f"{number:>6} {interval:>7.2f} {inside[0]:>8.1f} {inside[1]:>8.1f} {inside[2]:>8} {inside[3]:>8}")
    inside = [r for r in io_rows if any(b <= r[0] <= e for _, b, e, _, _ in stalls)]
    outside = [r for r in io_rows if r not in inside]
    print()
    if inside and outside:
        print(f"  io_some: inside stalls {statistics.mean(r[1] for r in inside):5.1f}%  "
              f"outside {statistics.mean(r[1] for r in outside):5.1f}%")
        print(f"  io_full: inside stalls {statistics.mean(r[2] for r in inside):5.1f}%  "
              f"outside {statistics.mean(r[2] for r in outside):5.1f}%")
        print(f"  disk:    inside stalls {sum(r[3] for r in inside)} reads / {sum(r[4] for r in inside)} writes, "
              f"outside {sum(r[3] for r in outside)} / {sum(r[4] for r in outside)}")
    print("  (a stall is the host's I/O when the inside numbers are much larger than the outside ones)")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""P4: where a frame's time goes -- the guest's own work, the wait for the host, the host's replay.

    cost.py GUEST.LOG HOST.LOG

Reads two of the lines the recorder writes when a boot is measured with `glframes=1 touch_lat=1`:

    guest:  tl: g N began US ended US DRAWS            (mach_absolute_time microseconds)
    guest:  hakios-remote: f N wait US us sent S rows  (how long that frame waited for the picture)
    host:   host_glwire: frame N, ..., X ms, interval I s (F fps), ...

and prints, for the frames that have both halves, the decomposition of a frame:

    interval    frame N's start to frame N+1's start (the frame rate the display got)
    guest       began -> ended: CoreAnimation's calls and what the recorder did with them, run
                under TCG -- this is the "TCG 解释" column
    of which waiting for the host (the flush; the host's replay is inside it)
    host        the same host_glwire line's "X ms" (note: it prints seconds with an "ms" label,
                so the number in the log is 1000x too small -- it is corrected here, see --raw)
    other       interval - guest: time the display spent before the next frame began

The three numbers P4 asks for are therefore: guest (TCG), wait (host's replay inside it), and
other (idle / scheduling).  Frame totals are printed as means and medians over the window.
"""
import re
import statistics
import sys

TL = re.compile(r"tl: g (\d+) began (\d+) ended (\d+) (\d+)")
WAIT = re.compile(r"hakios-remote: f (\d+) wait (\d+) us sent (\d+)(.*)")
HOST = re.compile(r"host_glwire: frame (\d+), \d+x\d+, (\d+) records B, .*?, ([\d.]+) ms, interval ([\d.]+) s")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    guest_log, host_log = sys.argv[1], sys.argv[2]
    raw = "--raw" in sys.argv

    frames = {}     # number -> dict(began, ended, draws)
    waits = {}      # number -> microseconds
    for line in open(guest_log, "rb").read().decode("utf-8", "replace").splitlines():
        m = TL.search(line)
        if m:
            frames[int(m.group(1))] = dict(began=int(m.group(2)), ended=int(m.group(3)), draws=int(m.group(4)))
            continue
        m = WAIT.search(line)
        if m:
            waits[int(m.group(1))] = int(m.group(2))

    hosts = {}
    for line in open(host_log, "rb").read().decode("utf-8", "replace").splitlines():
        m = HOST.search(line)
        if m:
            number = int(m.group(1))
            took = float(m.group(3))
            hosts[number] = dict(records=int(m.group(2)), took=took if raw else took * 1000.0,
                                 interval=float(m.group(4)) * 1000.0)

    numbers = sorted(set(frames) & set(hosts))
    if len(numbers) < 2:
        sys.exit(f"cost: {len(frames)} guest frames, {len(hosts)} host frames, {len(numbers)} in common: "
                 "was the boot run with glframes=1 touch_lat=1, and is the host log the same boot?")
    rows = []
    for n in numbers:
        f = frames[n]
        if n + 1 not in frames:
            continue
        interval = (frames[n + 1]["began"] - f["began"]) / 1000.0
        guest = (f["ended"] - f["began"]) / 1000.0
        wait = waits.get(n, 0) / 1000.0
        host = hosts[n]["interval"] / 1000.0 if hosts[n]["interval"] else 0.0
        rows.append(dict(n=n, interval=interval, guest=guest, wait=wait, host=host,
                         other=max(0.0, interval - guest)))
    if not rows:
        sys.exit("cost: no frame had a following frame to make an interval from")

    def summary(key):
        values = [r[key] for r in rows]
        return statistics.mean(values), statistics.median(values)

    print(f"cost: {len(rows)} frames with both halves (guest frames {len(frames)}, host frames {len(hosts)})")
    print(f"{'frame':>6} {'interval ms':>11} {'guest ms':>9} {'wait ms':>8} {'host ms':>8} {'other ms':>9} {'records B':>10}")
    for r in rows[-12:]:
        print(f"{r['n']:>6} {r['interval']:>11.1f} {r['guest']:>9.1f} {r['wait']:>8.1f} {r['host']:>8.1f} "
              f"{r['other']:>9.1f} {hosts[r['n']]['records']:>10}")
    print()
    for key, label in (("interval", "frame interval"), ("guest", "guest (TCG shows as this)"),
                       ("wait", "  of which waiting for the host"), ("host", "host replay+readback"),
                       ("other", "other (idle / scheduling)")):
        mean, median = summary(key)
        print(f"  {label:<38} mean {mean:8.1f} ms   median {median:8.1f} ms")
    mean_interval = summary("interval")[0]
    print()
    print(f"  fps from intervals: {1000.0 / mean_interval if mean_interval else 0:.2f}")


if __name__ == "__main__":
    main()

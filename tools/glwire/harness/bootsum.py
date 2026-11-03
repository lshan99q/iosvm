#!/usr/bin/env python3
"""bootsum.py BOOTS.LOG -- the table P0' is judged by, and the budget a measurement needs.

    bootsum.py /tmp/boots.log [--want 8]

Reads what harness/boots.sh wrote: one "=== PASS"/"=== FAIL" line an attempt (the boot-measure end
line, with ui seconds, frames, panics) and a "why:" line under the failures.  Prints:

  * the attempt table (interface seconds, end reason, frames, panic),
  * passes / attempts, interface seen / attempts,
  * the failures by class (noui / crash / noframes / timeout) and the panic lock addresses,
  * the budget: attempts per pass, and the 95% upper bound of the pass rate given what was seen
    (a rule of three: with no failures in n boots the rate is at least 1-3/n).

The lock addresses come from the panic lines themselves, so every failure is attributed.
"""
import re
import sys

PASS = re.compile(r"=== PASS (\d+): xb: (\S+) end=(\S+) ui=(\d+)s .*?guestframes=(\d+) hostframes=(\d+)"
                  r".*?panic=(\d+) biggest=(\d+)")
FAIL = re.compile(r"=== FAIL (\S+): xb: \S+ end=(\S+) ui=(\d+)s .*?hostframes=(\d+).*?panic=(\d+) biggest=(\d+)")
WHY = re.compile(r"why: panic\(cpu \d+ caller \S+\): Ticket spinlock\[(0x[0-9a-f]+)\]")
DONE = re.compile(r"=== boots done: (.*)")


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "/tmp/boots.log"
    want = 8
    if "--want" in sys.argv:
        want = int(sys.argv[sys.argv.index("--want") + 1])
    rows, locks, classes = [], {}, {}
    line_after_fail = False
    for line in open(path, errors="replace"):
        m = PASS.search(line)
        if m:
            rows.append(dict(label=m.group(2), end=m.group(3), ui=int(m.group(4)),
                             frames=int(m.group(6)), panic=int(m.group(7)), ok=int(m.group(7)) == 0))
            line_after_fail = False
            continue
        m = FAIL.search(line)
        if m:
            label, end, ui, frames, panic = m.group(1), m.group(2), int(m.group(3)), int(m.group(4)), int(m.group(5))
            classes[end] = classes.get(end, 0) + 1
            rows.append(dict(label=label, end=end, ui=ui, frames=frames, panic=panic,
                             ok=False))
            line_after_fail = True
            continue
        if line_after_fail:
            m = WHY.search(line)
            if m:
                locks[m.group(1)] = locks.get(m.group(1), 0) + 1
            line_after_fail = False
    passes = sum(1 for r in rows if r["ok"])
    interfaces = sum(1 for r in rows if r["ui"] > 0)
    print(f"{'attempt':<10} {'ui s':>6} {'frames':>8} {'end':>10} {'panic':>6}")
    for r in rows:
        print(f"{r['label']:<10} {r['ui']:>6} {r['frames']:>8} {r['end']:>10} {r['panic']:>6}")
    print()
    print(f"  passes {passes}/{len(rows)} (want {want}), interface seen {interfaces}/{len(rows)}")
    if classes:
        print("  failures by class: " + ", ".join(f"{k} {v}" for k, v in sorted(classes.items())))
    if locks:
        print("  panic locks: " + ", ".join(f"{k} x{v}" for k, v in sorted(locks.items())))
    if rows:
        print(f"  attempts per pass: {len(rows) / passes:.1f}" if passes else "  attempts per pass: none")
        misses = len(rows) - passes
        if misses == 0 and len(rows) >= 3:
            print(f"  pass-rate floor (rule of three, 95%): >= {100.0 * (1 - 3.0 / len(rows)):.0f}%")
        else:
            print(f"  pass rate seen: {100.0 * passes / len(rows):.0f}% "
                  f"({passes}/{len(rows)}, {misses} failures)")


if __name__ == "__main__":
    main()

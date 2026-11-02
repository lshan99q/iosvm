#!/usr/bin/env python3
"""Check the physical addresses vtop_test got from syscall 556 against QEMU's RAM file.

The guest fills a buffer with a pattern that depends on the byte's offset in the buffer, asks the
kernel for the physical address of a span inside it, and prints the answer. QEMU's ram.img *is* the
guest's physical memory when the machine runs with memory-backend-file,share=on, so each segment
can be read at `pa - RAM_BASE` and compared with the pattern the guest wrote.

    vtop_verify.py LOG RAMIMG [--base 0x40000000] [--page 0x4000]

It prints the two md5s (what the guest wrote, what the host read at the guest's physical
addresses) and PASS when every byte matches.
"""
import argparse
import hashlib
import mmap
import re
import sys

RAM_BASE = 0x40000000
PAGE = 0x4000


def pattern(offset):
    return (offset * 37 + 11) & 0xFF


def parse_log(path):
    text = open(path, "rb").read().decode("utf-8", "replace")
    buf = va = None
    length = None
    count = None
    segments = []
    for line in text.splitlines():
        m = re.search(r"vtop_test: buf=(0x[0-9a-fA-F]+) bytes=(\d+)", line)
        if m:
            buf, _ = int(m.group(1), 16), int(m.group(2))
            continue
        m = re.search(r"vtop_test: va=(0x[0-9a-fA-F]+) len=(\d+)", line)
        if m:
            va, length = int(m.group(1), 16), int(m.group(2))
            continue
        m = re.search(r"vtop_test: count=(\d+)", line)
        if m:
            count = int(m.group(1))
            continue
        m = re.search(r"vtop_test: seg=(\d+) pa=(0x[0-9a-fA-F]+) len=(\d+)", line)
        if m:
            segments.append((int(m.group(2), 16), int(m.group(3))))
    return buf, va, length, count, segments


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("ram")
    ap.add_argument("--base", default=str(RAM_BASE))
    ap.add_argument("--page", default=str(PAGE))
    args = ap.parse_args()
    base = int(args.base, 0)
    page = int(args.page, 0)

    buf, va, length, count, segments = parse_log(args.log)
    if buf is None or va is None or length is None:
        print("vtop_verify: no vtop_test block in %s" % args.log)
        return 1
    if count is None or count != len(segments):
        print("vtop_verify: count=%r but %d segment lines" % (count, len(segments)))
        return 1
    if not segments:
        print("vtop_verify: no segments")
        return 1
    covered = sum(n for _, n in segments)
    if covered != length:
        print("vtop_verify: segments cover %d of %d bytes" % (covered, length))
        return 1
    if (segments[0][0] & (page - 1)) != (va & (page - 1)):
        print("vtop_verify: first physical address is not offset like the virtual one")
        return 1

    fd = open(args.ram, "rb")
    ram = mmap.mmap(fd.fileno(), 0, access=mmap.ACCESS_READ)
    guest = hashlib.md5()
    host = hashlib.md5()
    at_va = va
    at_pa = 0
    for pa, length_seg in segments:
        if pa < base or pa + length_seg > len(ram):
            print("vtop_verify: segment 0x%x+%d is outside the RAM file" % (pa, length_seg))
            return 1
        offset = pa - base
        physical = ram[offset:offset + length_seg]
        start = at_va - buf
        expected = bytes(pattern(start + i) for i in range(length_seg))
        guest.update(expected)
        host.update(physical)
        if physical != expected:
            for i, (a, b) in enumerate(zip(physical, expected)):
                if a != b:
                    print("vtop_verify: first difference at physical 0x%x (virtual 0x%x): %02x != %02x"
                          % (pa + i, at_va + i, a, b))
                    break
            print("vtop_verify: FAIL")
            return 1
        at_va += length_seg
        at_pa += length_seg
    if at_va != va + length:
        print("vtop_verify: covered virtual range ends at 0x%x, expected 0x%x" % (at_va, va + length))
        return 1
    print("vtop_verify: %d segment(s), %d bytes, physical range 0x%x..0x%x"
          % (len(segments), covered, segments[0][0], segments[-1][0] + segments[-1][1]))
    print("vtop_verify: guest md5 %s" % guest.hexdigest())
    print("vtop_verify: host  md5 %s" % host.hexdigest())
    print("vtop_verify: PASS: the bytes at the physical addresses are the guest's buffer")
    return 0


if __name__ == "__main__":
    sys.exit(main())

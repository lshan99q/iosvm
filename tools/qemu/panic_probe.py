#!/usr/bin/env python3
"""panic_probe.py -- read the CPUs' registers when a guest panics on a lock timeout.

The Ticket spinlock panic says which thread owns the lock and which CPU it is on, but the kernel's
stackshot fails ("debugger synchronization failed") because QEMU's TCG cannot halt the other vCPUs
for the in-guest debugger. QMP can stop the whole machine from outside, and `info registers -a`
then gives the PC and SP of every vCPU -- including the one holding the lock. The kernel's stacks
are in the linear map, so a stack word can be read with `xp` at (va - 0xfffffff000000000).

    ./panic_probe.py --log /tmp/iosvm-live.log --qmp 4455 --out /tmp/panic.txt

Run it before the panic; it exits when it has written the dump, or after --timeout seconds.
"""
import argparse
import json
import os
import re
import socket
import sys
import time

KERNEL_BIAS = 0xFFFFFFF000000000


def qmp_connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=20)
    f = s.makefile("rwb", buffering=0)
    f.readline()

    def call(obj):
        f.write((json.dumps(obj) + "\n").encode())
        while True:
            line = f.readline()
            if not line:
                raise OSError("QMP closed")
            message = json.loads(line)
            if "return" in message or "error" in message:
                return message

    call({"execute": "qmp_capabilities"})
    return s, call


def hmp(call, line):
    return call({"execute": "human-monitor-command", "arguments": {"command-line": line}}).get("return", "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", default="/tmp/iosvm-live.log")
    ap.add_argument("--qmp", type=int, default=4455)
    ap.add_argument("--out", default="/tmp/panic-cpus.txt")
    ap.add_argument("--timeout", type=int, default=1800)
    args = ap.parse_args()
    deadline = time.time() + args.timeout
    trigger = None
    while time.time() < deadline:
        try:
            text = open(args.log, errors="ignore").read()
        except FileNotFoundError:
            text = ""
        match = re.search(r"panic\(cpu \d+.*$", text, re.M)
        if match:
            trigger = match.group(0)
            break
        time.sleep(1)
    if trigger is None:
        print("panic_probe: no panic within %d seconds" % args.timeout)
        return 1
    print("panic_probe: %s" % trigger, flush=True)

    s, call = qmp_connect(args.qmp)
    print("panic_probe: stopping the machine", flush=True)
    print(hmp(call, "stop"))
    time.sleep(0.5)
    registers = hmp(call, "info registers -a")
    out = open(args.out, "w")
    out.write(trigger + "\n\n" + registers + "\n")

    owner = None
    m = re.search(r"current owner: (0x[0-9a-f]+).*?on CPU (\d+)", trigger)
    if m:
        owner = int(m.group(1), 16)
        cpu = int(m.group(2))
        print("panic_probe: owner thread %#x on CPU %d" % (owner, cpu), flush=True)
        out.write("owner thread %#x on CPU %d\n" % (owner, cpu))

    # Each CPU's PC and SP; its stack is in the kernel's linear map.
    for block in re.split(r"\nCPU#", registers):
        m = re.match(r"(\d+)\n", block)
        if not m:
            continue
        cpu = int(m.group(1))
        pc = re.search(r"PC=([0-9a-f]+)", block)
        sp = re.search(r"SP=([0-9a-f]+)", block)
        lr = re.search(r"LR=([0-9a-f]+)", block)
        if not (pc and sp):
            continue
        pc = int(pc.group(1), 16)
        sp = int(sp.group(1), 16)
        lr = int(lr.group(1), 16) if lr else 0
        line = "CPU %d: PC=%#x LR=%#x SP=%#x" % (cpu, pc, lr, sp)
        print("panic_probe: " + line, flush=True)
        out.write(line + "\n")
        if sp >= KERNEL_BIAS:
            pa = sp - KERNEL_BIAS
            words = hmp(call, "xp /48gx %#x" % pa)
            print("panic_probe: CPU %d stack at %#x" % (cpu, pa), flush=True)
            out.write("CPU %d stack (physical %#x):\n%s\n" % (cpu, pa, words))
    out.close()
    s.close()
    print("panic_probe: wrote %s" % args.out)
    return 0


raise SystemExit(main())

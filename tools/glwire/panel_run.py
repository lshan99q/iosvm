#!/usr/bin/env python3
"""Wait for the guest's home screen, open Settings, pause the machine, and let the host draw that
frame back into the panel it came from.

The machine is left paused: the guest's own compositor redraws at several frames a second, and a
picture written into the panel while it runs is overwritten before it can be photographed. Paused,
the framebuffer is the picture of record.

    panel_run.py <ram.img> <base> [--log FILE] [--out DIR]

Nothing here restarts the guest.
"""
import argparse
import json
import mmap
import os
import socket
import subprocess
import sys
import time
from PIL import Image

W, H = 1125, 2436
FRAME = W * H * 4
RAMP = 0x40000000


def qmp(commands, port=4445):
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    f = s.makefile("rwb", buffering=0)
    f.readline()

    def call(obj):
        f.write((json.dumps(obj) + "\n").encode())
        while True:
            line = f.readline()
            if not line:
                return None
            message = json.loads(line)
            if "return" in message or "error" in message:
                return message

    call({"execute": "qmp_capabilities"})
    out = [call(c) for c in commands]
    s.close()
    return out


def tap(x, y):
    def ev(ax, v):
        return {"type": "abs", "data": {"axis": ax, "value": int(v / (W if ax == "x" else H) * 0x7FFF)}}

    events = [{"execute": "input-send-event", "arguments": {"events": [ev("x", x), ev("y", y)]}},
              {"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}},
              {"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}}]
    qmp(events)


class Frog:
    def __init__(self, path, base):
        self.file = open(path, "rb")
        self.map = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)
        self.offset = base - RAMP

    def frame(self):
        return self.map[self.offset:self.offset + FRAME]

    def png(self, path):
        Image.frombytes("RGBA", (W, H), self.frame(), "raw", "BGRA").convert("RGB").save(path)


def colourful(frame, step=8):
    """How much of the picture is in colour -- below the kernel's boot band as well, which is a
    rainbow strip at the top and black under it, and passed for a home screen once."""
    n = hit = 0
    first = W * 1000 * 4
    for i in range(first, len(frame) - 4, 4 * step):
        b, g, r = frame[i], frame[i + 1], frame[i + 2]
        n += 1
        if max(r, g, b) - min(r, g, b) > 40:
            hit += 1
    return hit / n


def difference(a, b, step=8):
    n = hit = 0
    for i in range(0, len(a) - 4, 4 * step):
        n += 1
        if abs(a[i] - b[i]) > 8 or abs(a[i + 1] - b[i + 1]) > 8 or abs(a[i + 2] - b[i + 2]) > 8:
            hit += 1
    return hit / n


def wait_base(log, timeout=180):
    end = time.time() + timeout
    while time.time() < end:
        try:
            for line in open(log, errors="ignore"):
                if "display 1125 x 2436 at " in line:
                    return int(line.rsplit(" ", 1)[1], 16)
        except FileNotFoundError:
            pass
        time.sleep(1)
    raise SystemExit("panel_run: the loader never named a framebuffer")


def wait_log(log, marker, timeout=600):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if marker in open(log, errors="ignore").read():
                return
        except FileNotFoundError:
            pass
        time.sleep(2)
    raise SystemExit("panel_run: %r never appeared in the log" % marker)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ram")
    ap.add_argument("--base", type=lambda v: int(v, 0), default=None)
    ap.add_argument("--log", default="/tmp/iosvm-0158.log")
    ap.add_argument("--out", default="/tmp/panel-run")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    base = args.base or wait_base(args.log)
    print("panel_run: framebuffer at %#x" % base, flush=True)

    frog = Frog(args.ram, base)
    # The RAM file still holds the previous machine's last frame until this one draws. Wait for the
    # window server, by which time the boot logo has replaced it, before believing any picture.
    wait_log(args.log, "backboardd[")
    time.sleep(20)
    # 1. the home screen: the boot logo is black and white, the home screen is not
    home = None
    quiet = 0
    previous = None
    last_report = 0
    while True:
        frame = frog.frame()
        c = colourful(frame)
        if c > 0.02:
            if previous is not None and difference(previous, frame) < 0.002:
                quiet += 1
            else:
                quiet = 0
            if quiet >= 2:
                home = frame
                break
        previous = frame
        if time.time() - last_report > 15:
            print("panel_run: waiting for the home screen (colour %.3f)" % c, flush=True)
            last_report = time.time()
        time.sleep(2)
    frog.png(os.path.join(args.out, "home.png"))
    print("panel_run: home screen is up", flush=True)

    # 2. Settings: the icon is the third of the fifth row of the home screen
    tap(663, 1690)
    time.sleep(3)
    settled = None
    for _ in range(30):
        frame = frog.frame()
        if difference(home, frame) > 0.20:
            if settled is not None and difference(settled, frame) < 0.001:
                break
            settled = frame
        else:
            settled = None
        time.sleep(1)
    time.sleep(1)
    frog.png(os.path.join(args.out, "settings.png"))
    print("panel_run: Settings is up; pausing the machine", flush=True)

    # 3. pause, so the pictures stay put
    print(qmp([{"execute": "stop"}]), flush=True)
    time.sleep(1)
    frog.png(os.path.join(args.out, "guest.png"))

    # 4. the host draws that frame back through the wire
    here = os.path.dirname(os.path.abspath(__file__))
    drawn = subprocess.run([os.path.join(here, "frame_replay"), args.ram, hex(base),
                            "--out", os.path.join(args.out, "host.bgra")],
                           capture_output=True, text=True)
    print(drawn.stdout, end="")
    print(drawn.stderr, end="", file=sys.stderr)
    frog.png(os.path.join(args.out, "host.png"))

    # 5. what the display device itself has
    r = qmp([{"execute": "screendump", "arguments": {"filename": os.path.join(args.out, "panel.ppm")}}])
    print("panel_run: screendump", r, flush=True)
    print("panel_run: the machine is paused at %#x; pictures in %s" % (base, args.out), flush=True)


main()

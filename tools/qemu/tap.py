#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tap the panel of a running machine, from the host.

The monitor's own commands do not deliver coordinates to this guest: `mouse_move` sends nothing
at all, and `input-send-event` typed into the human monitor delivers no EV_ABS (only its buttons
arrive -- see docs/research/linux-kernel-build.md addendum 27). Over QMP the same command works:

    virtio-input: event from 1: type 3 code 0 value 19893     <- EV_ABS ABS_X
    virtio-input: event from 1: type 3 code 1 value 12132     <- EV_ABS ABS_Y

    qemu/tap.py X Y            a tap at panel coordinates, 1125x2436 like the captures
    qemu/tap.py --swipe X0 Y0 X1 Y1 [steps]

The machine must be running with its QMP socket on 127.0.0.1:4445 (qemu/run.sh does that), and
its panel is assumed to be the 1125x2436 this project's captures are.
"""
import json, socket, sys, time

PANEL_W, PANEL_H = 1125, 2436
ABS_RANGE = 0x7FFF


def qmp(commands, port=4445):
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    f = s.makefile("rwb", buffering=0)

    def call(obj):
        f.write((json.dumps(obj) + "\n").encode())
        while True:
            line = f.readline()
            if not line:
                return None
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "return" in message or "error" in message:
                return message

    time.sleep(0.3)
    call({"execute": "qmp_capabilities"})
    for command in commands:
        print(call(command))
    s.close()


def abs_events(x, y):
    return [
        {"type": "abs", "data": {"axis": "x", "value": int(x / PANEL_W * ABS_RANGE)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y / PANEL_H * ABS_RANGE)}},
    ]


def button_events(down):
    return [{"type": "btn", "data": {"down": down, "button": "left"}}]


def send(events):
    return {"execute": "input-send-event", "arguments": {"events": events}}


def main(argv):
    if argv and argv[0] == "--swipe":
        x0, y0, x1, y1 = (int(v) for v in argv[1:5])
        steps = int(argv[5]) if len(argv) > 5 else 12
        # A finger is down for the whole gesture: position first, then the button goes down, the
        # positions move, and only then does it come up. (Sent the other way round -- positions,
        # then a press and a release in the last place -- nothing happens, which is how this was
        # found: the first version of this file did exactly that and the screen did not move.)
        qmp([send(abs_events(x0, y0)), send(button_events(True))])
        events = []
        for i in range(1, steps + 1):
            x = x0 + (x1 - x0) * i // steps
            y = y0 + (y1 - y0) * i // steps
            events.append(send(abs_events(x, y)))
        events.append(send(button_events(False)))
        qmp(events)
        return 0
    if len(argv) == 2:
        x, y = int(argv[0]), int(argv[1])
        qmp([send(abs_events(x, y)), send(button_events(True)), send(button_events(False))])
        return 0
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

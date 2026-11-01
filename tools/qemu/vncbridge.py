#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""vncbridge.py — a VNC server whose mouse is the guest's touch screen.

QEMU's own VNC console does not deliver this guest a pointer: the client's motion arrives at the
driver as EV_REL, which the touch driver cannot use (the measurement is in
docs/research/linux-kernel-build.md), so a VNC viewer can watch the machine but not operate it.

This is the bridge that makes the mouse work. It serves the screen itself -- one screendump per
request, at the panel's own 1125x2436 -- and turns the client's pointer events into QMP
`input-send-event`s, the path that is known to reach the guest:

    a press  -> absolute x/y, then the button down
    a drag   -> absolute x/y while the button is held
    a release-> the button up

    tools/qemu/vncbridge.py [--port 5910] [--qmp 4445] [--fps 1]

Then point any VNC viewer at 127.0.0.1:5910 and use it: clicking the Settings icon opens Settings.
The machine must be running with its QMP socket open (qemu/run.sh does 127.0.0.1:4445).
"""
import argparse
import io
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time

PANEL_W, PANEL_H = 1125, 2436
ABS_RANGE = 0x7FFF
HERE = os.path.dirname(os.path.abspath(__file__))


def qmp_events(events, port):
    """Send events over QMP, the interface that delivers this guest its coordinates."""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=3)
    except OSError:
        return False
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

    time.sleep(0.05)
    call({"execute": "qmp_capabilities"})
    result = call({"execute": "input-send-event", "arguments": {"events": events}})
    s.close()
    return result is not None


def abs_events(x, y):
    return [
        {"type": "abs", "data": {"axis": "x", "value": max(0, min(ABS_RANGE, int(x / PANEL_W * ABS_RANGE)))}},
        {"type": "abs", "data": {"axis": "y", "value": max(0, min(ABS_RANGE, int(y / PANEL_H * ABS_RANGE)))}},
    ]


def button(down):
    return [{"type": "btn", "data": {"down": down, "button": "left"}}]


class Screen:
    """The guest's screen, one screendump at a time, cached for a fraction of a second."""

    def __init__(self, fps):
        self.interval = 1.0 / max(0.2, fps)
        self.lock = threading.Lock()
        self.last = 0.0
        self.raw = None

    def get(self):
        with self.lock:
            now = time.time()
            if self.raw is not None and now - self.last < self.interval:
                return self.raw
            self.raw = self._grab()
            self.last = now
            return self.raw

    def _grab(self):
        path = "/tmp/vncbridge-screen.png"
        try:
            subprocess.run(
                ["bash", os.path.join(HERE, "screendump.sh"), path],
                timeout=20, check=False,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            )
            from PIL import Image
            import numpy as np
            with Image.open(path) as image:
                image = image.convert("RGB")
                if image.size != (PANEL_W, PANEL_H):
                    image = image.resize((PANEL_W, PANEL_H))
                rgb = np.asarray(image, dtype=np.uint8)
            # the wire wants BGRX: the channels reversed, and a fourth byte the client ignores
            bgrx = np.empty((PANEL_H, PANEL_W, 4), dtype=np.uint8)
            bgrx[..., 0] = rgb[..., 2]
            bgrx[..., 1] = rgb[..., 1]
            bgrx[..., 2] = rgb[..., 0]
            bgrx[..., 3] = 0xFF
            return bgrx.tobytes()
        except Exception as problem:                     # noqa: BLE001 - a black frame is the answer
            print(f"vncbridge: no screendump ({problem})", file=sys.stderr)
            return b"\x00\x00\x00" * (PANEL_W * PANEL_H)


def recv_exactly(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def serve_client(conn, screen, qmp_port):
    conn.settimeout(None)
    conn.sendall(b"RFB 003.008\n")
    recv_exactly(conn, 12)
    conn.sendall(bytes([1, 1]))                          # one security type: None
    if recv_exactly(conn, 1) != b"\x01":
        return
    recv_exactly(conn, 1)                                # ClientInit, shared
    name = b"iosvm"
    pixfmt = struct.pack(">BBBBHHHBBBxxx", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
    conn.sendall(struct.pack(">HH", PANEL_W, PANEL_H) + pixfmt + struct.pack(">I", len(name)) + name)

    pressed = False
    while True:
        kind = recv_exactly(conn, 1)[0]
        if kind == 0:                                    # SetPixelFormat
            recv_exactly(conn, 3 + 16)
        elif kind == 2:                                  # SetEncodings
            recv_exactly(conn, 1)
            count = struct.unpack(">H", recv_exactly(conn, 2))[0]
            recv_exactly(conn, 4 * count)
        elif kind == 3:                                  # FramebufferUpdateRequest
            recv_exactly(conn, 9)
            raw = screen.get()
            # FramebufferUpdate: type, pad, one rectangle, then the rectangle's x, y, width, height
            conn.sendall(struct.pack(">BBH", 0, 0, 1) + struct.pack(">HHHH", 0, 0, PANEL_W, PANEL_H))
            conn.sendall(raw)
        elif kind == 4:                                  # KeyEvent
            recv_exactly(conn, 7)
        elif kind == 5:                                  # PointerEvent
            mask, x, y = struct.unpack(">BHH", recv_exactly(conn, 5))
            down = bool(mask & 1)
            if down and not pressed:
                qmp_events(abs_events(x, y) + button(True), qmp_port)
                pressed = True
            elif down and pressed:
                qmp_events(abs_events(x, y), qmp_port)
            elif not down and pressed:
                qmp_events(button(False), qmp_port)
                pressed = False
        elif kind == 6:                                  # ClientCutText
            length = struct.unpack(">I", recv_exactly(conn, 7)[3:7])[0]
            recv_exactly(conn, length)
        else:
            return


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=5910)
    parser.add_argument("--qmp", type=int, default=4445)
    parser.add_argument("--fps", type=float, default=1.0)
    args = parser.parse_args(argv)

    screen = Screen(args.fps)
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.port))
    server.listen(4)
    print(f"vncbridge: 127.0.0.1:{args.port} -> QMP {args.qmp}, {PANEL_W}x{PANEL_H}", flush=True)
    while True:
        conn, _ = server.accept()
        print("vncbridge: a client connected", flush=True)
        threading.Thread(target=serve_client, args=(conn, screen, args.qmp), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

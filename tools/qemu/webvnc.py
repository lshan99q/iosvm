#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""webvnc.py — watch and operate the machine from a browser.

QEMU's own VNC can show this guest but not point at it (its pointer arrives at the guest as
relative motion, which the touch driver cannot use), so this serves the screen and the input
itself: frames come from the monitor's `screendump`, and clicks become touches over QMP --
the interface that reaches the guest.

    tools/qemu/webvnc.py --port 5980 --monitor 4454 --qmp 4455

then open http://127.0.0.1:5980/ . Click to tap, drag to swipe; the page keeps the panel's
aspect and scales it to whatever the window is.

No dependencies beyond Pillow: the WebSocket framing and the page are here, because this
machine may have no way to fetch a viewer from the internet.
"""
import argparse
import base64
import hashlib
import io
import json
import os
import socket
import struct
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PANEL_W, PANEL_H = 1125, 2436
ABS_RANGE = 0x7FFF
HERE = os.path.dirname(os.path.abspath(__file__))

PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>iosvm</title>
<style>
  html,body{margin:0;height:100%;background:#111;color:#ccc;font:13px system-ui,sans-serif;overflow:hidden}
  #wrap{position:fixed;inset:0;display:flex;align-items:center;justify-content:center}
  #screen{max-width:100%;max-height:100%;touch-action:none;cursor:pointer;image-rendering:auto}
  #bar{position:fixed;left:8px;bottom:6px;font-size:12px;opacity:.6}
  #status{position:fixed;right:8px;top:6px;font-size:12px;opacity:.6}
</style></head>
<body>
<div id="wrap"><img id="screen" alt=""></div>
<div id="bar">click = tap &middot; drag = swipe</div>
<div id="status">connecting…</div>
<script>
const img = document.getElementById('screen');
const status = document.getElementById('status');
let ws, down = false, moved = false, startX = 0, startY = 0, lastX = 0, lastY = 0;

function toPanel(ev) {
  const r = img.getBoundingClientRect();
  const x = (ev.clientX - r.left) / r.width * %W%;
  const y = (ev.clientY - r.top) / r.height * %H%;
  return [Math.max(0, Math.min(%W%, x)), Math.max(0, Math.min(%H%, y))];
}
function send(o) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(o)); }

img.addEventListener('pointerdown', ev => {
  ev.preventDefault(); img.setPointerCapture(ev.pointerId);
  [startX, startY] = toPanel(ev); lastX = startX; lastY = startY;
  down = true; moved = false;
  send({t: 'down', x: startX, y: startY});
});
img.addEventListener('pointermove', ev => {
  if (!down) return;
  const [x, y] = toPanel(ev);
  if (Math.abs(x - startX) > 4 || Math.abs(y - startY) > 4) moved = true;
  if (moved) { send({t: 'move', x, y}); lastX = x; lastY = y; }
});
function endPointer(ev) {
  if (!down) return;
  down = false;
  const [x, y] = toPanel(ev);
  send({t: 'up', x, y, moved});
}
img.addEventListener('pointerup', endPointer);
img.addEventListener('pointercancel', endPointer);

document.addEventListener('keydown', ev => {
  if (ev.metaKey || ev.ctrlKey || ev.altKey) return;
  if (ev.key === 'Backspace') { send({t: 'key', k: '\b'}); ev.preventDefault(); return; }
  if (ev.key === ' ') { send({t: 'key', k: ' '}); ev.preventDefault(); return; }
  if (ev.key.length === 1) { send({t: 'key', k: ev.key}); ev.preventDefault(); }
});

function connect() {
  ws = new WebSocket((location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws');
  ws.binaryType = 'blob';
  ws.onopen = () => status.textContent = 'connected';
  ws.onclose = () => { status.textContent = 'disconnected, retrying…'; setTimeout(connect, 1000); };
  ws.onmessage = ev => {
    if (typeof ev.data === 'string') { status.textContent = ev.data; return; }
    const url = URL.createObjectURL(ev.data);
    img.onload = () => URL.revokeObjectURL(url);
    img.src = url;
  };
}
connect();
</script></body></html>
"""


def ws_accept(key):
    return base64.b64encode(hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()).decode()


def ws_frame(payload, opcode=2):
    """A server frame: no mask, one of the three length forms."""
    header = bytearray([0x80 | opcode])
    length = len(payload)
    if length < 126:
        header.append(length)
    elif length < (1 << 16):
        header.append(126)
        header += struct.pack(">H", length)
    else:
        header.append(127)
        header += struct.pack(">Q", length)
    return bytes(header) + payload


def ws_read(conn, want):
    data = b""
    while len(data) < want:
        chunk = conn.recv(want - len(data))
        if not chunk:
            raise EOFError
        data += chunk
    return data


def ws_recv(conn):
    first = ws_read(conn, 2)
    opcode = first[0] & 0x0F
    masked = first[1] & 0x80
    length = first[1] & 0x7F
    if length == 126:
        length = struct.unpack(">H", ws_read(conn, 2))[0]
    elif length == 127:
        length = struct.unpack(">Q", ws_read(conn, 8))[0]
    mask = ws_read(conn, 4) if masked else b""
    payload = ws_read(conn, length) if length else b""
    if masked:
        payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    return opcode, payload


def qmp(events, port):
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

    time.sleep(0.02)
    call({"execute": "qmp_capabilities"})
    call({"execute": "input-send-event", "arguments": {"events": events}})
    s.close()
    return True


def abs_xy(x, y):
    return [
        {"type": "abs", "data": {"axis": "x", "value": max(0, min(ABS_RANGE, int(x / PANEL_W * ABS_RANGE)))}},
        {"type": "abs", "data": {"axis": "y", "value": max(0, min(ABS_RANGE, int(y / PANEL_H * ABS_RANGE)))}},
    ]


def btn(down):
    return [{"type": "btn", "data": {"down": down, "button": "left"}}]


# Where each key of the machine's own on-screen keyboard sits, in panel pixels: measured from a
# screendump of it (docs/demo/software-keyboard.png). Typing in the browser becomes taps here, which
# is the only way to enter text: the port has no HID keyboard service, so QMP key events reach the
# guest's driver and stop there (addendum 29a).
KEYBOARD = {
    "rows": [
        (1650, "qwertyuiop", [56, 168, 281, 393, 506, 618, 731, 843, 956, 1068]),
        (1810, "asdfghjkl", [111, 224, 336, 449, 563, 675, 788, 900, 1013]),
    ],
    "shift": (72, 1960),
    "delete": (1053, 1960),
    "space": (420, 2100),
    "zrow": (1960, "zxcvbnm", [224, 336, 449, 563, 675, 788, 900]),
}
KEY_POS = {}
for _y, _letters, _xs in KEYBOARD["rows"]:
    for _letter, _x in zip(_letters, _xs):
        KEY_POS[_letter] = (_x, _y)
_y, _letters, _xs = KEYBOARD["zrow"]
for _letter, _x in zip(_letters, _xs):
    KEY_POS[_letter] = (_x, _y)


class Screen:
    """One frame at a time, through QMP -- not the human monitor, which allows a single client and
    would then be unavailable to everything else (screendump.sh, tap.py's neighbours)."""

    def __init__(self, qmp_port, quality=72):
        self.qmp_port = qmp_port
        self.quality = quality
        self.lock = threading.RLock()
        self.path = f"/tmp/webvnc-{qmp_port}.png"
        self.conn = None
        self.file = None

    def _connect(self):
        try:
            self.conn = socket.create_connection(("127.0.0.1", self.qmp_port), timeout=8)
            self.file = self.conn.makefile("rwb", buffering=0)
            time.sleep(0.15)          # QMP greets first; a write that overtakes it reads as a timeout
            self._call({"execute": "qmp_capabilities"})
            return True
        except OSError:
            self.conn, self.file = None, None
            return False

    def _call(self, obj):
        self.file.write((json.dumps(obj) + "\n").encode())
        while True:
            line = self.file.readline()
            if not line:
                raise OSError("qmp closed")
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "return" in message or "error" in message:
                return message

    def grab(self):
        with self.lock:
            try:
                os.unlink(self.path)
            except OSError:
                pass
            for _ in range(2):
                if self.conn is None and not self._connect():
                    return None
                try:
                    self._call({"execute": "screendump",
                                "arguments": {"filename": self.path, "format": "png"}})
                except OSError:
                    try:
                        self.conn.close()
                    except OSError:
                        pass
                    self.conn = None
                    continue
                try:
                    from PIL import Image
                    with Image.open(self.path) as image:
                        image = image.convert("RGB")
                        if image.size != (PANEL_W, PANEL_H):
                            image = image.crop((0, 0, min(image.width, PANEL_W),
                                                min(image.height, PANEL_H)))
                            image = image.resize((PANEL_W, PANEL_H))
                        out = io.BytesIO()
                        image.save(out, "JPEG", quality=self.quality)
                    try:
                        os.unlink(self.path)
                    except OSError:
                        pass
                    return out.getvalue()
                except Exception as problem:              # noqa: BLE001 - a frame we could not make
                    print(f"webvnc: no frame ({problem})", flush=True)
                    return None
            return None


class Hub:
    """The frame a client should see, and who is listening."""

    def __init__(self, screen, fps):
        self.screen = screen
        self.interval = 1.0 / max(0.2, fps)
        self.clients = []
        self.clients_lock = threading.Lock()
        self.latest = None
        self.frames = 0
        threading.Thread(target=self._loop, daemon=True).start()

    def add(self, conn):
        with self.clients_lock:
            self.clients.append(conn)
            latest = self.latest
        if latest:
            try:
                conn.sendall(ws_frame(latest))
            except OSError:
                self.remove(conn)

    def remove(self, conn):
        with self.clients_lock:
            if conn in self.clients:
                self.clients.remove(conn)

    def _loop(self):
        while True:
            started = time.time()
            frame = self.screen.grab()
            if frame:
                self.latest = frame
                self.frames += 1
                with self.clients_lock:
                    for conn in list(self.clients):
                        try:
                            conn.sendall(ws_frame(frame))
                        except OSError:
                            self.remove(conn)
            rest = self.interval - (time.time() - started)
            if rest > 0:
                time.sleep(rest)


def tap_at(x, y, port):
    qmp(abs_xy(x, y) + btn(True), port)
    qmp(btn(False), port)


def type_key(char, port):
    """One character, as the taps that would enter it on the machine's own keyboard."""
    if char == "\b":
        tap_at(*KEYBOARD["delete"], port)
        return
    if char == " ":
        tap_at(*KEYBOARD["space"], port)
        return
    if len(char) != 1:
        return
    lower = char.lower()
    position = KEY_POS.get(lower)
    if position is None:
        return                      # not on this layout (digits and punctuation need the 123 page)
    if char.isupper():
        tap_at(*KEYBOARD["shift"], port)
        tap_at(*position, port)
        tap_at(*KEYBOARD["shift"], port)
        return
    tap_at(*position, port)


def make_handler(hub, qmp_port, page):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):                       # quiet
            pass

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                body = page.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if self.path == "/ws" and self.headers.get("Upgrade", "").lower() == "websocket":
                key = self.headers.get("Sec-WebSocket-Key", "")
                self.send_response(101, "Switching Protocols")
                self.send_header("Upgrade", "websocket")
                self.send_header("Connection", "Upgrade")
                self.send_header("Sec-WebSocket-Accept", ws_accept(key))
                self.end_headers()
                self.wfile.flush()
                try:
                    self._session()
                except (EOFError, OSError):
                    pass
                finally:
                    hub.remove(self.connection)
                return
            self.send_response(404)
            self.send_header("Content-Length", "0")
            self.end_headers()

        def _session(self):
            hub.add(self.connection)
            pressed = False
            while True:
                opcode, payload = ws_recv(self.connection)
                if opcode == 8:
                    return
                if opcode != 1:
                    continue
                try:
                    message = json.loads(payload)
                except json.JSONDecodeError:
                    continue
                kind = message.get("t")
                if kind == "down":
                    qmp(abs_xy(message["x"], message["y"]) + btn(True), qmp_port)
                    pressed = True
                elif kind == "move":
                    qmp(abs_xy(message["x"], message["y"]), qmp_port)
                elif kind == "up":
                    qmp(btn(False), qmp_port)
                    pressed = False
                elif kind == "key":
                    type_key(message.get("k", ""), qmp_port)

    return Handler


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=5980)
    parser.add_argument("--monitor", type=int, default=4444)
    parser.add_argument("--qmp", type=int, default=4445)
    parser.add_argument("--fps", type=float, default=1.0)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args(argv)

    page = PAGE.replace("%W%", str(PANEL_W)).replace("%H%", str(PANEL_H))
    hub = Hub(Screen(args.qmp), args.fps)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(hub, args.qmp, page))
    print(f"webvnc: http://{args.host}:{args.port}/  (monitor {args.monitor}, qmp {args.qmp})", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main(__import__("sys").argv[1:])

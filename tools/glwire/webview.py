#!/usr/bin/env python3
"""Serve the picture host_glwire composites to a browser, and send the browser's clicks back to the
guest.

host_glwire publishes every frame into GLWIRE_SHM (header + BGRA, see publish_picture() in
host_glwire.c); this bridge maps that file, JPEG-encodes the newest frame and streams it as
multipart/x-mixed-replace.  Input goes to QEMU's QMP socket as input-send-event, so it reaches the
guest's virtio-tablet the way the VNC console's pointer does.

    GLWIRE_SHM=/dev/shm/glwire-frame ./tools/glwire/host_glwire        # the replay, publishing
    python3 tools/glwire/webview.py --port 6082 --qmp 127.0.0.1:4455   # this bridge

Nothing here is on the frame path of the guest: it only reads a file the replay already writes.
"""
import argparse
import collections
import io
import json
import mmap
import os
import socket
import struct
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HEADER = struct.Struct("<QQIIIIQ")
PAGE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "webview.html")


class Picture:
    """The newest frame in the shared file, JPEG-encoded once per change."""

    def __init__(self, path, scale, quality):
        self.path = path
        self.scale = scale
        self.quality = quality
        self.crop = 0
        self.lock = threading.Lock()
        self.jpeg = b""
        self.seq = 0
        self.width = 0
        self.height = 0
        self.frames = 0
        self.times = collections.deque(maxlen=240)
        self.started = time.time()
        self.last_error = ""
        self.map = None
        self.ino = None
        self.size = 0

    def attach(self):
        fd = os.open(self.path, os.O_RDONLY)
        st = os.fstat(fd)
        size = st.st_size
        self.ino = st.st_ino
        self.map = mmap.mmap(fd, size, mmap.MAP_SHARED, mmap.PROT_READ)
        self.size = size
        os.close(fd)

    def snapshot(self):
        """(seq, width, height, pixels) or None when nothing is published yet."""
        try:                       # a restarted publisher makes a new file: follow it
            if self.map is not None and os.stat(self.path).st_ino != getattr(self, "ino", None):
                self.map = None
        except OSError:
            self.map = None
        if self.map is None:
            try:
                self.attach()
            except OSError as e:
                self.last_error = str(e)
                return None
        magic, seq, w, h, nbytes, _pad, _ns = HEADER.unpack_from(self.map, 0)
        if magic != 0x57464C47 or nbytes != w * h * 4:
            return None
        self.last_error = ""
        if HEADER.size + nbytes > self.size:
            return None
        return seq, w, h, bytes(self.map[HEADER.size:HEADER.size + nbytes])

    def encode(self, w, h, pixels):
        from PIL import Image
        image = Image.frombuffer("RGBA", (w, h), pixels, "raw", "BGRA", 0, 1).convert("RGB")
        if self.crop and h > self.crop:       # the guest draws its screen into the top rows of a
            image = image.crop((0, 0, w, self.crop))   # taller surface; the rest is padding
        if self.scale != 1.0:
            image = image.resize((int(image.size[0] * self.scale), int(image.size[1] * self.scale)),
                                 Image.BILINEAR)
        out = io.BytesIO()
        image.save(out, format="JPEG", quality=self.quality)
        return out.getvalue()

    def pump(self):
        while True:
            snap = self.snapshot()
            if snap is None:
                time.sleep(0.1)
                continue
            seq, w, h, pixels = snap
            if seq != self.seq:
                try:
                    jpeg = self.encode(w, h, pixels)
                except Exception as e:            # a torn or half-written frame: try the next one
                    self.last_error = "%s: %s" % (type(e).__name__, e)
                    time.sleep(0.02)
                    continue
                with self.lock:
                    self.jpeg = jpeg
                    self.seq = seq
                    self.width, self.height = w, h
                    self.frames += 1
                    self.times.append(time.time())
            time.sleep(0.008)


class Qmp:
    """Just enough QMP to push pointer and keyboard events into the guest."""

    def __init__(self, address):
        self.address = address
        self.sock = None
        self.f = None
        self.lock = threading.Lock()

    def _connect(self):
        host, _, port = self.address.partition(":")
        self.sock = socket.create_connection((host, int(port)), timeout=3)
        self.f = self.sock.makefile("rwb")
        self.f.readline()                       # greeting
        self._send({"execute": "qmp_capabilities"})

    def _send(self, payload):
        self.f.write((json.dumps(payload) + "\r\n").encode())
        self.f.flush()
        return self.f.readline()

    def event(self, events):
        with self.lock:
            if self.f is None:
                self._connect()
            return self._send({"execute": "input-send-event", "arguments": {"events": events}})

    def tap(self, x, y, hold_ms=60):
        ax = max(0, min(32767, int(x * 32767 / 1000)))
        ay = max(0, min(32767, int(y * 32767 / 1000)))
        self.event([{"type": "abs", "data": {"axis": "x", "value": ax}},
                    {"type": "abs", "data": {"axis": "y", "value": ay}}])
        self.event([{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(hold_ms / 1000.0)
        self.event([{"type": "btn", "data": {"down": False, "button": "left"}}])

    def key(self, qcode):
        with self.lock:
            if self.f is None:
                self._connect()
            return self._send({"execute": "send-key",
                               "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})


class Handler(BaseHTTPRequestHandler):
    picture = None
    qmp = None
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def _page(self, body, ctype, code=200):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            with open(PAGE, "rb") as f:
                return self._page(f.read(), "text/html; charset=utf-8")
        if path in ("/stats", "/status"):
            p = self.picture
            now = time.time()
            recent = sum(1 for t in p.times if now - t <= 3.0)
            height = min(p.height, p.crop) if (p.crop and p.height) else p.height
            body = json.dumps({"seq": p.seq, "width": p.width, "height": height,
                               "frames": p.frames, "seconds": round(now - p.started, 1),
                               "fps_now": round(recent / 3.0, 2),
                               "error": p.last_error}).encode()
            return self._page(body, "application/json")
        if path == "/frame.jpg":
            with self.picture.lock:
                jpeg = self.picture.jpeg
            return self._page(jpeg, "image/jpeg")
        if path == "/stream.mjpg":
            return self.stream()

    def stream(self):
        self.send_response(200)
        self.send_header("Content-Type", "multipart/x-mixed-replace; boundary=frame")
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        last = 0
        try:
            while True:
                with self.picture.lock:
                    seq, jpeg = self.picture.seq, self.picture.jpeg
                if seq != last and jpeg:
                    last = seq
                    self.wfile.write(b"--frame\r\nContent-Type: image/jpeg\r\nContent-Length: " +
                                     str(len(jpeg)).encode() + b"\r\n\r\n" + jpeg + b"\r\n")
                    self.wfile.flush()
                else:
                    time.sleep(0.01)
        except (BrokenPipeError, ConnectionResetError):
            return

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        try:
            request = json.loads(self.rfile.read(length) or b"{}")
        except ValueError:
            return self._page(b'{"ok":false}', "application/json", 400)
        try:
            kind = request.get("type")
            if kind == "tap":
                self.qmp.tap(float(request["x"]), float(request["y"]))
            elif kind == "key":
                self.qmp.key(request["name"])
            else:
                return self._page(b'{"ok":false}', "application/json", 400)
        except Exception as e:
            body = json.dumps({"ok": False, "error": "%s: %s" % (type(e).__name__, e)}).encode()
            return self._page(body, "application/json", 500)
        return self._page(b'{"ok":true}', "application/json")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=6082)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--shm", default=os.environ.get("GLWIRE_SHM", "/dev/shm/glwire-frame"))
    ap.add_argument("--qmp", default="127.0.0.1:4455")
    ap.add_argument("--scale", type=float, default=0.75)
    ap.add_argument("--quality", type=int, default=62)
    ap.add_argument("--crop", type=int, default=1624,
                    help="keep only the top N rows of the frame (the guest's screen); 0 = whole surface")
    args = ap.parse_args()

    Handler.picture = Picture(args.shm, args.scale, args.quality)
    Handler.picture.crop = args.crop
    Handler.qmp = Qmp(args.qmp)
    threading.Thread(target=Handler.picture.pump, daemon=True).start()
    server = ThreadingHTTPServer((args.bind, args.port), Handler)
    print("webview: http://%s:%d/ (shm %s, qmp %s, scale %.2f)" %
          (args.bind, args.port, args.shm, args.qmp, args.scale), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""widget_test.py -- opcode 202 through host_glwire, without a boot.

Feeds map_test's own record stream plus synthetic widget records and checks the picture that comes
back: a full-rect red widget must cover the centre pixel, a rect that starts at 2,2 must not cover
0,0, and a zero rectangle must take the earlier widget away again.  The host must be built
(host_glwire), and this needs map_test; if it is not in this directory, pass --map-test.
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
WIDTH = HEIGHT = 4
WIDGET_TEXTURE = 0xFFFF1001
RED = bytes([255, 0, 0, 255])               # RGBA red (what the guest uploads)
RED_BGRA = bytes([0, 0, 255, 255])          # the same pixel after glReadPixels
BASELINE = bytes([0, 51, 51, 255])          # e2e_test.py's centre without a widget


def rec(opcode, payload):
    return struct.pack("<III", opcode, len(payload), 0) + payload


def widget(pixels, x, y, w, h):
    payload = struct.pack("<IIiiiIIiiiiii", WIDGET_TEXTURE, 0x0DE1, 0x1908, WIDTH, HEIGHT,
                          0x1908, 0x1401, WIDTH * 4, 4, x, y, w, h)
    payload += struct.pack("<BI", 0, len(pixels)) + pixels
    return rec(202, payload)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def run(map_test, extra_records, extra_env=None):
    with tempfile.NamedTemporaryFile(suffix=".records") as records:
        subprocess.run([map_test, "--dump", records.name], check=True, stdout=subprocess.DEVNULL)
        stream = b"".join(extra_records) + open(records.name, "rb").read()
    port = free_port()
    env = dict(os.environ, GLWIRE_WIDGET="1")
    env.update(extra_env or {})
    host = subprocess.Popen([os.path.join(HERE, "host_glwire"), str(port)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, env=env)
    output = []
    try:
        deadline = time.time() + 15
        while time.time() < deadline:
            line = host.stdout.readline()
            if not line:
                break
            output.append(line)
            if "listening" in line:
                break
        else:
            raise RuntimeError("host_glwire did not listen")
        sock = socket.create_connection(("127.0.0.1", port), timeout=20)
        request = "R %d %d %d 64 0 0 0 0 2\n" % (WIDTH, HEIGHT, len(stream))
        sock.sendall(request.encode() + stream)
        reply = b""
        while not reply.endswith(b"\n"):
            chunk = sock.recv(1)
            if not chunk:
                break
            reply += chunk
        picture = b""
        while len(picture) < WIDTH * HEIGHT * 4:
            chunk = sock.recv(WIDTH * HEIGHT * 4 - len(picture))
            if not chunk:
                break
            picture += chunk
        sock.close()
        if reply != b"R 64\n" or len(picture) != WIDTH * HEIGHT * 4:
            raise RuntimeError("reply %r, picture %d bytes" % (reply, len(picture)))
        return picture, "".join(output)
    finally:
        host.terminate()
        try:
            output.extend(host.stdout.readlines())
        except Exception:
            pass
        try:
            host.wait(timeout=5)
        except subprocess.TimeoutExpired:
            host.kill()


def pixel(picture, x, y):
    return picture[(y * WIDTH + x) * 4:(y * WIDTH + x) * 4 + 4]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--map-test", default=os.path.join(HERE, "map_test"))
    args = parser.parse_args()
    if not os.path.exists(args.map_test):
        raise SystemExit("widget_test: %s not built; build it or pass --map-test" % args.map_test)

    failures = 0
    picture, output = run(args.map_test, [widget(RED * (WIDTH * HEIGHT), 0, 0, WIDTH, HEIGHT)])
    if pixel(picture, 2, 2) == RED_BGRA:
        print("widget_test: full rect PASS")
    else:
        failures += 1
        print("widget_test: full rect FAIL: centre %s" % list(pixel(picture, 2, 2)))
        print(output)

    picture, output = run(args.map_test, [widget(RED * (WIDTH * HEIGHT), 2, 0, 4, 4)])
    if pixel(picture, 3, 2) == RED_BGRA and pixel(picture, 0, 2) != RED_BGRA:
        print("widget_test: clipped rect PASS")
    else:
        failures += 1
        print("widget_test: clipped rect FAIL: 3,2 %s 0,2 %s"
              % (list(pixel(picture, 3, 2)), list(pixel(picture, 0, 2))))
        print(output)

    picture, output = run(args.map_test, [widget(RED * (WIDTH * HEIGHT), 0, 0, WIDTH, HEIGHT),
                                          widget(bytes(64), 0, 0, 0, 0)])
    if pixel(picture, 2, 2) == BASELINE:
        print("widget_test: zero rect removes PASS")
    else:
        failures += 1
        print("widget_test: zero rect removes FAIL: centre %s" % list(pixel(picture, 2, 2)))
        print(output)

    picture, output = run(args.map_test,
                          [widget(bytes([255, 255, 255, 255]) * (WIDTH * HEIGHT), 0, 0, WIDTH, HEIGHT)],
                          {"GLWIRE_WIDGET_FALLBACK": "1"})
    centre = pixel(picture, 2, 2)
    if centre != bytes([255, 255, 255, 255]) and centre != BASELINE:
        print("widget_test: white fallback PASS")
    else:
        failures += 1
        print("widget_test: white fallback FAIL: centre %s" % list(centre))
        print(output)

    picture, output = run(args.map_test,
                          [widget(RED * (WIDTH * HEIGHT), 0, 0, WIDTH, HEIGHT)],
                          {"GLWIRE_WIDGET_FALLBACK": "1"})
    if pixel(picture, 2, 2) == RED_BGRA:
        print("widget_test: fallback keeps real widget PASS")
    else:
        failures += 1
        print("widget_test: fallback keeps real widget FAIL: centre %s" % list(pixel(picture, 2, 2)))
        print(output)

    raise SystemExit(1 if failures else 0)


if __name__ == "__main__":
    main()

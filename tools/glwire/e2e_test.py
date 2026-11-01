#!/usr/bin/env python3
"""e2e_test.py -- host_glwire over a socket, with the mapping test's own call stream.

This is the part map_test cannot check: the request line, a per-connection GL context, the reply
carrying the picture, and the name maps surviving the trip. The stream is map_stream's (program 77,
textures 101/102, framebuffer 303, uniform locations 5/6, all different on the host).

    ./e2e_test.py [--port 9123]

Needs ./host_glwire and ./map_test built first (`make`).
"""
import argparse
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9123)
    args = ap.parse_args()
    records_path = "/tmp/glwire-e2e.records"

    subprocess.run([os.path.join(HERE, "map_test"), "--dump", records_path], check=True,
                   stdout=subprocess.DEVNULL)
    records = open(records_path, "rb").read()

    host = subprocess.Popen([os.path.join(HERE, "host_glwire"), str(args.port)],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        deadline = time.time() + 15
        while time.time() < deadline:
            line = host.stdout.readline()
            if not line:
                break
            sys.stdout.write("  " + line)
            if "listening" in line:
                break
        else:
            raise SystemExit("e2e_test: host_glwire did not listen")

        s = socket.create_connection(("127.0.0.1", args.port), timeout=20)
        request = "R 4 4 %d 64\n" % len(records)
        s.sendall(request.encode() + records)
        reply = b""
        while not reply.endswith(b"\n"):
            chunk = s.recv(1)
            if not chunk:
                break
            reply += chunk
        if not reply.startswith(b"R 64"):
            s.close()
            raise SystemExit("e2e_test: the host did not draw the frame: %r" % reply)
        picture = b""
        while len(picture) < 64:
            chunk = s.recv(64 - len(picture))
            if not chunk:
                break
            picture += chunk
        s.close()
        print("e2e_test: reply %r, %d picture bytes" % (reply.decode().strip(), len(picture)))
    finally:
        host.terminate()
        try:
            host.wait(timeout=5)
        except subprocess.TimeoutExpired:
            host.kill()
        for line in host.stdout:
            sys.stdout.write("  " + line)

    at = (2 * 4 + 2) * 4
    b, g, r, a = picture[at:at + 4]
    print("e2e_test: centre pixel B=%d G=%d R=%d A=%d" % (b, g, r, a))
    if not (49 <= r <= 53 and 49 <= g <= 53 and b <= 2 and a >= 253):
        raise SystemExit("e2e_test: FAIL: the host drew the wrong picture")
    print("e2e_test: PASS")


main()

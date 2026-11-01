#!/usr/bin/env python3
"""e2e_test.py -- host_glwire over a socket, with the mapping test's own call stream.

This is the part map_test cannot check: the request line, the search for the guest's picture by its
magic word in the RAM file, a per-connection GL context, the reply, and the pixels landing where the
guest will read them. The stream is map_stream's (program 77, textures 101/102, framebuffer 303,
uniform locations 5/6, all different on the host), so it also proves that the name maps survive the
trip.

    ./e2e_test.py [--port 9123]

Needs ./host_glwire and ./map_test built first.
"""
import argparse
import mmap
import os
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
RECORDS_MAGIC = 0x1111222233334444
FRAME_MAGIC = 0x5555666677778888


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9123)
    args = ap.parse_args()
    ram_path = "/tmp/glwire-e2e.ram"
    records_path = "/tmp/glwire-e2e.records"

    subprocess.run([os.path.join(HERE, "map_test"), "--dump", records_path], check=True,
                   stdout=subprocess.DEVNULL)
    records = open(records_path, "rb").read()

    with open(ram_path, "wb") as f:
        f.truncate(64 << 20)
    with open(ram_path, "r+b") as f:
        ram = mmap.mmap(f.fileno(), 0)
        ram[(1 << 20):(1 << 20) + 8] = struct.pack("<Q", RECORDS_MAGIC)
        ram[(32 << 20):(32 << 20) + 8] = struct.pack("<Q", FRAME_MAGIC)
        ram.flush()

    host = subprocess.Popen([os.path.join(HERE, "host_glwire"), ram_path, str(args.port)],
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
        request = "R %d %d 4 4 %d 64\n" % (RECORDS_MAGIC, FRAME_MAGIC, len(records))
        s.sendall(request.encode() + records)
        reply = b""
        while not reply.endswith(b"\n"):
            chunk = s.recv(1)
            if not chunk:
                break
            reply += chunk
        s.close()
        print("e2e_test: reply %r" % reply.decode(errors="replace").strip())
        if not reply.startswith(b"R done"):
            raise SystemExit("e2e_test: the host did not draw the frame")
    finally:
        host.terminate()
        try:
            host.wait(timeout=5)
        except subprocess.TimeoutExpired:
            host.kill()
        for line in host.stdout:
            sys.stdout.write("  " + line)

    with open(ram_path, "rb") as f:
        ram = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
        at = (32 << 20) + 8 + (2 * 4 + 2) * 4
        centre = ram[at:at + 4]
        ram.close()
    b, g, r, a = centre
    print("e2e_test: centre pixel B=%d G=%d R=%d A=%d" % (b, g, r, a))
    if not (49 <= r <= 53 and 49 <= g <= 53 and b <= 2 and a >= 253):
        raise SystemExit("e2e_test: FAIL: the host drew the wrong picture")
    print("e2e_test: PASS")


main()

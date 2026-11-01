#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""qmpmux.py — let more than one tool talk to a machine's QMP socket.

QEMU's socket chardev serves a single client: the first connection gets the greeting and every
later one connects and then waits forever. That is fine until two things want the machine at once
-- the web viewer and `qemu/tap.py`, say -- so this holds the one real connection and serves many:

    tools/qemu/qmpmux.py --qmp 4455 --port 4465

Commands are request/response on QMP, so each client's command is forwarded under a lock and its
answer returned to that client. Events QEMU sends on its own are dropped (there is one connection
and no way to know which client wants them).
"""
import argparse
import json
import socket
import threading

GREETING = {"QMP": {"version": {"qemu": {"micro": 0, "minor": 0, "major": 0},
                                "package": "qmpmux"}, "capabilities": ["oob"]}}
NOT_NEGOTIATED = {"error": {"class": "CommandNotFound",
                            "desc": "QMP is not in command mode yet: send qmp_capabilities"}}


class Upstream:
    """The one connection to the machine, and the lock that serialises commands on it."""

    def __init__(self, host, port):
        self.lock = threading.Lock()
        self.conn = None
        self.file = None
        self.host = host
        self.port = port
        self.connect()

    def connect(self):
        self.conn = socket.create_connection((self.host, self.port), timeout=15)
        self.file = self.conn.makefile("rwb", buffering=0)
        self.greeting = self._read_one()          # the real {"QMP": ...}
        self.call({"execute": "qmp_capabilities"})

    def _read_one(self):
        while True:
            line = self.file.readline()
            if not line:
                raise OSError("the machine's QMP closed")
            try:
                message = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "event" in message:
                continue                           # asynchronous: nobody here asked for it
            return message

    def call(self, command):
        with self.lock:
            try:
                self.file.write((json.dumps(command) + "\n").encode())
                return self._read_one()
            except OSError:
                self.connect()                     # one reconnect, then the same command
                self.file.write((json.dumps(command) + "\n").encode())
                return self._read_one()


def serve_client(conn, upstream):
    conn.settimeout(None)
    conn.sendall((json.dumps(upstream.greeting) + "\n").encode())
    reader = conn.makefile("rb")
    negotiated = False
    try:
        while True:
            line = reader.readline()
            if not line:
                return
            try:
                command = json.loads(line)
            except json.JSONDecodeError:
                conn.sendall((json.dumps({"error": {"class": "GenericError",
                                                    "desc": "not JSON"}}) + "\n").encode())
                continue
            name = command.get("execute")
            if name == "qmp_capabilities":
                negotiated = True
                conn.sendall(b'{"return": {}}\n')
                continue
            if not negotiated:
                conn.sendall((json.dumps(NOT_NEGOTIATED) + "\n").encode())
                continue
            conn.sendall((json.dumps(upstream.call(command)) + "\n").encode())
    except (EOFError, OSError):
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--qmp", type=int, default=4445, help="the machine's QMP port")
    parser.add_argument("--port", type=int, default=4465, help="what tools should connect to")
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args(argv)

    upstream = Upstream(args.host, args.qmp)
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind((args.host, args.port))
    server.listen(16)
    print(f"qmpmux: 127.0.0.1:{args.port} -> QMP {args.qmp}", flush=True)
    while True:
        conn, _ = server.accept()
        threading.Thread(target=serve_client, args=(conn, upstream), daemon=True).start()


if __name__ == "__main__":
    import sys
    main(sys.argv[1:])

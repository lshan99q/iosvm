#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Watch a published frame file and report the frame rate over time.

    tools/metalwire/fps-watch.py SHM_FILE [--seconds N] [--out CSV]

The file is glwire's format: magic 0x57464C47, then seq (u64), width, height, bytes, pad, ns,
then the BGRA picture. Both host_glwire (the Replay baseline) and metalwire_host (the translation
path) publish exactly this, which is why one watcher can measure both and why the same webview.py
serves both.

Output: one row per second -- t_rel, seq, frames_in_window, fps -- and a summary on stderr.
"""
import argparse
import mmap
import os
import struct
import sys
import time

HEADER = struct.Struct("<QQIIIIQ")
MAGIC = 0x57464C47


def open_when_ready(path, timeout=600.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            fd = os.open(path, os.O_RDONLY)
            st = os.fstat(fd)
            if st.st_size >= HEADER.size:
                mapping = mmap.mmap(fd, st.st_size, mmap.MAP_SHARED, mmap.PROT_READ)
                os.close(fd)
                return mapping
            os.close(fd)
        except OSError:
            pass
        time.sleep(0.2)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shm")
    ap.add_argument("--seconds", type=float, default=0.0, help="0: until SIGINT / idle stop")
    ap.add_argument("--out", default="")
    ap.add_argument("--idle-stop", type=float, default=45.0,
                    help="stop after this long with no new frame (0: never)")
    args = ap.parse_args()

    mapping = open_when_ready(args.shm)
    if mapping is None:
        print("fps-watch: no frame file at %s" % args.shm, file=sys.stderr)
        return 1

    out = open(args.out, "w") if args.out else None
    if out:
        out.write("t_rel,seq,frames,fps\n")
    start = time.monotonic()
    window_start = start
    last_seq = None
    window_frames = 0
    last_frame_at = start
    rows = 0
    try:
        while True:
            magic, seq, w, h, nbytes, _pad, _ns = HEADER.unpack_from(mapping, 0)
            now = time.monotonic()
            if magic == MAGIC:
                if last_seq is None:
                    last_seq = seq
                elif seq > last_seq:
                    window_frames += seq - last_seq
                    last_seq = seq
            if now - window_start >= 1.0:
                elapsed = now - window_start
                fps = window_frames / elapsed
                if out:
                    out.write("%.3f,%s,%d,%.2f\n" % (now - start, last_seq, window_frames, fps))
                    out.flush()
                rows += 1
                window_start = now
                window_frames = 0
            if args.seconds and now - start >= args.seconds:
                break
            if args.idle_stop and now - last_frame_at > args.idle_stop:
                print("fps-watch: no new frame for %.0fs, stopping" % args.idle_stop, file=sys.stderr)
                break
            time.sleep(0.02)
    except KeyboardInterrupt:
        pass
    if last_seq is not None:
        print("fps-watch: %d windows, last seq %d, frame %dx%d %d bytes"
              % (rows, last_seq, w, h, nbytes), file=sys.stderr)
    if out:
        out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarise metalwire measurement runs into one table.

    tools/metalwire/summarize.py RUN_DIR [RUN_DIR ...] [--json out.json]

A run directory is what tools/metalwire/measure-translation.sh writes (host.csv, host.log,
serial.log). Two sides are reported per run:

  host   the frame's arrival-to-publication cost, from the translator's CSV; recv is the guest's
         record+send time as the host saw it (frame begin -> frame end), the rest is the host's.
  guest  metal_frame's own numbers, parsed from the serial console: fill (the CPU picture), encode
         (record -> commit), commit (wire send), wait (ack round trip).

Numbers are microseconds (mean / median) and fps as the translator reported it.
"""
import argparse
import csv
import json
import os
import re
import statistics

FRAME_RE = re.compile(r"metal_frame: frame=(\d+) fill_us=(\d+) encode_us=(\d+) commit_us=(\d+) "
                      r"wait_us=(\d+) total_us=(\d+) status=(\d+)")
GUEST_SUMMARY_RE = re.compile(r"metalwire: summary: (\d+) frames, (\d+) ok, (\d+) failed .*?"
                              r"(\d+) bytes sent, (\d+) bytes by reference in (\d+) frames")
HOST_SUMMARY_RE = re.compile(r"summary: (\d+) frame\(s\) acknowledged, (\d+) executed with crc_ok, "
                             r"([\d.]+) s, ([\d.]+) fps")


def median(values):
    return statistics.median(values) if values else float("nan")


def read_host(run):
    path = os.path.join(run, "host.csv")
    rows = []
    if os.path.isfile(path):
        with open(path, newline="") as handle:
            for row in csv.DictReader(handle):
                rows.append({key: float(value) for key, value in row.items()})
    host = {}
    for key in ("recv_us", "exec_us", "gpu_us", "readback_us", "publish_us", "total_us"):
        values = [row[key] for row in rows]
        host[key] = {"mean": round(statistics.mean(values), 1) if values else None,
                     "median": round(median(values), 1) if values else None}
    host["frames"] = len(rows)
    log = os.path.join(run, "host.log")
    host["summary"] = None
    if os.path.isfile(log):
        with open(log, errors="replace") as handle:
            text = handle.read()
        match = HOST_SUMMARY_RE.search(text)
        if match:
            host["summary"] = {"frames": int(match.group(1)), "crc_ok": int(match.group(2)),
                               "seconds": float(match.group(3)), "fps": float(match.group(4))}
        # the sampled vs full checksum is a property of the run, keep its log line
        host["crc_line"] = next((line for line in text.splitlines() if "limits:" in line), "")
        host["ram_line"] = next((line for line in text.splitlines() if line.startswith("metalwire: ram:")),
                                "")
        host["gpu_line"] = next((line for line in text.splitlines() if "device 1:" in line), "")
    return host


def read_guest(run):
    path = os.path.join(run, "serial.log")
    frames = []
    text = ""
    if os.path.isfile(path):
        with open(path, errors="replace") as handle:
            text = handle.read()
        for match in FRAME_RE.finditer(text):
            frames.append({"fill_us": int(match.group(2)), "encode_us": int(match.group(3)),
                           "commit_us": int(match.group(4)), "wait_us": int(match.group(5)),
                           "total_us": int(match.group(6)), "status": int(match.group(7))})
    guest = {"frames_parsed": len(frames)}
    for key in ("fill_us", "encode_us", "commit_us", "wait_us", "total_us"):
        values = [frame[key] for frame in frames]
        guest[key] = {"mean": round(statistics.mean(values), 1) if values else None,
                      "median": round(median(values), 1) if values else None}
    match = GUEST_SUMMARY_RE.search(text)
    if match:
        guest["summary"] = {"frames": int(match.group(1)), "ok": int(match.group(2)),
                            "failed": int(match.group(3)), "bytes_sent": int(match.group(4)),
                            "bytes_by_ref": int(match.group(5)), "phys_frames": int(match.group(6))}
    guest["crc_line"] = next((line for line in text.splitlines() if line.startswith("metalwire: crc:")),
                             "")
    return guest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--json", default="")
    args = ap.parse_args()
    report = {}
    for run in args.runs:
        report[os.path.basename(run.rstrip("/"))] = {"host": read_host(run), "guest": read_guest(run)}
    if args.json:
        with open(args.json, "w") as handle:
            json.dump(report, handle, indent=2)
    header = ("run | fps | host total (mean/median us) | recv | gpu | readback | publish | "
              "guest fill | encode | commit | wait")
    print(header)
    print("|".join(["---"] * 12))
    for name, data in report.items():
        host = data["host"]
        guest = data["guest"]
        fps = host["summary"]["fps"] if host["summary"] else "?"
        cells = []
        for side, keys in ((host, ("total_us", "recv_us", "gpu_us", "readback_us", "publish_us")),
                           (guest, ("fill_us", "encode_us", "commit_us", "wait_us"))):
            for key in keys:
                value = side[key]
                cells.append("%s/%s" % (value["mean"], value["median"]))
        print("%s | %s | %s" % (name, fps, " | ".join(str(c) for c in cells)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

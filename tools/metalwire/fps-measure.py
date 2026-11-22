#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Measure the recorded-frame rate of a metalwire boot from its frame dumps.

    tools/metalwire/fps-measure.py --frames /tmp/wp4v6-frames \
        --host-log /tmp/wp4v6-metalwire-host.log \
        --serial mt-work/logs/wp4-v6/serial.log \
        --trigger 2026-11-23T00:56:39+08:00 \
        --out-json mt-work/logs/wp4-v6/fps.json \
        --out-markdown mt-work/logs/wp4-v6/fps.md

`metalwire_host --out DIR` writes one PPM/PNG per presented frame, and the file's mtime is when
the host wrote it (after the read-back and the publication), so the PPM mtimes are the wall-clock
timeline of the frames the host actually produced. This tool turns that timeline into the numbers
the WP-4 report needs: total frames and duration, a steady-state window that drops the boot's
Vulkan-compilation/first-page long frames, the recorded-frame FPS and the p50/p95/max frame
intervals, and the mean executed draws per frame from the host's per-frame log line.

Windows (all wall-clock, in the frames' own time zone):

  desktop-steady  [trigger - --steady-seconds, trigger) -- the desktop before the scene change,
                  i.e. after the boot's warmup and without the user interaction; this is the
                  reported baseline.
  post-trigger    [trigger + --post-offset, last frame] -- after the scene change (Control
                  Center/Spotlight), for comparison.
  last-N          [--tail] frames at the end of the run.

The trigger time is the moment the boot changes scene; pass it explicitly (the boot logs carry
the guest's clock, which is what the operator observed). Without --trigger only the totals and the
last-N window are emitted, and the markdown says so.

FPS is (frames - 1) / (last mtime - first mtime) inside a window, so it is the rate the host
sustained between the first and last recorded frame. p95 uses the nearest-rank method on the
sorted inter-frame gaps: sorted(gaps)[floor(len(gaps) * 0.95) - 1], the same definition the t9
report used, so the numbers can be compared directly.

Outputs: JSON (the full record) and markdown (the table a human reads). Both are written next to
the boot's logs; the recipe block records the repository commit, the command line and the image
hashes so a number can be reproduced later.
"""
import argparse
import datetime
import glob
import hashlib
import json
import os
import re
import statistics
import subprocess
import sys

FRAME_LINE = re.compile(
    r"render frame (\d+): executed (\d+) of (\d+) draw\(s\) on texture (\d+) \((\d+)x(\d+)\)")


def md5_if_exists(path):
    if not path or not os.path.exists(path):
        return None
    digest = hashlib.md5()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def git(repo, *args):
    try:
        return subprocess.run(["git", "-C", repo] + list(args), stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, text=True, timeout=30).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def ppm_geometry(path):
    """(width, height) of a P6 PPM, or None when the header is not one."""
    try:
        with open(path, "rb") as handle:
            magic = handle.read(2)
            if magic != b"P6":
                return None
            tokens = []
            while len(tokens) < 2:
                line = handle.readline()
                if not line:
                    return None
                line = line.split(b"#", 1)[0].strip()
                tokens += line.split()
            return int(tokens[0]), int(tokens[1])
    except OSError:
        return None


def read_frames(directory):
    paths = sorted(glob.glob(os.path.join(directory, "*.ppm")))
    frames = []
    geometries = {}
    for path in paths:
        geometry = ppm_geometry(path)
        key = "%dx%d" % geometry if geometry else "unknown"
        geometries[key] = geometries.get(key, 0) + 1
        if geometry is None:
            continue
        frames.append({"path": path, "index": int(re.search(r"(\d+)\.ppm$", path).group(1)),
                       "mtime": os.path.getmtime(path), "width": geometry[0],
                       "height": geometry[1], "geometry": key})
    frames.sort(key=lambda frame: frame["index"])
    return frames, geometries


def subset(frames, geometry):
    return [frame for frame in frames if frame["geometry"] == geometry]


def read_draws(host_log, size_filter):
    """frame seq -> (executed, total) draw counts, from the host's per-frame log line."""
    draws = {}
    if not host_log or not os.path.exists(host_log):
        return draws
    with open(host_log, "r", errors="replace") as handle:
        for line in handle:
            match = FRAME_LINE.search(line)
            if not match:
                continue
            width, height = int(match.group(5)), int(match.group(6))
            if size_filter is not None and (width, height) != size_filter:
                continue
            draws[int(match.group(1))] = (int(match.group(2)), int(match.group(3)))
    return draws


def iso(epoch):
    return datetime.datetime.fromtimestamp(epoch).astimezone().isoformat(timespec="seconds")


def window(frames, start, end, draws):
    selected = [frame for frame in frames if start <= frame["mtime"] <= end]
    if len(selected) < 2:
        return None
    span = selected[-1]["mtime"] - selected[0]["mtime"]
    gaps = [selected[i + 1]["mtime"] - selected[i]["mtime"] for i in range(len(selected) - 1)]
    executed = [draws[frame["index"]][0] for frame in selected if frame["index"] in draws]
    totals = [draws[frame["index"]][1] for frame in selected if frame["index"] in draws]
    return {
        "frames": len(selected),
        "first_frame": selected[0]["index"],
        "last_frame": selected[-1]["index"],
        "start": iso(selected[0]["mtime"]),
        "end": iso(selected[-1]["mtime"]),
        "duration_s": round(span, 3),
        "fps": round((len(selected) - 1) / span, 3) if span > 0 else None,
        "gap_p50_s": round(statistics.median(gaps), 3),
        "gap_p95_s": round(sorted(gaps)[int(len(gaps) * 0.95) - 1], 3),
        "gap_max_s": round(max(gaps), 3),
        "draws_per_frame": round(statistics.mean(executed), 2) if executed else None,
        "draws_total": sum(executed) if executed else None,
        "draws_lines": len(executed),
        "skipped_draws": sum(t - e for e, t in zip(executed, totals)) if executed else None,
    }


def main(argv):
    parser = argparse.ArgumentParser(description="Measure recorded-frame FPS from metalwire --out "
                                                 "frame dumps.")
    parser.add_argument("--frames", required=True, help="the host's --out directory")
    parser.add_argument("--size", default=None,
                        help="display geometry to measure (default: the most common one)")
    parser.add_argument("--host-log", default=None,
                        help="metalwire_host log (per-frame executed draw counts)")
    parser.add_argument("--serial", default=None,
                        help="the boot's serial.log (only used for provenance/start.epoch)")
    parser.add_argument("--trigger", default=None,
                        help="ISO-8601 time the boot changed scene (Control Center/Spotlight)")
    parser.add_argument("--steady-seconds", type=float, default=300.0,
                        help="desktop-steady window length before the trigger (default 300)")
    parser.add_argument("--post-offset", type=float, default=5.0,
                        help="seconds after the trigger where the post-trigger window starts")
    parser.add_argument("--tail", type=int, default=60,
                        help="last-N window size (default 60)")
    parser.add_argument("--repo", default=None, help="repository to record the commit from")
    parser.add_argument("--mt-work", default="/home/lshan/DSH-desktop/mt-work",
                        help="mt-work checkout for the image hashes")
    parser.add_argument("--boot-commit", default=None,
                        help="the commit the measured boot actually ran (the repository's HEAD "
                             "may have moved since)")
    parser.add_argument("--host-md5", default=None,
                        help="md5 of the metalwire_host binary the measured boot ran")
    parser.add_argument("--out-json", default=None)
    parser.add_argument("--out-markdown", default=None)
    parser.add_argument("--tag", default=None, help="boot tag for the report title")
    args = parser.parse_args(argv)

    frames, geometries = read_frames(args.frames)
    if not frames:
        print("fps-measure: no PPM frames in %s" % args.frames, file=sys.stderr)
        return 1
    size_filter = None
    display_geometry = None
    if args.size:
        display_geometry = args.size.lower()
        width, height = display_geometry.split("x")
        size_filter = (int(width), int(height))
    else:
        known = [item for item in geometries.items() if item[0] != "unknown"]
        if known:
            display_geometry = max(known, key=lambda item: item[1])[0]
    # The recorded-frame timeline counts every frame the host wrote (the t9 report's definition);
    # the display geometry is reported separately because intermediate surfaces are presented too.
    display_frames = subset(frames, display_geometry) if display_geometry else []
    draws_all = read_draws(args.host_log, None)
    draws_display = read_draws(args.host_log, size_filter) if size_filter else draws_all
    trigger_epoch = None
    if args.trigger:
        trigger_epoch = datetime.datetime.fromisoformat(args.trigger).timestamp()

    repo = args.repo or os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    serial_path = None
    if args.serial:
        candidates = [os.path.abspath(args.serial)]
        if not os.path.isabs(args.serial):
            candidates.append(os.path.join(os.path.dirname(os.path.abspath(args.mt_work)),
                                           args.serial))
        for candidate in candidates:
            if os.path.exists(candidate):
                serial_path = candidate
                break
    boot_start = None
    if serial_path:
        start_file = os.path.join(os.path.dirname(serial_path), "start.epoch")
        if os.path.exists(start_file):
            try:
                boot_start = float(open(start_file).read().strip())
            except (OSError, ValueError):
                boot_start = None

    report = {
        "recipe": {
            "tool": "tools/metalwire/fps-measure.py",
            "command": " ".join(sys.argv),
            "tag": args.tag,
            "frames_dir": os.path.abspath(args.frames),
            "size": "%dx%d" % size_filter if size_filter else None,
            "host_log": os.path.abspath(args.host_log) if args.host_log else None,
            "serial": serial_path,
            "trigger": iso(trigger_epoch) if trigger_epoch else None,
            "steady_seconds": args.steady_seconds,
            "post_offset_s": args.post_offset,
            "tail": args.tail,
            "generated": iso(datetime.datetime.now().timestamp()),
            "repo": repo,
            "commit": git(repo, "rev-parse", "HEAD"),
            "commit_subject": git(repo, "log", "-1", "--format=%s"),
            "boot_commit": args.boot_commit,
            "metalwire_host_md5": args.host_md5 or md5_if_exists(
                os.path.join(repo, "host/metalwire/metalwire_host")),
            "system_apfs_md5": md5_if_exists(os.path.join(args.mt_work, "hakios/userland/system.apfs")),
            "root_apfs_md5": md5_if_exists(os.path.join(args.mt_work, "hakios/userland/root.apfs")),
            "libcainject_md5": md5_if_exists(
                os.path.join(repo, "guest/ca-metal-inject/build/libcainject.dylib")),
            "hakiosmetal_md5": md5_if_exists(
                os.path.join(repo, "guest/metal-plugin/build/HakiosMetal.bundle/HakiosMetal")),
        },
        "frames": {
            "ppm_files": sum(geometries.values()),
            "recorded_frames": len(frames),
            "display_geometry": display_geometry,
            "display_frames": len(display_frames),
            "geometry_counts": geometries,
        },
        "timeline": {
            "boot_start": iso(boot_start) if boot_start else None,
            "first_frame": iso(frames[0]["mtime"]),
            "last_frame": iso(frames[-1]["mtime"]),
            "first_frame_after_boot_s": round(frames[0]["mtime"] - boot_start, 3)
            if boot_start else None,
            "total_duration_s": round(frames[-1]["mtime"] - frames[0]["mtime"], 3),
            "total_fps": round((len(frames) - 1) / (frames[-1]["mtime"] - frames[0]["mtime"]), 3),
            "display_first_frame": iso(display_frames[0]["mtime"]) if display_frames else None,
            "display_last_frame": iso(display_frames[-1]["mtime"]) if display_frames else None,
            "display_total_fps": round(
                (len(display_frames) - 1) /
                (display_frames[-1]["mtime"] - display_frames[0]["mtime"]), 3)
            if len(display_frames) > 1 else None,
        },
        "windows": {},
        "display_windows": {},
    }

    def windows_for(window_frames, window_draws, destination):
        if len(window_frames) < 2:
            return
        tail_start = window_frames[-args.tail]["mtime"] if args.tail and \
            len(window_frames) >= args.tail else window_frames[0]["mtime"]
        destination["last_%d" % args.tail] = window(window_frames, tail_start,
                                                    window_frames[-1]["mtime"], window_draws)
        if trigger_epoch is not None:
            destination["desktop_steady"] = window(
                window_frames, trigger_epoch - args.steady_seconds, trigger_epoch, window_draws)
            destination["post_trigger"] = window(
                window_frames, trigger_epoch + args.post_offset, window_frames[-1]["mtime"],
                window_draws)

    windows_for(frames, draws_all, report["windows"])
    windows_for(display_frames, draws_display, report["display_windows"])
    if trigger_epoch is not None:
        report["warmup"] = {
            "rule": "windows start at trigger - %.0f s (steady) / trigger + %.0f s (post), which "
                    "is after the boot's Vulkan-compilation and first-page frames"
                    % (args.steady_seconds, args.post_offset),
            "first_frame_after_boot_s": report["timeline"]["first_frame_after_boot_s"],
            "longest_gap_before_steady_s": round(
                max((frames[i + 1]["mtime"] - frames[i]["mtime"]
                     for i in range(len(frames) - 1)
                     if frames[i + 1]["mtime"] < trigger_epoch - args.steady_seconds),
                    default=0.0), 3),
        }

    text = json.dumps(report, indent=2) + "\n"
    if args.out_json:
        with open(args.out_json, "w") as handle:
            handle.write(text)

    if args.out_markdown:
        recipe = report["recipe"]
        lines = []
        lines.append("# Recorded-frame rate%s" % (" — boot %s" % recipe["tag"]
                                                   if recipe["tag"] else ""))
        lines.append("")
        lines.append("Generated by `tools/metalwire/fps-measure.py` at %s." % recipe["generated"])
        lines.append("")
        lines.append("## Recipe")
        lines.append("")
        lines.append("| item | value |")
        lines.append("|---|---|")
        rows = [
            ("commit", recipe["commit"] or "(not a git worktree)"),
            ("commit subject", recipe["commit_subject"] or ""),
            ("frames dir", recipe["frames_dir"]),
            ("display geometry", recipe["size"] or "(all)"),
            ("host log", recipe["host_log"] or "(none)"),
            ("serial log", recipe["serial"] or "(none)"),
            ("trigger", recipe["trigger"] or "(none: totals and last-N only)"),
            ("tool command", "`%s`" % recipe["command"]),
            ("boot commit", recipe["boot_commit"] or "(same as the repository HEAD above)"),
            ("metalwire_host md5", recipe["metalwire_host_md5"] or "(missing)"),
            ("system.apfs md5", recipe["system_apfs_md5"] or "(missing)"),
            ("root.apfs md5", recipe["root_apfs_md5"] or "(missing)"),
            ("libcainject.dylib md5", recipe["libcainject_md5"] or "(missing)"),
            ("HakiosMetal md5", recipe["hakiosmetal_md5"] or "(missing)"),
        ]
        for key, value in rows:
            lines.append("| %s | %s |" % (key, value))
        lines.append("")
        lines.append("## Totals")
        lines.append("")
        timeline = report["timeline"]
        lines.append("- PPM files: %d; recorded frames measured: %d; display frames (%s): %d" % (
            report["frames"]["ppm_files"], report["frames"]["recorded_frames"],
            report["frames"]["display_geometry"], report["frames"]["display_frames"]))
        lines.append("- first frame: %s; last frame: %s; duration: %.3f s; overall: %.3f fps" % (
            timeline["first_frame"], timeline["last_frame"], timeline["total_duration_s"],
            timeline["total_fps"]))
        if timeline["boot_start"]:
            lines.append("- boot start: %s; first frame after boot: %.3f s" % (
                timeline["boot_start"], timeline["first_frame_after_boot_s"]))
        if "warmup" in report:
            lines.append("- warmup: %s (longest gap before the steady window: %.3f s)" % (
                report["warmup"]["rule"], report["warmup"]["longest_gap_before_steady_s"]))
        lines.append("")
        lines.append("## Windows")
        lines.append("")
        lines.append("| window | frames | duration s | fps | p50 s | p95 s | max s | draws/frame |")
        lines.append("|---|---|---|---|---|---|---|---|")
        for name, entry in report["windows"].items():
            if entry is None:
                lines.append("| %s | (fewer than 2 frames) | | | | | | |" % name)
                continue
            lines.append("| %s | %d | %.1f | %.2f | %.3f | %.3f | %.2f | %s |" % (
                name, entry["frames"], entry["duration_s"], entry["fps"], entry["gap_p50_s"],
                entry["gap_p95_s"], entry["gap_max_s"],
                "%.2f" % entry["draws_per_frame"] if entry["draws_per_frame"] is not None
                else "(no host log)"))
        if report.get("display_windows"):
            lines.append("")
            lines.append("### Display geometry only (%s)" % report["frames"]["display_geometry"])
            lines.append("")
            lines.append("| window | frames | duration s | fps | p50 s | p95 s | max s | draws/frame |")
            lines.append("|---|---|---|---|---|---|---|---|")
            for name, entry in report["display_windows"].items():
                if entry is None:
                    continue
                lines.append("| %s | %d | %.1f | %.2f | %.3f | %.3f | %.2f | %s |" % (
                    name, entry["frames"], entry["duration_s"], entry["fps"],
                    entry["gap_p50_s"], entry["gap_p95_s"], entry["gap_max_s"],
                    "%.2f" % entry["draws_per_frame"]
                    if entry["draws_per_frame"] is not None else "(no host log)"))
            lines.append("")
            lines.append("The display-geometry table counts only frames presented at the panel's "
                         "size; the main table counts every frame `--out` wrote, including the "
                         "intermediate surfaces CA presents for icons/material layers, which is "
                         "the definition the t9 report used.")
        lines.append("")
        lines.append("## Method")
        lines.append("")
        lines.append("- Frame time is the PPM's mtime: `metalwire_host --out` writes each frame "
                     "after its read-back and publication, so consecutive mtimes are the wall-"
                     "clock distance between recorded frames.")
        lines.append("- FPS inside a window is `(frames - 1) / (last mtime - first mtime)`; the "
                     "first frame of the run has no predecessor and is not extrapolated.")
        lines.append("- p50 is the median inter-frame gap; p95 is nearest-rank, "
                     "`sorted(gaps)[floor(len(gaps) * 0.95) - 1]`, the definition the t9 report "
                     "used.")
        lines.append("- `draws/frame` is the mean of the host's "
                     "`render frame N: executed X of Y draw(s)` lines for the window's frames "
                     "that carry one; the `--missing-report` file remains the authority on skipped "
                     "draws.")
        if trigger_epoch is None:
            lines.append("- Without `--trigger` no boot/scene boundary is assumed: only the last-N "
                         "window is reported, so no steady-state number is claimed.")
        lines.append("")
        with open(args.out_markdown, "w") as handle:
            handle.write("\n".join(lines) + "\n")

    summary = report["windows"].get("desktop_steady") or report["windows"].get(
        "last_%d" % args.tail)
    if summary:
        print("fps-measure: %d display frames, overall %.2f fps; %s: %d frames / %.1f s = "
              "%.2f fps, gap p50 %.3f s p95 %.3f s max %.2f s, %.2f draws/frame" % (
                  report["frames"]["display_frames"], report["timeline"]["total_fps"],
                  "desktop_steady" if "desktop_steady" in report["windows"]
                  else "last_%d" % args.tail, summary["frames"], summary["duration_s"],
                  summary["fps"], summary["gap_p50_s"], summary["gap_p95_s"],
                  summary["gap_max_s"],
                  summary["draws_per_frame"] if summary["draws_per_frame"] is not None else -1.0))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

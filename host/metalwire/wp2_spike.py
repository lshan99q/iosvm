#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""wp2_spike -- the WP-2 Vulkan GLSL spike, measured on lavapipe (Vulkan device 1).

The spike asks three questions (docs/spike-wp2-vulkan-glsl.md):

  1. do the four hand-written base functions cover the three pipelines?
  2. does the rewritten interface consume the 13 sets completely (including R8Unorm)?
  3. what is CoreAnimation's blending behaviour, given blendingEnabled = NO?

It answers them by driving the real host execution path with the synthetic protocol-2 frame from
metalwire_gen.py --render, once per composition variant, and reading the published picture:

  fetch          draw B's pipeline is PBGRABsov_TimgXfc_Isrc (blendEnabled = NO) and the fragment
                 shader reads the destination snapshot and does source-over -- the CA path
  nofetch        the same draws with a fragment shader that does not read the destination
  fixed          the same draws with blendingEnabled = YES and source-alpha/one-minus-source-alpha
  fetch-noramp   the CA path, but the R8Unorm luma ramp is left empty (proves the R8 texture is
                 actually sampled: the resulting pixel differs)

The checks are:
  * the three pipelines are created and all three draws execute (host log),
  * no Vulkan validation error (when the validation layer is installed),
  * fetch and fixed agree on RGB (source-over is reproduced in-shader),
  * nofetch differs (blendingEnabled = NO really means replace unless the shader fetches),
  * fetch and fetch-noramp differ (the 128x1 R8Unorm texture is consumed).

Run from host/metalwire/:  ./wp2_spike.py     (make spike runs the same)
"""

import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "metalwire_host")
GEN = os.path.join(HERE, "metalwire_gen.py")
SHM_MAGIC = 0x57464C47
CENTER = (375, 812)  # inside every full-screen draw

EXPECTED = {
    "fetch": "in-shader source-over",
    "nofetch": "replace (no compositing)",
    "fixed": "fixed-function source-over",
    "fetch-noramp": "in-shader source-over, empty ramp",
}


def free_port():
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    return port


def wait_listening(log_path, timeout=10.0):
    needle = "metalwire: listening on 127.0.0.1:"
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with open(log_path, "r", errors="replace") as handle:
                if needle in handle.read():
                    return True
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    return False


def read_center_pixel(path):
    with open(path, "rb") as handle:
        data = handle.read()
    if len(data) < 40:
        raise RuntimeError("shared-memory frame is truncated")
    magic, _seq, width, height, nbytes = struct.unpack_from("<QQIII", data, 0)
    if magic != SHM_MAGIC:
        raise RuntimeError("shared-memory frame has the wrong magic")
    if len(data) < 40 + nbytes:
        raise RuntimeError("shared-memory frame is shorter than its bytes field")
    x, y = CENTER
    at = 40 + (y * width + x) * 4
    b, g, r, a = data[at : at + 4]
    return (r, g, b, a), (width, height)


def run_variant(variant, validation):
    port = free_port()
    tmp = tempfile.mkdtemp(prefix="wp2-spike-")
    shm = os.path.join(tmp, "frame.shm")
    log = os.path.join(tmp, "host.log")
    env = dict(os.environ)
    if validation:
        env["VK_INSTANCE_LAYERS"] = "VK_LAYER_KHRONOS_validation"
    with open(log, "wb") as handle:
        host = subprocess.Popen(
            [HOST, "--gpu", "1", "--port", str(port), "--shm", shm, "--frames", "0"],
            stdout=handle, stderr=subprocess.STDOUT, env=env)
    try:
        if not wait_listening(log):
            raise RuntimeError("host did not start listening")
        result = subprocess.run(
            [sys.executable, GEN, "--render", "--variant", variant, "--port", str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        time.sleep(0.3)
        pixel, geometry = read_center_pixel(shm)
        with open(log, "r", errors="replace") as handle:
            text = handle.read()
        executed = "executed 3 draw(s)" in text
        created = text.count("-> Vulkan pipeline")
        errors = re.findall(r"VUID-\S+|Validation Error", text)
        return {
            "variant": variant,
            "exit": result.returncode,
            "pixel": pixel,
            "geometry": geometry,
            "executed": executed,
            "pipelines": created,
            "validation_errors": len(errors),
            "output": result.stdout,
        }
    finally:
        host.terminate()
        try:
            host.wait(timeout=5)
        except subprocess.TimeoutExpired:
            host.kill()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--validation", action="store_true",
                        help="load VK_LAYER_KHRONOS_validation for every run")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    if not os.path.exists(HOST):
        sys.exit("wp2_spike: build the host first (make)")
    if args.validation and not os.path.exists(
            "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"):
        sys.exit("wp2_spike: the Khronos validation layer is not installed")

    results = []
    failures = 0
    for variant in ("fetch", "nofetch", "fixed", "fetch-noramp"):
        try:
            result = run_variant(variant, args.validation)
        except Exception as exc:  # noqa: BLE001 - the report is the point
            print(f"{variant:13s} FAIL: {exc}")
            failures += 1
            continue
        results.append(result)
        if result["exit"] != 0:
            failures += 1
        if not result["executed"] or result["pipelines"] != 3:
            failures += 1
        if args.validation and result["validation_errors"]:
            failures += 1

    by_variant = {r["variant"]: r for r in results}
    print("WP-2 spike on lavapipe (VK_ICD_FILENAMES=lvp_icd.json, host --gpu 1)")
    print(f"{'variant':13s} {'status':7s} {'pipelines':9s} {'center pixel':18s} expectation")
    for r in results:
        print(f"{r['variant']:13s} {'ok' if r['exit'] == 0 else 'FAIL':7s} "
              f"{r['pipelines']:<9d} {str(r['pixel']):18s} "
              f"{EXPECTED.get(r['variant'], '')}")
    checks = []
    if len(by_variant) == 4:
        fetch = by_variant["fetch"]["pixel"]
        nofetch = by_variant["nofetch"]["pixel"]
        fixed = by_variant["fixed"]["pixel"]
        noramp = by_variant["fetch-noramp"]["pixel"]
        checks.append(("three pipelines executed",
                       all(r["executed"] and r["pipelines"] == 3 for r in results)))
        checks.append(("no Vulkan validation error",
                       all(r["validation_errors"] == 0 for r in results)))
        checks.append(("in-shader fetch reproduces fixed-function source-over",
                       fetch[:3] == fixed[:3]))
        checks.append(("blendingEnabled=NO does replace without the fetch",
                       fetch[:3] != nofetch[:3]))
        checks.append(("the 128x1 R8Unorm ramp is sampled",
                       fetch[:3] != noramp[:3]))
    else:
        checks.append(("all four variants ran", False))
    for label, ok in checks:
        print(f"{'PASS' if ok else 'FAIL'} {label}")
        if not ok:
            failures += 1
    if args.json:
        import json

        print(json.dumps(results, indent=2))
    print(f"wp2_spike: {'PASS' if failures == 0 else 'FAIL'} ({len(results)}/4 variants)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

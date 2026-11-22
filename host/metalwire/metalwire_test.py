#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""metalwire_test -- the CI-safe test for host/metalwire.

It runs the real host translator on lavapipe (Mesa's CPU Vulkan implementation), drives it with
the synthetic wire client metalwire_gen.py, and checks the contract end to end:

  * the host builds a Vulkan device, executes the blits and answers two frames,
  * frame 0's CRC matches (crc_ok=1) and frame 1's deliberately wrong CRC is caught (crc_ok=0),
  * the CSV has one row per frame with plausible timings,
  * the shared-memory frame is glwire's format and its pixels are exactly the picture the client
    sent,
  * the PPM (and the diagnostic PNG) hold the same picture.

No real GPU is needed. Without lavapipe the test prints SKIP and exits 0, so `make test` stays
green on a machine that only has a proprietary driver or no Vulkan at all.
"""

import csv
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import threading

BASE = os.path.dirname(os.path.abspath(__file__))
GEN = os.path.join(BASE, "metalwire_gen.py")
HOST = os.path.join(BASE, "metalwire_host")
LAVAPIPE = "/usr/share/vulkan/icd.d/lvp_icd.json"
SHM_HEADER = struct.Struct("<QQIIIIQ")
WIDTH = 750
HEIGHT = 1624
FRAMES = 2
BAD_CRC_AT = 1
PICTURE_BYTES = WIDTH * HEIGHT * 4

sys.path.insert(0, BASE)
import metalwire_gen  # noqa: E402  (the picture definition lives with the generator)


class TestFailure(RuntimeError):
    pass


def require(condition, message):
    if not condition:
        raise TestFailure(message)


def to_rgb(picture, bgra):
    """The picture in PPM/PNG order, the same conversion the host does."""
    out = bytearray(len(picture) // 4 * 3)
    for i in range(0, len(picture), 4):
        at = (i // 4) * 3
        if bgra:
            out[at] = picture[i + 2]
            out[at + 1] = picture[i + 1]
            out[at + 2] = picture[i]
        else:
            out[at] = picture[i]
            out[at + 1] = picture[i + 1]
            out[at + 2] = picture[i + 2]
    return bytes(out)


def read_ppm(path, width, height):
    with open(path, "rb") as f:
        data = f.read()
    header = b"P6\n%d %d\n255\n" % (width, height)
    require(data.startswith(header), "%s: bad P6 header" % path)
    pixels = data[len(header):]
    require(len(pixels) == width * height * 3, "%s: %d pixel bytes, expected %d" %
            (path, len(pixels), width * height * 3))
    return pixels



def run_v4_fallback(env):
    """WP-4: a v4 frame whose second pipeline has no shader still produces a picture.

    The synthetic client names a pipeline with base functions this host does not implement. The
    host must execute the other two draws, skip that one, acknowledge the frame with status OK and
    write the pipeline (with the library it came from) to --missing-report. This is the whole
    "unknown pipeline never takes the boot down" contract, without a guest.
    """
    work = tempfile.mkdtemp(prefix="metalwire-v4-")
    report = os.path.join(work, "missing.txt")
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "3", "--shm", "",
         "--missing-report", report],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    host_lines = []
    host_lock = threading.Lock()
    listening = threading.Event()

    def drain_host():
        for line in host.stderr:
            with host_lock:
                host_lines.append(line.rstrip("\n"))
            if "listening on" in line:
                listening.set()

    reader = threading.Thread(target=drain_host, daemon=True)
    reader.start()
    try:
        require(listening.wait(60), "v4 host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the v4 host's port")
        gen = subprocess.run(
            [sys.executable, GEN, str(WIDTH), str(HEIGHT), "--render", "--variant", "unknown",
             "--protocol", "4", "--port", match.group(1)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=180)
        require(gen.returncode == 0,
                "v4 generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout, gen.stderr))
        require(host.wait(timeout=120) == 0,
                "v4 host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            output = "\n".join(host_lines)
        require("ack status=ok" in gen.stdout,
                "the v4 render frame was not acknowledged as OK:\n%s" % gen.stdout)
        require('no embedded Vulkan GLSL for vertex "mystery_vert_lpf"' in output,
                "the host did not name the missing pipeline:\n%s" % output)
        require("executed 2 draw(s), skipped 1" in output,
                "the host did not report 2 executed / 1 skipped draws:\n%s" % output)
        require("library 1: data" in output, "the host did not decode MW_CREATE_LIBRARY")
        require("library 2: data" in output, "the host did not decode the second library")
        with open(report) as f:
            report_text = f.read()
        require('pipeline 44 name "com.apple.coreanimation.draw.PBGRABsov_MysteryXfc_Isrc"' in
                report_text, "the missing report does not hold the pipeline:\n%s" % report_text)
        require("vertex_library 1 fragment_library 1 skipped_draws 1" in report_text,
                "the missing report does not hold the library reference and the count")
        require('function "mystery_vert_lpf"' in report_text,
                "the missing report does not hold the library's entry function")
        print("PASS fallback: unknown pipeline skipped, frame acknowledged, missing report written")
    finally:
        if host.poll() is None:
            host.kill()


def main():
    if not os.path.exists(LAVAPIPE):
        print("metalwire_test: SKIP: lavapipe ICD not found at %s" % LAVAPIPE)
        return 0
    require(os.path.exists(HOST), "metalwire_host is not built (run `make` first)")
    require(os.path.exists(GEN), "metalwire_gen.py is missing")

    work = tempfile.mkdtemp(prefix="metalwire-test-")
    shm = os.path.join(work, "frame.shm")
    csv_path = os.path.join(work, "frames.csv")
    out_dir = os.path.join(work, "frames")
    env = os.environ.copy()
    env.pop("VK_DRIVER_FILES", None)
    env["VK_ICD_FILENAMES"] = LAVAPIPE

    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", str(FRAMES), "--shm", shm,
         "--csv", csv_path, "--out", out_dir, "--png-every", "1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    host_lines = []
    host_lock = threading.Lock()
    listening = threading.Event()

    def drain_host():
        for line in host.stderr:
            with host_lock:
                host_lines.append(line.rstrip("\n"))
            if "listening on" in line:
                listening.set()

    reader = threading.Thread(target=drain_host, daemon=True)
    reader.start()
    gen = None
    try:
        require(listening.wait(60), "host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the host's port")
        port = int(match.group(1))

        gen = subprocess.run(
            [sys.executable, GEN, str(WIDTH), str(HEIGHT), "--frames", str(FRAMES),
             "--bad-crc-at", str(BAD_CRC_AT), "--port", str(port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=180)
        require(gen.returncode == 0,
                "generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout, gen.stderr))
        require(host.wait(timeout=120) == 0,
                "host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            host_output = "\n".join(host_lines)
        gen_output = gen.stdout
        require("llvmpipe" in host_output,
                "the host did not select lavapipe (llvmpipe):\n%s" % host_output)

        frame_lines = [line for line in gen_output.splitlines() if line.startswith("frame ")]
        require(len(frame_lines) == FRAMES,
                "expected %d frame lines from the generator, got %d:\n%s" %
                (FRAMES, len(frame_lines), gen_output))
        require("crc_ok=1" in frame_lines[0], "frame 0 did not report crc_ok=1: %s" % frame_lines[0])
        require("crc_ok=0" in frame_lines[BAD_CRC_AT],
                "the bad-CRC frame did not report crc_ok=0: %s" % frame_lines[BAD_CRC_AT])
        print("PASS frames: generator saw %d ack(s), crc_ok=1 then crc_ok=0 as expected" % FRAMES)

        with open(csv_path, newline="") as f:
            rows = list(csv.reader(f))
        require(len(rows) == FRAMES + 1, "CSV has %d rows, expected %d" % (len(rows), FRAMES + 1))
        require(rows[0] == ["seq", "bytes", "recv_us", "exec_us", "gpu_us", "readback_us",
                            "publish_us", "total_us", "crc_ok", "fps"], "bad CSV header: %s" % rows[0])
        for index in range(FRAMES):
            row = rows[index + 1]
            require(len(row) == 10, "CSV row %d has %d fields" % (index, len(row)))
            require(row[0] == str(index), "CSV row %d names seq %s" % (index, row[0]))
            require(int(row[1]) == PICTURE_BYTES,
                    "CSV row %d has %s bytes, expected %d" % (index, row[1], PICTURE_BYTES))
            for column in range(2, 8):
                require(int(row[column]) >= 0, "CSV row %d column %d is not a duration" % (index, column))
            require(int(row[7]) > 0, "CSV row %d has zero total_us" % index)
            require(int(row[8]) == (0 if index == BAD_CRC_AT else 1),
                    "CSV row %d has crc_ok=%s" % (index, row[8]))
        print("PASS csv: %d rows, timings present, crc_ok 1/%d" % (FRAMES, BAD_CRC_AT))

        require(os.path.exists(shm), "the host did not publish the shared frame file")
        with open(shm, "rb") as f:
            data = f.read()
        require(len(data) == SHM_HEADER.size + PICTURE_BYTES,
                "shm file is %d bytes, expected %d" % (len(data), SHM_HEADER.size + PICTURE_BYTES))
        magic, seq, width, height, nbytes, pad, _published = SHM_HEADER.unpack_from(data, 0)
        require(magic == 0x57464C47, "shm magic 0x%08x is not glwire's 0x57464c47" % magic)
        require(seq >= FRAMES, "shm sequence %d did not advance" % seq)
        require((width, height, nbytes, pad) == (WIDTH, HEIGHT, PICTURE_BYTES, 0),
                "shm geometry %dx%d %d bytes pad %d" % (width, height, nbytes, pad))
        expected = metalwire_gen.make_picture(WIDTH, HEIGHT, FRAMES - 1, bgra=True)
        require(data[SHM_HEADER.size:] == expected,
                "shm pixels differ from the last picture the client sent")
        print("PASS shm: glwire header and %d BGRA bytes of the last frame" % PICTURE_BYTES)

        for index in range(FRAMES):
            picture = metalwire_gen.make_picture(WIDTH, HEIGHT, index, bgra=True)
            ppm_path = os.path.join(out_dir, "frame-%06d.ppm" % index)
            require(os.path.exists(ppm_path), "%s was not written" % ppm_path)
            require(read_ppm(ppm_path, WIDTH, HEIGHT) == to_rgb(picture, bgra=True),
                    "%s does not hold frame %d in RGB order" % (ppm_path, index))
            png_path = os.path.join(out_dir, "frame-%06d.png" % index)
            require(os.path.exists(png_path), "%s was not written" % png_path)
            with open(png_path, "rb") as f:
                png = f.read(33)
            require(png.startswith(b"\x89PNG\r\n\x1a\n") and png[12:16] == b"IHDR",
                    "%s is not a PNG" % png_path)
            png_width = int.from_bytes(png[16:20], "big")
            png_height = int.from_bytes(png[20:24], "big")
            require((png_width, png_height, png[24], png[25]) == (WIDTH, HEIGHT, 8, 2),
                    "%s has %dx%d depth %d colour %d" % (png_path, png_width, png_height,
                                                         png[24], png[25]))
        print("PASS files: %d PPM frame(s) match the sent picture, PNG headers are valid" % FRAMES)
        run_v4_fallback(env)
        print("metalwire_test: PASS (lavapipe, %dx%d, %d frames, v4 fallback)" %
              (WIDTH, HEIGHT, FRAMES))
        return 0
    except TestFailure as failure:
        print("metalwire_test: FAIL: %s" % failure)
        try:
            host.kill()
        except OSError:
            pass
        if gen is not None:
            print("--- generator stdout ---\n%s\n--- generator stderr ---\n%s" %
                  (gen.stdout, gen.stderr))
        print("--- host stderr ---\n%s" % "\n".join(host_lines))
        print("work directory kept for inspection: %s" % work)
        return 1
    finally:
        if host.poll() is None:
            host.kill()
            host.wait()
        reader.join(timeout=5)
        if host.returncode == 0:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

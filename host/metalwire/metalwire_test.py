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
import time
import zlib

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


def half_ramp_rgb(ramp):
    """The PPM bytes the host's RGBA16Float conversion must produce: clamp, round to nearest."""
    out = bytearray()
    for at in range(0, len(ramp), 8):
        r, g, b, _alpha = struct.unpack_from("<4e", ramp, at)
        for value in (r, g, b):
            if value <= 0.0:
                out.append(0)
            elif value >= 1.0:
                out.append(255)
            else:
                out.append(int(value * 255.0 + 0.5))
    return bytes(out)


def run_format115(env):
    """WP-4 1.3: a 256x1 RGBA16Float texture (MTLPixelFormat 115) has to cross the wire.

    boot wp4-v4 answered 'texture 61: pixel format 115 is not implemented', so CoreAnimation's
    colour ramp was simply absent from the desktop. This runs one frame whose presented texture is
    that ramp: creation, the 8-byte-texel blit, the read-back CRC and the PPM/PNG conversion all
    have to agree with the bytes the client sent, and the host has to acknowledge the frame.
    """
    work = tempfile.mkdtemp(prefix="metalwire-format115-")
    out_dir = os.path.join(work, "frames")
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "1", "--shm", "", "--out", out_dir,
         "--png-every", "1", "--byte-check", "--verbose"],
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
    ok = False
    try:
        require(listening.wait(60), "format115 host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the format115 host's port")
        gen = subprocess.run(
            [sys.executable, GEN, "--rgba16f-ramp", "--port", match.group(1)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=180)
        require(gen.returncode == 0,
                "format115 generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout, gen.stderr))
        require("wire error" not in gen.stderr,
                "format115 generator saw a wire error:\n%s" % gen.stderr)
        require(host.wait(timeout=120) == 0,
                "format115 host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            output = "\n".join(host_lines)
        require("rgba16f frame 0: ack status=ok crc_ok=1" in gen.stdout,
                "the RGBA16Float frame was not acknowledged:\n%s" % gen.stdout)
        require("texture 61: 256x1 format 115, 2048 bytes" in output,
                "the host did not create a 256x1 8-byte-texel texture:\n%s" % output)
        require("pixel format 115 is not implemented" not in output,
                "the host still falls back on format 115:\n%s" % output)
        require("picture byte-identical: 2048 bytes of 256x1" in output,
                "the read-back is not byte-identical to the uploaded ramp:\n%s" % output)
        require("texture 62: pixel format 999 is not implemented" in output,
                "an unknown pixel format no longer takes the skip-draw fallback:\n%s" % output)
        ramp = metalwire_gen.make_half_ramp()
        width = metalwire_gen.RAMP_WIDTH
        ppm_path = os.path.join(out_dir, "frame-000000.ppm")
        require(os.path.exists(ppm_path), "%s was not written" % ppm_path)
        require(read_ppm(ppm_path, width, 1) == half_ramp_rgb(ramp),
                "%s does not hold the RGBA16Float ramp converted to RGB" % ppm_path)
        png_path = os.path.join(out_dir, "frame-000000.png")
        require(os.path.exists(png_path), "%s was not written" % png_path)
        with open(png_path, "rb") as f:
            png = f.read(33)
        require(png.startswith(b"\x89PNG\r\n\x1a\n") and png[12:16] == b"IHDR",
                "%s is not a PNG" % png_path)
        png_width = int.from_bytes(png[16:20], "big")
        png_height = int.from_bytes(png[20:24], "big")
        require((png_width, png_height, png[24], png[25]) == (width, 1, 8, 2),
                "%s has %dx%d depth %d colour %d" % (png_path, png_width, png_height, png[24],
                                                    png[25]))
        print("PASS format115: 256x1 RGBA16Float uploaded, presented and read back "
              "byte-identical")
        ok = True
    except TestFailure:
        with host_lock:
            print("--- format115 host stderr ---\n%s" % "\n".join(host_lines))
        raise
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)
        if ok:
            shutil.rmtree(work, ignore_errors=True)


def run_blur_pipelines(env):
    """WP-4 1.2: all eight backdrop pipelines execute instead of being skipped.

    boot wp4-v4 reported the 4x downsample plus the 23/15-tap blurs unsupported; boot wp4-v6's
    Control Center/Spotlight backdrop needed five more (8x downsample, 7/11/19/27-tap blurs) and
    the missing report counted 1916 skipped draws. This runs the synthetic blur fixture: one seed
    frame copies a 64x64 input texture, then one render frame draws all eight pipelines into one
    64x64 target -- each pipeline paints its own 16x16 tile of a 4x2 mosaic, sampling the same
    input. The PPM/PNG of that frame therefore carry every kernel's output, and the test compares
    each tile against the box/binomial kernels in Python.
    """
    tile = metalwire_gen.BLUR_TILE
    size = metalwire_gen.BLUR_TARGET
    source, expected = metalwire_gen.blur_fixture_reference()
    work = tempfile.mkdtemp(prefix="metalwire-blur-")
    out_dir = os.path.join(work, "frames")
    report = os.path.join(work, "missing.txt")
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "2", "--shm", "", "--out", out_dir,
         "--png-every", "1", "--missing-report", report, "--verbose"],
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
    ok = False
    try:
        require(listening.wait(60), "blur host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the blur host's port")
        gen = subprocess.run(
            [sys.executable, GEN, "--blur-pipelines", "--port", match.group(1)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=180)
        require(gen.returncode == 0,
                "blur generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout, gen.stderr))
        require("wire error" not in gen.stderr,
                "blur generator saw a wire error:\n%s" % gen.stderr)
        require(host.wait(timeout=120) == 0,
                "blur host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            output = "\n".join(host_lines)
        require("blur frame 1: ack status=ok" in gen.stdout,
                "the eight-pipeline frame was not acknowledged:\n%s" % gen.stdout)
        require("executed 8 of 8 draw(s) on texture 73 (64x64)" in output,
                "the host did not execute all eight blur draws in one frame:\n%s" % output)
        require("skipped" not in output.split("render frame 1: 1 encoder(s)")[0].split(
                    "render frame 1: executed")[-1],
                "the blurred frame reports skipped draws:\n%s" % output)
        for pipeline_id, name, _fragment, _kernel in metalwire_gen.BLUR_PIPELINES:
            require("-> Vulkan pipeline" in output and name in output,
                    "the host did not build a Vulkan pipeline for %s:\n%s" % (name, output))
        with open(report) as f:
            report_text = f.read()
        require("# frames 2, pipelines 0, skipped draws 0" in report_text,
                "the blur pipelines are still in the missing report:\n%s" % report_text)
        require("downsample_blur_vert_lpf" not in report_text and
                "narrow_blur_" not in report_text and "downsample_" not in report_text,
                "the missing report still names the blur family:\n%s" % report_text)
        ppm_path = os.path.join(out_dir, "frame-000001.ppm")
        require(os.path.exists(ppm_path), "%s was not written" % ppm_path)
        with open(ppm_path, "rb") as f:
            data = f.read()
        header = b"P6\n%d %d\n255\n" % (size, size)
        require(data.startswith(header), "%s: bad P6 header" % ppm_path)
        pixels = data[len(header):]
        require(len(pixels) == len(expected),
                "%s: %d pixel bytes, expected %d" % (ppm_path, len(pixels), len(expected)))
        worst = max(abs(a - b) for a, b in zip(pixels, expected))
        require(worst <= 2,
                "%s: blur mosaic differs from the predicted kernels by %d (allowed 2)" %
                (ppm_path, worst))
        worst_tile = 0
        for index in range(len(metalwire_gen.BLUR_PIPELINES)):
            tx, ty = metalwire_gen.BLUR_TILES[index]
            for y in range(tile):
                for x in range(tile):
                    at = ((ty + y) * size + tx + x) * 3
                    for c in range(3):
                        worst_tile = max(worst_tile, abs(pixels[at + c] - expected[at + c]))
        require(worst_tile <= 2,
                "a blur tile differs from its kernel by %d (allowed 2)" % worst_tile)
        png_path = os.path.join(out_dir, "frame-000001.png")
        require(os.path.exists(png_path), "%s was not written" % png_path)
        width, height, png_pixels = read_png_rgb(png_path)
        require((width, height) == (size, size), "%s: %dx%d" % (png_path, width, height))
        require(png_pixels == pixels, "%s differs from the PPM of the same frame" % png_path)
        print("PASS blur: 8 pipelines executed in one frame, 0 skipped, mosaic matches the "
              "predicted kernels (max diff %d/255)" % worst)
        ok = True
    except TestFailure:
        with host_lock:
            print("--- blur host stderr ---\n%s" % "\n".join(host_lines))
        raise
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)
        if ok:
            shutil.rmtree(work, ignore_errors=True)


def run_labels(env):
    """WP-4 D4: an A8 glyph mask must modulate the label colour, not become the colour.

    boot wp4-v6 drew every icon label from an A8Unorm mask (Metal's alpha-only glyph format,
    protocol 1) modulated by a colour constant; the host's single-channel image view handed the
    fragment stage (coverage, 0, 0), so every label pixel was (R, 0, 0) -- red glyphs, the whole
    D4 defect. The host now creates A8 views with a (1, 1, 1, coverage) swizzle, so a white label
    over black comes out as grey coverage in R, G and B, with the same value in the SHM alpha.
    """
    size = metalwire_gen.LABEL_MASK
    coverage = metalwire_gen.LABEL_COVERAGE
    work = tempfile.mkdtemp(prefix="metalwire-labels-")
    out_dir = os.path.join(work, "frames")
    shm_path = os.path.join(work, "frame.shm")
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "1", "--shm", shm_path, "--out", out_dir,
         "--png-every", "1", "--missing-report", os.path.join(work, "missing.txt"), "--verbose"],
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
    ok = False
    try:
        require(listening.wait(60), "labels host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the labels host's port")
        gen = subprocess.run(
            [sys.executable, GEN, "--labels", "--port", match.group(1)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        require(gen.returncode == 0,
                "labels generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout, gen.stderr))
        require(host.wait(timeout=60) == 0,
                "labels host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            output = "\n".join(host_lines)
        require("labels frame 1: ack status=ok" in gen.stdout,
                "the labels frame was not acknowledged:\n%s" % gen.stdout)
        require("executed 1 of 1 draw(s) on texture 91" in output,
                "the labels draw did not execute:\n%s" % output)
        ppm_path = os.path.join(out_dir, "frame-000000.ppm")
        with open(ppm_path, "rb") as f:
            data = f.read()
        header = b"P6\n%d %d\n255\n" % (size, size)
        require(data.startswith(header), "%s: bad P6 header" % ppm_path)
        pixels = data[len(header):]
        red_only = 0
        worst = 0
        for y in range(size):
            for x in range(size):
                at = (y * size + x) * 3
                r, g, b = pixels[at], pixels[at + 1], pixels[at + 2]
                expected = coverage[y * size + x]
                if r != g or g != b:
                    red_only += 1
                worst = max(worst, abs(r - expected), abs(g - expected), abs(b - expected))
        require(red_only == 0,
                "%d label pixels kept the mask in the red channel only (the D4 defect)" % red_only)
        require(worst <= 1,
                "the label picture is not the mask coverage in all channels (worst %d)" % worst)
        with open(shm_path, "rb") as f:
            shm = f.read()
        require(len(shm) >= 40 + size * size * 4, "%s is %d bytes" % (shm_path, len(shm)))
        body = shm[40:40 + size * size * 4]
        worst_alpha = 0
        for index in range(size * size):
            b, g, r, a = body[index * 4:index * 4 + 4]
            expected = coverage[index]
            require(r == g == b,
                    "SHM pixel %d is (%d, %d, %d), not grey" % (index, r, g, b))
            worst_alpha = max(worst_alpha, abs(a - expected))
        require(worst_alpha <= 1,
                "the SHM alpha is not the mask coverage (worst %d)" % worst_alpha)
        print("PASS labels: A8 glyph mask modulates a white colour -- grey coverage in RGB, mask in "
              "alpha (D4 red labels fixed)")
        ok = True
    except TestFailure:
        with host_lock:
            print("--- labels host stderr ---\n%s" % "\n".join(host_lines))
        raise
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)
        if ok:
            shutil.rmtree(work, ignore_errors=True)


def run_chain(env):
    """WP-4 D3/D5: a frame's later pass must sample the texture an earlier pass rendered.

    CA's backdrop chain renders one pass into an intermediate texture and the next pass samples
    it; the host used to execute every draw into the frame's last target, so the second pass
    sampled a texture the first pass never wrote (black). The fixture sends one frame with two
    render encoders (source -> texture 101, texture 101 -> texture 102) and a second frame that
    samples 101 again; both pictures must repeat the source.
    """
    size = metalwire_gen.CHAIN_SOURCE
    source = metalwire_gen.make_orientation_source(size)
    expected = bytearray()
    for at in range(0, len(source), 4):
        expected += bytes((source[at + 2], source[at + 1], source[at]))
    expected = bytes(expected)
    work = tempfile.mkdtemp(prefix="metalwire-chain-")
    out_dir = os.path.join(work, "frames")
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "2", "--shm", "", "--out", out_dir,
         "--png-every", "1", "--verbose"],
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
    ok = False
    try:
        require(listening.wait(60), "chain host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the chain host's port")
        gen = subprocess.run(
            [sys.executable, GEN, "--render-chain", "--port", match.group(1)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        require(gen.returncode == 0,
                "chain generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout, gen.stderr))
        require(host.wait(timeout=60) == 0,
                "chain host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            output = "\n".join(host_lines)
        require("chain frames: ack status=ok" in gen.stdout,
                "the chain frames were not acknowledged:\n%s" % gen.stdout)
        require("render frame 0: 2 encoder(s), 2 draws" in output,
                "the host did not execute both encoders of the chained frame:\n%s" % output)
        require("render frame 0: executed 2 of 2 draw(s) on texture 102" in output,
                "the chained frame did not complete:\n%s" % output)
        require("render frame 1: executed 1 of 1 draw(s) on texture 102" in output,
                "the second frame did not read the intermediate texture back:\n%s" % output)
        for index in (0, 1):
            ppm_path = os.path.join(out_dir, "frame-%06d.ppm" % index)
            require(os.path.exists(ppm_path), "%s was not written" % ppm_path)
            with open(ppm_path, "rb") as f:
                data = f.read()
            header = b"P6\n%d %d\n255\n" % (size, size)
            require(data.startswith(header), "%s: bad P6 header" % ppm_path)
            pixels = data[len(header):]
            require(pixels == expected,
                    "%s: the chained picture is not the source (max diff %d)" % (
                        ppm_path, max(abs(a - b) for a, b in zip(pixels, expected))))
            png_path = os.path.join(out_dir, "frame-%06d.png" % index)
            width, height, png_pixels = read_png_rgb(png_path)
            require((width, height) == (size, size), "%s: %dx%d" % (png_path, width, height))
            require(png_pixels == pixels, "%s differs from the PPM of the same frame" % png_path)
        print("PASS chain: per-pass targets honoured -- a frame's second pass samples the texture "
              "the first pass rendered (D3/D5 backdrop chain)")
        ok = True
    except TestFailure:
        with host_lock:
            print("--- chain host stderr ---\n%s" % "\n".join(host_lines))
        raise
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)
        if ok:
            shutil.rmtree(work, ignore_errors=True)


def _run_labels_once(base_env, work, extra_env):
    """Run the labels fixture once with a given environment; return (host output, host code)."""
    out_dir = os.path.join(work, "frames")
    os.makedirs(out_dir, exist_ok=True)
    env = dict(base_env)
    env.update(extra_env)
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "1", "--shm", "",
         "--out", out_dir, "--png-every", "1", "--verbose"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=env)
    lines = []
    lock = threading.Lock()
    listening = threading.Event()

    def drain():
        for line in host.stderr:
            with lock:
                lines.append(line.rstrip("\n"))
            if "listening on" in line:
                listening.set()

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    try:
        require(listening.wait(60), "draw-dump host did not print a listening line in 60 s")
        with lock:
            port = None
            for line in lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    port = match.group(1)
                    break
        require(port is not None, "cannot parse the draw-dump host's port")
        gen = subprocess.run([sys.executable, GEN, "--labels", "--port", port],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             timeout=120)
        require(gen.returncode == 0, "labels fixture failed under the draw-dump test:\n%s\n%s" %
                (gen.stdout, gen.stderr))
        code = host.wait(timeout=60)
        reader.join(timeout=10)
        with lock:
            return "\n".join(lines), code
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)


def run_draw_dump(env):
    """WP-4 D3/D5 support: MW_DUMP_DRAWS writes one machine-readable line per executed draw.

    The switch is off by default: no file, no extra output. With it on, every executed draw gets a
    stable key=value line carrying the vertex colour, the fragment uniform's first 16 bytes and the
    draw's target-space bbox -- the three fields t16 needed for a Dock/Search frame. The labels
    fixture draws once (32x32, white, texture 90, no fetch), so the line's values are known.
    """
    size = metalwire_gen.LABEL_MASK
    work = tempfile.mkdtemp(prefix="metalwire-drawdump-")
    dump_path = os.path.join(work, "draws.txt")
    ok = False
    try:
        on_output, on_code = _run_labels_once(env, work, {
            "MW_DUMP_DRAWS": "1", "MW_DUMP_DRAWS_FILE": dump_path})
        require(on_code == 0, "draw-dump host exited %s\n%s" % (on_code, on_output))
        require("draw dump: %s" % dump_path in on_output,
                "the enabled switch did not announce its file:\n%s" % on_output)
        require(os.path.exists(dump_path), "%s was not written" % dump_path)
        with open(dump_path) as handle:
            lines = [line.rstrip("\n") for line in handle if line.strip()]
        require(len(lines) == 1, "expected one draw line, got %d:\n%s" % (len(lines),
                                                                         "\n".join(lines)))
        pattern = re.compile(
            r'^draw seq=(\d+) frame=(\d+) pipeline="([^"]+)" '
            r"vertex_color=(-?\d+\.\d{4}),(-?\d+\.\d{4}),(-?\d+\.\d{4}),(-?\d+\.\d{4}) "
            r"frag_uniform_buffer=(\d+) frag_uniform_offset=(\d+) "
            r"uniform16=([0-9a-f]{32}) uniform16_f32=(-?\d+\.\d{4}),(-?\d+\.\d{4}),"
            r"(-?\d+\.\d{4}),(-?\d+\.\d{4}) "
            r"bbox=(-?\d+),(-?\d+),(-?\d+),(-?\d+) texture=(\d+) fetch=([01]) "
            r"target=(\d+) load=(\d+) store=(\d+)$")
        match = pattern.match(lines[0])
        require(match is not None, "the draw line does not match the documented format:\n%s" %
                lines[0])
        require(match.group(1) == "1" and match.group(2) == "0",
                "unexpected seq/frame: %s/%s" % (match.group(1), match.group(2)))
        require(match.group(3) == "com.apple.coreanimation.draw.PBGRA_TimgXfc_Isrc",
                "unexpected pipeline %s" % match.group(3))
        for group in range(4, 8):
            require(float(match.group(group)) == 1.0,
                    "unexpected vertex colour component %s" % match.group(group))
        require(match.group(8) == "93" and match.group(9) == "0",
                "unexpected fragment uniform binding %s/%s" % (match.group(8), match.group(9)))
        require(match.group(10) == "0" * 32,
                "unexpected uniform bytes %s" % match.group(10))
        require((match.group(15), match.group(16), match.group(17), match.group(18)) ==
                ("0", "0", str(size), str(size)),
                "unexpected bbox %s,%s,%s,%s" % (match.group(15), match.group(16),
                                                 match.group(17), match.group(18)))
        require(match.group(19) == "90" and match.group(20) == "0",
                "unexpected texture/fetch %s/%s" % (match.group(19), match.group(20)))
        require(match.group(21) == "91" and match.group(22) == "2" and match.group(23) == "1",
                "unexpected target/load/store %s/%s/%s" % (match.group(21), match.group(22),
                                                           match.group(23)))

        off_path = os.path.join(work, "off-draws.txt")
        off_output, off_code = _run_labels_once(env, os.path.join(work, "off"), {
            "MW_DUMP_DRAWS_FILE": off_path})
        require(off_code == 0, "off-switch host exited %s\n%s" % (off_code, off_output))
        require("draw dump" not in off_output,
                "the off switch still printed a draw-dump line:\n%s" % off_output)
        require(not os.path.exists(off_path),
                "the off switch still wrote %s" % off_path)
        print("PASS draw-dump: MW_DUMP_DRAWS=1 writes one documented line per draw "
              "(pipeline/vertex_color/uniform16/bbox/texture/fetch/target/load/store); off "
              "writes nothing")
        ok = True
    finally:
        if ok:
            shutil.rmtree(work, ignore_errors=True)


def run_material(env):
    """WP-4 D3/D5: a Tmua material draw must sample its support texture, not paint white.

    The Dock/Search bands come from `PBGRABsov_TmuaXfcn_IsrcCcl_Isup` (wp4-v12 dump lines
    100/103/310/313): white vertex colour, fetch=1, fragment texture 3 = the support texture. The
    host used to specialise that pipeline with HAS_TEXTURE=0 (the "Tmua" base function was not in
    the texture list), so the fragment shader painted the wire's white colour opaque over the
    fetched destination -- the flat white band. The fixture draws a non-white support texture
    through the same pipeline name over a cleared target; the picture must be the texture.
    """
    size = metalwire_gen.MATERIAL_SIZE
    pattern = metalwire_gen.MATERIAL_PATTERN
    expected = bytearray()
    for at in range(0, len(pattern), 4):
        b, g, r, a = pattern[at:at + 4]
        expected += bytes((r * a // 255, g * a // 255, b * a // 255))  # premultiplied source
    expected = bytes(expected)
    work = tempfile.mkdtemp(prefix="metalwire-material-")
    out_dir = os.path.join(work, "frames")
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "1", "--shm", "", "--out", out_dir,
         "--png-every", "1", "--verbose"],
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
    ok = False
    try:
        require(listening.wait(60), "material host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the material host's port")
        gen = subprocess.run(
            [sys.executable, GEN, "--material", "--port", match.group(1)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        require(gen.returncode == 0,
                "material generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout,
                                                           gen.stderr))
        require(host.wait(timeout=60) == 0,
                "material host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            output = "\n".join(host_lines)
        require("material frame 1: ack status=ok" in gen.stdout,
                "the material frame was not acknowledged:\n%s" % gen.stdout)
        require("PBGRABsov_TmuaXfcn_IsrcCcl_Isup\" -> Vulkan pipeline (topology 3, texture 1, "
                "fetch 1)" in output,
                "the Tmua material pipeline was not specialised as a textured fetch draw:\n%s" %
                output)
        ppm_path = os.path.join(out_dir, "frame-000000.ppm")
        with open(ppm_path, "rb") as f:
            data = f.read()
        header = b"P6\n%d %d\n255\n" % (size, size)
        require(data.startswith(header), "%s: bad P6 header" % ppm_path)
        pixels = data[len(header):]
        require(pixels == expected,
                "%s: the material picture is not the support texture (first pixel %s, expected "
                "%s)" % (ppm_path, tuple(pixels[0:3]), tuple(expected[0:3])))
        require(any(byte != 255 for byte in pixels),
                "the material picture is still opaque white (the D3/D5 defect)")
        png_path = os.path.join(out_dir, "frame-000000.png")
        width, height, png_pixels = read_png_rgb(png_path)
        require((width, height) == (size, size), "%s: %dx%d" % (png_path, width, height))
        require(png_pixels == pixels, "%s differs from the PPM of the same frame" % png_path)
        print("PASS material: a Tmua (support-texture + fetch) draw samples the texture -- the "
              "picture is the support pattern, not opaque white (D3/D5 host semantics)")
        ok = True
    except TestFailure:
        with host_lock:
            print("--- material host stderr ---\n%s" % "\n".join(host_lines))
        raise
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)
        if ok:
            shutil.rmtree(work, ignore_errors=True)


def run_dual_buffer(env):
    """WP-4 D8: the two display buffers must end up with the same picture.

    CA presents through two surfaces that alternate; each frame repaints only the layers that
    changed with load action LOAD, and CA never copies one surface's picture into the other (boot
    wp4-v13: texture 52 carries the status bar and the Dock icons, 55 does not, so the panel
    flickers every other frame). The fixture paints the static layers into buffer 90 once and then
    draws only the clock into 91/90/91/90 with LOAD; every clock frame must show the static layers
    plus the clock. A host that starts a LOAD frame from that buffer's stale content shows the
    clock on black for buffer 91 -- the D8 black frame.
    """
    size = metalwire_gen.DUAL_SIZE
    static = bytearray()
    for at in range(0, len(metalwire_gen.DUAL_STATIC), 4):
        b, g, r, _a = metalwire_gen.DUAL_STATIC[at:at + 4]
        static += bytes((r, g, b))  # premultiplied by alpha 255

    def expected(with_clock, quarter=False):
        picture = bytearray(len(static))
        for y in range(size):
            for x in range(size):
                if not quarter or (x < 16 and y < 16):
                    at = (y * size + x) * 3
                    picture[at:at + 3] = static[at:at + 3]
        if with_clock:
            for y in range(8, 24):
                for x in range(8, 24):
                    at = (y * size + x) * 3
                    picture[at:at + 3] = b"\x00\xff\x00"
        return bytes(picture)

    work = tempfile.mkdtemp(prefix="metalwire-dual-")
    out_dir = os.path.join(work, "frames")
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "6", "--shm", "", "--out", out_dir,
         "--png-every", "1", "--verbose"],
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
    ok = False
    try:
        require(listening.wait(60), "dual-buffer host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the dual-buffer host's port")
        gen = subprocess.run(
            [sys.executable, GEN, "--dual-buffer", "--port", match.group(1)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        require(gen.returncode == 0,
                "dual-buffer generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout,
                                                               gen.stderr))
        require(host.wait(timeout=120) == 0,
                "dual-buffer host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        for index, with_clock, quarter, label in (
                (0, False, False, "full static into 90"),
                (1, False, True, "poor quarter into 91"),
                (2, True, False, "clock into 90 (source poor, must not overwrite)"),
                (3, True, False, "clock into 91 (source rich, fills the layers)"),
                (4, True, False, "clock into 90"), (5, True, False, "clock into 90 dont-care")):
            path = os.path.join(out_dir, "frame-%06d.ppm" % index)
            with open(path, "rb") as f:
                data = f.read()
            header = b"P6\n%d %d\n255\n" % (size, size)
            require(data.startswith(header), "%s: bad P6 header" % path)
            pixels = data[len(header):]
            want = expected(with_clock, quarter)
            require(pixels == want,
                    "%s (%s): the picture is not the retained static layers plus the clock "
                    "(first pixel %s, expected %s, non-zero %d)"
                    % (os.path.basename(path), label, tuple(pixels[0:3]), tuple(want[0:3]),
                       sum(1 for byte in pixels if byte)))
        print("PASS dual-buffer: a partial frame never overwrites a rich display buffer and a rich "
              "source fills the other one (no black/missing-layer frame, D8)")
        ok = True
    except TestFailure:
        with host_lock:
            print("--- dual-buffer host stderr ---\n%s" % "\n".join(host_lines))
        raise
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)
        if ok:
            shutil.rmtree(work, ignore_errors=True)


def run_rg8(env):
    """WP-4 t26: MTLPixelFormatRG8Unorm (30) has to cross the wire like the 8-bit formats.

    Boot wp4-v14 answered "texture 146: pixel format 30 is not implemented; its draws are skipped"
    (wire error 1) for CoreAnimation's two-channel support texture. This runs two frames: one
    presents a 256x2 RG8 picture (creation, the 2-byte-texel blit, the read-back CRC against the
    bytes the client sent and the PPM conversion), the other samples the same texture through a
    Timg pipeline into a BGRA target (the Vulkan R8G8_UNORM sampling and the PPM again). Both PPMs
    must be (R, G, 0) and the missing report must stay empty.
    """
    ramp = metalwire_gen.make_rg8_ramp()
    expected = b"".join(bytes((ramp[i], ramp[i + 1], 0)) for i in range(0, len(ramp), 2))
    work = tempfile.mkdtemp(prefix="metalwire-rg8-")
    out_dir = os.path.join(work, "frames")
    missing = os.path.join(work, "missing.txt")
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "2", "--shm", "", "--out", out_dir,
         "--png-every", "1", "--missing-report", missing, "--verbose"],
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
    ok = False
    try:
        require(listening.wait(60), "rg8 host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the rg8 host's port")
        gen = subprocess.run(
            [sys.executable, GEN, "--rg8", "--port", match.group(1)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        require(gen.returncode == 0,
                "rg8 generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout, gen.stderr))
        require(host.wait(timeout=120) == 0,
                "rg8 host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            output = "\n".join(host_lines)
        require("ack status=ok crc_ok=1" in gen.stdout,
                "the presented RG8 picture did not read back byte-identical:\n%s" % gen.stdout)
        require("rg8 frame 1: ack status=ok" in gen.stdout,
                "the RG8 sampling frame was not acknowledged:\n%s" % gen.stdout)
        require("texture 61: 256x2 format 30, 1024 bytes" in output,
                "the host did not create the RG8 texture:\n%s" % output)
        require("render frame 1: executed 1 of 1" in output,
                "the RG8 sampling draw did not execute:\n%s" % output)
        require("is not implemented" not in output and "is not on the host" not in output,
                "the host still rejects the format:\n%s" % output)
        with open(missing) as handle:
            report = handle.read()
        require("frames 2, pipelines 0, skipped draws 0" in report,
                "the missing report is not empty:\n%s" % report)
        for index in (0, 1):
            path = os.path.join(out_dir, "frame-%06d.ppm" % index)
            with open(path, "rb") as f:
                data = f.read()
            header = b"P6\n%d %d\n255\n" % (metalwire_gen.RG8_WIDTH, metalwire_gen.RG8_HEIGHT)
            require(data.startswith(header), "%s: bad P6 header" % path)
            pixels = data[len(header):]
            require(pixels == expected,
                    "%s: the picture is not (R, G, 0) of the RG8 texels (first pixel %s, "
                    "expected %s)" % (path, tuple(pixels[0:3]), tuple(expected[0:3])))
        png_path = os.path.join(out_dir, "frame-000001.png")
        width, height, png_pixels = read_png_rgb(png_path)
        require((width, height) == (metalwire_gen.RG8_WIDTH, metalwire_gen.RG8_HEIGHT),
                "%s: %dx%d" % (png_path, width, height))
        require(png_pixels == expected, "%s differs from the RG8 PPM" % png_path)
        print("PASS rg8: a 256x2 MTLPixelFormatRG8Unorm (30) texture is created, blitted, "
              "presented and sampled -- 0 skipped, missing report empty, both PPMs are (R, G, 0)")
        ok = True
    except TestFailure:
        with host_lock:
            print("--- rg8 host stderr ---\n%s" % "\n".join(host_lines))
        raise
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)
        if ok:
            shutil.rmtree(work, ignore_errors=True)


def _run_leak_host(env, frames, extra_env):
    """Spawn the host with the leak-check knobs; return (host stderr, exit code, generator text)."""
    work = tempfile.mkdtemp(prefix="metalwire-leak-")
    host_env = dict(env)
    host_env.update(extra_env)
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", str(frames), "--shm", ""],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, env=host_env)
    lines = []
    lock = threading.Lock()
    listening = threading.Event()

    def drain():
        for line in host.stderr:
            with lock:
                lines.append(line.rstrip("\n"))
            if "listening on" in line:
                listening.set()

    reader = threading.Thread(target=drain, daemon=True)
    reader.start()
    try:
        require(listening.wait(60), "leak host did not print a listening line in 60 s")
        with lock:
            match = None
            for line in lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the leak host's port")
        gen = subprocess.run([sys.executable, GEN, "--leak", "--leak-frames", str(frames),
                              "--port", match.group(1)] + (["--leak-fixed"]
                                                           if extra_env.get("_fixed") else []),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                             timeout=180)
        code = host.wait(timeout=120)
        reader.join(timeout=10)
        with lock:
            return "\n".join(lines), code, gen.stdout + gen.stderr
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)
        shutil.rmtree(work, ignore_errors=True)


def run_leak_check(env):
    """WP-4 4.1c: the host's live set stays bounded when the guest keeps creating buffers.

    Boot wp4-soak-fix-ctrl showed the host RSS growing 68 -> 719 MB in 25 minutes; the guest had
    created 2,135 per-frame 256 KiB buffers with a new id each time and the host never freed one
    (the wire has no destroy in the plug-in's path). The host now tracks the last frame that named
    a buffer and, past MW_MAX_BUFFER_BYTES, destroys the oldest ones the wire has not named for
    MW_BUFFER_MIN_AGE frames; MW_LEAK_CHECK=1 prints the live set every 60 frames and at exit.
    This runs 130 frames that each create a fresh buffer (the boot's shape) with a 128 KiB cap and
    a two-frame age, then 40 frames on one fixed buffer (the plug-in's pooled shape).
    """
    output, code, gen = _run_leak_host(env, 130, {"MW_LEAK_CHECK": "1",
                                                  "MW_MAX_BUFFER_BYTES": "131072",
                                                  "MW_BUFFER_MIN_AGE": "2"})
    require(code == 0, "leak host exited %s\n%s" % (code, output))
    require("leak: 130 frame(s) sent, 0 failure(s)" in gen,
            "the leak fixture did not send every frame:\n%s" % gen)
    leak_lines = [line for line in output.splitlines() if "leak-check (" in line]
    require(any("leak-check (periodic)" in line for line in leak_lines),
            "no periodic leak-check line in a 130-frame run:\n%s" % output)
    require(any("leak-check (exit)" in line for line in leak_lines),
            "no exit leak-check line:\n%s" % output)
    periodic = [re.search(r"buffers=(\d+) buffer_bytes=(\d+)", line) for line in leak_lines
                if "periodic" in line]
    require(all(match is not None for match in periodic) and periodic,
            "periodic leak-check lines have no buffer counters:\n%s" % output)
    for match in periodic:
        require(int(match.group(2)) <= 131072 + 65536,
                "live buffer bytes passed the cap between frames: %s" % match.group(0))
    exit_line = [line for line in leak_lines if "exit" in line][-1]
    match = re.search(r"frames=(\d+) images=(\d+) image_bytes=(\d+) buffers=(\d+) "
                      r"buffer_bytes=(\d+) reclaimed=(\d+) pending_replaces=(\d+).* "
                      r"rss_kib=(\d+)", exit_line)
    require(match is not None, "cannot parse the exit leak-check: %s" % exit_line)
    require(int(match.group(1)) == 130, "exit frames %s, expected 130" % match.group(1))
    require(int(match.group(2)) == 1, "the host holds %s images, expected the one texture"
            % match.group(2))
    require(int(match.group(4)) <= 3, "the live buffer count grew to %s (cap was 128 KiB)"
            % match.group(4))
    require(int(match.group(5)) <= 131072 + 65536, "live buffer bytes %s, cap 196,608"
            % match.group(5))
    require(int(match.group(6)) >= 100, "only %s buffers were reclaimed of 130 created"
            % match.group(6))
    require(int(match.group(7)) == 0, "pending replaceRegion entries left behind: %s"
            % match.group(7))
    require(int(match.group(8)) < 300000,
            "host RSS reached %s KiB in a 130-frame run" % match.group(8))

    output, code, gen = _run_leak_host(env, 40, {"MW_LEAK_CHECK": "1",
                                                 "MW_MAX_BUFFER_BYTES": "131072",
                                                 "MW_BUFFER_MIN_AGE": "2",
                                                 "_fixed": "1"})
    require(code == 0, "fixed-buffer host exited %s\n%s" % (code, output))
    require("leak: 40 frame(s) sent, 0 failure(s)" in gen,
            "the fixed-buffer fixture did not send every frame:\n%s" % gen)
    exit_line = [line for line in output.splitlines() if "leak-check (exit)" in line][-1]
    match = re.search(r"buffers=(\d+) buffer_bytes=(\d+) reclaimed=(\d+)", exit_line)
    require(match is not None, "cannot parse the fixed-buffer leak-check: %s" % exit_line)
    require((match.group(1), match.group(2), match.group(3)) == ("1", "65536", "0"),
            "a reused buffer id still churned the host live set: %s" % match.group(0))
    print("PASS leak-check: 130 fresh per-frame buffers are reclaimed past the 128 KiB cap (live "
          "set <= 3 buffers) and a reused id creates nothing; MW_LEAK_CHECK reports images, "
          "buffers, replaces and RSS")


def read_png_rgb(path):
    """The host writes PNGs with stored (uncompressed) deflate blocks and filter 0: decode them."""
    with open(path, "rb") as f:
        data = f.read()
    require(data.startswith(b"\x89PNG\r\n\x1a\n"), "%s: not a PNG" % path)
    at = 8
    idat = b""
    width = height = 0
    while at + 8 <= len(data):
        length = int.from_bytes(data[at:at + 4], "big")
        kind = data[at + 4:at + 8]
        chunk = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(chunk[0:4], "big")
            height = int.from_bytes(chunk[4:8], "big")
            require(chunk[8:10] == b"\x08\x02", "%s: not 8-bit truecolour" % path)
        elif kind == b"IDAT":
            idat += chunk
        at += 12 + length
    require(width > 0 and height > 0 and idat, "%s: missing IHDR/IDAT" % path)
    raw = zlib.decompress(idat)
    stride = 1 + width * 3
    require(len(raw) == stride * height, "%s: %d raw bytes for %dx%d" % (path, len(raw), width,
                                                                       height))
    out = bytearray()
    for y in range(height):
        require(raw[y * stride] == 0, "%s: row %d has filter %d" % (path, y, raw[y * stride]))
        out += raw[y * stride + 1:(y + 1) * stride]
    return width, height, bytes(out)


def run_orientation(env):
    """WP-4 1.1: CA's Metal NDC y-up / Vulkan y-down must not mirror the picture.

    Metal's NDC has y up and CA's mvp puts a layer's top at NDC y=+1; Vulkan's viewport maps NDC y
    down. The host compensates with a negative viewport height, so the numbered-row source has to
    come out with the same rows in the present read-back, the PPM/PNG files, the SHM picture and
    the IOSurface by-reference write-back. Frame 2 adds a framebuffer-fetch draw (the Bsov base
    function) that blends 50% over the destination: a wrong fetch origin would read the mirrored
    row. The same fixture against a host without the negative height comes out exactly flipped
    (verified while writing this), so the assertion bites.
    """
    source, frame1, frame2 = metalwire_gen.orientation_expected()
    size = metalwire_gen.ORIENT_SIZE
    require(frame1[:9] != frame1[-9:], "the orientation source is not asymmetric")
    work = tempfile.mkdtemp(prefix="metalwire-orient-")
    out_dir = os.path.join(work, "frames")
    ram_path = os.path.join(work, "ram.img")
    shm_path = os.path.join(work, "frame.shm")
    with open(ram_path, "wb") as f:
        f.write(b"\x00" * metalwire_gen.ORIENT_RAM_BYTES)
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "3", "--ram", ram_path,
         "--shm", shm_path, "--out", out_dir, "--png-every", "1"],
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
    ok = False

    def picture(path):
        return read_ppm(path, size, size)

    try:
        require(listening.wait(60), "orientation host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the orientation host's port")
        gen = subprocess.run(
            [sys.executable, GEN, "--orientation", "--port", match.group(1)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=180)
        require(gen.returncode == 0,
                "orientation generator exited %d\n%s\n%s" % (gen.returncode, gen.stdout,
                                                             gen.stderr))
        require("wire error" not in gen.stderr, "orientation generator saw a wire error:\n%s" %
                gen.stderr)
        require(host.wait(timeout=120) == 0,
                "orientation host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            output = "\n".join(host_lines)
        for seq in (1, 2):
            require("orientation frame %d: ack status=ok" % seq in gen.stdout,
                    "orientation frame %d was not acknowledged:\n%s" % (seq, gen.stdout))
        for name in ("com.apple.coreanimation.draw.PBGRA_TimgXfc_Isrc",
                     "com.apple.coreanimation.draw.PBGRABsov_TimgXfc_Isrc"):
            require("-> Vulkan pipeline" in output and name in output,
                    "the host did not build a Vulkan pipeline for %s:\n%s" % (name, output))
        require(output.count("picture written into the guest's real IOSurface and read back by "
                             "reference") >= 2,
                "the IOSurface write-back did not happen for both render frames:\n%s" % output)
        # 1. the blit path (no viewport) and the render copy draw: top row is the source top row.
        seed = picture(os.path.join(out_dir, "frame-000000.ppm"))
        require(seed == frame1, "the blit seed frame does not hold the source in RGB order")
        copied = picture(os.path.join(out_dir, "frame-000001.ppm"))
        require(copied == frame1,
                "the render frame is not upright: host row 0 must be the source row 0")
        blended = picture(os.path.join(out_dir, "frame-000002.ppm"))
        require(blended == frame2,
                "the framebuffer-fetch frame reads the wrong row (or blends wrongly)")
        # 2. PPM and PNG are the same picture.
        for index, expected in ((1, frame1), (2, frame2)):
            png_path = os.path.join(out_dir, "frame-%06d.png" % index)
            require(os.path.exists(png_path), "%s was not written" % png_path)
            width, height, pixels = read_png_rgb(png_path)
            require((width, height) == (size, size), "%s: %dx%d" % (png_path, width, height))
            require(pixels == expected, "%s differs from the PPM of the same frame" % png_path)
        # 3. The SHM picture and the guest surface hold the same last frame, same direction.
        with open(shm_path, "rb") as f:
            shm = f.read()
        require(len(shm) >= 40 + size * size * 4, "%s is %d bytes" % (shm_path, len(shm)))
        magic, _seq, shm_w, shm_h, shm_bytes, _pad, _ns = struct.unpack_from("<QQIIIIQ", shm, 0)
        require(magic == 0x57464C47 and (shm_w, shm_h, shm_bytes) == (size, size, size * size * 4),
                "%s: bad SHM header" % shm_path)
        expected_bgra = bytearray()
        for y in range(size):
            for x in range(size):
                r, g, b = frame2[(y * size + x) * 3:(y * size + x) * 3 + 3]
                expected_bgra += bytes((b, g, r, 255))
        expected_bgra = bytes(expected_bgra)
        require(shm[40:40 + len(expected_bgra)] == expected_bgra,
                "the SHM picture is not the last frame in the source's row order")
        with open(ram_path, "rb") as f:
            f.seek(metalwire_gen.ORIENT_RAM_OFFSET)
            surface = f.read(size * size * 4)
        require(surface == expected_bgra,
                "the IOSurface by-reference write-back is not the last frame in row order")
        print("PASS orientation: Metal y-up -> Vulkan y-down compensated; read-back, PPM/PNG, SHM "
              "and IOSurface all hold the source's rows (fetch draw included)")
        ok = True
    except TestFailure:
        with host_lock:
            print("--- orientation host stderr ---\n%s" % "\n".join(host_lines))
        raise
    finally:
        if host.poll() is None:
            host.kill()
        reader.join(timeout=5)
        if ok:
            shutil.rmtree(work, ignore_errors=True)


def _multi_client_setup(client, width, height):
    """Hello plus the object ids every guest starts from: buffer 1, texture 1."""
    client.send(metalwire_gen.MW_HELLO,
                struct.pack("<II32s", metalwire_gen.WIRE_VERSION, os.getpid(), b"metalwire_test"))
    client.send(metalwire_gen.MW_CREATE_BUFFER, struct.pack("<IIQ", 1, 0, width * height * 4))
    client.send(metalwire_gen.MW_CREATE_TEXTURE,
                struct.pack("<IIIIII", 1, width, height, 80, 0, 0))


def _multi_client_frame(client, picture, width, height, seq):
    """One blit frame from this client's own buffer 1 / texture 1; returns (ack, ack_seconds)."""
    crc = zlib.crc32(picture) & 0xFFFFFFFF
    row_bytes = width * 4
    client.send(metalwire_gen.MW_FRAME_BEGIN,
                struct.pack("<QQ", seq, time.monotonic_ns()))
    client.send(metalwire_gen.MW_UPLOAD_BUFFER,
                struct.pack("<IIII", 1, 0, len(picture), 0) + picture)
    client.send(metalwire_gen.MW_COPY_BUFFER_TO_TEXTURE,
                struct.pack("<IIQIIIIII", 1, 1, 0, row_bytes, len(picture), width, height, 0, 0))
    client.send(metalwire_gen.MW_PRESENT, struct.pack("<II", 1, 0))
    client.send(metalwire_gen.MW_FRAME_END,
                struct.pack("<QQIIII", seq, 0, crc, width, height, row_bytes))
    started = time.monotonic()
    ack = client.recv_ack()
    return ack, time.monotonic() - started


def run_multi_client(env):
    """Two guests at once: the first holds its connection after its frame, the second connects
    while it does and must get its own ack promptly.

    The host used to accept one client and serve it until it disconnected, so a poster extension
    that connected while backboardd held the wire waited for backboardd to go away -- longer than
    launchd gives an extension to check in. Both clients here also use the same object ids (buffer
    1, texture 1) with different pictures, so the acks only match if each client has its own
    objects.
    """
    width, height = 128, 128
    work = tempfile.mkdtemp(prefix="metalwire-multi-")
    out_dir = os.path.join(work, "frames")
    host = subprocess.Popen(
        [HOST, "--gpu", "0", "--port", "0", "--frames", "2", "--shm", "", "--out", out_dir],
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
    first = second = None
    ok = False
    try:
        require(listening.wait(60), "multi-client host did not print a listening line within 60 s")
        with host_lock:
            match = None
            for line in host_lines:
                match = re.search(r"listening on .*:(\d+)$", line)
                if match:
                    break
        require(match is not None, "cannot parse the multi-client host's port")
        port = int(match.group(1))

        picture_a = metalwire_gen.make_picture(width, height, 0, bgra=True)
        picture_b = metalwire_gen.make_picture(width, height, 1, bgra=True)
        first = metalwire_gen.Client("127.0.0.1", port, 10.0)
        _multi_client_setup(first, width, height)
        ack_a, _ = _multi_client_frame(first, picture_a, width, height, 0)
        require(ack_a[2] == metalwire_gen.MW_STATUS_OK and ack_a[7] == 1,
                "first client's frame was not acknowledged ok: %s" % (ack_a,))
        require(ack_a[6] == zlib.crc32(picture_a) & 0xFFFFFFFF,
                "first client's read-back CRC differs from its picture")

        # The first client stays connected. The second one connects now and must be served too.
        second = metalwire_gen.Client("127.0.0.1", port, 10.0)
        _multi_client_setup(second, width, height)
        ack_b, ack_seconds = _multi_client_frame(second, picture_b, width, height, 0)
        require(ack_b[2] == metalwire_gen.MW_STATUS_OK and ack_b[7] == 1,
                "second client's frame was not acknowledged ok: %s" % (ack_b,))
        require(ack_b[6] == zlib.crc32(picture_b) & 0xFFFFFFFF,
                "second client's read-back CRC differs from its picture (objects are shared?)")
        require(ack_seconds < 5.0,
                "the second client waited %.1f s for its ack while the first was connected" %
                ack_seconds)

        first.close()
        first = None
        second.close()
        second = None
        require(host.wait(timeout=120) == 0,
                "multi-client host exited %s\n%s" % (host.returncode, "\n".join(host_lines)))
        reader.join(timeout=10)
        with host_lock:
            output = "\n".join(host_lines)
        require("client connected (2 client(s))" in output,
                "the host did not accept the second connection:\n%s" % output)
        require(output.count("client disconnected after 1 frame(s)") == 2,
                "the host did not account one frame per client:\n%s" % output)
        print("PASS multi-client: two connections at once, each ack ok in %.3f s, "
              "same ids stayed apart" % ack_seconds)
        ok = True
    except TestFailure:
        if host.poll() is None:
            host.kill()
            host.wait()
        raise
    finally:
        for client in (first, second):
            if client is not None:
                client.close()
        reader.join(timeout=5)
        if ok:
            shutil.rmtree(work, ignore_errors=True)


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
        run_format115(env)
        run_rg8(env)
        run_blur_pipelines(env)
        run_labels(env)
        run_draw_dump(env)
        run_material(env)
        run_dual_buffer(env)
        run_leak_check(env)
        run_chain(env)
        run_orientation(env)
        run_multi_client(env)
        print("metalwire_test: PASS (lavapipe, %dx%d, %d frames, v4 fallback, format 115, rg8, "
              "blur, labels, draw-dump, material, dual-buffer, leak-check, chain, orientation, multi-client)"
              % (WIDTH, HEIGHT, FRAMES))
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

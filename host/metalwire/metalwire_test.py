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
        run_blur_pipelines(env)
        run_orientation(env)
        print("metalwire_test: PASS (lavapipe, %dx%d, %d frames, v4 fallback, format 115, blur, "
              "orientation)" % (WIDTH, HEIGHT, FRAMES))
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

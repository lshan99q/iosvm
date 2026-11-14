#!/usr/bin/env python3
"""Save what a running QEMU's *display console* shows -- for the virtio-gpu scanout of stage 23 A2 --
through QMP, as a PNG.

usage: gpushot.py [OUT.png] [--instance N | --qmp-port P] [--full] [--crop WxH] [--timeout S]

  OUT.png        where to write the picture (default: /tmp/gpuscanout.png)
  --instance N   which QEMU: QMP is 4445 + 10*N (default: QEMU_INSTANCE, else qemu/instance, else 0)
  --qmp-port P   the QMP port itself, when it is not the instance rule
  --full         keep the whole scanout (default: the phone-sized top left 1125x2436, like screendump.sh)
  --crop WxH     keep an explicit WxH corner instead
  --timeout S    how long to wait for QEMU to write the file (default 10)

What it does: connects to QMP, negotiates capabilities, runs `screendump`. QEMU 10 writes PNG itself
(`format=png`); older ones only write PPM, which this script converts (QEMU pads every row to a multiple
of four bytes, which is not what the format says, so the rows are rebuilt).

For A2 the interesting console is the one virtio-gpu drives; the firmware's scanout (1280x800) shows up
before the guest's SET_SCANOUT, so an all-black or firmware-looking picture is a working screenshot.

A console needs a display backend for `screendump` to have a surface: `-display none` works for ramfb
(the ramfb device updates the console itself), but a virtio-gpu scanout may need `-vnc 127.0.0.1:N`
(or `-display egl-headless`, when the container has GL). If QMP answers "no display device" or the
picture stays empty, boot with `VNC=1 VNC_PORT=<n>` and take the picture again -- that is the fallback
written down in docs/stage23-a2-verify.md.
"""
import json
import os
import socket
import struct
import sys
import time
import zlib


def instance():
    """Which QEMU: QEMU_INSTANCE, or the number in the file "instance" next to run-darwin.sh, or 0."""
    try:
        return int(os.environ.get('QEMU_INSTANCE') or
                   open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'qemu', 'instance')).read())
    except (OSError, ValueError):
        return 0


class Qmp:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.file = self.sock.makefile('rw')
        self.file.readline()                        # the greeting
        self.command('qmp_capabilities')

    def command(self, name, **arguments):
        self.file.write(json.dumps({'execute': name, 'arguments': arguments}) + '\n')
        self.file.flush()
        while True:
            line = self.file.readline()
            if not line:
                raise RuntimeError(f'{name}: QMP closed the connection')
            reply = json.loads(line)
            if 'return' in reply or 'error' in reply:
                if 'error' in reply:
                    raise RuntimeError(f'{name}: {reply["error"]["desc"]}')
                return reply['return']


def await_file(path, timeout):
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            if os.path.getsize(path) > 0:
                return True
        except OSError:
            pass
        time.sleep(0.1)
    return False


def ppm_to_png(raw, out, crop):
    magic, size, maxval, pix = raw.split(b'\n', 3)
    if magic.strip() != b'P6':
        raise SystemExit(f'gpushot.py: not a binary PPM ({magic!r})')
    w, h = map(int, size.split())
    stride = len(pix) // h                       # QEMU pads to 4 bytes; do not trust w*3
    cw, ch = crop if crop else (w, h)
    cw, ch = min(cw, w), min(ch, h)
    rows = b''.join(b'\0' + pix[y * stride:y * stride + cw * 3] for y in range(ch))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    with open(out, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' +
                chunk(b'IHDR', struct.pack('>IIBBBBB', cw, ch, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(rows, 6)) + chunk(b'IEND', b''))
    return cw, ch


def main(argv):
    out = '/tmp/gpuscanout.png'
    port = None
    crop = (1125, 2436)
    timeout = 10.0
    full = False
    args = list(argv[1:])
    while args:
        a = args.pop(0)
        if a == '--instance':
            port = 4445 + 10 * int(args.pop(0))
        elif a == '--qmp-port':
            port = int(args.pop(0))
        elif a == '--full':
            full = True
        elif a == '--crop':
            cw, ch = args.pop(0).lower().split('x')
            crop = (int(cw), int(ch))
        elif a == '--timeout':
            timeout = float(args.pop(0))
        elif a.startswith('-'):
            sys.exit(f'gpushot.py: unknown option {a}')
        else:
            out = a
    if port is None:
        port = 4445 + 10 * instance()
    if full:
        crop = None
    out = os.path.realpath(out)                     # QEMU writes it: absolute

    qmp = Qmp(port)
    try:
        qmp.command('screendump', filename=out, format='png')
    except RuntimeError as e:
        if 'format' not in str(e).lower() and 'parameter' not in str(e).lower():
            raise
        # older QEMU: PPM only
        ppm = out + '.ppm'
        qmp.command('screendump', filename=ppm)
        if not await_file(ppm, timeout):
            sys.exit(f'gpushot.py: QEMU accepted screendump but wrote no {ppm}')
        raw = open(ppm, 'rb').read()
        os.unlink(ppm)
        w, h = ppm_to_png(raw, out, crop)
        print(f'{out} {w}x{h} (from PPM)')
        return 0
    if not await_file(out, timeout):
        sys.exit(f'gpushot.py: QEMU accepted screendump but wrote no {out}')
    with open(out, 'rb') as f:
        w, h = struct.unpack('>II', f.read(24)[16:24])
    print(f'{out} {w}x{h} (whole scanout)')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))

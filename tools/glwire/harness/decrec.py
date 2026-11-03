#!/usr/bin/env python3
"""Read a frame's record stream (host_glwire GLWIRE_SAVE_RECORDS) and print its calls.

The wire: for every call, u32 opcode, u32 length, then the payload.  The opcodes are the order of
tools/glwire/spec.def; the payload is the call's own arguments in that order, then the guest's
answer for the calls that have one.  Pointers carry their own length (see the header of spec.def).

usage: decrec.py FILE.records [-v]
"""
import re
import struct
import sys

SPEC = "/home/lshan/DSH-desktop/iosvm/tools/glwire/spec.def"


def load_spec(path=SPEC):
    calls = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^(gl\w+)\(([^)]*)\)(?:\s*->\s*(\w+))?(\s*!local)?$", line)
        if not m:
            continue
        args = [a.strip() for a in m.group(2).split(",")] if m.group(2).strip() else []
        calls.append({"name": m.group(1), "args": args, "ret": m.group(3), "local": bool(m.group(4))})
    return calls


class Reader:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def take(self, n):
        out = self.data[self.at:self.at + n]
        self.at += n
        return out

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def i32(self):
        return struct.unpack("<i", self.take(4))[0]

    def f32(self):
        return struct.unpack("<f", self.take(4))[0]

    def u8(self):
        return self.take(1)[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def left(self):
        return len(self.data) - self.at


SCALAR = {"E": ("u32", 4), "U": ("u32", 4), "I": ("i32", 4), "S": ("i32", 4), "F": ("f32", 4), "B": ("u8", 1)}
TYPE_BYTES = {0x1401: 1, 0x1403: 2, 0x1405: 4, 0x1406: 4, 0x1404: 4, 0x8B51: 4}


def image_arg(r, verbose):
    kind = r.u8()
    if kind == 0:
        n = r.u32()
        return f"inline({n}B)"
    if kind == 1:
        n = r.u32()
        segs = [struct.unpack("<QII", r.take(16)) for _ in range(n)]
        total = sum(s[1] for s in segs)
        return f"refs({n} segs, {total}B)"
    return f"kind{kind}?"


def decode_call(r, call, verbose):
    out = []
    scalars = {}
    for pos, t in enumerate(call["args"]):
        if t in SCALAR:
            kind, size = SCALAR[t]
            value = getattr(r, kind)()
            out.append(value)
            scalars[pos] = value
            continue
        if t == "s":
            n = r.u32()
            raw = r.take(n)
            out.append(raw.split(b"\0")[0].decode("utf-8", "replace")[:40])
        elif t == "v":
            out.append("<vertex pointer>")
        elif t == "d":
            count = scalars.get(1, 0)
            elem = TYPE_BYTES.get(scalars.get(2, 0x1405), 2)
            n = r.u32()
            r.take(n)
            out.append(f"<{n}B indices>")
        elif t in ("x", "y"):
            out.append(image_arg(r, verbose))
        elif t == "z":
            n = r.u32()
            r.take(n)
            out.append(f"<{n}B compressed>")
        elif t in ("t", "r"):
            n = scalars.get(0, 0) * 4
            vals = struct.unpack("<" + "I" * (n // 4), r.take(n)) if n else ()
            out.append("[" + ",".join(str(v) for v in vals) + "]")
        elif t == "g":
            out.append("<state>")
        elif t == "q":
            out.append(hex(r.u64()))
        elif t == "3":
            n = scalars.get(1, 0) * 12
            r.take(n)
            out.append(f"<{n}B>")
        elif t == "4":
            n = scalars.get(1, 0) * 16
            r.take(n)
            out.append(f"<{n}B>")
        elif t == "m":
            n = scalars.get(1, 0) * 36
            r.take(n)
            out.append(f"<{n}B>")
        elif t == "M":
            n = scalars.get(1, 0) * 64
            r.take(n)
            out.append(f"<{n}B>")
        elif t == "V":
            n = scalars.get(1, 0)
            for _ in range(max(n, 0)):
                ln = r.u32()
                r.take(ln)
            out.append(f"<{n} strings>")
        else:
            out.append(f"<{t}?>")
    if call["ret"] in ("E", "U", "I") and r.left() >= 4:
        out.append(f"-> {r.u32()}")
    elif call["ret"] == "q" and r.left() >= 8:
        out.append(f"-> {hex(r.u64())}")
    return out


def main():
    path = sys.argv[1]
    verbose = "-v" in sys.argv
    spec = load_spec()
    data = open(path, "rb").read()
    at = 0
    counts = {}
    n = 0
    while at + 8 <= len(data):
        opcode, length = struct.unpack("<II", data[at:at + 8])
        payload = data[at + 8:at + 8 + length]
        at += 8 + length
        n += 1
        if opcode >= len(spec):
            print(f"{n:4d}  opcode {opcode} length {length}")
            continue
        call = spec[opcode]
        counts[call["name"]] = counts.get(call["name"], 0) + 1
        r = Reader(payload)
        try:
            args = decode_call(r, call, verbose)
        except Exception as exc:                      # noqa: BLE001 - a diagnostic, not a parser
            args = [f"<decode failed: {exc}, {len(payload)}B>"]
        text = ", ".join(str(a) for a in args)
        if verbose or call["name"] not in ("glBindTexture", "glTexParameteri", "glTexParameterf",
                                           "glUniform4f", "glUniform1i", "glUniform1f"):
            print(f"{n:4d}  {opcode:3d} {call['name']}({text})")
    print(f"\n{n} records, {len(data)} bytes")
    for name, count in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"  {count:5d}  {name}")


if __name__ == "__main__":
    main()

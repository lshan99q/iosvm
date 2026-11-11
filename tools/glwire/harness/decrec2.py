#!/usr/bin/env python3
"""Decode glwire records with the stage-21/22 12-byte header.

`decrec.py` assumes the old 8-byte header (opcode, length).  Current guests send
version 2 records: opcode, length, context (12 bytes).  Starting with a glFlush
(15, length 0, context 0), the old decoder reads the context as the next opcode
and reports unknown 367/7939.

Usage: decrec2.py FILE.records [FILE.records ...]
       decrec2.py --header 8|12 FILE...
The header size is auto-detected by parsing to the end with 8 and 12.
"""
import os
import re
import struct
import sys

SPEC = os.environ.get("GLWIRE_SPEC", os.path.join(os.path.dirname(__file__), "..", "spec.def"))


def load_spec(path):
    names = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        m = re.match(r"^([A-Za-z0-9_]+)\(", line)
        if m:
            names.append(m.group(1))
    return names


def parse(data, header):
    at = 0
    out = []
    while at + header <= len(data):
        op, ln = struct.unpack_from("<II", data, at)
        if at + header + ln > len(data):
            break
        out.append((op, data[at + header:at + header + ln]))
        at += header + ln
    return out, at


def score(records, end, size):
    return (end == size, len(records), end)


def index_bytes(typ, count, raw):
    if typ == 0x1401:
        return list(raw[:count])
    if typ == 0x1403:
        return list(struct.unpack_from("<%dH" % count, raw, 0))
    if typ == 0x1405:
        return list(struct.unpack_from("<%dI" % count, raw, 0))
    return []


def decode(path):
    data = open(path, "rb").read()
    best = None
    for header in (12, 8):
        recs, end = parse(data, header)
        s = score(recs, end, len(data))
        if best is None or s > best[0]:
            best = (s, header, recs)
    _, header, recs = best
    names = load_spec(SPEC)
    attrs = {}
    last_color = None
    last_viewport = None
    last_scissor = None
    clears = []
    draws = []
    print(f"== {path}: {len(data)} B, header {header}, {len(recs)} records")
    for i, (op, p) in enumerate(recs):
        name = names[op] if op < len(names) else f"op{op}"
        try:
            if name == "glClearColor":
                last_color = struct.unpack_from("<ffff", p, 0)
                print(f"{i:4d} clearColor {last_color}")
            elif name == "glClear":
                mask = struct.unpack_from("<I", p, 0)[0]
                clears.append((i, mask, last_color, last_viewport, last_scissor))
                print(f"{i:4d} CLEAR mask={mask:#x} color={last_color} viewport={last_viewport} scissor={last_scissor}")
            elif name in ("glViewport", "glScissor"):
                vals = struct.unpack_from("<iiii", p, 0)
                if name == "glViewport":
                    last_viewport = vals
                else:
                    last_scissor = vals
                print(f"{i:4d} {name} {vals}")
            elif name == "glBindFramebuffer":
                print(f"{i:4d} bindFramebuffer {struct.unpack_from('<II', p, 0)}")
            elif name == "glDrawArrays":
                mode, first, count = struct.unpack_from("<iii", p, 0)
                draws.append((i, "arrays", count, None, None, dict(attrs)))
                print(f"{i:4d} drawArrays mode={mode} first={first} count={count}")
            elif name == "glDrawElements":
                mode, count, typ = struct.unpack_from("<iii", p, 0)
                blen = struct.unpack_from("<I", p, 12)[0]
                raw = p[16:16 + blen]
                idx = index_bytes(typ, count, raw)
                draws.append((i, "elements", count, typ, idx, dict(attrs)))
                print(f"{i:4d} drawElements mode={mode} count={count} type={typ:#x} idx={idx[:12]}{'...' if len(idx)>12 else ''}")
            elif op == 200:
                idx, size, typ, norm, stride = struct.unpack_from("<IiIBi", p, 0)
                blen = struct.unpack_from("<I", p, 17)[0]
                attrs[idx] = (size, typ, norm, stride, p[21:21 + blen])
            elif name == "glVertexAttribPointer":
                idx, size, typ, norm, stride = struct.unpack_from("<IiIBi", p, 0)
                print(f"{i:4d} attribPointer idx={idx} size={size} type={typ:#x} stride={stride}")
        except struct.error:
            print(f"{i:4d} {name} <truncated>")
    print("-- summary")
    print(f"clears: {len(clears)}")
    for c in clears:
        print(f"  #{c[0]} mask={c[1]:#x} color={c[2]} viewport={c[3]} scissor={c[4]}")
    if attrs:
        size, typ, norm, stride, data = attrs[0]
        verts = len(data) // stride if stride else 0
        pts = []
        if typ == 0x1406 and size >= 2:
            for v in range(verts):
                pts.append(struct.unpack_from("<ff", data, v * stride))
        if pts:
            print(f"attr0 bbox: {min(x for x,y in pts)},{min(y for x,y in pts)} .. {max(x for x,y in pts)},{max(y for x,y in pts)} ({len(pts)} verts)")
    for d in draws:
        print(f"  draw #{d[0]} {d[1]} count={d[2]} idx={str(d[4])[:80]}")
        a = d[5].get(0)
        if a:
            size, typ, norm, stride, data = a
            if typ == 0x1406 and size >= 2 and stride:
                pts = [struct.unpack_from("<ff", data, v*stride) for v in range(len(data)//stride)]
                if pts:
                    print(f"    attr0 bbox {min(x for x,y in pts)},{min(y for x,y in pts)} .. {max(x for x,y in pts)},{max(y for x,y in pts)}")


if __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["--header"]:
        # kept for compatibility; auto-detection still validates
        args = args[2:]
    if not args:
        sys.exit(__doc__)
    for path in args:
        decode(path)

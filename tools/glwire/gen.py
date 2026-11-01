#!/usr/bin/env python3
"""Generate both halves of the GL wire from spec.def.

One description, two programs that cannot drift apart: the guest's recorder that appends each call
to a byte buffer, and the host's dispatcher that reads the buffer and makes the same call on the
real GL. The format is deliberately dumb -- a little-endian length-prefixed record per call:

    u32 opcode, u32 payload bytes, payload

with integers as they are, floats as their bits, and pointers as a u32 byte count followed by the
bytes. A record the host does not understand is a bug on the guest side, so it stops loudly.
"""
import re
import sys

CTYPES = {
    "E": ("GLenum", "u32"),
    "I": ("GLint", "i32"),
    "F": ("GLfloat", "f32"),
    "U": ("GLuint", "u32"),
    "S": ("GLsizei", "i32"),
    "B": ("GLboolean", "u8"),
}


def parse(path):
    calls = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.match(r"(\w+)\(([^)]*)\)(\s*->\s*(\w+))?", line)
        if not match:
            raise SystemExit("bad spec line: %s" % line)
        name, args, _, returns = match.groups()
        types = [t.strip() for t in args.split(",") if t.strip()]
        calls.append((name, types, returns))
    return calls


def c_params(types):
    out = []
    for index, kind in enumerate(types):
        if kind in ("P", "N"):
            out.append("const void *a%d" % index)
            out.append("GLsizei n%d" % index)
        else:
            ctype = CTYPES[kind][0]
            out.append("%s a%d" % (ctype, index))
    return ", ".join(out) if out else "void"


def emit_encode(calls):
    lines = []
    for opcode, (name, types, returns) in enumerate(calls):
        params = c_params(types)
        body = []
        for index, kind in enumerate(types):
            if kind in ("P", "N"):
                body.append("\twire_bytes(&w, a%d, n%d);" % (index, index))
            else:
                body.append("\twire_%s(&w, a%d);" % (CTYPES[kind][1], index))
        if returns:
            lines.append("/* %s returns %s: the caller reads it back after a flush */" % (name, returns))
        lines.append("void gw_%s(%s)\n{\n\twire_open(&w, %d);\n%s\n\twire_close(&w);\n}\n"
                     % (name, params, opcode, "\n".join(body)))
    return "\n".join(lines)


def emit_dispatch(calls):
    lines = []
    for opcode, (name, types, returns) in enumerate(calls):
        decl = []
        args = []
        pre = []
        post = []
        for index, kind in enumerate(types):
            if kind in ("P", "N"):
                decl.append("\t\tGLsizei n%d = 0;" % index)
                decl.append("\t\tvoid *p%d = wire_take(r, &n%d);" % (index, index))
                args.append("p%d" % index)
                post.append("\t\tfree(p%d);" % index)
            else:
                ctype = CTYPES[kind][0]
                decl.append("\t\t%s a%d = wire_get_%s(r);" % (ctype, index, CTYPES[kind][1]))
                args.append("a%d" % index)
        call = "%s(%s)" % (name, ", ".join(args))
        if returns:
            call = "(void)" + call
        lines.append("\tcase %d: {\n%s\n\t\t%s;\n%s\t\tbreak;\n\t}\n"
                     % (opcode, "\n".join(decl) if decl else "\t\t/* no arguments */",
                        call, "\n".join(post) + ("\n" if post else "")))
    return "\n".join(lines)


def emit_slots(calls):
    """Trampolines that fit the EAGL dispatch table: every entry is called as
    entry(table[0], arguments...), so each one takes the context first and ignores it."""
    lines = []
    for opcode, (name, types, returns) in enumerate(calls):
        if returns:
            continue            # answers a value: it stays local (see emit_slots)
        params = []
        args = []
        for index, kind in enumerate(types):
            if kind in ("P", "N"):
                params.append("const void *a%d" % index)
                params.append("GLsizei n%d" % index)
                args.append("a%d" % index)
                args.append("n%d" % index)
            else:
                ctype = CTYPES[kind][0]
                params.append("%s a%d" % (ctype, index))
                args.append("a%d" % index)
        ret = "void" if not returns else (CTYPES.get(returns, (returns, ""))[0] if returns in CTYPES else "void *")
        signature = "void *context" + (", " + ", ".join(params) if params else "")
        if returns:
            # A call that returns a value cannot be answered by a recorder: the answer has to come
            # from somewhere local. Object names (shaders, programs, syncs) additionally have to
            # match the host's, which needs the name travelling with the call. Until that is done
            # these stay local: the local implementation runs and the host is not told.
            lines.append("static %s\nremote_%s(%s)\n{\n\t/* still local: %s() answers a value */\n"
                         "\treturn my_%s(%s);\n}\n"
                         % (ret, name, signature, name, name,
                            ", ".join("a%d" % index for index in range(len(types)))))
        else:
            # Record it for the host *and* still do it here: the local renderer keeps working,
            # which is what lets the host's handling of real CoreAnimation traffic be measured
            # before the handover is complete.
            lines.append("static %s\nremote_%s(%s)\n{\n\t(void)context;\n\tgw_%s(%s);\n}\n"
                         % (ret, name, signature, name, ", ".join(args)))
    return "\n".join(lines)


def emit_table(calls, slots_path):
    """Point the EAGL table's slots for these calls at the remote trampolines.

    eagl_slots.txt gives each entry's byte offset; the table is filled as table[offset / 8], so the
    override is the same indexing with a different function."""
    offsets = {}
    for line in open(slots_path):
        parts = line.split()
        if len(parts) >= 3 and parts[0].startswith("0x"):
            offsets[parts[2]] = int(parts[0], 16)
    lines = ["static void", "eagl_fill_table_remote(void **table)", "{"]
    missing = []
    for name, types, returns in calls:
        if returns:
            continue            # stays local: a recorder cannot answer a value
        if name not in offsets:
            missing.append(name)
            continue
        lines.append("\ttable[%#x / 8] = (void *)remote_%s;" % (offsets[name], name))
    lines.append("}")
    if missing:
        lines.append("/* no slot offset for: %s */" % " ".join(missing))
    return "\n".join(lines)


def main():
    calls = parse(sys.argv[1])
    mode = sys.argv[2]
    if mode == "table":
        print(emit_table(calls, sys.argv[3]))
    elif mode == "slots":
        print(emit_slots(calls))
    elif mode == "encode":
        print(emit_encode(calls))
    else:
        print("switch (opcode) {\n%s\n\tdefault:\n\t\tfprintf(stderr, \"glwire: opcode %%u\\n\", opcode);"
              "\n\t\treturn -1;\n\t}" % emit_dispatch(calls))


main()

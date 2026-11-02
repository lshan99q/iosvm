#!/usr/bin/env python3
"""Generate both halves of the GL wire from spec.def.

One description, two programs that cannot drift apart: the guest's recorder that appends each call
to a byte buffer, and the host's dispatcher that reads the buffer and makes the same call on the
real GL. The format is deliberately dumb -- a little-endian length-prefixed record per call:

    u32 opcode, u32 payload length, payload

with integers as they are, floats as their bits, and pointer arguments as a u32 byte count followed
by the bytes. A record the host does not understand is a bug on the guest side, so it stops loudly.

The argument list in spec.def is the real GL signature (a trampoline is called with exactly the
arguments the GL function takes), and each pointer type says how its byte count is worked out.
Calls that return a value carry it at the end of the payload: the guest's local call already
answered it, and the host uses it to learn the guest's object names. names.h on the host keeps the
guest -> host maps those names need.
"""
import re
import sys

SCALAR_C = {
    "E": "GLenum",
    "I": "GLint",
    "F": "GLfloat",
    "U": "GLuint",
    "S": "GLsizei",
    "B": "GLboolean",
}
SCALAR_WIRE = {
    "E": "u32",
    "I": "i32",
    "F": "f32",
    "U": "u32",
    "S": "i32",
    "B": "u8",
}
RETURN_WIRE = {"E": "u32", "U": "u32", "I": "i32", "q": "u64"}

POINTER_C = {
    "s": "const GLchar *",
    "v": "const void *",
    "d": "const void *",
    "x": "const void *",
    "y": "const void *",
    "z": "const void *",
    "t": "const GLuint *",
    "r": "const GLuint *",
    "g": "GLint *",
    "q": "void *",
    "3": "const GLfloat *",
    "4": "const GLfloat *",
    "m": "const GLfloat *",
    "M": "const GLfloat *",
}

# Calls whose scalar arguments are object names and have to be translated before the host call.
TEX_ARG = {"glBindTexture": 1, "glFramebufferTexture2D": 3}
FB_ARG = {"glBindFramebuffer": 1}
PROG_ARG = {
    "glAttachShader": 0,
    "glUseProgram": 0,
    "glBindAttribLocation": 0,
    "glGetUniformLocation": 0,
    "glDeleteProgram": 0,
}
SHADER_ARG = {"glAttachShader": 1, "glCompileShader": 0}
UNIFORM_CALLS = {
    "glUniform1f", "glUniform4f", "glUniform1i", "glUniform3fv", "glUniform4fv",
    "glUniformMatrix3fv", "glUniformMatrix4fv",
}


class Call:
    def __init__(self, name, args, returns):
        self.name = name
        self.args = args
        self.returns = returns
        # C parameter layout: token index -> first of its C parameters
        self.first = []
        self.params = []
        at = 0
        for token in args:
            self.first.append(at)
            if token == "V":
                self.params.append(("const GLchar *const *", "a%d" % at))
                at += 1
                self.params.append(("const GLint *", "a%d" % at))
                at += 1
            else:
                self.params.append((ctype(token), "a%d" % at))
                at += 1

    def a(self, token_index):
        return "a%d" % self.first[token_index]

    def b(self, token_index):
        return "a%d" % (self.first[token_index] + 1)

    def ctypes(self):
        return ", ".join(type_ for type_, _ in self.params)

    def cparams(self):
        return ", ".join("%s %s" % (type_, name) for type_, name in self.params)

    def ret_ctype(self):
        if self.returns is None:
            return "void"
        if self.returns in SCALAR_C:
            return SCALAR_C[self.returns]
        return "void *"


def ctype(token):
    if token in SCALAR_C:
        return SCALAR_C[token]
    if token in POINTER_C:
        return POINTER_C[token]
    raise SystemExit("bad spec type: %s" % token)


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
        tokens = [t.strip() for t in args.split(",") if t.strip()]
        calls.append(Call(name, tokens, returns))
    return calls


def encode_pointer(call, index):
    """The body line(s) that put pointer token `index` on the wire."""
    token = call.args[index]
    if token in SCALAR_WIRE:
        return ["\twire_%s(&w, %s);" % (SCALAR_WIRE[token], call.a(index))]
    if token == "s":
        return ["\twire_string(&w, %s);" % call.a(index)]
    if token in ("v", "g"):
        return []
    if token == "q":
        return ["\twire_u64(&w, (uint64_t)(uintptr_t)%s);" % call.a(index)]
    if token in ("t", "r"):
        return ["\twire_bytes(&w, %s, %s > 0 ? (GLsizei)(%s * 4) : 0);" %
                (call.a(index), call.a(0), call.a(0))]
    if token == "d":
        return ["\twire_bytes(&w, %s, wire_index_bytes(%s, %s));" %
                (call.a(index), call.a(1), call.a(2))]
    if token == "x":
        return ["\twire_bytes(&w, %s, wire_image_bytes(%s, %s, %s, %s));" %
                (call.a(index), call.a(3), call.a(4), call.a(6), call.a(7))]
    if token == "y":
        return ["\twire_bytes(&w, %s, wire_image_bytes(%s, %s, %s, %s));" %
                (call.a(index), call.a(4), call.a(5), call.a(6), call.a(7))]
    if token == "z":
        return ["\twire_bytes(&w, %s, %s > 0 ? %s : 0);" %
                (call.a(index), call.a(6), call.a(6))]
    if token == "3":
        return ["\twire_bytes(&w, %s, %s > 0 ? (GLsizei)(%s * 12) : 0);" %
                (call.a(index), call.a(1), call.a(1))]
    if token == "4":
        return ["\twire_bytes(&w, %s, %s > 0 ? (GLsizei)(%s * 16) : 0);" %
                (call.a(index), call.a(1), call.a(1))]
    if token == "m":
        return ["\twire_bytes(&w, %s, %s > 0 ? (GLsizei)(%s * 36) : 0);" %
                (call.a(index), call.a(1), call.a(1))]
    if token == "M":
        return ["\twire_bytes(&w, %s, %s > 0 ? (GLsizei)(%s * 64) : 0);" %
                (call.a(index), call.a(1), call.a(1))]
    if token == "V":
        return [
            "\tfor (GLsizei i = 0; i < %s; i++) {" % call.a(1),
            "\t\tconst GLchar *text = %s != NULL ? %s[i] : NULL;" % (call.a(2), call.a(2)),
            "\t\tGLint length = %s != NULL ? %s[i] : -1;" % (call.b(2), call.b(2)),
            "\t\tif (text == NULL) { wire_u32(&w, 0); continue; }",
            "\t\tif (length < 0) { length = (GLint)strlen(text); }",
            "\t\twire_u32(&w, (uint32_t)length);",
            "\t\tif (length > 0) { wire_put(&w, text, (size_t)length); }",
            "\t}",
        ]
    raise SystemExit("cannot encode %s %s" % (call.name, token))


def emit_encode(calls):
    out = []
    for opcode, call in enumerate(calls):
        params = call.cparams()
        result = ("%s result" % call.ret_ctype()) if call.returns else ""
        all_params = params + (", " if params and result else "") + result
        lines = []
        for index in range(len(call.args)):
            lines += encode_pointer(call, index)
        if call.returns == "q":
            lines.append("\twire_u64(&w, (uint64_t)(uintptr_t)result);")
        elif call.returns in RETURN_WIRE:
            lines.append("\twire_%s(&w, result);" % RETURN_WIRE[call.returns])
        out.append("/* %s%s */" % (call.name, " -> " + call.returns if call.returns else ""))
        out.append("void gw_%s(%s)\n{\n\twire_open(&w, %d);\n%s\n\twire_close(&w);\n}\n" %
                   (call.name, all_params, opcode, "\n".join(lines)))
    return "\n".join(out)


def hook_lines(call):
    if call.name == "glVertexAttribPointer":
        return ["\tremote_note_vertex_pointer(%s, %s, %s, %s, %s, %s);" %
                (call.a(0), call.a(1), call.a(2), call.a(3), call.a(4), call.a(5))]
    if call.name == "glEnableVertexAttribArray":
        return ["\tremote_note_attrib_enabled(%s, 1);" % call.a(0)]
    if call.name == "glDisableVertexAttribArray":
        return ["\tremote_note_attrib_enabled(%s, 0);" % call.a(0)]
    if call.name == "glPixelStorei":
        return ["\tremote_note_pixel_store(%s, %s);" % (call.a(0), call.a(1))]
    if call.name == "glDrawArrays":
        return ["\tremote_emit_vertex_data(%s, %s, NULL, 0);" % (call.a(1), call.a(2))]
    if call.name == "glDrawElements":
        return ["\tremote_emit_vertex_data(0, %s, %s, %s);" % (call.a(1), call.a(3), call.a(2))]
    return []


DEDUPE = ("glTexImage2D", "glTexSubImage2D")

def skip_if_identical(name, types):
    """For the two calls that carry a whole layer every frame.

    CoreAnimation refills its texture from an IOSurface whether or not the picture changed, and the
    pixels travel inside the record -- 4.8 MB twice a frame, which cost 4.47 s a frame over the
    socket. Comparing the bytes here and dropping the record when nothing changed collapses that
    stream to the calls that actually move pixels. A cheap sampled hash: 1 in 64 bytes, which is
    plenty for "is this the same picture", and it runs in this (already slow) process rather than on
    the wire."""
    last = max((i for i, kind in enumerate(types) if kind in ("P", "N")), default=-1)
    if name not in DEDUPE or last < 0:
        return None
    return ("""	if (a%(i)d != NULL && n%(i)d > 0) {
		static unsigned long long last_%(name)s;
		const unsigned char *bytes = a%(i)d;
		unsigned long long hash = 1469598103934665603ULL;

		for (GLsizei at = 0; at < n%(i)d; at += 64) {
			hash = (hash ^ bytes[at]) * 1099511628211ULL;
		}
		hash ^= (unsigned long long)n%(i)d;
		if (hash == last_%(name)s) {
			return;         /* the same picture as last frame: nothing to send */
		}
		last_%(name)s = hash;
	}
""" % {"i": last, "name": name})


def emit_slots(calls):
    """Trampolines that fit the EAGL dispatch table: every entry is called as
    entry(table[0], arguments...), so each one takes the context first and then exactly the GL
    signature -- no invented size parameter.

    Every call is recorded for the host *and* still run here, through the entry the table held
    before it was overridden (local_entry[]). That keeps eagl.c's own wrappers and bookkeeping in
    the local path, which is what -attachImage: needs.
    """
    out = []
    for index, call in enumerate(calls):
        ret = call.ret_ctype()
        body = []
        args = ", ".join(name for _, name in call.params)
        local_type = "void *" if not call.params else "void *, " + call.ctypes()
        local_args = "context" if not call.params else "context, " + args
        if ret == "void":
            body.append("\tif (local_entry[%d] != NULL) {" % index)
            body.append("\t\t((void (*)(%s))local_entry[%d])(%s);" %
                        (local_type, index, local_args))
            body.append("\t}")
        else:
            body.append("\t%s result = (%s)0;" % (ret, ret))
            body.append("\tif (local_entry[%d] != NULL) {" % index)
            body.append("\t\tresult = ((%s (*)(%s))local_entry[%d])(%s);" %
                        (ret, local_type, index, local_args))
            body.append("\t}")
        body += hook_lines(call)
        call_args = args + ((", " if args else "") + "result") if call.returns else args
        body.append("\tgw_%s(%s);" % (call.name, call_args))
        if call.returns:
            body.append("\treturn result;")
        out.append("static %s\nremote_%s(void *context%s%s)\n{\n%s\n}\n" %
                   (ret, call.name,
                    ", " + call.cparams() if call.params else "",
                    "", "\n".join(body)))
    return "\n".join(out)


def emit_table(calls, slots_path):
    """Point every EAGL slot at its trampoline, keeping the entry it held before (the local call)."""
    offsets = {}
    for line in open(slots_path):
        parts = line.split()
        if len(parts) >= 3 and parts[0].startswith("0x"):
            offsets[parts[2]] = int(parts[0], 16)
    lines = ["/* The entry each slot held before it was pointed at the recorder: the local call that",
             " * still has to run. */"]
    for call in calls:
        lines.append("static %s remote_%s(void *context%s);" %
                     (call.ret_ctype(), call.name,
                      "" if not call.params else ", " + call.cparams()))
    lines += ["", "static void *local_entry[%d];" % len(calls), "",
              "static void", "eagl_fill_table_remote(void **table)", "{"]
    missing = []
    for index, call in enumerate(calls):
        if call.name not in offsets:
            missing.append(call.name)
            continue
        lines.append("\tlocal_entry[%d] = table[%#x / 8];" % (index, offsets[call.name]))
        lines.append("\ttable[%#x / 8] = (void *)remote_%s;" % (offsets[call.name], call.name))
    lines.append("}")
    if missing:
        lines.append("/* no slot offset for: %s */" % " ".join(missing))
    return "\n".join(lines)


# --------------------------------------------------------------------------------------------------
# the host half


def read_arguments(call):
    out = []
    for index, token in enumerate(call.args):
        name = "a%d" % call.first[index]
        if token in SCALAR_WIRE:
            out.append("\t\t%s %s = wire_get_%s(r);" %
                       (ctype(token), name, SCALAR_WIRE[token]))
        elif token == "s":
            out.append("\t\tchar *p%d = wire_take_string(r);" % index)
        elif token == "q":
            out.append("\t\tuint64_t v%d = wire_get_u64(r);" % index)
        elif token in ("t", "r", "d", "x", "y", "z", "3", "4", "m", "M"):
            out.append("\t\tGLsizei n%d = 0;" % index)
            out.append("\t\tvoid *p%d = wire_take(r, &n%d);" % (index, index))
        elif token == "g":
            out.append("\t\tGLint g%d[16];" % index)
        elif token == "V":
            out += [
                "\t\tGLchar **ss = NULL;",
                "\t\tif (a1 > 0) {",
                "\t\t\tss = calloc((size_t)a1, sizeof(*ss));",
                "\t\t\tfor (GLsizei i = 0; i < a1 && ss != NULL; i++) {",
                "\t\t\t\tuint32_t len = wire_get_u32(r);",
                "\t\t\t\tss[i] = malloc((size_t)len + 1);",
                "\t\t\t\tif (ss[i] != NULL) {",
                "\t\t\t\t\tif (r->at + len <= r->capacity) { memcpy(ss[i], r->base + r->at, len); }",
                "\t\t\t\t\tr->at += len;",
                "\t\t\t\t\tss[i][len] = '\\0';",
                "\t\t\t\t}",
                "\t\t\t}",
                "\t\t}",
            ]
        elif token == "v":
            pass
        else:
            raise SystemExit("cannot read %s %s" % (call.name, token))
    return out


def free_arguments(call):
    lines = []
    for index, token in enumerate(call.args):
        if token in ("s", "t", "r", "d", "x", "y", "z", "3", "4", "m", "M"):
            lines.append("\t\tfree(p%d);" % index)
    if "V" in call.args:
        lines += [
            "\t\tif (ss != NULL) {",
            "\t\t\tfor (GLsizei i = 0; i < a1; i++) { free(ss[i]); }",
            "\t\t\tfree(ss);",
            "\t\t}",
        ]
    return lines


def mapped_scalar(call, index):
    name = "a%d" % call.first[index]
    if TEX_ARG.get(call.name) == index:
        return "names_texture(g, %s)" % name
    if FB_ARG.get(call.name) == index:
        return "names_framebuffer(g, %s)" % name
    if PROG_ARG.get(call.name) == index:
        return "names_program(g, %s)" % name
    if SHADER_ARG.get(call.name) == index:
        return "names_shader(g, %s)" % name
    if call.name in UNIFORM_CALLS and index == 0:
        return "names_uniform(g, %s)" % name
    return name


def host_call_args(call):
    args = []
    for index, token in enumerate(call.args):
        if token in SCALAR_C:
            args.append(mapped_scalar(call, index))
        elif token == "s":
            args.append("p%d" % index)
        elif token == "g":
            args.append("g%d" % index)
        elif token in ("t", "r", "d", "x", "y", "z", "3", "4", "m", "M"):
            args.append("p%d" % index)
        elif token == "q":
            args.append("(GLsync)(uintptr_t)v%d" % index)
        elif token == "v":
            args.append("NULL")
        elif token == "V":
            args.append("(const GLchar *const *)ss, NULL")
    return ", ".join(args)


def emit_dispatch(calls):
    out = ["switch (opcode) {"]
    for opcode, call in enumerate(calls):
        out.append("\tcase %d: {" % opcode)
        out.append("\t\t/* %s */" % call.name)
        out += read_arguments(call)
        ret = call.returns
        guest = None
        if ret in RETURN_WIRE:
            guest = "guest_result"
            result_ctype = "uint64_t" if ret == "q" else SCALAR_C[ret]
            out.append("\t\t%s guest_result = wire_get_%s(r);" %
                       (result_ctype, RETURN_WIRE[ret]))
        name = call.name
        if name in ("glGenTextures", "glGenFramebuffers"):
            kind = "texture" if name == "glGenTextures" else "framebuffer"
            out += [
                "\t\tGLuint *host_names = a0 > 0 ? calloc((size_t)a0, sizeof(GLuint)) : NULL;",
                "\t\t%s(a0, host_names);" % name,
                "\t\tif (host_names != NULL && p1 != NULL) {",
                "\t\t\tfor (GLsizei i = 0; i < a0; i++) {",
                "\t\t\t\tnames_%s_put%s(g, ((GLuint *)p1)[i], host_names[i]);" %
                (kind, "_new" if kind == "texture" else ""),
                "\t\t\t}",
                "\t\t}",
                "\t\tfree(host_names);",
                "\t\tfree(p1);",
            ]
        elif name in ("glDeleteTextures", "glDeleteFramebuffers"):
            kind = "texture" if name == "glDeleteTextures" else "framebuffer"
            out += [
                "\t\tif (p1 != NULL) {",
                "\t\t\tfor (GLsizei i = 0; i < a0; i++) {",
                "\t\t\t\t((GLuint *)p1)[i] = (GLuint)names_%s_take(g, ((GLuint *)p1)[i]);" % kind,
                "\t\t\t}",
                "\t\t}",
                "\t\t%s(a0, (const GLuint *)p1);" % name,
                "\t\tfree(p1);",
            ]
        elif name == "glCreateShader":
            out += [
                "\t\tGLuint host = glCreateShader(a0);",
                "\t\tnames_shader_put(g, (uint32_t)guest_result, host);",
            ]
        elif name == "glCreateProgram":
            out += [
                "\t\tGLuint host = glCreateProgram();",
                "\t\tnames_program_put(g, (uint32_t)guest_result, host);",
            ]
        elif name == "glGetUniformLocation":
            out += [
                "\t\tGLuint host_program = names_program(g, a0);",
                "\t\tGLint host_location = host_program != 0 ? glGetUniformLocation(host_program, p1) : -1;",
                "\t\tnames_uniform_put(g, (uint32_t)a0, (GLint)guest_result, host_location);",
                "\t\tfree(p1);",
            ]
        elif name == "glShaderSource":
            out += [
                "\t\tGLuint host_shader = names_shader(g, a0);",
                "\t\tnames_shader_source(host_shader, a1, (const GLchar *const *)ss, NULL);",
            ] + free_arguments(call)
        elif name == "glFenceSync":
            out += [
                "\t\tvoid *host_sync = (void *)glFenceSync(a0, a1);",
                "\t\tnames_sync_put(g, (uint64_t)guest_result, (uint64_t)(uintptr_t)host_sync);",
            ]
        elif name == "glDeleteSync":
            out += ["\t\tglDeleteSync((GLsync)(uintptr_t)names_sync_take(g, v0));"]
        elif name == "glClientWaitSync":
            out += [
                "\t\tGLsync host_sync = (GLsync)(uintptr_t)names_sync(g, v0);",
                "\t\t(void)glClientWaitSync(host_sync, a1, a2);",
                "\t\t(void)guest_result;",
            ]
        elif name == "glGetError":
            out += ["\t\t(void)glGetError();", "\t\t(void)guest_result;"]
        elif name == "glGetString":
            out += ["\t\t(void)glGetString(a0);"]
        elif name == "glGetIntegerv":
            out += ["\t\tglGetIntegerv(a0, g%d);" % (len(call.args) - 1)]
        elif name == "glVertexAttribPointer":
            out += ["\t\tnames_vertex_pointer(g, a0, a1, a2, a3, a4);"]
        elif name == "glEnableVertexAttribArray":
            out += ["\t\tglEnableVertexAttribArray(a0);",
                    "\t\tnames_attrib_enabled(g, a0, 1);"]
        elif name == "glDisableVertexAttribArray":
            out += ["\t\tglDisableVertexAttribArray(a0);",
                    "\t\tnames_attrib_enabled(g, a0, 0);"]
        elif name == "glDeleteProgram":
            out += [
                "\t\tGLuint host_program = (GLuint)names_program_take(g, a0);",
                "\t\tglDeleteProgram(host_program);",
            ]
        elif name == "glUseProgram":
            out += [
                "\t\tGLuint host_program = names_program(g, a0);",
                "\t\tglUseProgram(host_program);",
                "\t\tg->current_program = (uint32_t)a0;",
                "\t\tg->current_host_program = host_program;",
            ]
        elif name == "glCompileShader":
            out += [
                "\t\tGLuint host_shader = names_shader(g, a0);",
                "\t\tglCompileShader(host_shader);",
                "\t\tif (host_shader != 0) {",
                "\t\t\tGLint good = 0;",
                "\t\t\tglGetShaderiv(host_shader, GL_COMPILE_STATUS, &good);",
                "\t\t\tif (!good) {",
                "\t\t\t\tchar said[1024] = \"\";",
                "\t\t\t\tglGetShaderInfoLog(host_shader, sizeof(said), NULL, said);",
                "\t\t\t\tfprintf(stderr, \"glwire: shader %u does not compile: %s\\n\", host_shader, said);",
                "\t\t\t}",
                "\t\t}",
            ]
        elif name == "glLinkProgram":
            out += [
                "\t\tGLuint host_program = names_program(g, a0);",
                "\t\tglLinkProgram(host_program);",
                "\t\tif (host_program != 0) {",
                "\t\t\tGLint good = 0;",
                "\t\t\tglGetProgramiv(host_program, GL_LINK_STATUS, &good);",
                "\t\t\tif (!good) {",
                "\t\t\t\tchar said[1024] = \"\";",
                "\t\t\t\tglGetProgramInfoLog(host_program, sizeof(said), NULL, said);",
                "\t\t\t\tfprintf(stderr, \"glwire: program %u does not link: %s\\n\", host_program, said);",
                "\t\t\t}",
                "\t\t}",
            ]
        else:
            out.append("\t\t%s(%s);" % (name, host_call_args(call)))
            if name in ("glDrawArrays", "glDrawElements"):
                out.append("\t\tnames_attribs_release(g);")
            out += free_arguments(call)
        out.append("\t\tbreak;")
        out.append("\t}")
    out += [
        "\tcase 201: {",
        "\t\t/* eagl.c's own IOSurface upload: pixels the table never saw */",
        "\t\tGLuint texture = wire_get_u32(r);",
        "\t\tGLenum target = wire_get_u32(r);",
        "\t\tGLint internal = wire_get_i32(r);",
        "\t\tGLsizei width = wire_get_i32(r);",
        "\t\tGLsizei height = wire_get_i32(r);",
        "\t\tGLenum format = wire_get_u32(r);",
        "\t\tGLenum type = wire_get_u32(r);",
        "\t\tGLsizei row_bytes = wire_get_i32(r);",
        "\t\tGLsizei pixel_bytes = wire_get_i32(r);",
        "\t\tGLsizei bytes = 0;",
        "\t\tvoid *data = wire_take(r, &bytes);",
        "\t\tprintf(\"glwire: upload texture %u target %#x internal %#x %dx%d format %#x type %#x row %d pixel %d bytes %d\\n\",",
        "\t\t    texture, target, internal, width, height, format, type, row_bytes, pixel_bytes, bytes);",
        "\t\tfflush(stdout);",
        "\t\tnames_texture_upload(g, texture, target, internal, width, height, format, type,",
        "\t\t    row_bytes, pixel_bytes, data);",
        "\t\tbreak;",
        "\t}",
        "\tcase 200: {",
        "\t\t/* vertex arrays that the next draw needs (the guest sends them from its own",
        "\t\t * pointers; the GL signature of glVertexAttribPointer carries no size) */",
        "\t\tGLuint index = wire_get_u32(r);",
        "\t\tGLint size = wire_get_i32(r);",
        "\t\tGLenum type = wire_get_u32(r);",
        "\t\tGLboolean normalized = wire_get_u8(r);",
        "\t\tGLsizei stride = wire_get_i32(r);",
        "\t\tGLsizei bytes = 0;",
        "\t\tvoid *data = wire_take(r, &bytes);",
        "\t\tnames_attrib_data(g, index, size, type, normalized, stride, data);",
        "\t\tbreak;",
        "\t}",
        "\tdefault:",
        "\t\tfprintf(stderr, \"glwire: opcode %u\\n\", opcode);",
        "\t\treturn -1;",
        "\t}",
    ]
    return "\n".join(out)


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
        print(emit_dispatch(calls))


main()

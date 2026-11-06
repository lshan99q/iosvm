# ADR-012: eagl.c's frame boundary is not a CoreAnimation pass boundary

- Status: Accepted (stage 15)
- Date: 2026-11-06
- Supersedes: ADR-011's *motivation* (its wire change stands, as diagnostics)
- Superseded by: —
- Related: `docs/adr/ADR-011-gl-context-in-the-wire.md`, `docs/gpu-acceleration-stage15-plan.md`,
  `tools/glwire/host_glwire.c`, `userland/src/eagl.c`, `userland/src/gl_osmesa.c`

## Background

`eagl.c` ends a frame on `glFlush`/`glFinish` (`frame_end`), and CoreAnimation's render pass does not
end there.  The host, since stage 11, has bound **its own** framebuffer at the start of every frame
(`glBindFramebuffer(GL_FRAMEBUFFER, client->framebuffer)`) so that a replay always has a target.
Every draw that continues the previous pass across the flush therefore lands on that stand-in instead
of the framebuffer the guest had bound.

Stage 14 recorded the symptom as "the label pass draws into fbo 1 and nothing samples it".  Its first
reading blamed two merged EAGL contexts.  The stage-15 measurement refutes that reading:

- one boot with the record header carrying the context id (version 2): **3700 frames, every record
  context 0**; the second `EAGLContext` is created (two `an OpenGL ES 3 context for CoreAnimation`
  lines in the guest log) but never records a GL call;
- with `GLWIRE_KEEP_BINDING=1` the label pass's first draws move from host framebuffer 1 to the real
  layer framebuffers: guest 6 (`750x200`), guest 17 (`64x19`), guest 23 (`64x9`), …;
- the guest's OSMesa default framebuffer is a **16x16** scratch window (`gl_osmesa.c:43`,
  `#define WINDOW 16`), so "guest framebuffer 0" is not the screen; the host's stand-in for it is a
  reasonable target, but it is not what a continuing pass meant.

With the binding kept, a label draw's state is coherent: a `64x19` target, `scissor 0,0 64x19`, and a
`vertex_matrix` of `0.0312 … -15.0 / 0.1053 … -86.58` that maps the recorded vertices exactly onto
that target, with `glCheckFramebufferStatus` complete.

## Decision

**The host preserves the guest's framebuffer binding across the frame boundary.**  When the persisted
binding is 0 (the OSMesa default) or names a framebuffer that is gone or incomplete, the host falls
back to its own framebuffer, as before.  `GLWIRE_KEEP_BINDING=0` restores the stage-14 behaviour for
A/B runs, and the environment variable is the switch while the change is being verified.

## Consequences

- The first draws of a frame go where the guest meant them to go; the label pass now renders into the
  layer surfaces it will later be composited from (their textures are sampled by 2,874 draws of the
  boot).
- The picture is unchanged: every frame still draws its panel framebuffer last, and the readback is
  still taken from the last framebuffer drawn into.
- The remaining zero is at the **fragment** stage, not the target or the geometry.  Forced-colour
  evidence: with the same vertices and a trivial magenta fragment programme
  (`GLWIRE_LABEL_WHITE=1`) a label draw covers 11,520–17,010 pixels, 412 of 706 draws non-zero, and
  the screenshot shows magenta bands exactly where the glyph runs are.  With the real programmes
  every dump is zero, so what is missing is what the fragment samples (or the value that multiplies
  it), not where or whether it draws.
- What else a continuing pass needs (programme, uniforms, scissor, texture units) already survives
  the frame boundary in the host's GL context, so the framebuffer was the only piece the host reset.

## Evidence

```
host_glwire: client 17 speaks record version 2 (context in every header)
glwire: context 0 at opcode 19, record 0 of frame 1
glwire: draw#57 ctx 0 ... fbo 5 ... scissor on 0,0 750x200 ... u0:host12(guest16)
glwire: label state program 13 texmat0 1.0000 1.0000 0.0000 0.0000 matrix
        0.0027 0.0000 0.0000 0.0000 / 0.0000 0.0100 0.0000 0.0000 / ... / -1.3013 -5.3300 0.0000 1.0000
glwire: label draw #1 framebuffer 5 attachment texture 11 750x200: 0 nonzero pixels
glwire: label draw #1 framebuffer 5 attachment texture 11 750x200: 11520 nonzero pixels   (LABEL_WHITE)
```

(`/tmp/s14lab2-host.log`, `/tmp/s14mag-host.log`, `/tmp/s14ctx-host.log`.)

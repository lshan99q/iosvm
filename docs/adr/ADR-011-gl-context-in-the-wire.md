# ADR-011: one wire record belongs to one GL context

- Status: Accepted (stage 15 decision, evidence from stage 14)
- Date: 2026-11-06
- Supersedes: —
- Superseded by: —
- Related: `docs/gpu-acceleration-stage14-plan.md` §九, `docs/gpu-acceleration-stage15-plan.md`,
  `tools/glwire/`, `userland/src/eagl.c`

## Background

backboardd creates **two EAGL contexts** in one boot: the guest log has two
`hakios-gl: an OpenGL ES 3 context for CoreAnimation ... pid=16` lines, and `userland/src/eagl.c:373`
keeps `static struct eagl *contexts[8]` with per-context `framebuffer`, `colour`, `program`,
`bound[8]`, `arrays[8]` and `unit`. The wire, however, records every context's calls into **one**
stream, and `host_glwire` replays them on **one** GL context. The host therefore has one FBO binding,
one current programme, one set of uniforms and one texture-unit binding where the guest has two.

The stage-14 probe says what that costs. The app-label pass is the first draw of every frame, before
any `glBindFramebuffer`; it lands on the host's stand-in for guest framebuffer 0
(`client->framebuffer`, host name 1):

```
glwire: draw#57 elements ... program 13 (guest 13) fbo 1 viewport 0,0 750x1624 scissor on 0,0 184x90
        status 0x8cd5 drawbuffer 0x8ce0 u0:host13(guest17) u1:host11(guest15)
glwire: label draw #1309 framebuffer 1: 0 nonzero pixels, 0 with alpha
```

A 184x90 scissor with vertices at `458,433` is not one draw's state: either the target is a 184x90
glyph surface (then the 750x1624 stand-in is the wrong target) or the scissor comes from the other
context. Both readings point at the same defect. The programme source makes the second half worse:
programme 13's fragment shader multiplies its source by `y^4`, where `y` is the luminance of the
64x19 "screen backdrop" texture, so an empty backdrop makes the output zero **no matter what the
draw covers**. That 64x19 texture is created empty and its fill is not in this stream either —
another thing two merged contexts can hide.

The same defect explains why the label pass cannot be fixed by retargeting: the state that places the
quad, the object names it samples and the FBO it draws into all come from different contexts.

## Decision

**Every record carries the identity of the EAGL context that made the call, and the host replays
each context in its own GL context.**

1. Record header becomes `u32 opcode | u32 length | u32 context`. The guest interns the `void *context`
   that every EAGL-table trampoline already receives into 0..7; no call site changes.
2. The request line gains a version field. A host that sees an older version parses 8-byte headers and
   replays them as context 0 (the stage-14 behaviour), so an old image still boots.
3. The host keeps `struct ctx { EGLContext; EGLSurface; GLuint framebuffer; struct glstate names; }` per
   guest context, created lazily with `eglCreateContext(display, config, EGL_NO_CONTEXT, NULL)` —
   **not** shared, because the guest's contexts are not.
4. A record whose context differs from the current one makes that context current (`eglMakeCurrent`)
   before dispatching.
5. Framebuffer 0 means "the EAGL drawable" per context. The guest names that drawable's texture in the
   request line; the host binds an alias framebuffer whose colour attachment is that texture, so a
   draw to 0 writes into the surface the display reads, instead of into a 750x1624 renderbuffer.

## Options considered

| Option | Pro | Con |
|---|---|---|
| **Per-context record id + per-context host context (chosen)** | Correct by construction; state is isolated the way EAGL isolates it | Wire format change: both halves rebuilt together; name maps move into the context |
| Re-send a state snapshot when the context changes | No wire format change | The state surface is wide (FBO, scissor, program, uniforms, texture units, blend, attribs, clear colour); one missed item leaves a wrong frame, and a missed item is invisible in the log |
| Keep one stream, reset everything at frame boundaries | Cheapest | The label pass is *inside* a frame; the second context's state still overwrites the first's |
| Debug only: force the label pass into the panel FBO | Quick picture | It would draw with the other context's scissor/matrix; and the empty backdrop still zeroes programme 13 |

## Consequences

- `tools/glwire/{wire.h,encode.inc,dispatch.inc,remote.inc,host_glwire.c}` and the guest's copies in
  `userland/src/` change together; `make -C tools/glwire test` covers the format on the host, and a
  rebuilt `root.apfs` is required for a boot (`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop
  ./build.sh root.apfs`).
- Object-name maps become per context. Guest contexts do not share, so this is the correct mapping,
  and it will expose cross-context aliases that the merged stream accidentally made work.
- The frame's picture is read back from the **sender's** context; the guest names the sender in the
  request line.
- Rollback: `GLWIRE_CONTEXT_MERGE=1` on the host replays every record into context 0 (stage-14
  behaviour) for A/B runs.
- The 64x19 backdrop texture must be accounted for in the context that makes it; if it is still empty
  after isolation, the next question is "who was supposed to fill it", with the answer expected on the
  guest side.

## Evidence

- `glwire: fbo gen client=17 n=1: guest 1 -> host 2` and
  `glwire: fbo summary frame frame 1200 client 17 own 1 ... [guest 0 host 1 none 0 0x0 binds 0 draws 648]`
  (`/tmp/s14p0b-host.log`): host 1 is the renderer's own framebuffer, i.e. the stand-in for guest FBO 0.
- `glwire: label draw #1309 framebuffer 1: 0 nonzero pixels, 0 with alpha` × 1313 (`/tmp/s14p4-host.log`).
- `glwire: draw#57 ... scissor on 0,0 184x90` and `glwire: attrib 0 ... 458.0000 433.0000 0.0000 1.0000`
  (`/tmp/s14p0b-host.log`): the scissor is not that quad's.
- Two `hakios-gl: an OpenGL ES 3 context for CoreAnimation ... pid=16` lines in `/tmp/s14p0b.log`.
- `glwire: programme 13 shader 12 ... S = S * (y * y) * (y * y); return D*(1. - S.a) + S;`
  (`/tmp/s14p0b-host.log`): an empty backdrop zeroes programme 13.

## Amendment (2026-11-06 12:00): the measurement refuted the motivation

The first stage-15 boot with the context field in every record (3700 frames, one backboardd) logged
**only context 0**.  The second `EAGLContext` is created but never records a GL call.  So the label
pass's state was not another context's, and this ADR's motivation was wrong.

What stays: the record header, the version negotiation (old images still boot), the interning in the
recorder, and `GLWIRE_CONTEXT_LOG` as a diagnostic.  What does not: "per-context replay is the fix
for the label pass".  The actual defect and its fix are recorded in
`ADR-012-frame-boundary-not-a-pass-boundary.md`.

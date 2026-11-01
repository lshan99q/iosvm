# glwire — carrying CoreAnimation's GL calls to the host's GPU

The interface costs about 700 ms a frame because it is drawn by AArch64 instructions under TCG. The
host draws the same frame in 6–14 ms (see `tools/host-glquad`). This is the pipe between them.

## What has to cross

`userland/src/eagl_slots.txt` is the GL function table Darwin's userland dispatches through, and the
entries marked `*` are exactly the ones QuartzCore's OpenGL ES renderer drives: **62 calls**, not the
whole API. `spec.def` writes those signatures down once.

## One description, two programs

`gen.py` reads `spec.def` and emits both halves, so they cannot drift apart:

- `gw_<name>(...)` — the guest's recorder, one function per call, appending a record to a buffer
- a `switch` dispatching the same opcodes to the real GL functions on the host

The wire format is one record per call: `u32 opcode, u32 length, payload`, integers as they lie,
floats as their bits, pointers as a `u32` count followed by the bytes.

```
$ python3 gen.py spec.def encode   > encode.inc      # 57 recorders
$ python3 gen.py spec.def dispatch > dispatch.inc    # 57 cases
```

## The host half runs

`host_glwire` maps the guest's RAM file, opens a real GL context, and replays whatever arrives:

```
host_glwire: /tmp/fakeram.img mapped, 67108864 bytes
host_glwire: Tesla V100-SXM2-16GB/PCIe/SSE2 | 4.6.0 NVIDIA 580.178.04
host_glwire: listening on 9110
```

A stream of `glClearColor`, `glClear`, `glViewport` executes on the V100 without complaint.

## What is left

1. The guest side has to call `gw_*` instead of GL inside `eagl.c` (the recorders are generated; the
   plumbing of when to flush — at `glFlush`/the frame's end — is not written yet).
2. The dispatch needs the picture copied into the guest's surface, the way `gpu_host_ram` already
   writes a frame in place.
3. Five calls in the table are not in `spec.def` yet (`glFinishObjectAPPLE`, the sync-object pair,
   `glInsertEventMarkerEXT`, `glCopyTexSubImage2D` is there but untested end to end).

None of that changes the arithmetic: the drawing is 6–14 ms on the host against 700 ms in the guest.

## Both halves compile, and the records are the right size

`remote.inc` holds the trampolines in the shape the EAGL table calls (`entry(table[0], args...)`),
so wiring them in is a matter of pointing 62 slots at them. A test that records three calls:

```
$ ./glwire_test
recorded 60 bytes
```

which is exactly right: `glClearColor` (8 + 16), `glClear` (8 + 4) and `glViewport` (8 + 16).

Calls that **return** a value stay local for now (`my_<name>`), because a recorder cannot answer one:
`glCreateShader`/`glCreateProgram` would have to allocate a name that both sides agree on, which means
the name travelling with the call. None of the drawing calls return anything, so the interface's path
is unaffected by that gap.

## The guest module

`eagl_remote.c` is the guest half, and it compiles for arm64-iOS (42 KB object). It owns a 4 MiB
arena whose first word is a magic, records into it through the generated `gw_*` functions, and at the
end of a frame sends one line and the records to the host:

```
R <records magic> <frame magic> <width> <height> <record bytes> <frame bytes>
```

The host finds the records and the frame buffer by those two magics in the RAM file it has mapped --
so no address ever travels -- replays the calls on its own GL context, and writes the picture where
the guest will read it. `eagl_remote_frame()` hands that picture to whoever presents it, and
`eagl_remote_stats()` reports how many frames went this way and how long each took.

Still to attach in `eagl.c`: call `eagl_remote_begin()` when the boot argument asks for it, point the
62 table slots at `remote_*` from `remote.inc`, and call `eagl_remote_flush(width, height)` from
`frame_end()` (line 1027) instead of presenting the picture Mesa produced.

## What the first end-to-end attempt measured, and why it drew nothing

The wiring works mechanically -- the guest records, the host replays on the V100 -- but the
interface drew **no frames at all**:

```
hakios-gl: asked for string 0x1f01: softpipe
hakios-gl: -attachImage: target 0xde1, texture 0, an IOSurface of 'BGRA': not done
guest frames = 0
```

The cause is structural, and it is worth writing down before the next attempt: eagl.c does its own
GL work -- it creates textures and attaches IOSurfaces to them (`attachImage`) -- and those calls go
through the same dispatch table that was just pointed at the recorders. So the bookkeeping that makes
a picture exist was also sent to the host, and locally there was never a texture to draw.

A static split is not available: of the 57 calls the interface needs, **49 are also used by eagl.c
itself**. The separation has to be by *who is calling*, not by which function:

- eagl.c's internal GL must bypass the table and reach OSMesa directly -- `OSMesaGetProcAddress` in
  `gl_osmesa.c` already hands out those pointers, so a macro layer over the internal calls would do it;
- CoreAnimation's calls then arrive through the table and hit the recorders, which is the traffic
  worth sending.

Until that exists, the remote path cannot draw, and the honest frame rate of the interface is
whatever the local renderer gives: ~1.4 fps.

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

## The first frame with content: the Settings page

The recorder now does what the 22:44 note said it had to, but not by a static split: each of the 62
trampolines calls the entry its slot held **before** the override (`local_entry[]`, saved by
`eagl_fill_table_remote()`), then records. eagl.c's own wrappers stay in the local path, so
`-attachImage:` sees the texture it was just asked to attach. The other three fixes found while
making it draw: the arena is 32 MiB (one panel-sized `glTexImage2D` is 10.96 MB, and the old 4 MiB
dropped the rest of the record silently), the dispatcher keeps `glVertexAttribPointer`'s array until
the draw instead of freeing it with the call, and `frame_end()` writes the host's picture after the
local surface copy, not before.

The host half can be exercised without a guest:

```
$ gcc -O2 -o frame_replay frame_replay.c frame_record.c -lEGL -lGL
$ ./frame_replay /path/to/qemu/ram.img 0x33b9eb000
frame_replay: llvmpipe ... | 4.5 (Compatibility Profile) Mesa ...
frame_replay: 10962430 B of records for 10962000 B of layer; recorded in 6.8 ms, drawn in 58.0 ms;
              0 pixels differ (0.000%), mean 0.000, worst 0
```

`frame_record.c` builds the frame with the generated recorders (`encode.inc`, unmodified);
`frame_replay.c` reads the layer out of the guest's framebuffer in the RAM file, replays the records
through the same `dispatch.inc` `host_glwire` uses, draws it as a textured quad, reads it back and
puts it back in the same memory. The result is byte-identical to the guest's own Settings frame
(`docs/demo/glwire-settings-guest.png` and `-host.png`).

`panel_run.py` drives a running machine through that: it waits for the home screen by the colour
below the kernel's boot band, taps Settings over QMP, waits for the page, pauses the machine, runs
`frame_replay` and takes a screendump.

```
$ ./panel_run.py /path/to/qemu/ram.img --log /tmp/iosvm-0202.log --out /tmp/panel-run
```

The EGL context asks for the device first, so on a host where `/dev/dri` and `/dev/nvidia*` are open
it lands on the V100; in a sandbox that refuses them it falls back to llvmpipe and says so.

## What the guest still has to send

Calls that return a value (`glCreateProgram`, `glGenTextures`, `glGetUniformLocation`, the fence
pair) stay local on both sides, so the names they produce are not the same numbers on the host. For
CoreAnimation's own traffic the names have to travel with the calls (`opcode, length, args, result`)
and the dispatcher needs a guest-name -> host-name table for textures, framebuffers, programs,
shaders and uniform locations. Until then the wire carries a frame the host can draw, but not the
interface's own calls.

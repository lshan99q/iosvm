# glwire — carrying CoreAnimation's GL calls to the host's GPU

The interface costs about 700 ms a frame because it is drawn by AArch64 instructions under TCG. The
host draws the same frame in 6–14 ms (see `tools/host-glquad`). This is the pipe between them.

## What has to cross

`userland/src/eagl_slots.txt` is the GL function table Darwin's userland dispatches through, and the
entries marked `*` are exactly the ones QuartzCore's OpenGL ES renderer drives: **62 calls**, not the
whole API. `spec.def` writes those signatures down once, as the real GL functions have them.

An earlier version added a size parameter to every pointer argument. That does not fit an EAGL slot:
the table is called as `entry(context, glArguments...)`, so the extra parameter read whatever was in
the next register. `glVertexAttribPointer`, `glDrawElements` and `glShaderSource` were all affected
before this was found. The argument list is now exactly the GL one, and each pointer type says how
its byte count is worked out:

| token | meaning | bytes on the wire |
|---|---|---|
| `s` | `const GLchar *` | `strlen`, NUL put back by the host |
| `v` | `glVertexAttribPointer`'s array | none here; sent with the draw (opcode 200) |
| `d` | index array | `count * sizeof(type)` |
| `x`, `y` | texture / sub-texture pixels | `width * height * bpp`, unpack alignment applied |
| `z` | compressed pixels | `imageSize` |
| `t`, `r` | texture / framebuffer name arrays | `count * 4` |
| `g` | `glGetIntegerv`'s output | none; the host answers from its own state |
| `q` | `GLsync` | 8 bytes |
| `3`, `4`, `m`, `M` | uniform vectors / matrices | `count * (3, 4, 9, 16) * 4` |
| `V` | `glShaderSource`'s string vector | each string's length and bytes |

## One description, two programs

`gen.py` reads `spec.def` and emits both halves, so they cannot drift apart:

```
$ python3 gen.py spec.def encode   > encode.inc      # guest recorders
$ python3 gen.py spec.def dispatch > dispatch.inc    # host switch
$ python3 gen.py spec.def slots    > remote.inc      # EAGL slot trampolines
$ python3 gen.py spec.def table userland/src/eagl_slots.txt > remote_table.inc
```

The wire format is one record per call: `u32 opcode, u32 length, payload`, integers as they lie,
floats as their bits, and each pointer as a `u32` byte count followed by the bytes.

## Names: the guest's are the real ones

The guest runs every call locally as well as recording it: the trampoline calls the entry the slot
held before it was overridden (`local_entry[]`). That keeps `eagl.c`'s own wrappers in the path, and
it means the guest's answers are authoritative. The host must not make up its own numbers, because a
name is not a value that a recorder can return.

So calls that answer with an object name carry that answer at the end of their record:

| call | appended |
|---|---|
| `glCreateProgram`, `glCreateShader` | the guest's `GLuint` |
| `glGetUniformLocation` | the guest's `GLint` (per program) |
| `glFenceSync` | the guest's `GLsync` as 8 bytes |
| `glGenTextures`, `glGenFramebuffers` | the filled name array, which the `t`/`r` argument already carries |

`names.h` is the host's map: textures, framebuffers, programs, shaders and syncs, plus uniform
locations keyed by `(guest program, guest location)` — two programs may both call their sampler
location 3, and those are two different locations on the host too. Every call that consumes a name
translates it first; `glGen*` puts both names in, `glDelete*` takes them out.

`frame_record.c`'s synthetic frame still works: its stream names the objects its own context made,
and a missing map entry falls back to the same number.

## Vertex arrays

`glVertexAttribPointer` has no size in its signature and the GL only reads the array at the draw.
The recorder remembers the pointer, size, type and stride; at `glDrawArrays` / `glDrawElements` it
works out how many elements the draw reads (the count, or the largest index in the index buffer) and
emits an opcode-200 record with the bytes. The host applies that record with `glVertexAttribPointer`
and keeps the copy until the draw returns. The earlier code copied an arbitrary length at pointer
time, which is why a real stream could not have worked.

## The host half

`host_glwire` maps the guest's RAM file, listens on port 9110, and gives **each connection its own GL
context and name maps**: the guest's names are per process, and two processes may both call their
first texture 1. The request line carries the magic words in front of the guest's record arena and
picture; the guest makes them unique per process (the pid is in them) so the host can find the right
buffer when several processes are drawing.

```
host_glwire: /path/to/qemu/ram.img mapped, 12884901888 bytes
host_glwire: llvmpipe (LLVM 20.1.2, 256 bits) | 4.5 (Compatibility Profile) Mesa 25.2.8
host_glwire: listening on 9110
```

The reply is `R done <ms>` after the records are replayed and the picture is in the guest's buffer.

## Tests

```
$ make

$ ./map_test
map_test: 1267 bytes of records
map_test: current program 3 (linked 1), framebuffer 2 (status 0x8cd5), 2 shaders: 4(compile 1) 5(compile 1)
map_test: tint at 1 is 1 0.5 0 1
map_test: layer unit at 0 is 0; position attribute 0; error 0
map_test: centre pixel 51 51 0 255; maps: 2 textures, 1 framebuffers, 1 programs, 2 shaders, 1 syncs, 2 uniform locations
map_test: PASS

$ ./e2e_test.py
e2e_test: reply 'R done 0.09'
e2e_test: centre pixel B=0 G=51 R=51 A=255
e2e_test: PASS
```

`map_stream.c` deliberately names its objects 77, 88/89, 101/102 and 303 and its uniforms 5/6; the
test first makes dummy objects so the host's names are different again. The picture only comes out
right if every one of those names travelled. `e2e_test.py` sends that same stream through the socket
to `host_glwire` and checks the pixels in the fake RAM file.

`frame_replay` still checks the panel path: it reads the guest's framebuffer, makes a frame with the
guest recorders, replays it and writes the picture back. On the paused Settings page:

```
frame_replay: 10962480 B of records for 10962000 B of layer; recorded in 6.7 ms, drawn in 59.0 ms;
              0 pixels differ (0.000%), mean 0.000, worst 0
```

## The panel's surface is live

`panel_probe.py` is the screendump acceptance test, and it also records why the previous note was
wrong. QEMU's PPM writer writes `pixman_image_get_stride` bytes per row — for 1125 RGB pixels that is
3376, not 3375 — and a reader that assumes packed RGB sees a one-byte-per-row shear that looks like a
frame from a slide-in animation. Read with the stride QEMU actually wrote, the surface is a live view
of the guest framebuffer:

```
$ ./panel_probe.py qemu/ram.img
panel_probe: 1. the settled screen
panel_probe:    screendump vs guest framebuffer: 0 differing pixels
panel_probe: 2. a magenta rectangle at 40,40 120x120
panel_probe:    magenta pixels in the screendump: 14400 of 14400; other differing pixels: 0
panel_probe: 3. restoring the framebuffer
panel_probe:    after restore: 0 differing pixels
panel_probe: PASS: the console surface is live
```

`tools/qemu/qemu-ppm-stride.patch` is the one-line QEMU fix (write `width * 3` rather than the
padded stride) for a build that wants standard PPMs; the readers here handle QEMU's current output.

## What is left

1. **A live stream from a boot that has the switch.** The current guest was booted without a network
   device and before `/redwood/bin/glremote_on` existed in the image, so its `eagl_remote_begin()`
   never ran. A boot with `QEMU_NET=1` and that file in `root.apfs` is what exercises the host with
   CoreAnimation's own calls; see `docs/research/linux-kernel-build.md` for the disk constraint that
   kept this round from starting one.
2. **Textures `eagl.c` makes itself.** `-attachImage:` creates textures for IOSurfaces outside the
   table, so their names never travel. A frame that samples such a texture can only be right if the
   host learns about it (an explicit record when eagl.c makes one), or if the IOSurface's memory is
   shared with the host.
3. **One context per process.** The recorder's arena is process-global; two EAGL contexts used
   alternately in one process would interleave. CoreAnimation appears to use one.
4. **GLSL ES to desktop GLSL.** The host is a desktop GL 4.5 compatibility context. `names.h` strips
   `#version 100`, `precision` lines and `lowp`/`mediump`/`highp`; shaders that rely on
   `EXT_shader_framebuffer_fetch` still need a plan.

## It works end to end, and the pipe is now the problem

With the recorders installed the interface really does go through the host:

```
guest:  hakios-remote: end of frame, on=1 images=1 first=750x1624
guest:  hakios-remote: flush -> 1
host:   host_glwire: frame 2, 750x1624, 9959384 records B, 4872000 picture B, 0.03 ms, interval 4.47 s
```

Ten megabytes of records a frame, and a replay that costs the host 0.03 ms to *dispatch*. The cost
is not drawing and not the GPU: it is carrying the records. Two texture uploads of 4.8 MB each are
being inlined into the stream every frame, because CoreAnimation refills its layer textures.

That is exactly the problem `tools/host-glquad` already solved for a single layer: do not send the
pixels, let the host read them where they lie. The host has `qemu/ram.img` mapped (QEMU must run with
`QEMU_RAM_SHARE=on`, which is now the default), so a texture upload only needs to name the memory and
the geometry, not carry it.

Two ways to get there, in order of cost:

1. **Deduplicate first** (cheap, no address translation): hash the pixels in the recorder and skip
   the record when a texture's contents are unchanged since the last upload. If CoreAnimation refills
   with identical content -- which the two same-size uploads a frame suggest -- the stream collapses
   to almost nothing and the frame rate follows the 6-14 ms the host already measures.
2. **Pass by reference** (the real fix): teach the recorder to hand over the guest address of the
   upload and let the host translate it into its own mapping of the RAM file. That needs the guest
   to know the physical offset of a user buffer, which this port does not currently expose.

So the honest position: the path is proven end to end and the host is fast; what remains is the wire
carrying ten megabytes a frame, and the fix is a known one rather than an open question.

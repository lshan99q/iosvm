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

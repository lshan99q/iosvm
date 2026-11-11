# S22 host_info: source trace, patch, and two-boot verification

Status: source analysis + kernel patch.  No boot from this document.

## 1. What the stack says

`ext3` (PodcastsWidget[45]) has 90 samples in:

```
_mach_msg2_trap + 8
_host_info + 228
___pthread_init + 164
_libSystem_initializer + 132
dyld ... runAllInitializers
```

Disassembling the extracted `libsystem_pthread` (`___pthread_init` at `0x1d0cd70bc`)
shows the call at `+0xa0`:

```
x0 = host port (result of the preceding mach_host_self-ish call)
w1 = 5                  # HOST_PRIORITY_INFO
x2 = &info[8]
x3 = &count (8)
bl  _host_info
```

So the observed call is **`host_info(host, HOST_PRIORITY_INFO, ..., count=8)`**, not
`HOST_BASIC_INFO` (the memory bug below is real but is not this call's flavor).

## 2. Kernel source audit

- `osfmk/kern/host.c:190 host_info()`: no host lock, no sleep, no `mach_msg`; it is a
  direct `switch (flavor)` and returns a value.  `HOST_PRIORITY_INFO` is implemented
  (`host.c:250`), writes 8 integers, returns `KERN_SUCCESS`.
- `HOST_BASIC_INFO` is implemented (`host.c:197`); `HOST_PROCESSOR_SLOTS` is **not
  defined** in this tree's `osfmk/mach/host_info.h`, and `host_info` has no case for it:
  it falls to `default: return KERN_INVALID_ARGUMENT` immediately, not a wait.
- Host port MIG: `osfmk/mach/mach_host.defs:90` routine `host_info`; generated
  `BUILD/.../mach_host_server.c:199 _Xhost_info` calls `host_info` under no host lock;
  `ipc_kobject_server_internal()` (`osfmk/kern/ipc_kobject.c:384`) calls the generated
  routine under no host lock.  `ipc_host.c:122` creates the `IKOT_HOST` port.
- Therefore **there is no host lock deadlock inside host_info** and **no flavor can
  block there**.  If the thread is at `_host_info` → `mach_msg2`, it is waiting for the
  MIG reply, not spinning in the `switch`.

## 3. Ranked cause hypotheses

1. **MIG reply/port path** (strongest): the request reaches the kernel but the reply is
   not delivered to the extension's reply port.  Evidence: `mach_msg2` at the top, no
   `xpc` frames, process otherwise runnable.  Falsify: `hakios_host_info_trace=1`; if
   `enter` is logged but `ret` never is, the server call or reply is stuck; if `enter`
   and `ret` both appear while the user PC stays in `host_info`, it is the MIG reply.
2. **MIG filter/MACF drop**: `CONFIG_MACF` calls `mac_task_kobj_msg_evaluate` before the
   routine; an extension policy returning an error would produce a MIG error reply (not a
   wait), but a policy hook could sleep.  Falsify: same trace entry/ret.
3. **Missing/unsupported flavor**: `HOST_PROCESSOR_SLOTS` is absent from this tree; the
   observed `HOST_PRIORITY_INFO` is supported.  If a different libSystem build calls
   flavor 2, `host_info` returns `KERN_INVALID_ARGUMENT` immediately; a libSystem retry
   loop would look like repeated `host_info` entries, not a single wait.
4. **Zero-ish HOST_BASIC_INFO**: `machine_info.memory_size = (uint32_t)mem_size` turns
   12 GB (`0x300000000`) into **0**; `host_basic_info.memory_size` is documented as
   capped at 2 GB.  This is a real correctness bug, but it is not the observed flavor;
   fix it while tracing.

## 4. Patch (minimal)

`tools/glwire/patches/host-info-fix.patch` (applied to
`xnu-src/xnu-8796.121.2/osfmk/kern/host.c`):

- wrap `host_info` with an opt-in boot-arg trace
  (`hakios_host_info_trace=1`), printing flavor/count/host and the return;
- cap `host_basic_info.memory_size` at 2 GB from `machine_info.max_mem` instead of
  returning the truncated 0;
- fall back to 1 for zero `max_cpus`/`avail_cpus`/physical/logical CPU fields.

It does **not** bypass libSystem and does **not** special-case extensions.

Build:

```sh
cd /host/iosvm-work/hakios-xnu-kernel
make kernel
```

Current container blockers (after deleting the stale `BUILD/obj`; that removed the
`/home/lshan/...` path errors):

1. `bison: No such file or directory` for `SETUP/config/parser.c` (host build tool);
2. `fatal error: 'uuid/uuid.h' file not found` in the host tools.

Both are missing build dependencies, not patch errors.  A build container needs
bison/flex/m4 and libuuid headers (or the tree's generated `BUILD/obj` from a matching
host), then `make kernel`; `strings hakios-kernel | grep 'hakios: host_info'` is the
no-boot proof that the patch linked.

Self-check without boot:

```sh
strings hakios-kernel | grep 'hakios: host_info'
```

Expected exactly the two new trace format strings.  After boot, the trace line is
`hakios: host_info enter flavor=5 count=8 ...` followed by `ret=0 count=8`.

## 5. Two-boot verification

Base: C1 `system=6be494f1…`, root `/tmp/root-byref.apfs` (3207a35e…), `QEMU_SMP=1`,
`ZERO_RAM=1`, `launchd_disable_all_timeouts=1` so the extension is not killed.

### ext4 — trace host_info

```sh
XCMD="glwire_local=1 launchd_disable_all_timeouts=1 hakios_host_info_trace=1" \
HOST_ENV="GLWIRE_NO_PANEL=1" \
MINFRAMES=999999 MINSHOT=300000 UI_GRACE=400 NOUI_BY=900 MAXSECS=1120 \
  ./harness/boot-measure.sh ext4 /tmp/root-byref.apfs 1120 --no-panel
```

Filter:

```sh
grep -aE "hakios: host_info|checked in as alive|ustacks: PodcastsWidget|uncorking exec" /tmp/ext4.log
```

Decision:

- many `enter flavor=5 count=8` + `ret=0 count=8`, extension still stuck → host_info
  returns; the block is **MIG reply/port** (H1) or libSystem after the call, not `host_info`;
- `enter` with no matching `ret` → server-side block; the wrapper narrows it to
  `host_info_inner`/MIG reply;
- no `enter` at all while stuck → request never reaches `ipc_kobject_server`; inspect
  host port/MACF;
- extension reaches `checked in as alive` → the CPU/memory fallback was sufficient
  (unlikely for flavor 5, but the trace will say).

### ext5 — CheckIn

Only after ext4 says `host_info` returns:

```sh
XCMD="glwire_local=1 launchd_disable_all_timeouts=1" ... boot-measure ext5 ...
```

Success is one line in the guest log:

```text
... extension checked in as alive ...
```

If ext4 shows a single `mach_msg2` wait with `enter` but no `ret`, **two boots are not
enough**: add an `ipc_kobject_server_internal` / `host_info` reply trace or a kdebug
event, because the fault is below `host_info`, not in its switch.

## 6. Can two boots decide?

- **ext4** decides whether the kernel `host_info` entry/return happens.  It is enough to
  separate "host_info switch blocked" from "MIG reply/port" or "request never arrives".
- **ext5** can confirm CheckIn only if ext4's patch/fix actually clears it.
- If ext4 shows `enter`+`ret` but the extension remains at `mach_msg2`, **two boots are
  not enough**; the next evidence needed is a reply-port/MIG trace (or a rebuilt
  tracepoint in `ipc_kobject_server_internal`), not another timeout/limits boot.

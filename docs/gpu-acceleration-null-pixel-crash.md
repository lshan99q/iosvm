# The fourth round: why backboardd died, and the one-line fix

> 2026-11-02. The short version: `glTexImage2D(..., pixels = NULL)` is a legal "allocate an empty
> texture", but the recorder computed a nonzero byte count from the dimensions and `memcpy`'d from
> NULL. That was the whole `SIGSEGV`. With the guard in place, the remote path ran 152 host frames,
> `MATCH in-place rows`, 13.8 fps, and the home screen appeared (448,734-byte screenshot).

This note is the walkthrough; the measurements and the surrounding rounds are in
`gpu-acceleration-plan.md` sections 0.7-0.10.

## 1. Split "recorder installed" from "frame sent"

New switch: `/redwood/etc/glwire_noflush` (build with `GLWIRE_NOFLUSH=1`) or boot-arg
`glwire_noflush=1`. The 62 recorders are installed and every call is recorded; `eagl_remote_flush()`
resets the arena and returns 0, and `eagl_remote_surface()` also returns 0. No socket, no 556 vtop,
no `IOSurfaceLock`, no picture copy.

Image `/tmp/root-noflush-local.apfs` (`glremote_on` + `glwire_local`, so the guest still draws
locally), 750x1624, no host renderer. Result:

- 297 `hakios-remote: flush -> 0`;
- 6m20s after boot: `(user/501/com.apple.backboardd [14]) exited due to SIGSEGV`;
- restarted as pid 77.

So the crash is not in the flush or the write-back. It is in "the slots are replaced and every call
is recorded".

## 2. Catch the address without a debugger

Kernel: `patches/0011-usertrap-filter.patch`. It adds

- `ustrace=1`: the exception trace alone (does **not** turn on `systrace`'s syscall logging);
- `ustrace_name=NAME`: print only that process.

Boot argument: `ustrace=1 ustrace_name=backboardd`. Never `qemu/touch.py stacks` -- this QEMU
configuration cannot halt a CPU and the debugger path panics.

Caught at 15:44:16 (`/tmp/iosvm-1501.log`, shared-cache slide `0x49b34000`):

```
usertrap: backboardd[14] exception 1 code 0x1 0x0:
  pc 0x21a76fdf4 lr 0x10285dc04 sp 0x16ffaf960, shared cache slide 0x49b34000
  called from 0x10285f928
  called from 0x1d1f321b0 ...
hakios: signal 11 at 0x0, pc 0x1d0c3bdf4, lr 0xb8d29c04,
  called from 0xb8d2b928 0x1883fe1b0 ...
```

Unslid:

| address | module | what |
|---|---|---|
| `pc 0x1d0c3bdf4` | `libsystem_platform.dylib`, base `0x1d0c3b000`, +`0xdf4` | `memcpy` loop, `ldnp x12,x13,[x1]`; exception code 1 = read at 0 |
| `lr 0xb8d29c04` | `libhakios.gl.dylib`, base `0xb8ccc000`, +`0x5dc04` | next instruction after `bl _memcpy` in `_wire_image` |
| `0xb8d2b928` | same, +`0x5f928` | next instruction after `bl _wire_image` in `_gw_glTexImage2D` |

The last guest line before the fault is the empty texture allocation:

```
hakios-gl: a new kind of texture: empty, internal format 0x1908, format 0x1908,
           type 0x1401, 750 x 1624
```

## 3. Root cause

`userland/src/wire.h`:

```c
/* before */
uint32_t size = count > 0 ? (uint32_t)count : 0;
if (size != 0 && pixels != NULL && wire_vtop_hook != NULL) { ...vtop... }
...
wire_put(w, pixels, size);          /* size = 750*1624*4, pixels = NULL */
```

`glTexImage2D` with `pixels == NULL` still has nonzero dimensions. The vtop branch checked the
pointer; the inline fallback did not. `wire_put` calls `memcpy(dst, NULL, 4.87 MB)`.

## 4. Fix

```c
uint32_t size = (count > 0 && pixels != NULL) ? (uint32_t)count : 0;
```

The same guard is in `wire_bytes()` (compressed texture uploads). Null pixels now record length 0;
the host allocates an empty texture, which is what GL means.

## 5. Verification

### No frame sent

`/tmp/root-fixed-local.apfs`, `glwire_noflush=1`, `ustrace=1 ustrace_name=backboardd`,
`/tmp/iosvm-1502.log`:

- 16:01:07 to 16:16:03 (15 minutes);
- 4,583 `flush -> 0`;
- **0 SIGSEGV, 0 usertrap**;
- the empty `750 x 1624` texture appears and does not crash;
- 6-8 fps.

### Full path, host running

`/tmp/root-fixed-net.apfs` = `GL=1 GLREMOTE=1 GLWIRE_LOCAL=1 CONFIGD=1 RC=rc.vtop` (rc.vtop runs
`netcfg en0 10.0.2.15 10.0.2.2`), `host_glwire 9110 qemu/ram.img`,
`/tmp/iosvm-1504.log` and `/tmp/iosvm-1504-host.log`:

- 16:28:53 boot, host connected on 9110;
- first 3 frames slow (known boot stall), then recovered;
- 16:37 screenshot 448,734 B: home screen, Dock, icons, search, `rdar:45025538` banner;
- host frame 152: `750x1624, 7439 records B, 4872000 picture B, 0.03 ms,
  1 image refs (4884992 B), panel ... picture ... MATCH in-place rows`, 13.83 fps;
- guest frame 140: 66 GL calls, 9 draws, 142 ms;
- 304 `flush -> 2 (rows into the surface)` (the console prints every line twice);
- **0 SIGSEGV, 0 usertrap**.

Screenshot: `docs/demo/remote-home-screen-2026-11-02.png`.

## 6. Reproduce

```sh
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
export THEOS=$HOME/theos IOSVM_USE_LLD=0          # cctools ld64; lld drops -image_base
export PATH=$IOSVM/xnu-deps/host/bin:/tmp/cctools/bin:$PATH

# build the image with netcfg + host switch
cd $IOSVM
GL=1 GLREMOTE=1 GLWIRE_LOCAL=1 CONFIGD=1 RC=rc.vtop ./userland/build.sh
cp userland/root.apfs /tmp/root-fixed-net.apfs

# one QEMU only, no compilation at the same time
QEMU_SMP=4 QEMU_TB_MB=1536 QEMU_RAM_GB=12 QEMU_NET=1 QEMU_ESP_IMAGE=$PWD/qemu/esp.img \
  IMG=$PWD/userland/system.apfs ROOT=/tmp/root-fixed-net.apfs \
  CMDLINE="cpus=4 fb_beat=0 fbscale=2 metal=1 ustrace=1 ustrace_name=backboardd" \
  timeout 7500 bash qemu/run-darwin.sh 900 > /tmp/iosvm-1504.log 2>&1 &

# after QEMU has opened qemu/ram.img:
cd /home/lshan/DSH-desktop/iosvm/tools/glwire
./host_glwire 9110 $IOSVM/qemu/ram.img > /tmp/iosvm-1504-host.log 2>&1 &

# read it
grep -a 'host_glwire: frame' /tmp/iosvm-1504-host.log | tail -3
grep -a 'SIGSEGV\|usertrap:' /tmp/iosvm-1504.log | tail
$IOSVM/qemu/screendump.sh /tmp/iosvm-1504.png
```

## 7. Bisection switch (available, not needed for this crash)

`glwire_slots=MASK` keeps only the recorders whose spec index has its bit set; the rest are restored
to `local_entry`. `gen.py` emits `remote_slot_offset[]`. `glTexImage2D` is spec index 22, so
`glwire_slots=0x3fffffff80000000` keeps 31-61 and leaves it out. The `ustrace` PC/stack already
named `gw_glTexImage2D`, so the fourth round did not spend two more boots on a half-slot run; the
switch is there for the next question.

## 8. What is left

- repeat it: 5 cold boots, at least 4 reaching a >300 KB screen (R1's acceptance);
- the first 3 frames of the successful boot still take minutes (the known boot stall);
- W1/W2 (host-only drawing) are still off: this run had `glwire_local`, i.e. the guest draws and
  the host reproduces the result; the speed work starts after repeatability.

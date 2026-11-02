# Building the arm64 kernel on a Linux host: what it takes, and where it stopped

The project needs its own kernel (drivers are added inside it), and the build must work on Linux
because that is the platform the project targets. The upstream tree builds on macOS with Xcode; this
records the Linux path, as executed here on 2026-10-31, including the four things that had to be
replaced and the one thing that is still open.

## What the build needs

| Piece | Where it came from | Note |
|---|---|---|
| `theos` | `git clone https://github.com/theos/theos` | the build only uses its layout |
| `iPhoneOS16.5.sdk` | `github.com/theos/sdks`, sparse checkout of that one directory (233 MB) | the public SDK, the same one the Linux path has always used |
| Apple open sources | `xnu-deps/setup.sh --stage1` | AvailabilityVersions, libdispatch, libpthread, mig sources, the macOS 13.3 SDK for DriverKit headers |
| the kernel tree | already present (`xnu-src/xnu-8796.121.2`) | Apple's `xnu-8796.121.2` with the HakiOS work on top |
| `libfirehose_kernel.a`, `libpthread_kext.a` | `xnu-deps/setup.sh --stage2` | compiled against the kernel's own exported headers |

## Four host tools that are Apple's on macOS and have to be stood in for on Linux

Each failed loudly, and each is the kind of thing that silently changes what gets built if it is
faked badly:

1. **`ld`** — there is no cctools on this host, but LLVM ships `ld64.lld`, which speaks the Mach-O
   command line. Two shims are needed: the binary must be invoked **as `ld64.lld`** (an `ld`
   symlink makes LLD choose its GNU personality), and it wants `-arch` and `-platform_version`
   stated, which clang only passes for a Darwin target. The wrapper supplies `-arch arm64` and
   `-platform_version ios 16.5 16.5` when absent and passes everything else through.
2. **`libtool`** — Apple's static-archive driver. A shim maps `-static -o LIB objs…` onto
   `llvm-ar rcs`. **It must be `llvm-ar`, not GNU `ar`**: GNU ar writes an ELF-style archive index,
   and `ld64.lld` then refuses with *archive has no index*.
3. **`mig`** — built by stage 1 from Apple's own `mm_bootstrap_cmds`, so the generated stubs are
   Apple's dialect rather than a reimplementation.
4. **`tcsh`** — the build calls `csh` for a few scripts; its own build failed on
   `unable to find the tgetent()` (no ncurses dev package here). Ubuntu's `tcsh` provides `csh` and
   is enough; it is linked into `xnu-deps/host/bin` rather than built.

With those in place the whole tree **compiles**: 320 objects in `osfmk`'s directory alone, 203 MB of
build products under `BUILD/obj/DEVELOPMENT_ARM64_BCM2837`.

## The link: Apple's `-alias_list`, and how it was replaced

The final link failed with **9 undefined symbols**, and they were not missing code. XNU links with
`-Wl,-alias_list,<file>`; the file's pairs say *the plain name a caller writes is an alias of the
implementation that carries a suffix*:

```
_IOMallocData_external       _IOMallocData
_mach_vm_allocate_external   _mach_vm_allocate
_vm_fault_external           _vm_fault
```

`vm_user.o` defines `_mach_vm_allocate_external` and `_mach_vm_allocate_kernel`, and nothing else --
so without the alias list the plain name really is undefined. `ld64.lld` has no `-alias_list`.

The wrapper now implements it: it reads the file it was handed, emits one trampoline per pair
(`_mach_vm_allocate: b _mach_vm_allocate_external`), assembles it for `arm64-apple-ios16.5`, and
appends the object to the link. The trampolines are **weak**, because a few of those names do exist
plainly in the tree and a strong duplicate would fail the link (`_task_id_token_port_name_to_task`
did exactly that).

Two more host-tool gaps surfaced behind it:

- **`strip`** was GNU binutils, which cannot read Mach-O. It is now `llvm-strip`.
- **`libtool -ca … -filelist`** was not handled by the shim; it is now, reading the file list.

And one portability bug of my own: `${var//pat/}` is a bashism and `/bin/sh` here is dash, so the
shim died with `Bad substitution` until it became `tr -d ' '`.

## What exists now, and what it still refuses

**The kernel compiles and links on Linux:** `mach.development.bcm2837`, 15,819,368 bytes,
`Mach-O 64-bit arm64 executable`. Building it needed the theos layout, the public 16.5 SDK, Apple's
open sources, and the five stand-ins above (**ld**, **libtool**, **strip**, **mig**, **tcsh**) --
none of them Apple's binaries.

**It does not boot yet.** Handed to the project's own harness (`tools/qemu-open.sh`), the loader
rejects it before the kernel runs:

```
payload entry 'kernel' size f16268
kernel va 0..1300000 entry 0
unexpected kernel layout
```

That message is the upstream `xnuloader` checking the kernel's segment layout, and `ld64.lld` lays
segments out differently from cctools' `ld` -- a known difference, not a defect in the build. The
upstream prebuilt kernel still boots (PASS, five markers), so the harness itself is fine.

So the remaining step is exactly one thing: **get cctools' `ld64` onto this host**, at which point the
wrapper stops emulating Apple's linker and uses it. That build needs libdispatch, which Ubuntu does
not package; it was built from source (`swift-corelibs-libdispatch`, with clang -- the default GCC
rejects its warning flags). The `ld64` build itself was interrupting at `libhelper.la` when driven
from a subdirectory; it has to be driven from `cctools/ld64`, and that is where it stands.

## Addendum: how far the boot gets, and where it stops

With the toolchain in place the whole upstream userland side also builds on Linux: `root.apfs` (the
init image), `xnuloader/loader.efi` (built with the device identity from the user's IPSW), and
`system.apfs`. Two build-only gaps had to be closed on the way, both about the linker rather than the
code: the dynamically linked programs need an SDK stub (`libSystem.tbd`, which the public SDK ships
only as `libSystem.B.tbd`, so a version-less symlink is made), and cctools' `ld64` cannot read `.tbd`
text stubs at all without TAPI -- so those two links go through LLVM's `ld64.lld`, which can.

The boot reaches further than it ever has here:

```
resident image at pa fc000000 size 93a00000: it is the root (md0), the payload ramdisk is md1
BSD root: md0, major 1, minor 0
apfs: device 1,0: requests to it cost next to nothing (a disk in memory)
load_init_program: attempting to load /sbin/launchd
shared_region: [1(launchd)] check_np(...) vm_shared_region_start_address() failed
** Stackshot Succeeded **
```

So the APFS volume mounts as the BSD root, the kernel finds and starts loading `/sbin/launchd`, and
then dies setting up the shared region -- the step where dyld's shared cache has to be mapped.

**It is not the kernel and not the image.** The same panic occurs with mainline upstream's own
prebuilt kernel and upstream's own images, in upstream's default configuration. That makes it a boot
*configuration* question, and the concrete hypothesis is written down for the next round: `init`'s
`mount_cryptex()` expects the OS cryptex as a separate APFS container (container `C` of `md0`, or its
own device), and both images built here are single-volume containers -- so the cache is present as
files but never mounted where dyld looks for it, and the shared-region setup has nothing to map.

## Addendum 2: the user space boots -- launchd, 60 processes, SpringBoard

Three fixes, each found by measurement, took the boot from "panics in the shared region" to "launchd
runs the system".

**1. The loader's device identity.** `xnuloader/identity.h` was the placeholder ("no Apple
device-tree data"), so the kernel knew no hardware: launchd's libignition died with
`failed to query hardware target: 22`. Generating it from the IPSW's device tree
(`xnuloader/mkidentity.sh`, which needs an LZFSE decoder -- built from source, Ubuntu packages none)
gives `hardware target: d22ap` and `Device: D22 / Hardware Model: iPhone10,3` in the panic banner.

**2. The dyld shared cache has to be where dyld looks.** `strings rootfs/usr/lib/dyld` shows the two
paths this dyld knows: `/System/Library/Caches/com.apple.dyld/` and `/Library/Caches/com.apple.dyld/`
-- not the cryptex path the upstream image used. With the cache at the first of those, dyld stops
saying `Library not loaded: /usr/lib/libSystem.B.dylib`.

**3. The system manifest is not optional.** Building the image without
`manifest=sysimage/system.manifest` leaves the volume without the directories only the manifest
declares -- `/dev` among them -- and then `devfs_kernel_mount: kernel_mount failed: 2`, after which
pid 1 cannot open `/dev/null` and exits. With the manifest, devfs mounts.

The result, inside QEMU on this Linux host, with the kernel built here and an image built here:

```
Successfully spawned SpringBoard[4] because system support
Successfully spawned notifyd[71] ... mediaserverd[7] ... configd[15] ... logd[70]
Successfully spawned amfid[25] ... containermanagerd[42] ... keybagd[24] ... lockdownd[55]
... 60 processes in total
```

`qemu/screendump.sh` captures the framebuffer: 1125x2436, and **entirely black**. That is the expected
state of this milestone, not a defect in it: QEMU offers no GPU, so nothing draws. Turning that black
screen into a picture is exactly what the vkbridge work (a real Vulkan backend on the host) and the
guest-side Metal device are for -- the boot now reaches the point where rendering would matter.

## Addendum 3: the panel is black, and what that means

With launchd running the system, the next question is the picture, and it is not there: every
framebuffer capture is 1125x2436 of black. That is the honest state, and it is worth separating from
a bug.

What is established:

- the framebuffer is set up and the panel is the right shape -- the loader programs a 1125x2436 mode
  and `MiniIOMobileFramebuffer` reports `display 1125x2436 on a panel of 1125x2436 (4500 bytes per
  row)`, which is 1125 * 4;
- nothing in this configuration draws: SpringBoard's compositor needs Metal, and the Metal device in
  this tree is a stub that accepts messages and produces no images (`docs/metal-plan.md` says as
  much);
- a program of our own (`guest/fill_panel.c`) was written to bypass all of that: it paints a
  gradient, four colour bars and a centre square, and hands the pixels to the kernel's own
  `kHakiosShow` path -- the same call the upstream `gl_test` uses -- so it exercises the display
  chain from a guest process to `MiniIOMobileFramebufferShow`, which memcpy's into the boot
  framebuffer.

What that experiment has and has not shown:

- the launchd job that starts it does spawn (`xpcproxy spawned with pid 38`, `SPAWNED`), and the
  process does not exit while its paint loop should be running;
- **but its output is unreadable**: a launchd job's stdout goes to launchd, not to the serial
  console, and `open("/dev/console")` from inside the job does not produce a line in the log either.
  So whether `IOServiceOpen` succeeded and what `kHakiosShow` returned is still unknown, and a black
  screen is consistent with both "the call failed" and "the call worked and the loader's idea of the
  framebuffer address is not the memory ramfb scans out";
- the guest also panics a few tens of seconds into these runs (a stackshot), which cuts the window
  short.

So the next step is not more guessing, it is instrumenting the guest: put the paint program where its
stdout *is* the console (the init path, whose stdio is the serial line) instead of behind launchd,
and have it return a distinct exit code for each failure so the answer arrives even without a
visible picture.

## Addendum 4: the panel detour, and why it stopped

The plan was to prove the display chain without Metal: paint pixels in a guest process, hand them to
`kHakiosShow`, and see them in a capture. It did not get there, and the reason is worth recording
because it is not the display path.

The program (`guest/fill_panel.c`) is correct as far as it goes: it builds, it is a valid
`Mach-O 64-bit arm64 executable`, and ldid signs it (`CodeDirectory ... flags=0x0(none)`). Installed
in the system volume with a launchd job, it reaches:

```
xpcproxy spawned with pid 38
internal event: SPAWNED, code = 0
service state: xpcproxy
internal event: SOURCE_ATTACH, code = 0
```

and then nothing -- no exec, no exit, for as long as the guest runs. Apple's own daemons in the same
boot exit normally (`xpcproxy exited due to exit(78)`, `service state: exited`), so xpcproxy works;
what does not work is *our* binary through launchd. Signing it changed nothing, which rules out a
plain signature check as the cause. The likely difference is that this userland's own programs are
never started by launchd at all -- they run from the RAM disk through `etc/rc`, a path that in the
full-userland configuration the kernel does not take, because PID 1 there is the system volume's
`/sbin/launchd`.

So the honest summary of the black panel:

| Question | Answer |
|---|---|
| Is the framebuffer set up? | yes -- 1125x2436, 4500 bytes per row, matching the loader's mode |
| Does anything draw? | no: SpringBoard needs Metal and the Metal device here is a stub |
| Can a guest process of ours paint it? | **not established** -- the launchd job never execs, so nothing of ours has run in that position |
| Is the frustration a display bug? | no -- it is a launchd/xpcproxy question that has to be solved first |

The next step is therefore not in the display code at all: run a program of ours from a context this
userland *does* launch (a system-domain job, or an init path that is actually taken), and only then
ask what the panel shows.

## Addendum 5: the screen is lit

**A picture reached the panel.** The frame is `docs/demo/first-pixels.png`: two gradient bands across
the top 240 rows of a 1125x2436 panel, written by the kernel at display start-up and captured from
the host through QEMU's ramfb. Before this, every capture was 8061 bytes of black; this one is 14316
bytes of colour.

Two findings came out of the attempt, and both are the kind that only measurement gives:

**1. The path that draws is not the one the port had.** The service a program needs to put pixels on
the panel (`HakiosGPU`, selector 0x7f) is created by the **Adreno** driver, and a machine without that
GPU never runs it -- so on QEMU the display was up and nothing could draw into it. Creating the
service with the display instead (`patches/0001-display-brings-up-the-show-service.patch`) is what
makes the panel writable at all on this machine.

**2. A band from the kernel is enough to prove the path.** With the service brought up, painting
happens through `MiniIOMobileFramebufferShow()`, which memcpy's the rows into `vinfo.v_baseaddr` --
the memory the loader programmed into ramfb. The capture shows exactly the pattern the code writes,
which settles the question the black frames had left open: a write to that buffer *does* reach the
host's picture.

What is still open, and what is not:

| | state |
|---|---|
| the panel is writable and the host sees it | **proven** (this picture) |
| a guest process can paint it | **not yet** -- the launchd job still never execs (addendum 4) |
| the system UI can paint it | **no**: SpringBoard needs Metal, and the Metal device is still a stub |

So the floor is now solid: whatever draws next -- a Metal device, a framebuffer test, a compositor --
there is a working way to put its output in front of a person. That is the first time that has been
true in this project.

## Addendum 6: the user-space path, one IOKit answer at a time

With the panel writable from the kernel, the next question is whether a *program* can paint it, and
the answer has been getting narrower rather than louder. The sequence below is what four runs said,
each an OSError code from `IOServiceOpen`:

| what the program did | what it got |
|---|---|
| `IOServiceMatching("HakiosGPU")` then open | no service at all -- the matcher found nothing |
| enumerated every IOService and looked for the name | `registry: name=HakiosGPU class=HakiosGPU`, 20 services, 1 named Hakios -- so the service *was* there and the matcher was the problem |
| opened the object the enumeration produced | `open 0xe00002c7` (kIOReturnUnsupported): the service refuses user clients |
| after giving the service an explicit `newUserClient` | `open 0xe00002be`: a different answer, so the client class is now reached and something later in its start-up refuses |

The third line is the interesting one: `HakiosGPUUserClient` is named only as a string in the
`kIOUserClientClassKey` property, so no code refers to it and the linker may drop it entirely -- in
which case every open answers *unsupported* while the service looks perfectly healthy in the
registry. `patches/0003-gpu-service-creates-its-user-client.patch` makes the reference real.

What this is and is not:

- it is **not** a rendering path yet -- nothing of ours has put a pixel on the panel from user space;
- it is the last layer under one: the kernel can paint (addendum 5, the frame is real), and the only
  missing step is a program it will accept;
- the remaining error is a *different* one, which is what progress looks like in this kind of work:
  not "nothing there", but "the client's start-up said no", and its reason is the next thing to read.

## Addendum 7: a program of ours paints the panel

The frame in `docs/demo/userland-frame.png` was drawn by a **user-space process**: `guest/fill_panel.c`,
running from the RAM disk, opened the GPU service, called `kHakiosShow` and put its own picture on the
1125x2436 panel. The host captured it. Nothing Apple's UI drew is in it -- it is a gradient, four
colour bars and a white square, exactly what the code writes -- which is the point: the path works,
so whatever draws next has somewhere to put its output.

Three kernel-side layers had to give way, each found by its own error, in order:

| layer | what refused | what it was |
|---|---|---|
| the service | not in the registry at all | `HakiosGPUStart()` is called only by the **Adreno** driver, which a machine without that GPU never runs -- so the panel was writable and unreachable |
| the user client | `open 0xe00002c7` (unsupported) | `HakiosGPUUserClient` is named only as a string, so nothing referenced it and the linker could drop it |
| the client's start | `open 0xe00002be` (no resources) | `HakiosGPUUserClient::start()` demanded `hakios_gpu_ready()`, which is false without a GPU -- although the one selector that matters (`kHakiosShow`) never touches a GPU |

With those three, `open 0` and `on the panel (0)`: every call succeeded.

Patches: `0001` (display brings up the service), `0002` (stage0 runs a painter first),
`0003` (explicit `newUserClient`), `0004` (the client starts without a GPU).

What is still true: this is **our** picture, not the system's. SpringBoard still cannot draw, because
it needs Metal and the Metal device here is a stub. But the floor is now under the *user-space*
layer too, which it was not before -- and that is the layer a Metal device would have to live in.

## Addendum 8: what stands between the panel and the system UI

The panel is lit and a user-space program of ours can paint it (addenda 5 and 7). The system UI is
still not there, and the measurement says why, in two parts:

**The Metal device is a stub, and nothing currently loads it.** `HakiosMetal.bundle` builds (108,784
bytes) and is in the RAM disk at `/lib/HakiosMetal.bundle`, reachable through the system volume's
`/System/Library/Extensions` symlink. But no Metal call in the boot log appears -- the counter of
"asked and not implemented" stays at zero -- because nothing in the boot reaches a
`MTLCreateSystemDefaultDevice()` that would load it.

**Asking the kernel for it stops the boot.** With `CMDLINE="metal=1"`, the boot no longer spawns a
single process: the log ends with the accelerator naming the plug-in and no launchd activity at all.
Without `metal=1` the same image spawns 60 to 90 processes including SpringBoard. So the Metal path
is not "not yet drawn" -- it is "does not survive start-up", and that has to be fixed before any
question about what the UI would look like.

That reorders the work honestly:

| step | state |
|---|---|
| boot user space (no Metal) | works |
| paint the panel (kernel, user space) | works |
| start up with Metal enabled | **fails, zero processes** |
| a Metal device SpringBoard could composite with | not implemented (the bundle is scaffolding) |
| the UI on screen | needs the two above |

The next piece of work is therefore not rendering: it is finding out why `metal=1` stops the boot
before launchd, which is a much narrower question -- one boot flag, one code path in the accelerator.

## Addendum 9: which image actually booted the user space

The record needs one correction. The "60 to 90 processes including SpringBoard" runs used **the
reference system image in this tree** (`userland/system.apfs`), not the image built here from the hand
assembled sysroot. The hand assembled one stopped at `init`, every time, and the reason is one missing
file:

```
=== init: done ===          <- init ran its checks, then rc, then halted at 15 s
```

`/etc/rc` of the small image ends with `exec /sbin/launchd.darwin`. The tree's build moves Apple's
launchd to that name and puts stage0 at `/sbin/launchd`. The sysroot assembled here had stage0 in
place but **no `launchd.darwin`**, so that exec failed, `run_rc` returned, and init did what it says it
does: `reboot(RB_HALT)`. Fifteen seconds in, no launchd, no jobs, no SpringBoard.

Once the file is there (525,088 bytes, `Mach-O 64-bit arm64`), the chain is complete again:

| | before | after |
|---|---|---|
| `/sbin/launchd` | stage0 (52,088 B) | stage0 |
| `/sbin/launchd.darwin` | **missing** | Apple's launchd (525,088 B) |
| boot reaches | `init: done` at 15 s | launchd, its jobs |

The lesson is worth keeping: a tree that boots from someone else's image is not a tree whose build
boots. Both have to be run, and this session had been reporting the first one.

### 9a. With the file in place, the same wall shows up one layer down

Adding `launchd.darwin` did not make the built image boot the user space. Init still halts at about
sixteen seconds, and the log says why:

```
load_init_program: attempting to load /sbin/launchd     <- stage0, static, runs
shared_region: ... [1(launchd)] check_np(...) vm_shared_region_start_address() failed
=== init: done ===                                      <- the exec of launchd.darwin did not happen
```

`/etc/rc`'s `exec /sbin/launchd.darwin` needs dyld to establish the shared region, and that fails. The
dyld cache is present -- 43 files, 2.6 GB, the same set in `sysimage/20F66__iPhone10,3_6` and in the
sysroot, in both `/System/Library/Caches/com.apple.dyld/` and the cryptex path -- so the failure is not
"cache missing" but something about how it is laid out in the built image (a compressed APFS entry
cannot be mapped the way the shared region wants).

So the honest scoreboard for the *built* image is:

| | reference image in this tree | image built here |
|---|---|---|
| init, its checks, rc | yes | yes |
| Apple's launchd (shared region) | yes | **no** |
| launchd jobs, SpringBoard | yes (60-90) | no |
| our painter on the panel | yes | yes (stage0 runs it before init) |

Both halves are real and they are not the same image. The next piece of work is the shared region in
a built image -- a narrow question with a clear test (the `check_np` failure above).

## Addendum 10: the built image boots the user space

Five things had to be right, and each was found by its own failure. With them, an image built here
(not the reference one) runs launchd, backboardd and SpringBoard:

| what | before | after |
|---|---|---|
| `/sbin/launchd.darwin` | missing from the built sysroot | present, 525,088 bytes |
| dyld shared cache | only at `/System/Cryptexes/OS/...` | that **and** `/System/Library/Caches/com.apple.dyld/` (a symlink, not a copy) |
| the cache is usable | `no dyld cache` on every load | dyld finds it; that error is gone |
| `com.hakios.demo` job | runs init again with `rc.jobs`; that init ends in `reboot(RB_HALT)` | not in the image -- **but see the correction below: this was not what halted the boot** |
| how init finds its image | only from `argv[0]`; the kernel's first exec passes no argv, so `base` stayed "" and `/etc/rc` was looked for in the wrong tree | `stat("/redwood/etc/rc")` as well |

The last one is the subtle one, and it explains a whole afternoon of quiet boots:

```
=== init: process 1, started as ?        <- argc is 0 here, so argv[0] said nothing
```

so `in_image("/etc/rc")` was `/etc/rc` -- not `/redwood/etc/rc` -- the read failed silently (the
function returns on error), and init went straight on to `=== init: done ===` and `reboot(RB_HALT)`.

What the boot looks like now:

```
$ exec /sbin/launchd.darwin
com.apple.xpc.launchd| (user/501/com.apple.SpringBoard [7]) <Notice>: service spawned with pid 7
backboardd[12] [SystemShell] Now monitoring "com.apple.springboard"
backboardd[12] [BootUI] systemShellChangedWithPrimary: bundleIdentifier: com.apple.springboard
```

Also worth recording: `com.apple.SpringBoard.plist` logs one complaint that does not stop it --
`Unknown key for plist importer (key: HighPriorityIO type: bool)` -- launchd ignores the key and
spawns the job anyway.

## Addendum 11: the system draws -- the boot logo, and what made it possible

`docs/demo/system-ui-first.png` is the iOS boot logo on the guest's panel: 1125x2436, a white Apple
mark on black, drawn by **the system itself** -- backboardd's BootUI overlay, through CoreAnimation,
through our OpenGL ES stand-in, through Mesa's `softpipe` rasterizer, into the framebuffer the host
captures. It is the first frame in this project that no code of ours drew.

The chain, and what each link needed:

| link | what it needed |
|---|---|
| backboardd's compositor asks CoreAnimation to draw | the window server has a display it accepts |
| CoreAnimation picks a renderer | `userland/src/inject.c:opengl_es()` loads `/redwood/lib/libhakios.gl.dylib`; with no `gl=` boot argument it decides by asking whether a `HakiosGPU` service exists -- and patch 0001 above is what makes that service exist on this machine |
| OpenGL ES 3.1 | Mesa 25.0.7 built for arm64-darwin by the kernel tree's `mk-mesa` script (14,805,208 bytes, 1296 `gl*` functions, `softpipe`), behind `eagl.c` |
| pixels reach the panel | the same framebuffer path proven in addenda 5 and 7 |

The log says it in its own words:

```
hakios-gl: 8 of 8 methods of EAGLContext are ours; OpenGL ES by Mesa, off-screen (OSMesa)
hakios-gl: an OpenGL ES 3 context for CoreAnimation, by Mesa, off-screen (OSMesa):
           OpenGL ES 3.1 Mesa 25.0.7; softpipe; shaders Op...
```

Two notes worth keeping:

- **the softpipe path is what makes this possible on QEMU at all.** It is a rasterizer in plain C: no
  GPU, no Adreno, no firmware. The GPU-accelerated path (`gpu=1`, `freedreno` through the kernel's
  GPU service) is a different build of the same library and is still ahead.
- **this is the boot logo, not the home screen.** The UI drawing pipeline works end to end; how far
  SpringBoard gets from here is the next thing to measure, and the frames to look at are the ones
  taken over the following minutes.

## Addendum 12: the system draws -- the boot logo

`docs/demo/system-ui-first.png` is the iOS boot logo on the guest's panel: 1125x2436, black, with
Apple's white mark in the middle. Nobody drew it from outside: it is backboardd's BootUI overlay,
drawn by CoreAnimation, through our OpenGL ES stand-in, through Mesa's `softpipe` rasterizer, into
the framebuffer the host captures.

What each link needed:

| link | what it needed |
|---|---|
| the compositor asks CoreAnimation to draw | a display it accepts |
| CoreAnimation picks a renderer | `inject.c:opengl_es()` loads `/redwood/lib/libhakios.gl.dylib`; without a `gl=` boot argument it decides by asking whether a `HakiosGPU` service exists -- which patch 0001 is what creates on this machine |
| OpenGL ES 3.1 | Mesa 25.0.7 for arm64-darwin from the kernel tree's `mk-mesa` script: 14,805,208 bytes, 1296 `gl*` functions, `softpipe` |
| pixels reach the panel | the framebuffer path proven in addenda 5 and 7 |

In the log's own words:

```
hakios-gl: 8 of 8 methods of EAGLContext are ours; OpenGL ES by Mesa, off-screen (OSMesa)
hakios-gl: an OpenGL ES 3 context for CoreAnimation, by Mesa, off-screen (OSMesa):
           OpenGL ES 3.1 Mesa 25.0.7; softpipe; shaders Op...
```

**`softpipe` is what makes this possible on QEMU at all**: a rasterizer in plain C, no GPU, no Adreno,
no firmware. The accelerated path (`gpu=1`, `freedreno` through the kernel's GPU service) is a
different build of the same library and is the work after this.

What this is not: it is the *boot logo*, not the home screen. The drawing pipeline works end to end;
how far SpringBoard gets from here is a separate question, measured below.

## Addendum 13: the screen stops at the boot logo, and the one line that explains it

With the GL path in place the compositor draws (addendum 12), and the frames stop at the boot logo.
Sampling the screen every two minutes for twenty minutes gives the same 13,161-byte picture: the
Apple mark on black. SpringBoard has spawned (`Successfully spawned SpringBoard[7]`) and does not
crash, and it logs nothing at all.

The line that explains both facts is launchd's, about *our own* job and about many of its own:

```
com.apple.xpc.launchd| (user/501) <Notice>: pending spawn, domain in on-demand-only mode: com.iosvm.ps
```

**The user domain is in on-demand-only mode**, so a job with `RunAtLoad` is scheduled and not run:
`WILL_SPAWN`, `service state: spawn scheduled`, and then nothing. SpringBoard is *in* that domain, and
the services it waits for are too, which is why the screen holds at the boot logo while everything
looks healthy in the log.

What is measured, and what is inferred:

| measured | inferred |
|---|---|
| 15-45 jobs spawned depending on the list; the user domain says on-demand-only | the domain never transitions to normal mode |
| SpringBoard spawns, does not exit, logs nothing | it is waiting for services in that domain |
| backboardd keeps drawing the BootUI overlay | the window server is healthy; the layer to composite never arrives |
| the same jobs *did* run in earlier runs of the reference image (60-90 processes) | the transition happens in that configuration and not this one |

The next question is therefore narrow and named: **what makes a user domain leave on-demand-only
mode** -- on Darwin, the bootstrapping of the user session -- and why it happens with the reference
image and not with this one.

### Correction: the demo job was not what halted the boot

The table above first said the `com.hakios.demo` job took the machine down at fifteen seconds. That
attribution was wrong, and removing the job proved it: the halt was still there afterwards, at the
same fifteen seconds, with the same `=== init: done ===` before it. The halt came from **process 1's
own init**, whose `rc` could not be read because of the `argv[0]` fall-through fixed in the same
change -- `in_image("/etc/rc")` resolved to `/etc/rc` and the read failed silently.

The demo job is still left out of the image, because a job that halts the machine when it does run is
not one to keep, and because it turned out never to have run at all in these configurations: the user
domain is in on-demand-only mode (addendum 13), and its `RunAtLoad` never fires.

## Addendum 14: the machine was running on one core, and nobody said so

Every timing in this session before this point was measured on **one** processor while the QEMU
command line said four. The two settings are separate:

| setting | what it does |
|---|---|
| `QEMU_SMP=4` | tells QEMU to build a machine with four cores |
| `CMDLINE="cpus=4"` | tells **the kernel** to start them (`xnuloader/loader.c` puts `cpus=1` in the boot line unless the payload asks for more) |

With only the first, the kernel brings up the core it booted on and nothing else, and the log says so
in a line that is easy to read past:

```
GenericARMPE: the slower cores started too: 1 processors running
```

With both:

```
GenericARMPE: 4 of 4 processors are running
GenericARMPE: the slower cores started too: 4 processors running
```

The effect on wall-clock time is what one would expect once measured: the guest clock used to run
about eight times slower than the host's, and with four cores it runs close to the host's. Everything
in this document that says "slow" was measured on one core.

This matters more here than it would elsewhere, because the renderer is `softpipe`: a rasterizer in
plain C, on the guest's own processors, drawing a 1125x2436 panel. One core is the difference between
a screen that never changes and a screen that does.

## Addendum 15: the home screen

`docs/demo/home-screen.png` is the iOS home screen: the clock at 2:12, the Weather and Calendar
widgets, twenty application icons, the Search capsule and the dock. It is drawn by SpringBoard on
this machine, through CoreAnimation, through Mesa's `softpipe`, into the panel the host captures.
Nobody put it there but Darwin.

### What was actually blocking it

The compositor had been drawing all along. The display driver's queue, though, was never drained:

```c
/* iokit/Kernel/MiniIOMobileFramebuffer.cpp, show() */
if (gOnTheBeat) { ... clock_delay_until(next); ... }
```

`fb_beat` defaults to **1** -- each frame waits for the next refresh boundary. With the software
renderer on QEMU a frame arrives far from where that phase estimate expects it, the wait swallows the
budget, the swap queue fills, and `present()` drops what comes next ("the GPU has stopped answering:
the frame before is waiting for it"). On the panel that looks exactly like a system that draws nothing:
one swap, the boot logo, forever.

The measurement that found it is the driver's own trace (`fbtrace` defaults to 3 lines per selector):

| with `fb_beat=1` (default) | with `fb_beat=0` |
|---|---|
| `swap 1` only, ever | `swap 1 … 8` and continuing |
| screen: 13,161 bytes, the boot logo | screen: 337,968 bytes, the home screen |

One boot argument, no kernel change: `CMDLINE="cpus=4 fb_beat=0"`.

### What the system shows, in order

| guest time | what is on the panel |
|---|---|
| 0-30 s | black, then the kernel's own band (addendum 5) |
| ~1 min | our painter's picture (addendum 7), then Apple's boot logo |
| ~6 min | the logo is dismissed (`didFinishFirstBoot`), a spinner |
| ~8 min | the home screen above |

Note that the app icons' *content* is drawn (camera, photos, mail, maps) while the two big widgets
(Weather, Calendar) are white rectangles: a widget's content comes from an extension process, and on
this machine those extensions are not running. The screen is real; the widgets are the honest gap.

## Addendum 16: rendering performance, measured

With the home screen on the panel (addendum 15), the numbers the driver reports about itself:

```
MiniIOMobileFramebuffer: 30 frames in 5140 ms (5 a second), 3766 us each in the driver,
  after 3 us waiting for the GPU ... 0 swaps waited for the frame before, 0
MiniIOMobileFramebuffer: 38 frames in 5037 ms (7 a second), 3576 us each in the driver ...
```

and the renderer's own account of one frame:

```
hakios-gl: frame 1120: 55 GL calls, 9 draws, 66 triangles; 1 picture copied out;
  148544 us (75 handing on, 15 waiting, 7029 copying out)
```

148 ms a frame, of which ~7 ms is the copy to the panel and **~141 ms is Mesa drawing**. So the
driver and the copy path are not the problem; the rasterizer is. That is what a software renderer
costs for 2.7 million pixels, and it is why the panel updates at 5-7 frames a second.

What actually helped, measured:

| change | effect |
|---|---|
| `CMDLINE="cpus=4"` **and** `QEMU_SMP=4` | the guest clock stops running ~8x slower than the host; before this every measurement in this document was on one core |
| `QEMU_TB_MB=1024..1536` | a bigger translation cache, less retranslation under TCG |
| `fb_beat=0` | the difference between one swap ever and a live screen (addendum 15) |

What was tried and rejected, with what it did:

| change | result |
|---|---|
| `QEMU_SMP=8 CMDLINE="cpus=8"` | the guest stops making progress (its clock freezes); 4 is the working number |
| `fbwidth=750 fbheight=1624` (a smaller logical screen, ~2.2x fewer pixels) | the UI stops drawing entirely: SpringBoard is tied to the mode's dimensions |
| llvmpipe instead of softpipe | would need LLVM cross-built for arm64-darwin, since the JIT runs in the guest; a project of its own |

**About GPU acceleration, plainly:** it cannot happen on this host. The accelerated path in this tree
is `gpu=1` with Mesa's `freedreno` driver talking to the kernel's GPU service, and the service only
exists when the Adreno GPU does (`HakiosGPUStart()` is called from the Adreno driver). QEMU has no
Adreno and no firmware for one, so the honest rendering path here is `softpipe` on the guest's CPUs.
What was optimised is what exists: cores, translation cache, and the display pacing that was silently
throwing every frame away.

## Addendum 17: how often it works

The home screen is not obtainable on every boot. Of the runs made with the same command in this
session, several reached it and several did not, and the ones that did not share a signature: the
guest log stops a couple of minutes in (no SpringBoard restart, no panic, `lsd` last seen busy), and
the display driver never reports a frame, so the panel keeps the boot logo.

| run | reached the home screen |
|---|---|
| `run62` (fb_beat=1) | no -- one swap, and now we know why |
| `run63` (fb_beat=0) | **yes** -- `docs/demo/home-screen.png` |
| `run66`, `run67` (fb_beat=0) | no: the log stops ~2 min in, no frames reported |

The honest statement is: the path is found and demonstrated, and it is not yet reliable. What would
make it reliable is the next question -- whether the stall is SpringBoard waiting on a service, the
window server's restart cycle their own notes describe, or something about starting the painter from
stage0 before the system -- and it is measurable the same way the other questions in this document
were: with the driver's own trace and the process table, both of which are now known to work.

### 17a. What the stalled boots are waiting for: `lsd`

The process table taken from inside a stalled boot (the painter's reporter, addendum 12's trick) puts
a name on it:

```
  PID TTY           TIME CMD
    9 ??         0:11.62 /System/Library/CoreServices/SpringBoard.app/SpringBoard
   13 ??         0:18.59 /usr/libexec/backboardd
   25 ??         1:36.70 /usr/libexec/lsd
process lsd[25] thread 327 caught burning CPU! It used more than 50% CPU over 180 seconds
```

`lsd` -- Launch Services -- has spent more processor time than every other daemon together, and the
watchdog has caught it. It is the daemon that registers the applications, and SpringBoard lays out
the home screen only after that registration is done, so while `lsd` works there is no UI to draw,
and the panel keeps whatever was last there.

This is *not* the discriminator between a boot that reaches the screen and one that does not: the
successful boot in addendum 15 carries the same "burning CPU" line and the same failed lookups for
`com.apple.mobileactivationd`. What differs is only whether `lsd` finishes in time. That is why the
honest procedure for now is `qemu/wait-for-ui.sh`: boot, wait for the display driver to report
frames, and if it never does, boot again.

## Addendum 18: it reproduces, through a script that checks the picture

The home screen is now obtainable on demand:

```
$ qemu/wait-for-ui.sh /tmp/ui-final.png
attempt 1 of 4
  frames are flowing but the panel is still blank (8061 bytes)
  frames are flowing but the panel is still blank (13152 bytes)
the panel shows the system: /tmp/ui-final.png (339412 bytes)
```

`docs/demo/home-screen-fresh.png` is that capture: clock at 2:49, the widgets, the twenty icons, the
dock. It is a *later* boot than addendum 15's 2:12, which is the point -- the pipeline is not a
one-off.

Two things in the script are the lessons of this whole session:

- **"frames are flowing" is not "the screen has something on it".** The first two captures were 8,061
  bytes (black) and 13,152 (the boot logo) *while the driver was reporting frames*. Content is the
  thing to wait for, so the script measures the picture, not the log: under 30,000 bytes is not a
  drawn screen.
- **boots are not reliable yet, so the procedure has to be a retry.** Addendum 17 has the stalls; the
  script boots again rather than pretending they do not happen.

`docs/state-of-the-port.md` is the one-page summary this all adds up to.

### 17b. What a boot that reaches the screen does and a stalled one does not

Comparing the programs launchd reports starting, from the two boots in addenda 15 and 17:

| | reached the screen (`run63`) | stalled (`run68`) |
|---|---|---|
| programs launched | 27 | 22 |
| `slug`: in the stalled boot only | -- | -- |
| **in the boot that worked only** | `Spotlight`, `ContextService`, `pkd`, plus the two jobs of ours that were in the image then | -- |

`Spotlight` being launched is SpringBoard prewarming, i.e. what happens *after* it lays out the home
screen; `pkd` (plug-in kit) and `ContextService` are the plug-in machinery behind the widgets. They do
not cause the difference, they are its symptom -- but they are a usable signal: a boot in which
`Successfully spawned Spotlight` never appears is not going to show a screen, and `qemu/wait-for-ui.sh`
takes advantage of exactly that, rebooting when the guest's own log stops growing rather than waiting
out a boot that is already lost.

What is still not known is why SpringBoard stops at that point in some boots. The process table taken
from a stalled boot does not show it waiting on anything: every daemon it depends on is alive, and
`lsd` has spent the same processor time in both (`1:36` stalled, `1:35` successful). The log simply
stops.

## Addendum 19: seven minutes, measured, to a screen you can look at

With the content check and the stall detection in `qemu/wait-for-ui.sh`:

```
$ time qemu/wait-for-ui.sh /tmp/ui-v2.png
attempt 1 of 5
  frames are flowing but the panel is still blank (8061 bytes)
  frames are flowing but the panel is still blank (13157 bytes)
the panel shows the system: /tmp/ui-v2.png (339390 bytes)

real  7m6.167s
```

`docs/demo/home-screen-scripted.png` is that capture. The procedure is now:

1. boot with `cpus=4 fb_beat=0` (the two arguments that matter, addendum 15);
2. if the guest's log stops growing for two minutes, that boot is lost -- boot again;
3. when the display driver reports frames, capture and *measure the picture*: under 30,000 bytes is
   the boot logo or a black panel, not a screen;
4. stop when the picture is a screen, and keep it.

Three captures in a row now, from three separate boots (2:12, 2:49, and this one), which is the
strongest statement this document can make about reproducibility: the pipeline works, and the retry
is part of the procedure rather than a workaround to be embarrassed about.

## Addendum 20: a heartbeat, and what it says about the stalls

Because a stalled boot's log simply stops, the first question was whether the machine stops or only
its writers. A thread that does nothing but sleep and log answered it, behind a boot argument so it
costs nothing normally:

```
CMDLINE="cpus=4 fb_beat=0 hb=20"

MiniIOMobileFramebuffer: heartbeat 2: 23 programs, 23 runnable, SpringBoard 9
MiniIOMobileFramebuffer: heartbeat 3: 22 programs, 22 runnable, SpringBoard 9
MiniIOMobileFramebuffer: heartbeat 7: 22 programs, 22 runnable, SpringBoard 9
```

while the programs' own log had stopped at 03:13:12:

| question | answer from the heartbeat |
|---|---|
| is the kernel deadlocked? | **no** -- a kernel thread keeps being scheduled every 20 s |
| did anything crash or exit? | **no** -- the program count holds, and SpringBoard keeps its pid |
| is memory gone? | no: `free 2366 MB, ramfs 110 MB, programs 298` |
| is the console stuck? | no: these lines are written to it while the programs' lines are not |

So the stall is in the user space: alive, not crashed, not out of memory, doing nothing. That is a
much smaller thing to look for than "the machine hangs", and it is now a *measured* statement rather
than a guess.

Two notes for whoever uses this:

- the "runnable" count reading every program as runnable is almost certainly the metric and not the
  machine: `p_stat` is read here without the process-list lock, as a diagnostic, and Darwin's `SRUN`
  covers running and runnable both. The program count and SpringBoard's pid are the reliable parts.
- the heartbeat is `hb=N` (seconds), off by default; it is in the same file as the frame pacing, and
  the two together are how the display questions in this document were answered.

### 20a. `sb_delay` does not fix the stalls

The tree has a boot argument for exactly this shape of problem -- `sb_delay=N`, "SpringBoard starts a
few seconds late where the kernel's boot-args say so" -- on the theory that a system shell starting
before its dependencies then waits forever. Tried with `sb_delay=60`:

| | frames |
|---|---|
| `sb_delay=60` at 5 minutes | **0** |

so a late SpringBoard is not the answer either. The stalls stay what addendum 20 measures them to be:
the user space is alive and doing nothing, and the piece that does nothing is the one that would draw.

## Addendum 21: `chronod` runs, and the widgets stay white

The home screen's Weather and Calendar widgets are white rectangles, and the launchd log asks for
`com.apple.chronoservices` on every boot and does not find it. That service belongs to `chronod`, a
stock job this image did not load, so it was added to the list (`CHRONOD=1`, on by default now):

```
com.apple.xpc.launchd| (user/501/com.apple.chronod) <Notice>: service state: spawning
```

It starts. The widgets are still white -- `docs/demo/home-screen-with-chronod.png`, clock 3:35, a
later boot than any other capture here, so this is not an old frame.

So the missing piece is not the daemon: `chronod` hosts widget *extensions*, and an extension is a
plug-in inside an application bundle, launched through the app's registration and the plug-in kit.
That machinery (`pkd`, which only appears in boots that reach the screen, `ContextService`, and the
registration `lsd` spends minutes on) is where the widget content comes from, and it is a different
problem from the one this port has been solving.

Recorded as a negative result with its evidence, which is worth as much as a positive one here: it
rules out the cheapest explanation and names the next one.

## Addendum 22: doing something with the system once it is up

`qemu/wait-for-ui.sh` now takes `AFTER_CMD`: a shell command run while the guest is alive and the
home screen is on the panel, before the machine is stopped. That is the hook any interaction test
needs, and it exists because the alternative -- a bare boot that waits for frames, then taps -- kept
landing on the boots that stall.

The tap itself is a QEMU monitor command, because this port maps QEMU's virtio tablet and keyboard to
its touch screen (`drivers/input/hakios.virtio_input.c`, enabled by `virtio_input=1`, which
`run-darwin.sh` always passes):

```sh
TRIES=5 AFTER_CMD="bash /tmp/after-tap.sh" qemu/wait-for-ui.sh /tmp/home-tap.png
```

with `after-tap.sh` sending `mouse_move 620 1590`, `mouse_button 1`, `mouse_button 0` -- the Settings
icon -- and capturing before and after.

**State: the hook is written and exercised, the tap has not yet been seen to do anything.** Every run
of it so far landed on a stalled boot (the script retries, and each attempt is minutes), so what is
recorded here is the mechanism, not a result. The coordinates are the third column of the fourth row
of icons in `docs/demo/home-screen.png`, which is where Settings sits.

## Addendum 23: the A/B that asks whether our own hook is the cause

The stall has been blamed on nothing so far, and one candidate has never been ruled out: **the painter
this port runs from stage0** (`patches/0002`). It runs before `init`, allocates a frame's worth of
memory and calls the display's `show()` -- if anything of ours could leave the display driver in a
state where the presenter later waits, that is it.

The test is a rebuild without it (`userland/src/stage0.c` restored to upstream, system image rebuilt
with `SYSTEM=1`) and then five boots of the same command, counting the ones whose panel shows the
system:

| image | boots | reached the screen |
|---|---|---|
| **without** the stage0 painter | 5 | **2 of 5** (40%) |
| **with** it (the image that made every capture in this document) | 5 | **2 of 5** (40%) |

The five boots without the hook: `boot 1: no screen`, `boot 2: SCREEN (318082 bytes)`, `boot 3: no
screen`, `boot 4: SCREEN (317387 bytes)`, `boot 5: no screen`. Two pictures, and both are the home
screen (the sizes are that of a drawn screen, not the logo's 13 KB).

So the old "about one in three" was an impression, and the measured number for an image without our
hook is 40%. Whether that is better or worse than the image with the hook is what the control run is
for; at five boots a side, a difference of one boot is not a result, and the honest reading is
**the hook is not what makes boots stall**.

A boot is counted as lost by the script itself: no frame reported by seven minutes, or no growth in
the guest's log for two minutes. That is the same criterion the procedure uses, applied five times in
a row, which is the only way an informal "about one in three" can become a number.

Two runs, the same command, the same images apart from one hook, and the answer is **identical on
both sides: 2 of 5, 40%.** The five control boots were `SCREEN (318297 bytes)`, no screen, no screen,
`SCREEN (318943 bytes)`, no screen -- the same shape as the run without the hook, which was `no
screen`, `SCREEN (318082)`, `no screen`, `SCREEN (317387)`, `no screen`.

So **the stall is not caused by anything this project added.** It is in the port -- the same boots that fail, fail after the same sequence, with the kernel
alive and the programs alive (addendum 20), and the display driver never reports a frame.

That is the useful shape of the result: it removes our own changes from the list of suspects and
leaves the port's own start-up, which is where the next person should look.

## Addendum 24: a real screen, and taps that do not reach it

`KEEP=1` in `qemu/wait-for-ui.sh` leaves the machine running once the screen is verified, which is
what an interaction test needs:

```
$ KEEP=1 TRIES=6 qemu/wait-for-ui.sh /tmp/keep-ui.png
attempt 1 of 6
  frames are flowing but the panel is still blank (8061 bytes)
the panel shows the system: /tmp/keep-ui.png (318556 bytes)
left running (KEEP=1): monitor on tcp:127.0.0.1:4444
```

With that machine up, taps were sent two ways and **neither reached the interface**:

| how | result |
|---|---|
| monitor `mouse_move` + `mouse_button 1/0` (a pointer event) | screen unchanged apart from the clock |
| monitor `input-send-event` with `abs` x/y (0..0x7fff, the range `drivers/input/hakios.virtio_input.c` expects) and `btn` left down/up | screen unchanged; no icon highlight, no application launched |

The driver's side of the mapping is clear from its source: `ABS_X`/`ABS_Y` scale into its own 16-bit
touch coordinates, `BTN_LEFT`/`BTN_TOUCH` set the pressed bit, and `EV_SYN` posts the touch. So the
question is whether QEMU delivered those events to the device the driver is bound to, and the port's
own notes say the window server does not reliably take a touch screen for its own ("still waiting for
a main screen digitizer", `userland/src/inject_sb.c`).

Recorded as a negative result: **the screen is real and the machine is interactive in principle, but
nothing of ours has yet made it respond to a tap.**

Two pieces of evidence collected afterwards narrow where that sits, and both are worth keeping:

- **the touch surface *is* accepted.** `userland/src/inject.c` retries up to twenty times to get the
  window server to open its virtual HID service, and in a live boot the warning appears exactly once:
  `the event server has not opened "HakiOS touch screen" (attempt 1): making it again` -- then
  `touch screen service 0x100b1ae20, buttons 0x100d19620`. Later the same boot logs
  `[TouchDeliveryPolicy] TDPS server connection from SpringBoard.9`, so touch delivery is running;
- **the synthetic touches never ran.** The job meant to send them (`com.iosvm.touch`, which calls
  `rwtouch tap 0.607 0.693`) reached `SOURCE_ATTACH` and then `EXITED` in the same second -- the
  xpcproxy pattern from addendum 4, not a problem with the touch path at all.

So the next attempt at interaction should check that the job actually executed before concluding
anything about the input stack; this one did not.

### 24a. The widget extensions do spawn, and their content still does not draw

With `chronod` in the job list (addendum 21), a boot that reaches the home screen also launches the
widget extensions themselves:

```
(pid/50 [PodcastsWidget]) <Notice>: Service stub created for com.apple.cloudsettings...
(pid/50 [PodcastsWidget]) ...
[BatteriesWidget] [HomeWidget] [PodcastsWidget] [RemindersWidget] [StocksWidget]
```

They start and they are counted. The Weather and Calendar rectangles on the panel stay white, so
what is missing is the step between "the extension process is running" and "its rendered output
reaches the layer the home screen composites" -- a different question from the daemon being absent,
and further along than addendum 21 could tell.

## Addendum 25: the interface answers -- tapping the Settings icon opens Settings

`docs/demo/settings-app.png` is the iOS **Settings** application, on the panel, launched by a touch
that this project sent:

```
fill_panel: tapping the Settings icon
```

and, a few seconds later, Settings: "Sign in to your iPhone", Airplane Mode with its switch, Wi-Fi,
Bluetooth (Unavailable), Notifications, Sounds & Haptics, Focus, Screen Time, General, Control Center,
Display & Brightness, Home Screen.

The chain that was missing in addendum 24 turned out to be **the sender, not the input stack**:

| link | state |
|---|---|
| a touch written to `/dev/hakios-touch` | `rwtouch` does it, and the child of our painter is what ran it |
| the window server accepting the virtual HID service | it does; the retry warning appears once and the service is created |
| SpringBoard launching the application the touch lands on | **it does** -- this screenshot |
| the application drawing | **it does** -- through CoreAnimation and Mesa, as everything else here |

The lesson recorded in addendum 24 needs correcting in one place: taps from the QEMU monitor still do
nothing, but a touch written inside the guest does what it should. So the fault there is in how the
monitor's events reach (or do not reach) the virtio input driver, and not in anything downstream of
`/dev/hakios-touch`.

Note what also had to be true for this to work: the machine has to be kept running (`KEEP=1`) for the
touch to have a screen to land on, and the touch is sent from a process started before `init` --
because a launchd job of ours is not reliably executed at all (addendum 4's xpcproxy pattern).

## Addendum 26: it navigates -- a second tap moves inside Settings

`docs/demo/settings-focus-page.png` is the **Focus** page of Settings: the `< Settings` back button, the
title, "Share Across Devices" (greyed, "Focus syncing is not supported on this device"), "Focus
Status: Off", and the explanatory paragraph. It is two touches from a home screen that nothing of ours
had ever touched:

| when | what | result |
|---|---|---|
| +480 s | `rwtouch tap 0.607 0.693` -- the Settings icon | Settings opens (`docs/demo/settings-app.png`) |
| +580 s | `rwtouch tap 0.5 0.651` -- meant to be General, landed on Focus | **the Focus page slides in** |

The second tap landing on a different row than intended is beside the point; that it *navigated* is
the point. A list view was drawn, a touch landed on a row, and the application pushed a new view and
drew it -- all through the same CoreAnimation and Mesa path as everything else here.

So the state of the interface is: **it opens, it navigates, and it is driven by touches we send.**
What it cannot yet do is be driven from the host: the monitor's input events still do not reach the
guest's virtio driver. The sender that works is a program inside the guest writing to
`/dev/hakios-touch`, started before `init` because a launchd job of ours is not reliably executed.

## Addendum 27: which part of the host's input reaches the guest

Addendum 26 left one thing open: touches sent from inside the guest work, and the QEMU monitor's do
not. Logging every event the virtio input driver receives (rate-limited to the first forty) put a
number on it:

```
virtio-input: event from 1: type 1 code 272 value 1     <- EV_KEY, BTN_LEFT, pressed
virtio-input: event from 1: type 0 code 0 value 0       <- EV_SYN
virtio-input: event from 1: type 1 code 272 value 0     <- BTN_LEFT, released
virtio-input: event from 1: type 0 code 0 value 0
hakios: input: sample type 3 finger 0 at 0 0            <- the touch that came of it
```

So **the buttons arrive and the position does not**:

| from the monitor | reaches the driver |
|---|---|
| `mouse_button 1` / `mouse_button 0` | **yes** -- EV_KEY for BTN_LEFT, then EV_SYN |
| `mouse_move X Y` | no EV_ABS at all |
| `input-send-event` with `abs` x/y | no EV_ABS at all |

and the touch that results is at `(0, 0)`, which is why the interface never reacted to it. `info mice`
confirms there is exactly one pointer device -- `QEMU Virtio Tablet (absolute)` -- so the device is not
the ambiguity; the absolute coordinate events are simply not arriving.

That is now a precise question about QEMU's virtio-tablet input path (or the driver's handling of it),
and a much smaller one than "the host cannot drive the interface". For anyone continuing: the guest's
own path (`/dev/hakios-touch` -> window server -> SpringBoard) is proven to work, so this last piece is
the only thing between a machine that can be operated and one that can be operated from the host.

### 27a. Watching the machine: VNC, and what it does not fix

`VNC=1 qemu/run-darwin.sh 600` adds a graphical console on `127.0.0.1:5909` (any VNC client connects;
the RFB handshake answers `RFB 003.008`). That is useful on its own -- the iOS screen can be watched
live instead of photographed -- and it was tried as a possible reason the host's pointer did nothing:

| configuration | host pointer coordinates reach the guest? |
|---|---|
| `-display none` (as it was) | no -- no EV_ABS at all |
| with a VNC console added | no -- still no EV_ABS |
| device named (`id=tablet`) and events addressed to it | no -- still no EV_ABS |
| buttons from the same monitor, in every one of those | **yes** -- EV_KEY arrives every time |

So the console was not the reason and the device name was not the reason. What is measured is that
`mouse_button` reaches the guest and `mouse_move`, `input-send-event` with `abs`, and the same
addressed to a named device, do not. The guest's own input path is proven (addenda 25 and 26), so this
is entirely a question about QEMU's virtio-tablet absolute pointer.

The VNC option is kept because watching the machine live is worth having, and because it is the
natural way for a person to use this port from outside while the touch path is settled.

## Addendum 28: the host drives the interface -- QMP, not the human monitor

`docs/demo/settings-from-host.png` is Settings, opened by a tap sent **from the host**:

```sh
qemu/tap.py 683 1689        # the Settings icon, in panel pixels
```

Before the tap the panel is the home screen (318,804 bytes); after it, Settings (135,720 bytes). No
program of ours ran inside the guest to make that happen.

The missing piece was not QEMU's virtio-tablet at all -- it was **which interface the command is sent
through**. The same event, in the two places:

| sent through | what the guest's driver receives |
|---|---|
| the human monitor (HMP), `input-send-event` | EV_KEY for its buttons, **no EV_ABS at all** |
| **QMP**, `input-send-event` with a list of events | `type 3 code 0 value 19893` (ABS_X), `type 3 code 1 value 12132` (ABS_Y), the buttons, the EV_SYN |

So addendum 27's conclusion needs one correction: the coordinates *do* reach this guest, and the HMP's
JSON argument parsing is what swallowed them. `qemu/tap.py` is the tool -- a tap, or a swipe with
`--swipe X0 Y0 X1 Y1` -- and it sends everything over QMP (the socket `qemu/run.sh` already opens on
127.0.0.1:4445).

What that means for the state of the port, which is now worth stating as a whole:

| from the host | works |
|---|---|
| watch the screen | yes -- `VNC=1`, or a screendump |
| tap an icon, open an application | **yes** -- `qemu/tap.py` |
| navigate inside an application | yes -- the same tool with new coordinates |
| type on a keyboard | not tried; `virtio-keyboard-device` is there |

The interface is now something a person can operate, not only photograph.

### 28a. And it goes deeper: Screen Time, from the host

One more tap from the host, on the Settings list: `qemu/tap.py 562 1586` -- meant for General, landed
on Screen Time -- and the Screen Time page comes up (`docs/demo/screen-time-from-host.png`): the
hourglass, "Get insights about your screen time...", Weekly Reports, Content & Privacy Restrictions,
Downtime & App Limits, and the "Turn On Screen Time" and "Set Up Later" buttons.

That is three levels deep from a cold boot, all of it driven from outside the machine:

```
home screen  --tap(683,1689)-->  Settings  --tap(562,1586)-->  Screen Time
```

Nothing in the guest was told to do any of this; the taps are `qemu/tap.py`, and everything after them
-- launching the application, pushing a view, drawing a new page with buttons and icons -- is Darwin's
own work through CoreAnimation and Mesa.

### 28b. Gestures work too, with two details that matter

`qemu/tap.py --swipe 562 2425 562 500 20` returns to the home screen
(`docs/demo/home-after-gesture.png`, the home screen again with the clock moved on). Two things had to
be right, and both were found by getting them wrong first:

| detail | wrong | right |
|---|---|---|
| the order of the events | positions, then a press and a release at the end | press **first**, move while the finger is down, release last |
| where a swipe starts | `562, 2300` -- 135 px above the bottom edge: nothing happened | `562, 2425` -- the bottom edge itself: the home gesture |

So the whole loop is now driven from outside the machine:

```
home screen --tap(683,1689)--> Settings --tap(562,1586)--> Screen Time
            <--swipe(562,2425 -> 562,500)--
```

Taps open applications and pages; a swipe from the bottom edge dismisses them. Every one of those is
`qemu/tap.py`, and every pixel after them is Darwin's own drawing.

### 28c. Control Center, and the blur effect drawn by Mesa

`qemu/tap.py --swipe 1050 20 1050 900 18` -- a pull down from the top right corner -- opens the
**Control Center** (`docs/demo/control-center.png`): the connectivity tile with airplane, Wi-Fi,
Bluetooth and cellular; the rotation lock and screen mirroring; Focus; the brightness and volume
sliders; flashlight, timer, calculator and camera; and the home indicator.

Two things about that picture are worth more than the screenshot:

- **the backdrop is blurred.** Control Center dims and blurs what is behind it, and that blur is
  drawn by CoreAnimation's own shader through our OpenGL ES stand-in and Mesa's `softpipe`. So the
  path is not only filling rectangles: it renders the effect work too;
- **it is 1,034,320 bytes**, three times the home screen's, which is what a blurred composite costs
  the PNG encoder -- a small confirmation that this is a rendered effect and not a flat colour.

The gesture set is now: tap an icon, tap a row inside an application, swipe up from the bottom edge,
swipe down from the top right. All four from the host, all through `qemu/tap.py`.

## Addendum 29: one command, the whole sequence

```sh
qemu/demo.sh /tmp/demo
```

boots the machine (retrying, because not every boot reaches the screen), waits for a drawn screen, and
then operates it from the host, keeping a picture of every step:

| file | bytes | what it is |
|---|---|---|
| `00-home.png` | 317,332 | the home screen |
| `01-settings.png` | 135,744 | Settings, opened by tapping its icon |
| `02-inside.png` | 63,879 | a page inside Settings, opened by tapping a row |
| `03-home.png` | 322,811 | the home screen again, after a swipe up from the bottom edge |
| `04-control-center.png` | 1,027,411 | the Control Center, after a swipe down from the top right |

Those five are in `docs/demo/sequence/`. The run took about eight minutes, most of it the boot.

That is the whole of what this port can do today, reproducible with one command: the system's own
interface, drawn by Darwin through CoreAnimation and Mesa, operated from outside the machine.

### 29a. What the keyboard can and cannot do

`drivers/input/hakios.virtio_input.c` maps exactly these keys, and no others:

| key | becomes |
|---|---|
| `KEY_POWER` (116), `KEY_L` (38) | the lock button |
| `KEY_VOLUMEUP` (115), `KEY_UP` (103) | volume up |
| `KEY_VOLUMEDOWN` (114), `KEY_DOWN` (108) | volume down |
| `KEY_HOME` (102), `KEY_H` (35) | the home button |

Everything else is ignored -- there is no HID keyboard service behind this device the way there is one
for the touch screen, so **typing text into iOS is not supported** by this port today. That is a
limitation of the driver, not of the QMP path: the events arrive, and the driver has nothing to do
with the letters.

So the input story is: touches, gestures and the hardware buttons work from the host; text entry does
not, and would need the same kind of virtual HID service that addendum 25 shows working for touches.

## Addendum 30: the machine is on the network

```
netcfg: en0 10.0.2.15, default route through 10.0.2.2
net_test: en0: flags 0x8843 up running, type 0x6, address 52:54:00:12:34:56
net_test: en0: inet 10.0.2.15 netmask 255.255.255.0
net_test: tcp 10.0.2.2 port 18080: connected in 0.05 s
net_test: tcp 10.0.2.2: 447 bytes in 0.06 s, the answer begins "HTTP/1.1 401 Unauthorized"
```

That is Darwin in the guest holding an IPv4 address on `en0`, with a default route, opening a TCP
connection to the host and reading an HTTP answer back -- through QEMU's user-mode network, with no
root on the host and no tap device.

The pieces, and which of them were already there:

| piece | state |
|---|---|
| the card: `QEMU_NET=1` adds `virtio-net-device` and the boot argument `virtio_net=1` | in the port; the test machine simply was not started with it |
| the driver: `net: en0 attached on QEMU's virtio network device, address 52:54:00:12:34:56, link up` | in the port, works untouched |
| the address: configd's IPConfiguration plugin, asked for DHCP by `net_test dhcp en0` | **does not work here**: configd starts (launchd reports `Successfully spawned configd[17]`) but never registers `com.apple.network.IPConfiguration`, so the lookup returns `0x10000003` and `net_test: store` says `no session with configd` |

So the address is set by hand instead: `guest/netcfg.c` does what the plugin would have done --
`SIOCAIFADDR` with 10.0.2.15/255.255.255.0, `IFF_UP|IFF_RUNNING`, and an `RTM_ADD` default route
through 10.0.2.2 (those being QEMU's user-mode network: guest 10.0.2.15, host 10.0.2.2). It is built
like the other guest programs and lands in `/redwood/bin` through `EXTRA=`.

One thing this does not give: **names**. `net_test resolve example.com` needs mDNSResponder, which is
fed its servers by configd -- and configd's plugin is exactly what is missing. IP literals work;
`example.com` does not.

And the reason the checks run at all: they are started from `/etc/rc`, which `init` executes before it
hands process 1 to launchd, with `&` so each one waits in the background. A launchd job of ours is not
reliably executed here (the xpcproxy pattern, addendum 4), and this way nothing depends on it.

## Addendum 31: a VNC viewer can operate the machine, through a bridge of our own

QEMU's VNC console shows this guest but cannot drive it: with the console as the machine's only
display (`-display vnc=127.0.0.1:9`) a client's pointer still arrives at the driver as `type 2`
(EV_REL) and `type 1` buttons, never `type 3` (EV_ABS) -- measured again after making VNC the primary
display, so the earlier conclusion was not about being a second console. The touch driver has nothing
to do with relative motion.

So the bridge does the job itself. `tools/qemu/vncbridge.py` is an RFB server on 127.0.0.1:5910 that

- **serves the screen itself**: one `screendump` per request, at the panel's own 1125x2436 (the frame
  is BGRX on the wire; that is what a client asks for and what this hands it);
- **turns the client's pointer into touches**: a press becomes absolute x/y plus the button, a drag
  becomes absolute x/y while the button is held, a release becomes the button up -- all of it through
  QMP, the interface that is known to reach this guest (addendum 28), never through QEMU's own VNC
  input path.

Two pictures of it working, both driven by a VNC client connected to the bridge:

| file | what the client did | what the machine did |
|---|---|---|
| `docs/demo/vnc-long-press-menu.png` | pressed and held for a second | the home screen's context menu for the Settings icon: Remove App, Battery, Wi-Fi, Bluetooth, with the backdrop blurred |
| `docs/demo/vnc-click-settings.png` | dismissed the menu, then a quick click | Settings, restored to the last page it had open |

A press of 0.08 s is a tap; a press of a second is a long press. Both arrive, so the whole gesture
vocabulary of the interface is available from a VNC viewer -- on a machine that QEMU's VNC cannot
point at all.

```
tools/qemu/vncbridge.py --port 5910 --qmp 4445 --fps 1     # then any VNC viewer on 127.0.0.1:5910
```

## Addendum 32: watching and operating it from a browser

`docs/demo/webvnc-focus-page.png` is machine 2's Settings, on the Focus page, opened by a click in a
**browser**: `tools/qemu/webvnc.py` serves the page, pushes frames over a WebSocket, and turns the
clicks into touches over QMP.

Three things had to be true, and two of them were not at first:

| piece | what it took |
|---|---|
| a frame quickly enough to feel live | `screendump` per request through **QMP**, not the human monitor: the file lands in ~0.12 s and is read before the next one starts, so the page runs at several frames a second |
| more than one tool at the machine | QEMU's socket chardev serves **one** client: the first gets the greeting, later ones connect and wait forever. `tools/qemu/qmpmux.py` holds that one connection and serves many, so the web viewer, `tap.py` and a screendump can all be in use at once |
| the pointer reaching the guest | the same discovery as addendum 31: QMP absolute events, never QEMU's VNC input path |

```
tools/qemu/qmpmux.py --qmp 4455 --port 4465        # one QMP connection, many clients
tools/qemu/webvnc.py --port 5980 --qmp 4465 --fps 4
```
then `http://127.0.0.1:5980/` -- click to tap, drag to swipe, the panel keeps its aspect in whatever
window you give it. No viewer library is fetched: the page, the WebSocket framing and the frame
source are all in the one file, because this machine may have no way to reach the internet.

### 32a. Typing, from the browser, without a keyboard service

The port has no HID keyboard service -- the virtio driver maps only the power, volume and home keys
and drops the rest (addendum 29a) -- so a browser's letters cannot reach Darwin as key events. They
reach it as **touches instead**: the software keyboard is drawn like everything else here
(`docs/demo/software-keyboard.png`), and a character is the tap that would hit its key.

The key positions were measured from a screendump of the keyboard itself (row by row, by the runs of
key-coloured pixels), and `tools/qemu/webvnc.py` turns each character into one, two or three taps:

| typed | what is sent |
|---|---|
| `a` | one tap on `a` |
| `A` | shift, `a`, shift |
| space | one tap on `space` |
| backspace | one tap on the delete key |

`docs/demo/webvnc-typed-abc.png` is the FaceTime sign-in field showing `abc`, typed in a browser.
Digits and punctuation are not mapped yet: they live on the keyboard's second page, which would need
that page's own measurement.

### 32b. Making it tolerable to use: five measurements, five fixes

Watching the machine was fine until something moved -- then it dragged. Measured, not guessed:

| what was wrong | measured | what it is now |
|---|---|---|
| every pointer event opened its own QMP connection | ~20 ms each, so a drag of thirty events queued for most of a second -- **this is what "laggy" felt like** | one connection, kept open: **2 ms** an event |
| frames were PNG from QEMU | 0.251 s a frame | PPM, raw: **0.066 s** -- QEMU writing pixels is four times faster than compressing them |
| frames were the whole panel at full quality | 132 KB each | 75% of the panel, quality 70: **30 KB** |
| frames were written to disk | 8 MB a frame, several a second | `/dev/shm` |
| the viewer ran at one rate whatever happened | 19.5% of a core, all the time, competing with the guest | **1 fps idle** (and identical frames are not sent at all), **8 fps for 2.5 s after any touch**, ~4-7 fps of fresh frames during a drag; **7%** of a core |

What does not change: the guest's own ceiling. It presents about ten frames a second and reports
3.2-3.5 ms a frame in the driver, but a composited frame costs ~140 ms in Mesa's software rasteriser,
so somewhere around seven frames a second is all the animation this machine can do. The fixes above
make the *interface* responsive -- a drag follows the finger instead of queueing -- and stop the
viewer from taking the guest's time; they cannot make the guest draw faster than it draws.

## Addendum 33: the frame-rate ceiling is not Mesa — it is CoreAnimation's own software renderer

Measured while chasing 24 fps. Scrolling a Settings list is the workload:

| display scale (`fbscale`) | rasterised pixels | scroll |
|---|---|---|
| 3 (the default, 1125x2436) | 2 740 500 | **~1.3 frames a second** |
| 2 (750x1624) | 1 218 000 | (this is what the machine now runs) |
| 1 (375x812) | 304 500 | **~2-4 frames a second**, and the panel's upscaling costs the driver 17-20 ms a frame instead of 3.5 |

Nine times fewer pixels bought about twice the frame rate. That ratio is the finding: the cost is not
per-pixel work in our rasteriser, it is **the emulated CPU executing the drawing code**, and the log
says whose code that is:

```
hakios: no accelerated renderer; the software one will draw
```

CoreAnimation has a GPU renderer and a software one, and here it takes the software one -- because
there is no Metal device to give it. So the picture is rasterised by Apple's software renderer, running
as ARM instructions under QEMU's TCG, and only then scaled into the panel by `MiniIOMobileFramebuffer`
(3.5 ms at scale 3, because writing 10.5 MB a frame is cheap; the drawing is what costs).

That has two consequences worth writing down before anyone optimises the wrong thing:

- **llvmpipe would not have helped the interface.** Mesa is behind `libhakios.gl`, which applications
  that ask for GL use; it is not in CoreAnimation's compositing path. A faster software rasteriser
  there changes games and GL demos, not the home screen;
- **the only real lever is the GPU path this project exists to build** -- Metal's command stream
  translated to the host's Vulkan (`proto/`, `host/vkbridge/`). 24 fps needs that, not tuning.

What tuning can buy, measured or bounded: `fbscale=2` (2.25x fewer pixels, softer picture, and
SpringBoard's red service bar appears at the top), and `fb_filter=0` (bilinear to nearest, ~5 ms a
frame in the display thread). Both are one boot argument each.

## Addendum 34: putting an application on the home screen — what does not work

The goal was a third-party `.app` on the springboard, and from there an installer. Four things were
measured, and three of them closed a door:

| what was tried | what happened |
|---|---|
| copy a bundle into `/Applications` and rebuild the system image | **no icon.** The image verifiably contains `TestApp.app` (`apfs-tool ls`), the machine boots, the home screen is unchanged, and `lsd` never mentions it |
| `LSRegisterURL` on the bundle | **the symbol does not exist** on this system: `register_app: no LSRegisterURL in MobileCoreServices` |
| ask the runtime what is there instead | `LSApplicationWorkspace` has 155 instance methods including **`registerApplication:`**, `LSApplicationProxy` has **`applicationProxyForBundleURL:`**, and `defaultWorkspace` answers |
| call it | the workspace object is built, the proxy for our bundle comes back non-null -- and the next call **hangs**. These are synchronous XPC calls into `lsd`, made from a process with no entitlement for them |

One more fact explains the rest: **`allApplications = 0`**. LaunchServices starts with an empty
database, so the home screen's icons do not come from it at first; something else (MobileInstallation,
most likely) is what puts the stock applications there, and our bundle never entered that path.

So the install channel is not a configuration: it needs either a process that holds the entitlement
to talk to `lsd` (that is `installd`, which is in the image but not in the job list), or a writer for
the LaunchServices database itself. That is a piece of work, not a setting -- and it is the same piece
whether the application is a test bundle or WeChat.

What this also settles, for the record: **jailbreaking is not the missing piece here.** The kernel
never judges a signature (AMFI is a stand-in that accepts anything parseable, and ad-hoc signatures
are handed `CS_PLATFORM_BINARY`), so there is nothing for TrollStore's CoreTrust trick to bypass. What
is missing is registration and installation, in user space.

### 34a. Entitlements can be forged here, and it is still not enough

Two more attempts, both worth recording because they say where the wall really is.

**An `ldid` exists on this machine** (`/tmp/ldid/ldid`, built from source earlier in the project),
so a guest binary *can* carry an entitlement plist -- and because the port's AMFI reads that plist
straight out of the signature without verifying anything (addendum on code signing), the plist is
taken at face value. `register_app` was signed with `platform-application`,
`com.apple.private.mobileinstallationservices`, `com.apple.lsd.modifydb`,
`com.apple.private.launchservices.allowall` and a few more, and `ldid -e` shows them in the binary.

**It still hangs.** With the entitlements in place the tool gets further:

```
register_app: workspace 0x101805fd0, 0 applications known before
register_app: proxy for /Applications/TestApp.app -> 0x101907400
register_app: calling registerApplication: ...
```

and there it stops, for as long as one cares to wait. `lsd` is alive throughout -- launchd reports it
spawned, `INIT`, `Successfully spawned`, and never an exit -- so the call is not failing, it is simply
never answered. Whatever `registerApplication:` sends over XPC, this process cannot get a reply to,
entitlement or not.

**Where the icons actually come from** is the other half of the answer: `SpringBoard.app` ships
`DefaultIconState~iphone.plist`, which names the bundles the home screen lays out
(`com.apple.Preferences` among them). So the layout is static and the applications behind it are
resolved in SpringBoard's own privileged context -- not from the empty database a plain process sees.

Which leaves the honest position: an application can be put into the image, it cannot yet be made to
appear. Doing so needs the installation path Apple uses (installd, in the image but not in the job
list) or a writer for the LaunchServices database, and both are userland work rather than a setting.

### 34b. Registering from inside SpringBoard: the call blocks, and it takes the UI with it

Addendum 34a left one door open -- entitlements can be forged here, so perhaps the registration
call only needed to come from a process the system trusts. SpringBoard is that process, and this
port already injects a library into it (`libhakios.sb.dylib`), so the call was moved there.

What it answered is worth having:

```
hakios: register: SpringBoard sees 173 applications
hakios: register: proxy 0x28257ea00 for /Applications/TestApp.app
```

**173 applications**, where a plain tool saw 0: LaunchServices is not empty, it simply does not
answer an unprivileged caller. So the database is fine, the stock icons are real, and our bundle is
one proxy away from being in it.

And then `registerApplication:` **never returns** -- not from a plain tool, not from an entitled one,
not from inside SpringBoard, and the last of those is the part that matters: with the call on
SpringBoard's main queue the whole interface froze behind it (the panel went black and stayed black);
moved to a background queue the call still never returns, and the interface came up but the call
never completed either.

So this is not a permissions problem and not a queueing problem. `registerApplication:` waits for
something this port does not provide -- most likely the MobileInstallation/installd side of the same
transaction, which is not running here. The hook was removed from the boot path; the function stays,
unused, because the next attempt should start from those two lines rather than from nothing.

## Addendum 35: the accelerated renderer can be turned on, and on this machine it is slower

The interface draws at 1.3 frames a second while scrolling because CoreAnimation has no accelerated
renderer (addendum 33). There is a switch for that, and it works: build the userland with `GL=1`
(`libhakios.gl.dylib` and `libOSMesa.8.dylib` go into the image), boot with `metal=1`, and the window
server takes the other path:

```
hakios: the server has an accelerated renderer (0x103204330)
```

CoreAnimation no longer says `Failed to initialize Metal or OpenGLES`, and no longer falls back to its
software renderer. So the architectural switch the project wants is reachable from where we are --
and that is where the good news ends.

**Measured, the same boot argument, the same image, only the two libraries different:**

| | first frame |
|---|---|
| software renderer (no GL libraries) | ~6 min to the home screen |
| "accelerated" renderer (GL + OSMesa in the image) | **none in 7 minutes**, twice: the boot aborts with `no frames after 420 s` |

Because the accelerated renderer here draws through Mesa's `softpipe`, which runs inside the guest as
AArch64 instructions under QEMU's TCG -- and that pipeline is *slower* than Apple's own software
rasteriser doing the same rectangles. Turning the GPU path on does not make this machine faster; it
makes it too slow to reach the home screen.

Two numbers to keep the goal honest: 24 frames a second is 41.7 ms a frame; scrolling now costs about
700 ms a frame. That is a factor of about seventeen, and no boot argument, resolution or QEMU tunable
found in this project spans it. It needs the drawing to happen **outside** the emulated CPU -- the
Metal-to-Vulkan bridge (`proto/` carries the commands, `host/vkbridge/` replays them on the host,
where the code is native x86). The host half builds and passes its own probe; the **guest half, which
would have to encode Metal into those commands, does not exist yet.**

Metal itself is farther along than the renderer switch suggests, which is worth recording because it
is the foundation the bridge stands on. From the port's own diagnostic (`metal=1`, `metal_test`):

```
MTLCreateSystemDefaultDevice() = "HakiOS Adreno 642L (Metal: a device that exists, nothing more)"
  a buffer of 65536 bytes: written and read back, 0 words wrong
  the GL renderer's library: 274 functions, 3 vertex, 271 fragment
  queue made, buffers at GPU addresses 0x0 and 0x0, command buffer nil
  no blits: the device has no GPU queue (gpu=1, and the plug-in built with hakios-metal)
```

So a device exists, buffers and libraries verify, and the missing piece is named in the diagnostic
itself: no GPU queue, because that needs `hakios-metal` -- and behind it a GPU that can execute.

### 35a. The emulator-side knobs, measured and rejected

Two of them were worth trying, and neither helps:

- **`-cpu max`** (with `tb-size=4096`): the machine produced no frame in seven minutes, where
  `cortex-a76` reaches the home screen in five or six. `max` turns on SVE and the rest of the
  optional architecture, and TCG emulates those slowly -- slower than whatever it gains.
- **`tb-size=4096`** instead of 1536: tried with the above, so it cannot be separated from it, and
  the effect of a larger translation cache this far above the working set is small by construction.
  Not worth another boot cycle to isolate.

So the emulator is not where the seventeen times are hiding either. Recorded here so the next
attempt does not spend another evening on it.

### 34c. A correction: the kernel does not check signatures, user space does

An earlier addendum here said signing was not the blocker because the port's AMFI stand-in accepts
anything. That was half the story, and the missing half matters: the stand-in still **reads the
entitlements out of the signature** — `xnu/files/libkern/amfi/miniamfi_entitlements.cpp` says a
process holds the entitlements its program's signature lists, and one without a signature holds none.
User space then enforces them on its own account. `userland/build.sh` ldid-signs `record_test`,
`battery_test` and `keychain_test` for exactly this reason.

That reframes the LaunchServices result. `lsd` answering a plain tool with zero applications while
SpringBoard sees 173 is better explained by entitlements than by an empty database: SpringBoard holds
`com.apple.private.coreservices.canmaplsdatabase` and friends, and a tool signed with nothing holds
none. The useful entitlements, read out of the dyld shared cache:
`com.apple.private.coreservices.can-register-install-results`,
`can-perform-rebuild-registration`, `canmaplsdatabase`, `can-send-install-notifications`,
`canmapbundleidtouuid`, plus `com.apple.private.mobileinstall.allowedSPI`.

`installd` carries `can-register-install-results` and `can-perform-rebuild-registration` — it is the
process the system designates for registering applications, which is why getting it into the job list
(`INSTALLD=1`) was the right move.

So the honest state of the install problem: not "the kernel does not care", but "the kernel does not
verify, and user space still asks who you are".

## Addendum 36: the guest reaches the internet — two bugs, both in our own plumbing

Verified end to end in one boot (`/tmp/netcheck-run.log`, evidence lines in
`/tmp/netcheck-evidence.txt`):

```
(system/com.apple.configd [21]) Successfully spawned configd[21] because speculative
net_test: dhcp: IPConfiguration says of "setservice en0 DHCP": 0 (done), service 258795C8-…
State:/Network/Global/DNS = { ServerAddresses = ( "10.0.2.3" ); __CONFIGURATION_ID__ = "Default: 0"; }
net_test: resolve example.com, 0.05 s: 172.66.147.243 104.20.23.154
net_test: reach example.com: flags 0x2: reachable
net_test: tcp 10.0.2.2: 75329 bytes in 0.07 s, the answer begins "HTTP/1.0 200 OK"
```

**The first bug was ours.** `mkjobs.py` passed each plist's `LimitLoadToSessionType` through as it
found it. Every job that landed in `(system/…)` (notifyd, logd, fseventsd) names it as the plain
string `System`; configd's is a *conditional dictionary*
(`{#Then: System, #IfFeatureFlagEnabled: UserManagement/SystemSessionD1}`). So configd was loaded
into the per-user 501 domain, where the Mach services it publishes — `com.apple.network.IPConfiguration`
and `com.apple.SystemConfiguration.DNSConfiguration` — are registered in the user bootstrap and
therefore invisible to everything that could use them. Normalizing the conditional form to its `#Then`
value puts configd back in the system domain, and the whole chain works.

**The second was in the rc file.** etc/rc is run by init, and nothing init forks has a bootstrap port
at all — so `bootstrap_look_up` in such a process fails with `0x10000003`, which is
`MACH_SEND_INVALID_DEST`, not "the service is missing". That code was previously read as evidence that
configd never registered anything; it said only that the caller could not talk to bootstrap. The DHCP
and resolve steps now run as launchd jobs (system domain for the DHCP ask, user domain for the
resolve), which is the only context where those calls mean anything.

Two corrections to earlier notes, recorded because they were both confidently wrong:

- `installd` does **not** start under `INSTALLD=1`: it is an on-demand Mach job and nothing demands
  `com.apple.mobile.installd`, so no spawn happens.
- there is no `/etc/resolv.conf` in this image, and `getaddrinfo` would not read one if there were
  (that path belongs to `libresolv`'s `res_*` API). The resolver comes from configd's store or not at
  all.

`netcfg`'s source was also restored to `userland/src/`: the tree had none, only a copy under /tmp that
a fresh image does not contain, while the rc still called it — a fresh build silently lost its address.

## Addendum 36a: two agents, one tree — the ROOT rule

Two people (or agents) cannot share a VM: `qemu/ram.img` and `qemu/payload.bin` are written by every
boot. `ROOT` makes it survivable, since both `run-darwin.sh` and `wait-for-ui.sh` honour it:

```
cd userland && CONFIGD=1 RC=rc.netcheck ./build.sh root.apfs && cp root.apfs /tmp/root.net.apfs
cd .. && QEMU_NET=1 ROOT=/tmp/root.net.apfs bash qemu/wait-for-ui.sh /tmp/net.png
```

The tree's `userland/root.apfs` is then never read by the boot and stays the other worker's. Two
gotchas: `payload.bin` is still shared (write it only when no qemu is running), and `TMPDIR` is not
honoured for the per-boot log, so the log is whichever `/tmp/tmp.*` is newest.

## Addendum 37: the Settings frame, drawn by the host, pixel for pixel

The handover said the guest was at launchd and `lsd`; it was not. By the time this session looked,
the machine had already come up, drawn the home screen, and then died of a **kernel panic**, not of
anything done to it:

| boot | what it did | how it ended |
|---|---|---|
| `/tmp/iosvm-0100b.log` | backboardd 01:17:42, home screen in the framebuffer at 01:26 | `Ticket spinlock[0xfffffff007b68880]` timeout at 883 s, owner on CPU 3, panicked task `fseventsd` |
| `/tmp/iosvm-0142.log` | backboardd 02:01:45, home screen, Settings | the same lock and the same 60 s timeout, panicked task `backboardd` |

No host OOM, no `kill`; the owner of the lock is on CPU 3 and never releases it. With `cpus=4`
under TCG this is reproducible within a quarter of an hour, so the working window is about ten
minutes from `backboardd`, and a machine paused inside that window stays usable. The current guest
(`/tmp/iosvm-0202.log`) was paused at the Settings page and left that way.

### The two pictures

The loader allocates the panel at `0x33b9eb000`, 1125 x 2436, 4500 bytes a row; the kernel draws
the interface there, and the CoreAnimation IOSurfaces behind it are 1125 x 2436 **BGRA at 4608 bytes
a row** (`MiniIOSurface: backboardd[15] created surface 1 ... 4608 bytes per row`) — 1152 pixels, not
1125. Reading `0x33b9eb000` as 1125 x 2436 BGRA gives the UI exactly.

QEMU's display surface is **not** that memory, and after the first update it is not a live view of
it either. With the machine paused on the settled Settings page, a screendump returned a frame from
the middle of the slide-in, and magenta probes written into `0x33b9eb000` (at both 4500 and 4608
strides) never appeared in it. Further resumes and screendumps returned the *same bytes*, so the
console surface is a copy taken at the first update after the loader's mode set, and later updates
do not refresh it. The guest went on drawing into its framebuffer; the display device kept the
snapshot. That
is worth separating from the guest's rendering, and it is very likely part of what the earlier notes
called "the screen does not appear on every boot": the machine often *has* drawn, and the display
device is showing an older push.

**Correction (Addendum 39):** that conclusion was a PPM reading artefact, not a property of the
display device. QEMU writes pixman's padded line stride per row, and a reader that assumed packed RGB
saw a sheared frame. Read correctly, the surface is live: a probe written into `0x33b9eb000` appears
in the next screendump, and the whole frame is byte-for-byte the guest's. Keep the observation, but
do not build on it.

### glwire: what was actually wrong, and what now runs

The 22:44 note was right that pointing the 62 table slots at the recorders took the local state with
them — `-attachImage:` asked for `GL_TEXTURE_BINDING_2D` and got 0. Four things were wrong, and one
of them had not been seen:

1. **The recorder has to keep the local call.** `remote_gl*` now calls the entry the slot held
   before it was overridden (`local_entry[i]`, saved by `eagl_fill_table_remote()`), then records.
   That is not a fallback: it keeps `eagl.c`'s own wrappers in the path, and it is what lets
   `-attachImage:` see the texture it was just given.
2. **4 MiB was too small for one call.** A panel-sized `glTexImage2D` is 10,962,000 bytes; the
   recorder arena was 4 MiB and `wire_put` drops what does not fit, silently, leaving a record whose
   header promises more than it holds. It is 32 MiB now.
3. **`glVertexAttribPointer`'s array was freed with the call.** The GL keeps that pointer until the
   draw; the dispatcher freed the copy immediately, so the host draw read freed memory. The
   dispatcher keeps up to sixteen and frees them after the draw or flush.
4. **`frame_end()` copied the local picture over the host's.** The remote write now happens after the
   loop that copies the local texture into the surface, not before it.

The host half is now a program that can be run without a guest: `frame_record.c` builds a frame with
the generated guest recorders (`encode.inc`, unchanged), `frame_replay.c` reads the panel's frame out
of the RAM file, replays the records through the same `dispatch.inc` `host_glwire` uses, draws it as
a textured quad into a panel-sized framebuffer, reads it back and writes it into the same place.

```
$ cd tools/glwire && gcc -O2 -o frame_replay frame_replay.c frame_record.c -lEGL -lGL
$ ./frame_replay ~/DSH-desktop/iosvm-work/hakios-xnu-kernel/qemu/ram.img 0x33b9eb000
frame_replay: llvmpipe (LLVM 20.1.2, 256 bits) | 4.5 (Compatibility Profile) Mesa 25.2.8
frame_replay: 10962430 B of records for 10962000 B of layer; recorded in 6.8 ms, drawn in 58.0 ms;
              0 pixels differ (0.000%), mean 0.000, worst 0
```

The 6.8 ms is the recorder (memcpy mostly), the 58 ms is the draw and the readback. The sandbox this
session ran in refuses `/dev/dri/*` and `/dev/nvidia*`, so EGL fell back to `llvmpipe`; the same
binary asks for the EGL device first and picks the V100 where the nodes are open, which is the 1.9 ms
`glquad` measured. **The V100 was not reachable from this session** -- that is a sandbox limit, not a
result about the GPU.

The frame that came out is the Settings page, and it is byte-identical to the one the guest drew:

```
$ md5sum docs/demo/glwire-settings-guest.png docs/demo/glwire-settings-host.png
a4c814e766cb0eff2d3a1c2aeee95272  docs/demo/glwire-settings-guest.png
a4c814e766cb0eff2d3a1c2aeee95272  docs/demo/glwire-settings-host.png
```

`docs/demo/glwire-settings-guest.png` is the guest's own settled frame read from `0x33b9eb000`;
`docs/demo/glwire-settings-host.png` is what `frame_replay` read back after the GPU pass.
`docs/demo/glwire-settings-panel.png` is QEMU's screendump of the same moment -- the slide-in frame
described above, not the settled one. `docs/demo/glwire-home.png` is the home screen from the same
boot.

The whole sequence is one script:

```
$ tools/glwire/panel_run.py /path/to/qemu/ram.img --log /tmp/iosvm-0202.log --out /tmp/panel-run
```

It waits for the home screen (colour below the kernel's boot band, so the band does not pass for it),
taps Settings at (663, 1690), waits for the page to settle, pauses the machine, runs `frame_replay`,
and takes the screendump. The tap goes over QMP, as before; the pause is what keeps the picture from
being overwritten by the guest's next frame.

### What is still between this and the interface's own calls

The host now draws a real panel frame from the wire format, and the guest's local renderer keeps
working with the recorder installed. The missing link is still the traffic itself: calls that return
a value (`glCreateProgram`, `glGenTextures`, `glGetUniformLocation`, the fence pair) stay local on
both sides, so the host's object names are its own. For CoreAnimation's stream the names have to
travel with the calls, and the dispatcher needs the guest name -> host name mapping (textures,
framebuffers, programs, shaders, uniform locations). That is the next thing to build, and it is the
reason a `GL=1` boot of the current tree would still draw with its own renderer, not the host's.

Also open, and independent: QEMU's ramfb surface should follow the guest's framebuffer (or the
screendump will keep being a frame behind), and the IOSurface stride (4608) and the panel's (4500)
are different numbers that the show path has to reconcile.

**Addendum 39 settles the first half:** the surface does follow the guest's framebuffer; the PPM
reader was wrong. The segment and IOSurface question is the guest's, not the display device's.

## Addendum 38: the names travel, and the wire's signatures were wrong

The last note named the next step correctly -- the guest's object names have to travel with the
calls -- but the wire had a second, older fault that would have stopped CoreAnimation's own stream
before names ever mattered.

**The trampolines did not fit the table.** `gen.py` had written every pointer argument as two C
parameters, `(const void *p, GLsizei n)`, because `frame_record.c` could pass a size. An EAGL slot
is called as `entry(table[0], glArguments...)` with exactly the GL signature, so the extra parameter
was whatever happened to be in the next register. `glVertexAttribPointer` (6 GL arguments, the
trampoline read a 7th), `glDrawElements` (4, read a 5th) and `glShaderSource` (4, invented two more
than the GL has) were all reading garbage. Nothing had noticed because only `frame_record.c`, which
called the recorders directly with the extra argument, had ever run.

`spec.def` now writes the real GL signatures, and each pointer argument says how its byte count is
worked out: `strlen` for a string, `count * sizeof(type)` for an index array, width and height and
the pixel format for a texture, `count * 4` for a name array, the guest's `GLsync` for a fence. A
vertex array has no size at pointer time and the GL does not read it until the draw, so the pointer
tuple is remembered and the bytes go with `glDrawArrays`/`glDrawElements` in an opcode-200 record;
the host applies them with `glVertexAttribPointer` and frees the copy after the draw.

**The answers travel.** `glCreateProgram`, `glCreateShader`, `glGetUniformLocation` and
`glFenceSync` append the guest's result to their record; `glGenTextures` and `glGenFramebuffers`
already carry the filled name array in their `t`/`r` argument (the recorder runs after the local call,
which is what fills it). `names.h` keeps the maps: textures, framebuffers, programs, shaders, syncs,
and uniform locations keyed by `(guest program, guest location)`. Every call that consumes a name
translates it, and the delete calls remove the mapping.

**Per process, not per machine.** Each guest process has its own record arena and picture, and the
host used to find them by one fixed magic word -- the first process's buffer, every time. The words
now carry the process id, the host caches the address after one scan, and each connection gets its
own GL context and maps.

The acceptance without a guest uses names that are deliberately unlike the host's:

```
$ ./map_test
map_test: 1267 bytes of records
map_test: current program 3 (linked 1), framebuffer 2 (status 0x8cd5), 2 shaders: 4(compile 1) 5(compile 1)
map_test: tint at 1 is 1 0.5 0 1
map_test: layer unit at 0 is 0; position attribute 0; error 0
map_test: centre pixel 51 51 0 255; maps: 2 textures, 1 framebuffers, 1 programs, 2 shaders, 1 syncs, 2 uniform locations
map_test: PASS
```

The stream names its program 77, its shaders 88 and 89, its textures 101 and 102, its framebuffer 303
and its uniform locations 5 and 6; the host context first makes dummy objects, so its names are
3, 4/5, 8/9, 2 and 0/1. The centre pixel is the layer (0.2, 0.4, 0.8) times the tint uniform
(1, 0.5, 0, 1); if any name or either location went across untranslated the pixel is black.

The socket path is its own test, because map_test does not exercise the request line, the search for
the picture in the RAM file, the context per connection or the reply:

```
$ ./e2e_test.py
e2e_test: reply 'R done 0.09'
e2e_test: centre pixel B=0 G=51 R=51 A=255
e2e_test: PASS
```

and the panel path is unchanged by the new signatures:

```
$ ./frame_replay qemu/ram.img 0x33b9eb000
frame_replay: 10962480 B of records for 10962000 B of layer; recorded in 6.7 ms, drawn in 59.0 ms;
              0 pixels differ (0.000%), mean 0.000, worst 0
```

### What was not run, and why

The live half -- the host receiving the current guest's own CoreAnimation calls -- was **not** run
this round. The paused guest cannot provide it, and the reason is worth writing down instead of
calling the synthetic tests an end-to-end result:

- the guest was booted **without a network device** (`netcfg: SIOCAIFADDR: Device not configured`,
  only `lo0` up), so `eagl_remote_flush()` has nowhere to connect;
- it was booted **before** `/redwood/bin/glremote_on` existed in `root.apfs`, so
  `eagl_remote_install()` returned early in every process, and the EAGL slots still hold the local
  functions;
- both are boot-time facts. Enabling the recorder in the live processes would mean patching their
  tables and `local_entry[]` in guest memory, and the recorder would still have no transport.

A second boot would need a 12 GB RAM file with the 2.4 GB system image in it. `/data3/iosvm-scratch`
is the right place for it, but the file sandbox refuses writes there, and the fallback is `/tmp` with
a 2 GB ceiling -- smaller than the image itself. Two other ways to hold guest RAM without a disk file
were tried and are also closed: `/dev/shm` refuses the 12 GB file (`truncate: Permission denied`), and
QEMU's anonymous `memory-backend-memfd` cannot be shared because `/proc/<qemu-pid>/fd` is unreadable
from this sandbox. The tree is ready for that boot, though:

`root.apfs` was rebuilt with `GL=1 EXTRA=/tmp/glremote_on`, which puts the switch at
`/redwood/bin/glremote_on`, and `libhakios.gl.dylib` now contains the corrected recorders (582,024
bytes). The APFS writer's `-z` path starts a `ProcessPoolExecutor` and this sandbox denies its
semaphore (`PermissionError: [Errno 13] ... SemLock`), so the image was made uncompressed:
20,570,112 bytes instead of 4 MiB. That still fits the payload, but it is a build-environment
workaround, not a decision.

The command for the next boot, when there is room, is the ordinary one with a network:

```
QEMU_NET=1 QEMU_SMP=4 QEMU_TB_MB=1024 QEMU_RAM_GB=12 \
  IMG=$PWD/userland/system.apfs ROOT=$PWD/userland/root.apfs \
  timeout 1700 bash qemu/run-darwin.sh 1620 >/tmp/iosvm-live.log 2>&1 &
tools/glwire/host_glwire qemu/ram.img 9110
```

with `/redwood/etc/glremote` optional (the default is `10.0.2.2:9110`, which is QEMU's user network).
`QEMU_NET=1` adds `virtio_net=1` to the kernel's arguments. The window is still about ten minutes
from `backboardd` before the Ticket spinlock panic.

### What that boot will not fix

`eagl.c` makes textures of its own for IOSurfaces (`-attachImage:`), outside the 62-call table; a
frame that samples one of them cannot be rebuilt from the GL stream alone. And the recorder's arena
is process-global, so two EAGL contexts used alternately in one process would interleave. Both are
the next structural questions, independent of the names.

## Addendum 39: the console surface is live -- the "slide-in" was a PPM stride

The previous commit concluded that QEMU's console surface was a copy taken at the first update after
the loader's mode set, because a magenta probe written into `0x33b9eb000` never appeared and every
later screendump returned the same bytes. Both observations were artefacts of how the PPM was read.

QEMU's `ppm_save()` writes `pixman_image_get_stride(linebuf)` bytes per row. For a 1125-pixel wide
RGB image pixman's stride is 3376, not `1125 * 3 = 3375`; P6 has no padding. A reader that follows
the format therefore starts every row one byte early, and 2436 rows later the picture is sheared by
2436 pixels -- a diagonal wipe that is easy to mistake for a frame from the Settings slide-in. The
bytes never changed because the settled page behind the shear never changed.

Read with the stride QEMU actually wrote, the surface is a **live view**:

- the screendump and the guest's framebuffer at `0x33b9eb000` are **0 differing pixels** over the
  whole 1125 x 2436 frame, not a sample;
- writing a 120 x 120 magenta rectangle at (40, 40) into the guest's framebuffer makes **14,400 of
  14,400** of those pixels magenta in the next screendump, with **0** other differing pixels;
- restoring the rectangle restores the frame byte for byte (**0** differing pixels).

```
$ tools/glwire/panel_probe.py qemu/ram.img
panel_probe: 1. the settled screen
panel_probe:    screendump vs guest framebuffer: 0 differing pixels
panel_probe: 2. a magenta rectangle at 40,40 120x120
panel_probe:    magenta pixels in the screendump: 14400 of 14400; other differing pixels: 0
panel_probe: 3. restoring the framebuffer
panel_probe:    after restore: 0 differing pixels
panel_probe: PASS: the console surface is live
```

`docs/demo/glwire-settings-panel.png` was the sheared read; it is now the same moment read
correctly, and `docs/demo/panel-probe.png` is the probe. The frame is the settled Settings page, not
an animation.

The readers were fixed (`tools/screendump.sh`, `panel_run.py`; `tools/qemu/screendump.sh` and
`webvnc.py` already handled the stride), and `tools/qemu/qemu-ppm-stride.patch` is the one-line QEMU
change that writes `width * 3` instead of the padded stride. It applies cleanly to QEMU 8.2.2
(`patch -p1 --dry-run`), but the running machine's binary was not rebuilt: the surface itself did not
need fixing.

Two consequences. The ramfb surface does follow the guest framebuffer, so the "display device is
showing an older push" explanation for "the screen does not appear on every boot" is **not** this;
whatever that turns out to be, it is elsewhere. And the IOSurface stride (4608) and the panel's
(4500) are about the guest's compositing, not about QEMU's display surface.

## Addendum 40: the 07:00 screendump's diagonal is one byte a row, measured

The 07:00 picture (`/tmp/guest-0700.ppm`, 1125 x 2436, 11668 colours) looked sheared, with a
torn block at the top left. That is the reader, not the guest. The numbers:

| what | value |
|---|---|
| guest panel (`MiniIOMobileFramebuffer`) | 1125 x 2436, **4500 bytes a row** |
| ramfb config (the loader's `etc/ramfb`) | address `0x33b9eb000`, stride 4500 |
| QEMU PPM pixel area | 8,223,936 bytes = 2436 x **3376** |
| packed RGB a row would be | 1125 x 3 = **3375** |

The PPM is one byte wider than the pixels on every row. Reading it as packed RGB starts each row one
byte early; 2436 rows later the picture has slid by 2436 bytes, which is the diagonal and the torn
block at the top left. Qt/ImageMagick/PIL all read P6 as packed.

With the stride QEMU actually wrote, the screendump is the guest's framebuffer exactly. The machine
was stopped for the two reads, so the frame could not change between them:

```
differing pixels: 0 of 2,740,500
ppm RGB md5:      dbf17f6711d31cac5799dd8b85715e7f
guest RGB md5:    dbf17f6711d31cac5799dd8b85715e7f
```

`docs/demo/guest-0700-screendump.png` and `docs/demo/guest-0700-framebuffer.png` are the two, and
their PNG md5 is the same (`1d0a492d1a23b1cd6933f3c0d1ec3771`). The guest was resumed (`cont`) after
the comparison; it is not restarted.

The guest's real pitch is 4500 (the panel's, and the loader's ramfb stride); the CoreAnimation
IOSurface is 4608, but that is the layer's memory, not what ramfb scans. `tools/screendump.sh` and
`tools/glwire/panel_probe.py` already drop the PPM padding; `tools/qemu/screendump.sh` and
`webvnc.py` already did. The raw PPM still needs a stride-aware reader.

## Addendum 41: the black frame was a missing IOSurface, and the texture path now draws

The 04:26 frames the host saved were 10,962,000 bytes of RGB **zero**: 0 nonzero pixels, one colour.
That is not a black scene; it is the absence of every texture. The records for those frames were 25
KB, not the 10.9 MB a panel-sized `glTexImage2D` would need.

The reason is a boundary the table does not cross. `glBindTexture` and the draw calls come through
the 62 EAGL slots and are recorded; the pixels do not. `eagl.c`'s `surface_to_texture()` calls
`glTexImage2D` **itself**, directly into Mesa, so the recorder never sees the upload and the host
keeps the name but not the content. The same is true for a target texture: it is drawn into, and
`surface_to_texture()` is never called for it, so its content exists only on the guest.

What was added, and where:

- `eagl_remote_upload()` in `eagl_remote.c`, called from `surface_to_texture()` right after the
  local `glTexImage2D`, with the guest texture name, internal format, size, format/type, row stride
  and bytes-per-pixel. It is wire opcode 201.
- The host's `names_texture_upload()` maps or creates the host texture, packs the rows (the IOSurface
  is 4608 bytes a row for a 1125-pixel picture), converts BGRA to RGBA for llvmpipe, and uploads.
- A target texture is drawn into, so `frame_end()` now also uploads the **composed panel surface**
  (`0xFFFF0001`, 1125 x 2436, the IOSurface's own stride) before the host hand-over. That is the
  texture the host's panel draw uses.
- The copies back into the CA surface are row-wise now: the surface is 4608 bytes a row, the host
  picture 4500; the old flat `memcpy` sheared every row and corrupted the window server's surface.

llvmpipe 25.2.8 faulted a few calls after replaying the CA draw stream once a large texture had been
uploaded and attached (`SIGSEGV` in `libgallium`, after `glFramebufferTexture2D`; the failing call
was `glClearColor`). The host therefore processes opcode 201 **first, with no framebuffer bound**, and
when a panel-sized upload is present draws that composed surface as a textured quad. This is the
"panel" path, not a replay of CA's FBO graph; it produces the guest's composed picture.

Evidence from the live guest (gl=1, glcopy=1):

```
host_glwire: upload texture 4294901761 target 0xde1 internal 0x1908 1125x2436
             format 0x80e1 type 0x1401 row 4608 pixel 4 bytes 11225088
host_glwire: saved frame-6-1125x2436.bgra
frame 6: 88,371 nonzero RGB pixels, 251 colours  -> docs/demo/host-live-textured-frame.png
frame 11: 3,876 nonzero RGB pixels, 142 colours  -> docs/demo/host-live-spinner.png
```

The Apple logo and the spinner are drawn by the host from a real texture upload; the earlier black
frame is `docs/demo/host-live-frame-black.png`. `host-live-textured-frame.png` is not the Settings
page: the gl=1/glcopy=1 boot stayed at the boot spinner past eleven minutes (the original guest's
Settings page is drawn by the CPU renderer, no GL). So the texture path is fixed and demonstrated;
the Settings-page acceptance still needs a boot that reaches SpringBoard's home screen.

## Addendum 42: the Ticket spinlock is pset0's scheduler lock

Both panics name the same word: `0xfffffff007b68880`. The kernel symbols put `_pset0` at
`0xfffffff007b68780`, and `struct processor_set` has `sched_lock` at +0x100 (a
`lck_ticket_t`, `SCHED_PSET_TLOCK`). The lock is the **processor set's scheduler ticket lock**.

The waiting thread's backtrace (symbolized from `mach.development.bcm2837`):

```
thread_select -> lck_ticket_contended -> hw_lck_ticket_contended
  -> hw_lck_ticket_timeout_panic -> panic
```

It is in `thread_block_reason` / `ipc_mqueue_receive`, i.e. an ordinary Mach message receive that
needs the scheduler lock to block. The owner is a thread on **CPU 3** (tickets: cticket 0x41,
waiting 0x44, nticket 0x45 in one panic; 0xee/0xef/0xf2 in the other), and the owner did not release
for 60,000,024 ticks. The panicked task is whoever waited longest (`fseventsd`, `backboardd`), not the
holder.

One line: **a thread on CPU 3 holds `pset0.sched_lock` and never releases it; the visible panic is
only the 60-second timeout of another thread waiting for the scheduler.**

The holder's PC cannot be recovered from the in-guest stackshot: the panic log says "Attempting to
forcibly halt cpu 3; cpu 3 failed to halt with error -5: halt not supported for this configuration"
and then "debugger synchronization failed, no stackshot". `tools/qemu/panic_probe.py` is the way
around it: it waits for the panic line in the serial log, then uses QMP `stop` + `info registers -a`
from outside the guest and dumps each vCPU's PC/SP and kernel stack through `xp`. That needs a boot
that actually panics; the 07:24 boot stayed idle at the spinner and had not panicked at 12 minutes.
Until such a dump, the workaround is a smaller machine (`cpus=1` or `cpus=2`), which removes the
four-vCPU scheduler contention this lock timeout grows out of.

### The 07:08 panic: a second lock, and its owner is already 0

The 07:00 resume produced another Ticket panic at 07:08, on a **different** word:

```
Ticket spinlock[0xfffffff007b0d380] timeout after 154091095 ticks;
cticket 0x40, nticket 0x41, waiting for 0x40,
current owner: 0 (on CPU -1), orig owner: 0xfffffff62b250608
Panicked task: pid 0 kernel_task
```

`0xfffffff007b0d380` is `thread_call_groups[THREAD_CALL_INDEX_HIGH].tcg_lock`: the array starts at
`0xfffffff007b0d100`, the group is 0x280 bytes, and `tcg_lock` is the first field. The owner field is
**0** while the ticket is still 0x40 — there is no thread holding it. That is a lost unlock (the
owner cleared but the ticket never advanced), not a thread that forgot to unlock.

So the two panic families are:

| lock | symbol | owner | panicked task |
|---|---|---|---|
| `0xfffffff007b68880` | `pset0.sched_lock` (+0x100) | a thread on CPU 3 | `fseventsd` / `backboardd` |
| `0xfffffff007b0d380` | `thread_call_groups[HIGH].tcg_lock` | **0 (CPU -1)** | `kernel_task` |

One line for the second: **the high-priority thread-call ticket lock is stuck with no owner; its
ticket never advanced, so the 60-second timeout is a lost unlock, not a held lock.** Both locks are
ticket locks whose wait/unlock path uses `wfe`/`sev` and atomics; under TCG that is the common
factor to test next, before blaming either subsystem.

### Why owner=0: the unlock is two stores, and the panic is the window

`lck_ticket_unlock()` in `osfmk/kern/lock_ticket.c` is:

```c
os_atomic_store(&tlock->lck_ticket_owner, 0, relaxed);   /* line 959 */
hw_lck_ticket_unlock_internal(&tlock->tu);               /* line 960 */
```

The owner field is cleared **before** `cticket` is advanced. The 07:08 panic reads the lock between
those two stores: `owner = 0`, `cticket = 0x40`, `nticket = 0x41`, waiter wants 0x40. That is not a
lost unlock in the ticket algorithm; it is the two-instruction unlock window. It becomes permanent
only because the vCPU that was executing line 959 never runs line 960 — the same class of "CPU 3 does
not come back" the stackshot complains about. So the one-line answer for this panic is: **the holder
was interrupted between clearing the owner and advancing the ticket; the 60 s timeout is the
incomplete unlock, not a lock held by a live thread.**

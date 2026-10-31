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
| OpenGL ES 3.1 | Mesa 25.0.7 built for arm64-darwin by `tools/mk-mesa.sh` (14,805,208 bytes, 1296 `gl*` functions, `softpipe`), behind `eagl.c` |
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
| OpenGL ES 3.1 | Mesa 25.0.7 for arm64-darwin from `tools/mk-mesa.sh`: 14,805,208 bytes, 1296 `gl*` functions, `softpipe` |
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

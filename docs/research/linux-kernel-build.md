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

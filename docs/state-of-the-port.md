# The state of this port, in one page

What works, what does not, and how to tell the difference -- written so that the next session does
not have to rediscover any of it.

## What works

| | evidence |
|---|---|
| XNU builds on Linux from Apple's sources, with cctools `ld64` | `docs/research/linux-kernel-build.md` |
| an image built here boots the Darwin user space: launchd, backboardd, SpringBoard | addendum 10 |
| the panel is writable, from the kernel and from a user-space program | addenda 5 and 7, `docs/demo/first-pixels.png`, `userland-frame.png` |
| CoreAnimation draws through Mesa `softpipe` | addendum 12, `docs/demo/system-ui-first.png` |
| **the iOS home screen is drawn by the system itself** | addendum 15, `docs/demo/home-screen.png` |

## How to get the home screen

```sh
cd ~/DSH-desktop/iosvm-work/hakios-xnu-kernel
qemu/wait-for-ui.sh /tmp/home.png        # boots, waits for a drawn screen, retries if the boot stalls
```

or by hand, which is what the script does:

```sh
QEMU_SMP=4 QEMU_TB_MB=1536 QEMU_RAM_GB=12 \
  IMG=$PWD/userland/system.apfs ROOT=$PWD/userland/root.apfs \
  CMDLINE="cpus=4 fb_beat=0" qemu/run-darwin.sh 1440
```

`cpus=4` is not optional: without it the kernel starts one processor and the guest clock runs about
eight times slower than the host's. `fb_beat=0` is not optional either: with the default the display
driver waits for a refresh boundary the software renderer never lands near, the swap queue fills, and
every frame after the first is dropped -- the panel then shows the boot logo forever.

## What does not work yet

| | what is known |
|---|---|
| the screen does not appear on every boot | the stalled boots stop about two minutes in with `lsd` burning CPU; the successful one has the same warning, so it is a timing race, not a different path |
| the Weather and Calendar widgets are white | their content comes from extension processes, which are not running here |
| 5-7 frames a second | 148 ms a frame, of which 141 ms is Mesa rasterising 2.7 M pixels on the guest's CPUs |
| no GPU acceleration | the accelerated path needs the Adreno driver to create `HakiosGPU`; QEMU has no Adreno and no firmware for one |

## The three boot arguments that matter

| argument | why |
|---|---|
| `cpus=4` (with `QEMU_SMP=4`) | start four processors; the loader puts `cpus=1` in the boot line unless asked |
| `fb_beat=0` | do not pace frame presentation to a refresh boundary (addendum 15) |
| `metal=1` | make the accelerator name the Metal plug-in; nothing loads it yet, so it changes nothing today |

## Patches this repository contributes

`patches/` in the project repository, each with the failure that motivated it: the display bringing up
its show service, a stage0 that can run a program before init, an explicit `newUserClient` so the
client class cannot be dropped, a user client that starts without a GPU, `build.sh` fixes (dyld cache
path, no halting demo job), and init finding its own image without `argv[0]`.

## The interface can now be operated, not only photographed

```sh
qemu/demo.sh /tmp/demo          # boot, then: open Settings, a page inside it, back home, Control Center
qemu/tap.py X Y                 # a tap, from the host
qemu/tap.py --swipe X0 Y0 X1 Y1 # a gesture, from the host
VNC=1 qemu/run-darwin.sh 600    # and watch it live on 127.0.0.1:5909
```

What was proven, with pictures in `docs/demo/` and `docs/demo/sequence/`:

| from the host | state |
|---|---|
| watch the screen | works — VNC or screendump |
| tap an icon, open an application | works — Settings, from `qemu/tap.py` |
| navigate inside an application | works — Settings -> Screen Time |
| swipe up from the bottom edge | works — back to the home screen |
| swipe down from the top right | works — Control Center, backdrop blur included |

Two things that are easy to get wrong, both written down in the research note: the events must go
over **QMP** (the human monitor's `input-send-event` delivers buttons and no coordinates), and in a
gesture the finger is down for the whole thing — press, move, release, in that order.

## The tools that reproduce it

They live in `tools/qemu/`, copied from the working tree so that this repository contains what it
describes:

| tool | what it does |
|---|---|
| `wait-for-ui.sh` | boots, retries until a boot reaches a drawn screen, optionally leaves the machine running (`KEEP=1`) |
| `tap.py` | a tap or a swipe at panel coordinates, sent from the host over QMP |
| `demo.sh` | the whole sequence in one command |

`patches/0007` and `0008` carry the two changes made while proving the input path: the event logging
that showed what the driver actually receives, and the VNC console becoming the primary display (which
is what makes a VNC client's pointer absolute rather than relative).

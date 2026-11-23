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
| (experiment, branch `experiment/metal-translation`) a synthetic Metal client's blit/present is translated to the host's Vulkan, checked byte for byte and shown in a browser | `docs/stage29-metal-translation.md`, `docs/demo/metal-translation-webview.png` |
| (experiment, same branch) CoreAnimation's Metal renderer reaches this port's device plug-in, its render encoder is **serialized and executed** (metalwire protocol 3): render pass, 16–17 sets, 3 indexed draws with their recorded buffer offsets; the layer image and the R8 luma ramp arrive through `replaceRegion:`, the display texture is a real IOSurface, and the BootUI frame is a picture in the browser (0 VUID, lavapipe) | `docs/stage30-ca-metal.md`, `docs/stage31-ca-metal-wire.md`, `docs/stage33-ca-metal-picture.md` |
| (experiment, same branch) CoreAnimation's **desktop** composites reach the host (metalwire protocol 4 + host GL for SpringBoard): the composited desktop is **upright** (Metal's y-up NDC compensated in the CA→Vulkan path), the background/Dock blur family (8 hand-written GLSL kernels), RGBA16Float (format 115) and RG8Unorm (format 30) are host implementations, and the `Tmua` material samples its support texture. Stage 39 fixed D8: a content-aware per-frame copy keeps the two display surfaces equivalent, so the status bar/Dock/Search are in **every** frame (0 black frames over 260 lavapipe / 472 V100 steady frames; the last 250 pairs of surfaces differ by median 1 byte). The final boots acknowledge 2,119 (lavapipe) and 2,295 (V100) frames with `pipelines 0, skipped draws 0` and wire error 0, and record **2.83 fps** (lavapipe) / **2.55 fps** (V100) — same binary generation; the older 1.82/1.96 pair belongs to a different generation. Pipeline libraries are described by hash/entry names, so a pipeline without an embedded shader costs its draws, never the frame. Still open: the wallpaper is black and two widgets are white placeholders (guest/image side), and five blur kernels have only fixture verification because Control Center/Spotlight does not open | `docs/stage39-d8-double-buffer.md`, `docs/stage35-wp4-close.md`, `docs/stage34-wp4-desktop.md` |
| (experiment, same branch) the desktop's **4.1 leak/OOM soak** is closed: the host reclaims the per-frame 256 KiB buffers the wire never destroys (2 live / 0.5 MB, `reclaimed=0`) and the plug-in pools the buffers CoreAnimation keeps, tracked by frame availability (`HakiosMetalBuffer alloc=5 reused=2398`). A 2,400 s lavapipe soak holds **host RSS 185.5 → 195.6 MB (end/start 1.05)**, the last 300 frames within 1.9 MB, with a **25.3-minute desktop**, 2,991 frames / 29,634 draws, `pipelines 0, skipped draws 0`, wire error 0, and **0 jetsam / 0 memorystatus / 0 OOM / 0 real panics**. OOM is judged by events plus `available` ≥500 MB, not by `free` (16 MB free / 1,135 MB available is not OOM). Tools: `MW_LEAK_CHECK=1`, `MW_BUFFER_LOG=1`, the `--leak` regression (the suite is 15 checks), lifecycle counters every 600 committed frames; plug-in md5 `bcaf99c6…`, host `ad778a06…`, default root image `a4a6e2ce…`. Boundaries: the inherited `-dealloc` chain is never called (`base_dealloc=0`; the queue's safe release is marker-ON only, validated by a 1,500 s run on `0d6a0c21…`), and the t35 exit RSS jump was not reproduced | `docs/stage43-leak-and-oom.md` |

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
| SMP>1 (multi-core) | single vCPU is the baseline; 2 vCPUs give +58–60 % in short runs but the stage-44 T2 (`tb-size=4096`) and the delivery-value control **both panicked** on a ticket lock with both vCPU threads busy — host starvation/preemption ruled out. Stage 45's v1 probe located the owner in the waitq wake path (`_waitq_select_queue_flush+0x134`, the `thread_go` site); stage 46's v2 probe then caught the owner's last eight stages **all in the pset-ticket acquire/release loop** at `_sched_timeshare_maintenance_continue+0xf4` (`_pset0+0x100`, last taker `_thread_quantum_expire+0x26c`, `pset#1,800,157` / ring 26,712,436 — the stage-27 T6 site), while the round's other SMP=2 run was a mutex interlock timeout. **H-B (guest waitq/scheduling under MTTCG) is supported at function/lock level; the owner loops/holds in the timeshare-maintenance path, so a plain lock-order inversion is not the best description.** The probe kernels (`eaf71509…`, `1bce762b…`) are not the delivery kernel (`f62c3d4b…`, restored); `isolcpus`/stopping win2022 still needs approval. Multi-core is still not deliverable — single vCPU remains the baseline — see `docs/stage46-waitq-deep-probe.md` |

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

### Operating it from a VNC viewer

```sh
tools/qemu/vncbridge.py            # 127.0.0.1:5910, the panel's own 1125x2436
```
point any VNC viewer at it and click: taps open applications, a one-second press raises the context
menu, drags work. QEMU's own VNC console can only show the machine -- its pointer arrives at the
guest as relative motion, which the touch driver cannot use -- so the bridge takes the client's
pointer and sends it as touches instead, over QMP.

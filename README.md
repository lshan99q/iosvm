<div align="center">

# IOSVM

**iOS, booting in QEMU on Linux — and operable from a web browser.**

[![status](https://img.shields.io/badge/status-boots%20to%20the%20home%20screen-brightgreen)]()
[![platform](https://img.shields.io/badge/host-Linux%20x86__64-blue)]()
[![guest](https://img.shields.io/badge/guest-Darwin%2016.5%20%C2%B7%20arm64-blue)]()
[![renderer](https://img.shields.io/badge/renderer-Mesa%20softpipe%20or%20host%20GL%20replay-orange)]()
[![license](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

<img src="docs/demo/control-center.png" width="230" alt="Control Center">
<img src="docs/demo/settings-app.png" width="230" alt="Settings">
<img src="docs/demo/screen-time-from-host.png" width="230" alt="Screen Time">

<sub>All three are the panel of a machine booted by this project: Control Center (backdrop blur and
all), Settings, and Screen Time — the last two reached by **clicking in a browser**.</sub>

</div>

---

## What this is

A Darwin/XNU port that boots Apple's **iOS 16.5 user space** in QEMU on ordinary Linux, draws its
system UI, and lets you drive it from the host: taps, swipes and the software keyboard, through a QMP
control channel and a small web/VNC bridge that this repository carries.

The point is the loop, not the screenshot:

```
    your browser ──► webvnc.py ──► QMP ──► virtio-tablet ──► Darwin's touch driver
         ▲                                                        │
         │                                                        ▼
    JPEG frames ◄── screendump ◄── ramfb ◄── IOMobileFramebuffer ◄── CoreAnimation
                                                       ▲
                                        Mesa softpipe ─┘  (a CPU rasteriser, hence the frame rate)
```

The second, faster loop — the host does the drawing, the guest only records it:

```
    guest CoreAnimation ──GL records──► host_glwire ──► real GL (a V100 here)
                                             │
                                             └──► JPEG stream ──► browser, clicks back over QMP
```

## What works today

| | state | evidence |
|---|---|---|
| boot to the home screen | **yes**, ~8 minutes, about 40 % of attempts (the rest stall inside the port) | [home screen](docs/demo/home-screen.png) |
| tap an icon, launch an application | **yes** | [Settings](docs/demo/settings-app.png) |
| navigate inside an application | **yes** | [Screen Time](docs/demo/screen-time-from-host.png) |
| swipe (home gesture, pull down) | **yes** | [Control Center](docs/demo/control-center.png) |
| operate it from the host | **yes** — QMP, `tools/qemu/tap.py` | [addendum 28](docs/research/linux-kernel-build.md) |
| operate it from a browser | **yes** — `tools/qemu/webvnc.py` | [Focus, by browser click](docs/demo/webvnc-focus-page.png) |
| type text | **yes**, as taps on the software keyboard | [typing `abc`](docs/demo/webvnc-typed-abc.png) |
| networking to the host (LAN) | **yes** — `en0` 10.0.2.15, HTTP both ways | [addendum 30](docs/research/linux-kernel-build.md) |
| names, DNS | no — configd's plugin never registers here | addendum 30 |
| GPU acceleration | no — a CPU rasteriser is what this machine has | — |
| animation smoothness | ~4–7 fps locally; **15.9 fps with the host drawing the frame** (2 pinned vCPUs), complete picture | [stage 25](docs/stage25-multicore.md) |
| complete frame, drawn on the host | **yes** — the guest's CoreAnimation calls are replayed on the host's GL, the result is what you see | [stage 23 verdict](docs/stage23-verdict.md) |
| watch that frame in a browser | **yes** — `tools/glwire/webview.py`, JPEG stream + clicks back through QMP | `tools/glwire/webview.html` |

`docs/state-of-the-port.md` is the one-page handover view of the same table.

## Quick start

```sh
git clone --recursive https://github.com/lshan99q/iosvm.git
cd iosvm

# 1. Bring your own IPSW — nothing of Apple's is in this repository
tools/unpack-ipsw.sh ~/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw

# 2. Build the kernel, the images and the loader (see docs/ for the pieces)
make

# 3. Boot, and wait until something is actually drawn (it retries: not every boot lands)
tools/qemu/wait-for-ui.sh /tmp/home.png

# 4. Operate it — from a browser…
tools/qemu/qmpmux.py --qmp 4445 --port 4465
tools/qemu/webvnc.py --port 5980 --qmp 4465      # then http://127.0.0.1:5980/
#    …or from a shell
tools/qemu/tap.py 683 1689                      # the Settings icon

# 5. Or let the host draw the frame: boot with the guest replaying only, then watch that frame
#    (the guest's own panel stays half-empty — see "Drawing the frame on the host" below)
GLWIRE_NO_PANEL=1 GLWIRE_STATIC_PATCH=1 \
  GLWIRE_STATIC_PATCH_FILE=/tmp/patch-full.bgra GLWIRE_SHM=/dev/shm/glwire-frame \
  tools/glwire/host_glwire &                   # the replay, publishing each frame
python3 tools/glwire/webview.py --port 6083 --crop 1624   # then http://127.0.0.1:6083/
```

The tools in `tools/qemu/` are the ones this project actually uses:

| tool | does |
|---|---|
| `wait-for-ui.sh` | boots, retries until a screen is drawn; `KEEP=1` leaves the machine running |
| `tap.py` | a tap or a swipe at panel coordinates, over QMP |
| `qmpmux.py` | QEMU serves one QMP client; this serves many |
| `webvnc.py` | the browser viewer: frames out, clicks and keys in |
| `vncbridge.py` | the same for a VNC viewer instead of a browser |
| `demo.sh` | the whole sequence in one command, a picture of every step |

## Drawing the frame on the host (stages 23–24)

The CPU rasteriser is the ceiling, so the next step was to stop asking it to draw: the guest records
its CoreAnimation GL calls, ships them over a socket, and the host replays them on a real GL device.
The frame the host composites is the one to look at.

| | measured |
|---|---|
| host replay, complete picture | **13.9 fps** one vCPU (the long-run setting) / **15.6–18.3 fps** on two pinned vCPUs, which is a burst mode: the guest still panics about once an hour on a contended scheduler lock; picture byte-identical to the baseline |
| the host's own cost per frame | ~41 ms before `gkbf=1`; **~1.4 ms** with it (records 90 KB → 4–19 KB a frame) — never the bottleneck |
| the guest's frame budget | was 117.1 ms = CA 71.0 + encode/copy 46.8 + wait 1 µs; the "CA 71 ms" turned out to be the display driver's **full-panel upscale blit** (750×1624 → 1125×2436, 2.74 M pixels): 62 ms/swap, now skipped (`fb_noblit=1`) |

Measured 2026-11-17: with `gkbf=1 glwire_ack_only=1 fb_noblit=1` the guest spends 75 ms a frame and the
picture is byte-identical to the 9.09 fps baseline. The wall-clock figure moves ±30% with host load, so
compare paired runs, never single shots. See [stage 24 blit](docs/stage24-blitfast.md) and
[the gkbf negative result](docs/stage24-gkbf.md).

Two consequences worth writing down, because both look like they should work and do not:

- **the guest is not waiting for the host.** `wait = 1 µs`, so asynchronous replies and double
  buffering buy nothing.
- **the records are tiny.** 12.65 KB per frame, 0.26 % of the 4.87 MB picture; de-duplicating them
  saves under 0.12 ms. The 46.8 ms is a 4.9 MB round trip, not encoding.

`virtio-gpu` (stage 23) is implemented end to end — control queue, 2D resource, scanout, transfer,
flush — and measured **zero** frame-rate gain (1.80 → 1.85 fps, inside the noise): the transport was
never the bottleneck. It stays as the groundwork for a 3D path, not as a speed-up.

## How it came to boot

Three things had to be true, and the research note records the measurement behind each:

1. **the display must have a show service** — on a phone the GPU driver starts it; in QEMU nothing
   did, so user-space painting had nowhere to land (`patches/0001`);
2. **`fb_beat` must be 0** — the driver's on-the-beat pacing drops every frame after the first when
   rendering is this slow, which is exactly the "boots to a logo and stops" symptom;
3. **something must send the touch** — a launchd job of ours is not reliably executed here, so the
   synthetic touches come from `/etc/rc`, which `init` runs before it hands process 1 to launchd.

The long version — with the dead ends and three self-corrections —
is [`docs/research/linux-kernel-build.md`](docs/research/linux-kernel-build.md).

## Repository layout

| path | what |
|---|---|
| `patches/` | the changes made to the port: one patch per finding, with the reason |
| `tools/qemu/` | the boot-and-operate tooling above |
| `tools/glwire/` | the guest's GL wire protocol: `host_glwire.c` (the replay), `webview.py` (that frame in a browser) |
| `tools/apfs/` | reading and writing the images without Apple's tools |
| `guest/` | programs that run inside the machine (the painter, `netcfg`) |
| `proto/`, `host/vkbridge/` | GPU transport and its host-side Vulkan backend (submodules) |
| `docs/` | ADRs, the research note, the state of the port, demo screenshots |

## What this is not

- **Not Apple's software.** No kernel, no system image, no dyld cache, no firmware, no font lives
  here. You supply an IPSW you obtained yourself; see `PROVENANCE.md`. Three rules are vetoes: no
  Apple binaries in the tree, no impersonating a device to Apple's servers, no credentials.
- **Not a jailbreak.** The port's own kernel is lenient about code signing by construction (AMFI is a
  stand-in that accepts any parseable signature); nothing here attacks a real device.
- **Not fast.** The renderer is a CPU rasteriser. With the host drawing the frame the 11 fps target is
  met (13.9 fps / 75 ms a frame, 2026-11-17); the old 9.09 fps figure is kept in the stage-24 docs as the
  baseline it is compared against. 24 fps is a target, not a claim: it needs a real 3D path or
  same-architecture virtualisation.

## Documentation

| | |
|---|---|
| [`docs/state-of-the-port.md`](docs/state-of-the-port.md) | one page: what works, what does not |
| [`docs/research/linux-kernel-build.md`](docs/research/linux-kernel-build.md) | the long note: every measurement, and the mistakes |
| [`docs/architecture.md`](docs/architecture.md) | how the pieces fit |
| [`docs/phases.md`](docs/phases.md) | milestones, one acceptance command each |
| [`docs/adr/`](docs/adr) | the decisions, with their evidence |
| [`docs/zh/`](docs/zh) | Chinese translations |

## License

MIT for this project's code (`LICENSE`); Apple's open-source XNU keeps its APSL
(`LICENSE.APSL`), and the GPL-2.0 parts (`LICENSE.GPL2`) stay GPL-2.0. See `NOTICE`.

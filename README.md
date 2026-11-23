<div align="center">

# IOSVM

**iOS, booting in QEMU on Linux — and operable from a web browser.**
**在 Linux 上的 QEMU 中启动 iOS，并可通过网页浏览器操作。**

[![status](https://img.shields.io/badge/status-boots%20to%20the%20home%20screen-brightgreen)]()
[![platform](https://img.shields.io/badge/host-Linux%20x86__64-blue)]()
[![guest](https://img.shields.io/badge/guest-Darwin%2016.5%20%C2%B7%20arm64-blue)]()
[![renderer](https://img.shields.io/badge/renderer-Mesa%20softpipe%20or%20host%20GL%20replay-orange)]()
[![license](https://img.shields.io/badge/license-MIT-lightgrey)](LICENSE)

<img src="docs/demo/control-center.png" width="230" alt="Control Center">
<img src="docs/demo/settings-app.png" width="230" alt="Settings">
<img src="docs/demo/screen-time-from-host.png" width="230" alt="Screen Time">

<sub>All three are the panel of a machine booted by this project: Control Center (backdrop blur and
all), Settings, and Screen Time — the last two reached by **clicking in a browser**.<br/>
以上三张均为本项目启动的虚拟机屏幕：控制中心（含背景模糊）、设置、屏幕使用时间——后两者通过**浏览器点击**进入。</sub>

<br/>

[**English**](#english) · [**中文**](#中文)

</div>

---

<a id="english"></a>

# English

## Contents

- [What this is](#what-this-is)
- [Milestones](#milestones)
- [What works today](#what-works-today)
- [Quick start](#quick-start)
- [Drawing the frame on the host](#drawing-the-frame-on-the-host-stages-2324)
- [Multi-core: an open problem](#multi-core-an-open-problem-stage-27)
- [How it came to boot](#how-it-came-to-boot)
- [Repository layout](#repository-layout)
- [What this is not](#what-this-is-not)
- [Documentation](#documentation)
- [License](#license)

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

## Milestones

Dates are commit dates on `main`. The repository has about 700 commits from 2026-10-31 to 2026-11-18.

| Date | Milestone | Commit |
|---|---|---|
| 2026-10-31 | **M0** — the kernel is built on Linux from source and boots | `4101663` |
| 2026-10-31 | The user space boots: launchd, backboardd, SpringBoard | `9d3babb` |
| 2026-10-31 | **First pixels** — our own program lights the panel | `d41c95a` |
| 2026-10-31 | The system UI draws through Mesa softpipe (boot logo) | `3d054a8` |
| 2026-10-31 | **Home screen** — SpringBoard's UI on the panel | `5b0cfd3` |
| 2026-10-31 | **Operated from the host** — tapping Settings opens it; three levels deep to Screen Time | `f4fb4e9`, `c300753` |
| 2026-11-01 | **Operated from a browser**, including typing without a keyboard service | `16b5712`, `e4d7579` |
| 2026-11-01 | Host-side GL replay begins: the guest records GL calls and the host draws the panel | `0a3e128` |
| 2026-11-02 | A NULL-pixel fix carries the remote path to the home screen | `fb4e9fa` |
| 2026-11-03 | Stage 9 — boot stability: with the lock deadlines raised, 8 of 8 cold boots show no panic | `3bf05e0` |
| 2026-11-07 | **First real glyphs** on the screen through the host replay | `cecbd2e` |
| 2026-11-15 | virtio-gpu works end to end, but measured **zero** frame-rate gain — the transport is not the bottleneck | `b7fdef5` |
| 2026-11-17 | **Complete frame drawn on the host: 13.9 fps, 75 ms a frame** (the delivered setting, one vCPU) | `82190eb` |
| 2026-11-17 | Two pinned vCPUs: 15.8 fps median; a 30-minute run with 0 panics (later revisited, see below) | `cc33526` |
| 2026-11-18 | Stage 26 — `fb_hz=15` cuts CPU per frame from 69 ms to 51 ms with an identical picture | `cc17734`, `730e0f7` |
| 2026-11-18 | Stage 27 — multi-core reopened as a must-solve item; single-threaded TCG runs SMP=2 for 30 minutes with no panic, which points at parallel vCPUs | `b5bd190`, `6df855c` |
| 2026-11-22 | Stage 29 (experiment branch) — a Metal client's commands are recorded in the guest, translated to Vulkan on the host and shown in a browser: 2,000/2,000 frames byte-exact, 58.3 fps median for the synthetic 750×1624 client; CoreAnimation still draws through GL | [`experiment/metal-translation`](docs/stage29-metal-translation.md) |
| 2026-11-22 | Stage 30 (experiment branch) — CoreAnimation's Metal renderer is reached: the `metallib` assert and six Apple-implementation walls are answered, and it **records its own indexed draws** (three triangles a frame, `com.apple.coreanimation.draw.*`); no picture yet (no AIR→SPIR-V, no render wire) | [`docs/stage30-ca-metal.md`](docs/stage30-ca-metal.md) |
| 2026-11-22 | Stage 31 (experiment branch) — CoreAnimation's render encoder is **serialized on metalwire protocol 2**: render pass, 13 sets and 3 indexed draws a frame reach the host, which decodes and validates them (no pixels yet: WP-2 shaders) | [`docs/stage31-ca-metal-wire.md`](docs/stage31-ca-metal-wire.md) |
| 2026-11-22 | Stage 32 (experiment branch) — the host **executes** CoreAnimation's three pipelines and 3 draws / 13 sets on Vulkan (lavapipe): four hand-written Vulkan GLSL base functions, three Vulkan pipelines created, no Vulkan/validation error; a picture still waits on the wire's missing per-draw offset setters and on texture uploads | [`docs/stage32-ca-metal-execute.md`](docs/stage32-ca-metal-execute.md) |
| 2026-11-22 | Stage 33 (experiment branch, protocol 3) — **the picture appears**: per-draw buffer-offset setters and `replaceRegion:` (BGRA8 + R8Unorm) are on the wire, the display texture is a real IOSurface (≈300 physical segments), and the host renders CA's three recorded draws into it; the browser's JPEG stream shows the BootUI Apple logo. 0 VUID with the Khronos layer loaded; the v2 synthetic client stays 60/60 | [`docs/stage33-ca-metal-picture.md`](docs/stage33-ca-metal-picture.md) |
| 2026-11-22 | Stage 34 (experiment branch, protocol 4) — **the desktop composites execute** on the host: pipeline libraries are described by hash/entry names, and a pipeline without an embedded shader costs its draws, never the frame; the draw cap is measured at 512 and the missing report names what is absent. One hybrid boot acknowledges 462 frames with 0 wire errors; the icon grid, labels and status bar appear (flipped, wallpaper black) | [`docs/stage34-wp4-desktop.md`](docs/stage34-wp4-desktop.md) |
| 2026-11-23 | Stage 35 (experiment branch, WP-4 close) — the desktop is **upright** (Metal's y-up NDC compensated in the CA→Vulkan path), the **8-kernel background/Dock blur family** and **RGBA16Float (format 115)** are host implementations, and the `Tmua` material samples its support texture (the Dock white band and the Search capsule are gone). The closing boot acknowledges 706 frames with `pipelines 0, skipped draws 0`, and recorded-frame baselines exist for lavapipe (1.82 fps) and the V100 (1.96 fps) — an earlier binary generation. Open: black wallpaper and white widget placeholders (guest/image side); the two-buffer alternation D8 was fixed in stage 39 | [`docs/stage35-wp4-close.md`](docs/stage35-wp4-close.md) |
| 2026-11-23 | Stage 39 (experiment branch) — **the two display buffers are equivalent (D8 fixed)**: the host copies the previous picture into a preserving frame's sibling surface, but only when the source is at least as rich, so the Dock/Search layers can no longer be overwritten; Metal's **RG8Unorm (format 30)** is a real image (the second wire error an earlier boot exposed). The final boots hold the complete desktop in both surfaces — 0 black frames, `pipelines 0, skipped draws 0`, wire error 0 — at **2.83 fps** (lavapipe) and **2.55 fps** (V100) recorded-frame rate. Open: black wallpaper, white widget placeholders, and five blur kernels that only the fixture exercises | [`docs/stage39-d8-double-buffer.md`](docs/stage39-d8-double-buffer.md) |
| 2026-11-23 | Stage 43 (experiment branch) — **the 4.1 leak/OOM soak closes**: the host reclaims the per-frame 256 KiB buffers the wire never destroys (2 live / 0.5 MB, `reclaimed=0`) and the plug-in pools the buffers CoreAnimation keeps, tracked by frame availability (`HakiosMetalBuffer alloc=5 reused=2398`). A 2,400 s lavapipe soak holds host RSS **185.5 → 195.6 MB (1.05)** with a **25.3-minute desktop**, 2,991 frames, 0 jetsam / 0 memorystatus / 0 OOM, and `available` 1,135 MB; the criterion is events + `available` ≥500 MB, not `free` (16 MB is file cache). Tools: `MW_LEAK_CHECK`, `MW_BUFFER_LOG`, `--leak`; plug-in md5 `bcaf99c6…`, host `ad778a06…`. Boundary: the queue's inherited `-dealloc` chain is never called (`base_dealloc=0`; safe release is marker-ON only) | [`docs/stage43-leak-and-oom.md`](docs/stage43-leak-and-oom.md) |
| 2026-11-23 | Stage 44 (experiment branch) — the **4.2 multi-core recon**: T2 (`tb-size=4096`) and the delivery-value control **both panic** (2 frames at 14.6 min / 2,035 frames), despite pinning and both vCPU threads at 60–87 % CPU; **host starvation (H1) and host preemption (H-C) are rejected**, H-A stays strongest and H-B (guest waitq/idle path) gains weight. Multi-core is **not deliverable** — single vCPU stays the baseline; T5 (kernel build `LC_MAIN`→`LC_UNIXTHREAD`, waitq/idle stage probes, `isolcpus`/stop win2022 needs approval) is the only remaining path | [`docs/stage44-multicore.md`](docs/stage44-multicore.md) |
| 2026-11-23 | Stage 45 (experiment branch) — **T5's probe kernel located the lock owner**: a single-vCPU self-check passed (2,089 frames, 0 panics, same desktop), and one SMP=2 run captured the owner's last eight stage markers — the owner took the `_pset0` lock inside `_waitq_select_queue_flush+0x134` (the `thread_go` site), after `_ipc_port_clear_receiver → _waitq_wakeup64_all_locked → do_waitq_select_n_locked`, and never returned. **H-B (guest waitq/scheduler loses a wake-up or does not return under MTTCG) is now supported by a located owner path; H-A is the enabling condition.** Multi-core is still not deliverable — single vCPU stays the baseline; the probe kernel `eaf71509…` is not the delivery kernel `f62c3d4b…` (restored) | [`docs/stage45-t5-probe.md`](docs/stage45-t5-probe.md) |
| 2026-11-23 | Stage 46 (experiment branch) — the **waitq deep probe (v2)**: the 550-line probe adds `thread_go` / `do_waitq_select_n_locked` / `waitq_select_queue_flush` / pset+waitq+thread_call ticket marks; the single-vCPU self-check stays clean (1,888 frames, 0 panics, same desktop). Two SMP=2 runs: run A is a **mutex interlock timeout**, run B is a ticket timeout whose **owner stages are all on the pset ticket** — `_sched_timeshare_maintenance_continue+0xf4` taking/releasing `_pset0+0x100` (last taker `_thread_quantum_expire+0x26c`), `pset#1,800,157` / `stage ring 26,712,436`, i.e. the stage-27 T6 site. **H-B is supported at function/lock level; a simple lock-order inversion is not the best description.** The probe kernel `1bce762b…` is not the delivery kernel `f62c3d4b…` (restored) | [`docs/stage46-waitq-deep-probe.md`](docs/stage46-waitq-deep-probe.md) |

## Performance over time

![Frame rate history](docs/charts/fps-history.png)

The development dashboard, and how it is regenerated:

![Development dashboard](docs/charts/dashboard.png)

- `python3 docs/charts/make_chart.py` redraws `docs/charts/fps-history.png` from the measured runs.
- `python3 docs/charts/dashboard.py` redraws `docs/charts/dashboard.png`.

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
| animation smoothness | ~4–7 fps on the CPU rasteriser; **13.9 fps with the host drawing the frame** (one vCPU, the delivered setting); two pinned vCPUs reach ~15.9 fps in short runs, but stage-44's SMP=2 runs still panic and stage 45/46's probes located the owner in the waitq wake path and then the timeshare-maintenance pset loop (host starvation ruled out) — [open problem](#multi-core-an-open-problem-stage-27) | [stage 25](docs/stage25-multicore.md), [stage 27](docs/stage27-multicore.md), [stage 44](docs/stage44-multicore.md), [stage 45](docs/stage45-t5-probe.md) |
| complete frame, drawn on the host | **yes** — the guest's CoreAnimation calls are replayed on the host's GL, the result is what you see | [stage 23 verdict](docs/stage23-verdict.md) |
| Metal calls translated to the host's Vulkan (experiment, separate branch) | **partly** — a Metal client's blit/present is recorded in the guest, executed on the host's Vulkan GPU and served in a browser; separately, CoreAnimation's Metal renderer is reached, its render encoder serialized and executed on Vulkan; since stage 35 the window server's **desktop composites** (31–88 draws a frame) are **upright**, the background/Dock blur family (8 kernels), RGBA16Float (format 115), RG8Unorm (format 30) and the `Tmua` material semantics are host implementations, and since stage 39 the **two display buffers are equivalent** (D8 fixed by a content-aware per-frame copy) — the icons, labels, status bar, Dock and Search show the system's own layers in **every** frame on lavapipe and the V100, at 2.83 / 2.55 fps recorded-frame rate (same binary generation; the 1.82 / 1.96 pair is an earlier generation). The desktop's **4.1 soak** keeps host RSS flat (185.5 → 195.6 MB, end/start 1.05) with 2 live buffers / 0.5 MB and 0 jetsam/OOM over a 25.3-minute lavapipe run (plug-in `bcaf99c6…`, host `ad778a06…`; `MW_LEAK_CHECK`, `MW_BUFFER_LOG`, the `--leak` test). Still open: the wallpaper is black and two widgets are white placeholders (guest/image side), five blur kernels have only fixture coverage because Control Center/Spotlight does not open, and the queue's inherited `-dealloc` chain is never called (`base_dealloc=0`; safe release is marker-ON only); protocol 4 makes an unknown pipeline cost a draw, never the frame | [stage 29](docs/stage29-metal-translation.md), [stage 30](docs/stage30-ca-metal.md), [stage 33](docs/stage33-ca-metal-picture.md), [stage 34](docs/stage34-wp4-desktop.md), [stage 35](docs/stage35-wp4-close.md), [stage 39](docs/stage39-d8-double-buffer.md), [stage 43](docs/stage43-leak-and-oom.md) |
| watch that frame in a browser | **yes** — `tools/glwire/webview.py`, JPEG stream + clicks back through QMP | `tools/glwire/webview.html` |

`docs/state-of-the-port.md` is the one-page handover view of the same table.

## Quick start

Nothing in this repository is Apple's. You supply an IPSW you obtained yourself.

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
| host replay, complete picture | **13.9 fps** on one vCPU (the delivered setting; picture byte-identical to the baseline). Two pinned vCPUs reach ~15.9 fps (stage 25) but have panicked on a scheduler lock in stage 27 — see below |
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

## Multi-core: an open problem (stage 27)

Multi-core is **not ruled out** — it is the largest measured speed-up available, and it is being
worked on as a must-solve item. Pinning helps; it has not yet made the configuration reliable.

| configuration (same host load) | median fps | result |
|---|---|---|
| 1 vCPU, pinned | 9.9 | baseline |
| 2 vCPUs, pinned to two host cores | 15.6–15.9 | +58–60 %; stage 25 ran 30 min with 0 panics, stage 27 reproduced scheduler-lock panics |
| 4 vCPUs, pinned | 14.4 | no gain: total CPU per frame nearly doubles (MTTCG overhead and lock contention) |
| 2 vCPUs, not pinned | — | Ticket spinlock panic within 5 frames |
| 2 vCPUs, `thread=single` (T1), 30 min | no panic | the deadlock needs parallel vCPUs (hypothesis H-A) |
| 2 vCPUs, MTTCG (`thread=multi`), pinned, 25-min runs | — | stage 44: `tb-size=4096` panicked after 14.6 min / 2 frames, the delivery `tb-size=1536` after 2,035 frames; both vCPU threads busy, so host starvation/preemption ruled out |
| 2 vCPUs, MTTCG, T5 probe kernel (owner stages) | — | stage 45: panic at 17:07:21 (187 s after boot, 2 frames); the owner's last stages: `_ipc_port_clear_receiver` (50/51/52) → `_waitq_wakeup64_all_locked` → `_waitq_select_queue_flush+0x134`, holding `_thread_call_groups+0x280` — H-B supported |
| 2 vCPUs, MTTCG, probe v2 (lock/loop marks) | — | stage 46: run A a mutex interlock timeout; run B a ticket timeout with the owner looping on `_pset0+0x100` in `_sched_timeshare_maintenance_continue+0xf4` — the stage-27 T6 site, H-B at function level |

Stage 27 (2026-11-18) records five pinned two-vCPU runs: two ran 30 minutes without a panic, three
crashed within 5–10 minutes, all on the scheduler processor-set lock (`_pset0`). Loosening the
ticket-lock timeout 4× did not help, which points at a lock holder that stops releasing rather than a
short host preemption. The first test (T1) ran with `thread=single` and passed 30 minutes with no panic,
which supports hypothesis H-A: the failure needs vCPUs running in parallel. The rest of the test
matrix (`tb-size`, CPU model, a panic-time dump of every CPU, host isolation) is in
[stage 27](docs/stage27-multicore.md). Until those tests pass, the delivered setting stays one vCPU.

Stage 44 (2026-11-23) ran the cheapest untried row, T2 (`tb-size=4096`), and a same-day control at the delivery value: **both panicked** on the same ticket lock, T2 before the desktop (2 frames) and the control after 2,035 frames. The two vCPU threads were executing at 60–87 % CPU on a narrow pin, so host starvation (H1) and preemption (H-C) are rejected; the lock owners landed in the guest's `_waitq_pull_thread_locked` / `_idle_thread` paths, which keeps H-A strongest and raises H-B. Multi-core is not deliverable; single vCPU remains the baseline, and T5 (a bootable probe kernel plus a real isolation decision) is the only remaining path — see [`docs/stage44-multicore.md`](docs/stage44-multicore.md).

Stage 45 (2026-11-23) built the T5 probe kernel (the build pipeline now uses the cctools linker: `LC_MAIN`→`LC_UNIXTHREAD` is a linker choice, and `hakios-xnu-kernel/tools/kernel-rebuild.sh` does the whole check in one command), passed a single-vCPU self-check (2,089 frames, 0 panics, same desktop), and caught the owner in one SMP=2 run: `_ipc_port_clear_receiver → _waitq_wakeup64_all_locked → do_waitq_select_n_locked → _waitq_select_queue_flush+0x134` (the `thread_go` call site) holding the pset lock and not returning. **H-B is now supported by a located owner path; H-A remains the enabling condition.** Multi-core is still not deliverable; the next T5 step is the `thread_go` / `do_waitq_select_n_locked` loop and the pset↔waitq lock order — see [`docs/stage45-t5-probe.md`](docs/stage45-t5-probe.md).

Stage 46 (2026-11-23) deepened the probe (v2): marks on `thread_go`, `do_waitq_select_n_locked`, `waitq_select_queue_flush`, and the pset/waitq/thread_call ticket acquire/release pairs. The single-vCPU self-check stays clean (1,888 frames, 0 panics, same desktop). Two SMP=2 runs: run A is a **mutex interlock timeout** (a different lock class), run B is a ticket timeout whose owner's last eight stages are **all in the pset-ticket acquire/release loop at `_sched_timeshare_maintenance_continue+0xf4`** (`_pset0+0x100`; last taker `_thread_quantum_expire+0x26c`), with `pset#1,800,157` and a 26.7 M-event stage ring. **H-B is now supported at function/lock level; the owner loops in the timeshare-maintenance path rather than holding a second lock, so a plain lock-order swap is not the best description.** The next step is v3: loop head/tail stages with the condition variables and an iteration counter, a `thread_quantum_expire` entry mark, and a light mutex-timeout context — or shipping SMP>1 only as an experimental feature with a startup guard — see [`docs/stage46-waitq-deep-probe.md`](docs/stage46-waitq-deep-probe.md).

## Metal translation (experiment, stage 29)

A separate branch, [`experiment/metal-translation`](docs/stage29-metal-translation.md), asks the
other half of [ADR-002](docs/adr/ADR-002-api-translation.md): instead of replaying CoreAnimation's
GL records, replace the guest's Metal device and translate at the API level. It is an experiment,
not a delivered path — the Replay path above is untouched and remains the baseline.

What it shows: the port's own `IOAcceleratorES` + `MetalPluginName` discovery path loads a Metal
device bundle we wrote; a Metal client in the guest can make a device, a queue, a buffer, a texture
and a blit+present; the command buffer is recorded at `commit` and executed on the host's Vulkan
device (the second V100, slot 1); the picture is read back, CRC-checked and published into the same
shared memory `tools/glwire/webview.py` serves, so a translated frame is visible in a browser:

![A translated frame, served by webview.py](docs/demo/metal-translation-webview.png)

| | measured (2026-11-21/22, one vCPU, GPU 1, synthetic 750×1624 client) |
|---|---|
| frames / correctness | 5 × 400 frames, 2,000 / 2,000 `crc_ok=1`, full `memcmp` of source vs read-back identical, 0 panics |
| fps | median **58.3** (57.4–60.1) |
| guest, per frame | picture fill 2.6 ms, Metal calls 0.19 ms, commit 7.1 ms, wait 13.7 ms |
| host, per frame | 4.2 ms (GPU copy 0.94 ms, read-back + sampled checksum 0.31 ms, publish 2.9 ms) |
| wire | 0 picture bytes on the socket: the host reads the pixels from guest RAM (`hakios_vtop`, as the Replay path does) |
| transport, before/after | **1.94 fps** with the 4.87 MB picture inline on the socket, **58.3 fps** median with the by-reference upload — the transport, not the translation, was the cost |

![Frame rate by implementation: the transport and the checksum were the cost](docs/charts/metal-translation-history.png)

What it does **not** show: a CoreAnimation frame. This port's window server does not take a Metal
path (its renderer library is missing and, behind it, an AIR→SPIR-V compiler), so the translated
client is a synthetic Metal program painted by the test, and its 58.3 fps must not be compared with
the delivered 13.9 fps as if it were the same workload. The honest comparison, the dead ends and
the self-corrections are in [stage 29](docs/stage29-metal-translation.md); the decision is
[ADR-013](docs/adr/ADR-013-metal-translation.md); the research note is
[the feasibility report](docs/research/metal-translation-feasibility.md).

Stage 30 then asked how far CoreAnimation itself gets when the window server *is* shown the device.
The answer is in [stage 30](docs/stage30-ca-metal.md): past the `metallib_url` assert, past Apple's
own `_MTLDevice`/`_MTLLibrary`/`_MTLFunction`/`_MTLPipelineLibrary` implementations that a stub
subclass inherits, to its first recorded indexed draws — three triangles a frame, pipelines named
`com.apple.coreanimation.draw.*`, from the `fixed_*` base functions the missing metallib would have
carried. That recording is now serialized (stage 31): the render pass, 13 sets and 3 indexed draws
a frame cross on metalwire protocol 2 and the host decodes them. There is still no picture: no
shader is compiled, the display's IOSurface is not wrapped, and the host acknowledges the frame
without drawing it. The static analysis behind every wall is
[`docs/research/ca-metallib-url.md`](docs/research/ca-metallib-url.md).

How to run it (the branch owns the tooling; nothing here changes main's build):

```sh
make metalwire-host                              # the Vulkan translator (no guest needed)
make metalwire-test                              # its CI-safe test on lavapipe
HAKIOS=<hakios checkout> make metalwire-guest     # guest bundle + client (needs the iPhoneOS SDK)
HAKIOS=<hakios checkout> make metalwire-worktree  # private run tree (sudo once; ~90 MB)
tools/metalwire/prepare-guest.sh translate        # root image with the Metal client
tools/metalwire/run-guest.sh translate 300        # boot it (instance 3, ports 4474/4475)
```

It uses GPU 1 and ports 9111/9210/4474/4475 on purpose, so it can run beside another experiment on
the same host; the delivered Replay path (GPU 0, port 9110) is never touched.

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
| `host/metalwire/`, `guest/metal-plugin/` | the Metal→Vulkan experiment: the host Vulkan translator and the guest Metal device bundle (stage 29, branch `experiment/metal-translation`) |
| `tools/metalwire/` | the experiment's own run tree, root-image preparation and measurement harness |
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
| [`docs/stage25-multicore.md`](docs/stage25-multicore.md) | the first pinned two-vCPU measurements |
| [`docs/stage27-multicore.md`](docs/stage27-multicore.md) | the multi-core problem: hypotheses, test matrix, acceptance |
| [`docs/research/linux-kernel-build.md`](docs/research/linux-kernel-build.md) | the long note: every measurement, and the mistakes |
| [`docs/stage29-metal-translation.md`](docs/stage29-metal-translation.md) | the Metal→Vulkan experiment: what was measured, and what it does not prove |
| [`docs/stage30-ca-metal.md`](docs/stage30-ca-metal.md) | CoreAnimation's Metal renderer: the walls, the first recorded draws, and why there is no picture yet |
| [`docs/stage31-ca-metal-wire.md`](docs/stage31-ca-metal-wire.md) | stage C WP-1: the render encoder serialized on protocol 2, what the host decodes, and why there is still no picture |
| [`docs/stage32-ca-metal-execute.md`](docs/stage32-ca-metal-execute.md) | stage C WP-2: the host executes CoreAnimation's three pipelines and 3 draws / 13 sets on Vulkan |
| [`docs/stage33-ca-metal-picture.md`](docs/stage33-ca-metal-picture.md) | stage C WP-3: the picture — offset setters, `replaceRegion:` and a real IOSurface |
| [`docs/stage34-wp4-desktop.md`](docs/stage34-wp4-desktop.md) | WP-4: the desktop composites execute; protocol 4's dynamic-pipeline capture and the three gaps it named |
| [`docs/stage35-wp4-close.md`](docs/stage35-wp4-close.md) | WP-4 close: the upright desktop, the blur family, RGBA16Float, the material semantics, and the baselines |
| [`docs/stage39-d8-double-buffer.md`](docs/stage39-d8-double-buffer.md) | stage 39: the two display buffers become equivalent (D8 fixed), RG8Unorm (format 30), and the final frame rates |
| [`docs/stage43-leak-and-oom.md`](docs/stage43-leak-and-oom.md) | stage 43: the 4.1 leak/OOM soak, the bounded host buffer set, the memory criterion, and the tools |
| [`docs/stage44-multicore.md`](docs/stage44-multicore.md) | stage 44: the 4.2 multi-core recon — T2/control, H1/H-C rejected, and the T5 path |
| [`docs/stage45-t5-probe.md`](docs/stage45-t5-probe.md) | stage 45: the T5 probe kernel — owner stages, the waitq wake that does not return, and the restore |
| [`docs/stage46-waitq-deep-probe.md`](docs/stage46-waitq-deep-probe.md) | stage 46: probe v2 — the pset-ticket loop, run A/B, v1-vs-v2, and the v3 plan |
| [`docs/research/ca-metallib-url.md`](docs/research/ca-metallib-url.md) | the static analysis: the NSBundle lookup, the 451-module AIR corpus, the pipeline database, every fault address |
| [`docs/research/metal-translation-feasibility.md`](docs/research/metal-translation-feasibility.md) | the M1 note: where Metal can be intercepted, why CA is not on it, which translator exists |
| [`docs/adr/ADR-013-metal-translation.md`](docs/adr/ADR-013-metal-translation.md) | the decision: device plug-in, commit-level translation, by-reference transport, host Vulkan |
| [`docs/architecture.md`](docs/architecture.md) | how the pieces fit |
| [`docs/phases.md`](docs/phases.md) | milestones, one acceptance command each |
| [`docs/adr/`](docs/adr) | the decisions, with their evidence |
| [`docs/zh/`](docs/zh) | Chinese translations |

## License

MIT for this project's code (`LICENSE`); Apple's open-source XNU keeps its APSL
(`LICENSE.APSL`), and the GPL-2.0 parts (`LICENSE.GPL2`) stay GPL-2.0. See `NOTICE`.

<p align="right"><a href="#iosvm">↑ back to top / 返回顶部</a></p>

---

<a id="中文"></a>

# 中文

## 目录

- [项目简介](#项目简介)
- [里程碑](#里程碑)
- [当前能力](#当前能力)
- [快速开始](#快速开始)
- [宿主绘制画面（第 23–24 阶段）](#宿主绘制画面第-2324-阶段)
- [多核：未解决的问题](#多核未解决的问题第-27-阶段)
- [启动是怎么实现的](#启动是怎么实现的)
- [仓库结构](#仓库结构)
- [本项目不是什么](#本项目不是什么)
- [文档](#文档)
- [许可证](#许可证)

## 项目简介

这是一个 Darwin/XNU 移植。它在普通 Linux 上的 QEMU 中启动 Apple 的 **iOS 16.5 用户空间**，绘制系统界面，并允许从宿主机操作：点击、滑动和软键盘输入，通过 QMP 控制通道以及本仓库自带的一个小型网页/VNC 桥接器实现。

重点在于这个闭环，而不是截图：

```
    浏览器 ──► webvnc.py ──► QMP ──► virtio-tablet ──► Darwin 触摸驱动
      ▲                                                        │
      │                                                        ▼
    JPEG 帧 ◄── screendump ◄── ramfb ◄── IOMobileFramebuffer ◄── CoreAnimation
                                                       ▲
                                        Mesa softpipe ─┘  （CPU 软光栅，因此帧率较低）
```

第二条更快的闭环：由宿主机负责绘制，客人机只负责记录：

```
    客人机 CoreAnimation ──GL 记录──► host_glwire ──► 真实 GL（此处为 V100）
                                           │
                                           └──► JPEG 流 ──► 浏览器，点击经 QMP 回传
```

## 里程碑

日期为 `main` 分支上的提交日期。仓库从 2026-10-31 到 2026-11-18 共约 700 个提交。

| 日期 | 里程碑 | 提交 |
|---|---|---|
| 2026-10-31 | **M0**——内核在 Linux 上从源码构建并启动 | `4101663` |
| 2026-10-31 | 用户空间启动：launchd、backboardd、SpringBoard | `9d3babb` |
| 2026-10-31 | **第一批像素**——我们自己的程序点亮了屏幕 | `d41c95a` |
| 2026-10-31 | 系统界面通过 Mesa softpipe 绘制（启动图标） | `3d054a8` |
| 2026-10-31 | **主屏幕**——SpringBoard 的界面出现在屏幕上 | `5b0cfd3` |
| 2026-10-31 | **从宿主机操作**——点击设置即可打开；三层深入到屏幕使用时间 | `f4fb4e9`、`c300753` |
| 2026-11-01 | **从浏览器操作**，包括无需键盘服务的文字输入 | `16b5712`、`e4d7579` |
| 2026-11-01 | 宿主侧 GL 回放开始：客人机记录 GL 调用，宿主机绘制屏幕 | `0a3e128` |
| 2026-11-02 | 修复 NULL 像素问题，远程路径可以到达主屏幕 | `fb4e9fa` |
| 2026-11-03 | 第 9 阶段——启动稳定性：提高锁超时后，8 次冷启动均无 panic | `3bf05e0` |
| 2026-11-07 | 通过宿主回放在屏幕上出现**第一批真实字形** | `cecbd2e` |
| 2026-11-15 | virtio-gpu 端到端可用，但实测帧率**零提升**——传输层不是瓶颈 | `b7fdef5` |
| 2026-11-17 | **宿主绘制完整画面：13.9 fps，每帧 75 ms**（交付配置，单 vCPU） | `82190eb` |
| 2026-11-17 | 双 vCPU 绑核：中位 15.8 fps；30 分钟运行 0 次 panic（后被重新审视，见下文） | `cc33526` |
| 2026-11-18 | 第 26 阶段——`fb_hz=15` 使每帧 CPU 从 69 ms 降至 51 ms，画面完全一致 | `cc17734`、`730e0f7` |
| 2026-11-18 | 第 27 阶段——多核重新列为必须解决的问题；单线程 TCG 下 SMP=2 运行 30 分钟无 panic，指向并行 vCPU | `b5bd190`、`6df855c` |
| 2026-11-22 | 第 29 阶段（实验分支）——客人机 Metal 客户端的命令被翻译为宿主 Vulkan 并显示在浏览器中：2000/2000 帧逐字节一致，合成 750×1624 客户端中位 58.3 fps；CoreAnimation 仍走 GL | [`experiment/metal-translation`](docs/stage29-metal-translation.md) |
| 2026-11-22 | 第 30 阶段（实验分支）——CoreAnimation 的 Metal 渲染器被打通：越过 `metallib` 断言与六道 Apple 实现墙，**记录到它自己的索引绘制**（每帧三个三角形，`com.apple.coreanimation.draw.*`）；尚无画面（缺 AIR→SPIR-V 与渲染命令传输） | [`docs/stage30-ca-metal.md`](docs/stage30-ca-metal.md) |
| 2026-11-22 | 第 31 阶段（实验分支）——CA 的 render encoder 被**序列化到 metalwire 协议 2**：每帧的 render pass、13 个 set、3 个索引绘制到达宿主并被解码校验（仍无像素：WP-2 着色器未做） | [`docs/stage31-ca-metal-wire.md`](docs/stage31-ca-metal-wire.md) |
| 2026-11-22 | 第 32 阶段（实验分支）——宿主在 Vulkan（lavapipe）上**执行** CA 的 3 条 pipeline 与 3 个绘制 / 13 个 set：4 个手写 Vulkan GLSL 基函数、创建 3 条 Vulkan pipeline、无 Vulkan/校验错误；画面仍待协议补齐逐绘制 offset setter 与纹理上传 | [`docs/stage32-ca-metal-execute.md`](docs/stage32-ca-metal-execute.md) |
| 2026-11-22 | 第 33 阶段（实验分支，协议 3）——**画面出现**：逐绘制 buffer offset setter 与 `replaceRegion:`（BGRA8 + R8Unorm）上线，显示纹理是真实 IOSurface（约 300 个物理段），宿主把 CA 的三个录制绘制渲染进去；浏览器 JPEG 流可见 BootUI 苹果 logo。加载 Khronos 校验层时 0 VUID；v2 合成客户端保持 60/60 | [`docs/stage33-ca-metal-picture.md`](docs/stage33-ca-metal-picture.md) |
| 2026-11-22 | 第 34 阶段（实验分支，协议 4）——**桌面合成帧在宿主执行**：管线库以 hash/入口名描述，缺少嵌入着色器的管线只损失自己的绘制、绝不损失整帧；绘制上限按实测定为 512，缺失报告按名字记录缺项。一次混合启动确认 462 帧、0 次 wire 错误；图标网格、标签与状态栏出现（画面翻转、壁纸为黑） | [`docs/stage34-wp4-desktop.md`](docs/stage34-wp4-desktop.md) |
| 2026-11-23 | 第 35 阶段（实验分支，WP-4 收尾）——桌面**正向**（在 CA→Vulkan 路径补偿 Metal 的 y-up NDC），**8 核背景/Dock 模糊族**与 **RGBA16Float（format 115）**成为宿主实现，`Tmua` material 采样其 support 纹理（Dock 白带与 Search 胶囊消失）。收尾启动确认 706 帧、`pipelines 0, skipped draws 0`；记录帧率基线（旧 binary 代）：lavapipe 1.82 fps、V100 1.96 fps。仍开放：黑壁纸、白色部件占位（客人机/镜像侧）；双缓冲交替 D8 已在第 39 阶段修复 | [`docs/stage35-wp4-close.md`](docs/stage35-wp4-close.md) |
| 2026-11-23 | 第 39 阶段（实验分支）——**两个显示 buffer 等价（D8 修复）**：宿主把上一帧画面拷入“保留内容”的兄弟 surface，且只在源至少同样完整时才拷贝，Dock/Search 层不会再被覆盖；Metal 的 **RG8Unorm（format 30）**成为真实 image（前一轮 boot 暴露的第二个 wire error）。最终 boot 两 buffer 都持完整桌面——black 帧 0、`pipelines 0, skipped draws 0`、wire error 0——记录帧率 **2.83 fps**（lavapipe）/ **2.55 fps**（V100）。仍开放：黑壁纸、白色部件占位、5 个 blur 核仅 fixture 验证 | [`docs/stage39-d8-double-buffer.md`](docs/stage39-d8-double-buffer.md) |
| 2026-11-23 | 第 43 阶段（实验分支）——**4.1 泄漏/OOM soak 收口**：宿主回收“线上从不 destroy”的每帧 256 KiB buffer（live 2 个 / 0.5 MB、`reclaimed=0`），插件把 CA 一直持有的 buffer 按帧可用性池化（`HakiosMetalBuffer alloc=5 reused=2398`）。2400 s lavapipe soak：host RSS **185.5 → 195.6 MB（1.05）**、桌面 **25.3 min**、2991 帧、jetsam/memorystatus/OOM 全 0、`available` 1135 MB；口径为“事件 0 + available ≥500 MB”，不用 free（16 MB 是文件缓存）。工具：`MW_LEAK_CHECK`、`MW_BUFFER_LOG`、`--leak`；插件 md5 `bcaf99c6…`、host `ad778a06…`。边界：继承的 `-dealloc` 链永不调用（`base_dealloc=0`；safe release 仅 marker ON） | [`docs/stage43-leak-and-oom.md`](docs/stage43-leak-and-oom.md) |
| 2026-11-23 | 第 44 阶段（实验分支）——**4.2 多核 recon**：T2（`tb-size=4096`）与交付值对照组**都 panic**（14.6 min 仅 2 帧 / 2035 帧），虽已绑核且两个 vCPU 线程 60–87% CPU 在跑；**H1（宿主饿死）与 H-C（宿主抢占）被否决**，H-A 仍最强、H-B（guest waitq/idle 路径）权重上升。多核**不可交付**——单 vCPU 继续基线；T5（内核构建 `LC_MAIN`→`LC_UNIXTHREAD`、waitq/idle 环形探针、`isolcpus`/停 win2022 需批准）是唯一剩余路径 | [第 44 阶段](docs/stage44-multicore.md) |
| 2026-11-23 | 第 45 阶段（实验分支）——**T5 探针内核定位到锁的 owner**：单 vCPU 自检通过（2089 帧、0 panic、桌面一致），一次 SMP=2 抓到 owner 最近 8 条 stage——owner 在 `_waitq_select_queue_flush+0x134`（`thread_go` 调用点）拿住 `_pset0` 锁，此前经 `_ipc_port_clear_receiver → _waitq_wakeup64_all_locked → do_waitq_select_n_locked`，之后**一直不返回**。**H-B（guest waitq/调度在 MTTCG 下丢唤醒/不返回）因定位到 owner 路径而成立；H-A 是使能条件。** 多核仍不可交付——单 vCPU 继续基线；探针内核 `eaf71509…` 非交付内核（`f62c3d4b…` 已恢复） | [第 45 阶段](docs/stage45-t5-probe.md) |
| 2026-11-23 | 第 46 阶段（实验分支）——**waitq 深挖（探针 v2）**：550 行探针加入 `thread_go` / `do_waitq_select_n_locked` / `waitq_select_queue_flush` 与 pset/waitq/thread_call 票锁标记；单 vCPU 自检保持干净（1888 帧、0 panic、桌面一致）。SMP=2 两发：run A 是 **mutex interlock 超时**，run B 的 **owner stages 全部落在 pset 票锁的拿/放循环**——`_sched_timeshare_maintenance_continue+0xf4` 反复拿放 `_pset0+0x100`（last taker `_thread_quantum_expire+0x26c`），`pset#1,800,157` / `stage ring 26,712,436`，即 stage27 T6 的站点。**H-B 在函数/锁层面成立；更准确的说法是循环不退出/丢唤醒，而非简单锁序反转。** 探针内核 `1bce762b…` 非交付内核（`f62c3d4b…` 已恢复） | [第 46 阶段](docs/stage46-waitq-deep-probe.md) |

## 当前能力

| | 状态 | 证据 |
|---|---|---|
| 启动到主屏幕 | **是**，约 8 分钟，约 40% 的尝试成功（其余在移植内部卡住） | [主屏幕](docs/demo/home-screen.png) |
| 点击图标，启动应用 | **是** | [设置](docs/demo/settings-app.png) |
| 在应用内导航 | **是** | [屏幕使用时间](docs/demo/screen-time-from-host.png) |
| 滑动（主屏手势、下拉） | **是** | [控制中心](docs/demo/control-center.png) |
| 从宿主机操作 | **是**——QMP，`tools/qemu/tap.py` | [附录 28](docs/research/linux-kernel-build.md) |
| 从浏览器操作 | **是**——`tools/qemu/webvnc.py` | [浏览器点击聚焦](docs/demo/webvnc-focus-page.png) |
| 输入文字 | **是**，通过软键盘点击实现 | [输入 `abc`](docs/demo/webvnc-typed-abc.png) |
| 与宿主机的网络（局域网） | **是**——`en0` 为 10.0.2.15，双向 HTTP 可达 | [附录 30](docs/research/linux-kernel-build.md) |
| 域名、DNS | 否——configd 插件在此从未注册 | 附录 30 |
| GPU 加速 | 否——本机只有 CPU 软光栅 | — |
| 动画流畅度 | CPU 软光栅约 4–7 fps；**宿主绘制时 13.9 fps**（单 vCPU，交付配置）；双 vCPU 绑核短跑约 15.9 fps，但第 44 阶段的 SMP=2 运行仍 panic、第 45/46 阶段的探针把 owner 定位到 waitq 唤醒路径与分时维护的 pset 循环（宿主饿死已排除）——见[开放问题](#多核未解决的问题第-27-阶段) | [第 25 阶段](docs/stage25-multicore.md)、[第 27 阶段](docs/stage27-multicore.md)、[第 44 阶段](docs/stage44-multicore.md)、[第 45 阶段](docs/stage45-t5-probe.md)、[第 46 阶段](docs/stage46-waitq-deep-probe.md) |
| 宿主绘制完整画面 | **是**——客人机的 CoreAnimation 调用在宿主 GL 上回放，所见即结果 | [第 23 阶段结论](docs/stage23-verdict.md) |
| Metal 调用翻译到宿主 Vulkan（实验，独立分支） | **部分**——客人机 Metal 客户端的 blit/present 被记录、在宿主 Vulkan GPU 上执行并在浏览器中可见；另外，CoreAnimation 的 Metal 渲染器已被打通、其 render encoder 已序列化并在 Vulkan 上执行；自第 35 阶段起窗口服务器的**桌面合成帧**（每帧 31–88 个绘制）**正向**，背景/Dock 模糊族（8 个核）、RGBA16Float（format 115）、RG8Unorm（format 30）与 `Tmua` material 语义均为宿主实现；自第 39 阶段起**两个显示 buffer 等价**（D8 由内容感知的逐帧拷贝修复）——图标、标签、状态栏、Dock 与 Search 在 lavapipe 与 V100 上**每一帧**都显示系统自己的图层，记录帧率 2.83 / 2.55 fps（同一 binary 代；1.82 / 1.96 属于更早的一代）。桌面 **4.1 soak** 宿主 RSS 恒定（185.5 → 195.6 MB，末/初 1.05）、live buffer 2 个 / 0.5 MB、25.3 min 无 jetsam/OOM（插件 `bcaf99c6…`、host `ad778a06…`；`MW_LEAK_CHECK`、`MW_BUFFER_LOG`、`--leak` 测试）。仍开放：壁纸为黑、两个部件是白色占位（客人机/镜像侧），5 个 blur 核仅 fixture 覆盖（CC/Spotlight 打不开），且继承的 `-dealloc` 链永不调用（`base_dealloc=0`；safe release 仅 marker ON）；协议 4 保证未知管线只损失绘制、不损失整帧 | [第 29 阶段](docs/stage29-metal-translation.md)、[第 30 阶段](docs/stage30-ca-metal.md)、[第 33 阶段](docs/stage33-ca-metal-picture.md)、[第 34 阶段](docs/stage34-wp4-desktop.md)、[第 35 阶段](docs/stage35-wp4-close.md)、[第 39 阶段](docs/stage39-d8-double-buffer.md)、[第 43 阶段](docs/stage43-leak-and-oom.md) |
| 在浏览器中观看该画面 | **是**——`tools/glwire/webview.py`，JPEG 流，点击经 QMP 回传 | `tools/glwire/webview.html` |

`docs/state-of-the-port.md` 是同一张表的一页式交接视图。

## 快速开始

本仓库中不包含任何 Apple 的软件，你需要自行获取 IPSW。

```sh
git clone --recursive https://github.com/lshan99q/iosvm.git
cd iosvm

# 1. 自备 IPSW——本仓库中不包含任何 Apple 文件
tools/unpack-ipsw.sh ~/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw

# 2. 构建内核、镜像和加载器（各部分说明见 docs/）
make

# 3. 启动，并等待屏幕确实绘制出内容（会自动重试：并非每次启动都成功）
tools/qemu/wait-for-ui.sh /tmp/home.png

# 4. 操作——通过浏览器……
tools/qemu/qmpmux.py --qmp 4445 --port 4465
tools/qemu/webvnc.py --port 5980 --qmp 4465      # 然后访问 http://127.0.0.1:5980/
#    ……或在命令行中
tools/qemu/tap.py 683 1689                      # 点击设置图标

# 5. 或者由宿主机绘制画面：客人机只做回放启动，然后观看该画面
#    （客人机自己的面板会保持半空——见下文"宿主绘制画面"）
GLWIRE_NO_PANEL=1 GLWIRE_STATIC_PATCH=1 \
  GLWIRE_STATIC_PATCH_FILE=/tmp/patch-full.bgra GLWIRE_SHM=/dev/shm/glwire-frame \
  tools/glwire/host_glwire &                   # 回放进程，逐帧发布
python3 tools/glwire/webview.py --port 6083 --crop 1624   # 然后访问 http://127.0.0.1:6083/
```

`tools/qemu/` 中的工具是本项目实际使用的工具：

| 工具 | 作用 |
|---|---|
| `wait-for-ui.sh` | 启动并重试，直到屏幕绘制出内容；`KEEP=1` 保持虚拟机运行 |
| `tap.py` | 在面板坐标处点击或滑动，经 QMP 发送 |
| `qmpmux.py` | QEMU 只允许一个 QMP 客户端连接；它允许多个 |
| `webvnc.py` | 浏览器查看器：输出画面，输入点击和按键 |
| `vncbridge.py` | 相同功能，面向 VNC 查看器而非浏览器 |
| `demo.sh` | 一条命令完成整个流程，每一步都保存截图 |

## 宿主绘制画面（第 23–24 阶段）

CPU 软光栅是性能上限，因此下一步是不再让它绘制：客人机记录 CoreAnimation 的 GL 调用，通过 socket 发送，由宿主机在真实的 GL 设备上回放。应该观察的是宿主机合成出的画面。

| | 实测 |
|---|---|
| 宿主回放，完整画面 | 单 vCPU **13.9 fps**（交付配置；画面与基线逐字节一致）。双 vCPU 绑核可达约 15.9 fps（第 25 阶段），但在第 27 阶段出现调度器锁 panic——见下文 |
| 宿主每帧自身开销 | `gkbf=1` 之前约 41 ms；开启后 **约 1.4 ms**（每帧记录从 90 KB 降至 4–19 KB）——从来不是瓶颈 |
| 客人机的帧预算 | 原为 117.1 ms = CA 71.0 + 编码/拷贝 46.8 + 等待 1 µs；所谓"CA 71 ms"实际是显示驱动的**全面板放大 blit**（750×1624 → 1125×2436，274 万像素）：每次交换 62 ms，现已跳过（`fb_noblit=1`） |

2026-11-17 实测：使用 `gkbf=1 glwire_ack_only=1 fb_noblit=1` 时，客人机每帧耗时 75 ms，画面与 9.09 fps 基线逐字节一致。挂钟时间受宿主负载影响约 ±30%，因此应比较成对运行，而不是单次数据。详见[第 24 阶段 blit](docs/stage24-blitfast.md) 和 [gkbf 负面结果](docs/stage24-gkbf.md)。

有两点值得记录，因为两者看起来应该有效，实际上并无效果：

- **客人机并不等待宿主。** `wait = 1 µs`，因此异步应答和双缓冲毫无收益。
- **记录本身很小。** 每帧 12.65 KB，仅占 4.87 MB 画面的 0.26%；去重最多节省 0.12 ms。那 46.8 ms 是 4.9 MB 的往返传输，而非编码。

`virtio-gpu`（第 23 阶段）已端到端实现——控制队列、2D 资源、scanout、传输、刷新——但实测帧率**零提升**（1.80 → 1.85 fps，在误差范围内）：传输层从来不是瓶颈。它作为未来 3D 路径的基础保留，而不是加速手段。

## 多核：未解决的问题（第 27 阶段）

多核**并未被排除**——它是目前可测得的最大加速空间，正作为必须解决的问题推进。绑核有帮助，但尚未使配置变得可靠。

| 配置（同一宿主负载） | 中位帧率 | 结果 |
|---|---|---|
| 1 vCPU，绑核 | 9.9 | 基线 |
| 2 vCPU，绑定到两个宿主核心 | 15.6–15.9 | 提升 +58–60%；第 25 阶段 30 分钟 0 次 panic，第 27 阶段重现了调度器锁 panic |
| 4 vCPU，绑核 | 14.4 | 无收益：每帧总 CPU 几乎翻倍（MTTCG 开销与锁竞争） |
| 2 vCPU，不绑核 | — | 5 帧内 Ticket spinlock panic |
| 2 vCPU，`thread=single`（T1），30 分钟 | 无 panic | 死锁需要并行 vCPU（假设 H-A） |
| 2 vCPU，MTTCG（`thread=multi`）绑核，25 分钟 | — | 第 44 阶段：`tb-size=4096` 在 14.6 min / 2 帧后 panic，交付值 `tb-size=1536` 在 2035 帧后 panic；两个 vCPU 线程都在跑，宿主饿死/抢占被排除 |
| 2 vCPU，MTTCG，T5 探针内核（owner stages） | — | 第 45 阶段：17:07:21 panic（boot 后 187 s、2 帧）；owner 最近 stage：`_ipc_port_clear_receiver`（50/51/52）→ `_waitq_wakeup64_all_locked` → `_waitq_select_queue_flush+0x134`，持 `_thread_call_groups+0x280` —— H-B 成立 |
| 2 vCPU，MTTCG，探针 v2（锁/循环标记） | — | 第 46 阶段：run A mutex interlock 超时；run B 票锁超时，owner 在 `_sched_timeshare_maintenance_continue+0xf4` 反复拿放 `_pset0+0x100` —— stage27 T6 站点，H-B 在函数层面成立 |

第 27 阶段（2026-11-18）记录了 5 次双 vCPU 绑核运行：2 次运行 30 分钟无 panic，3 次在 5–10 分钟内崩溃，全部发生在调度器 processor-set 锁（`_pset0`）上。将票据锁超时放宽 4 倍无效，这说明更可能是持锁者停止释放锁，而非宿主短暂抢占。第一项测试（T1）使用 `thread=single`，运行 30 分钟无 panic，支持假设 H-A：故障需要 vCPU 并行运行。测试矩阵的其余部分（`tb-size`、CPU 型号、panic 时转储所有 CPU、宿主机隔离）见[第 27 阶段文档](docs/stage27-multicore.md)。在这些测试通过之前，交付配置仍为单 vCPU。

第 44 阶段（2026-11-23）跑了最便宜的未做项 T2（`tb-size=4096`）与同日交付值对照：**两者都 panic**，同一个 ticket 锁；T2 在桌面出现前（2 帧）崩溃，对照组在 2035 帧后崩溃。两个 vCPU 线程在窄核绑定下仍有 60–87% CPU 在执行，因此宿主饿死（H1）与抢占（H-C）被否决；锁的 owner 落在 guest 的 `_waitq_pull_thread_locked` / `_idle_thread` 路径，H-A 仍最强、H-B 权重上升。多核不可交付，单 vCPU 继续基线；T5（可启动的探针内核 + 真正的隔离决定）是唯一剩余路径——见 [`docs/stage44-multicore.md`](docs/stage44-multicore.md)。

第 45 阶段（2026-11-23）做出 T5 探针内核（构建管线改用 cctools 链接器：`LC_MAIN`→`LC_UNIXTHREAD` 是链接器选择，`hakios-xnu-kernel/tools/kernel-rebuild.sh` 一条命令完成构建与检查），单 vCPU 自检通过（2089 帧、0 panic、画面与交付内核一致），一次 SMP=2 抓到 owner：`_ipc_port_clear_receiver → _waitq_wakeup64_all_locked → do_waitq_select_n_locked → _waitq_select_queue_flush+0x134`（`thread_go` 调用点）持 pset 锁不返回。**H-B 因定位到 owner 路径而成立，H-A 仍是使能条件。** 多核仍不可交付；T5 下一步是 `thread_go` / `do_waitq_select_n_locked` 内部循环与 pset↔waitq 锁序——见 [`docs/stage45-t5-probe.md`](docs/stage45-t5-probe.md)。

第 46 阶段（2026-11-23）把探针加深到 v2：给 `thread_go`、`do_waitq_select_n_locked`、`waitq_select_queue_flush` 以及 pset/waitq/thread_call 票锁的拿/放都加了标记。单 vCPU 自检保持干净（1888 帧、0 panic、画面一致）。SMP=2 两发：run A 是 **mutex interlock 超时**（另一锁类），run B 是票锁超时，owner 最近 8 条 stage **全部在 pset 票锁的拿/放循环**——`_sched_timeshare_maintenance_continue+0xf4`（`_pset0+0x100`；last taker `_thread_quantum_expire+0x26c`），`pset#1,800,157`、stage ring 26.7M。**H-B 在函数/锁层面成立；owner 是在分时维护路径里循环/持锁，而不是拿着第二把锁等第一把，因此“简单锁序反转”不是最佳描述。** 下一步 v3：在维护循环头/尾记录条件变量与迭代计数、加 `thread_quantum_expire` 入口标记、给 mutex 超时加轻量现场；或只把 SMP>1 作为实验特性 + 启动守卫交付——见 [`docs/stage46-waitq-deep-probe.md`](docs/stage46-waitq-deep-probe.md)。

## Metal 转译（实验，第 29 阶段）

独立分支 [`experiment/metal-translation`](docs/stage29-metal-translation.md) 探索
[ADR-002](docs/adr/ADR-002-api-translation.md) 的另一半：不再回放 CoreAnimation 的 GL 记录，而是替换客人机的
Metal 设备、在 API 层翻译。这是实验路径，不是交付路径——上面的 Replay 路径原样保留，仍是基线。

它验证了：移植自带的 `IOAcceleratorES` + `MetalPluginName` 发现链能加载我们写的 Metal 设备 bundle；客人机里的
Metal 客户端可以创建设备、队列、buffer、texture 并完成 blit+present；命令缓冲在 `commit` 时被记录、在宿主
Vulkan 设备（第二块 V100，1 号卡）上执行；画面被读回、CRC 校验并写入与 `tools/glwire/webview.py` 相同的共享
内存，因此转译后的画面能在浏览器中看到：

![转译画面，由 webview.py 提供](docs/demo/metal-translation-webview.png)

| | 实测（2026-11-21/22，单 vCPU，1 号卡，合成 750×1624 客户端） |
|---|---|
| 帧数 / 正确性 | 5 × 400 帧，2000/2000 `crc_ok=1`，源数据与读回数据完整 `memcmp` 一致，0 次 panic |
| 帧率 | 中位 **58.3**（57.4–60.1） |
| 客人机每帧 | 生成画面 2.6 ms、Metal 调用 0.19 ms、commit 7.1 ms、wait 13.7 ms |
| 宿主每帧 | 4.2 ms（GPU 拷贝 0.94 ms、读回+抽样校验 0.31 ms、发布 2.9 ms） |
| 传输 | socket 上 0 图片字节：宿主经 `hakios_vtop` 直接从客人机内存读取像素（与 Replay 路径同一机制） |
| 传输前后对比 | 480 万像素内联在 socket 上时 **1.94 fps**；改为按引用传输后中位 **58.3 fps**——代价在传输，不在翻译 |

![各实现的帧率：代价在传输与校验，不在翻译](docs/charts/metal-translation-history.png)

它**没有**验证：CoreAnimation 的 Metal 帧。本移植的窗口服务器不走 Metal（缺渲染库，且背后缺 AIR→SPIR-V
编译器），所以被翻译的客户端是测试程序，其 58.3 fps 不能与交付的 13.9 fps 当作同一负载比较。诚实的对比、
死胡同和自我修正都在[第 29 阶段](docs/stage29-metal-translation.md)；决策在
[ADR-013](docs/adr/ADR-013-metal-translation.md)；调研在
[可行性报告](docs/research/metal-translation-feasibility.md)。

第 30 阶段接着问：当窗口服务器**真的**被展示 Metal 设备时，CoreAnimation 自己能走多远。答案在
[第 30 阶段](docs/stage30-ca-metal.md)：越过 `metallib_url` 断言、越过桩子类会继承的
`_MTLDevice`/`_MTLLibrary`/`_MTLFunction`/`_MTLPipelineLibrary` 的 Apple 实现，直到记录到它自己的
第一批索引绘制——每帧三个三角形，管线名为 `com.apple.coreanimation.draw.*`，来自缺失的 metallib 本该
提供的 `fixed_*` 基础函数。这份记录现在已被序列化（第 31 阶段）：render pass、13 个 set、3 个索引绘制
每帧经 metalwire 协议 2 到达宿主并被解码。但**仍无画面**：没有编译任何着色器，显示器的 IOSurface 没有
被包装，宿主只确认帧而不绘制。每道墙背后的静态分析在
[`docs/research/ca-metallib-url.md`](docs/research/ca-metallib-url.md)。

运行方式（工具在实验分支；不改变 main 的构建）：

```sh
make metalwire-host                              # 宿主 Vulkan 翻译器（无需客人机）
make metalwire-test                              # 在 lavapipe 上的 CI 测试
HAKIOS=<hakios 检出目录> make metalwire-guest      # 客人机 bundle + 客户端（需要 iPhoneOS SDK）
HAKIOS=<hakios 检出目录> make metalwire-worktree   # 私有运行树（一次 sudo；约 90 MB）
tools/metalwire/prepare-guest.sh translate         # 生成带 Metal 客户端的 root 镜像
tools/metalwire/run-guest.sh translate 300         # 启动（实例 3，端口 4474/4475）
```

它刻意使用 1 号卡和 9111/9210/4474/4475 端口，以便与同一宿主上的其他实验并行；交付的 Replay 路径（0 号卡、
端口 9110）从不被触碰。

## 启动是怎么实现的

有三件事必须同时成立，研究笔记中记录了每一项的测量依据：

1. **显示必须有 show 服务**——在手机上由 GPU 驱动启动它；在 QEMU 中没有任何东西启动它，因此用户空间的绘制无处落地（`patches/0001`）；
2. **`fb_beat` 必须为 0**——当渲染速度如此之慢时，驱动按节拍的刷新会丢掉第一帧之后的所有帧，这正是"卡在启动图标"的症状；
3. **必须有东西发送触摸事件**——我们自己的 launchd 任务在这里无法可靠执行，因此合成触摸来自 `/etc/rc`，它在 `init` 将 1 号进程交给 launchd 之前运行。

完整版本——包括死胡同与三次自我修正——见 [`docs/research/linux-kernel-build.md`](docs/research/linux-kernel-build.md)。

## 仓库结构

| 路径 | 内容 |
|---|---|
| `patches/` | 对移植所做的改动：每个发现对应一个补丁，并附原因 |
| `tools/qemu/` | 上文所述的启动与操作工具 |
| `tools/glwire/` | 客人机 GL 线协议：`host_glwire.c`（回放）、`webview.py`（在浏览器中查看该画面） |
| `host/metalwire/`、`guest/metal-plugin/` | Metal→Vulkan 实验：宿主 Vulkan 翻译器与客人机 Metal 设备 bundle（第 29 阶段，分支 `experiment/metal-translation`） |
| `tools/metalwire/` | 实验自用的运行树、root 镜像准备与测量工具 |
| `tools/apfs/` | 无需 Apple 工具即可读写镜像 |
| `guest/` | 运行在虚拟机内的程序（绘制程序、`netcfg`） |
| `proto/`、`host/vkbridge/` | GPU 传输层及其宿主侧 Vulkan 后端（子模块） |
| `docs/` | ADR、研究笔记、移植状态、演示截图 |

## 本项目不是什么

- **不是 Apple 的软件。** 这里没有内核、系统镜像、dyld 缓存、固件或字体。你需要自行获取 IPSW，见 `PROVENANCE.md`。三条底线：仓库中不得包含 Apple 二进制文件；不得冒充真实设备访问 Apple 服务器；不得包含凭据。
- **不是越狱工具。** 移植的内核在设计上对代码签名宽松（AMFI 是一个接受任何可解析签名的替身）；这里没有任何东西攻击真实设备。
- **并不快。** 渲染器是 CPU 软光栅。在宿主绘制画面的情况下，11 fps 的目标已达成（2026-11-17：13.9 fps / 每帧 75 ms）；旧的 9.09 fps 数据保留在第 24 阶段文档中，作为比较的基线。24 fps 是目标而非结论：它需要真正的 3D 路径或同架构虚拟化。

## 文档

| | |
|---|---|
| [`docs/state-of-the-port.md`](docs/state-of-the-port.md) | 一页纸：哪些能用，哪些不能 |
| [`docs/stage25-multicore.md`](docs/stage25-multicore.md) | 最初的双 vCPU 绑核测量 |
| [`docs/stage27-multicore.md`](docs/stage27-multicore.md) | 多核问题：假设、测试矩阵、验收标准 |
| [`docs/research/linux-kernel-build.md`](docs/research/linux-kernel-build.md) | 长篇笔记：每一次测量与所犯的错误 |
| [`docs/stage29-metal-translation.md`](docs/stage29-metal-translation.md) | Metal→Vulkan 实验：测了什么，以及它不能证明什么 |
| [`docs/research/metal-translation-feasibility.md`](docs/research/metal-translation-feasibility.md) | M1 调研：Metal 可在何处拦截、CA 为何不在 Metal 上、有哪些翻译器 |
| [`docs/adr/ADR-013-metal-translation.md`](docs/adr/ADR-013-metal-translation.md) | 决策：设备插件、commit 级翻译、按引用传输、宿主 Vulkan |
| [`docs/architecture.md`](docs/architecture.md) | 各部件如何配合 |
| [`docs/phases.md`](docs/phases.md) | 里程碑，每个都有一条验收命令 |
| [`docs/adr/`](docs/adr) | 决策及其依据 |
| [`docs/zh/`](docs/zh) | 中文译文 |

## 许可证

本项目代码采用 MIT（`LICENSE`）；Apple 开源的 XNU 保留其 APSL（`LICENSE.APSL`），GPL-2.0 部分（`LICENSE.GPL2`）仍为 GPL-2.0。详见 `NOTICE`。

<p align="right"><a href="#iosvm">↑ 返回顶部 / back to top</a></p>

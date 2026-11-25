# stage 57: the PosterBoard on the wire — injected, alive, and never asks for a device

> Status: **measured; negative.** t83 built variant #6 — the first image that puts **PosterBoard**
> itself into both `/etc/inject.conf` and `/lib/HakiosMetal.clients` (on top of backboardd and the
> six poster/widget appexes). t84 booted it for 3,600 s with `launchd_disable_all_timeouts=1`:
> **0 panic, 0 `EXTENSION_CHECK_IN_TIMEOUT`, 0 `hanging on launch`, 0 wire error, 0 missing
> pipeline / 0 skipped draw**, 6,706 frames (6,547 desktop). PosterBoard is injected
> (`ca-inject: loaded as pid 101 (PosterBoard)`), execs from
> `/Applications/PosterBoard.app/PosterBoard` 15.2 min into the boot, and stays **running to the
> end** — the boot-arg holds. But it **never asks for a Metal device at all**: serial has
> `is shown the device` only for backboardd (×2) and `is not shown the device` only for
> mediaserverd / SpringBoard / chronod — **PosterBoard prints neither line**, so it never reaches
> even `mw_process_allowed()`. The host keeps **one client** (backboardd, pid 20), **no second
> `hello`**, **0 textures ≥1000×2000** (`1125x2436` appears 0 times), and the desktop is unchanged
> (main-area black **58.9 %**, widgets 100 % white, wallpaper strip **one** colour, pure black;
> Dock 66.3 / Search 13.6, no regression). **The wire-client list is not the bottleneck and the
> watchdog is no longer the blocker: D1 is the PosterBoard → backboardd scene/poster handshake.**
> D2 (white widgets) is independent of this variant. Delivery root `a4a6e2ce…`, system
> `49ae3cf4…` and kernel `f62c3d4b` were restored after the run; variant #6 stays only in
> `mt-work/exp-d1d2f/`. Branch `experiment/ca-metal-device`, 2026-11-24. This continues
> [`docs/stage56-launchd-timeouts.md`](stage56-launchd-timeouts.md).

## 1. The question

Stage 56 removed the 5 s launchd kill with the boot-arg `launchd_disable_all_timeouts=1` and
exposed the next blocker: the appexes stay alive but never call `MTLCreateSystemDefaultDevice`.
The standing hypothesis after t79/t80 was that **PosterBoard** — the wallpaper renderer — is the
process that owns the wallpaper surface, and that it was absent from both wire levers
(`inject.conf` and `HakiosMetal.clients`). Variant #6 tests exactly that hypothesis:

1. does PosterBoard load `libcainject` / the Metal plug-in when its name is on both levers?
2. does it then ask the wire for a Metal device, appear as a second host client, and upload a
   1125×2436 wallpaper texture?
3. do the wallpaper (D1) and widget (D2) pixels change?

## 2. Inputs and hashes

| item | value |
|---|---|
| root.apfs booted (variant #6) | `ea0ac44bbb32bdf79c172fd28adecef8`, 6,234,112 B, sha256 `2a83c653b33a74f5d5fb10fc1b0cc268e4908f19cb1477568acf776426941203` |
| delivery root backup | `/tmp/t84-delivery-root-a4a6e2ce.apfs` = `a4a6e2ce50e6d0324e26a49ea20fbd84`, 6,238,208 B |
| system.apfs (delivery, untouched) | `49ae3cf4fd53334b475a921eb9915008`, 2,476,752,896 B |
| kernel (delivery, untouched) | `f62c3d4b852d467f22c4703b8fc620c9`, 15,774,768 B |
| metalwire_host | `b0bf8f767d816299a864f23785dee03b` (commit `2d3f325`, stage54 multi-client) |
| host_glwire | `5b28249a37cb11207f3ab160bcf4ac23` |
| HakiosMetal (guest plug-in) | `bcaf99c6392d24d1d96b76586df9e246` (t34) |
| libcainject | `aa123ccdd6ee4661fa335ac91311bbb1` |
| lavapipe | `VK_ICD_FILENAMES=…/lvp_icd.json --gpu 0`, "llvmpipe (LLVM 20.1.2, 256 bits)" |
| vars.fd | `/tmp/t84-vars0xff.fd`, 64 MiB of `0xFF` |
| boot tag | `wp4-d1d2-j`, 2026-11-24 17:55:48 → 18:55:48 (+08:00, full 3,600 s) |
| qemu | pid 527988, SMP=1 / cpus=1, core 26,27,28, tb 1536 MB, ram 7 GB |

Variant #6 deltas (t83, built on the delivery root; `apfsck` clean, 502 entries read back):
`/etc/inject.conf` 304 → **466 B** (backboardd + six appexes + **PosterBoard**);
`/lib/HakiosMetal.clients` 35 B / 3 lines → **166 B / 10 lines** (adds PosterBoard); the duplicate
`LaunchAngels/com.apple.PosterBoard.plist` label stays removed. No `Info.plist` change — the
stage-53 regression (six appex `XPCService` keys in system.apfs) is **not** in this image.

Boot-args (serial, verbatim):
`debug=0x8 serial=3 rd=md0 loader_return_after=3600 epoch=1795514148 virtio_input=1
virtio_sound=1 virtio_net=1 cpus=1 fb_beat=0 fbscale=2 metal=1 launchd_disable_all_timeouts=1
applog=1 applog_name=CollectionsPoster,GradientPosterExtension,WeatherPoster,WeatherWidget,
CalendarWidgetExtension,AppStoreWidgetsExtension,PosterBoard`

## 3. Observation a — PosterBoard is injected and stays alive

- `hakios: exec pid=101 path=/Applications/PosterBoard.app/PosterBoard`
- `ca-inject: loaded as pid 101 (PosterBoard)` — the plug-in loads into PosterBoard.
- launchd: `service spawned with pid 101` at guest 10:11:02 (boot at 09:55:52, i.e. **15.2 min**
  in) → `service state: running`, `job state = running`, `SOURCE_ATTACH`; **no** exited / killed /
  crash / throttle record until the end of the run.
- Memory snapshot at spawn: `(pid/101 [PosterBoard]) memory: free 104, files 1221, programs 176,
  wired 2688 (2 surfaces 9, ramfs 146)`.

So the launchd half of the hypothesis is confirmed: with the boot-arg, PosterBoard is no longer
killed, and with the two levers it receives the plug-in.

## 4. Observation b — it never creates a Metal device

This is the decisive negative result. The plug-in's allow gate prints one of two lines:

- `metalwire: <program> is shown the device: HakiosMetal.clients beside the bundle names it`
- `metalwire: <program> is not shown the device (metal=1: …)`

t84 serial contains:

| line | programs |
|---|---|
| `is shown the device` | backboardd ×2 |
| `is not shown the device` | mediaserverd, SpringBoard, chronod |
| **neither line** | **PosterBoard** |

`mw_process_allowed()` is the first thing the plug-in calls when a client tries to open the wire;
if PosterBoard had called it, one of the two lines would exist. **PosterBoard never attempts to
create an `MTLDevice`** — adding its name to `/lib/HakiosMetal.clients` changes nothing because the
clients file is only consulted inside that gate. The same holds for the six appexes (exec 7 kinds,
`ca-inject` 6 lines + 1 duplicate, `hello` 0).

## 5. Observation c — the host still serves one client

- `metalwire: client connected (1 client(s))` → `hello: version 4 pid 20 program "backboardd"` →
  `client disconnected after 6706 frame(s)`.
- **No PosterBoard hello, no second connection** until the end of the run.
- Wire target lines: only backboardd (`10.0.2.2:9111`, connect timeout 2000 ms, ack timeout
  15000 ms); `wire: error` 0.

The multi-client host from stage 54/55 was therefore never exercised by a second appex in this
run: it is ready, but the guest never presents a second client.

## 6. Observation d — no wallpaper texture

Host texture sizes across the whole run (12 distinct):

| size | count |
|---|---|
| 750×1624 | 13,097 (frame presents) |
| 138×138 | 300 |
| 180×180 | 19 |
| 752×1626 | 2 |
| 381×818 | 2 |
| 100×209 / 129×158 / 138×100 / … | small icon/UI textures |

- **textures ≥1000×2000: 0**
- **`1125x2436`: 0**

PosterBoard and backboardd never upload any wallpaper-sized image to the wire. If the wallpaper
were rendered as a Metal surface, this is where it would appear.

## 7. Observation e — pixels, stability, pipelines

Pixel metrics (stage2 and stage3 agree; frame `wp4-d1d2-j`):

| metric | t84 (#6) | t80 (#4) | delivery baseline t35 |
|---|---|---|---|
| main-area black (80:1416) | **0.589** | 0.655 | 0.649 |
| widget bbox1 / bbox2 pure white | **1.000 / 1.000** | 1.000 / 1.000 | 1.000 / 1.000 |
| Dock luminance | 66.3 | 66.3 | 66.3 |
| Search luminance | 13.6 | 13.6 | 13.6 |
| wallpaper probes top/left/right/above-search black | 0.927 / 0.942 / 0.955 / 0.986 | — | — |
| wallpaper strip (rows 100–120) distinct colours | **1** (pure black) | 1 | 1 |

The main-area black fraction is slightly lower than t80 because the icon grid is denser in this
frame, but the wallpaper area is still **a single black colour** (D1 unchanged) and both widget
boxes are **100 % white** (D2 unchanged). Dock/Search/status bar do not regress.

Stability and pipeline accounting:

| item | t84 | t80 | t77-h2 |
|---|---|---|---|
| panic | **0** | 0 | 0 |
| `EXTENSION_CHECK_IN_TIMEOUT` | **0** | 0 | 20 |
| `hanging on launch` | **0** | 0 | 60 |
| appex exec kinds | 7 (PosterBoard + 6) | 39 lines (poster 3) | 2 (poster) |
| `ca-inject` loaded lines | 6 (+1 duplicate) | 7 | 112/113 |
| appex `is shown` | **0** | 0 | 0 |
| host clients / hello | **1 / backboardd** | 1 / backboardd | 1 / backboardd |
| frames | **6,706 (desktop 6,547)** | 6,039 | — |
| missing pipeline / skipped draw | **0 / 0** | 1 / 1 | — |
| wire error | **0** | 0 | — |

`/tmp/t84-missing.txt` (144 B):
`# frames 6706, pipelines 0, skipped draws 0` / `library 6 kind url bytes 64 hash
0x0472d062082ed7b7 functions 0`. The single missing blur pipeline seen in t80
(`PBGRA_Tsb3Xf_Isrc` + `single_pass_blur_3_lpf`) **did not reproduce**, so it is timing-related,
not a fixed pipeline.

## 8. Verdict

1. **Variant #6 is negative.** PosterBoard is injected, execs and stays running, but it **never
   creates a Metal device** (prints neither `is shown` nor `is not shown`), never sends a `hello`,
   never becomes a second client, and uploads no wallpaper texture. The `HakiosMetal.clients`
   list is **not** the gate for D1.
2. **D1's remaining gap is the PosterBoard → backboardd scene/poster handshake.** In this iOS
   build PosterBoard does not render the wallpaper with its own Metal context; the wallpaper is a
   CA client layer that backboardd composites. The failure is upstream of the wire: no wallpaper
   scene is established (or its content texture never reaches backboardd), so the delivery path
   correctly shows the black fallback.
3. **D2 (white widgets) is independent.** Widget bboxes stay 100 % white with PosterBoard on the
   wire; it is a widget content/render problem (WidgetKit/WidgetRender or its resources), not a
   PosterBoard one.
4. **The boot-arg holds and is orthogonal.** With `launchd_disable_all_timeouts=1`, PosterBoard
   spawns late (15.2 min) and survives indefinitely; no launchd timeout line appears. It remains
   an experiment-only lever (it disables all launchd timeouts).
5. **applog is still not delivering appex stderr** (serial has only the six boot-arg echoes); the
   capture path needs a separate look.

## 9. Boundary and next

- **Next (D1)**: instrument backboardd's CA/scene side — does a wallpaper scene get created
  (`CAWallpaper` / poster scene messages from PosterBoard), and is there a surface whose content
  is empty? The question moves from "who is on the wire" to "which CA layer carries the
  wallpaper".
- **Next (D2)**: WidgetKit / WidgetRender content generation for Weather and Calendar; check
  whether their layers are also CA-only and whether the content texture is ever generated.
- **Not retried**: `launchd_disable_all_timeouts=1` is required for PosterBoard to survive long
  enough to spawn; any future D1 boot should keep it and the applog list (PosterBoard included),
  while reverting the delivery root/system afterwards.
- **Restored**: `userland/root.apfs` = `a4a6e2ce50e6d0324e26a49ea20fbd84` (6,238,208 B); system
  `49ae3cf4…` and kernel `f62c3d4b…` untouched; hosts SIGTERM'ed (missing report flushed); qemu
  terminated by its own timeout (`terminating on signal 15`). Raw frames
  `/data3/iosvm-tmp/wp4-d1d2-j-frames` (45 G, 13,412 files) deleted; `/data3` 284 G free, root
  disk 54 G free.

## 10. Evidence and hashes

Evidence lives in `/tmp/t84-evidence/`:

| file | bytes | md5 |
|---|---|---|
| `T84-REPORT.md` | 8,622 | `f15e2c42…` |
| `serial-wp4-d1d2-j.log` | 25,853,964 | `b041043795fd114484695a59c532fcb4` |
| `metalwire-host.log` | 2,615,030 | `7ce5891b67601cc85c7fe7f12a704cb2` |
| `missing.txt` | 144 | `c2eec52cc9cfd6c275fb72c27adaebc7` |
| `monitor-wp4-d1d2-j.txt` | 7,181 | `f1f4d9190686d04c03491a6b26d6b74a` |
| `stage2-desktop-750x1624.png` | 200,545 | `c6524549982e9a48c63c2e1870f8c29a` |
| `stage3-steady-750x1624.png` | 200,270 | `0559d5b5dae1542fcf280eefa07fcc88` |
| `stage1-bootui-750x1624.png` | 7,801 | `a82befa5c10b7b65f7248b027087d4eb` |

`HASHES.json` in the same directory lists all 20 evidence files plus the seven image/host
artifacts. Variant #6 image: `/home/lshan/DSH-desktop/mt-work/exp-d1d2f/root-d1d2f.apfs`
(`ea0ac44b…`), copy `/tmp/d1d2f-root.apfs`.

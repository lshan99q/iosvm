# stage 53: the appex watchdog extension — the plist key, and a possible regression

> Status: **measured; the fifth D1/D2 experiment is negative and carries a regression warning.**
> Stage 52 showed the `inject.conf` lever works (an appex loaded `libcainject`) but the appex was
> killed 5.63 s after spawn. Variant #5 moves the watchdog override into the **appex's own
> Info.plist** — `XPCService._LaunchWatchdogTimeOut = 120` on six appexes — and reuses variant #4's
> root image. The 3,600 s boot is **invalid**: the poster appexes
> (**CollectionsPoster / GradientPosterExtension) do not exec at all** (t68 had both exec, and
> GradientPosterExtension was injected and killed after 5.63 s), the widget appexes exec 27 times
> but **`ca-inject` still loads only backboardd**, **no appex asks for a Metal device**, the host
> still has **one `hello` (backboardd)** and **no 1125×2436 texture**, and the desktop is unchanged
> (**63.2 %** main-area black, widgets **100 % white**, Dock 66.3 / Search 13.6 / direction normal).
> The plist change did not extend the poster appex lifetime — the poster appexes never started —
> and may have broken launchd's extension-job parsing/signature. **Control A/B boots are required**
> before concluding; if the regression is confirmed, the watchdog direction is closed and the
> remaining path is a short-lived appex with host multi-client + fast handshake. Delivery system
> `49ae3cf4…` and root `a4a6e2ce…` were restored at 11:48 and checked; kernel `f62c3d4b`. Branch
> `experiment/ca-metal-device`, 2026-11-24. This continues
> [`docs/stage52-appex-double-lever.md`](stage52-appex-double-lever.md).

## 1. The question

Stage 52's double lever proved the injection works but the appex dies in 5.63 s, before the clients
gate. t63/t70 identified the job-level watchdog as the lever: the delivery system's
`/private/etc/com.apple.xpc.launchd.limits.plist` already has `ExtensionWatchdog: true` and
`ExtensionWatchdogTimeout: 120`, yet the appex still dies at ~5.6 s, so the effective timeout is the
**job's `XPCService._LaunchWatchdogTimeOut`**, not the global one. Variant #5 writes that key into
six appexes' Info.plist and boots 3,600 s with `applog=1` to see whether the appex survives long
enough to query a Metal device and connect the wire.

## 2. What stage 52 left (t68 recap)

* The `inject.conf` lever works: `ca-inject: loaded as pid 129 (GradientPosterExtension)` right
  after exec (applog made the appex stderr visible).
* The appex is killed as `hanging on launch` 5.63 s after spawn (8.7 s to exit,
  `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`), before it asks for a Metal device.
* The clients lever was not exercised (no `is shown`/`is not shown` appex line); the host had one
  `hello` and no 1125×2436 texture; desktop 64.6 % black, widgets 100 % white.
* The binding blocker was the launchd extension watchdog (~5.6–9 s).

## 3. Variant #5 (t70)

### 3.1 Image scope

The six appex Info.plists live in **system.apfs**, not root.apfs
(`/System/Library/.../CollectionsPoster.appex`, `/System/Library/.../GradientPosterExtension.appex`,
`/Applications/Weather.app/PlugIns/...`, `/Applications/MobileCal.app/PlugIns/...`,
`/Applications/AppStore.app/PlugIns/...`). Variant #5 is therefore **two images**:

| image | path | md5 | sha256 | size |
|---|---|---|---|---|
| variant #5 system | `mt-work/exp-d1d2e/system-d1d2e.apfs` | **`fd1d6fc7037e66b5407c5176cd1e19ea`** | `04efb3c898346c4eed77517f8af7e4609c8006c17fbd4e0671663c8ff49fa076` | 2,476,752,896 B |
| variant #4 root (reused) | `mt-work/exp-d1d2d/root-d1d2d.apfs` | `899bd71181b8cec1847181a7658cb42a` | `44b1ed74…` | 6,234,112 B |
| delivery system (restored) | `mt-work/hakios/userland/system.apfs` | `49ae3cf4fd53334b475a921eb9915008` | — | 2,476,752,896 B |
| delivery root (restored) | `mt-work/hakios/userland/root.apfs` | `a4a6e2ce50e6d0324e26a49ea20fbd84` | — | 6,238,208 B |

The variant root keeps variant #4's changes (inject.conf 440 B, clients 154 B/9 lines, PosterBoard
label dedup); the system variant changes only the six Info.plists.

### 3.2 The six Info.plist changes

Each plist gains `XPCService = {_LaunchWatchdogTimeOut = 120}` while keeping `NSExtension`:

| appex | before sha256 → after | size |
|---|---|---|
| CollectionsPoster | `ed37c37b…` → `a29af25e…` | 1,079 → 1,127 |
| GradientPosterExtension | `f77217a4…` → `ffd07c5c…` | 1,503 → 1,539 |
| WeatherPoster | `ca6aef5e…` → `61f1ab23…` | 1,149 → 1,197 |
| WeatherWidget | `7dd697d5…` → `077cb3d3…` | 1,141 → 1,192 |
| CalendarWidgetExtension | `9a7988f1…` → `9c74522d…` | 1,233 → 1,284 |
| AppStoreWidgetsExtension | `5c2e1a93…` → `c735d8ff…` | 916 → 967 |

### 3.3 Why the job-level key

The delivery `launchd.limits.plist` already carries `ExtensionWatchdog: true` and
`ExtensionWatchdogTimeout: 120` (build.sh:648-650), yet the appex dies at ~5.6 s. t63's launchd
strings (`XPC Service Info.plist is missing XPCService dictionary`, `launch watchdog timeout = %u`)
point to the **job's XPCService dictionary** as the effective source; variant #5 puts the key there.

### 3.4 Build, validation, risks

* Built by `cp -al sysimage/rootfs → sysroot`, replaying the system part of `userland/build.sh`
  (LaunchDaemons/LaunchAngels/xpc, staged apps, stage0 = `obj/stage0` md5 `38d8763f…` matching the
  delivery `/sbin/launchd`, HAL/Metal symlinks, cryptex, dyld cache, launchd limits), then
  `mkapfs.py -z -o system-d1d2e.apfs System=sysroot,manifest=sysimage/system.manifest`.
* Validation: `apfsck -cw: clean`; 62,274 read-back entries match sysroot; against delivery
  system.apfs, **45,726 entries are identical and only the six plists change size**.
* **Code signature**: changing Info.plist invalidates `_CodeSignature/CodeResources`; if
  launchd/ExtensionKit verifies it, the appex may be rejected (no exec, no `ca-inject`). Kernel AMFI
  is NOT_ENFORCING, but launchd-side verification is unknown — this is exactly what t71 tests.
* **launchd semantics**: adding `XPCService` may make launchd treat the job as an XPC service rather
  than an extension (many appexes already have the dictionary; medium-low risk).
* **plistlib re-serialization** changes key order (semantics unchanged).
* The system image is 2.4 GB; t71 must back up/restore `userland/system.apfs` and check the md5.

## 4. The decision boot (t71)

### 4.1 Boot and disk discipline

Tag `wp4-d1d2-e`, 2026-11-24 10:45:36 → 11:46:16 (**3,600 s**), SMP=1/cpus=1, 35 B clients,
0xFF vars, lavapipe pinned, kernel `f62c3d4b`, `applog=1 applog_name=…`. **0 panic, 8,028 frames
acknowledged**; the desktop came up and idled. Host load 5–8, vCPU ~70–80 %, win2022 ~190 %.

The raw frames went to `/data3/iosvm-tmp/wp4-d1d2-e-frames` (55 G) and were deleted after the boot;
the root disk is at **56 G** free and `/data3` at **284 G**. The observation window was not affected.

### 4.2 Serial: poster appexes do not exec

| observation | variant #5 | variant #4 (t68) | baseline |
|---|---|---|---|
| `ca-inject: loaded as pid …` | **only pid 20 (backboardd) ×2** | pid 20 ×2 + **pid 129 (GradientPosterExtension)** | only backboardd |
| CollectionsPoster exec | **0** | yes (multiple) | yes |
| GradientPosterExtension exec | **0** | yes (pid 129, injected, killed 5.63 s) | yes |
| widget appex exec | **27** (CalendarWidgetExtension ×5, WeatherWidget ×4, AppStoreWidgetsExtension, Extragalactic/Emoji/Unity/PridePoster, ScreenTime/Reminders/Podcasts/People/Passbook/NewsToday2, …) | 54 | yes |
| appex `is shown the device` | **0** | 0 | 0 |
| `EXTENSION_CHECK_IN_TIMEOUT` | **11** | 21 | 89 |
| `hanging on launch` | **46** | 80 | 193 |

PosterBoard itself runs (`exec pid=102 path=/Applications/PosterBoard.app/PosterBoard`, 113
mentions), but **CollectionsPoster and GradientPosterExtension are never spawned** — in t68 both
executed. The widget appexes are in the inject.conf list yet show **no `ca-inject` line** either;
only backboardd loaded the plug-in. The device lines remain backboardd-shown and three
not-shown (mediaserverd/SpringBoard/chronod); no appex line.

### 4.3 Host

* **hello = 1** (`pid 20 program "backboardd"`) — no appex client.
* 7 textures, **none ≥1000×2000**; `1125x2436` **0**; `750x1624` 16,049 (display target only).
  No wallpaper upload/draw.

### 4.4 Desktop

| item | variant #5 | baseline | target |
|---|---|---|---|
| main-area black (desktop signature frame) | **63.2 %** | 64.9 % | <20 % ❌ |
| widget bbox1/bbox2 | black 0 % / **white 100 %** | pure white | not pure white ❌ |
| Dock / Search / orientation | 66.3 / 13.6 / normal ✓ | same | no regression ✓ |

Stage 2 (transition) is 59.0 % black, stage 1 (BootUI) 98.7 %. The desktop did not regress.

### 4.5 Verdict and the regression warning

**Invalid.** The `XPCService._LaunchWatchdogTimeOut` key did not extend the poster appex lifetime —
the poster appexes **never started** in this boot, so there is nothing to extend. No device query,
no new host `hello`, no wallpaper texture; the desktop is unchanged.

**Regression warning (needs a control):** the poster appexes went from "exec + killed after 5.63 s"
(t68) to **"no exec at all"** (t71). Two explanations:

1. adding `XPCService` to the appex Info.plist changed launchd's extension-job parsing/validation
   (or invalidated the code signature), so the job is no longer spawned;
2. this boot simply never requested a poster (boot-to-boot variance).

The only way to tell is the **control A/B**: (a) variant system + **delivery root** (isolates the
system/Info.plist change), and (b) **delivery system** + variant root (isolates the root changes).
A repeat of variant #5 would also show whether the poster exec is stably gone.

### 4.6 Restore

Delivery system restored at **11:48** (mtime 11:48:43), md5
`49ae3cf4fd53334b475a921eb9915008`; delivery root at 11:48 (mtime 11:48:45), md5
`a4a6e2ce50e6d0324e26a49ea20fbd84`; kernel `f62c3d4b852d467f22c4703b8fc620c9`. Variant #5 stays in
`mt-work/exp-d1d2e/` and variant #4 root in `mt-work/exp-d1d2d/`; the frames were deleted from
`/data3` after the boot.

## 5. What this establishes and what it does not

**Establishes**

* The **job-level `XPCService._LaunchWatchdogTimeOut`** was the intended lever; this boot did not
  test it on a running poster appex because **no poster appex executed**.
* The **poster appex regression is a real signal to investigate**: t68 had both poster appexes
  exec; t71 has zero, while PosterBoard itself runs and widget appexes exec.
* The `ca-inject` lever from stage 52 is **not reproduced** in this boot: even the widget appexes
  in the inject.conf list show no load, so the injection/visibility path is still not stable.
* The desktop did not regress; the host still has one client and no wallpaper texture.

**Does not establish**

* **That the plist change caused the regression.** It is one of two explanations; the control A/B
  and a repeat are required.
* **Whether the watchdog key works at all.** It was not exercised on a live poster appex.
* **That the appex would query Metal or connect the wire if it survived.** Still untested.
* **A rate.** n=1 boot; the poster exec count is the first thing to reproduce.
* **A delivery change.** The system image is experimental; the `.sig`/CodeResources were not
  updated, secure surfaces may differ, and no V100/isolation core is involved.

## 6. Next steps

1. **Control A/B** (the captain's plan): (a) **variant system + delivery root** — isolates the six
   Info.plist changes; (b) **delivery system + variant root** — isolates the inject.conf/clients/
   dedup changes. Look only at whether the poster appexes exec.
2. **Repeat variant #5** once to see whether "poster appex 0 exec" is stable or a one-boot variance.
3. **If the Info.plist change is confirmed as a regression**, close the watchdog direction: the
   extension-point key (t56) and the job-level key (t70) both fail or break launch. The remaining
   path is a **short-lived appex** (5–9 s) with **host multi-client + fast handshake**, or a
   different extension/scene-host approach.
4. **If a poster appex survives** in a later boot but still does not query a device, inspect the
   `MTLCreateSystemDefaultDevice` entry and the secure-mode CA context.
5. **Acceptance gate unchanged:** a second host `hello` whose program is the appex, before looking
   at wallpaper pixels.

## 7. Evidence and hashes

| item | value |
|---|---|
| variant #5 system | `exp-d1d2e/system-d1d2e.apfs` md5 `fd1d6fc7037e66b5407c5176cd1e19ea`, sha256 `04efb3c898346c4eed77517f8af7e4609c8006c17fbd4e0671663c8ff49fa076`, 2,476,752,896 B |
| variant #4 root (reused) | md5 `899bd71181b8cec1847181a7658cb42a`, sha256 `44b1ed74…`, 6,234,112 B |
| delivery system / root (restored) | `49ae3cf4fd53334b475a921eb9915008` / `a4a6e2ce50e6d0324e26a49ea20fbd84` |
| six Info.plists | CollectionsPoster `ed37c37b…`→`a29af25e…`; GradientPosterExtension `f77217a4…`→`ffd07c5c…`; WeatherPoster `ca6aef5e…`→`61f1ab23…`; WeatherWidget `7dd697d5…`→`077cb3d3…`; CalendarWidgetExtension `9a7988f1…`→`9c74522d…`; AppStoreWidgetsExtension `5c2e1a93…`→`c735d8ff…` |
| t71 report | `/tmp/t71-evidence/T71-REPORT.md` sha256 `e171bcc25b3d37ec0a84d5ba685b70a2a7c79c24552cf472cc5ff2186dfdb759` |
| t71 serial / host / missing / monitor | serial `3d0d39148fde9d49386cd6c14aaf44702484656b051e4958fd1f430d132e41c9` (17.7 MB); host `079486ef…`; missing `1c40d829…`; monitor `f02ca454…` |
| t71 captures | stage1 `71a943d7…`, stage2 `c961e827…`, stage3 `f643d6ca…`, thumb `2a7d8ced…` |
| t68 recap evidence | serial `d06556b0…`; report `fb3354b1…`; pid129 5.63 s |
| kernel / host / glwire / image | kernel `f62c3d4b852d467f22c4703b8fc620c9`; host `ad778a06…`; glwire `5b28249a…`; system `49ae3cf4…`, root `a4a6e2ce…`, libcainject `aa123ccd…`, plug-in `bcaf99c6…` |

### 7.1 Repository gates

`make test` is green at this tree: provenance clean (491 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 8. Files changed in this record

- `docs/stage53-appex-watchdog.md` — this note.
- `docs/state-of-the-port.md` — the D1/D2 rows gain the watchdog-plist negative and the regression
  warning.
- `README.md` — the stage-53 milestone row, the EN+ZH desktop status rows and the EN documentation
  index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the variant images and the
staging tree live outside the repository.

## 9. Reproduce

```sh
# t71 evidence
grep -a -o 'ca-inject: loaded as pid [0-9]* ([^)]*)' mt-work/logs/wp4-d1d2-e/serial.log
grep -a -c 'exec pid=.*CollectionsPoster\|exec pid=.*GradientPosterExtension' mt-work/logs/wp4-d1d2-e/serial.log  # 0
grep -a -c 'exec pid=.*\.appex' mt-work/logs/wp4-d1d2-e/serial.log            # 27
grep -a -c 'EXTENSION_CHECK_IN_TIMEOUT' mt-work/logs/wp4-d1d2-e/serial.log   # 11
grep -a -c 'hanging on launch' mt-work/logs/wp4-d1d2-e/serial.log             # 46
grep -a 'hello' /tmp/t71-metalwire-host.log                                   # backboardd only
python3 /tmp/t71-evidence/analyze3.py mt-work/logs/wp4-d1d2-e/serial.log /tmp/t71-metalwire-host.log

# restore (already done)
md5sum mt-work/hakios/userland/system.apfs   # 49ae3cf4fd53334b475a921eb9915008
md5sum mt-work/hakios/userland/root.apfs     # a4a6e2ce50e6d0324e26a49ea20fbd84
```

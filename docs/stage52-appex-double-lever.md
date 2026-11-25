# stage 52: the appex double lever — the plug-in loads, then the watchdog wins

> Status: **partial, and the first positive step of the D1/D2 chain.** Stage 51 showed that the
> clients file alone cannot help because it is checked only after the plug-in loads. Variant #4
> pulls **both levers at once** — `libcainject` for the appexes in `/etc/inject.conf` **and** the
> appex names in `/lib/HakiosMetal.clients` — and boots with `applog=1`. The 3,600 s boot gives the
> first observed appex injection: **`ca-inject: loaded as pid 129 (GradientPosterExtension)`** right
> after its exec. But the appex is **killed as `hanging on launch` 5.6 s after spawn** (spawn
> 02:02:47.208 → kill 02:02:52.834 → exit 02:02:55.934, `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`),
> so it **never asks for a Metal device**, the host still has **one `hello` (backboardd)** and **no
> 1125×2436 texture**, and the desktop is unchanged: **64.6 %** main-area black (baseline 64.9 %),
> widget bboxes **100 % white**, Dock 66.3 / Search 13.6 / direction normal. The `inject.conf` lever
> works; the **clients lever was not exercised** because the appex dies first. The current blocker is
> the **launchd extension watchdog (~5.6–9 s)**, then the host's single-client wire. Delivery root
> `a4a6e2ce…` was restored at 10:11 and checked; kernel `f62c3d4b`. Branch
> `experiment/ca-metal-device`, 2026-11-24. This continues
> [`docs/stage51-appex-metal-clients.md`](stage51-appex-metal-clients.md).

## 1. The question

Stage 51's clients-file variant was negative because the clients gate is downstream of the plug-in:
the delivery `inject.conf` (304 B) injects only backboardd, so the appex never loads the bundle that
would read the clients file. Variant #4 asks the obvious next question: if the plug-in **is** injected
into the appex **and** the appex is in the clients file, does the appex get a Metal device, connect
the wire, and produce a wallpaper? The boot also enables `applog=1` so the appex's stderr becomes
visible (t63's visibility finding).

## 2. What stage 51 left (t65 recap)

* The real wire gate is **`HakiosMetal.bundle`**, admitted by `/redwood/lib/HakiosMetal.clients`
  (delivery: `metal_frame`, `metal_probe`, `backboardd`); the appexes were not listed.
* The clients file is checked **after** the plug-in is loaded; the delivery `inject.conf` (304 B)
  lists only backboardd, so the appex never loaded the bundle and the clients check never ran.
* The 3,600 s variant #3 boot: appex exec 19 times, `ca-inject` only pid 20 (backboardd), no appex
  `is shown the device`, host one `hello`, no 1125×2436 texture, main black 61.8 %, widgets white.
* The ~9 s kill is launchd's **default extension check-in watchdog**, not the extension-point
  `_LaunchWatchdogTimeOut` in `/xpc/extensions.plist`.

Variant #4 therefore combines the two levers instead of testing them separately.

## 3. Variant #4 (t67)

### 3.1 Changes

`mt-work/exp-d1d2d/root-d1d2d.apfs` (copy `/tmp/d1d2d-root.apfs`) is the delivery root image with
three changes:

1. `/etc/inject.conf`: sha256 `780466d1…` (304 B) → `d4c785c9…` (440 B, under the 511-byte kernel
   limit). The new line is
   `/redwood/lib/libcainject.dylib CollectionsPoster GradientPosterExtension AppStoreWidgetsExtension CalendarWidgetExtension WeatherWidget`;
   the six original lines (including `libcainject.dylib backboardd`) are kept.
2. `/lib/HakiosMetal.clients`: sha256 `92482275…` (35 B, 3 lines) → `4295fcb8…` (154 B, 9 lines):
   `metal_frame`, `metal_probe`, `backboardd`, `CollectionsPoster`, `GradientPosterExtension`,
   `WeatherPoster`, `WeatherWidget`, `CalendarWidgetExtension`, `AppStoreWidgetsExtension`.
   The matching rules are the same as t63: `HakiosMetal.m:6802-6820` line match, `metal=1`
   fallthrough in `mw_process_allowed()` (`6876-6910`).
3. `LaunchAngels/com.apple.PosterBoard.plist` deleted (sha256 `dfc25261…`), keeping the
   LaunchDaemons copy (`89784260…`). `/xpc/extensions.plist` stays the delivery version
   (`e92ee2b8…`); no appex Info.plist is changed. The other 481 files' sha256 match delivery; 502
   entries read back; `apfsck -cw: clean`. The delivery root was never touched
   (`a4a6e2ce…`).

### 3.2 Risks

* **Host single-client**: `mw_wait_for_client()` accepts one client and serves it until disconnect;
  backboardd holds the wire. If an appex connects, it queues behind backboardd and waits for the
  hello ack (`MW_DEFAULT_ACK_MS = 15000`, `mw_wire.h:30`), far beyond the check-in window. The host
  needs multi-client support or a single-appex slot.
* **Watchdog**: even with the plug-in loaded, the appex is killed in ~5.6–9 s unless the launchd
  extension watchdog is extended or bypassed.
* **Sandbox**: the guest sandbox is a stand-in; a profile could refuse the dylib or the TCP wire.
* **Secure surfaces**: the poster appex carries `com.apple.QuartzCore.secure-mode`; a secure CA
  context may not be supported.
* **Metal entry**: if the appex never calls `MTLCreateSystemDefaultDevice` (pure CA rendering), the
  clients list cannot change behavior — the `is shown the device` line is the test.

## 4. The decision boot (t68)

### 4.1 Boot and disk discipline

Tag `wp4-d1d2-d`, 2026-11-24 09:09:23 → 10:10:03 (**3,600 s**), SMP=1/cpus=1, 35 B clients,
0xFF vars, lavapipe pinned, delivery kernel `f62c3d4b`. **0 panic, 3,686 frames acknowledged**;
the desktop came up and idled. Host load 5.9–7.2, vCPU ~77–81 %, win2022 193 %.

The boot used a frame pruner (keep the last 240 frames) and kept `/tmp/t68-frames` at ~11 G; after
the boot the frames were moved to `/data3/iosvm-tmp/wp4-d1d2-d-frames` (11 G), leaving the root disk
at **61 G free**. The observation window was not affected.

### 4.2 Serial: the plug-in loads, the appex dies

| observation | variant #4 | baseline (wp4-soak-fix3) |
|---|---|---|
| appex exec | **54** (WeatherWidget 120/121/122/125, CalendarWidgetExtension 119/123/124/126, GradientPosterExtension 129, CollectionsPoster 130, …) | yes |
| `ca-inject: loaded as pid …` | **3 lines: pid 20 (backboardd) ×2, `pid 129 (GradientPosterExtension)` ×1** | only backboardd |
| appex `is shown the device` | **0** | 0 |
| `EXTENSION_CHECK_IN_TIMEOUT` | **21** | 89 |
| `hanging on launch` | **80** | 193 |

**GradientPosterExtension timeline** (the key evidence): exec pid 129 (serial line 370918) →
`ca-inject: loaded as pid 129 (GradientPosterExtension)` (line 370995) → `Extension is hanging on
launch. Killing.` at 02:02:52.834 (line 374303; **5.63 s after the 02:02:47.208 spawn**) →
`exited … XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT` at 02:02:55.934 (8.7 s after spawn). The
injection happens immediately; the check-in watchdog kills the appex before it can ask for a device.

The device lines are still only `backboardd is shown the device` (1) and `mediaserverd /
SpringBoard / chronod are not shown` (3) — no appex line. The applog made the appex's `ca-inject`
line visible, which is how the injection was confirmed.

### 4.3 Host

* **hello = 1** (`pid 20 program "backboardd"`) — no appex client.
* 8 textures, **none ≥1000×2000**; `1125x2436` **0**; `750x1624` 5,613 (display target only).
  No wallpaper upload/draw.
* The single-client limit was **not triggered**: the appex never opened a connection.

### 4.4 Desktop

| item | variant #4 | baseline | target |
|---|---|---|---|
| main-area black (desktop signature frame) | **64.6 %** | 64.9 % | <20 % ❌ |
| widget bbox1/bbox2 | black 0 % / **white 100 %** | pure white | not pure white ❌ |
| Dock / Search / orientation | 66.3 / 13.6 / normal ✓ | same | no regression ✓ |

The stage captures are all the same desktop frame (64.6 % black); the desktop is static after boot.
Icons, Dock, Search, rdar banner and direction show no regression.

### 4.5 Verdict

**Partial.** The `inject.conf` lever **works** — for the first time an appex process
(`GradientPosterExtension`, pid 129) loaded `libcainject` immediately after exec. But the appex is
killed as `hanging on launch` **5.6 s after spawn**, before it queries a Metal device, so:

* the **clients lever was not exercised** (no `is shown`/`is not shown` appex line);
* the host still has one wire client and no wallpaper texture;
* the wallpaper and widgets are unchanged (64.6 % black, 100 % white).

The current blocker is the **launchd extension watchdog (~5.6–9 s)**; after that, the host's
single-client wire and the appex's Metal entry are the next questions.

### 4.6 Restore

Delivery root restored at **10:11** (file mtime 10:13:23), md5
`a4a6e2ce50e6d0324e26a49ea20fbd84`; kernel `f62c3d4b852d467f22c4703b8fc620c9`. Variant #4 stays in
`mt-work/exp-d1d2d/` and `/tmp/d1d2d-root.apfs`; delivery and staging were not modified. The disk
frames are in `/data3/iosvm-tmp/wp4-d1d2-d-frames` (11 G); the root disk is back to 61 G free.

## 5. What this establishes and what it does not

**Establishes**

* The **inject.conf lever is real**: with the appex names in `/etc/inject.conf`, the appex process
  loads `libcainject` right after exec, and `applog=1` makes that visible (`ca-inject: loaded as pid
  129`).
* The **check-in watchdog is now the binding constraint**: the appex is killed 5.6 s after spawn
  (8.7 s to exit), before any Metal device query.
* The clients file was **not tested** in this boot; its result from variant #3 (no effect because
  the plug-in was absent) still stands.
* The desktop did not regress (Dock/Search/icons/direction normal).

**Does not establish**

* **Whether the appex would query `MTLCreateSystemDefaultDevice`** once it survives; it died first.
* **Whether the clients lever works.** No appex reached the `mw_process_allowed()` check.
* **A rate or a content fix.** n=1 boot; the wallpaper is 64.6 % black, widgets 100 % white; the
  launch counts (21/80) are within the large boot-to-boot variance seen earlier.
* **A host multi-client result.** The appex never connected, so the single-client limit was not
  exercised.
* **A clean appex log.** `applog=1` exposed the `ca-inject` line, but the appex stderr lines are
  still sparse; a longer-lived appex is needed to see SwiftUI/scene/Metal errors.
* **A delivery change.** The image is experimental; the `.sig` is not re-signed, secure surfaces may
  differ, and no V100/isolation core is involved.

## 6. Next steps

1. **Extend or bypass the launchd extension watchdog.** The extension-point
   `_LaunchWatchdogTimeOut` is not the lever (t59); the job's `XPCService._LaunchWatchdogTimeOut`
   (appex Info.plist) or a validated global `ExtensionWatchdog` in
   `/System/Library/xpc/launchd.plist` is. The target is to keep one appex alive long enough to run
   its Metal initialization and check in.
2. **Then exercise the clients lever.** With the appex alive and the plug-in loaded, the `is shown
   the device` line should appear; only then does `HakiosMetal.clients` matter.
3. **Plan for the host's single client.** If the appex connects, backboardd already owns the wire;
   add multi-client support or a single-appex slot before interpreting the first hello.
4. **If the appex still does not query Metal**, inspect the appex's Metal entry
   (`MTLCreateSystemDefaultDevice` vs pure CA) and the secure-mode CA context.
5. **Acceptance gate unchanged:** a second host `hello` whose program is the appex, before looking
   at wallpaper pixels.

## 7. Evidence and hashes

| item | value |
|---|---|
| variant #4 image | `exp-d1d2d/root-d1d2d.apfs` md5 `899bd71181b8cec1847181a7658cb42a`, sha256 `44b1ed7425ff99ffa623a8b57cc571a24d5827822a62c861890ed10d28b3affd`, 6,234,112 B; copy `/tmp/d1d2d-root.apfs` same md5 |
| inject.conf before/after | `780466d105528b6ecf0cdda62414b3e325c066cbc24a44afee6de2c2707bbb04` (304 B) → `d4c785c912e5231f75e57546ad720d455a0e6782b760ca02f5f9ffa65ada6e36` (440 B) |
| clients file before/after | `92482275349dc9df164110f5d24bd9eed4eabcb55f82823c52c495ceb1e1ece7` (35 B) → `4295fcb85ae5f55c78b1c2922d6abca8b44182358ae19ae2777d59e655612868` (154 B) |
| extensions.plist | delivery/variant `e92ee2b8…` (unchanged) |
| t68 report | `/tmp/t68-evidence/T68-REPORT.md` sha256 `fb3354b1da93de652d568dfe2b8134e1a8294f9921e5f61a2048dda86c559cfb` |
| t68 serial / host / missing / monitor | serial `d06556b046008c34d1722c5a2d8d1484415e562c07f42fb095978339cf4c78e1` (30.9 MB); host `f1c9c8b1…`; missing `79b1eaad…`; monitor `617f3a08…` |
| t68 captures | desktop `f1819119…` + thumb `1c540c0f…`; stage1 `3c7fe712…`, stage2 `27b9e1f2…`, stage3 `0bafb31c…` |
| t65 recap evidence | serial `ef6b858e…`; host `22536269…`; stage3 `3c8df10c…`; t65 report `50031cf9…` |
| delivery / kernel | root `a4a6e2ce50e6d0324e26a49ea20fbd84`; kernel `f62c3d4b852d467f22c4703b8fc620c9` |
| host / glwire / image | host `ad778a06…`; glwire `5b28249a…`; system `49ae3cf4…`, libcainject `aa123ccd…`, plug-in `bcaf99c6…` |

### 7.1 Repository gates

`make test` is green at this tree: provenance clean (490 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 8. Files changed in this record

- `docs/stage52-appex-double-lever.md` — this note.
- `docs/state-of-the-port.md` — the D1/D2 rows gain the partial double-lever result and the watchdog
  blocker.
- `README.md` — the stage-52 milestone row, the EN+ZH desktop status rows and the EN documentation
  index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the variant image and the
staging tree live outside the repository.

## 9. Reproduce

```sh
# t68 evidence
grep -a -o 'ca-inject: loaded as pid [0-9]* ([^)]*)' mt-work/logs/wp4-d1d2-d/serial.log
grep -a 'is shown the device\|is not shown' mt-work/logs/wp4-d1d2-d/serial.log
grep -a -c 'exec pid=.*\.appex' mt-work/logs/wp4-d1d2-d/serial.log          # 54
grep -a -c 'EXTENSION_CHECK_IN_TIMEOUT' mt-work/logs/wp4-d1d2-d/serial.log # 21
grep -a -c 'hanging on launch' mt-work/logs/wp4-d1d2-d/serial.log           # 80
grep -a 'hello' /tmp/t68-metalwire-host.log                                 # backboardd only
python3 /tmp/t68-evidence/analyze3.py mt-work/logs/wp4-d1d2-d/serial.log /tmp/t68-metalwire-host.log

# restore (already done)
md5sum mt-work/hakios/userland/root.apfs   # a4a6e2ce50e6d0324e26a49ea20fbd84
```

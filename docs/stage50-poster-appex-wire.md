# stage 50: the poster appex and the wire — an injection experiment that never loaded

> Status: **measured; the second D1/D2 experiment is negative.** t59 localized the break to the
> poster/widget appex **extension check-in / scene-host handshake** and found that `/etc/inject.conf`
> injects `libcainject` **only into backboardd**, so no appex ever connects the wire. A second
> experimental image added the poster/widget appex names to `inject.conf` (and kept the PosterBoard
> label dedup, without a watchdog override). The 3,600 s decision boot showed the appexes **do exec**
> (CollectionsPoster repeatedly, GradientPosterExtension, ExtragalacticPoster, WeatherPoster, …), but
> **libcainject loaded only into pid 20 (backboardd)** — no `ca-inject:` line for any appex — so the
> host still has **one wire client**, **no 1125×2436 wallpaper texture**, the wallpaper is still
> black (**46.1 %** main-area black vs 64.9 % baseline; target <20 %) and the widget bboxes are still
> **100 % white**. The black-pixel drop from 61.8 % to 46.1 % is more icons/labels being drawn, not
> a wallpaper. Two blockers remain: **the appex injection path does not load**, and **the appex is
> killed as `hanging on launch` within seconds** (6.2–52.1 s; t61's summary ≈9 s), so even a loaded
> appex might not reach the wire. Delivery root `a4a6e2ce…` was restored at 01:48 and checked; the
> kernel is the delivery `f62c3d4b`. Branch `experiment/ca-metal-device`, 2026-11-24. This continues
> [`docs/stage49-poster-widgets.md`](stage49-poster-widgets.md).

## 1. The question

Stage 49's watchdog+dedup variant booted but did not change D1/D2 content. The next question was
whether the missing wallpaper/widget layers are caused by the appex never connecting the
metalwire path: if `libcainject` were loaded into the poster appex, the host would see a second
`hello` (program = appex) and any CA/Metal content the appex creates. Stage 50 records the read-only
localization (t59), the second experimental image that adds the appex names to
`/etc/inject.conf` (t60), and the 3,600 s boot that tests whether the injection happens (t61).

## 2. What t59 established (read-only)

### 2.1 The break is at appex check-in / scene-host, not at CA or the host

The chain is: poster appex exec → ExtensionKit check-in / scene-host handshake → poster surface →
CA composite. The baseline shows the appexes exec and then die with
`XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`; the extension point itself requires
`EXRequiresSceneHosting = true` (delivery `/xpc/extensions.plist`), so the handshake is the natural
break. CA receives nothing because no poster surface is produced; the host draws every draw it
receives and has no wallpaper-sized texture. The classification from t59:

| link | verdict | evidence |
|---|---|---|
| appex not running | baseline: **no** (exec happens); variant #1: **unknown** (9.2 s window) | baseline exec lines 91904/91963; variant 0 lines + log truncation |
| running but no scene/check-in failure | **yes (main cause)** | `EXTENSION_CHECK_IN_TIMEOUT` after 6.5–28.1 s in baseline; `EXRequiresSceneHosting = true` |
| draws but does not go through CA | not reached | no poster surface, CA has no content; appex never connects wire (only backboardd injected) |
| CA does not submit / host drops | **no** | host draws every received draw; no wallpaper-sized texture |

### 2.2 Variant #1's "0 poster mentions" was an observation artifact

The variant #1 serial (`wp4-d1d2-a`, 61,926 lines) has PosterBoard uncorking at line 57943
(16:15:37.469) and its last guest timestamp is 16:15:46.64 — only **9.2 s** after uncorking; it also
contains the truncation marker
`[... the log between the first 256 KB and the last 1 MB was dropped ...]` (line 60492; 743 stubs vs
1,161 in the baseline). The baseline had ~8 minutes after uncorking (uncorking line 91878 at
06:02:45.48, first poster exec +1.1 s at 06:02:46.60, CollectionsPoster at 06:03:07.10, log to
06:11). So the variant's 0 CollectionsPoster/GradientPoster mentions do **not** mean "the appex
cannot start"; the window was too short and truncated.

### 2.3 The 120 s watchdog did not take effect

Variant #1 appexes were killed **6.2–52.1 s** after launch (baseline 6.5–28.1 s). A 120 s
`_LaunchWatchdogTimeOut` was never reached, so either the key is not honored on this path or the
kill comes from another `hanging on launch` source. This is why the second experiment **did not add
a watchdog**: the plist change was not a working lever.

### 2.4 `inject.conf` only injects backboardd

`/etc/inject.conf` (delivery, line 6) is `libcainject.dylib backboardd`. No poster or widget appex
is in the list, so **no appex can connect the wire**, even if it runs. That is the direct reason the
host never sees a poster texture: the content is never produced/forwarded, not dropped.

### 2.5 Config and entitlements are consistent

* `CollectionsPoster`: entitlements `com.apple.posterkit.provider`, `com.apple.QuartzCore.secure-mode`,
  `com.apple.posterkit.enhanced-memory-limits`; Info
  `EXAppExtensionAttributes.EXExtensionPointIdentifier = com.apple.posterkit.provider`; links
  PosterKit/SwiftUI/WallpaperKit/UIKit (no direct Metal — its CA content would be composited by
  backboardd).
* `PosterBoard`: entitlements `com.apple.runningboard.posterkit.host`,
  `com.apple.runningboard.assertions.frontboard`, `com.apple.springboard.wallpaper.get`; links the
  real PaperBoardUI/PosterBoard/PosterBoardServices/PosterKit/FrontBoard/ExtensionFoundation.
* The extension point has `EXRequiresSceneHosting = true`, `EXPresentsUserInterface = true`,
  `EXRequiredEntitlements = {com.apple.runningboard.posterkit.host: true}`.

So the resources and permission chain are consistent; the missing piece is the runtime handshake,
not a missing entitlement or a missing framework.

## 3. Variant #2 (t60)

### 3.1 Changes

`mt-work/exp-d1d2b/root-d1d2b.apfs` (copy `/tmp/d1d2b-root.apfs`) is the delivery root image with
two changes:

1. `/etc/inject.conf`: sha256 `780466d1…` (304 B) → `d4c785c9…` (440 B, still under the kernel's
   511-byte limit). One line added:
   `/redwood/lib/libcainject.dylib CollectionsPoster GradientPosterExtension AppStoreWidgetsExtension CalendarWidgetExtension WeatherWidget`;
   the original six lines are kept, including `libcainject.dylib backboardd`. The kernel
   `exec_inject_env()` matches the exec's **basename** (`cn_nameptr`/`cn_namelen`), and these are the
   appex binary names seen in the baseline `exec path=…` lines.
2. `LaunchAngels/com.apple.PosterBoard.plist` deleted (sha256 `dfc25261…`), keeping the
   LaunchDaemons copy (`89784260…`). `/xpc/extensions.plist` stays the delivery version
   (`e92ee2b8…`) — **no watchdog override**.

The rest of the manifest is unchanged: 481 other files' sha256 match delivery, 502 entries read
back, `apfsck -cw: clean`. The delivery root image was never touched (md5 `a4a6e2ce…`).

### 3.2 StandardOutPath is not usable for appexes

An appex's launchd job is generated dynamically by launchd/ExtensionKit from the appex's
Info.plist; the image has no per-appex job plist. Adding a same-label LaunchDaemon plist reproduces
the PosterBoard "same label" error and does not create the extension job; `StandardOutPath` only
applies to ordinary launchd jobs. The chosen visibility instead is the injected `libcainject`: an
appex that loads it prints `metalwire:` lines directly into the serial (the plug-in's `mw_log`).

### 3.3 Risks

* **sandbox**: the guest sandbox is a stand-in (`Seatbelt sandbox policy (stand-in)`, `minisandbox:
  … allowed`); a profile could still refuse the dylib or the plug-in socket, in which case the appex
  either fails dyld (no `metalwire:` after exec) or runs uninjected.
* **secure surfaces**: the appex carries `com.apple.QuartzCore.secure-mode`; a secure CA context may
  not be supported by the host path.
* **511-byte limit**: the file is now 440 B; another name could be silently truncated (init.c
  warns about this).
* **API surface**: `libcainject` was written for backboardd; an appex may use CA/Metal APIs it does
  not cover (in-process `CAMetalLayer`), with crash risk.
* **basename/xpcproxy**: if launchd execs the appex through `xpcproxy` and the final basename
  differs, the match does not fire — the acceptance is a second host `hello`.

## 4. The decision boot (t61)

### 4.1 Boot

Tag `wp4-d1d2-b`, 2026-11-24 00:42:48 → 01:43:48 (**3,600 s**), SMP=1/cpus=1, 35 B clients,
0xFF vars, lavapipe pinned, delivery kernel `f62c3d4b`. **0 panic, 4,998 frames acknowledged**;
the desktop came up and idled. Host: load 3–8, vCPU threads ~65–70 %, win2022 188 %.

### 4.2 Serial: appexes exec, injection does not

| observation | variant #2 | baseline (wp4-soak-fix3) |
|---|---|---|
| `EXTENSION_CHECK_IN_TIMEOUT` | **18** | 89 |
| `hanging on launch` | **122** | 193 |
| poster appex exec | **yes, many** (CollectionsPoster pid 108/121/123/…/265; GradientPosterExtension 107; ExtragalacticPoster 109; WeatherPoster 110; EmojiPoster 111; PridePoster 112; UnityPoster 113; PhotosPosterProvider 114) | yes |
| libcainject load | **only `ca-inject: loaded as pid 20 (backboardd)`** (2 lines, same process) | only backboardd |
| appex outcome | `Extension is hanging on launch. Killing.` (t59's range 6.2–52.1 s; t61's summary ≈9 s) | same |

The kernel printed the new `init: library insertion: … libcainject.dylib CollectionsPoster
GradientPosterExtension …` line, so the configuration **was read**; the appex processes still show
**no `ca-inject:` line** — the injection did not load on that path. The appexes are also still
killed as hanging within seconds, so even a successful load would have little time to connect.

### 4.3 Host: one client, no wallpaper

* **client connected = 1**, **hello = 1** (`pid 20 program "backboardd"`) — no appex client.
* 7 textures, **none ≥1000×2000**; `1125x2436` **0**; `750x1624` 9,990 (display target only).
  No wallpaper upload/draw.

### 4.4 Desktop: content unchanged

| item | variant #2 | baseline | target |
|---|---|---|---|
| main-area black | **46.1 %** | 64.9 % (variant #1 61.8 %) | <20 % ❌ |
| widget bbox1/bbox2 | black 0 % / **white 100 %** | pure white | not pure white ❌ |
| Dock / Search / orientation | 66.3 / 13.6 / normal ✓ | same | no regression ✓ |

The black-pixel drop from 61.8 % to 46.1 % is **more icons/labels drawn** (the desktop got further),
not a wallpaper; the widget bboxes are unchanged. The other desktop elements (icons, Dock, Search,
rdar banner, orientation) show no regression.

### 4.5 Verdict

**Invalid.** The appex injection did not happen (only backboardd loaded `libcainject`), so the
appexes do not connect the wire, no wallpaper texture appears, and D1/D2 remain: wallpaper black,
widgets white. Two blockers are now explicit:

1. **Injection matching/loading on the appex path does not fire** even though the config line is
   read and the basename matches the appex binary. The next probe must log `exec_inject_env()`'s
   match attempts for appex execs (including an `xpcproxy` path).
2. **The appex is killed as hanging within seconds** (6.2–52.1 s; ≈9 s in the summary), before it
   can check in or use a loaded plug-in. This is the check-in/scene-host problem from t59, still
   open.

Acceptance for a future attempt: a **second host `hello` whose program is the appex** — that is
the first observable proof of a working appex wire, before looking at wallpaper textures.

### 4.6 Restore

Delivery root restored at **01:48** (file mtime 01:48:09), md5
`a4a6e2ce50e6d0324e26a49ea20fbd84`; kernel `f62c3d4b852d467f22c4703b8fc620c9`. Variant #2 stays in
`mt-work/exp-d1d2b/` and `/tmp/d1d2b-root.apfs`; delivery and staging were not modified.

## 5. What this establishes and what it does not

**Establishes**

* The D1/D2 content gap is upstream of CA: the poster appex never produces a surface, and the
  appex does not connect the wire; the host is not dropping a layer.
* The `inject.conf` appex line is **read** but does **not load** on the appex path — the injection
  blocker is concrete, not speculative.
* The appex check-in kill (t59) is still present in a 3,600 s window (18 timeouts / 122 hanging),
  and the 120 s watchdog is not a working lever.
* The second variant did not regress the desktop (Dock/Search/icons/orientation normal).

**Does not establish**

* **Why** the injection match/load fails (basename vs xpcproxy, sandbox, dyld, secure-mode, plug-in
  init order). No kernel/plug-in log for the appex path yet.
* **Whether a loaded appex would fix D1/D2.** The appex is killed within seconds; even a successful
  load may not reach check-in. The scene-host handshake is still the deeper unknown.
* **A rate.** n=1 boot for variant #2; the launch counts vary strongly between boots (0–89 on the
  same delivery image), and this round's 18/122 is not a fix.
* **A clean signature/entitlement story.** The image is experimental; the `.sig` was not re-signed,
  and a secure CA context may behave differently from the stand-in sandbox.
* **A general result.** The variant is a copy of the delivery root image with two edits; no other
  image, no V100, no isolation core.

## 6. Next steps

1. **Log the injection match.** Add a kernel log in `exec_inject_env()` for appex execs: print the
   exec basename, the matched rule, and whether the dylib was inserted. This tells "rule not
   matched" from "dyld refused" from "plug-in init failed".
2. **Keep the appex alive past check-in.** The 9 s kill is the second blocker; instrument the
   extension check-in/scene-host handshake (or the `hanging on launch` timeout source) before
   adding more injection rules.
3. **Use the second hello as the acceptance gate.** Any future image should be judged first by
   `client connected`/`hello` with an appex program; only then look at wallpaper textures and
   black pixels.
4. **If the appex still cannot check in**, treat D1/D2 as an image/ExtensionKit problem and keep
   them open; the delivery desktop is unaffected. A launchd patch or a different extension point
   remains an experimental-image change with its own boot/rollback.

## 7. Evidence and hashes

| item | value |
|---|---|
| variant #2 image | `exp-d1d2b/root-d1d2b.apfs` md5 `77ee27220e1f8e6fbebf7348d4f4dec2`, sha256 `254605b926d9500ab1c5201fe5699a9711d075f695d766e749acef53eb8fe94e`, 6,234,112 B; copy `/tmp/d1d2b-root.apfs` same md5 |
| delivery root (restored) | md5 `a4a6e2ce50e6d0324e26a49ea20fbd84` |
| inject.conf before/after | `780466d105528b6ecf0cdda62414b3e325c066cbc24a44afee6de2c2707bbb04` (304 B) → `d4c785c912e5231f75e57546ad720d455a0e6782b760ca02f5f9ffa65ada6e36` (440 B) |
| extensions.plist | delivery/variant `e92ee2b8…` (unchanged) |
| t61 report | `/tmp/t61-evidence/T61-REPORT.md` sha256 `c4b56a9e267c7342023574afb9c1b0542d18570ad9da9d646304eac17b364da6` |
| t61 serial / host / missing / monitor | serial `5827ae64fb040c2f8df2022b0467c6b8eb10b2248e7d99414eb710482ee9afc2`; host `a1f7b0fc…`; missing `5df9d242…`; monitor `5a419df5…` |
| t61 captures | desktop `0b228900…` + thumb `c070ac9c…`; stage1 `71a943d7…`, stage2 `c04a9dc5…`, stage3 `9c2395ff…` |
| t59 baseline evidence | t35 serial `wp4-soak-fix3` `33a16906…`; variant #1 serial `wp4-d1d2-a` `ac644996…`; t27 serial `115d2f99…`; t27 host `6a980d69…` |
| kernel / host / glwire / image | kernel `f62c3d4b852d467f22c4703b8fc620c9`; host `ad778a06…`; glwire `5b28249a…`; root `a4a6e2ce…`, system `49ae3cf4…`, libcainject `aa123ccd…`, plug-in `bcaf99c6…` |

### 7.1 Repository gates

`make test` is green at this tree: provenance clean (488 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 8. Files changed in this record

- `docs/stage50-poster-appex-wire.md` — this note.
- `docs/state-of-the-port.md` — the D1/D2 rows gain the injection blocker and the 9 s kill.
- `README.md` — the stage-50 milestone row, the EN+ZH desktop status rows and the EN documentation
  index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the variant image and the
staging tree live outside the repository.

## 9. Reproduce

```sh
# t61 evidence
grep -a -c EXTENSION_CHECK_IN_TIMEOUT mt-work/logs/wp4-d1d2-b/serial.log     # 18
grep -a -c 'hanging on launch' mt-work/logs/wp4-d1d2-b/serial.log            # 122
grep -a 'ca-inject: loaded as' mt-work/logs/wp4-d1d2-b/serial.log            # only pid 20
grep -a 'library insertion' mt-work/logs/wp4-d1d2-b/serial.log | head -1     # the new inject.conf line
grep -a -o '1125x2436\|750x1624' /tmp/t61-metalwire-host.log | sort | uniq -c
python3 /tmp/t61-evidence/analyze2.py mt-work/logs/wp4-d1d2-b/serial.log /tmp/t61-metalwire-host.log

# restore (already done)
md5sum mt-work/hakios/userland/root.apfs   # a4a6e2ce50e6d0324e26a49ea20fbd84
```

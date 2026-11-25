# stage 49: the wallpaper and the white widgets — an appex check-in failure, and a negative fix

> Status: **measured; the D1/D2 decision experiment is negative.** D1 (black wallpaper) and D2
> (white Weather/Calendar widgets) share one guest-side root: **poster and widget extensions are
> killed by launchd with `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`** — 89 timeouts / 97 hanging
> launches in the t35 baseline, with CollectionsPoster dying after ≈11.3 s and GradientPoster after
> ≈5.1 s. The image is complete (WallpaperKit, CollectionsPoster.appex, Assets.car, Bokeh
> `.wallpaper` collections, PosterBoard.app); the host is not at fault (only backboardd is a client,
> no 1125×2436 texture, no full-screen wallpaper draw). Two image-side defects were found and fixed
> in an experimental variant: the missing `_LaunchWatchdogTimeOut` in `/xpc/extensions.plist` and
> the duplicate PosterBoard service label (LaunchAngels + LaunchDaemons). The variant booted
> (1,800 s, 0 panic, 1,746 frames) and the **content did not change**: main-area black 61.8 % vs
> baseline 64.9 % (target <20 %), widget bboxes 100 % white (target: not pure white), host still no
> 1125×2436 texture. Launch counts fell to 2 timeouts / 10 hanging, but the same delivery image
> varies between 0/0 and 89/193 across boots, so that is not evidence of a fix. **The launch-side
> change is real; the content is not fixed.** The delivery image `a4a6e2ce…` was restored at
> 00:17:14 and checked. Branch `experiment/ca-metal-device`, 2026-11-24.

## 1. The question

Stage 35's close and stage 39's D8 fix left two open items on the desktop: the wallpaper is black
(D1) and the Weather/Calendar widgets are pure white (D2). Both were classified as guest/image-side
because the host renders every layer it receives; what was missing was **why the guest never
produces those layers**. Stage 49 records the investigation (t54), the experimental image built to
test the two candidate image defects (t56), and the decision boot that ran it (t57). The separate
D2 read-only task (t55) was cancelled; t54's investigation covered the widget side as well.

## 2. The root cause (t54, read-only)

### 2.1 The baseline: every poster appex is killed at check-in

t35 (`wp4-soak-fix3/serial.log`, 101,020 lines) shows PosterBoard running as pid 105 and spawning
the poster extensions; then launchd kills them all:

* `WallpaperKit.CollectionsPoster [125]`: spawn 06:03:07.097 → "Extension is hanging on launch.
  Killing." 06:03:18.275 → `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT` 06:03:20.654 (≈11.3 s to
  the kill; ≈13.6 s to the exit);
* `GradientPosterExtension [123]`: spawn 06:02:57.529 → kill 06:03:02.532 (≈5.1 s) → exit 06:03:05.056;
* Extragalactic / Emoji / Pride / Unity / Photos: two spawns and two timeouts each; CollectionsPoster
  alone has six timeouts (all retries die).

The whole log has **89 `EXTENSION_CHECK_IN_TIMEOUT` and 97 `hanging on launch`** lines. It is not a
wallpaper-only problem: `AppStore.Widgets`, `CalendarWidgetExtension`, `weather.widget`,
`MusicWidgets` and others time out too — the same mechanism explains D2's white widget placeholders.

### 2.2 The image is complete

The resources are present (read-only check of system.apfs and root.apfs): WallpaperKit /
WatchFacesWallpaperSupport / PaperBoardUI / PaperKit / PosterBoard / PosterBoardServices /
PosterBoardUI / PosterKit frameworks; `WallpaperKit.framework/PlugIns/CollectionsPoster.appex`
(with `EXExtensionPointIdentifier = com.apple.posterkit.provider`); `WallpaperKit.framework/Assets.car`;
`/Library/Wallpaper/Collections/Bokeh.wallpaperCollection/…` with the `.wallpaper` packages; and
`/Applications/PosterBoard.app`. The root image has `/LaunchDaemons/com.apple.PosterBoard.plist`,
`/LaunchAngels/com.apple.PosterBoard.plist` (the duplicate label) and `/xpc/extensions.plist`
(104,731 bytes).

### 2.3 Two image defects

* **Duplicate service label.** t27 (`wp4-v14b/serial.log`) line 1159:
  `Caller tried to import service with same label as an existing service: path =
  /redwood/LaunchAngels/com.apple.PosterBoard.plist`, and line 1164:
  `Failed to bootstrap path: … error = 17: File exists` (the LaunchDaemons copy is the other one).
  PosterBoard then starts late (line 6584, 23:55:46), and the log has **0** CollectionsPoster /
  GradientPoster mentions — on that boot no poster extension ran at all. t42 shows the same
  conflict (2 lines). So on some boots the duplicate plist alone is enough to lose PosterBoard.
* **No extension watchdog override.** `/xpc/extensions.plist` has the
  `com.apple.posterkit.provider` extension point (`EXRequiresSceneHosting = True`, requires
  `com.apple.runningboard.posterkit.host`) but **no `_LaunchWatchdogTimeOut`**; other extension
  points carry 0/10. Under MTTCG + host load, a poster appex that takes longer than the default
  check-in window is killed even though it might eventually be ready.

### 2.4 The host is not the missing piece

t27's host log has **one client, backboardd** (hello, line 13); its 65 textures top out at the
750×1624 panel surfaces, with **no 1125×2436** and no other wallpaper-sized image; `replaceRegion`
calls are small (180×180, 129×158, 128×1, 256×1, …). The end-of-run draw dump is 20 full-screen
`PBGRA_Xfc` pure-black fills, 9 LUT draws (`PBGRA_TlubXf_Isrc`, texture 46 = 128×1), 2 surface
composites (texture 57 = 750×1624) and 1 `PBGRABsov_Xfc` LUT (texture 127 = 256×1) — **no
full-screen wallpaper image draw**. The screenshot's main area is 64.9 % black while the icons and
status bar are correct, so the host's colour/texture/composite path works.

### 2.5 Classification

| candidate | verdict | evidence |
|---|---|---|
| guest resources missing | **no** | frameworks/appex/Assets.car/.wallpaper/PosterBoard.app all present |
| guest service not running | **yes, precisely: appex check-in fails** | 89 timeout / 97 hanging; 7 poster appexes die; duplicate PosterBoard label on some boots |
| CA does not send the layer | **yes (downstream consequence)** | no wallpaper draw, no wallpaper-sized texture; only black fill + LUTs |
| host does not draw | **no** | icons/status bar/gradients render; every dump draw executed |

## 3. The D1/D2 decision experiment (t56/t57)

### 3.1 The variant image

`mt-work/exp-d1d2/root-d1d2.apfs` (copy `/tmp/d1d2-root-d1d2.apfs`) is a copy of the delivery root
image with exactly two changes:

1. `/xpc/extensions.plist` — sha256 `e92ee2b8…` → `e3d6fe60…` (104,731 → 33,020 bytes; plistlib
   compact re-serialization). Semantic changes only: `com.apple.posterkit.provider` and
   `com.apple.widgetkit-extension` gain
   `XPCService = {RunLoopType: NSRunLoop, ServiceType: Application, _AdditionalSubServices:
   {apple-extension-service: true}, _LaunchWatchdogTimeOut: 120}`, and
   `com.apple.widget-extension`'s existing `XPCService` gains `_LaunchWatchdogTimeOut: 120`. The
   normalized comparison keeps all 138 extension-point keys; only these three entries change
   semantically (other diffs are dict key order).
2. `LaunchAngels/com.apple.PosterBoard.plist` deleted (sha256 `dfc25261…`), keeping
   `LaunchDaemons/com.apple.PosterBoard.plist` (`89784260…`). The serial still lists an **empty**
   `/redwood/LaunchAngels` directory on the variant boot (2 lines), but the plist is gone and the
   same-label errors are gone; the delivery image has 483 files, the variant 482, and the other
   481 files' sha256 match delivery.

`extensions.plist.sig` is **kept, not re-signed**. Basis: xnu `bsd/kern/kern_minimac.c:60-110`'s
`miniamfi_policy_syscall()` returns `AMFI_BOOTARG_STATE_NOT_ENFORCING (0x86)` for ExtensionKit's
`AMFI_SYSCALL_QUERY_BOOTARG_STATE (0x60)`, so ExtensionKit reads the table without an AMFI
signature check. The variant boot's `appex refused` count is 0, which supports that (no
"does not correspond to a known extension point" errors).

The image was built with `hakios/tools/apfs/mkapfs.py -z --plain fw -o root-d1d2.apfs redwood=root` from a
staging copy (the same packaging line as `userland/build.sh`), and checked with
`hakios/tools/apfs/check-image.sh` (`apfsck -cw: clean`; `apfs-tool` read back all 502 entries matching
staging). The delivery root image was never modified (md5 `a4a6e2ce…` throughout).

### 3.2 The decision boot (`wp4-d1d2-a`)

Booted 23:45:53, 1,800 s window, SMP=1/cpus=1, 35 B clients, 0xFF vars, lavapipe pinned, delivery
kernel `f62c3d4b` (not a probe). **0 panic, 1,746 frames acknowledged**; the desktop came up and
then idled. Host: load 2.9–8.2, QEMU peak 93 %, vCPU threads up to 91.6 %, win2022 188 %.

### 3.3 What changed and what did not

| observation | variant `wp4-d1d2-a` (1,800 s) | baseline `wp4-soak-fix3` (2,400 s) | control `t53-selfcheck` (1,500 s) |
|---|---|---|---|
| `EXTENSION_CHECK_IN_TIMEOUT` | **2** | 89 | 0 |
| `hanging on launch` | **10** | 193 | 0 |
| CollectionsPoster / GradientPoster mentions | **0 / 0** | 175 / 59 | 0 / 0 |
| PosterBoard | 60 mentions, `exec pid=105` ✓ | 158 | — |
| AppStore.Widgets | 32 mentions, running 3 | 44 | — |
| remaining timeouts | **FindMyWidgetItems, AppStore.Widgets** | many | — |
| duplicate-label / `Failed to bootstrap` for PosterBoard | **0** | — (t27: 2) | — |
| host wallpaper texture (1125×2436 / ≥1000×2000) | **0 / 0** | 0 | 0 |
| host textures | 6, max 750×1624 (3487 occurrences) | — | — |
| main-area black | **61.8 %** (stage 2) | 64.9 % | — |
| widget bbox1/bbox2 | **black 0 % / white 100 %** | — | — |
| Dock / Search / orientation | 66.3 / 13.6 / normal | — | — |

* **Launch side:** the duplicate-label errors are gone and the timeout/hanging counts fell — but
  the same delivery image produced 0/0 in one boot and 89/193 in another, so the counts are not a
  criterion. The variant's 2/10 is inside that variance.
* **Content side:** the wallpaper is still black (61.8 % vs 64.9 % baseline, target <20 %), the
  widget bboxes are still **100 % white** (target: not pure white), and the host still has **no
  1125×2436 texture and no full-screen wallpaper draw** (6 textures, all panel/small; 750×1624
  3487 times).
* **New information:** in the variant boot the poster appexes are not mentioned **at all**
  (0/0 vs baseline 175/59). Whether they were never requested, or their logs vanished, cannot be
  told from this boot; the two remaining timeouts are widget appexes, so the widget path still
  times out. PosterBoard itself runs and the duplicate plist is gone.

### 3.4 Verdict

**Invalid for D1/D2 content.** The watchdog override and the duplicate-label fix address real
image defects and change the launch bookkeeping, but the desktop content does not change:
**launch-count improvement is not a content fix**. The root cause remains at the appex check-in /
content-delivery layer: a poster appex that never checks in (or is never asked for) cannot produce
a wallpaper surface, and a widget appex that times out cannot produce widget content. The decision
boot also rules out the simple "watchdog too short" explanation for the content: even with a 120 s
watchdog the wallpaper path produced nothing.

### 3.5 Restore

The delivery root image was restored at **00:17:14** (file mtime 00:17:18), md5
`a4a6e2ce50e6d0324e26a49ea20fbd84`, sha256 `dc4100d3…`, and the delivery kernel is unchanged
(`f62c3d4b…`). The variant remains only in `mt-work/exp-d1d2/` and `/tmp/d1d2-root-d1d2.apfs`; the
delivery image and staging tree were never touched (staging `/xpc/extensions.plist` still
`e92ee2b8…`, mtime 11-20 19:30).

## 4. What this establishes and what it does not

**Establishes**

* D1 and D2 share a guest-side mechanism: **poster/widget appex check-in failures** (89/97 in the
  t35 baseline), not missing resources and not a host rendering bug.
* Two concrete image defects exist and are fixed in the variant: the missing
  `_LaunchWatchdogTimeOut` on the poster/widget extension points, and the duplicate PosterBoard
  service label.
* The content is still missing after those fixes; the experiment is a **negative**, not a partial
  success.

**Does not establish**

* **Why the poster appex never checks in.** The variant boot has 0 poster-appex mentions; the
  baseline shows them spawned and killed. The next probe must read the appex's own log / XPC
  handshake, not only launchd's bookkeeping.
* **That the variant is worse or better on content.** n=1 variant boot; the counts vary 0–89 on the
  same delivery image; the content metrics are unchanged within a few points.
* **A launchd fix.** No launchd binary patch was attempted (it has a wider blast radius); the
  experiment only changed the plist and removed the duplicate.
* **A clean signature story.** The `.sig` was not re-signed; the variant boot accepted the table
  (appex refused 0), but a different boot path could still reject it. This is an experimental
  image, not a delivery candidate.
* **A general result for other images.** The variant is a copy of the delivery root image with two
  edits; the delivery image and the staging tree were not modified.
* **D2 separately.** The separate D2 read-only task (t55) was cancelled; the D2 evidence in this
  record is the widget-appex timeout family from t54 and the variant's still-white bboxes.

## 5. Next steps

1. **Read the poster appex's own log.** In the baseline it is spawned and killed; in the variant it
   is not mentioned. Instrument or capture `CollectionsPoster`/`GradientPoster` stdout/stderr and
   the ExtensionKit XPC check-in, to tell "never requested" from "requested, stuck before
   check-in".
2. **Follow the appex → CA delivery path.** The host never sees a 1125×2436 texture, so the missing
   layer is upstream of metalwire. A poster appex that checks in but produces no surface is the
   next branch; the wallpaper picker/PosterBoard request path is the other.
3. **If the watchdog direction is continued, use a same-boot control.** The 0–89 spread on the
   same image means only a within-boot A/B (same load, same duration) can attribute a count change
   to the plist edit.
4. **Keep the delivery unchanged.** D1/D2 remain open; the single-vCPU desktop baseline is
   unaffected. A launchd patch, if ever needed, stays an experimental-image change with its own
   boot and rollback plan.

## 6. Evidence and hashes

| item | value |
|---|---|
| variant image | `exp-d1d2/root-d1d2.apfs` md5 `ab4f11f513a6eb8b22409aaaaea3f3b7`, sha256 `2d1ad8ad2fedeea198f2cc7b7f930e03f02c336f1ccfaead14917d546862a701`, 6,217,728 B; copy `/tmp/d1d2-root-d1d2.apfs` same md5 |
| delivery root (restored) | md5 `a4a6e2ce50e6d0324e26a49ea20fbd84`, sha256 `dc4100d3841a05d7e7ab4ca89c0119a4cb02c8170fbd0a292e95afd7ab0c0a67` |
| staging / variant plists | staging `/xpc/extensions.plist` sha256 `e92ee2b8…`; variant `e3d6fe60…` (`/tmp/d1d2-extensions.plist`, 33,020 B) |
| t57 report | `/tmp/t57-evidence/T57-REPORT.md` sha256 `6dff47a2a85ee2b3560fe52e1019cbac80cee3727dbcb19af81c7204b7fed8ef` |
| variant serial / host / missing / monitor | serial `ac644996177f2148333b48b2e87cf830d998a779431c8cece88954c1396100a8`; host `f3b5b5b09e9ea62ea0514fcc617e493c30dc3826f7bf8bca2a61b44faf40691b`; missing `3822a9bdc407e84aa25f4c101d330713739fccc3212ff0d935411f4ea399b103`; monitor `efa9ef34…` |
| variant screenshots | stage1 `71a943d7…`, stage2 `4474d911…`, stage3 `2f3931c3…` (750×1624); 375×812 thumb `1f3c3e9f…` |
| baseline t35 serial | `wp4-soak-fix3/serial.log` sha256 `33a169067bf9cad08f25144b99ec7bd538bece843c326ffbe18c978c7966b27e` (89 timeout / 97 hanging) |
| t27 baseline serial / host / draw dump | `wp4-v14b/serial.log` `115d2f99…`; host `6a980d69…`; `draws-at-end.txt` `eb4962d0…`; baseline screenshot `/tmp/wp4v14b-evidence/stage3-steady-750x1624.png` sha256 `da437513…` |
| kernel / host / glwire / image | kernel `f62c3d4b852d467f22c4703b8fc620c9`; host `ad778a06…`; glwire `5b28249a…`; root `a4a6e2ce…`, system `49ae3cf4…`, libcainject `aa123ccd…`, plug-in `bcaf99c6…` |

### 6.1 Repository gates

`make test` is green at this tree: provenance clean (487 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 7. Files changed in this record

- `docs/stage49-poster-widgets.md` — this note.
- `docs/state-of-the-port.md` — the D1/D2 rows now carry the appex check-in root cause and the
  negative variant result.
- `README.md` — the stage-49 milestone row, the EN+ZH desktop status rows and the EN documentation
  index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the variant image and the
staging tree live outside the repository.

## 8. Reproduce

```sh
# t54 evidence (read-only): baseline t35 boot
grep -a -c EXTENSION_CHECK_IN_TIMEOUT mt-work/logs/wp4-soak-fix3/serial.log   # 89
grep -a -c 'hanging on launch' mt-work/logs/wp4-soak-fix3/serial.log          # 97
grep -a 'CollectionsPoster\|GradientPoster' mt-work/logs/wp4-soak-fix3/serial.log | head

# t57 evidence: variant boot (image built by t56)
grep -a -c EXTENSION_CHECK_IN_TIMEOUT mt-work/logs/wp4-d1d2-a/serial.log      # 2
grep -a -c 'hanging on launch' mt-work/logs/wp4-d1d2-a/serial.log             # 10
grep -a -o '1125x2436\|750x1624' /tmp/t57-metalwire-host.log | sort | uniq -c
python3 /tmp/t57-evidence/analyze.py mt-work/logs/wp4-d1d2-a/serial.log /tmp/t57-metalwire-host.log

# restore the delivery image (already done)
md5sum mt-work/hakios/userland/root.apfs   # a4a6e2ce50e6d0324e26a49ea20fbd84
```

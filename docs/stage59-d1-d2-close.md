# stage 59: the wallpaper and the widgets — the gate moves from launchd to the poster publisher, and the desktop metric is corrected

> Status: **measured; D1 and D2 still open.** Stage 58 (t91, commit `440f887`) fixed the host's
> render-wire accounting, so the multi-client host now reports poster frames (`--csv` rows,
> `mw_record_frame()`); that is infrastructure, not a D1/D2 fix. Stage 59 records the D1/D2
> investigation chain t86–t94: the launchd extension watchdog was bypassed
> (`launchd_disable_all_timeouts=1`), the hakios wallpaper recipe was transplanted
> (`PRSService prewarmWithCompletion:` + `+[SBSUIWallpaperService restoreDefaultWallpaper]`), the
> PosterBoard device-family gate was located (`(userInterfaceIdiom & ~4) == 1` in
> `PBUIWallpaperConfigurationManager`) and bypassed with a PosterBoard-only idiom spoof. That
> spoof is the first positive D1 step: `Hello NOT iPad` **0** / `not today` **0**, PosterBoard
> becomes the **second wire client** (pid 37), renders 91/80/78/67/64/43-draw frames, and creates
> the **first legacy poster configuration** (`com.apple.PaperBoard.LegacyPoster:configuration:
> 4E13B5D6-…/0-0`) plus a switcher configuration. It still does not produce a visible wallpaper:
> `PBFWallpaperPublisher` then fails with **`failed to load homeConfig`** and SpringBoard never
> logs `Created new wallpaper scene`. D2 is unchanged (widgets 100 % white); t87 located it in
> chronod/ChronoCore (no timeline, no WidgetRenderer, persona type 6 missing) and ruled the host
> out. The stage also **corrects the WP-4 desktop metric**: the Dock/Search/status-bar numbers
> reported since stage 35 were sampled while the BootUI overlay was still up; after
> `didFinishFirstBoot` dismisses it (present in t84/t88/t90 as well), the live SpringBoard scene
> has no Dock/Search/status bar. Delivery root `a4a6e2ce…`, system `49ae3cf4…`, kernel
> `f62c3d4b…` are restored; all variant images stay only under `mt-work/exp-d1d2*`. Branch
> `experiment/ca-metal-device`, 2026-11-25. This continues
> [`docs/stage57-posterboard-wire.md`](stage57-posterboard-wire.md); stage 56 is
> [`docs/stage56-launchd-timeouts.md`](stage56-launchd-timeouts.md).

## 1. The question

Stage 57 left D1 at “PosterBoard is injected and alive but never asks for a device”. By then the
launchd 5 s kill was already gone (stage 56), and the poster appex model was dead: appexes stay
alive but never publish. This stage asks four questions:

1. who actually creates the wallpaper surface, and what is the appex/PosterBoard ↔
   backboardd ↔ SpringBoard handshake (t86, t88)?
2. what does the hakios wallpaper recipe do, and does it port to 16.5 (t89, t90)?
3. where exactly is the iPhone gate that refuses `restoreDefaultWallpaper`, and what is the
   cheapest way past it (t92, t93)?
4. once past the gate, what is the next failing step (t94)?

A parallel question covers D2: where do the Weather/Calendar white placeholder widgets get their
content, and does it touch metalwire at all (t87)?

## 2. Inputs and hashes

| item | value |
|---|---|
| delivery root.apfs | `a4a6e2ce50e6d0324e26a49ea20fbd84`, 6,238,208 B |
| delivery system.apfs | `49ae3cf4fd53334b475a921eb9915008`, 2,476,752,896 B |
| delivery kernel | `f62c3d4b852d467f22c4703b8fc620c9` |
| host used in t94 | `3c487ee2b733277b6adeeef407f857e0` (stage 58, commit `440f887`) |
| host_glwire | `5b28249a37cb11207f3ab160bcf4ac23` |
| t89 experimental root | `4314255b8b6f5d58d11979f4f93144da`, 6,234,112 B |
| t93/t94 experimental root | `5afce9674574c85c4bea4aff14635c84`, 6,238,208 B, sha256 `dce5c28a…` |
| t93 dylib | `libhakios.pb.dylib`, 67,008 B, md5 `598ff83f3dc29af0f7e31bba39073349` |
| t89 dylib | `libhakios.sb.dylib`, 111,520 B, md5 `86db0a4d6818e87a1311391773a7a51d` |
| t94 boot tag | `wp4-d1-l`, 2026-11-25 17:06:33 → 17:36:53 CST (1,800 s + loader_return), serial 7,427,517 B |
| t94 boot-args | `debug=0x8 serial=3 rd=md0 loader_return_after=1800 … cpus=1 … launchd_disable_all_timeouts=1 applog=1 applog_name=SpringBoard,PosterBoard,backboardd` |

The t93 root is the t89 root plus exactly two changes: the `inject.conf` line for PosterBoard and
the new `libhakios.pb.dylib`. `apfsck` is clean and the 503 entries read back identically from the
source tree.

## 3. D1: from "no scene" to "no homeConfig"

### 3.1 Who owns the wallpaper surface (t86, read-only)

The surface is created by **SpringBoard**, not by backboardd and not by the host:

- `SBWallpaperController`, `SBWallpaperServer`, `_SBWallpaperWindow`,
  `_SBWallpaperSecureWindow`, `SBWallpaperEffectView`;
- PaperBoardUI: `PBUIWallpaperWindowScene`, `PBUIPosterWallpaperRemoteViewController`,
  `PBUIPosterHomeViewController`;
- the publisher chain runs PosterBoard `PBFWallpaperPublisher` → PosterBoardServices
  `PRSWallpaperPublisher` → SpringBoard `PRSWallpaperObserver`.

backboardd is only the generic CA render server; it has no wallpaper/PosterBoard strings.
PosterBoard.app and `SBRendererService` do **not** link Metal.framework/MetalKit, so “PosterBoard
creates an MTLDevice” was the wrong expectation — that is why no wire client ever appeared.
The image also lacks the wallpaper assets that the legacy path expects:
`/Library/Wallpaper/Wallpapers~iphone.plist` is absent, `Stills/iPhone` is absent (only
`Stills/Any`, seven Heritage Stripe bundles, plus the iOS_16 collection), and Setup Assistant was
never run (`com.apple.purplebuddy.plist` has `ForceNoBuddy`), so no initial poster was seeded.

### 3.2 Evidence boot t88: SpringBoard never creates the scene

t88 boots the delivery images with `applog_name=SpringBoard,backboardd,PosterBoard,…`
(`wp4-d1-e1`, 1,800 s, 0 panic / 0 timeout / 0 wire error, 2,630 frames, host one client,
no 1125×2436 texture). SpringBoard logs **no** `Created new wallpaper scene`, no
`SBWallpaperController`, no `PBUIWallpaperWindowScene`, no `PRSWallpaperObserver`. What it does
log: `Autobahn` (×2) — `wallpaper migration will not run in SpringBoard with Autobahn enabled.` —
and PosterBoard logs `Start wallpaper migration` twice and `restoreDefaultWallpaper… not today`
twice. `End wallpaper migration` is **0**. So the migration path starts and is then refused.

### 3.3 The hakios recipe, transplanted (t89, t90)

hakios (iOS 26 tree, `userland/userland/src/inject26.c:844-914`) solves the same problem:

- `sb_wake_posterboard`: `[objc_getClass("PRSService") alloc/init]` then
  `prewarmWithCompletion:` — PosterBoard never starts by itself because SpringBoard only asks for
  it when a poster already exists;
- `sb_restore_wallpaper`: `+[SBSUIWallpaperService restoreDefaultWallpaper]` — the same operation
  as Settings → “Restore Default Wallpaper”;
- both are dispatched 20 s / 40 s after the SpringBoard constructor, on a global queue (a service
  answer on the main thread would deadlock).

The 16.5 dyld cache contains `SBSUIWallpaperService`, `restoreDefaultWallpaper`, `PRSService`,
`prewarmWithCompletion:` and `PBFPosterExtensionDataStoreXPCServiceGlue` (checked in
`sysimage/20F66__iPhone10,3_6/dyld_shared_cache_arm64.*`). t89 ports the two calls into
`userland/src/inject_sb.c` L2271–L2348 (default on, `sb_wallpaper=0` off, log prefix
`hakios: wallpaper:`) and builds the experimental root `4314255b…`. t90 boots it
(`wp4-d1-k`, 1,800 s): both calls are **executed** —

- `hakios: wallpaper: PosterBoard: prewarm asked` (grep -n line 1,579 @06:59:52) and PosterBoard
  is spawned at 06:59:57 by `launch job demand`;
- `hakios: wallpaper: asking for the default wallpaper (SBSUIWallpaperService)` (line 1,724
  @07:00:07.970) and `the default wallpaper asked for` at 07:02:51;

but the PRSService completion **never answers** (`prewarm answered` 0), PosterBoard never calls
`mw_process_allowed` (`grep -c 'PosterBoard is '` = 0), SpringBoard still prints `not today`
twice, and there is no wallpaper texture (host one client, 2,224 frames, main-area black 0.618,
widgets white 1.000). t90 is **PARTIAL**: the driver ran, D1 did not change.

### 3.4 The gate (t92, read-only)

16.5 has the full poster create/persist/publish API, but not the iOS 26 helper classes:

- `PBFPosterExtensionDataStore` (PosterBoard.framework `0x1e29ebb38`)
  `createPosterConfigurationForProviderIdentifier:posterDescriptorIdentifier:completion:`
  (`0x1c0ada8b0`) → `_stateLock_createPosterConfigurationForProviderIdentifier:
  posterDescriptorIdentifier:error:` (`0x1c0ad61b9`) → `_stateLock_applyUpdatesAndIngest…`
  (`0x1c0ad6094`) → `_stateLock_ingestPosterConfiguration…` (`0x1c0ad681c`) →
  `_notifyObserversDidAddConfiguration:` (`0x1c0ad473c`);
- `PRSService` (PosterBoardServices `0x1e2344d58`) also has
  `createAndSelectLegacyPosterConfigurationIfNeededWithCompletion:` (`0x1a368eaf6`) and
  `fetchStaticPosterDescriptorsForExtension:completion:` (`0x1a36912f6`).

The refusal is a device-family check inside
`PBUIWallpaperConfigurationManager -restoreDefaultWallpaperForAllVariantsAndNotify:`
(PaperBoardUI, class_t `0x1e20ba928`, IMP `0x1b0ca7da8`):

```
0x1b0ca7df0  [UIDevice userInterfaceIdiom]      ; selref 0x1d8af9588
0x1b0ca7e00  and  x8, x22, #~4
0x1b0ca7e04  cmp  x8, #1
0x1b0ca7e08  b.ne 0x1b0ca7ed8                   ; iPhone(0) → not-today path
…
0x1b0ca7f14  (not today) → log 0x1b0d1629e ("Hello NOT iPad, trying to restoreDefaultWallpaperForAllVariantsAndNotify? not today.")
```

Only Pad (1) and Mac (5) pass. t92 proposes three repairs in order: **B** spoof the idiom inside
PosterBoard (cheapest, proves/disproves the gate), **A** hook
`createAndSelectLegacyPosterConfigurationIfNeededWithCompletion:` (creates *and selects*),
**C** build a legacy wallpaper from SpringBoard.

### 3.5 The spoof (t93, t94): gate gone, publisher fails

t93 adds `userland/src/inject_pb.c` (140 lines). In the PosterBoard process only, the constructor
replaces `-[UIDevice userInterfaceIdiom]` with a function returning
`UIUserInterfaceIdiomPad` (1); `pb_ipad_idiom=0` disables it; a 500 ms retry loop covers the
class lookup; log prefix `hakios: pb:`. It builds `libhakios.pb.dylib` (67,008 B, md5
`598ff83f…`) and the experimental root `5afce967…`.

t94 (`wp4-d1-l`, 1,800 s) is the first boot with a PosterBoard on the wire:

- spoof line appears once: `hakios: pb: -[UIDevice userInterfaceIdiom] spoofed to
  UIUserInterfaceIdiomPad (1)` (serial grep -n line 4,339); `Hello NOT iPad` **0**, `not today`
  **0** (t90: 2/2);
- `client connected (2 client(s))` + `hello: version 4 pid 37 program "PosterBoard"`; PosterBoard
  executes frame 7–13 with **91/64/64/78/80/67/43** draws and 25 pipelines; it draws 19×180×180
  thumbnails and many small surfaces;
- but its render targets are only `750x1624` ×2,267, `64x64` ×1, `1x1` ×1 — **no ≥1000×2000 and
  no 1125×2436** surface;
- the poster pipeline advances: `Generated legacy poster successfully` (09:24:02), `Stored
  configured properties` (09:24:05), `Successfully built switcher configuration` (09:24:37), all
  for `com.apple.PaperBoard.LegacyPoster:configuration:4E13B5D6-…/0-0`;
- then the concrete blocker: `[Observer] <PBFWallpaperPublisher:0x2808bce70> failed to load
  homeConfig for com.apple.PaperBoard.LegacyPoster:configuration:4E13B5D6-…/0-0` (09:30:58, serial
  line 70,646). `Created new wallpaper scene` 0, `End wallpaper migration` 0,
  `PBFPosterConfiguration` 0, `PRSWallpaper` 0, `SBWallpaperController` 0;
- SpringBoard still prints `wallpaper migration will not run in SpringBoard with Autobahn
  enabled.` (09:22:53) and `no windowScene passed in somehow` (09:25:19), i.e. the scene system
  is not wired;
- side evidence: `WKWallpaperBundleImporter: No wallpaper configuration file found at URL
  'file:///Library/Wallpaper/Wallpapers~iphone.plist'` (09:11:08) and
  `WKDefaultWallpaperManager … falling back to synthesized wallpaper` — the default asset the
  image expects is missing, so even a successful legacy poster may be pure black.

**Verdict: PARTIAL for the experiment, FAIL for D1.** The idiom spoof moved D1 from “gate
refuses” to “configuration created, publication fails”: the remaining gap is the homeConfig the
publisher loads, plus the absent default wallpaper assets, plus SpringBoard's unwired scene.

## 4. D2: the widgets (t87, read-only)

Widget content never goes through metalwire. In t84 the host has exactly one wire client
(backboardd) and no widget-sized texture; the 24 widget extensions that spawn and the 13
`extension checked in as alive` lines never become clients. The chain is:

SpringBoardHome `SBHWidgetViewController` / `SBHWidgetWrapperView` →
ChronoUIServices `CHUISWidgetHostViewController` / `CHUISWidgetScene` → chronod
(ChronoCore/ChronoKit `TimelineProvider`, SwiftUI render) → IOSurface → SpringBoard placement →
backboardd composition.

What fails in chronod: `WidgetRenderer` 0, `timeline` 0, persona failures 688 lines
(`kpersona_find_by_type() failed errno 3`, “Unable to find persona with type 6” — the strongest
hypothesis is the missing **SYSTEM_PROXY persona, type 6**); secondary gaps: `weatherd`,
`calaccessd`, `dataaccessd` binaries+plists exist but never spawn; `locationd`/CoreLocation is
missing entirely while `WeatherWidget` sets `NSWidgetWantsLocation=true`. The desktop widgets
are white placeholders (bbox white 1.000 in every run), independent of D1. Suggested next probes
(t87): applog SpringBoard+chronod, kernel instrumentation of `persona_find_by_type`/
`persona_find_all`, reuse `inject_sb.c`'s `widget_cacontext_at`/`widget_cacontext` to check
CAContext image emptiness, Calendar data seeding; do not look for the bug in the host or shaders.

## 5. The desktop metric correction (WP-4)

t94 exposes something that every earlier WP-4 report got wrong: the Dock/Search/status-bar numbers
were sampled **while the BootUI overlay was still displayed**. backboardd dismisses that overlay
with `[BootUI] updateOverlay: Dismissing overlay: didFinishFirstBoot` at 09:28:44.419 (serial
line 50,748, t94); the next frames (601 onward, 17:28:51 CST) have `dock 0`, `search 0`,
`status 0`, while the wallpaper area stays black (`main_black` 0.618 → 0.590) and the two widgets
stay pure white (1.000). The same dismissal exists in earlier serials: t84 at 10:15:43.558
(line 16,027), t88 at 06:27:41.388 (line 60,527), t90 at 07:25:10.222 (line 52,788) — this is
**not** a stage-59 regression; those boots simply ended (or restarted) soon after.

Consequence: “desktop correct” for WP-4 must report **two windows** — overlay period
(frame 0 … dismissal: dock 66.3, search 13.6, status 73.9 in t94) and live period (after
dismissal: dock/search/status all 0). The earlier claims of a complete desktop (stage 35/39,
`Dock 66.3 / pure white 13,199 / Search 13.59`) describe the overlay period; the live scene has
not yet been shown to produce those elements. This does not change the WP-4 host correctness
(number of executed draws, format coverage, framing) but it changes what “the desktop looks
right” means and must be stated in later reports.

## 6. Boundaries and next steps

Still open, in priority order:

1. **homeConfig** — trace `PBFWallpaperPublisher`'s home-config load (which data-store role/path,
   why it returns nil although the legacy poster was stored) and the minimal fix: either call
   `PRSService -createAndSelectLegacyPosterConfigurationIfNeededWithCompletion:`, or write/select
   the home configuration explicitly. This is the nearest D1 blocker.
2. **Wallpaper assets** — `/Library/Wallpaper/Wallpapers~iphone.plist` (and `Stills/iPhone`) are
   missing; the image falls back to a synthesized wallpaper that is likely pure black. Provide
   the legacy bundle plist or a solid-colour wallpaper and re-run the same experiment to tell
   “publication fails” from “content is black”.
3. **SpringBoard scene** — `no windowScene passed in somehow` plus `Autobahn enabled` suggests
   SpringBoard's scene/window wiring is incomplete; this may also be why the live scene has no
   Dock/Search/status bar (D1 and the metric correction may share this root cause).
4. **D2** — the chronod/persona path; keep it separate from D1 and from the host.
5. **Stage-58 statistics** — the t94 `missing-report` printed `frames 0` while `frames.csv` has
   2,269 rows; recheck the flush order after the stage-58 accounting change (t91).

Not yet done: a boot with a real legacy wallpaper asset; an A/B boot with `pb_ipad_idiom=0` to
separate the spoof's side effects from the rest; any SpringBoard scene instrumentation.

## 7. Evidence index

| experiment | report | key hash |
|---|---|---|
| t86 D1 root-cause recon | `/tmp/t86-evidence/T86-REPORT.md`, 29,440 B | md5 `605f5dbb804e2e5ff4e39c75c8de5c38` |
| t87 D2 recon | `/tmp/t87-evidence/T87-REPORT.md`, 15,936 B | md5 `114e2668bb2dd063e6788861850bb96a` |
| t88 E1 evidence boot | `/tmp/t88-evidence/T88-REPORT.md`, 13,782 B | md5 `e1dbda21…`; serial md5 `b11d8703…` |
| t89 hakios transplant | `/tmp/t89-evidence/T89-REPORT.md` | dylib `86db0a4d…`, root `4314255b…` |
| t90 hakios boot | `/tmp/t90-evidence/T90-REPORT.md`, 60 lines | md5 `8a6249bcf9a7db74c9577de4f8f14679`; serial md5 `99a6687f…`; host log md5 `9571dc7a…` |
| t92 gate recon | `/tmp/t92-evidence/T92-REPORT.md`, 25,227 B | md5 `eec1f3c0746d6ff6f32c73c3adb5397a` |
| t93 idiom-spoof image | `/tmp/t93-evidence/T93-REPORT.md`, 14,646 B | md5 `44cd08afde236cdbb79b92aed4122d0c`; dylib `598ff83f…`; root `5afce967…` |
| t94 spoof boot | `/tmp/t94-evidence/T94-REPORT.md`, 13,177 B | md5 `d951e35dfa3fca0976f84e382b44ba38`; serial md5 `4576b046…`; host log md5 `d9f58626…` |

Delivery images restored after every run: root `a4a6e2ce50e6d0324e26a49ea20fbd84`, system
`49ae3cf4fd53334b475a921eb9915008`, kernel `f62c3d4b852d467f22c4703b8fc620c9`. Variant images
remain only under `mt-work/exp-d1d2g/` (t89), `mt-work/exp-d1d2h/` (t93) and the older
`exp-d1d2*` directories; no variant is on the delivery path.

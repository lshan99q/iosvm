# stage 60: the renderer gets its pixels, the poster gets a data model, and the desktop wallpaper is still the snapshot that never comes

> Status: **measured; D1 still open, D2 untouched.** Stage 60 records the t95–t127 investigation
> chain (boots `wp4-d1-ac` … `wp4-d1-aq`, 2026-11-25/26). Three real walls were broken and one
> false one was found:
>
> 1. **QuartzCore's `default.metallib` was not in the delivered rootfs.** `SBRendererService`
>    aborted in `-[CAContext …]` because `metallib_url != nil` was false; the resource lives only
>    in the IPSW's rootfs DMG and `sysimage/select-system.py` skipped `.metallib` on purpose. The
>    metal plugin now answers `-[NSBundle URLForResource:withExtension:]` for that one bundle, and
>    the renderer renders a real wallpaper scene: 750×1624 frames, four wire clients, zero
>    aborts (`wp4-d1-ac`). That is the first time a SpringBoard-owned wallpaper was drawn through
>    metalwire.
> 2. **The poster data model needed a home configuration.** PosterBoard's `PBUIWallpaperPublisher`
>    failed to load `homeConfig`; the home role is written through
>    `-[PRMutableSwitcherConfiguration ingestNewPosterConfiguration:toBeAssociatedWithHomeScreenConfiguration:]`
>    (owner found by a full-class scan in-process, not statically), and the plugin now hooks it and
>    substitutes a synthetic home configuration when the store passes nil. PosterBoard reaches
>    `hud: _update #1 done (15)` and publishes a configuration.
> 3. **The desktop wallpaper is a replica snapshot, and that request is never answered.**
>    `_SBWallpaperSecureWindow` (level −3) is rooted at `PBUIPosterWallpaperRemoteViewController`
>    with a four-level tree of empty `UIView`s; `PBUIWallpaperViewController` has **zero** instances
>    (the earlier “init at …” lines were our own hook bug), `PBUIPosterWallpaperViewController` is
>    never created, SpringBoard logs `[lock]/[home] Existing snapshot failed to be read. Requesting
>    new one.` and nothing ever answers it.
>
> Two instrumentation bugs also had to be fixed before any of the above could be trusted: a swizzle
> on an inherited `-init` (so every object in the process went through our hook) and a layer dump
> that sent `-UTF8String` to a `UIColor`. Both are in the artifact table below. The delivery image
> (`root a4a6e2ce…`, system `49ae3cf4…`, kernel `f62c3d4b…`) is unchanged and still boots; every
> experiment image stays under `mt-work/exp-d1d2*` and `/tmp/d1d2*-root.apfs`. This continues
> [`docs/stage59-d1-d2-close.md`](stage59-d1-d2-close.md).

## 1. The question

Stage 59 ended with the renderer path broken at `SBRendererService`'s first `CAContext`: the
process was alive, the wire was up, and no pixels ever came out. The question for stage 60 was
narrow: *is the home-screen wallpaper missing because the renderer cannot draw, or because nobody
ever gives it a poster to draw?* The answer, measured: both, in that order — and then a third
layer (the snapshot) that is not a renderer problem at all.

## 2. The renderer wall: a missing `default.metallib`

`SBRendererService` aborted with `-[NSBundle URLForResource:withExtension:]` returning nil for
`QuartzCore.framework` / `metallib`, then `metallib_url != nil` failing inside the
`MetalContext` initialiser. The delivered `root.apfs` and `sysimage/rootfs` carry only
`Info.plist`, `_CodeSignature` and `default.pipelinelib` for QuartzCore; the original IPSW rootfs
DMG (`userland-raw/096-04088-077.dmg`) still has `default.metallib` (1,014,208 bytes, magic
`MTLB`) plus `odt_*.dat`. `sysimage/select-system.py` drops `.metallib`/`.dat` with `SKIP_EXT` to
save RAM, which is where the file went.

Fix, in `guest/metal-plugin/HakiosMetal.m` (`870a45d5…`): swizzle
`-[NSBundle URLForResource:withExtension:]`; when the original result is nil, the bundle path
contains `QuartzCore.framework` and the extension is `metallib`, return
`/redwood/lib/QuartzCore-default.metallib` (the extracted file, md5 `e0c67a32…`). The plugin logs
the substitution, so the line

```
resources: /System/Library/Frameworks/QuartzCore.framework/default.metallib -> /redwood/lib/QuartzCore-default.metallib (1014208 byte(s))
```

is the proof it hit. Result in `wp4-d1-ac`: `SBRendererService` is the **fourth wire client**
(`hello: version 4 pid 134 program "SBRendererService"`), creates its 750×1624 MiniIOSurface, and
draws the orange/black stripe wallpaper (`/tmp/ac-shm-latest.png`); `QEMU_INSTANCE=3
screendump.sh` shows the same content on the kernel framebuffer. No SIGABRT, no assertion, no
`doesNotRecognizeSelector` in the whole serial log.

Efficiency note for the next stage: the two `.dat` files stayed out on purpose. Only the
`metallib` matters here; the renderer does not ask for the rest.

## 3. The data model: `homeConfig`

With the renderer alive, PosterBoard still failed at `PBFWallpaperPublisher`: the switcher
configuration had no home role. The store class is `PRMutableSwitcherConfiguration`; the ingest
call is

```
-[PRMutableSwitcherConfiguration ingestNewPosterConfiguration:toBeAssociatedWithHomeScreenConfiguration:]
```

found by scanning all 84796 classes in-process (`sb_selector_owners`, t107). The plugin
(`inject_pb.c`, t113/t124) hooks it and, when the home argument is nil and spoofing is on,
substitutes a synthetic configuration (`homecfg_make()`), logging `homeRole: ingest #%lu poster %s
home %s`. The publisher then runs: `hakios: pb: hud: _update #1 start` → `_update #1 done (15)` →
`hakios: pb: hud: publisher init` and the `PRSWallpaperPublisher` observer connects.

This is what the `homeConfig` failure in stage 59 was: not a missing file, but a nil argument the
store passes on first migration because no home role was ever written.

## 4. Why the injections kept “not loading”

Three separate mechanisms conspired, and all three were measured:

1. **`inject.conf` is line-grouped**, not semicolon-grouped. `userland/src/init.c` joins the lines
   with `;` before `sysctl kern.exec_inject`; the kernel `exec_inject_env()` then splits on `;`
   and matches by program basename. One program can appear in two groups, but in practice only the
   first injected library is reliable (`inject_sb.c:2957` had already recorded this).
2. **cainject's crash-PC scan is a full-class walk.** `cai_scan_imp()` iterates
   `objc_copyClassList()` and compares IMPs, which costs 20+ minutes per process on this TCG CPU.
   It is now behind `/redwood/etc/ca-inject-scan` (absent → `crash-PC scan off`), which removed the
   scan from every process. The scan never mattered for the wallpaper path — the two PCs it resolves
   are crash-report decoration.
3. **cainject's Metal pipeline-descriptor patch loads Metal.framework**, which costs another 20+
   minutes inside PosterBoard. Skipping cainject in PosterBoard entirely (t111) made the migration
   stall worse, so the final arrangement keeps the kernel injection but lets cainject load the
   poster board helper itself: `ca_inject.m`'s `cai_load()` ends with
   `dlopen("/redwood/lib/libhakios.pb.dylib")` when `getprogname()` is `PosterBoard`. That is one
   injected library per program (`libcainject`), with `libhakios.pb` pulled in by its constructor.

Measured cost of the chain in `wp4-d1-aq`: PosterBoard spawned at 13:31, `crash-PC scan off` right
after, cainject's Metal load finished at 13:57 (`render pipeline descriptor function accessors
hooked: 3`), `hakios: pb: constructor in PosterBoard` one line later, publisher hooks at 14:11,
migration at 14:13, wallpaper bundle import at 14:22. Every step is 20–50× slower than the same step
in a native boot; a poster that took 73 s in `wp4-d1-an` takes 90 minutes here.

## 5. The assertion that kept killing SpringBoard

`SpringBoard` aborted three times in this stage (`ak` 07:50, `an` 11:10, `ap` 13:11) with

```
CoreAnimation: CA_ASSERT_MAIN_THREAD_TRANSACTIONS is set and an implicit transaction wasn't created on a main thread
```

The flag is not ours: it is in Apple's own
`LaunchDaemons/com.apple.SpringBoard.plist` `EnvironmentVariables` (a one-key dict) and in the
shared cache. It turns any implicit CoreAnimation transaction created off the main thread into a
hard abort. The trigger in these boots is the image's autopilot
(`root/etc/rc` + `LaunchDaemons/com.iosvm.layers.plist`: `rwtouch wait 900 … button home … swipe …
tap 0.607 0.693 … zigzag`) racing our switcher hooks. Experiment images now drop the key
(plistlib rewrite) and disable the autopilot (`rc` lines commented, `layers.plist` keeps only the
unlock notification). With both gone, SpringBoard survived all probes in `aq` long enough to reach
the migration.

Worth keeping in mind: the flushes this abort guards against are real CA misuse, so the key should
stay in shipping images; it is an experiment-only removal, recorded here so nobody “fixes” it in
the delivery plist.

## 6. The snapshot wall (current blocker)

Nothing draws the desktop wallpaper because the thing that should draw it never gets content:

| fact | value |
|---|---|
| wallpaper windows | `_SBWallpaperSecureWindow` level −3, `SBHomeScreenWindow` level −2 |
| secure window root VC | `PBUIPosterWallpaperRemoteViewController` |
| its view tree | four nested bare `UIView`s, frame 0,0 375×812, all `contents no` |
| `PBUIWallpaperViewController` instances | 0 (t116 hook fix; earlier counts were our bug) |
| `PBUIPosterWallpaperViewController` | never created |
| `PBUIWallpaperWindowScene` / `Created new wallpaper scene` | 0 |
| SpringBoard log | `[lock] Existing snapshot failed to be read. Requesting new one. error=<NSError: 0x283468db0 …>` and the same for `[home]`, then silence |
| PaperBoardUI strings for that path | `[%@] Snapshotting poster...`, `[%@] Snapshot capture failed. Error=%@`, `[%@] Created new poster scene: %@`, `Failed to create new poster scene.`, `PBUISnapshotReplicaView`, `PBUIPosterReplicaSnapshotProviding`, `_performSnapshotOnQueue:completion:`, `_scheduleSnapshotIfNeeded` |
| measured | `Created new poster scene` **0**, `Snapshotting poster` **0**, `Snapshot capture failed` **0** |

So the request stops before the scene is even created: the home wallpaper is defined as a replica
snapshot of the lock poster, and SpringBoard never gets one. `_SBWallpaperSecureWindow` stays an
empty remote view controller; the desktop composites the black behind the icons.

The layer probe (`sb_window_layers`, t125) also rules out the obvious alternative: the home screen
window is `opaque 0 bg nil`, its root view layer is `opaque 1 bg nil contents no`, and hiding it
(`sb_hide_home_window`) shows black plus the status-bar strip, not a wallpaper. The secure window's
window layer is `opaque 0` with a grey color, its root view `opaque 1 contents no` — empty, not
occluded.

## 7. Instrumentation bugs fixed on the way (both ours)

1. **`hook_method()` swizzled inherited methods.** `class_getInstanceMethod` returns the inherited
   implementation when the class does not implement the selector, and
   `PBUIWallpaperViewController` does not implement `-init`. The plugin therefore replaced
   `NSObject`'s `-init` for the whole process, recording keyboard windows as “wallpaper view
   controllers”. Fixed by walking `class_copyMethodList(cls)` and leaving the method alone
   otherwise (`hook %s %s: not implemented by the class itself; left alone`).
2. **`sb_text()` sent `-UTF8String` to a `UIColor`.** The nightly layer dump crashed SpringBoard
   with `-[UICachedDeviceWhiteColor UTF8String]: unrecognized selector`. It now checks
   `respondsToSelector:` and falls back to `-description`. (Same shape as the stage-44 lesson:
   never assume a `-description`-like selector exists on whatever a tree walk finds.)

Both bugs had produced *plausible* data (a VC that “exists but never loads”), which is why they cost
two boots.

## 8. Timers: the global queue does not fire here

`dispatch_after_f` on the global queue never runs in this guest — not in SpringBoard, not in
PosterBoard (`sb_selector_owners` at 60 s took the one worker for 20 minutes its scan needed, so an
ask queued behind it starved; but after moving to a later slot the global-queue timers still never
fired at all). Main-queue function-pointer timers fire on time (20 s hooks, 2000 s vc state, 2400 s
ask). All probes now schedule on the main queue; the poster store scan in `inject_pb.c` is the one
remaining global-queue user and is unused in the current boot.

Also measured: `+[SBSUIWallpaperService restoreDefaultWallpaper]` must not be called on the main
queue. It is answered from SpringBoard's own main thread, so a main-queue call waits on itself
(`dada`'s stage-57 note). In `aq` the ask at 2400 s returned (`the default wallpaper asked for`)
because it ran from a main-queue timer that then yielded; the call itself must stay off the main
queue, which is where `wallpaper_restore_late` dispatches it.

## 9. Boot recipe (standardised this stage)

`mt-work/tools/boot.sh TAG IMG [SECS] [EXTRA]` does everything the hand-rolled boots did:

1. refuse to start if 4474/9111/9210 are taken;
2. `cp IMG userland/root.apfs` (the root volume is a **ramdisk**: guest writes never persist, so a
   poster store has to be baked into the next image with `poster-bake.py`);
3. rewrite `vars.fd` to 64 MB of `0xFF`;
4. start `metalwire_host` (9111, `--csv /tmp/pb-probe-TAG/host.csv --out
   /data3/iosvm-tmp/TAG-frames --png-every 250 --missing-report … --byte-check`) and
   `host_glwire` (9210 `--no-panel`) pinned to cores 29,30 with the lavapipe ICD;
5. start `watch.py` to keep the latest `/dev/shm/metalwire-frame` as a png;
6. run `run-guest.sh TAG SECS` with
   `MW_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1 launchd_disable_all_timeouts=1 applog=1 applog_name=SpringBoard,PosterBoard,backboardd,SBRendererService poster_dump=1 $EXTRA"`.

Two facts about the root volume and one about QEMU:
- `run-darwin.sh` loads the root as a ramdisk (`mkpayload.py … ramdisk=$ROOT`), so §“keep the data
  volume between boots” is not possible without changing the payload format; `poster-bake.py` is
  the workaround (it parses the `hakios: posterfile: begin/end` base64 blocks the plugin emits with
  `poster_dump=1` and writes them back into an image tree).
- The shm frame header is `[0]=0x57464c47` (“GLFW”), `[4]=width`, `[5]=height`, `[6]=byte count`,
  `[8]=crc`, `[9]=sequence`; pixels start at offset 40 and are BGRA (swap for RGB png).

## 10. Artifact table

| what | md5 | note |
|---|---|---|
| `libhakios.sb.dylib` (t127) | `762626e5da27ac617a9413bc3dfe7659` | probes rescheduled to 6000–6900 s for slow posters |
| `libhakios.pb.dylib` (t124) | `0ac3e451fd4bff5da7a97e49ee229b7f` | adds deferred cainject dlopen |
| `libcainject.dylib` | `b953ac0aff5432271c5754c2fe5937bd` | scan opt-in + PosterBoard pulls the pb helper |
| `HakiosMetal.m` | `870a45d52070da7394b9756b34eba533` | metallib override |
| QuartzCore `default.metallib` | `e0c67a3256778108d82e61d4f1f0a2b8` | from the IPSW rootfs DMG |
| experiment image `d1d2ap` | `4d5900baf1f11c34e077f0486772228b` | t121 + pb t122 + sb t123 |
| experiment image `d1d2as` | `1446e6dded5592b67199f810e4923003` | CA env removed, single-library chain, poster_dump |
| experiment image `d1d2au` | `b4dab80fd2937de3208f1ac0c9e9a4df` | d1d2at + t127 (late probes, 2 h boot) |
| boot `wp4-d1-ac` serial | `7d4eec34ede67e702a663244df71c7a8` | renderer pixels, 4 clients, no abort |
| boot `wp4-d1-an` serial | `b33b9cbcd22311b293caa7d6f2682427` | first poster + switcher, empty snapshot |
| boot `wp4-d1-aq` serial | `e1bafa46c5c8c1f1d30d085f94dd3d5a` | 90 min to poster, snapshot still unanswered |

Delivery images are untouched: `userland/root.apfs` is restored to `a4a6e2ce50e6d0324e26a49ea20fbd84`
after every experiment.

## 11. Next

1. `wp4-d1-ar` (2 h, d1d2au) runs the poster path again with probes at 80–115 min and the CA
   assertion gone; the question is whether `PBUIPosterWallpaperRemoteViewController` gets a scene
   and whether the `[lock]/[home]` snapshot request is answered once the poster exists long enough.
2. If the snapshot request stays unanswered, the next lever is the poster *scene*: PaperBoardUI
   wants `PBUIWallpaperSceneSessionRoleWallpaper` / `com.apple.PaperBoard-wallpaper` and
   `_performSnapshotOnQueue:`. Either SpringBoard never creates the scene, or the remote view
   controller never asks for it. The t126 probe (`sb_poster_remote_probe`) dumps the remote
   controller's methods and the controller's poster properties at that moment; its output decides
   which side to instrument next.
3. D2 is deliberately untouched: `chronod` has no timeline and no `WidgetRenderer` surface, and the
   persona type 6 calls fail (`kpersona_find_by_type() failed with ret -1 errno 3`), which is the
   same kernel gap seen in `aq`/`an`.

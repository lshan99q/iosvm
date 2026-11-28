# stage 61: the D1 desktop wallpaper is a live legacy poster, not the snapshot replica

> Status: **measured; D1 black-screen closed by direct frames; production content still open; replica
> snapshot path still open.** Stage 61 records the t143–t157 chain and the two clean boots `wp4-d1-cc`
> (bz8) and `wp4-d1-cd` (bz9/t157). The desktop wallpaper is no longer black: a real image is written
> through `PBUIWallpaperConfigurationManager -setWallpaperImage:wallpaperOptions:forVariants:` into
> `LockBackground.cpbitmap` (4,885,081 B), legacy migration turns it into a poster, and frames 6400/6500
> show a full-screen non-black wallpaper (`y0–400` dark 0.00 %, `first_nonblack_row=0`). This is **not**
> the replica-snapshot chain stage 59/60 were chasing: `Snapshotting poster=0`,
> `Created new wallpaper scene=0`, and the `[lock]/[home]` refresh still returns
> `NSCocoaErrorDomain -1` with an empty `userInfo`. The direct-frame criterion is used and the
> structural deviation is recorded in §4, not hidden as a pass. This continues
> [`docs/stage60-d1-renderer-and-poster.md`](stage60-d1-renderer-and-poster.md).

## 1. Result in one paragraph

`wp4-d1-cd` (bz9/t157, 2026-11-28 12:54:26 → 14:57:27, serial
`/data3/iosvm-tmp/wp4-d1-cd/serial.log`, 606,742 lines, md5 `d6cf25ffce76af08e380b2584975dad2`,
sha256 `f2de79fba8473543acc5b5fbce498f2cf6e591d2400c85d24048d294461419e4`) is the clean acceptance
boot: `frame-6400.bin` sha256 `70c285855c5e44532744c065c4607b8c08a0f7cca85d0adbd084a422c393576b`
and `frame-6500.bin` sha256 `1fe7ef7be71b4024d8063965ba9b26d84cec7a49dd943feb7c9e9ef72a8e8ec7`
both have `y0–400` dark 0.00 % and `first_nonblack_row=0`. `grep -c 'Terminating app'` = 0 and
`grep -c 'userabort'` = 0 (the five SIGABRT/SIGSEGV hits are handler-install lines 1637/1896/148027/
250279/433515). The same content criterion is already met in `wp4-d1-cc` (bz8) with
`frame-6400.bin` `2a3c5f02c624ba4a89ecd78c36d5a0cc369e5c021e6c6839e8918be246f608c2` and
`frame-6500.bin` `1e2ea181d673312fb8efc81fa7e2bbc0e46177c6f24ca95c71c8deee3ebb6b42`; cc is not the
clean acceptance boot because its old scene probe crashed SpringBoard at el 6586 (fixed by t157).

## 2. Root cause chain

1. **The wallpaper cache held 203 B empty cpbitmaps, so there was no pixel source.** The four
   `/var/mobile/Library/SpringBoard/{Lock,OriginalLock,Home,OriginalHome}Background.cpbitmap` files
   are the legacy wallpaper cache. The empty file is 64 B header + 61 B colorSpace plist + 50 B info
   plist (`2.0`) + 28 B trailer, last four bytes `a2 43 b5 dc`; the pixel payload is absent. Evidence:
   `/data3/iosvm-tmp/d1d2bw-content-source.md` (20,553 B, md5 `a867d8cb17a913482083d725ec788c94`)
   `:11` and `:12`. In `wp4-d1-cb` the 3000 s probe found the files **absent**:
   `/data3/iosvm-tmp/wp4-d1-cb/serial.log:192780–192783` (`...LockBackground.cpbitmap absent errno 2`,
   `...OriginalLockBackground.cpbitmap absent errno 2`, `...HomeBackground.cpbitmap absent errno 2`,
   `...OriginalHomeBackground.cpbitmap absent errno 2`).
2. **The poster store's three directories were missing from the data-volume template.** `var.skel`
   had no `/private/var/mobile/Library/{PosterBoard,PosterBoard/Extensions,Wallpaper}` lines; t143
   added a runtime `mkstore` at 6100 s and t46 (commit `65fcd7e7b313e2d120748791387ed5140200b1b8`)
   appended the three `printf 'd\t755\t501\t501\t...'` lines to `userland/build.sh` (:400–402 in the
   current tree). `wp4-d1-cc` then lists all seven entries during `posterdump`:
   `/data3/iosvm-tmp/wp4-d1-cc/serial.log:290894–290899`, `:291018`
   `hakios: posterdump: scan done 7 entri(es)`.
3. **Legacy migration ran, but wrote no content while the source was empty.** In `wp4-d1-cb` migration
   reached `Generated legacy poster` only after the content source existed; in `wp4-d1-cc` the store
   configuration's `contents` directory still lists **0** entries
   (`/data3/iosvm-tmp/wp4-d1-cc/serial.log:520707–520714`, `cfg lock list count 0`;
   `:526284–526288`, `cfg home list count 0`) even though the live poster is shown from the written
   cpbitmap. The empty cache is the black-screen cause; the empty store is a separate structural gap.
4. **The replica snapshot is a second, independent failure.** SpringBoard still asks for
   `[lock]/[home]` snapshots (`cc:520713`, `cc:526288`) and gets
   `NSCocoaErrorDomain -1` with `userInfo __NSDictionary0` (`cc:532404/532424`;
   `cd:530615/530616`, `cd:536105/536106`). `Existing snapshot failed` appears twice
   (`cd:421938/421943`, `PaperBoardImageEncoding 3 / NSCocoa 260 / POSIX 2`). This layer was not
   needed to remove the black screen and remains open.

## 3. Fix chain (what actually made the wallpaper non-black)

1. **t146 — seeding PNG bytes as `.cpbitmap` failed.** `t146` (commit `eb79c9b9576ba0e07d099fcfab5496c27fb9a2b4`,
   patch `/data3/iosvm-tmp/t146-inject_sb.patch`, 4,817 B, md5 `d27b41b8f404246bab52c9d54c2ee0c4`)
   wrote `/redwood/var/d1test.png` into the four cpbitmap paths at 500/1500/2500/3000 s.
   `wp4-d1-bx` rejects it: serial `218172` `Error deserializing wallpaper image:
   Error Domain=CPBitmapErrorDomain Code=6 "Invalid magic in trailer"`, 40 such lines through
   `541681`; the ImageIO fallback never produced a wallpaper. E6-1 is closed negative.
2. **t152 — a real `UIImage` through ImageIO.** Commit
   `a99359feadeff4a40b4966bf0866180141a3edb7`, patch `/data3/iosvm-tmp/t152-inject_sb.patch`
   (9,698 B, md5 `24a5ea14aeda758e5f0ac0932b9034a8`), image `/tmp/d1d2bz5-root.apfs`
   (6,619,136 B, md5 `51b73e6feef7d083bc5b0a0cf864aee4`). The setwp call now receives
   `imageWithCGImage:scale:orientation:` instead of raw PNG data.
3. **t154 — real `PBUIWallpaperOptions`.** Commit
   `0f7e9a72845ca8126b48cc0bb4cbde5942cf2043`; patches
   `/data3/iosvm-tmp/t154-inject_sb.patch` (10,229 B, md5 `18daeed12ee723c42eba78b9329956ee`) and
   `t154-inject_pb.patch` (14,568 B, md5 `0fb727c6dca3f1135cbd6db0351056be`); image
   `/tmp/d1d2bz6-root.apfs` (6,627,328 B, md5 `c6d80bd43ae3b0d7eb777c339f707f88`). `wp4-d1-cb`
   still stops at `setwp skip size ... size 0x0` (`cb:8731/8732`) because the size precheck sees 0×0.
4. **t155 — all three setter shapes, 5-arg first, no size precheck.** Commit
   `69bde6f02dbb1d15fd31df95dac09017f978a15d`; patch
   `/data3/iosvm-tmp/t155-inject_sb.patch` (8,554 B, md5 `76f37b3ccfc1a4ea1f6ef5c6cefb9208`); image
   `/tmp/d1d2cc-root.apfs` (6,627,328 B, md5 `4257b8a1fafc7aa29e88219ab7e61061`). Current source:
   `userland/src/inject_sb.c:3969` `sb_setwp_once`, 5-arg send at `:3998–4002`
   (`setWallpaperImage:wallpaperOptions:forVariants:`), call at `:4064`.
   `wp4-d1-cc` serial `8889`:
   `hakios: sbwp: setwp2: try1 sel=setWallpaperImage:wallpaperOptions:forVariants: sent=1 ret=1 src manager`;
   try2/try3 `sent=0 ret=-1` (`8890/8891`). The 3000 s probe then sees real files:
   `196901`/`196902` `LockBackground.cpbitmap`/`OriginalLockBackground.cpbitmap` **4,885,081 B**,
   mtime 1795835391/1795835392 (11:09:51/52); `196903`/`196904` home absent errno 2.
5. **The legacy migration consumes the cache.** `wp4-d1-cc`: `226931` `Start wallpaper migration`
   el 3425 → `279487` `Generated legacy poster successfully` el 4016 → `281466` `Successfully built
   switcher configuration` el 4042 → `295354` `hud: _update #1 start` el 4175 → `354352`
   `hud: _update #1 done (15)` el 4776. Frames 6400/6500/6480 show the live poster full-screen.
6. **t157 — scene callback type fix.** Commit
   `fd943e50c7ba9a3f4331ecc0c0d494367c03a732`; patch `/data3/iosvm-tmp/t157-inject_sb.patch`
   (9,663 B, md5 `48cef4572e20d896f7529d8287200215`); image `/tmp/d1d2dd-root.apfs`
   (6,627,328 B, md5 `371de931f4a896975bc3eec13bd70414`). `wp4-d1-cd` scene:
   `538355` `scene: asking FBScene options nil traits nil` → `538529` `scene: snapshot image nil` →
   `538530` `scene: cb arg class nil`; no `+[_FBSSnapshot domain]`, no `Terminating app`.

## 4. Criterion matrix (no unmarked “pass”)

| # | criterion | verdict | evidence |
|---|---|---|---|
| 1 | 6400/6500 frame wallpaper non-black | **passed** | cc: `frame-6400` `2a3c5f02…`, `frame-6500` `1e2ea181…`; cd: `frame-6400` `70c28585…`, `frame-6500` `1fe7ef7b…`; cd `y0–400` dark 0.00 %, `first_nonblack_row=0`; cd `y0–400` mean(B) 99.22 / full-frame mean 130.28 (6400), 112.99 / 131.46 (6500). |
| 2 | Snapshotting poster / Created new wallpaper scene = 0 | **deviation, not passed** | Both are 0 in cc/cd (`cc`/`cd` serial grep); PosterBoard has no window/VC (`cc:279102–279106`, `e7 windows 0` / `injected 0` / `snapshot no vc` at `279342`). The black-screen criterion is replaced by **direct frames**, and this row stays marked “偏离”. The replica refresh is still failing (see §2.4). |
| 3 | no userabort / Terminating app | **passed on cd** | `grep -c 'Terminating app'` = 0, `grep -c 'userabort'` = 0; five SIGABRT/SIGSEGV hits are handler-install lines 1637/1896/148027/250279/433515. `cc:532419/532423` (old scene probe at el 6586) is recorded as pre-t157, not as a clean-boot result. |
| 4 | delivery root restored | **passed** | `/home/lshan/DSH-desktop/mt-work/hakios/userland/root.apfs` 6,238,208 B, md5 `a4a6e2ce50e6d0324e26a49ea20fbd84`, sha256 `dc4100d3841a05d7e7ab4ca89c0119a4cb02c8170fbd0a292e95afd7ab0c0a67`; system `49ae3cf4fd53334b475a921eb9915008`; kernel `f62c3d4b852d467f22c4703b8fc620c9`; boot.sh `0cc8458cb4906ab3ddbe2edafbe6f6a4`. Restore evidence `/data3/iosvm-tmp/d1d2slot-restore.md` (75 lines, md5 `80294ef3caeb9fcaa3914285e008b5c2`). |
| 5 | md5 ledger | **passed** | §7 lists every image, dylib, patch, commit, serial and report with md5/sha256. |

## 5. Known defects and limits (all measured)

1. **The content source is a test card.** `/redwood/var/d1test.png` is 14,368 B, md5
   `ce70b79d8719f1ebb41c08097d65ab48`; it is a color-block test image, not a publishable wallpaper.
   `frame-6300` in cc is that card under the black band (top `y0–141`), and cd `frame-6300`
   (`dc64de948f24d3586b37b1ebdda66652d47ec4f24daee57dbc89b8c91e7fb4e8`, `y0–400` mean 147.04,
   dark 38.01 %, unique 58, `first_nonblack_row=142`) is the same pre-handover state.
2. **Top black band on the home screen after the handover window.** cd `frame-6650`
   (`1e96a3e8decb817cea5fc552c908aeaefc546ad08774e0176188f9293a49645e`) returns to
   `y0–400` dark 38.01 %, unique 58, `first_nonblack_row=142` — the same picture as cc 6300. 6400/6500
   are the clean full-screen frames; 6650+ is a known regression window, not counted as D1 pass.
3. **Cold boot is slow on TCG.** cd timeline: migration start el 3410, `Generated legacy poster`
   el 4009, switcher el 4024, frame-6300 at el ~6260 still has the band, frame-6400 at el ~6360 is
   full-screen. Wallpaper is visible only after the legacy migration finishes (~6300 s in these
   7200 s boots).
4. **The replica snapshot chain is still unanswered.** refresh asks `cc:520713` / `cc:526288`,
   errors `cc:532404/532424` and `cd:530615/530616`, `cd:536105/536106`; `Existing snapshot failed`
   `cd:421938/421943`. Backgrounding/transition snapshots can still flash black; this is a separate
   open item from the static wallpaper.
5. **The delivered image is still the old one.** Restoring the slot to `a4a6e2ce…` means the tested
   t157 dylib is not in the delivered rootfs until the next delivery build merges it.

## 6. Delivery checklist (for t12; no git in this task)

Files to commit:
- `iosvm-metal/docs/stage61-d1-wallpaper-closed.md` (this file).

Next delivery build must merge:
- `libhakios.sb.dylib` t157 (`3c12290b3c0242bc4165cb8dff668623`, 184,064 B) — patch
  `/data3/iosvm-tmp/t157-inject_sb.patch` (md5 `48cef4572e20d896f7529d8287200215`), commit
  `fd943e50c7ba9a3f4331ecc0c0d494367c03a732`.
- `/redwood/var/默认壁纸.png` — replace the d1test test card with the production wallpaper before
  delivery; d1test md5 `ce70b79d8719f1ebb41c08097d65ab48` must not ship.
- `userland/build.sh` var.skel three lines (t46 commit `65fcd7e7b313e2d120748791387ed5140200b1b8`):
  `/private/var/mobile/Library/PosterBoard`, `.../PosterBoard/Extensions`, `.../Wallpaper`.
- Keep the t152/t154/t155/t156/t157 patches together; the sb dylib above already contains them.

Do not run another boot before the delivery step: `mt-work/tools/boot.sh:26` copies the experiment
image over the delivery slot, which is why t38 had to restore `a4a6e2ce…`.

## 7. Ledger

### 7.1 Boots

| boot | image (md5) | serial | md5 / sha256 | report |
|---|---|---|---|---|
| `wp4-d1-bx` (t147) | `73475039…` | `/data3/iosvm-tmp/wp4-d1-bx/serial.log` | 47,893,780 B / 610,116 lines; md5 `0960141f4739fee556e4c4c04d0e31bd`; sha256 `346af92f6a7a08469eacf57f5c3d8e224df850535cbdd6e83b42eab969c5a1dc` | `/data3/iosvm-tmp/wp4-d1-bx/t7-report.md` (md5 `b0163a8b7dc7d24386b7272b27e8f302`) |
| `wp4-d1-cb` (bz6/t154) | `/tmp/d1d2bz6-root.apfs` `c6d80bd4…` | `/data3/iosvm-tmp/wp4-d1-cb/serial.log` | 44,098,763 B / 556,467 lines; md5 `fe0c59df3ab6b38dc7a0f999744c1f06`; sha256 `a168fb0264d39dd43df1831e9da8454cc5f16f54817062f9c1a9dc30a7cd1f9a` | t36 summary |
| `wp4-d1-cc` (bz8) | `/tmp/d1d2dc-root.apfs` `69e13768…` | `/data3/iosvm-tmp/wp4-d1-cc/serial.log` | 45,994,873 B / 580,681 lines; md5 `d83a9452969948c4a9a8035d3cc9d772`; sha256 `20a0f155dbabf526a3aa5725814fe7be7efc0e8363304f921102f0bad7ffe540` | `/data3/iosvm-tmp/wp4-d1-cc/t41-report.md` (md5 `03e52b4b4bb5fbbe8253a0b2fbd27165`) |
| `wp4-d1-cd` (bz9/t157) | `/tmp/d1d2dd-root.apfs` `371de931…` | `/data3/iosvm-tmp/wp4-d1-cd/serial.log` | 47,489,442 B / 606,742 lines; md5 `d6cf25ffce76af08e380b2584975dad2`; sha256 `f2de79fba8473543acc5b5fbce498f2cf6e591d2400c85d24048d294461419e4` | `/data3/iosvm-tmp/wp4-d1-cd/t48-report.md` (md5 `a2ae37c3423a0a5fb37b95fa25c5b57a`) |

### 7.2 Commits

| task | commit | subject |
|---|---|---|
| t146 | `eb79c9b9576ba0e07d099fcfab5496c27fb9a2b4` | seed PNG test picture into cpbitmap at 500/1500/2500/3000 s |
| t152 | `a99359feadeff4a40b4966bf0866180141a3edb7` | setwp through a real UIImage, one main-queue call at 900 s |
| t154 | `0f7e9a72845ca8126b48cc0bb4cbde5942cf2043` | options from manager/ivars/defaultOptions; E7 snapshot probe |
| t155 | `69bde6f02dbb1d15fd31df95dac09017f978a15d` | all three setwp setters; option property dump |
| t156 | `4c4eff67982c8719c325fa7691c3da3704b019c8` | E7 ticks at init+300/500/900 s |
| t157 | `fd943e50c7ba9a3f4331ecc0c0d494367c03a732` | scene callback is an IOSurface; paint at 6600 s |
| t46 | `65fcd7e7b313e2d120748791387ed5140200b1b8` | var.skel poster store directories |

### 7.3 Images and dylibs

| artifact | size | md5 |
|---|---|---|
| `/tmp/d1d2bw-root.apfs` (t146) | 6,615,040 | `32d17775e5f604c81fd9aa74976c17dd` |
| `/tmp/d1d2bz5-root.apfs` (t152) | 6,619,136 | `51b73e6feef7d083bc5b0a0cf864aee4` |
| `/tmp/d1d2bz6-root.apfs` (t154) | 6,627,328 | `c6d80bd43ae3b0d7eb777c339f707f88` |
| `/tmp/d1d2cc-root.apfs` (t155) | 6,627,328 | `4257b8a1fafc7aa29e88219ab7e61061` |
| `/tmp/d1d2dc-root.apfs` (t155+t156) | 6,627,328 | `69e13768b54aa9c7938ea5af71595019` |
| `/tmp/d1d2dd-root.apfs` (t157) | 6,627,328 | `371de931f4a896975bc3eec13bd70414` |
| `libhakios.sb.dylib` t155 | 166,816 | `0dcd8d304c1e71414933256f14695700` |
| `libhakios.pb.dylib` t156 | 88,912 | `4f529b278760c1e409937ea0758ffe1d` |
| `libhakios.sb.dylib` t157 | 184,064 | `3c12290b3c0242bc4165cb8dff668623` |

### 7.4 Patches

| patch | size | md5 |
|---|---|---|
| `t146-inject_sb.patch` | 4,817 | `d27b41b8f404246bab52c9d54c2ee0c4` |
| `t152-inject_sb.patch` | 9,698 | `24a5ea14aeda758e5f0ac0932b9034a8` |
| `t154-inject_sb.patch` | 10,229 | `18daeed12ee723c42eba78b9329956ee` |
| `t154-inject_pb.patch` | 14,568 | `0fb727c6dca3f1135cbd6db0351056be` |
| `t155-inject_sb.patch` | 8,554 | `76f37b3ccfc1a4ea1f6ef5c6cefb9208` |
| `t157-inject_sb.patch` | 9,663 | `48cef4572e20d896f7529d8287200215` |
| `t46-build-var-skel.patch` | 1,079 | `5f3cd33f4d0a8a77bb191d79332e90ea` |

### 7.5 Primary frames

| boot | frame | sha256 (bin) | y0–400 dark | first_nonblack_row | note |
|---|---|---|---|---|---|
| cc | `frame-6400.bin` | `2a3c5f02c624ba4a89ecd78c36d5a0cc369e5c021e6c6839e8918be246f608c2` | 0.00 % | 0 | pass |
| cc | `frame-6500.bin` | `1e2ea181d673312fb8efc81fa7e2bbc0e46177c6f24ca95c71c8deee3ebb6b42` | 0.00 % | 0 | pass |
| cd | `frame-6400.bin` | `70c285855c5e44532744c065c4607b8c08a0f7cca85d0adbd084a422c393576b` | 0.00 % | 0 | clean pass |
| cd | `frame-6500.bin` | `1fe7ef7be71b4024d8063965ba9b26d84cec7a49dd943feb7c9e9ef72a8e8ec7` | 0.00 % | 0 | clean pass |
| cc | `frame-6300.bin` | `83e320708dd43670468fc48a11098e984755ed0a590e4a02d9ff39faa4b11935` | 38.01 % | 142 | pre-handover band |
| cd | `frame-6300.bin` | `dc64de948f24d3586b37b1ebdda66652d47ec4f24daee57dbc89b8c91e7fb4e8` | 38.01 % | 142 | pre-handover band |
| cd | `frame-6650.bin` | `1e96a3e8decb817cea5fc552c908aeaefc546ad08774e0176188f9293a49645e` | 38.01 % | 142 | post-handover regression |

## 8. Next

1. Replace d1test with the production wallpaper and rebuild the delivery image with the t157 dylib,
   t46 var.skel lines and the default wallpaper asset; then one confirmation boot (after the
   delivery slot copy is understood) checks the wallpaper and the 6650+ band.
2. Keep the replica-snapshot path open: refresh `NSCocoaErrorDomain -1`, empty `userInfo`, and
   `Existing snapshot failed` are unchanged. Background/transition flash-black is not fixed.
3. D2 is untouched; this stage only closes the D1 static-black-screen branch.

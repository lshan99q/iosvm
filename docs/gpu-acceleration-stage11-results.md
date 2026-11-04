<!-- Same document as /home/lshan/dsh.MD/IOSVM-GPU加速第十一阶段.md; change one and change the other -->

# IOSVM GPU acceleration — stage 11 results

> 2026-11-04 13:00–. P1'''' has its evidence chain; P2''''/P3''''/P4'''' are placeholders for the captain.
> This file records the numbers measured, the verdict on each decision, and the line it got stuck at.
> Goals and criteria are in `gpu-acceleration-stage11-plan.md`.

---

## 0. One-paragraph status

| criterion | verdict | numbers |
|---|---|---|
| P1'''' — where the picture went | **the stage-10 premise "W1.5+W3 is really black" is falsified**: the rows are written into the display's surface, the display driver reads that surface, the screen follows it | 5,362 frames: 5,362 × `flush -> 2` and 5,362 × `in-place rows`; guest surface hash == host picture hash on 5/5 sampled rows, frame 1 … frame 5300 |
| P1'''' — what "black" was | **two real mechanisms, neither of them W3**: (1) the kernel zeroes the panel when the bottom layer is taken away; (2) a swap may present a panel-sized surface that `frame_end` has not written | `swap 3 (#2): surfaces 0 0 0 0, layers set 0x1 cleared 0x1` → `bzero(fBase…)`; `swap 2 layer 0: surface 2` while the guest wrote surface 1 |
| P0'''' — the picture on screen | **the transport works; the 300 KB criterion cannot be met** (highest 245 KB); what is missing is content: wallpaper, LA08 glyphs, widget snapshots | same-frame diff **97.7 %** (captain, s11b, `diffdump --tolerance 8`); of 42 PNG screenshots: 15 all black, 14 black + 24 % green, 8 home screen (225–245 KB, 26.6 k colours) |
| P2'''' — CAImageQueue registration | **the symbol is not interposable, and it is not the wire's business**: 0 registration lines in a boot where the two processes made 24 surfaces, the LA08 among them | 2 pools (backboardd, SpringBoard) × 2 loads, 0 `ciq:` create/register lines; kernel `created surface` 24 lines incl. `312x202 'LA08'`; SpringBoard imports only `_SBSystemAppMain`, backboardd imports no `CAImageQueue*`, QuartzCore calls it with 2 direct `BL`s |
| P3'''' — the Today rebuild | **one call a boot, removable, but not the stall majority**: `today_off=1` skips it, the interface comes ~60 s earlier, +22 % frames, −17 % stalls (criterion was −50 %) | s11d 3,275 frames / 3.8 fps / 35 stalls vs s11e 4,004 frames / 4.7 fps / 29 stalls; the rebuild itself at 505 s builds the two Today VCs 2.4 s apart |
| P4'''' — the four cells | **W3 is 7.6× the W3-off cell in the same window and no longer black**; the two local cells cannot fill a 120-frame window | W1.5+W3 **8.89 fps** (120 frames / 13.51 s, screenshot 242,259 B) vs W1.5 **1.17 fps** (102.72 s, 243,662 B); pure local / by reference TIMEOUT at 17/129 and 74/170 frames |

One line: **the screen is not black because the pipe is broken.** It is black whenever the guest itself draws an empty frame, or whenever the display presents a panel-sized surface that nobody has written yet. The acceleration path (the host draws the frame and writes its rows into the display's surface) does exactly what it was built to do; what is missing is content (wallpaper, glyph, widget), not the transport.

`file:line` below is as of 2026-11-04 14:20; the sources keep moving, so the function names are the anchors.

---

## 1. How the stage-10 "real black screen" was born

Stage 10 wrote, in its own P2''' table:

```
| W1.5+W3 | 截图 14,324 B——真黑屏 | 凑不满（host 4,101 帧 / 807 s） | 5.08 fps（host 时钟） | 行写回写了、显示面不更新 |
```

That row was **one screenshot**, taken once, at one moment. The 14,324 B state is real, but it is a state of the **guest's own drawing**, and it recurs in every run:

- `/tmp/xb-m8ww15w32-3828034.png` (stage 10, 11:28, 14,324 B) and `/tmp/s11a-shots/shot-133558.png` (stage 11, s11a, 13:35:58) are the **same image**: black 2,054,203 px + `0,255,0` 683,954 px + edge 1,217/562 px, green box `y 1218–2435, x 563–1124`, and both files are 14,324 B.
- That state is what the host drew for that frame, and what the guest's own surface held:
  - host: `host_glwire: stage11 frame 1 surface 4988928 B, 3072 B a row: picture 301e18b8 301e18b8 86072291 02440778 02440778 surface 301e18b8 301e18b8 86072291 02440778 02440778 px ff000000 ff000000`
  - guest: `hakios-remote: s11 after f 1 surface 1 750x1624 row 3072 rows 301e18b8 301e18b8 86072291 02440778 02440778 px ff000000 ff00ff00`
- A screenshot is a coin flip between states. In s11b (single-surface writeback, 6,958 frames, one screenshot every 20 s, 42 PNGs): **15 all black (8,061 B), 14 black + 24 % green (14,324 B), 2 near-black (14,316 B), 8 home screen (225–245 KB, 26.6 k colours), 3 in between**. In s11a: 16 all black, 11 black + green, 14 home screen, 2 in between.

So the stage-10 conclusion "行写回写了、显示面不更新" is wrong in its second half: the display surface **is** updated (section 3), and the screenshot happened to land on a frame the guest drew black. The correct statement: **the transport is fine; the screen is only as good as the frame the guest drew.**

---

## 2. P1'''' (a) — where the rows went: surface, stride, bytes

The guest points the frame's picture at the display's surface in `frame_end` (`userland/src/eagl.c`):

```
direct = eagl_remote_surface((GLsizei)e->images[0].width, (GLsizei)e->images[0].height,
    IOSurfaceGetBaseAddress(e->images[0].surface), (GLsizei)IOSurfaceGetBytesPerRow(e->images[0].surface));
```

`eagl_remote_surface()` (`userland/src/eagl_remote.c:335`) asks the kernel for the physical segments of that
user range (`vtop`, syscall 556), requires that they cover **every byte** of `row_bytes × height`, and returns 1
only then. The request line carries the stride, so the host knows where each row starts
(`eagl_remote.c:467`):

```
R <w> <h> <records bytes> <picture bytes> <segments> <row bytes> [<segments2> <row bytes2>]
```

The host validates every segment against the RAM file and requires `sum(len) == row_bytes × height`
(`tools/glwire/host_glwire.c:880-940`), then copies **only `width × 4` bytes per row**, at `y × row_bytes`
(`write_picture_rows()`, `host_glwire.c:504-537`). The numbers for the two panel-sized surfaces, from the logs:

| surface | geometry | stride | bytes written | log line |
|---|---|---|---|---|
| IOSurface 1 | 750×1624 BGRA | **3072** B/row | **4,988,928** | `MiniIOSurface: backboardd[16] created surface 1: 750x1624 'BGRA', 3072 bytes per row, 0 planes, 4997120 bytes` |
| IOSurface 2 | 750×1624 BGRA | **3008** B/row | **4,884,992** | `host_glwire: stage11 frame 4 surface 4884992 B, 3008 B a row: …` |

The guest's own per-frame line names which surface was written (s11c has the extended form):

```
hakios-remote: flush -> 2 (rows into the surface) surface 1 row 3072
hakios-remote: flush -> 2 (rows into the surface) surface 1 row 3072 and a second
```

`flush -> 2` is the host's `R 0` (in place, nothing travels back); `flush -> 1` would be the old
"picture travels back and the guest copies it" path. The suffix `(rows into the surface)` only means
`eagl_remote_surface()` accepted the address — it does **not** say the write was in place; only the number does.

---

## 3. P1'''' (b) — why the display surface holds it: the hashes are equal on both sides

The same five rows and five pixels a row are hashed with the same FNV-1a on both sides — the guest from
its own `IOSurfaceGetBaseAddress()` (`eagl.c:1205-1221`, printed by `surface_say()` `eagl.c:1223-1262`,
call sites `eagl.c:1269` "left" and `eagl.c:1454` "after"), the host from its picture **and** from the
picture's own segments read back out of the guest's RAM (`host_glwire.c:452-501`):

```
host_glwire: stage11 frame 1 surface 4988928 B, 3072 B a row: picture 301e18b8 301e18b8 86072291 02440778 02440778 surface 301e18b8 301e18b8 86072291 02440778 02440778 px ff000000 ff000000
hakios-remote: s11 after f 1 surface 1 750x1624 row 3072 rows 301e18b8 301e18b8 86072291 02440778 02440778 px ff000000 ff00ff00
```

All five rows agree, and they keep agreeing: `host_glwire: stage11 frame 5300 … picture 491b7af0 559bff38 301e18b8 301e18b8 301e18b8 surface 491b7af0 559bff38 301e18b8 301e18b8 00000000` vs
`hakios-remote: s11 after f 1 surface 2 … rows 491b7af0 559bff38 301e18b8 301e18b8 301e18b8`
(the host's `00000000` on the last row is a sampling limitation, not a write failure: `write_picture_rows()`
splits a row across segment boundaries, but `stage11_report()` samples a row only inside one segment — the
guest reads its own mapping and gets the real bytes).

Two more things this proves:

1. **Nothing wipes it between frames.** The "left" sample of the next frame equals the "after" sample of the
   previous one: `s11 left f 0 surface 1 … rows 301e18b8 301e18b8 86072291 02440778 02440778` follows
   `s11 after f 1 surface 1 … rows 301e18b8 …` and matches it.
2. **`vtop` translates an IOSurface user mapping correctly.** Before this, `vtop` had only been verified on an
   anonymous `mmap` (`userland/src/vtop_test.c:52-62`); the IOSurface mapping is what W3 writes through, and
   the equality above is the end-to-end proof. Cache maintenance is not needed either: QEMU runs the guest
   RAM as `-object memory-backend-file,…,share=on`, so the host's `MAP_SHARED` write is the guest's memory.

The `px` field differs on purpose: the host samples the first two pixels of the middle row from the
surface, the guest samples the first **and the last** pixel. The last pixel is the green one
(`ff00ff00` = `0,255,0`), which is why the host's `px ff000000 ff000000` and the guest's
`px ff000000 ff00ff00` are both right.

---

## 4. P1'''' (c) — the compositor and the display driver: order, `s->pixels()`, 1.5×

**Order.** In the same serial stream, the frame's flush comes before the swap that shows its surface:

```
hakios-remote: flush -> 2 (rows into the surface)                       ← s11b line 1814
MiniIOMobileFramebuffer: swap 1 layer 0: surface 1 (750x1624 'BGRA') from 0,0 750x1624 to 0,0 750x1624, layers set 0x80000007   ← line 1820
```

so `show()` reads the surface **after** the host's rows landed in it, and the surface id it presents (1) is
the one the guest had just written (`s11 after … surface 1`).

**Path.** CoreAnimation submits its swap through `IOMobileFramebuffer::swap()`; the surface number is the
`SWAP_SURFACE(layer)` word (`iokit/Kernel/MiniIOMobileFramebuffer.cpp:70`, `0x54 + 4*layer`), and the driver
takes a reference to it (`MiniIOSurfaceCopy`, `:1027-1029`). The presenter thread (`:1043-1064`) calls
`show()` (`:1068`); for layer 0 it calls `drawLayer()` (`:1175`), which reads the surface through
`s->pixels()` (`:923`; `MiniIOSurface.h:147-149`, `kpixels + info.offset`), stride `info.bytesPerRow`
(`:924`), and writes into the panel `fBase` (`:496`, `vinfo.v_baseaddr`, row `fRowBytes` `:499`/`:723`).

**Scaling.** `MiniIOMobileFramebuffer: display 750x1624 on a panel of 1125x2436 (4500 bytes per row)`
(`:522-524`): `drawLayer()` maps the 750×1624 surface onto the 1125×2436 panel, exactly ×1.5, bilinear by
default (`fb_filter=1`, `:160`). `fBase` is what QEMU's ramfb scans (`-device ramfb`; `tools/glwire/panel_probe.py`
documents that `ram.img` is the guest's physical memory and a screendump follows the guest framebuffer), so a
screenshot is the panel, i.e. the surface at ×1.5.

**Coordinates agree end to end.** The green block is `rows 812-1623, cols 375-749` in the picture and
`y 1218-2435, x 563-1124` in the screenshot — exactly ×1.5, no vertical flip. (Row *i* of the picture is row
*i* of the surface is panel row 1.5 *i*: CoreAnimation renders the surface upside-down into the host's
framebuffer, and `glReadPixels` turns it back over, so the write order is correct.)

---

## 5. The two mechanisms that really make the screen black

**5.1 The kernel zeroes the panel when the bottom layer is taken away.**
`MiniIOMobileFramebuffer::show()` has, for `sid == 0` on layer 0:

```
MiniIOMobileFramebuffer: swap 3 (#2): surfaces 0 0 0 0, layers set 0x1 cleared 0x1
→ MiniIOMobileFramebuffer.cpp:1146-1152  bzero(fBase + y*fRowBytes, fPanelWidth*4)
```

In s11a the guest drew only 4–6 frames during the first seven minutes of the boot
(`host_glwire: frame 3, 750x1624, 2424 records B, 4872000 picture B, 3.5 ms, interval 288.84 s (0.00 fps), 0 image refs (0 B) in-place rows`),
the panel was zeroed at 13:21–13:22, and the first screenshot with content again is 13:27:55 → **about six minutes
of black** and every screenshot in between is 8,061 B (pure black). That is a boot-phase property, not a W3 defect.

**5.2 The swap can present a panel-sized surface that `frame_end` has not written.**
The guest wrote surface 1, the next swap presented surface 2:

```
hakios-remote: s11 after f 1 surface 1 750x1624 row 3072 …
MiniIOMobileFramebuffer: swap 2 layer 0: surface 2 (750x1624 'BGRA') from 0,0 750x1624 to 0,0 750x1624, layers set 0x1
```

and surface 2 was still all zeroes the first time the guest sampled it
(`hakios-remote: s11 left f 0 surface 2 750x1624 row 3008 rows b9fee455 b9fee455 b9fee455 b9fee455 b9fee455 px 00000000 00000000`)
→ that swap put zeroes on the panel. The same mechanism shows with W3 off: the last screenshot of the
11:37 run (W1.5, no W3) is 8,061 B pure black (`/tmp/xb-m8ww152b-3965878.png`). So "black screenshot" is
**not** a W3 property and must not be read as one.

---

## 6. The second display surface (implemented; being verified)

A frame knows **28 images** (`eagl.c:1356-1370` prints the first six, first 3 frames and every 200th):

```
hakios-remote: s11 f 1 images 28 0:2T- 750x1624/3008 1:1T- 750x1624/3072 2:5-- 180x180/720 3:6-- 180x180/720 4:7T- 16x16/64 5:8-- 180x180/720
```

`0:2T-` = image 0 is IOSurface 2, target, 3008 B/row; `1:1T-` = image 1 is IOSurface 1, target, 3072 B/row.
**Both are panel-sized targets**; writing only `images[0]` leaves the other one at the frame the display last
looked at it — which is the black screenshot every other time.

The fix is in the tree and running (s11c, build 14:03):

- `eagl_remote_surface2()` (`userland/src/eagl_remote.c:373-399`) translates the second surface the same way;
  `frame_end()` names it right after the first (`eagl.c:1397-1402`) and locks both around the flush;
- the request line grew to eight fields (`eagl_remote.c:467`), the host reads the second segment list and
  stride (`host_glwire.c:635`, `:660-664`), validates them and writes both; `in_place` requires **both** to
  succeed (`host_glwire.c:880-940`), otherwise the picture travels back;
- the host prints both samples: `stage11 first frame 2 …` and `stage11 second frame 2 surface 4884992 B, 3008 B a row: …`;
- the guest's flush line says when a second surface was named: `… surface 1 row 3072 and a second`
  (2,840 such lines in s11c and 2,843 host frames when this was written).

`s11c` (14:03–14:18, 2,843 frames, 2,840 `… and a second` flush lines): before the interface appears the panel
is black (3 × 8,061 B, 21 × 24,960 B in the boot phase, all before 14:13), and **after it appears 19 screenshots
are all the home screen** — 17 of 240,799–244,645 B and two of 24,960 B at 14:13:15/14:13:37 (the first home
screen being drawn). Zero 14,324 B states after the interface. In s11b, the same window had 15 black and 14
black+green screenshots out of 42. So the dual writeback is what removes the flicker; the black of the boot
phase is the guest drawing nothing yet (section 5).

---

## 7. P0'''' — the criterion and what is actually missing

The plan's P0'''' has two halves: screenshot > **300 KB**, and same-frame diff `≥95 %`. Results:

- **the diff half passes**: **97.7 %** (captain, s11b, `harness/diffdump.py --tolerance 8`, same frame);
- **the 300 KB half cannot pass in this configuration**: the highest screenshots are **245,157 B** (s11a) and
  **244,661 B** (s11b); the pure-local run is 446 KB. 300 KB is a "the wallpaper and the glyphs are there"
  size, not a "the picture arrived" size.

So the criterion should be re-based on the same-frame diff (with the 300 KB number kept as the
"everything present" reference), and the missing content named:

| missing on the screen | evidence | why it is missing |
|---|---|---|
| **text / glyphs** | `MiniIOSurface: SpringBoard[10] created surface 4: 312x202 'LA08', 640 bytes per row, 0 planes, 131072 bytes` — the **only** mention of surface 4 in the 28,677-line s11a guest log; no `-texImageIOSurface:`, no `-attachImage:` | the LA08 (luminance+alpha) texture is never bound through EAGL, so its pixels never reach the wire; the icons do: `hakios-gl: -texImageIOSurface: surface 5 ('' 180 x 180, 0 planes) as texture 19` → icons visible, text not |
| **a widget snapshot** | `MiniIOSurface: SpringBoard[10] created surface 3: 184x124 'BGRA', 736 bytes per row` — surface 3 never appears in the `-texImageIOSurface:` list either | same class as the LA08 (a surface the renderer never binds through EAGL) |
| **wallpaper** | every host picture is mostly black (frame ~1000: black 61 %, white 21 %, `43,52,191` 4 %); no wallpaper-sized surface is confirmed — the kernel's surface trace is capped, so "none created" cannot be claimed from it | P2'''' says the user-space register cannot name it (0 lines); the kernel's `created surface` log is the only register that works, and no wallpaper-sized BGRA is among the 24 surfaces of that boot. Either it is a solid colour, or it is created outside the window the log covers — to be settled by a kernel-side sample of `-attachImage:`/texture binds |
| **widget placeholders** | the white rounded blocks on the home screen | their content surfaces are in the same class as the two above |

The green `0,255,0` is **not** a broken pixel: it is the same in the guest's surface, in the host's picture and
on the panel (a 375×812 rectangle = ¼ of the 750×1624 surface; its origin in the guest's drawing is still
open — the cheapest way to name it is `GLWIRE_SAVE_RECORDS=<prefix> GLWIRE_SAVE_AT=<frame>` on the host and to
read opcode 6 (`glClearColor`) and opcode 44 (`glUniform4fv`, colours) out of the record stream; no rebuild needed).

---

## 8. P2'''' — CAImageQueue registration: the hook that cannot see anything

**Built.** `libhakios.ciq.dylib` (guest `userland/src/inject_ciq_probe.c`; `build.sh` compiles it and
INJECT gives it to both backboardd and SpringBoard), with two `__DATA,__interpose` pairs:

- `CAImageQueueRegisterIOSurfaceBuffer` — QuartzCore `__TEXT,__text`, unslid `0x18833c9dc` in this
  system's 20F66 dyld shared cache. The disassembly fixes its ABI: `x0` the CAImageQueue (checked,
  then `ldr x8,[x0,#0x30]`), `x1` the IOSurface (checked, passed on), `x2` flags (`tbnz` on bits
  0/1/2); the result comes from a tail call at `0x188345d50` — a queue-buffer id, 0 on failure. The
  probe declares whole registers as `uintptr_t` and passes them through untouched.
- `IOSurfaceCreate` — the call that actually makes a surface, hooked because the first one can be
  silent by construction.

**One boot (s11d, 14:21–14:36, `today_log=1`):**

- the probe is in both processes, twice each (both were restarted once):
  `ciq: probe loaded (backboardd): CAImageQueueRegisterIOSurfaceBuffer + IOSurfaceCreate`
  `ciq: probe loaded (SpringBoard): CAImageQueueRegisterIOSurfaceBuffer + IOSurfaceCreate`
- **zero create/register lines** — in a boot where the kernel's own log says the two processes made
  24 surfaces, among them the one this stage is about:
  `MiniIOSurface: SpringBoard[10] created surface 4: 312x202 'LA08', 640 bytes per row, 0 planes, 131072 bytes`
  (line 3761 of the serial log, after both probes were loaded at 929/1066), and 24 in total
  (`backboardd[16] created surface 5 … 25`, the 180x180 icon surfaces).

**Why it is silent** — in the images, not in the probe:

- `SpringBoard.app/SpringBoard` is a 112,048-byte shell whose only import is `_SBSystemAppMain`; it
  imports no `CAImageQueue*` at all. The real SpringBoard code lives in the shared cache.
- backboardd's 740 chained-fixup imports carry `_IOSurfaceCreate`, `_IOSurfaceGetWidth`,
  `_IOSurfaceGetHeight`, `_IOSurfaceGetBytesPerRow` — and no `CAImageQueue*`.
- QuartzCore calls its own `CAImageQueueRegisterIOSurfaceBuffer` with two direct `BL`s
  (`0x188386980`, `0x188461ba8`, both inside QuartzCore's `__TEXT`): same-image calls, which dyld
  interposition cannot redirect.
- and the `__interpose` section only redirects bindings dyld has not made yet; this library is put
  into the process at exec, after the shared cache's chained fixups are resolved. (The same is
  presumably why the `IOSurfaceCreate` pair, which *does* cross an image boundary, printed nothing.)

**What it means for the wire: do not put this symbol into it.** `CAImageQueueRegisterIOSurfaceBuffer`
registers an already-existing surface and returns a queue-buffer id; no pixels travel through it, and
it is not the screen's swap path. The surface register that does work is the kernel's own line: 24
`created surface` lines a boot, with the id, size, pixel format and stride — `312x202 'LA08'` by
SpringBoard, `180x180` icons by backboardd — which is what the stage-10 target wanted to line up with.
Route B's next step is a kernel-side probe (or reading the surface where it is shown), not a
user-space interpose; GOT rebinding (`fishhook`) is the other user-space option and was not tried.

**Criterion check.** The criterion asked for ≥5 registration lines a boot from the interpose,
including the LA08. The interpose gives **0**; the kernel's `created surface` log gives **24**, one of
them the LA08. The information the criterion wanted is there; the mechanism the plan named is not.


## 9. P3'''' — the Today rebuild: once a boot, and it is not where the stalls are

**What triggers it** (20F66 disassembly of the shared cache; addresses are unslid):

1. `-[SBIconController initWithMainDisplayWindowScene:…]` +0x2d4 (`0x1c4be54ac`) calls `_updateIconModelStore`
   unconditionally — every boot installs the icon model;
2. `_updateIconModelStore` +0x2bc (`0x1c4bed38c`) sends `setModel:`;
3. `-[SBIconController setModel:]` `0x1c4bed594` takes `[model rootFolder]` and calls `_rootFolderDidChange` at
   `0x1c4bed5ac` — the only direct call site in the whole cache;
4. `setModel:` also registers `_iconModelDidLayout:` for `SBIconModelDidLayoutIconStateNotification`;
5. `-[SBIconController _iconModelDidLayout:]` (`0x1c4bed95c`) is a single `b _objc_msgSend$_rootFolderDidChange` —
   every icon-state layout rebuilds it again.

**What the log says** (the `today_log=1` boot, s11d):
`hakios: today: _rootFolderDidChange #1, the first of this boot (505077 ms since boot, the main thread)`
`hakios: today: the Today view is built, #1 (location 1, 505442 ms)`
`hakios: today: the Today view is built, #2 (location 2, 507803 ms)`
— **one call per boot**, ~505 s in, building the two Today view controllers (main screen + overlay) 2.4 s later.
It is not a repeating cost and not the Spotlight initialisation (the Spotlight presenter is downstream of
`SBTodayViewController.loadView`).

**The switch** (`userland/src/inject_sb.c`, `today_*` boot args; nothing is hooked unless one is given):
`today_log=1` logs both methods; `today_light=1` makes the factory return nil (the loadView → search presenter →
feather blur → MTMaterialView chain never runs, the rest of `_rootFolderDidChange` stays); `today_off=1` returns
from `_rootFolderDidChange` itself and the factory returns nil. Verified in s11e (`today_off=1`):
`hakios: today: nothing of the Today page is done (today_off=1) …`, the two builds say `…, not here, …`, the home
screen is a normal 243 KB picture (244,552 B at 14:48), and the Today page is blank.

**The numbers** (same image, same SMP, 900 s boots, whole boot):

| boot | UI seen | host frames | stalls ≥1 s | stalled seconds | last 600 frames |
|---|---|---|---|---|---|
| s11d `today_log=1` (rebuild happens) | 14:31 (≈600 s in) | 3,275 (3.8 fps) | 35 | 646 s | 5 stalls / 27.4 s |
| s11e `today_off=1` (rebuild skipped) | 14:47 (≈540 s in) | 4,004 (4.7 fps) | 29 | 601 s | 5 stalls / 18.0 s |

So: the interface comes up **~60 s earlier**, frames **+22 %**, stalls **−17 %**, stalled seconds **−7 %**. The
criterion asked for **−50 %**, and it is not met. The reason is visible in the same table: the largest stall is
the 460 s frame gap **before** the interface (a boot-phase hole, not this rebuild; see
`host_glwire: frame 3 … interval 460.55 s`), and after the interface the picture is served at 4.7 fps with only
5 stalls. **The Today rebuild is one real, removable cost, not the stall majority.**


## 10. P4'''' — the four cells, with both display surfaces written

All four cells were rebuilt from the current source (dual writeback included), same host binary,
`MINSHOT=200000` so the W1.5/W3 look counts as the interface, and the window opened 60 s after it
(`WINDOW_AFTER=60 WINDOW_FRAMES=1 WINDOW_SPAN=120`; 900 s per boot, `/tmp/s11-p4.txt`):

| cell | image | interface | window: 120 frames | screenshot |
|---|---|---|---|---|
| pure local | `GL=1 CONFIGD=1` — no recorder, the guest draws | 600 s | **TIMEOUT**: 17 of 129 guest frames in 900 s (host 0) | 446,859 B |
| by reference + panel | `GLWIRE_LOCAL=1` — the guest draws as well | 580 s | **TIMEOUT**: 74 of 170 host frames | 446,791 B |
| W1.5 (W3 off) | `GLREMOTE=1`, `GLWIRE_NO_INPLACE=1` | 420 s | **1.17 fps** — 120 frames in 102.72 s (host 111→231) | 243,662 B |
| W1.5+W3 | `GLREMOTE=1`, in-place on (both surfaces) | 560 s | **8.89 fps** — 120 frames in 13.51 s (host 549→685) | 242,259 B |

Two readings:

- **W3 is 7.6× the W3-off cell in the same window** (8.89 vs 1.17 fps), and it is no longer black:
  242,259 B against W1.5's 243,662 B — the same picture minus the same missing content. The stage-10
  "5.08 fps" was a whole-boot number taken with a black screen; after the interface, with both
  surfaces written, the window is **8.89 fps**.
- **the two local cells cannot fill a 120-frame window at all** (17/129 and 74/170 frames in 15 min),
  so the "same window" rule still cannot be applied to all four cells at once. The fair comparison is
  the W1.5↔W3 pair: same image, same interface path, only the writeback differs.

Screenshots: `/tmp/s11p4pure-shot.png` (446,859 B), `-local-` (446,791 B), `-w15-` (243,662 B),
`-w3-` (242,259 B).

---

## 11. Not done / next

1. **B path (P2'''')**: the LA08 glyph surface, the 184×124 widget surface and the wallpaper are the three
   known missing pieces; the registration probe is the first step, and §7 says what it must name.
2. **Kernel-side sample (the third link) is still inferred, not printed.** Today the chain is proven by
   (a) equal hashes guest↔host, (b) flush before swap in the same stream, (c) the screen matching the picture
   at ×1.5. A five-line sample in `show()` (`MiniIOMobileFramebuffer.cpp`, after `drawLayer()` at `:1175`,
   over `s->pixels()`) would print it directly; the plan for it is in the P1'''' report.
3. **The 300 KB criterion** should be replaced by the same-frame diff (97.7 % already measured), with
   300 KB kept only as the "wallpaper + glyph present" reference.
4. **The dual display-surface writeback** (s11c) needs its own number: the black-screenshot ratio before
   (s11b: 15 all-black + 14 black+green of 42) and after.
5. **Reading the old logs**: `flush -> 2` = in place, `flush -> 1` = the picture came back; the suffix
   `(rows into the surface)` says nothing about in-place. `in-place rows` on the host only means the
   validation passed and the host wrote — not that the pixels reached the screen.
6. **P3''''/P4''''** as above.

---

## 12. Artifacts

| artifact | what it is |
|---|---|
| `/tmp/s11a.log`, `/tmp/s11a-host.log` | single-surface run, 5,362 frames, 43 screenshots, 245 KB max |
| `/tmp/s11b.log`, `/tmp/s11b-host.log` | single-surface run, 6,958 frames, 42 PNGs, 8.2 MB `s11b-frames/` |
| `/tmp/s11c.log`, `/tmp/s11c-host.log` | dual-surface build (14:03), 2,843 host frames when this was written |
| `/tmp/s11a-frames/`, `/tmp/s11b-frames/` | every 100th picture (and the first 20), 750×1624 BGRA |
| `/tmp/xb-m8ww15w32-3828034.png` | the stage-10 "black" screenshot (14,324 B) — the same image as s11a's `shot-133558.png` |
| `/tmp/xb-m8ww152b-3965878.png` | the last screenshot of the W3-off run: 8,061 B pure black — black is not a W3 property |

Run parameters (s11a/s11b/s11c, `harness/boot-measure.sh <tag> root.apfs 900 --save …`):
`XCMD="slto_us=30000000 tlto_us=60000000 lpto_us=60000000 glwire_local=0"`, the host at its defaults
(in-place on), `GLWIRE_DUMP_AT=600 GLWIRE_DUMP_PAIR=1`, one screenshot every 20 s.

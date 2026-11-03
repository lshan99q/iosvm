# GPU acceleration, stage 7 — results

> 2026-11-03, the measurements of `gpu-acceleration-stage7-plan.md` (the plan is unchanged; this is what
> it measured).  One line: **D1 was decided as a boot parameter plus a retry protocol, not as a root
> cause; D2 was decided against the panel route (A) with a concrete next step for the record route (B);
> the rest is numbers or an explicit "not measured".**

---

## 0. The three numbers, updated

1. **Boot**: with `cpus=4` and `nice -n -20`, three cold boots of `/tmp/root-fixed-net.apfs` gave
   **two interfaces (420 s, 660 s), one pass of the criterion** (the other panicked within 300 s of the
   interface).  Raising the ticket-lock deadline from 60e6 ticks (~0.96 s) to 240e6 (~3.84 s) did not
   help — the panic itself reports `timeout after 240000030 ticks`, so the boot argument took effect.
   With `tlto_us=30000000` (30 s) **2/2 boots passed, 0 panics** — but the stalls did not go away
   (10–17 s frame gaps).  33 panic logs hit **21 `pset0.sched_lock`, 4 `thread_call_groups` and 8
   different heap addresses**: not one deadlock, the whole guest stops for seconds.
2. **Picture**: what `remote_only` misses is not "CA's software path is not recorded" but **"the display
   surface is never drawn"**: with the panel uploaded in `remote_only` and the host neither drawing it
   nor writing back, the panel dump is **0 painted pixels** while the host's replay paints 38.2%
   (icons, no text).  So route A is dead, C is the choice for this round, and B has a next step.
3. **Measurement**: the biggest output of the round is the criterion itself.  Three things were lying
   before it: the previous boot's frame still in `qemu/ram.img`, `fs.protected_regular=2` silently
   blocking every root-run screendump, and "a big screenshot means the interface is up".
   With them fixed, the interface appears at **420–660 s**, not at two minutes.

---

## 1. P0 / D1 — a boot you can predict

### 1.1 The criterion

One cold boot is one sample.  **The interface** is the first screenshot >= 300 KB taken after the host
has frames.  **PASS** = interface, then 300 s with 0 panic / 0 SIGSEGV / 0 usertrap.  `harness/boots.sh`
is the retry protocol: `IMAGE=… WANT=8 TRIES=12 ./harness/boots.sh`.

### 1.2 Probe A — host priority (`nice -n -20`), 2026-11-03 17:17–17:51

| # | interface | frames | result |
|---|---|---|---|
| 1 | 660 s (447,379 B) | 306 | **FAIL**: panic within 300 s of the interface, `Ticket spinlock[0xfffffff007b68880] timeout after 60000005 ticks; current owner: 0xfffffff6008e4c10 (on CPU 0)` |
| 2 | 420 s (448,409 B) | 2,466 | **PASS**: 0 panic, last 100 frames 7.32 fps |
| 3 | none | 3 | **FAIL**: panic at 5 min, the same lock |

### 1.3 Probe C — the deadline

| deadline | boot arguments | result |
|---|---|---|
| 3.84 s | `tlto_us=3840000` | interface at 420 s, **panic** (`timeout after 240000030 ticks` — the argument works) |
| 30 s | `tlto_us=30000000` | **2/2 PASS** — interface at 570 s and 510 s, 0 panics; the two P2/P1/P4 boots that followed (same argument) were 0 panic too, so **5 boots, 0 panics**.  The stalls are still there: 37 and 41 frame gaps over 1 s, worst 16.96 s and 10.45 s |

The 1 s deadline is what turns a multi-second stall into a panic.  `tlto_us` is a **boot argument**
(`PE_parse_boot_argn("tlto_us")` in `osfmk/arm64/machine_routines.c:ml_init_lock_timeout()`), so no
kernel rebuild was needed for this probe.

### 1.4 D1's conclusion

Selected parameters, written into `harness/README.md`:

```
cpus=4 fb_beat=0 fbscale=2 metal=1 tlto_us=30000000, image /tmp/root-fixed-net.apfs,
harness/boots.sh with ZERO_RAM=1 and UI_GRACE=300
```

The "10 boots, >= 8 interfaces, 0 panic" acceptance has **not** been run (5 samples so far: 2 probe +
P2/P1/P4 boots).  The stalls remain and their cause is not known; `/proc/pressure/io` read some
avg300 = 20% during these boots, and `ZERO_RAM=1` rewrites 2.4 GB of `ram.img` per boot — both are
suspects, neither is proven.

---

## 2. P2 — the phase

Clean control: `XCMD="tlto_us=30000000 glwire_nopicture=1" HOST_ENV="GLWIRE_NO_INPLACE=1" … --no-panel`:
128 host frames, **0 MATCH / 128 DIFF**; and offline, with 112 frames that have both hashes,
`panel(N) == picture(N)`, `== picture(N-1)` and `== picture(N+1)` are all **0/112**.
The fourth stage's `np3` (17/141, 72/140) is not reproduced.  The hash is the wrong yardstick here: the
guest's renderer and Mesa on the V100 never agree pixel for pixel (the matches were probably
black-on-black frames).  Next: a tolerant comparison of a dumped pair; per the plan this is "change the
acceptance", not "relax it".

## 3. P1 / D2 — A is dead, C is the choice, B has a next step

Panel uploaded in `remote_only`, host neither drawing it nor writing back, guest ignoring the picture:

```
XCMD="tlto_us=30000000 glwire_panel_remote=1 glwire_nopicture=1"
HOST_ENV="GLWIRE_NO_INPLACE=1 GLWIRE_DUMP=/tmp/s7/p1b/f GLWIRE_DUMP_AT=600"  … --no-panel
$ python3 harness/diffdump.py /tmp/s7/p1b/f -o /tmp/s7/p1b/diff
  painted: panel 0 (0.0%), picture 1202572 (98.7%)
```

The panel is black.  In `remote_only` the guest executes none of the GL draws, so nothing draws into its
display surface, and there is nothing for A to merge with.  The text is not in the record either (nine
triangle draws a frame; `count_spans()`' glyph spans never appear): it is drawn by a GL entry point that
is **not one of the 62 slots in `spec.def`**, executed locally in the guest and never copied to the
surface.  So:

* **A (W1.6)** — dead, measured.
* **C (admit incompleteness)** — chosen: `remote_only` stays a speed mode; the complete screen is the
  R1 shape (guest draws + panel + row write), 448 KB and 6.3–7.3 fps on the same day.
* **B (record the missing path)** — next step: wrap the EGL slots `eagl_slots.txt` does not replace,
  count them for one boot, and see which call draws the glyphs (`glDrawArraysInstanced`,
  `glDrawElementsInstanced`, `glDrawRangeElements` are the candidates).

P1's acceptance (> 400 KB with text and Dock) is **not met**: 8,061 B screenshot, 0 pixels in the panel.
The record stays small: median 8,951 B, max 98,317 B.

## 4. P4 — one frame in three parts

`XCMD="tlto_us=30000000 glframes=1 touch_lat=1"`, 2,934 host frames and 2,934 guest wait lines
(the first 200 frames dropped):

| | mean | median |
|---|---|---|
| frame interval | 99.4 ms | **20.0 ms** |
| waiting for the host | 21.1 ms | **7.6 ms** |
| of which the host's replay | 2.7 ms | 2.0 ms |
| guest's own work + idle | 82.9 ms | 11.9 ms |

20.0 ≈ 7.6 + 11.9: the three numbers add up.  A frame itself costs 20 ms (50 fps of headroom); what
keeps the port below 9 fps is not any of the three segments but the **stalls** — so the answer to "what
has to go first for more than 9 fps" is "the stalls", not "the replay" and not "the wait".

Caveat: in `remote_only` `e->in_frame` is never set (the guest does not execute the draws), so the
`tl: g` and `hakios-gl: frame` lines never print; the guest's own work and the idle time cannot be
separated yet.

## 5. P3 — not measured, and why

`rc.touch1`'s first action is at 600 s; the interface appears at 420–660 s, so the 20 s window it marks
falls before or during boot rather than on a scrolling screen.  None of the four cells was re-measured.
The numbers that do exist, with their provenance: W1.5 **6.74 fps** (stage 5's one real window), the
R1 shape 6.3–7.3 fps (today's probe A boots), the pure-local baseline not measured.
Next: let `rwtouch` wait for the first frame, or drive `qemu/touch.py swipe` once the interface is up.

## 6. What was fixed in the harness (and what it cost to learn)

| file | change |
|---|---|
| `harness/boot-measure.sh` | `UI_GRACE=S`, `ZERO_RAM=1`, per-run screenshot paths, interface requires host frames, `end=ui` |
| `harness/boots.sh` | the retry protocol, with per-attempt classification and the panic's lock in the failure line |
| `harness/diffdump.py` | panel vs picture, pixel by pixel |
| `harness/cost.py` | P4's three-part account |
| `host_glwire.c` | `GLWIRE_DUMP_AT=<seconds>`; the per-frame "X ms" was printing seconds (1000× small) |
| `userland/src/eagl.c` (port) | `glwire_panel_remote=1`; the per-frame `hakios-remote: f N wait US us` line |

**The build trap**: `userland/build.sh root.apfs` only repacks `system.apfs` when `obj/stage0` is newer
than it.  Editing `src/eagl.c` does not touch stage0, so the root image got the new library while the
system image kept the old injection and the guest ran the previous build (symptom: not one
`glframes=1` line).  Correct form: `touch obj/stage0 && ./build.sh root.apfs` (2m21s for the 2.4 GB
image).

`make test` in `tools/glwire`: `map_test: PASS`, `e2e_test: PASS`.

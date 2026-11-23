# stage 43: the desktop soak — a flat host RSS, a bounded buffer set, and a memory criterion

> Status: **measured; WP-4 item 4.1 (the leak/OOM soak) is closed for the desktop path.** The
> baseline leaked both sides: the host grew **135 → 867 MB** over a 1,500 s soak because the plug-in
> never sends `MW_DESTROY_BUFFER` while CoreAnimation asks for a fresh 256 KiB buffer every frame,
> and the guest kept one 256 KiB buffer per frame because CoreAnimation never releases it. Four
> commits bound the host's live set, pool the per-frame objects, and track buffer availability until
> the using frame is acknowledged. The final 2,400 s soak keeps **host RSS flat (185.5 → 195.6 MB,
> end/start 1.05)**, **two live buffers (0.5 MB), `reclaimed=0`**, a **25.3-minute desktop** with
> **0 jetsam / 0 memorystatus / 0 OOM / 0 real panics**, and the buffer pool working
> (**`HakiosMetalBuffer alloc=5 reused=2398`**). A separate marker-ON run validates the queue's
> safe-release probe. Branch `experiment/ca-metal-device`, 2026-11-23. This continues
> [`docs/stage39-d8-double-buffer.md`](stage39-d8-double-buffer.md), whose §4 left the 5-minute
> leak check open.

## 1. The question

WP-4 item 4.1 asks whether the desktop can run unattended — 25 minutes on lavapipe, no OOM — and
whether the two memory curves that stage 34/35 noticed are bounded:

* **host RSS** grew monotonically in every long boot, and the growth was in the host process, not
  the guest;
* **guest memory** showed a shrinking `memory: free` counter;
* the plug-in's private-base `-dealloc` leak was known and documented as a boundary
  (`docs/stage34-wp4-desktop.md` §4), not fixed.

A soak needs a criterion, not just a curve. This record states the criterion it uses (§5) and gives
the four-run evidence chain that meets it (§4): baseline, control, first re-verification, final
soak; plus one marker-ON run for the queue repair.

## 2. What was wrong

### 2.1 The host kept every buffer the guest ever named

`metalwire_host.c` had one Vulkan buffer per `MW_CREATE_BUFFER` and freed it only on
`MW_DESTROY_BUFFER`. The plug-in never sends a destroy — its `-dealloc` frees the guest side only —
and CoreAnimation asked for a fresh 256 KiB render buffer every frame. The t30 control log has
**2,135** `newBufferWithLength:262144` calls: **2,135 × 256 KiB = 547 MiB** of buffer memory plus the
driver's per-buffer overhead, the **+651 MB** in that run. The `replaceRegion:` path was not the
leak (it is drained after every frame), and images were few (94 in the same run).

### 2.2 The guest allocated a fresh buffer every frame, and the pool never hit

Stage 41/42 pooled command buffers and encoders, but `HakiosMetalBuffer` still showed
`alloc=1803 / reused=0` in 2,134 frames. The factory entry was never the problem: the t33 serial log
has **750** `newBufferWithLength:262144 options:0x0` lines, the exact entry that pools. The lifetime
counters say why: **`HakiosMetalBuffer alloc=1803 dealloc=4`** — CoreAnimation creates its per-frame
buffer and never releases it, so a pool that waits for `-retainCount == 1` can never hand it out, and
the guest leaks 256 KiB per frame with it.

### 2.3 The inherited dealloc chain is not runnable

The stub classes sit on Metal's private bases, and the plug-in has always skipped those `-dealloc`
implementations: Apple's code dereferences dispatch state that a plain `-init` never created
(`_MTLCommandQueue` releases ivar 0xb0 `_submittedGroup` without a nil check —
`docs/research/ca-metallib-url.md` §10.1). Boot t30 tried the chain once, behind
`/redwood/etc/metal-base-dealloc`: the guest stopped rendering — **0 frames in 1,500 s**, the guest
clock 2.8× behind the wall clock, no panic. That is why the repair is a **safe release** of the one
dispatch group the analysis pins down, not a call into the inherited chain.

## 3. What changed

| commit | what it establishes |
|---|---|
| `a245067` (stage41) | per-frame objects come from instance pools (command buffer, blit encoder, render encoder); the queue's base-dealloc probe behind the marker; a per-class lifecycle counter table |
| `e93efb7` (stage42) | the marker now selects only the **safe release** of `_submittedGroup` (the inherited chain is unreachable from the plug-in); buffers are pooled by length; a sixth counter column, `safe_release` |
| `efb3140` (stage42) | the host bounds its live buffer set (age `MW_BUFFER_MIN_AGE`, cap `MW_MAX_BUFFER_BYTES`), prints it under `MW_LEAK_CHECK=1`, and gets the `--leak` regression |
| `a842f2d` (stage43) | a buffer's **availability** is tracked until the frame that used it is acknowledged, so the pool hits even though CoreAnimation keeps its reference; `MW_BUFFER_LOG=1` hit/miss lines; lifecycle print every 600 committed frames |

The host side is a reclamation rule: every buffer records the last frame a message named it
(`mw_buffer_find`), and at the end of each frame the host destroys the oldest buffers the wire has
not named for `MW_BUFFER_MIN_AGE` frames (default 120) until the live set is under
`MW_MAX_BUFFER_BYTES` (default 64 MiB; `0` disables). The wire is synchronous and every message that
uses a buffer names it in that frame, so a buffer not named for 120 frames is not in any in-flight
frame. With the guest pool working, the normal path creates one buffer and never trips the cap.

The guest side is availability tracking: buffers handed out in a frame are tracked in a small list
(64 entries) and marked available when the frame's ack arrives (or on every commit failure path, so
a frame that never reaches the host does not pin them). The pool hands out an available buffer of
the same length, clears its memory, refreshes options/`uploaded_seq`, and mints a new remote id only
when the connection generation changed — the host keeps refreshing the one buffer it already has.

The plugin is `build/HakiosMetal.bundle/HakiosMetal` md5 **`bcaf99c6392d24d1d96b76586df9e246`**
(t34; two clean builds identical, 0 warnings); the host is
**`ad778a064bea462a11f6c67e552eacdb`** (t32; unchanged by t34). The changed trees are `guest/metal-plugin/` and `host/metalwire/`; no doc claims a fix they
do not contain.

## 4. Evidence

### 4.1 Baseline (t29, `wp4-saok-base`)

1,500 s, idle desktop, old plug-in `1152abab…`, host `3830787b…`: **host RSS 135 → 867 MB
(+732 MB, 6.4×)** over 37 samples, monotonically; qemu RSS 3,280 → 3,618 MB; guest
`memory: free` **585 → 2–7 MB**, `available` **1,075 → 634 MB**, `programs` 712 → 829,
`wired` 2,727 MB; 2,714 frames / 26,809 draws, wire error 0, missing 0/0; **no jetsam, no
memorystatus, no real panic** (the 10 `panic` lines are launchd's `_PanicOnCrash key: … not enabled`
lint). The serial log names the **eleven** classes whose base `-dealloc` is skipped at registration
(Device, Queue, CommandBuffer, BlitEncoder, Buffer, Library, Function, RenderPipelineState,
DepthStencilState, SamplerState, PipelineLibrary); 2,135 buffers / 547 MiB were live; 12 of the
17 dealloc-related lines are the plug-in's own "deliberate leak" statements.

### 4.2 The control and the marker-ON failure (t30, `wp4-soak-fix-ctrl`)

New plug-in `03dc9294…` with the marker **OFF**: 2,134 frames / 21,188 draws, host RSS
**68 → 719 MB (+651 MB)**, `HakiosMetalBuffer alloc=1803 / reused=0`, while command buffer, render
encoder and blit encoder stopped multiplying (`reused=3629`). The same plug-in with the marker
**ON**: **0 frames in 1,500 s**, no wire connection, guest clock 2.8× slow, no panic — the inherited
queue chain is unusable, and became the boundary recorded in §6.

### 4.3 The first re-verification (t33, `wp4-soak-fix2`) — host fixed, guest not yet

Plug-in `7136b0de…`, host `ad778a06…`: **host RSS 196 → 246 MB (end/start 1.25)** and constant at
246 MB from frame 300 on; the buffer cap held the live set at **256 buffers / 64 MiB** with 433
reclaims. But the run failed two criteria: the guest's `free` ended at 15 MB (the old ≥100 MB
criterion), and the plug-in pool still missed — the host reclaimed one 256 KiB buffer per frame
(`buffer 568..572, last used frame 429..433`), and only 690 frames (257 s of desktop) were
committed, below the 1,800-frame lifecycle print threshold of the day. That is the state t34's
availability tracking fixes.

### 4.4 The final soak (t35, `wp4-soak-fix3`) — all criteria pass

2,400 s window, 2026-11-23 13:31:03 → 14:11:03; desktop at 13:45:47 (boot → desktop 14.7 min,
host load up to 8.57); **desktop window 25.3 min**. `MW_LEAK_CHECK=1` on the host (the pool
evidence below is the lifecycle line; the guest's optional `MW_BUFFER_LOG` lines are not in this
serial log):

* **2,991 committed frames / 29,634 executed draws**, `--missing-report`:
  `frames 2991, pipelines 0, skipped draws 0`; wire error 0, format 30 0, format 115 0.
* **host RSS / leak-check**: 49 periodic lines, **185.5 → 195.6 MB** (end/start **1.05**); the last
  300 frames (2640–2940) are **193.7–195.6 MB, a 1.9 MB band**; **`buffers=2`, `buffer_bytes=524288`,
  `reclaimed=0`**; images 68–69 (27.7 MB); `pending_replaces=0`.
* **guest memory**: last segment **`free 16 / available 1135 / wired 2752 / programs 303` MB**;
  jetsam 0, memorystatus 0, out-of-memory 0, `real_panic=0` (10 launchd lint lines).
* **lifecycle**: `HakiosMetalBuffer alloc=5 reused=2398` (≈ the frame count),
  `CommandBuffer 2/2399`, `RenderCommandEncoder 13/2436`, `BlitEncoder 1/1`;
  **`base_dealloc=0` in every class**; totals `alloc=178 dealloc=20 base_dealloc=0 base_skipped=4
  reused=7234`; the leak-check **`reclaimed=0`** — the per-frame reclaim is gone.
* **picture**: stage 2 `08d80a6c…`, stage 3 `af3013a5…`, BootUI `2edab816…`; the desktop is
  complete and upright (Dock mean 66.3, 13,199 pure-white icon-glyph pixels, Search 13.6).

One post-window observation: the **exit snapshot** jumped from 195.6 to 438.5 MB (images 69 → 94),
*after* the soak window ended. It is recorded in §6 as an unreproduced residual.

### 4.5 The marker-ON run (t36, `wp4-soak-marker-on2`) — the queue repair works

Root image with `/redwood/etc/metal-base-dealloc` (md5 `0d6a0c21…`), same plug-in/host. Run 1
(900 s) reached only the BootUI (ack=3) — under a load of 5–8 the desktop needs ~15–20 min — but
already printed the two `queue safe release` pre/post lines each. Run 2 was extended to **1,500 s**
(recorded as such): desktop at 14:58:57, **ack 1,477 / draws 14,976**, wire error 0, format 30 0,
missing `frames 1477, pipelines 0, skipped draws 0`.

```
metalwire: queue safe release: pre: _submittedGroup is NULL, ivar offset 176, instance 0x100e20ac0
metalwire: queue safe release: post: created 0x100e33cf0 and released it; the slot is NULL again
           (create/release balanced, inherited -dealloc not called)
lifecycle: HakiosMetalQueue alloc=2 dealloc=1 base_dealloc=0 base_skipped=1 reused=0 safe_release=1
```

`base_dealloc=0` in every class; no panic/jetsam/memorystatus/OOM; the picture is the same complete
desktop (`6cda63fd…`, Dock 66.3 / 13,199 / 13.6). The t35 exit jump **did not reproduce**: the last
periodic lines are 187,644 / 187,648 / 188,892 KB and the exit line is **188,892 KB**, images 64 → 67,
buffers 2 (0.5 MB), reclaimed 0.

### 4.6 Four runs, one table

| item | t29 baseline | t30 control | t33 | **t35 final** |
|---|---|---|---|---|
| plug-in / host | 1152abab / 3830787b | 03dc9294 / 3830787b | 7136b0de / ad778a06 | **bcaf99c6 / ad778a06** |
| host RSS (steady) | 135 → 867 MB (+732) | 68 → 719 MB (+651) | 196 → 246 MB (1.25) | **185.5 → 195.6 MB (1.05)** |
| live buffers | unbounded (2,135 / 547 MiB) | same | 256 / 64 MiB, 433 reclaimed | **2 / 0.5 MiB, 0 reclaimed** |
| Buffer alloc / reused | 1803 / 0 | 1803 / 0 | 1803 / 0 (pool missed) | **5 / 2398** |
| guest end (free / available) | 2–7 MB / — | 18 MB / — | 15 / 1089 MB | **16 / 1135 MB** |
| jetsam / OOM / real panic | 0 / 0 / 0 | 0 / 0 / 0 | 0 / 0 / 0 | **0 / 0 / 0** |
| ack / draws | 2714 / 26809 | 2134 / 21188 | 690 / 7192 | **2991 / 29634** |

All four runs are lavapipe (`VK_ICD_FILENAMES=…/lvp_icd.json --gpu 0`), one TCG vCPU, idle desktop.
The host and plug-in md5s are the determinism check: `ad778a06` (host) and `bcaf99c6` (plugin) were
rebuilt from the committed tree for the final runs.

### 4.7 Hashes and artifacts

| item | value |
|---|---|
| commit (both final runs' tree) | `a842f2db98b682b2e04daf39da700b2eb133a625` |
| `metalwire_host` (git archive) | `ad778a064bea462a11f6c67e552eacdb` |
| `HakiosMetal` (plugin, two clean builds) | `bcaf99c6392d24d1d96b76586df9e246` |
| `host_glwire` | `5b28249a37cb11207f3ab160bcf4ac23` |
| `system.apfs` | `49ae3cf4fd53334b475a921eb9915008` |
| `root.apfs` (default, marker OFF) | `a4a6e2ce50e6d0324e26a49ea20fbd84` |
| `root.apfs` (marker ON, t36) | `0d6a0c212fe8301e73c98316d000f44f` |
| `libcainject.dylib` | `aa123ccdd6ee4661fa335ac91311bbb1` |
| clients file | 35 B: `metal_frame\nmetal_probe\nbackboardd\n` |
| vars 0xFF source | sha256 `dd30d9e07e89c1749cd420e998190ab9e31d4b43d27b5862887320ba2a2b8b0f` |

Run artifacts (not committed): t35 host log `ff4fb48a…`, missing `11296557…`, `check.json`
`5137b727…`, `rss.txt` `a69d5c78…`, serial `33a16906…`, stage-3 screendump `a00a3086…`; t36 host log
`d93cb66c…`, missing `85dafd46…`, `rss.txt` `60dd5af8…`, serial `69e514e6…`, run1 log `454099bc…`.
The soak used `MW_LEAK_CHECK=1` on the host; the pool evidence is the lifecycle line in the
guest's serial log (the optional `MW_BUFFER_LOG` path was not active in this run).

### 4.8 Repository gates

`make test` is green at this tree: provenance clean (481 tracked files), the proto suite passes, and
the lavapipe vkbridge probe reports every operation `[ok]`. The translator's suite is **15 PASS**,
including the leak-check fixture (`PASS leak-check: 130 fresh per-frame buffers are reclaimed past
the 128 KiB cap ...`). `make style` is not green on this branch for the pre-existing reasons
recorded in the earlier stage notes (legacy SPDX headers, archived link targets); this record adds
no new failure of either kind.

## 5. The memory criterion

The soak judges OOM by **events and available memory**, not by the guest's `free` counter:

> **No OOM** = `jetsam` events **0**, `memorystatus` events **0**, `out of memory` **0**, and the
> guest's last-segment **`available` ≥ 500 MB**. `free` low on its own is **not** an OOM.

Why the `free` counter alone would misjudge: under this workload the guest keeps ~2,752 MB `wired`
and fills the rest with file cache, so `free` sits at 16 MB while `available` is 1,135 MB and no
jetsam fires. The same shape appears in the runs that sampled it:

| run | last `free` | last `available` | `wired` | `programs` | jetsam / memorystatus / OOM |
|---|---|---|---|---|---|
| t29 baseline | 2–7 MB | 634 MB | 2,727 MB | 829 | 0 / 0 / 0 |
| t33 | 15 MB | 1,089 MB | 2,725 MB | 376 | 0 / 0 / 0 |
| **t35 final** | **16 MB** | **1,135 MB** | **2,752 MB** | **303** | **0 / 0 / 0** |
| t36 (marker ON) | — | — | — | — | 0 / 0 / 0 |

This criterion does not say every MB of `free` is cache: it says the OOM decision is the event
record plus `available`, and t29/t33's low-`free` runs are kept in the table as the contrast. t35's
`available` never fell below 500 MB after the desktop was established.

## 6. What this does not establish

* **The inherited `-dealloc` chain is never called.** `base_dealloc=0` in every dump (t33/t35/t36);
  the eleven registered classes still skip it. The pools stop the *per-frame* members from
  multiplying (command buffer, blit encoder, buffer — `reused=2398` while `alloc` stays 1–5), and
  the queue's safe release (marker ON) balances only `_submittedGroup`; it does **not** call the
  chain. Each non-pooled class still keeps Metal's allocation for its object, deliberately. The
  measured skipped calls in the final dump are `base_skipped=4` (Queue 1, Library 3); the classes
  that never deallocated simply never reached a dealloc, so the counter is the evidence, not a class
  count.
* **The marker is off by default.** `a4a6e2ce…` (marker OFF) is the default image and the one the
  main soak used; the safe-release path is verified (`0d6a0c21…`, t36) but enabling it by default is
  a separate change. The t30 hang is why the inherited chain cannot be selected.
* **The exit RSS jump is not reproduced.** t35's exit snapshot (438.5 MB, images 94) happened after
  the soak window; t36's exit snapshot is the same 188,892 KB as its last periodic line. Recorded as
  a known residual observation, not a criterion failure; if it recurs, the disconnect/shutdown path
  is the place to look.
* **Boot time.** Desktop appeared 14.7 min after boot in t35 (20.7 min in t33, ~10 min in t29) with
  host load 5–8.6; the correlation is noted, not explained. A faster boot is not claimed.
* **A general leak fix.** This is the desktop path's accounted memory: one connection, one frame in
  flight, idle home screen. It does not cover Control Center/Spotlight backdrops, a V100 soak, or a
  second client; those have no soak of their own.
* **Frame rate.** The soak's frame counts are commits, not smoothness; the recorded-frame rates are
  in the stage-35/39 records.

## 7. Tools this adds

* **`MW_LEAK_CHECK=1`** (host): prints the live set every 60 frames and at exit —
  `leak-check (<when>): frames= images= image_bytes= buffers= buffer_bytes= reclaimed=
  pending_replaces= draws= rss_kib=` (RSS from `/proc/self/status`). `MW_MAX_BUFFER_BYTES`
  (default 64 MiB; `0` disables) and `MW_BUFFER_MIN_AGE` (default 120 frames) set the reclaimer.
* **`MW_BUFFER_LOG=1`** (plug-in, read in the guest's environment): rate-limited buffer-pool lines,
  `buffer pool: <len> bytes hit/MISS (pool= frame= hits= misses=)`; off by default.
* **The `--leak` regression** (`metalwire_gen.py --leak`, `metalwire_test.py`'s live-set check, run
  as part of `make test`): 130 frames that each create a fresh 64 KiB buffer under a 128 KiB cap and
  a two-frame age must leave at most three live buffers, reclaim at least 100, keep one texture and
  zero pending replaces, and stay under 300 MB RSS; 40 frames on one reused id must create nothing
  (`buffers=1 buffer_bytes=65536 reclaimed=0`). A host without the reclaimer fails with 130 live
  buffers. The suite is **15 checks** at this tree.
* **The lifecycle counters** (plug-in): per-class `alloc / dealloc / base_dealloc / base_skipped /
  reused / safe_release` plus a totals line, printed on SIGTERM/SIGINT, at exit, and every **600**
  committed frames — a short soak can read them.

## 8. Reproduce

```sh
# the translator's own suite (no guest, no Apple file), including the --leak fixture
cd host/metalwire && make clean && make test

# the final soak: plug-in bcaf99c6, host ad778a06, marker OFF, lavapipe
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  MW_LEAK_CHECK=1 host/metalwire/metalwire_host --gpu 0 --port 9111 \
    --ram .../mt-work/hakios/qemu/ram.img --shm /dev/shm/metalwire-frame \
    --out /tmp/wp4soak-frames --png-every 1 --verbose \
    --missing-report /tmp/wp4soak-missing.txt &
MW_CMDLINE="cpus=1 fb_beat=0 fbscale=2 metal=1" \
  tools/metalwire/run-guest.sh wp4-soak-fix3 2400
# optional: MW_BUFFER_LOG=1 in the plug-in's guest environment prints the pool hit/miss lines

# the marker-ON path: same run with /redwood/etc/metal-base-dealloc in the root image
# (root.apfs 0d6a0c21…); the serial log must show the queue safe-release pre/post lines.
```

## 9. Files changed in this record

- `docs/stage43-leak-and-oom.md` — this note.
- `docs/state-of-the-port.md` — the 4.1 row (soak result, memory criterion, tools, plug-in md5).
- `README.md` — the milestone row, the Metal state rows (EN and ZH) and the documentation index.

The implementation commits listed in §3 changed `guest/metal-plugin/` (HakiosMetal.m and its
README) and `host/metalwire/` (metalwire_host.c, metalwire_gen.py, metalwire_test.py, README.md);
no `tools/` or `main` file was touched by this record.

# stage 48: H-A cross-check and v4 — the dualq loop is not it either

> Status: **measured. Two H-A variants and one new probe round narrow the 4.2 failure without
> fixing it.** `split-wx=on` and `-cpu max` (ARMv8.1 LSE) both still panic within minutes, so the
> two concrete "code generation/atomic implementation" mechanisms of H-A are **rejected**; H-A as
> "MTTCG parallelism is the enabling condition" remains. Probe v4 then marked the real dualq scan
> (`sched_dualq_thread_update_scan`, stages 150–157) and the hw_lock/mutex owner paths; in three
> SMP=2 runs the dualq counters **`i2#/o2#/pst#` were all 0** — the dualq pset loops never ran, so
> t51's "the 2.1 M pset takes come from the dualq scan" was **not confirmed**. The pset ticket is
> taken in `_thread_go` (waitq wake) and in `_sched_timeshare_maintenance_continue`; run C is an
> **ownerless heap ticket lock** (`owner took it at 0`, no owner stages), the same family as t48 C's
> garbage `0xe0fffff0` — memory/atomic corruption is an independent line (H-A form). Multi-core is
> still not deliverable; single vCPU remains the baseline. Probe v4 (`54db45a7…`) is not the
> delivery kernel — `f62c3d4b` was restored at 23:42:54 and checked on both paths. Branch
> `experiment/ca-metal-device`, 2026-11-23. This continues
> [`docs/stage47-v3-three-forms.md`](stage47-v3-three-forms.md).

## 1. The question

Stage 47 left two hypotheses on the table: **H-A** (parallel TCG breaks atomics/memory ordering;
the garbage `0xe0fffff0` spinlock value and the ownerless heap lock are its forms) and **H-B** (the
guest's waitq/scheduler path loops or loses a wake-up under parallel vCPUs). Stage 48 asks two
narrow questions:

1. **Can a concrete H-A mechanism be switched off?** `split-wx=on` changes TCG's translation of
   memory operations; `-cpu max` enables ARMv8.1 LSE atomics (replacing LL/SC sequences). If either
   removes the panic, H-A gets a mechanism; if both still panic, those two mechanisms are out.
2. **Where is the pset ticket actually taken?** t51 found that the active scheduler is `dualq`
   (`CONFIG_CLUTCH` is not `CONFIG_SCHED_CLUTCH`, so `SCHED()` resolves to `sched_dualq`), and that
   the v3 `sched_traditional` counters were zero because they were on the wrong function. v4 marks
   the real `sched_dualq_thread_update_scan` (stages 150–157), the maintenance loop body
   (130–132, `dt`/`lt`), and the hw_lock/`hw_wait_while_equals32` owner path (140–142, `wwe#`).

## 2. H-A cross-check (t50, delivery kernel, no probe)

### 2.1 Setup

* QEMU **8.2.2 (Debian)**; `split-wx` exists and is accepted (the group-1 command line contains
  `-accel tcg,thread=multi,split-wx=on,tb-size=1536`).
* Kernel: **delivery `f62c3d4b`**, not a probe kernel (md5 checked before and after the round:
  `f62c3d4b852d467f22c4703b8fc620c9`).
* Common parameters: SMP=2, `cpus=2`, `taskset 26–29`, `tb-size=1536`, 1,800 s limit, same host
  image (`root.apfs a4a6e2ce…`, marker OFF) and host binary (`ad778a06…`).

### 2.2 Group 1: `-accel tcg,thread=multi,split-wx=on` (`t50-splitwx`)

Boot 21:35:53; **2 frames acknowledged** (desktop never appeared). Panic:

```
panic(cpu 0 caller 0xfffffff0072eacd4): Ticket spinlock[0xfffffff007b0d380] timeout after 60000025 ticks;
cticket: 0xc9, nticket: 0xcb, waiting for 0xca,
pset last took it at 0xfffffff00733cab8 (thread 0xfffffff8f6e764e8),
owner took it at 0xfffffff0072ffa88,
select loops 265299, maint_s 0, stages 3/3/0/0,
current owner: 0xfffffff8f6eb1ca0 (on CPU 1), orig owner: 0xfffffff8f6eb1ca0,
start time: 19775642307, now: 19835642332, timeout: 60000000 @lock_ticket.c:437
```

Symbols (delivery `qemu/kernel.syms`): lock `0xfffffff007b0d380` → **`_thread_call_groups+0x280`**
(the t38 T2 / t48 B address); pset last `0xfffffff00733cab8` →
**`_waitq_pull_thread_locked+0x60`**; owner take `0xfffffff0072ffa88` →
`_sched_timeshare_init+0x160`; caller → `_hw_lck_ticket_contended+0x164`. The panic header's own
epochs put the panic at **21:41:02, 309 s (5.2 min)** after boot; the host monitor saw QEMU stop at
21:42:22. (The t50 report's "~10 min / 21:44" is an estimate; the epoch is used here.) Host: load
2.08–7.44, QEMU peak 175 %, vCPU threads 82 % / 37 %, win2022 189 %.

### 2.3 Group 2: `-cpu max` (ARMv8.1 LSE, `t50-cpumax`)

Boot 21:54:39; **2 frames acknowledged**. Panic:

```
panic(cpu 1 caller 0xfffffff0072eacd4): Ticket spinlock[0xfffffff007b68c80] timeout after 60000019 ticks;
cticket: 0xd3, nticket: 0xd5, waiting for 0xd4,
pset last took it at 0xfffffff00733cf10 (thread 0xfffffff3b017cc10),
owner took it at 0xfffffff00733cf10,
select loops 274513, maint_s 0, stages 3/3/0/0,
current owner: 0xfffffff3b000e4e8 (on CPU 0), orig owner: 0xfffffff3b000e4e8,
start time: 21411178722, now: 21471178741, timeout: 60000000 @lock_ticket.c:437
```

Symbols: lock `0xfffffff007b68c80` → **`_pset0+0x500`**; pset last = owner take
`0xfffffff00733cf10` → **`_waitq_wakeup64_all_locked+0x34`** (the **t38 control** address); caller
`_hw_lck_ticket_contended+0x164`. Panic at **22:00:17, 338 s (5.6 min)** after boot. (Report:
"~7.5 min / 22:02"; epoch used.) Host: load 3.04–8.05, QEMU peak 176 %, vCPU threads 82 % / 36 %,
win2022 189 %.

### 2.4 What the cross-check says

| configuration | kernel | panic | guest time (epoch) | lock / owner |
|---|---|---|---|---|
| t38 T2 (`tb-size=4096`) | delivery | yes | 14.6 min | `_thread_call_groups+0x280` / `_thread_dispatch+0x534` |
| t38 control (`tb-size=1536`) | delivery | yes | ≈24 min | `_thread_call_groups+0x280` / `_idle_thread+0x64` |
| t48 A | v3 probe | yes | 909 s | `_pset0+0x100` / `_sched_timeshare_maintenance_continue+0x144` |
| t48 B | v3 probe | yes | 696 s | `_thread_call_groups+0x280` / `_thread_go+0x120` |
| t48 C | v3 probe | yes | 151 s | heap spinlock, value `0xe0fffff0` |
| **t50 split-wx** | delivery | **yes** | **309 s** | `_thread_call_groups+0x280` / `_sched_timeshare_init+0x160` |
| **t50 -cpu max (LSE)** | delivery | **yes** | **338 s** | `_pset0+0x500` / `_waitq_wakeup64_all_locked+0x34` (= t38 control) |

**Both H-A mechanisms are rejected.** `split-wx` changes how TCG splits memory operations and the
guest still dies in the same waitq/pset family; `-cpu max` switches the guest to LSE atomics and it
dies even faster, on the t38-control address. H-A therefore cannot be "LL/SC split" or "LL/SC vs
LSE"; as the statement **"MTTCG parallel execution is the enabling condition"** it still stands
(`thread=single` is stable). H-B (guest waitq/scheduler loop/lost wake-up) remains the main
manifestation. The next probe has to watch the guest's own locks, not the TCG atomics.

## 3. Probe v4 and its runs (t51/t53)

### 3.1 Probe v4: the real dualq scan, the loop body, and the ownerless lock path

Probe v4 is the v3 patch extended to **ten files** (`xnu/hakios-probe.patch`, 1,022 lines,
numstat **+490/−11**): `lock_ticket.c` +261, `waitq.c` +32, `sched_prim.c` +56, `processor.h`
+6/−2, `thread_call.c` +10, `sched_traditional.c` +31, `priority.c` +13, `lock_mtx.c` +8,
**`sched_dualq.c` +40**, **`locks.c` +33**. The panic summary grows to a **512-byte** buffer and
adds `i2# o2# pst# wwe# dt# lt#` after the v3 fields.

| stage ids | where |
|---|---|
| 130/131/132 | `sched_timeshare_maintenance_continue` loop body: snapshot `dt = sched_tick_delta` and `lt = late_time` / after `update_vm_info` / after `compute_averages` |
| 140/141/142 | `hw_lock`/`hw_wait_while_equals32` path: wait start / every 1,024 iterations / wait end (`wwe#` counts the iterations; `wwe_last_value`/`wwe_addr` are kept) |
| **150–157** | **`sched_dualq_thread_update_scan`**: entry / loop1 head before `pset_lock` / after `pset_unlock` / loop1 restart / loop2 head / loop2 unlock after / loop2 restart / exit; `i2#` = loop2 inner count, `o2#` = loop2 outer count, `pst#` = its `pset_lock` calls |
| 40–58 / 60–65 / 70–83 / 85–88 / 90–93 / 100–103 / 110–116 / 120–125 | v1–v3 marks unchanged |

`locks.c` also gives the plain spinlock and `hw_wait_while_equals32` panics the waiter's owner
context (`owner pset pc`, `owner stages`, `wwe#`), because the t48 run C panic had none; the v3
mutex owner context in `lock_mtx.c` is unchanged. The
maintenance loop body is now marked so a panic can read the two decision variables (`dt`, `lt`)
rather than only the entry/scan edges.

**Probe v4 kernel** md5 **`54db45a78fac270dc541f1fbff263bb2`**; symbols
`v4/kernel.syms` (53,378 lines, md5 **`1484f0dbd9a76ff5459d986dd96d41c3`**), `__start+0x0`
validated. Build logs: first 384-byte summary buffer, final 512-byte buffer; both 0 compile
errors / 0 warnings, `make` exit 2 is the known kext symbol-set check. The delivery kernel was
backed up, v4 installed 22:08:13, and **restored 23:42:54** (both tree and mt-work symlink
`f62c3d4b852d467f22c4703b8fc620c9`).

### 3.2 Single-vCPU self-check (`t53-selfcheck`, 1,500 s)

**0 panics.** The host acknowledged **838 frames**, of which **665 are 750×1624 desktop frames**
(335 on target texture 53, 327 on 51, 2 on 40, 1 on 48; the other 173 are small frames —
172 at 128×64 and one at 98×207). The last
sampled frame has **2,831,536 / 4,872,000 non-zero** bytes, the desktop's magnitude. One sampled
PPM (frame-000815) shows horizontal tearing — the host writes the PPM while the guest writes back
the IOSurface, so it is a read-back artifact, not a drawing regression (recorded, not hidden).
Captures: `selfcheck-desktop-750x1624.png` sha256 `a88a1957…` (375×812 `729f2895…`).

### 3.3 Run A: pset0, owner in a waitq wake (`t53-smp2-a`)

Boot 22:38:48; **2 frames**. Panic at **22:40:54 (126 s, 2.1 min)**; capture dir 22:41:10. (The t53
report's "~22:50 / 11 min" is not used; the epoch and capture agree.)

```
panic(cpu 1 caller 0xfffffff0072ebf44): Ticket spinlock[0xfffffff007b6d300] timeout after 60000000 ticks;
cticket: 0x6b, nticket: 0x6d, waiting for 0x6c,
pset last took it at 0xfffffff0072f77f8 (thread 0xfffffff8ca9b1ee0),
owner took it at 0xfffffff0073016e0,
select loops 123555, maint_s 0, stages 3/3/0/0/0/0/0/0, stage ring 5806482,
owner stages 71@0xfffffff0073016e0 70@0xfffffff0073016e0 82@0xfffffff00733ef6c
             81@0xfffffff00733ef6c 80@0xfffffff00733ef6c 86@0xfffffff00733edd0
             78@0xfffffff00733e94c 77@0xfffffff00733e94c
| recent 70 112 111 110 71 70 82 81 | pset#349266 stage#5806482
| scan0/0 out0/0 cpu0 rst0 qe#19249 m#967 i2#0 o2#0 pst#0 wwe#0 dt#1 lt#7810613,
current owner: 0xfffffff8ca971458 (on CPU 0), orig owner: 0xfffffff8ca971458,
start time: 8169299247, now: 8229299247, timeout: 60000000 @lock_ticket.c:647
```

Symbols: lock `_pset0+0x100`; pset last `_thread_quantum_expire+0x2b8`; owner take
**`_thread_go+0x120`**; stages 82/81/80 at `_waitq_resume_identified_thread+0x7c`, 86 at
`_waitq_wakeup64_identify_locked+0x54`, 78/77 at `_do_waitq_select_n_locked+0x38`. So this owner is
in the **waitq wake path** (thread_go during a resume), not in the maintenance loop. Host: QEMU
110–111 %, vCPU 97.8 % / 12.8 %, win2022 189 %, load 2.8–7.0.

### 3.4 Run B: pset0, owner in the maintenance loop (`t53-smp2-b`)

Boot 22:55:31; **441 frames** (the desktop was up). Panic at **23:08:35 (784 s, 13.1 min)**;
capture 23:08:51.

```
panic(cpu 0 caller 0xfffffff0072ebf44): Ticket spinlock[0xfffffff007b6d300] timeout after 60000022 ticks;
cticket: 0x88, nticket: 0x8a, waiting for 0x89,
pset last took it at 0xfffffff0073016e0 (thread 0xfffffff6baf12f70),
owner took it at 0xfffffff00730b0f8,
select loops 634434, maint_s 0, stages 3/3/0/0/0/0/0/0, stage ring 26703575,
owner stages 71@0xfffffff00730b0f8 70@0xfffffff00730b0f8 73@0xfffffff00730b0f8
             71@0xfffffff00730b0f8 70@0xfffffff00730b0f8 73@0xfffffff00730b0f8
             72@0xfffffff00730b0f8 71@0xfffffff00730b0f8
| recent 70 82 81 80 78 77 45 44 | pset#1583433 stage#26703575
| scan0/0 out0/0 cpu0 rst0 qe#80686 m#4784 i2#0 o2#0 pst#0 wwe#11 dt#8 lt#99999999,
current owner: 0xfffffff6baedfdc0 (on CPU 1), orig owner: 0xfffffff6baedfdc0,
start time: 49273246581, now: 49333246603, timeout: 60000000 @lock_ticket.c:647
```

Symbols: pset last `_thread_go+0x120`; owner take
**`_sched_timeshare_maintenance_continue+0x184`**; stages are the pset cycle
71/70/73/71/70/73/72/71 at that PC. Host: QEMU ~113 %, vCPU 56 % / 56 %, win2022 189 %, load
1.6–7.8.

### 3.5 Run C: an ownerless heap ticket (`t53-smp2-c`)

Boot 23:20:53; **340 frames**. Panic at **23:32:42 (709 s, 11.8 min)**; capture 23:33:00.

```
panic(cpu 1 caller 0xfffffff0072ebf44): Ticket spinlock[0xfffffff4e9944f40] timeout after 60000026 ticks;
cticket: 0x13, nticket: 0x15, waiting for 0x14,
pset last took it at 0xfffffff0072f77f8 (thread 0xfffffff31d3b83c8),
owner took it at 0,
select loops 610636, maint_s 0, stages 3/3/0/0/0/0/0/0, stage ring 25792078,
owner stages none | recent 113 114 73 72 71 70 112 111
| pset#1523463 stage#25792078
| scan0/0 out0/0 cpu0 rst0 qe#77285 m#4480 i2#0 o2#0 pst#0 wwe#1 dt#8 lt#99999999,
current owner: 0xfffffff31d50f7b8 (on CPU 0), orig owner: 0xfffffff31d50f7b8,
start time: 44689812062, now: 44749812088, timeout: 60000000 @lock_ticket.c:647
```

The lock is a **ticket lock in the heap region** (`0xfffffff4e9944f40`, nearest symbol
`_kd_early_buffer+0x4e19acf40`), not pset0; the pset-last ring has an entry
(`_thread_quantum_expire+0x2b8`), but **`owner took it at 0` and `owner stages none`** — the pset
ring has no record for the current owner, so the owner took this ticket through a path the probe
does not cover, or the owner id is already corrupt. Host: QEMU ~125 %, vCPU 61.7 % / 61.1 %,
win2022 188 %, load 1.4–7.6.

### 3.6 Symbol table (v4 syms `1484f0db…`)

| field | address | symbol + offset |
|---|---|---|
| run A/B lock | `0xfffffff007b6d300` | **`_pset0+0x100`** |
| run A pset last | `0xfffffff0072f77f8` | `_thread_quantum_expire+0x2b8` |
| run A owner take | `0xfffffff0073016e0` | **`_thread_go+0x120`** |
| run A stages 82/81/80 | `0xfffffff00733ef6c` | `_waitq_resume_identified_thread+0x7c` |
| run A stage 86 | `0xfffffff00733edd0` | `_waitq_wakeup64_identify_locked+0x54` |
| run A stages 78/77 | `0xfffffff00733e94c` | `_do_waitq_select_n_locked+0x38` |
| run B pset last | `0xfffffff0073016e0` | `_thread_go+0x120` |
| run B owner take | `0xfffffff00730b0f8` | **`_sched_timeshare_maintenance_continue+0x184`** |
| run C lock | `0xfffffff4e9944f40` | `_kd_early_buffer+0x4e19acf40` (heap) |
| ticket panic caller | `0xfffffff0072ebf44` | `_lck_grp_ticket_update_spin+0x0` |

## 4. Reading

* **The dualq scan is not the spot.** All three runs report `i2#/o2#/pst# = 0/0/0` — the
  `sched_dualq_thread_update_scan` loops (stages 150–157) never ran, and t51's "the 2.1 M pset
  takes come from the dualq scan" is **not confirmed** by this round. The pset ticket is taken in
  `_thread_go` (run A) and in `_sched_timeshare_maintenance_continue` (run B); those two are the
  live sites.
* **No tick stall.** `dt#` is 1/8/8 and `lt#` is 7,810,613 or the 99,999,999 sentinel — the
  maintenance loop's `sched_tick_delta` / `late_time` snapshots do not show a stopped clock. `qe#`
  and `m#` are ordinary for the run lengths. This weakens the "timer tick lost" variant of H-B.
* **The memory/atomic line is independent.** Run C's ownerless heap ticket (`owner 0`, no stages)
  and t48 C's plain spinlock with value `0xe0fffff0` are two different locks, two different
  rounds, both with a lock word/owner that cannot be explained by a normal acquire. That is the
  H-A form, and it is now the most direct evidence that some path writes a lock word without going
  through the instrumented acquire.
* **H-A status:** the two concrete mechanisms (split-wx, LL/SC→LSE) are rejected by t50; the
  enabling-condition statement remains; the dualq hypothesis is rejected by v4. The root is still
  unknown. H-B remains the main manifestation; the ownerless/garbage lock line is the strongest
  H-A-form evidence so far.
* **The delivery is unchanged.** Multi-core is still not deliverable; single vCPU remains the
  baseline; the probe kernel is not the delivery kernel.

## 5. What this does not establish

* **A root cause or a fix.** No kernel patch is part of this record; the ownerless lock is a
  symptom, not the writer.
* **That the dualq loops are never used at all.** The counters say they did not run in these three
  runs; they do not prove the dualq scan is never called in other phases (the boot may use it
  before the probe counters are reset).
* **A rate or a mechanism.** n=2 for t50 (one per variant) and n=3 for v4; the panic times
  (309/338 s in t50, 126/784/709 s in v4) vary by more than 6×, consistent with a timing-sensitive
  race. The hot-path stores and the `__builtin_return_address` per mark change timing.
* **That `-cpu max` only changes atomics.** It changes the whole CPU model/feature set; the LSE
  conclusion is "LSE does not remove the panic", not "no CPU-model difference matters".
* **A clean self-check picture.** The 750×1624 self-check frame has tearing from the host PPM
  read-back racing the guest's IOSurface write-back; the last-frame non-zero count is the
  quantitative check, the image is not a clean screenshot.
* **The probe kernel as a deliverable.** It is `54db45a7`, not `f62c3d4b`; the kernel tree still
  carries the probe patch, so a rebuild from that tree reproduces it.
* **A general multi-core result.** Still lavapipe/TCG, win2022 at 188–189 %, no isolation core, no
  V100; `isolcpus`/stopping win2022 still needs the user's approval.

## 6. Next steps

1. **Watch the ownerless lock.** v5 should instrument the heap-lock acquire paths that are not
   covered today (`lck_spin_lock` inline/macro variants, `lck_mtx_lock` fast path) and print the
   raw lock word on timeout, so run C's `owner 0` / t48 C's `0xe0fffff0` can be reproduced with a
   writer. This is the strongest H-A-form lead.
2. **Mark the thread_go pset iteration.** v4's 130–132 cover the maintenance loop body only; run A
   shows the owner in `_thread_go+0x120` — add an iteration count and the pset acquire/release
   sequence around that call site.
3. **Keep the two H-A variants closed.** `split-wx=on` and `-cpu max` are now measured negatives;
   don't repeat them as if untested. A `thread=single` run remains the diagnostic baseline, not a
   candidate.
4. **Delivery:** keep SMP>1 as an experimental feature with a startup guard that falls back to one
   vCPU, or leave it disabled; the single-vCPU path is unaffected. `isolcpus`/stopping win2022
   remains a separate, user-approved experiment.

## 7. Evidence and hashes

| item | value |
|---|---|
| reports | `/tmp/t50-evidence/T50-REPORT.md` sha256 `0740345c…`; `/tmp/t53-evidence/T53-REPORT.md` `8a221de9…` |
| delivery kernel (t50, unchanged) | md5 `f62c3d4b852d467f22c4703b8fc620c9` |
| probe v4 / delivery (restored) | md5 `54db45a78fac270dc541f1fbff263bb2` / `f62c3d4b…` (restored 23:42:54, both paths) |
| v4 patch | 1,022 lines, md5 `8f8e767b615f8dfe746d20fc3c963396`, sha256 `0cc2528d09ba0ec4390ca793d95fc23043e61433a293fd742e6f39a4f1c96873` |
| v4 build logs | `v4/kernel-build.log` `0eeeed7a…`; `v4/kernel-build2.log` `48822d33…` |
| v4 symbols | `v4/kernel.syms`, 53,378 lines, md5 `1484f0dbd9a76ff5459d986dd96d41c3` |
| t50 serials / hosts / monitors | split-wx `1f952123…` / `a457302c…` / `a0e00783…`; cpumax `76c3db15…` / `b65f1c2b…` / `e225ca0f…`; missing both `bb052cca…` |
| t53 self-check | serial `01d99e53…`; host `c74b03f1…`; missing `0e755695…`; PNG 750×1624 `a88a1957…`, 375×812 `729f2895…` |
| t53 A serial / host / missing / QMP | `abd2c4fb…` / `a5c8d5da…` / `bb052cca…` (`frames 2`) / `ca7a71c1…` |
| t53 B serial / host / missing / QMP | `211a0084…` / `1288a588…` / `9f972954…` (`frames 441`) / `8bfd09aa…` |
| t53 C serial / host / missing / QMP | `d1b31217…` / `0314c4e4…` / `416b9424…` (`frames 340`) / `b8523044…` |
| kernel-swap / monitors | `kernel-swap.txt` `db4c280f…`; `monitor-a` `27b41546…`, `monitor-b` `1ccf9691…`, `monitor-c` `a09287e1…` |
| host / glwire / image | md5 `ad778a06…` / `5b28249a…`; root `a4a6e2ce…` (marker OFF), system `49ae3cf4…`, libcainject `aa123ccd…`, plug-in `bcaf99c6…` |

### 7.1 Repository gates

`make test` is green at this tree: provenance clean (486 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 8. Files changed in this record

- `docs/stage48-h-a-crosscheck-v4.md` — this note.
- `docs/state-of-the-port.md` — the SMP>1 row gains the t50 negatives and the v4 result.
- `README.md` — the stage-48 milestone row, the EN+ZH multi-core table/paragraph updates, the
  EN+ZH status rows and the EN documentation index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the probe patch and kernels
live in the `iosvm-work` tree.

## 9. Reproduce

```sh
# t50: delivery kernel, no probe; split-wx and -cpu max variants
cd /home/lshan/DSH-desktop/mt-work/hakios
# QEMU_ACCEL="tcg,thread=multi,split-wx=on" QEMU_CPU=cortex-a76  (group 1)
# QEMU_ACCEL="tcg,thread=multi"             QEMU_CPU=max         (group 2)
# both: SMP=2, cpus=2, taskset 26-29, tb-size=1536, 1800 s

# v4: build in the pre-patched kernel tree (same fixed cctools linker)
cd /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
make kernel -j16 BUILD_DSYM=0          # exit 2 = known kext symbolset check
md5sum hakios-kernel                   # 54db45a78fac270dc541f1fbff263bb2

# install v4 and boot SMP=2; read a panic's fields from the serial log:
python3 /tmp/t5-probe-backup/v4/sym.py 0xfffffff007b6d300 0xfffffff0073016e0 \
    0xfffffff00730b0f8 0xfffffff0072f77f8 0xfffffff4e9944f40 0xfffffff0072ebf44

# restore the delivery kernel
cp /tmp/t5-probe-backup/hakios-kernel.tree.f62c3d4b ...
md5sum hakios-kernel                   # f62c3d4b852d467f22c4703b8fc620c9
```

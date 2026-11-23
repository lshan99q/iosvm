# stage 47: probe v3 — three forms, and the scan loop is not it

> Status: **measured. Probe v3 boots and passes a single-vCPU self-check; three SMP=2 runs show three
> different forms of the same failure family.** Run A is a ticket timeout on `_pset0+0x100` with the
> owner looping on the pset ticket at `_sched_timeshare_maintenance_continue+0x144`; run B is a
> ticket timeout on `_thread_call_groups+0x280` with the owner at `_thread_go+0x120` in a waitq
> wake; run C is a **plain spinlock timeout whose lock value is `0xe0fffff0` — a garbage value, not
> 0 or 1**. The new v3 counters are the key: both ticket runs report
> `scan0/0 out0/0 cpu0 rst0`, so the `sched_traditional_thread_update_scan` inner/outer loop is
> **not** where the owner spins; `pset#/m#` is **369–382**, far above the two CPUs, so the pset
> ticket is being taken and released repeatedly at one site. With run C's corrupted lock value and
> the earlier mutex timeout, the round **leans H-A (parallel TCG breaks atomics/memory ordering) as
> the root and H-B (guest waitq/scheduler loop) as the manifestation**. That is a direction, not a
> proof. Multi-core is still not deliverable; single vCPU remains the baseline. Probe v3
> (`6cb27edf…`) is not the delivery kernel — `f62c3d4b` was restored at 21:33:24 and checked on both
> paths. Branch `experiment/ca-metal-device`, 2026-11-23. This continues
> [`docs/stage46-waitq-deep-probe.md`](stage46-waitq-deep-probe.md).

## 1. The question

Stage 46's v2 caught the owner on the pset ticket in `_sched_timeshare_maintenance_continue`, but
the eight stage entries could not say which loop that was, whether the loop was in the maintenance
scan, or what its exit condition depended on. Run A of t45 was a mutex timeout with **no** stage
history at all. T5 v3 asks:

1. **Where exactly is the maintenance spin?** Add stage/counter marks at the maintenance entry, the
   `SCHED(thread_update_scan)` call, the scan's inner-loop head (before `pset_lock`), after
   `pset_unlock`, the outer restart, the exit, and `thread_update_process_threads`.
2. **Is the loop in the scan?** If the scan's own counters stay zero, the inner/outer scan loops are
   excluded and the spin must be in the maintenance continuation around the pset lock itself.
3. **What about the other lock classes?** Give the mutex timeout the same owner context, so a mutex
   timeout can name its owner's stages too.
4. **Do the forms share a cause?** A/B are ticket timeouts on different locks; C is an ordinary
   spinlock with an impossible value. The round should record whether the forms are the same family.

## 2. Implementation

### 2.1 Probe v3: counters and the scan marks

Probe v3 extends the v2 patch to **eight files** (`xnu/hakios-probe.patch`, 779 lines, numstat
**+382/−4**): `lock_ticket.c` +235, `waitq.c` +32, `sched_prim.c` +47, `processor.h` +6/−2,
`thread_call.c` +10, **`sched_traditional.c` +31**, **`priority.c` +13**, **`lock_mtx.c` +8**. The
stage ring is unchanged (64 entries, thread/pc/stage), but the panic text buffer grows to **384
bytes** (the first build used 320 bytes and was too small for the extra counters; the final build
uses 384) and the summary line gains new fields after `recent`/`pset#`/`stage#`:

```
| scan<inner>/<inner_max> out<outer>/<outer_max> cpu<cpu_id> rst<restart> qe#<n> m#<n>
```

| stage ids | where |
|---|---|
| **100/101/102/103** | `sched_timeshare_maintenance_continue`: entry / before `SCHED(thread_update_scan)` / after it / before the closing `assert_wait`; plus `hakios_maint_calls` (m#) |
| **110/111/112/113** | `thread_quantum_expire`: entry / before `SCHED(quantum_expire)` / after / exit; plus `hakios_qe_calls` (qe#) |
| **114/115/116** | `sched_timeshare_consider_maintenance`: entry / the `thread_wakeup` decision / the maintenance thread's own early return |
| **120/121/122/123/124/125** | `sched_traditional_thread_update_scan`: entry / inner-loop head before `pset_lock` / after `pset_unlock` / outer restart / exit / `thread_update_process_threads`; plus `hakios_scan_inner/inner_max/outer/outer_max/cpu/restart` and the scan-call counter |
| 40–58 / 60–65 / 70–83 / 85–88 / 90–93 | v1/v2 marks (waitq pull/wakeup/flush, idle, pset+waitq tickets, `thread_go`, `do_waitq_select_n_locked`, `thread_call`) unchanged |

`lock_mtx.c` now gives the **mutex** timeout the same owner context: it prints
`owner pset pc: … owner stages …` using the exported `hakios_stage_summary()` /
`hakios_pset_pc_for()`. The ticket panic keeps printing the owner's last eight stages plus the raw
`recent` ids and the counters above.

### 2.2 Build and restore chain

* probe v3 kernel md5 **`6cb27edfb6aab33fb1c57959efec7c2c`** (v2 `1bce762b…`, v1 `eaf71509…`,
  delivery `f62c3d4b…`);
* v3 symbols `v3/kernel.syms` (53,370 lines, `llvm-nm -n`, md5
  **`389a62d405287b39b0beda845797cf24`**); `sym.py` sanity checks
  `0xfffffff00745f4a8 → __start+0x0` and `0xfffffff0072e9ddc → _hakios_stage+0x0`;
* build logs `v3/kernel-build.log` (first, 320-byte buffer) and `v3/kernel-build2.log` (final,
  384-byte buffer), both 0 compile errors / 0 warnings; `make` exit 2 is the known kext symbol-set
  check;
* `kernel-swap.txt`: delivery `f62c3d4b` at 20:07 `--before` → backup → **probe v3 `6cb27edf`
  installed 20:07:06** → runs → **delivery restored 21:33:24**, both the tree binary and the
  mt-work symlink `f62c3d4b852d467f22c4703b8fc620c9`.

The patch was applied to a clean `apply.sh` baseline (`v3/applytest2`) and the eight files' md5
matched the working tree (t47). The working tree still carries the probe; the booted kernel after
the round is the delivery binary.

## 3. Measurements

### 3.1 Single-vCPU self-check (`t48-selfcheck`, 1,500 s)

Booted 20:07:56; **0 panics**, **1,478 desktop frames at 750×1624**, desktop **Dock 66.3 / pure
white 13,199 / Search 13.6** — identical to the delivery baseline. **PASS.** The monitor (94
samples) has QEMU up to 178 %, vCPU threads up to 95.9 %, load 2.2–9.7. Captures:
`selfcheck-desktop-750x1624.png` sha256 `d2522f29…` (375×812 `fd25132c…`); host log
`t48-selfcheck-metalwire-host.log` `25d5929e…` (`client disconnected after 1478 frame(s)`), missing
`49b17c75…` (`frames 1478, pipelines 0, skipped draws 0`).

### 3.2 Run A: the pset ticket, with the owner in the maintenance loop (`t48-smp2-a`)

Booted 20:35:36, 1,800 s window, SMP=2 / `cpus=2`, `tb-size=1536`, `thread=multi`,
`taskset 26–29`; **1,485 frames acknowledged** (the desktop was up). Panic:

```
panic(cpu 0 caller 0xfffffff0072ebbd4): Ticket spinlock[0xfffffff007b6d300] timeout after 60000008 ticks;
cticket: 0x75, nticket: 0x77, waiting for 0x76,
pset last took it at 0xfffffff0073010a0 (thread 0xfffffff6f8120c10),
owner took it at 0xfffffff00730aa78,
select loops 821078, maint_s 0, stages 3/3/0/0/0/0/0/0, stage ring 34389016,
owner stages 71@0xfffffff00730aa78 70@0xfffffff00730aa78 73@0xfffffff00730aa78 72@0xfffffff00730aa78
             71@0xfffffff00730aa78 70@0xfffffff00730aa78 73@0xfffffff00730aa78 72@0xfffffff00730aa78
| recent 70 82 81 80 78 77 45 44 | pset#2105619 stage#34389016
| scan0/0 out0/0 cpu0 rst0 qe#97020 m#5714,
current owner: 0xfffffff6f8123578 (on CPU 1), orig owner: 0xfffffff6f8123578,
start time: 57274156204, now: 57334156212, timeout: 60000000 @lock_ticket.c:621
```

The panic header's own epochs put it at **20:50:46, 909 s (15.1 min) after boot**; the capture
directory is stamped 20:51. (The t48 report's "~20:53 / 17.5 min" does not match either; the epoch
and capture times are used here.) Host: QEMU up to 178 %, vCPU threads up to 88.7 % (the report's
sample: 110–122 % / 60 %), win2022 189 %, load 2.0–12.6; the QMP capture is CPU0-only.

### 3.3 Run B: the thread_call ticket, with the owner in a waitq wake (`t48-smp2-b`)

Booted 20:57:50; **3 frames acknowledged** (the desktop did not appear). Panic:

```
panic(cpu 0 caller 0xfffffff0072ebbd4): Ticket spinlock[0xfffffff007b11380] timeout after 60000025 ticks;
cticket: 0x7c, nticket: 0x7e, waiting for 0x7d,
pset last took it at 0xfffffff0073010a0 (thread 0xfffffff4db30fdc0),
owner took it at 0xfffffff0073010a0,
select loops 603750, maint_s 0, stages 3/3/0/0/0/0/0/0, stage ring 25215279,
owner stages 58@0xfffffff00733e658 58@0xfffffff00733e658 58@0xfffffff00733e658 57@0xfffffff00733e658
             83@0xfffffff00733e458 73@0xfffffff0073010a0 72@0xfffffff0073010a0 71@0xfffffff0073010a0
| recent 85 81 80 56 55 53 78 77 | pset#1607094 stage#25215318
| scan0/0 out0/0 cpu0 rst0 qe#98869 m#4208,
current owner: 0xfffffff4db3e6728 (on CPU 1), orig owner: 0xfffffff4db3e6728,
start time: 43986995053, now: 44046995078, timeout: 60000000 @lock_ticket.c:621
```

Panic at **21:09:26, 696 s (11.6 min)** after boot; capture stamped 21:09. (Report: "~21:16 /
18.3 min" — not used.) Host: QEMU up to 174 %, vCPU up to 87.5 %, win2022 189 %, load 1.5–7.7.

### 3.4 Run C: a plain spinlock with a garbage value (`t48-smp2-c`)

Booted 21:22:01; **2 frames acknowledged**. Panic:

```
panic(cpu 0 caller 0xfffffff0072ebf84): Spinlock[0xfffffff0baac5134] timeout after 251658296 ticks;
current value: 0xe0fffff0, start time: 9358892656, now: 9610550952,
timeout: 251658240 @locks.c:514
```

Panic at **21:24:32, 151 s (2.5 min)** after boot; capture stamped 21:24. (Report "~21:30 /
8.5 min" — not used.) Host: QEMU up to 175 %, vCPU up to 89.8 %, win2022 189 %, load 2.3–6.6. This
is a **different lock class** (ordinary `hw_lock` spinlock, HW-spin timeout 251,658,240 — the same
constant as the t45 mutex timeout, ≈4.2× the 60 M ticket timeout), the lock address
is outside the kernel text (nearest symbol `_kd_early_buffer+0xb2b2d134`, a heap region), and
**`0xe0fffff0` is not a legal spinlock value** — neither held (1) nor free (0).

### 3.5 Symbol table (v3 syms `389a62d4…`)

Re-symbolised here with the v3 `sym.py`:

| field | address | symbol + offset |
|---|---|---|
| run A lock | `0xfffffff007b6d300` | **`_pset0 + 0x100`** |
| run A `pset last` | `0xfffffff0073010a0` | **`_thread_go + 0x120`** |
| run A owner take + 8 stages | `0xfffffff00730aa78` | **`_sched_timeshare_maintenance_continue + 0x144`** |
| run B lock | `0xfffffff007b11380` | **`_thread_call_groups + 0x280`** (the t42 v1 lock) |
| run B `pset last` / owner take | `0xfffffff0073010a0` | **`_thread_go + 0x120`** |
| run B stages 58/57 | `0xfffffff00733e658` | **`_waitq_wakeup64_one_locked + 0xd8`** |
| run B stage 83 | `0xfffffff00733e458` | **`_waitq_select_queue_flush + 0x134`** |
| run B stages 73/72/71 | `0xfffffff0073010a0` | `_thread_go + 0x120` |
| run C lock | `0xfffffff0baac5134` | `_kd_early_buffer + 0xb2b2d134` (heap; value `0xe0fffff0`) |
| run C caller | `0xfffffff0072ebf84` | **`_hw_wait_while_equals32 + 0x0`** |
| ticket panic caller (A/B) | `0xfffffff0072ebbd4` | `_lck_grp_ticket_update_spin + 0x0` |

The stage ids decode as before: 70/71 = pset ticket waiting/held, 72/73 = release before/after,
80–83 = `thread_go` entry/unblock/leave, 55–58 = `waitq_select_queue_flush`/`thread_go` edges,
85–88 = `do_waitq_select_n_locked`. In run A all eight owner entries are the pset cycle
`71 (held), 70, 73, 72, 71, 70, 73, 72` at the same PC; in run B the owner's newest entries are the
waitq flush/thread_go marks and only the older three are the pset cycle.

## 4. Reading the three forms

### 4.1 What the counters rule out

* **`scan0/0 out0/0 cpu0 rst0` in both ticket runs.** The scan counters are reset at
  `sched_traditional_thread_update_scan` entry and updated in its inner/outer loop; all zero means
  the scan path (stages 120–125) never ran, and therefore the owner's spin is **not** the
  `thread_update_scan` inner loop or its outer restart. The maintenance spin is at the pset-ticket
  level (`_sched_timeshare_maintenance_continue+0x144` in run A).
* **`m#5714/4208` maintenance calls and `qe#97020/98869` quantum expiries** are ordinary magnitudes
  for the run lengths (≈6–7 maintenance calls/s, ≈17 (A) / 23 (B) quantum expiries per maintenance call); the
  round does not show these paths being starved as a whole.
* **`pset#/m#` = 2,105,619/5,714 ≈ 369 (A) and 1,607,094/4,208 ≈ 382 (B).** That is two orders of
  magnitude above the two CPUs: the pset ticket is taken and released repeatedly at one site,
  matching the owner's `71/70/73/72` stages. (`pset#` is the global pset-acquisition ring index, not
  one thread's count; the ratio is the useful part.)

### 4.2 The three forms and the family

| | v3 run A | v3 run B | v3 run C |
|---|---|---|---|
| type | ticket | ticket | **ordinary spinlock** |
| panic lock | `_pset0+0x100` | `_thread_call_groups+0x280` | `_kd_early_buffer+0xb2b2d134` (heap) |
| owner | `_sched_timeshare_maintenance_continue+0x144` | `_thread_go+0x120` | — (no stage print) |
| value | ticket 0x76 waited / 0x75 owner | ticket 0x7d waited / 0x7c owner | **0xe0fffff0 (garbage)** |
| scan counters | all 0 | all 0 | — |
| frames acked | 1,485 | 3 | 2 |
| panic time (epoch) | 20:50:46 / 909 s | 21:09:26 / 696 s | 21:24:32 / 151 s |

The two ticket runs land in the scheduler/lock family already seen in v1/v2 (waitq wake and
timeshare maintenance), on two different ticket instances. Run C is the third form: not a ticket,
not 0/1, and not an owner that can be named. A pure lock-order inversion explains A and B as
"someone holds a lock and waits for another", but it does not explain a spinlock word that has
become `0xe0fffff0`. That is why the round **leans H-A (parallel TCG corrupts the atomics/memory
ordering, and the guest then spins on a broken word) as the root, with H-B (waitq/scheduler loops
and lost wake-ups) as the visible manifestation**. It remains a hypothesis: run C is one sample, and
no probe has yet watched the word being written.

### 4.3 v2 → v3

| | v2 (`t45-smp2-b`) | v3 run A | v3 run B |
|---|---|---|---|
| owner site | `_sched_timeshare_maintenance_continue+0xf4` | `_sched_timeshare_maintenance_continue+0x144` (same function; v3 code shifted the offset) | `_thread_go+0x120` |
| panic lock | `_pset0+0x100` | `_pset0+0x100` | `_thread_call_groups+0x280` |
| pset# / ring | 1,800,157 / 26,712,436 | 2,105,619 / 34,389,016 | 1,607,094 / 25,215,318 |
| scan/out/cpu/rst | not instrumented | **0/0 0/0 0 0** | **0/0 0/0 0 0** |
| qe# / m# | not instrumented | 97,020 / 5,714 | 98,869 / 4,208 |

The raw counters scale with run length (A ran 909 s vs v2's 800 s); the new information is the
scan/qe/m split, not the growth. The v2 `+0xf4` and v3 `+0x144` are the same function — v3 adds
marks inside it, so the offset moves.

## 5. What this does not establish

* **A root cause or a fix.** The round names the loops, the locks and the impossible value; it does
  not show the write that corrupts the spinlock, nor prove the TCG atomic hypothesis. No kernel
  patch is part of this record.
* **That run C is the same bug.** It is a different lock class, one sample, no stage history. It is
  the strongest hint toward H-A, not proof.
* **That the scan counters are a clean exclusion.** They are plain `unsigned` increments (racy by
  design) and are reset per scan; "all zero" says the marked scan path did not run, not that no
  other scan variant exists.
* **Probe neutrality or rate.** The v3 marks add stores to the maintenance/scan/quantum-expire and
  lock paths; timing and therefore the race probability change. The three runs are n=1 per form and
  the panic times (909 s / 696 s / 151 s) already vary widely.
* **A partial capture pipeline.** Each run's `panic-block.txt` (4.8–6.0 KB) and
  `panic-probe-fields.txt` exist, but the root `paniccap-*.log` files are empty; the QMP captures
  contain CPU0 only (its PC is outside the kernel text in all three runs), so the guest's ticket
  line remains the owner record. The quoted panic lines were read directly from the serial logs.
* **The probe kernel as a deliverable.** It is `6cb27edf`, not `f62c3d4b`; the kernel tree still
  carries the probe patch, so a rebuild from that tree reproduces it.
* **A general multi-core result.** Still lavapipe/TCG, win2022 at 189 %, no isolation core, no V100.

## 6. Next steps

1. **Watch the broken word.** Give `hw_lock`/ordinary spinlock (`locks.c`) and the mutex path the
   same owner context as the ticket (PC + eight stages), and, for run C's form, record the lock's
   value and the last writer if the hardware supports a watchpoint — a single sample of
   `0xe0fffff0` is not enough to call it corruption.
2. **Instrument the maintenance loop body.** The v3 marks stop at the entry/scan edges; the pset
   `lock`/`unlock` inside `_sched_timeshare_maintenance_continue` needs an iteration sequence and the
   values of the loop's condition variables, so the next panic can say whether the loop is
   unbounded or merely long.
3. **Cross-check the TCG hypothesis.** Re-run the same probe with `split-wx=on` and `-cpu max`; if
   the garbage value or the spin disappears/changes with the translation mode, that supports H-A.
4. **Keep SMP>1 experimental.** The delivery stays one vCPU; a startup guard that falls back to one
   vCPU is the honest delivery shape if the root is not found. `isolcpus`/stopping win2022 still
   needs the user's approval.

## 7. Evidence and hashes

| item | value |
|---|---|
| report | `/tmp/t48-evidence/T48-REPORT.md` sha256 `76d553896dd6a6990fcce2de810f46f3a1e1631ff153b982e9f38241bdabb8e8` |
| probe v3 / delivery kernel | md5 `6cb27edfb6aab33fb1c57959efec7c2c` / `f62c3d4b852d467f22c4703b8fc620c9` (restored 21:33:24, both paths) |
| v3 patch | `hakios-xnu-kernel/xnu/hakios-probe.patch`, 779 lines, md5 `ab7050682a19fa9bdc1ebae6f8e3bdd7`, sha256 `278fef9c8abf4ec6f63d468e401f3872566c29d3e070c9c2754092d1c43696b7` |
| build logs | `v3/kernel-build.log` sha256 `fe2e474c89e7a4b1a90c943534afe629c2a9e38158f29ef96d4c2111b3c36831`; `v3/kernel-build2.log` `068bf84dac560f9a99148bf9a660e37f87e29e648d76fe96363d266f6d862c67` |
| v3 symbols | `v3/kernel.syms`, 53,370 lines, md5 `389a62d405287b39b0beda845797cf24` |
| self-check | serial `38e3fad2a83185a6c38d20ab3d1ac5c95ea7a3464e9d9e94798e5bf7cd435ff6`; host `25d5929e4aee3f947b2a4c12ae8cedfed446ac69fa2e60abde25f80b01ee1e2f`; missing `49b17c756e61931e4d4a8692141e0a053caca6ffaaa70e0340ee3528220844fa`; PNG 750×1624 `d2522f29…`, 375×812 `fd25132c…` |
| run A serial / host / missing / QMP | `d871fa2701170e722210fb9e05ac8495a0068407b67d3595b12c4b5e53222f58`; host `0d5ecc65…`; missing `a820a460…` (`frames 1485`); QMP `5659526f…` |
| run B serial / host / missing / QMP | `d6c95526024e61ef1f798525e5994341e2dadfba89cf17494097ac720fa0bd77`; host `316467b9…`; missing `6e048eadeec068aa…` (`frames 3`); QMP `cf981de9…` |
| run C serial / host / missing / QMP | `ccaf888dd525fff9626e442d87cab498854346c7de40fb0559b4acce9f6526c8`; host `1cf4b937…`; missing `bb052cca…` (`frames 2`); QMP `acb262ed…` |
| kernel-swap / monitors | `kernel-swap.txt` `ff847a21b6c0af3e…`; `monitor-a` `ba38c739…`; `monitor-b` `cd86cf04…`; `monitor-c` `55f4652f…` |
| host / glwire / image | md5 `ad778a06…` / `5b28249a…`; root `a4a6e2ce…` (marker OFF), system `49ae3cf4…`, libcainject `aa123ccd…`, plug-in `bcaf99c6…` |

### 7.1 Repository gates

`make test` is green at this tree: provenance clean (485 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 8. Files changed in this record

- `docs/stage47-v3-three-forms.md` — this note.
- `docs/state-of-the-port.md` — the SMP>1 row gains the v3 three forms and the H-A/H-B reading.
- `README.md` — the stage-47 milestone row, the EN+ZH multi-core table/paragraph updates, the
  EN+ZH status rows and the EN documentation index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the probe patch and kernels
live in the `iosvm-work` tree.

## 9. Reproduce

```sh
# build probe v3 in the pre-patched kernel tree (same fixed cctools linker as stages 45-46)
cd /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
# the probe patch is already applied here; a clean apply is xnu/hakios-probe.patch (8 files)
make kernel -j16 BUILD_DSYM=0          # exit 2 = known kext symbolset check
md5sum hakios-kernel                   # 6cb27edfb6aab33fb1c57959efec7c2c

# install the probe and boot SMP=2 (tb-size=1536, thread=multi, taskset 26-29)
# then read a panic's owner stages from the serial log:
python3 /tmp/t5-probe-backup/v3/sym.py 0xfffffff007b6d300 0xfffffff0073010a0 \
    0xfffffff00730aa78 0xfffffff00733e658 0xfffffff00733e458 0xfffffff0072ebf84 \
    0xfffffff0baac5134

# restore the delivery kernel
cp /tmp/t5-probe-backup/hakios-kernel.tree.f62c3d4b ...
md5sum hakios-kernel                   # f62c3d4b852d467f22c4703b8fc620c9
```

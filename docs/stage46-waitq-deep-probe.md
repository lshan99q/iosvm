# stage 46: the waitq deep probe (v2) — the owner loops on the pset ticket

> Status: **measured. Probe v2 boots and passes a single-vCPU self-check; two SMP=2 runs give a
> mutex-timeout and a ticket-timeout, and the second one captured the owner's last eight stages —
> every one of them at `_sched_timeshare_maintenance_continue+0xf4`, in the pset ticket's
> acquire/release wrapper.** The panic lock is `_pset0+0x100` (`0xfffffff007b69280`), the same lock
> and function as stage 27's T6 result; the last taker is `_thread_quantum_expire+0x26c`. The owner
> is not sleeping after taking the lock: its stages (newest first) are `71 = held`, `70 =
> waited`, `73 = released`, `72 = release`, repeating, with the newest entry **71 = held**, and the global counters reached
> **pset#1,800,157 / stage ring 26,712,436**. That is the signature of the timeshare-maintenance loop
> not getting out (lost wake-up / exit condition not becoming true under MTTCG, H-B), not of a simple
> lock-order swap. Multi-core is still not deliverable; single vCPU remains the baseline. The probe
> kernel v2 (`1bce762b…`) is not the delivery kernel — `f62c3d4b` was restored at 19:48:22 and
> checked on both paths. Branch `experiment/ca-metal-device`, 2026-11-23. This continues
> [`docs/stage45-t5-probe.md`](stage45-t5-probe.md).

## 1. The question

Stage 45's v1 probe located the owner in the waitq wake path
(`_waitq_select_queue_flush+0x134`, `thread_go` never returning). That is a site, not a reason: the
probe could say where the owner was, but not what it did next, and it had no marks on the other
ticket locks of the same scheduler path. T5 v2 asks three narrower questions:

1. **Does the owner forward through `thread_go` / `do_waitq_select_n_locked`, or does it loop
   there?** v1's site is inside the wake path; v2 marks the entry, the queue scan, the sets, the
   `thread_go` entry/unblock/leave, and `waitq_select_queue_flush`'s entry/leave.
2. **What is the lock order?** v2 marks the pset ticket (acquire wait/held, release before/after),
   the waitq ticket (wait/lock/reserve/unlock), and `thread_call`'s ticket (90–93), so the stage ring
   can be read as a time-ordered acquire/release trace.
3. **Is the panic on one lock or a family?** v1 and t38 had different panic locks and owner sites;
   v2 should tell whether the pset lock is the common one and whether the owner keeps cycling it.

## 2. Implementation

### 2.1 Probe v2: one ring, six new lock/loop families

Probe v2 is the v1 patch extended to **five files** (`xnu/hakios-probe.patch`, 550 lines,
+279/−2: `lock_ticket.c` +198, `waitq.c` +32, `sched_prim.c` +33, `processor.h` +6/−2,
`thread_call.c` +10). Everything still goes into the same 64-entry `hakios_stage_ring`
(`thread`, `pc`, `stage`, `reserved`); the panic formatter walks it backwards and prints, for the
owner, the newest eight `stage@pc` values, followed by the raw recent stage ids and two counters:
`| recent … | pset#… stage#…`.

| stage ids | where |
|---|---|
| 40–45 | `waitq_pull_thread_locked`: entry / reserve / `waitq_lock_wait` before+after / remove / unlock (v1) |
| 50–54 | `waitq_wakeup64_all_locked`: entry / `do_waitq_select_n_locked` before+after / `waitq_select_queue_flush` before+after (v1) |
| 55–58 | `waitq_select_queue_flush`: entry / `thread_go` before / after / leave (55–57 v1, 58 new) |
| 60–65 | `idle_thread`: entry / `spllo` / `processor_idle` before+after / `thread_run` / `thread_block` (v1) |
| **70–73** | pset ticket: `hakios_pset_lock` **waiting / held**, `hakios_pset_unlock` **release before / after** — `processor.h`'s `pset_lock`/`pset_unlock` now route through the probe |
| **74–79** | waitq ticket: `waitq_lock_wait` / `waitq_lock` before/held / `waitq_unlock` before/after / `waitq_lock_reserve` |
| **80–83** | `thread_go`: entry / `thread_unblock` before / unblock succeeded / leave |
| **85–88** | `do_waitq_select_n_locked`: entry / after the queue scan / before the sets / leave |
| **90–93** | `thread_call_groups`: `thread_call_lock_spin` before/held, `thread_call_unlock` before/after |

The build log's first attempt (`v2/kernel-build.log`) failed on a macro scope in
`HAKIOS_IDLE_STAGE`; the fix moved the `extern` into the macro, and the second build
(`v2/kernel-build2.log`, 112,607 bytes) is clean: 0 compile errors / 0 warnings, with `make`'s
exit 2 being the known kext symbol-set check of this configuration. The build used the same fixed
toolchain as stage 45 (cctools `ld64`; `LC_MAIN` → `LC_UNIXTHREAD` is a linker choice).

**Probe kernel v2** md5 **`1bce762b2ebc0a54c202391f37754d25`** (v1 `eaf71509…`, delivery
`f62c3d4b…`). The v2 symbol table is `v2/kernel.syms` (53,360 lines, `llvm-nm -n`, md5
`5ed0ffffe713614a66caa27315ddc21f`); `sym.py` sanity-checks `0xfffffff00745e4a8 → __start+0x0`
and `0xfffffff0072e9dc0 → _hakios_stage+0x0`. The patch was applied to a clean `apply.sh` baseline
(`v2/applytest2`) and the five files' md5 matched the working tree (t44).

### 2.2 The build/restore chain

Delivery kernel backed up before installing the probe (`kernel-swap.txt`): delivery
`f62c3d4b` at 18:27 `--before` → backup → **probe v2 `1bce762b` installed 18:27:10** → runs → **delivery
restored 19:48:22**, both the tree binary and the mt-work symlink checked as
`f62c3d4b852d467f22c4703b8fc620c9`.

## 3. Measurements

### 3.1 Probe v2 single-vCPU self-check

* `t45-selfcheck` (900 s, 18:28): **0 panic**, but under load 5.4–9.9 it only reached the BootUI
  (ack 3) in the window — recorded as a slow-start observation, not a failure.
* `t45-selfcheck2` (18:39): **void** — the previous QEMU still held monitor port 4474
  (`Failed to find an available port: Address already in use`), no guest.
* `t45-selfcheck3` (18:40:17, **1,500 s**, extended because 900 s was not enough under the load):
  **0 panic**, **1,888 desktop frames at 750×1624**, desktop **Dock 66.3 / pure white 13,199 /
  Search 13.6** — identical to the delivery kernel's baseline. **PASS.**
  Evidence note: the successful run's serial log is `mt-work/logs/t45-selfcheck3/serial.log`
  (5,742 `750x1624` lines, 0 panic lines), while its host-side files kept the
  `t45-selfcheck2-` prefix from the retry (`client disconnected after 1888 frame(s)`, missing
  `frames 1888, pipelines 0, skipped draws 0`); the desktop PNGs are
  `3c710a3b…` (750×1624) and `5a4e620b…` (375×812).

### 3.2 SMP=2 run A: a mutex timeout (`t45-smp2-a`)

Booted 19:13:37, 1,800 s window, SMP=2 / `cpus=2`, `tb-size=1536`, `thread=multi`,
`taskset 26–29`. The panic is **a different lock class**:

```
panic(cpu 1 caller 0xfffffff0072e5858): Mutex interlock[0xfffffff007d996a8] timeout after 251658242 ticks;
current owner: 0xfffffff623e29698, <0x000000 0x22 0x000000a7 0x30091610 0x4001 0x4001>,
start time: 13589295854, now: 13840954096, timeout: 251658240 @lock_mtx.c:320
```

No `owner stages` on this path (the v2 stage printer is on the ticket-lock path, not the mutex
path). The host had **2 frames** (desktop never appeared); QEMU peaked at **172 %**, the two vCPU
threads around **60 % / 58 %** (monitor max 86.7 %), win2022 **190 %**, load **1.7–5.5**. The panic
header's own epochs put it at **19:17:17**, **220 s** after boot; the capture file is stamped 19:17
(the t45 report's "~11 min" does not match either clock and is not used).

### 3.3 SMP=2 run B: the ticket timeout with owner stages (`t45-smp2-b`)

Booted 19:25:53, same configuration. Panic (the guest printed the block twice):

```
panic(cpu 0 caller 0xfffffff0072eaecc): Ticket spinlock[0xfffffff007b69280] timeout after 60000043 ticks;
cticket: 0x26, nticket: 0x28, waiting for 0x27,
pset last took it at 0xfffffff0072f6e40 (thread 0xfffffff6e5df8e50),
owner took it at 0xfffffff00730a640,
select loops 671716, maint_s 0, stages 3/3/0/0/0/0/0/0, stage ring 26712436,
owner stages 71@0xfffffff00730a640 70@0xfffffff00730a640 73@0xfffffff00730a640 72@0xfffffff00730a640
             71@0xfffffff00730a640 70@0xfffffff00730a640 73@0xfffffff00730a640 72@0xfffffff00730a640
| recent 70 71 70 73 72 71 70 73 | pset#1800157 stage#26712436,
current owner: 0xfffffff6e5df9698 (on CPU 1), orig owner: 0xfffffff6e5df9698,
start time: 50265196420, now: 50325196463, timeout: 60000000 @lock_ticket.c:584
```

The host had **217 frames** (the desktop came up and rendered before the panic); QEMU up to
**171 %**, vCPU threads 62.8 % / 52.7 % at the reported sample (monitor max 86.6 %), win2022
**189 %**, load 1.9–7.6. The panic header's epochs put it at **19:39:13**, **800 s** after boot; the
capture file is stamped 19:39. (The t45 report's "~21.5 min" is inconsistent with both and is not
used.)

### 3.4 Symbolization (v2 syms `5ed0ffff…`)

The three addresses were re-symbolised here with `sym.py` over the v2 table:

| field | address | symbol + offset |
|---|---|---|
| panic lock | `0xfffffff007b69280` | **`_pset0 + 0x100`** — the pset0 ticket lock |
| `pset last took it` | `0xfffffff0072f6e40` | **`_thread_quantum_expire + 0x26c`** |
| `owner took it` + all eight owner stages | `0xfffffff00730a640` | **`_sched_timeshare_maintenance_continue + 0xf4`** |
| panic caller | `0xfffffff0072eaecc` | `_hakios_stage_text_for + 0x0` (probe formatter) |

The stage ids in the owner line decode as **70 = pset ticket waiting, 71 = held, 72 = release
before, 73 = release after**. The owner's newest entry is **71 (held)**; older entries read
`70, 73, 72, 71, 70, 73, 72`: the same call site taking and releasing the same pset ticket, over and
over, ending in a hold. The `recent` line (all threads, newest first) is
`70 71 70 73 72 71 70 73`, and the counters are `pset#1800157` / `stage#26712436` — 1.8 M pset
acquisitions and 26.7 M ring events in the run. The two QMP captures only reach CPU0 (PC
`0x1ffb12b90`, outside kernel text); the guest's own `owner stages` line is the owner record.

### 3.5 v1 vs v2

| | t42 v1 (`t42-smp2`) | t45 v2 run B |
|---|---|---|
| panic lock | `0xfffffff007b0d380` (`_thread_call_groups+0x280` in the symbol table) | **`_pset0 + 0x100`** (`0xfffffff007b69280`) |
| owner site | `_waitq_select_queue_flush+0x134` (waitq wake / `thread_go`) | **`_sched_timeshare_maintenance_continue+0xf4`** |
| `pset last` | `_waitq_select_queue_flush+0x134` | **`_thread_quantum_expire+0x26c`** |
| new marks hit | — | **70/71/72/73** (pset ticket) all eight owner stages; waitq/thread_go marks absent from the owner line |
| verdict | waitq wake that does not return | timeshare-maintenance loop on the pset ticket, held at panic |

The two locks are different addresses, so this is **not** the same lock instance: v1 panicked on
a ticket at `0xfffffff007b0d380` (nearest symbol `_thread_call_groups+0x280`), v2 on `_pset0`'s
own ticket. What is common is the family — the
guest's scheduler path takes the pset ticket in a waitq wake or in timeshare maintenance and then
stops making progress. v2's `_pset0+0x100` + `_sched_timeshare_maintenance_continue` **is exactly the
stage-27 T6 site**, which is why the two records now agree. Run A's mutex interlock is a third
manifestation in another lock class (its timeout is 251,658,240 ticks, 4× the ticket timeout).

## 4. What this establishes

* **The v2 owner is in the pset-ticket acquire/release loop of
  `_sched_timeshare_maintenance_continue`, holding the ticket when the panic fires.** That is a
  concrete loop and a concrete lock, with the caller (`_thread_quantum_expire+0x26c`) naming the
  quantum-expiry path into it.
* **H-B is now supported at function/lock level.** The 4.2 question is no longer "where does the
  owner stop" but "why does the timeshare-maintenance loop not leave / why does the waiter not get
  the released ticket". A simple lock-order inversion is not the best description: the owner's
  stages are the acquire/release wrapper itself, not a second lock held across the first.
* **The family is real.** v1's waitq wake site, v2's timeshare-maintenance site, stage 27's T6, and
  run A's mutex timeout all live in the same scheduler/lock pathology under MTTCG; the pset ticket
  is the common object in v2/T6.
* **The probe kernel is not a regression** on the single-vCPU path: 1,888 frames, 0 panic, the same
  desktop metrics as the delivery kernel.
* **Multi-core is still not deliverable.** Two more SMP=2 runs ended in timeout panics; the single
  vCPU remains the baseline.

## 5. What this does not establish

* **The exact loop condition.** Eight stage entries show the owner at one PC in a loop; they do not
  show the loop's bounds, the two variables it waits on, or whether it is infinite or merely very
  long. `pset#1,800,157` is a global acquisition counter, not proof that this loop alone ran 1.8 M
  times.
* **That run A is the same bug.** The mutex interlock is a different lock class and the v2 printer
  does not mark it; it may be the same pathology downstream or an independent one.
* **A fix.** No kernel change was made; the next probe has to read the loop's condition variables.
* **Probe neutrality.** The v2 marks add per-event stores to the pset/waitq/thread_go/thread_call
  hot paths (plus a `__builtin_return_address` per mark); timing and therefore the race probability
  change. The three SMP runs in this record are n=1 each; the earlier rounds had different panic
  times (v1 ≈3.1 min, v2-A 220 s, v2-B 800 s), which is consistent with a timing-sensitive race.
* **A clean evidence pipeline.** The t45 capture scripts produced empty `panic-block.txt` /
  `panic-stages.txt` files for both runs (the serial logs contain NUL bytes and the grep pipeline
  treated them as binary); the panic lines in §3.2/§3.3 were read directly from the serial logs.
  The QMP files only contain CPU0, whose PC is outside the kernel text.
* **The probe kernel as a deliverable.** It is `1bce762b`, not `f62c3d4b`; the kernel tree still
  carries the probe patch, so a rebuild from that tree reproduces the probe.
* **A general multi-core result.** Still lavapipe/TCG, win2022 at 190 %, no isolation core, no V100.

## 6. Next steps

1. **Instrument the loop, not the lock.** Put stages at the head/tail of
   `_sched_timeshare_maintenance_continue`'s loop with (a) the processor/thread index it is on,
   (b) the two condition variables it tests, and (c) a per-loop iteration counter; print those in
   the ticket panic. That distinguishes "loop is unbounded" from "the loop is long but progressing".
2. **Mark the `thread_quantum_expire → maintenance` boundary.** The current last-take is
   `_thread_quantum_expire+0x26c`; mark its entry and the call edge so the panic can say which
   quantum expiry entered the maintenance routine.
3. **Give the mutex timeout a light owner context.** Run A's `Mutex interlock` has no stages; even
   the last five stage ids of its owner would tell whether it is the same family.
4. **Run the SMP=2 probe more than once per configuration** and record the panic time; the race is
   timing-sensitive, and n=1 per site is only enough to locate, not to rank.
5. **If no fix appears**, the delivery choice remains: keep SMP>1 experimental with a startup guard
   that falls back to one vCPU (headless), and leave `isolcpus`/stopping win2022 as a future
   isolation experiment that needs the user's approval.

## 7. Evidence and hashes

| item | value |
|---|---|
| report | `/tmp/t45-evidence/T45-REPORT.md` sha256 `172ab45b2f51284bf3e02349524fd48bdeb371010240a08446812749eac5fb62` |
| probe v2 kernel / delivery kernel | md5 `1bce762b2ebc0a54c202391f37754d25` / `f62c3d4b852d467f22c4703b8fc620c9` (restored 19:48:22, both paths) |
| v2 patch | `hakios-xnu-kernel/xnu/hakios-probe.patch`, 550 lines, md5 `bfd2ccd33a384df5cd304a141d096e43`, sha256 `dcdbdf75ebb0866dbc36113650791cc4c269d50c2b3c0600e377e83ca054dd73` |
| build logs | `v2/kernel-build.log` sha256 `ad78cb2fb12eff44905f745f79b3baa3364e8819b34667780964cc764674f7c3`; `v2/kernel-build2.log` `4183332c39502980792eddcd5d3893279f969d534dddc40f40a9f78d751c7746` |
| v2 symbols | `v2/kernel.syms`, 53,360 lines, md5 `5ed0ffffe713614a66caa27315ddc21f`; copies `kernel.syms.1bce762b…` |
| probe binary evidence copy | `/tmp/t45-evidence/probe-v2.bin`, sha256 `51745bc5be049833db66e4885c11ddf69290a88ce05a0ba2edc55edb828b46dd` |
| run A serial / host / missing / QMP | `logs/t45-smp2-a/serial.log` sha256 `841c32707ce39f4a6afb043ca9f76dc7ef3ac77ce999c279b3fc0f456f3dbf3b`; host `2fa5ed50…`; missing `bb052cca…` (`frames 2`); QMP `d780aeb7…` |
| run B serial / host / missing / QMP | `logs/t45-smp2-b/serial.log` `3c6290e3d8f837d9557cec2cde25d0fdb1b2132bf11460a04618b8327098cad2`; host `d66145f8…`; missing `648725732c060714…` (`frames 217`); QMP `6ebd1cae…` |
| self-check serial / host / missing / captures | `logs/t45-selfcheck3/serial.log` `f5e9dd40969b3e3728b3c004fdca275c3e9298ff80a53580a531153ad2b0d637`; `/tmp/t45-selfcheck2-metalwire-host.log` `579c7ba9…`; `-missing` `d5cab266…` (`frames 1888`); PNG 750×1624 `3c710a3b…`, 375×812 `5a4e620b…` |
| kernel-swap / monitor A / monitor B | `kernel-swap.txt` `b6128978980c1511…`; `monitor-a` `0cd6fbf1…`; `monitor-b` `0a117b45…` |
| host / glwire | md5 `ad778a064bea462a11f6c67e552eacdb` / `5b28249a37cb11207f3ab160bcf4ac23` |
| image | root.apfs `a4a6e2ce…` (marker OFF); system `49ae3cf4…`; libcainject `aa123ccd…`; plug-in `bcaf99c6…` |

### 7.1 Repository gates

`make test` is green at this tree: provenance clean (484 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 8. Files changed in this record

- `docs/stage46-waitq-deep-probe.md` — this note.
- `docs/state-of-the-port.md` — the SMP>1 row gains the v2 result.
- `README.md` — the stage-46 milestone row, the EN+ZH multi-core table/paragraph updates, the
  EN+ZH status rows and the EN documentation index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the probe patch and kernels
live in the `iosvm-work` tree.

## 9. Reproduce

```sh
# build probe v2 in the pre-patched kernel tree (same fixed toolchain as stage 45)
cd /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
# the probe patch is already applied here; a clean apply is xnu/hakios-probe.patch
# (verify: git apply --check against the apply.sh baseline; 5 files)
make kernel -j16 BUILD_DSYM=0          # exit 2 = known kext symbolset check
md5sum hakios-kernel                   # 1bce762b2ebc0a54c202391f37754d25

# install the probe and run SMP=2 (tb-size=1536, thread=multi, taskset 26-29)
cp hakios-kernel.f62c3d4b.bak hakios-kernel  # (restore the delivery binary when done)
# boot t45-smp2-b, then read the owner stages from the serial log:
python3 /tmp/t45-evidence/sym.py /tmp/t5-probe-backup/v2/kernel.syms \
    0xfffffff007b69280 0xfffffff0072f6e40 0xfffffff00730a640 0xfffffff0072eaecc

# restore
cp /tmp/t5-probe-backup/hakios-kernel.tree.f62c3d4b ...
md5sum hakios-kernel                   # f62c3d4b852d467f22c4703b8fc620c9
```

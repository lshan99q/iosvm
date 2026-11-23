# stage 45: T5 — the owner's last stages, and a waitq wake that does not return

> Status: **measured. The T5 probe kernel boots, passes a single-vCPU self-check, and one SMP=2 run
> captured the lock owner's last stage markers — the first time the owner (not just the spinning CPU)
> is visible.** The owner took the scheduler processor-set lock inside
> `_waitq_select_queue_flush+0x134` (the `thread_go` call site), after entering through
> `_ipc_port_clear_receiver → _waitq_wakeup64_all_locked → do_waitq_select_n_locked`, and never
> returned. The lock address is the same pset lock as T2's (`_thread_call_groups+0x280`), and the
> site is in the waitq family that t38's two owner sites also landed in. **H-B (the guest kernel's
> waitq/scheduling path loses a wake-up or does not return under MTTCG) is now supported by a
> located owner path; H-A (parallel TCG) remains the enabling condition.** Multi-core is still not
> deliverable; single vCPU remains the baseline. The probe kernel is **not** the delivery kernel —
> `f62c3d4b` was restored and checked. Branch `experiment/ca-metal-device`, 2026-11-23. This
> continues [`docs/stage44-multicore.md`](stage44-multicore.md) and
> [`docs/stage27-multicore.md`](stage27-multicore.md).

## 1. The question

Stage 27's T5 plan and stage 44's T2/control runs left the same gap: the panic names *where the
spinner waited*, and (since stage 27's pset probe) *where the lock was last taken*, but not **what
the owner was doing**. Three owner sites had been seen from different runs —
`_waitq_pull_thread_locked+0x60` (t38 T2), `_idle_thread+0x64` (t38 control), and the locking site
in `_thread_dispatch` — all in the waitq/scheduler family, all on the same `_pset0` lock. T5 asks
for the owner's **recent history at panic time**, from a kernel probe that can be built, booted and
reverted without touching the delivery image's kernel.

## 2. Implementation

### 2.1 First the build pipeline: `LC_MAIN` → `LC_UNIXTHREAD`

The probe could not be booted at stage 27 because the local kernel build produced the wrong Mach-O
entry: `LC_MAIN` (with `__TEXT vmaddr = 0`) instead of `LC_UNIXTHREAD`. The loader rejects that with
"unexpected kernel layout", so a probe kernel built this way never starts. The blocking point is the
**linker choice**, not the compiler:

* the repository's `xnu-deps/host/bin/ld64` wrapper prefers cctools' `ld64` (it implements XNU's
  `-image_base`/`-segaddr` segment layout), but `xnu-deps/host/cctools/` does not ship one, so it
  falls back to Ubuntu's `ld64.lld` (LLD 18), which does not implement those flags → `LC_MAIN` and
  `__TEXT vmaddr 0x0`.
* the working linker is `/home/lshan/DSH-desktop/iosvm-work/toolchain/cctools/bin/aarch64-apple-darwin11-ld`;
  `make kernel` also reuses a stale `mach.development.bcm2837` when it thinks the link is up to
  date, so the old (vmaddr 0) output comes back unless the link outputs are removed first.

`hakios-xnu-kernel/tools/kernel-rebuild.sh` (in the kernel tree) makes that a single command: it sets `CCTOOLS_LD64`
to the cctools linker, removes the stale link outputs, runs `make kernel` (and tolerates exit 2,
which is this configuration's known kext symbol-set check), then **checks the Mach-O itself**:
`__TEXT vmaddr == 0xfffffff007004000`, `entry_va != 0`, and optional marker strings in the binary.
The t41 build log (808 lines, 0 errors) ends with:

```
kernel-rebuild: linking with .../toolchain/cctools/bin/aarch64-apple-darwin11-ld
kernel-rebuild: segments[0]=0xfffffff007004000 entry=0xfffffff00745d4a8
kernel-rebuild: marker "pset last took it" -> 1 hit(s)
kernel-rebuild: marker "owner stages" -> 1 hit(s)
kernel-rebuild: marker "stage ring" -> 1 hit(s)
eaf71509477e90ae0a5ec7ab70a0ae1d  .../hakios-kernel
```

The probe kernel is `eaf71509477e90ae0a5ec7ab70a0ae1d`; the delivery kernel was backed up first
(`hakios-kernel.f62c3d4b`, md5 `f62c3d4b852d467f22c4703b8fc620c9`) and restored after the run
(17:17:09, both paths checked).

### 2.2 The probe patch

`xnu/hakios-probe.patch` is a 366-line patch (200 added lines: `lock_ticket.c` 152, `waitq.c` 20,
`sched_prim.c` 28), applied to the `xnu/apply.sh` baseline. It adds no new header to the approved
file set (`processor.h` is untouched; the few shared symbols are declared `extern` inside the
functions, as stage 27's stage counters already did).

* **`osfmk/kern/lock_ticket.c`** — the ring:
  `HAKIOS_STAGE_RING 64`, `struct hakios_stage_evt { thread, pc, stage, reserved }`,
  `hakios_stage_ring[64]` + index, and `hakios_stage(stage, pc)` which stores
  `current_thread()`, the caller's `__builtin_return_address(0)` and the stage id. The ticket-timeout
  panic line grows from `stages 0..3` to `stages 0..7`, adds `stage ring <idx>`, and appends
  **`owner stages <8 entries>`** — the owner's last eight `stage@pc` values, newest first. The
  formatting is panic-safe and lock-free (`hakios_stage_text[320]`, hand-written decimal/hex).
* **`osfmk/kern/waitq.c`** — one macro, `HAKIOS_STAGE(stage)`, and **14 markers**:
  `waitq_pull_thread_locked` 40 entry / 41 reserve / 42–43 `waitq_lock_wait` before/after / 44
  remove / 45 unlock; `waitq_wakeup64_all_locked` 50 entry / 51–52 `do_waitq_select_n_locked`
  before/after / 53–54 `waitq_select_queue_flush` before/after; `waitq_select_queue_flush` 55 entry
  / 56–57 `thread_go` before/after.
* **`osfmk/kern/sched_prim.c`** — `HAKIOS_IDLE_STAGE` and **stages 60–65** in `idle_thread` (entry,
  after `spllo`, around `processor_idle`, before `thread_run`, before `thread_block`), plus the
  existing per-CPU `hakios_sched_stage[8]` marks in `thread_select` (1–7) that stage 27 added.

The patch was verified by applying it to a clean `apply.sh` tree (`xnu/applytest`) and comparing the
three files' md5 with the working tree — all matched. The working kernel tree **still carries the
probe**; the booted kernel is the restored delivery binary. That is a boundary (§5), not a claim that
the tree is production-clean.

## 3. Measurements

### 3.1 The probe kernel's single-vCPU self-check (`t42-selfcheck`)

SMP=1 / `cpus=1`, `taskset 26–28`, 1,500 s window, started 16:37:21:

* **0 panics, 0 "Ticket spinlock" lines**, **2,089 committed frames**; `--missing-report`:
  `frames 2089, pipelines 0, skipped draws 0`;
* the 750×1624 desktop frame is the same picture as the delivery kernel's: Dock band mean 66.3 with
  **13,199** pure-white icon-glyph pixels, Search mean 13.6 (the 750×1624 PNG is
  `3d11c906…`; the 375×812 thumbnail `47d47abc…`);
* the probe's hot-path stores therefore do not break the single-vCPU path; the probe kernel is
  bootable and its rendering is unchanged.

### 3.2 The SMP=2 capture (`t42-smp2`)

SMP=2 / `cpus=2`, `tb-size=1536`, `thread=multi`, `taskset 26–29`, 1,800 s window, booted 17:04:14.
The run was killed after the panic; the host had received **2 frames** (the desktop never appeared).

Panic (the guest printed the same block twice; values identical):

```
panic(cpu 1 caller 0xfffffff0072ead6c): Ticket spinlock[0xfffffff007b0d380] timeout after 60000027 ticks;
cticket: 0x9f, nticket: 0xa1, waiting for 0xa0,
pset last took it at 0xfffffff00733d0e4 (thread 0xfffffff812135ca0),
owner took it at 0xfffffff00733d0e4,
select loops 199360, maint_s 0,
stages 3/3/0/0/0/0/0/0, stage ring 433680,
owner stages 57@0xfffffff00733d2d4 56@0xfffffff00733d2d4 55@0xfffffff00733d2d4
             52@0xfffffff0072b0bac 51@0xfffffff0072b0bac 50@0xfffffff0072b0bac
             52@0xfffffff0072b0bac 51@0xfffffff0072b0bac,
current owner: 0xfffffff812135ca0 (on CPU 0), orig owner: 0xfffffff812135ca0,
start time: 12196877217, now: 12256877244, timeout: 60000000 @lock_ticket.c:538
```

**When:** the panic header's own epoch fields put it at **17:07:21** — `Boot 0x6b04018e` →
`Calendar 0x6b040249`, a difference of **187 s (≈3.1 min)** after the QEMU boot; the capture script
wrote the block at **17:07:38**. (The t42 report estimates ~12.2 min; the panic log's own epoch fields and the capture
timestamp both give ≈3.1 min, so this record uses 3.1 min. The 60,000,027-tick timeout is the
same value as every earlier ticket panic — it is not a wall-clock measure.)

**Host:** the monitor (`monitor.txt`, 39 samples) has QEMU at **158–178 %** while the guest was
alive, the two vCPU threads at **81.3 % / 75.6 %** on the peak sample (up to 89.4 % / 86.8 % in the
window; cores 29/28), win2022 at **190 %**, and host load **4.0–6.8** before the panic; after the
panic the QEMU process drops to 0. The CPU0 `info registers` were captured after the panic: PC
`0x1ffb12b90`, X30 `0x1ffb12410`, SP `0x47609300`, PSTATE `20000305 --C- EL1h` — the PC is outside the
kernel text (`0xfffffff007xxxxxx`), so the guest's own panic line is the authoritative owner
record, not the late QMP sample.

### 3.3 Symbolization

Symbols: `/tmp/t5-probe-backup/kernel.syms.eaf71509` (`llvm-nm -n`, 53,358 lines, md5
`9218aacd82a07706e4932e44359d0283`), decoded with the evidence copy of `symstages.py`. The delivery
kernel's symbol file was restored afterwards, so the live tree's `qemu/kernel.syms` is not the probe
one; the probe copy is the record.

| field | address | symbol + offset |
|---|---|---|
| panic lock | `0xfffffff007b0d380` | `_thread_call_groups + 0x280` (the pset lock; **same address as t38 T2's**) |
| `pset last took it` / `owner took it` | `0xfffffff00733d0e4` | **`_waitq_select_queue_flush + 0x134`** (the `thread_go` call site) |
| owner stage 57/56/55 | `0xfffffff00733d2d4` | **`_waitq_wakeup64_one_locked + 0xd4`** |
| owner stage 52/51/50 | `0xfffffff0072b0bac` | **`_ipc_port_clear_receiver + 0x50`** |
| panic caller | `0xfffffff0072ead6c` | `_hakios_stage_text_for + 0x0` (the probe's formatter) |

The owner's sequence, newest first: `_waitq_select_queue_flush+0x134` (stage 57 — taking/holding the
pset lock) → 56 → 55 (same function, around `thread_go`) → 52 → 51 → 50
(`_ipc_port_clear_receiver+0x50` entering `waitq_wakeup64_all_locked`) → 52 → 51 (an earlier wake).
In other words: **the owner enters a waitq wake from `ipc_port_clear_receiver`, reaches
`waitq_select_queue_flush/thread_go`, takes the pset lock, and never returns**, while the other CPU
spins 60 M ticks in `_hw_lck_ticket_contended`.

### 3.4 The restore

`kernel-swap.txt` records the whole chain: delivery `f62c3d4b` backed up at 16:36:45 → probe
`eaf71509` installed → run → **delivery restored at 17:17:09**, with both
`mt-work/hakios/hakios-kernel` and the kernel-tree binary checked as
`f62c3d4b852d467f22c4703b8fc620c9`. The probe binary is kept (both in the kernel tree's
`probe-kernels/` and the evidence copy) but is not the delivery kernel.

## 4. What this establishes

* **H-B is now supported by a located owner path.** Earlier rounds named the *site that took* the
  lock; this run names **what the owner was doing**: a waitq wake from `ipc_port_clear_receiver`,
  inside `waitq_select_queue_flush` / `thread_go`, holding the `_pset0` lock and not returning. That
  is a concrete place for the "lost wake-up / lock order / stuck holder" family under parallel vCPUs.
* **H-A is the enabling condition, not the whole story.** `thread=single` is stable; MTTCG dies.
  The owner site is guest-kernel code, so TCG parallelism exposes it rather than being the bug by
  itself.
* **It is a family, not a single point.** Three runs, three different owner sites —
  `_waitq_pull_thread_locked+0x60` (t38 T2), `_idle_thread+0x64` (t38 control), and now
  `_waitq_select_queue_flush+0x134` — and the same pset lock address in this round and t38 T2. The
  waitq/scheduler path is where the 4.2 work has to look.
* **The probe kernel is not a regression** on the single-vCPU path (2,089 frames, same desktop,
  0 panics), so the T5 method itself is usable.
* **Multi-core is still not deliverable.** This run is one more SMP=2 panic; single vCPU remains the
  baseline. The T5 result narrows the bug, it does not fix it.

## 5. What this does not establish

* **A root-cause fix.** We know the owner's site and sequence; we do not know why `thread_go` does
  not return (lock order against the pset lock, a missing wake-up, an interrupt/IPI lost under
  MTTCG, or a re-entered pset lock). No kernel patch was made in this record.
* **That the probe is neutral.** The stage ring is a 64-entry per-event store on the waitq/idle
  hot paths (an `HAKIOS_STAGE` at every marked point). It can change timing and therefore the
  probability of the race: the panic occurred in ≈3.1 min here, 14.6 min in t38's T2, and after
  2,035 frames in t38's control (boot 15:42:51 → panic ≈16:07, ≈24 min). The result is a located site, not a rate.
* **A reliable capture every time.** The ring is lock-free: 64 entries can tear or wrap while the
  panic formatter reads them, and the last eight entries can include older wake cycles (the
  `52/51` repeat shows exactly that). The `stages 3/3/0/0/0/0/0/0` fields are the coarse
  scheduler marks, not a full stack.
* **The probe kernel as a deliverable.** The probe binary is `eaf71509`, not `f62c3d4b`; the
  working xnu tree still carries the probe patch, so **a rebuild from that tree reproduces the probe
  kernel**. Restore/rebuild steps are in the kernel tree; this record does not change the delivery
  image.
* **A general multi-core result.** It is one run (`n=1`) with the probe, on lavapipe/TCG, with
  win2022 at 190 % CPU; no V100, no isolation core, no second client.
* **The QMP CPU0 registers as owner evidence.** They were captured after the panic and are outside
  the kernel text; the guest's `owner stages` line is the claim.

## 6. Next steps

1. **Narrow the owner's non-return inside `thread_go` / `do_waitq_select_n_locked`.** Add stage
   markers inside the loop/returns of `waitq_select_queue_flush` and around the `pset`-lock
   operations it can reach, and repeat the SMP=2 capture. The question is specific: does the owner
   spin on another lock (lock order), or is it preempted and never rescheduled (lost wake-up)?
2. **Check the pset ↔ waitq lock order.** The owner takes `_pset0` (via `_thread_call_groups+0x280`)
   inside the waitq wake path; enumerate the other paths that take the same lock in the opposite
   order (the t38 `_thread_dispatch+0x534` / `waitq_wakeup64_all_locked+0x34` pair is one) and see
   whether a cycle exists under parallel vCPUs.
3. **Make the probe reproducible.** More than one SMP=2 probe run, and a marker set that records
   the owner's lock-acquisition count, so "stuck" and "slow" can be told apart.
4. **If the fix is not found**, keep SMP>1 as an experimental feature only: a startup guard that
   detects SMP>1 and falls back to one vCPU (headless), or a hard label on the configuration. This
   is a delivery choice, not a kernel fix; it is not implemented here.
5. **Isolation stays a separate approval.** `isolcpus` or stopping win2022 would close the host
   side, but both need the user's approval (stage 44 §2).

## 7. Evidence and hashes

| item | value |
|---|---|
| report | `/tmp/t42-evidence/T42-REPORT.md`, sha256 `ead73bdda38338bac8c78f6f7b52ab4c2d3512a48539d61ae7c5fca3f9298aee` |
| probe kernel / delivery kernel | md5 `eaf71509477e90ae0a5ec7ab70a0ae1d` / `f62c3d4b852d467f22c4703b8fc620c9` |
| probe patch | `xnu/hakios-probe.patch`, sha256 `9a3187cb21815ecbf3d28a48c42d6453f58fac9577cb15b1deca1d67fd9612fc` |
| build log | `/tmp/t5-probe-backup/kernel-rebuild.log`, sha256 `b66441965caa9527b113706e169d9b3d967631a95800db28bc0e4254647281e0` (808 lines) |
| probe symbol table | `/tmp/t5-probe-backup/kernel.syms.eaf71509`, md5 `9218aacd82a07706e4932e44359d0283` |
| self-check serial / host log / missing | `.../logs/t42-selfcheck/serial.log` sha256 `f4f9f981e106fc77751677950813dc0d0039e16202b45681e13327b63b7870a9`; `/tmp/t42-selfcheck-metalwire-host.log` `d2fdc4b44573242f633905d5513aee8dbeea72b7039764e5519b4ba40516d1c5`; `-missing.txt` `8ad0c212…` (`frames 2089, pipelines 0, skipped draws 0`) |
| SMP=2 serial / host log / missing | `.../logs/t42-smp2/serial.log` `afed435194acfa7a3aa8e6531993f765641a0cfba65d7150a8594cb909c4be16`; `/tmp/t42-smp2-metalwire-host.log` `ac95ec2a…`; `-missing.txt` `bb052cca…` (`frames 2, pipelines 0, skipped draws 0`) |
| panic block / stages extract / time | `panic-block.txt` `771c21d7…`; `panic-stages.txt` `51105d32…`; `panic-time.txt` `e8c5ddff…` |
| QMP registers / kernel-swap / monitor | `qmp-registers.txt` `3398a770…`; `kernel-swap.txt` `23875e33…`; `monitor.txt` `97f47262…` |
| self-check captures | 750×1624 `3d11c906…`; 375×812 `47d47abc…` |
| host / glwire | md5 `ad778a064bea462a11f6c67e552eacdb` / `5b28249a37cb11207f3ab160bcf4ac23` |
| image | root.apfs `a4a6e2ce…` (marker OFF); system `49ae3cf4…`; libcainject `aa123ccd…`; plug-in `bcaf99c6…` |

### 7.1 Repository gates

`make test` is green at this tree: provenance clean (483 tracked files), proto pass, lavapipe
vkbridge probe all `[ok]`; the translator's suite is 15 PASS. `make style` keeps the pre-existing
failures (check-links/check-spdx); this record adds no new failure.

## 8. Files changed in this record

- `docs/stage45-t5-probe.md` — this note.
- `docs/state-of-the-port.md` — the SMP>1 row now carries the T5 owner result.
- `README.md` — the stage-45 milestone row, the EN+ZH multi-core table/paragraph updates, the
  EN+ZH status rows and the EN documentation index.

No `host/`, `guest/`, `tools/` or kernel file is changed by this commit; the probe patch and kernels
live in the `iosvm-work` tree, and this record only points at them.

## 9. Reproduce

```sh
# 1. build the probe kernel (tree with xnu/hakios-probe.patch applied)
cd /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
tools/kernel-rebuild.sh "pset last took it" "owner stages" "stage ring"
md5sum hakios-kernel                 # eaf71509477e90ae0a5ec7ab70a0ae1d

# 2. back up the delivery kernel and install the probe
cp hakios-kernel hakios-kernel.f62c3d4b.bak
cp xnu/probe-kernels/hakios-kernel.t5-eaf71509 hakios-kernel

# 3. single-vCPU self-check (SMP=1, taskset 26,27,28)
#    and the SMP=2 capture (SMP=2, cpu=2, tb-size=1536, thread=multi, taskset 26-29)
#    using qemu/run-darwin.sh as in stage 44; then read the panic and:
python3 symstages.py .../logs/t42-smp2/serial.log

# 4. restore the delivery kernel and check both paths
cp hakios-kernel.f62c3d4b.bak hakios-kernel
md5sum hakios-kernel mt-work/hakios/hakios-kernel   # f62c3d4b852d467f22c4703b8fc620c9
```

# stage 44: the 4.2 multi-core recon — T2, the control, and why one vCPU stays the baseline

> Status: **measured; the multi-core question is not answerable by the cheap matrix rows.** T2
> (`thread=multi` + `tb-size=4096`, 1,800 s) panicked after **14.6 min** with only **2 frames**
> (the desktop never appeared); the delivery-value control (`tb-size=1536`) panicked after
> **2,035 frames**. Both runs were pinned to cores 26–29 and both vCPU threads were executing at
> 60–87% CPU, so **H1 (host starvation) and H-C (host preemption) are rejected**; the lock owners
> landed in the guest's **waitq/idle** paths, so **H-A (the failure needs parallel vCPUs) remains the strongest and H-B (a
> guest-kernel scheduling/waitq bug) gains weight**. Multi-core is **not
> deliverable**: single vCPU remains the baseline, and SMP=2 is a laboratory configuration only
> (pinned, short runs). T5 is the only remaining path, and it needs a kernel build-pipeline fix plus
> an isolation decision the user must approve. Branch `experiment/ca-metal-device`, 2026-11-23. This
> continues [`docs/stage27-multicore.md`](stage27-multicore.md) (the matrix) and
> [`docs/stage25-multicore.md`](stage25-multicore.md) (the speed-up measurements).

## 1. The question

4.2 asks whether SMP>1 can be made a delivered configuration. Stage 25 measured the prize
(2 vCPUs pinned: 15.6–15.9 fps vs 9.9 on one vCPU, +58–60 %) but stage 27 recorded five pinned
two-vCPU runs: two lasted 30 minutes, three crashed within 5–10 minutes on the scheduler
processor-set lock `_pset0`. The crash signature is a **ticket spinlock timeout** — the guest spins
60,000,000 ticks and then panics while the lock's owner does not release it; loosening the timeout
4× did not help.

Stage 27 left a six-row matrix (T1–T6). T1 (`thread=single`) passed 30 minutes, which pointed at
parallel TCG (H-A); T3 (`-cpu max`, LSE atomics) and T6 (host pinning, including same-core) still
crashed; T5 (kernel probes) was half-built but the local kernel build pipeline could not produce a
bootable kernel. This stage runs the cheapest untried row — **T2, the TB-cache size** — with a
same-day control at the delivery value, to test whether the translation-block cache is the
contended resource, and to re-test the host-starvation/preemption explanations under a controlled
pin.

## 2. The matrix, and what this machine can do

| # | experiment | status | result / evidence |
|---|---|---|---|
| T1 | `-accel tcg,thread=single` + SMP=2, pinned | **done** (stage 27, `s27-smp2-single`) | 415 frames, 0 panics, 8.1 fps — serial TCG has no speed-up, so the deadlock needs parallel vCPUs (H-A) |
| **T2** | `thread=multi` + `tb-size=4096` | **this round** (`mc-t2a`) | **still panics** (14.6 min, 2 frames, desktop never appeared) → the TB cache is not the factor |
| T3 | CPU model (`-cpu max`, LSE atomics) | **done** (stage 27, `s27-smp2-lse`) | same panic type → not the LL/SC implementation |
| T4 | panic-time capture (QMP `info registers` + host thread state) | **done** (stage 27 `s27-smp2-dump`) + this round | CPU0 registers are readable (T2: PC `_DebuggerWithContext+0x180`); CPU1's are not (QMP does not preserve them across the call); the host vCPU threads' CPU is visible |
| T5 | kernel probe: dump the lock owner's last stages at panic | **half done** (probe kernel `73c2a5de` already prints `pset last took it` / `owner took it` / `select loops` / `stages`) | the local build pipeline produces a non-bootable kernel (`LC_MAIN`/`LC_UNIXTHREAD`, stage 27 §5) — must be fixed first |
| T6 | host isolation (pinning / cgroup cpuset / `isolcpus`) | **done** (stage 25 P0 pinning ran 30 min; stage 27 `s27-pset5-samecore` still panicked) | pinning lowers the probability but does not fix it; same-core still panics → not host parallelism |

**What this machine can do.** `sudo` is available (cgroups, image rebuilds), but the session has
**no `CAP_SYS_NICE`** (RT priorities are not available). The win2022 VM
(`qemu-system-x86_64 -name guest=win2022-gpu`) owns the `04:00.0` GPU, floats across **all 0–31
cores** and burns **~190 % CPU**; it cannot be moved. A "truly isolated core" therefore exists only
as a narrow `taskset` for our own QEMU (this round: 26–29); taking cores away from win2022 or using
`isolcpus` needs the user's approval and is not done. T5 is the most expensive row; T2 was the
cheapest untried one, which is why this round picked it.

## 3. This round's two runs

| item | T2 (`mc-t2a`) | control (`mc-ctrl`) |
|---|---|---|
| QEMU | `-smp 2 -accel tcg,thread=multi,tb-size=4096` | `-smp 2 -accel tcg,thread=multi,tb-size=1536` (delivery value) |
| guest CMDLINE | `cpus=2 fb_beat=0 fbscale=2 metal=1` (guest prints `2 of 2 processors are running`) | same |
| pinning / RAM | `taskset -c 26,27,28,29`, `QEMU_RAM_GB=7` | same |
| image / hosts | `root.apfs` `a4a6e2ce…` (t34 plug-in, marker OFF); `metalwire_host` `ad778a06…`; `host_glwire` `5b28249a…` | same |
| launch | 15:19:19, 1,800 s window | 15:42:51, 1,800 s window |
| outcome | **panic at ~15:33:50 (14.6 min), 2 frames** | **panic at ~16:07; 2,035 frames** |

The guest kernel in both runs is `Darwin Kernel Version 22.5.0` (`xnu-8796.121.2-dirty`,
`DEVELOPMENT_ARM64_BCM2837`) — the same kernel as the stage-27 runs.

### 3.1 T2: `tb-size=4096`

Panic (serial `mc-t2a`):

```
panic(cpu 1 caller 0xfffffff0072eacd4): Ticket spinlock[0xfffffff007b0d380] timeout after 60000027 ticks;
cticket: 0xb3, nticket: 0xb5, waiting for 0xb4, pset last took it at 0xfffffff0073033d8
(thread 0xfffffff52c333dc0), owner took it at 0xfffffff00733cab8, select loops 538741, maint_s 0,
stages 3/3/0/0, current owner: 0xfffffff52c548c10 (on CPU 0), orig owner: 0xfffffff52c548c10,
start time: 39315475495, now: 39375475522, timeout: 60000000 @lock_ticket.c:437
```

Symbolled: `pset last took it` → **`_thread_dispatch+0x534`**; `owner took it` →
**`_waitq_pull_thread_locked+0x60`**; caller → `_hw_lck_ticket_contended+0x164`.

T4 capture: the other CPU's halt failed (CPU0; `error -5: halt not supported`); QMP `info registers` on **CPU0**
gave PC `_DebuggerWithContext+0x180`, LR `+0x168`, X07 `_hw_lck_ticket_contended+0x164`,
PSTATE `004003c4 EL1t`, SP `fffffffbfeb5b510`. The host monitor (25 samples, 15:30:04–15:42:09)
showed load 2.9–7.8 (first launch sample 7.84; steady 2.9–5.7), QEMU 109–120 %, win2022 190 %;
the two vCPU threads ran at **60→81 %** and **58→27 %**, both landing on core 27 from 15:27 on
(initially 26/27). The run was killed around 15:42; the control started at 15:42:51.

### 3.2 The control: `tb-size=1536` (delivery value)

Panic (serial `mc-ctrl`), after the desktop was up and the host had acknowledged **2,035 frames**
(guest clock `08:01:36`):

```
panic(cpu 0 caller 0xfffffff0072eacd4): Ticket spinlock[0xfffffff007b68c80] timeout after 60000015 ticks;
cticket: 0x93, nticket: 0x95, waiting for 0x94, pset last took it at 0xfffffff00733cf10
(thread 0xfffffff7c30f4e50), owner took it at 0xfffffff00730998c, select loops 1068860, maint_s 0,
stages 3/3/0/0, current owner: 0xfffffff7c2e34e50 (on CPU 1), orig owner: 0xfffffff7c2e34e50,
start time: 73174684538, now: 73234684553, timeout: 60000000 @lock_ticket.c:437
```

Symbolled: `pset last took it` → **`_waitq_wakeup64_all_locked+0x34`**; `owner took it` →
**`_idle_thread+0x64`**. Host monitor (63 samples): load 1.7–10.1, QEMU up to **172 %**, win2022
190 %; the two vCPU threads ran at **67 % / 67 %** in the report's sample (up to 87 % / 83 % in the monitor;
cores 26–29). The guest was
acknowledging frames until the panic (`frame 2034 … crc matches`), and the host log ends with
`client disconnected after 2035 frame(s)`.

The two runs differ in how far they got, not in how they died: the same 60 M-tick ticket timeout,
same `stages 3/3/0/0`, same `_pset0` lock. T2 died before the desktop; the control died **after**
2,035 frames — so the later failure is not a boot-only race.

## 4. What the two runs establish

* **H1 (host starvation) is rejected.** Both runs are pinned to a narrow set (26–29) and both vCPU
  threads are executing at 60–87 % CPU in the monitor; the timeout is a **real ~60,000,000-tick spin** (60,000,027 in T2, 60,000,015 in the
  control), not a time jump. A starved vCPU would show a sleeping/blocked thread, not two busy ones.
* **H-C (host preemption) is rejected.** The same measurement rules out the "host took the vCPU
  away for the whole timeout" story, and stage 27's same-core experiment already removed the host
  parallelism explanation.
* **H-A remains the strongest.** `thread=single` (T1) is stable while MTTCG dies; the failure needs
  parallel vCPUs. This round does not change that.
* **H-B gains weight.** The owners now land in the **waitq/idle** code — T2:
  `_waitq_pull_thread_locked`; control: `_idle_thread` holding the lock — and the panic's
  `pset last took it` is in `_thread_dispatch` / `_waitq_wakeup64_all_locked`. A missing wake-up or
  a lock-order inversion in the guest's scheduler/waitq path under parallel vCPUs fits both sites.
  That is a hypothesis, not a root cause: the probe currently records only four stage counters, and
  the owner's last stages are not printed yet (T5).
* **Multi-core is not deliverable.** Single vCPU remains the baseline. SMP=2 stays a laboratory
  configuration: pinned, short, and not promised. The stage-25 15.8 fps is a **short-run** number;
  long runs still panic.
* **The frame-rate prize is unchanged** (15.6–15.9 fps in the short pinned runs), so the work item
  stays open — it is just blocked on the diagnosis, not on a tuning knob.

## 5. T5: the only remaining path

T5 is now the only experiment that can turn "guest kernel waitq/scheduler" from a weighted
hypothesis into a located bug. Its prerequisites, in order:

1. **Fix the kernel build pipeline.** The local build produces a kernel the loader will not start
   (`LC_MAIN` instead of `LC_UNIXTHREAD`; stage 27 §5). Without a bootable patched kernel there is
   no probe to read. This is a build-pipeline job, not a kernel patch.
2. **Ring-buffer stage probes** in `waitq_pull_thread_locked`, `waitq_wakeup64_all_locked`,
   `idle_thread` and the ticket-lock acquisition, expanding the existing `stages 3/3/0/0` fields to
   the owner's **last 8 stages** at panic time. That directly tests the missing-wake-up /
   lock-order stories.
3. **A truly isolated core** (or a clean core set) if the host-side story is to be closed for good:
   stop the win2022 VM or use `isolcpus`. **Both need the user's approval** — this record does not
   do either, and the current runs only narrow `taskset` to 26–29 while win2022 floats over 0–31 at
   ~190 % CPU.

If 4.2 is later expected to *ship* SMP>1 before the bug is found, the honest options are a startup
guard that detects SMP>1 and falls back to one vCPU (headless detection), or keeping SMP>1 labelled
experimental. Neither is implemented by this record.

## 6. What this does not establish

* **A root cause.** H-A/H-B are weighted, not proven; the owner's call chain is one frame deep
  (`_waitq_pull_thread_locked+0x60`, `_idle_thread+0x64`) and the probe does not yet print the
  owner's last stages.
* **A fix, or a reason to expect one.** No kernel patch was made; the build pipeline that would
  carry one does not produce a bootable kernel.
* **A statistically strong T2 result.** This round contributes **one run per configuration**
  (T2 once, control once); stage 27's five pinned runs and stage 25's two 30-minute runs are the
  rest of the sample. The T2 result is a negative on the TB-cache hypothesis, not a rate.
* **A host-side clean bill.** The host load was high (2.9–7.8 on T2, 1.7–10.1 on the control) and
  win2022 kept 190 %; the pinning rules out starvation *of our vCPU threads*, not every possible
  host interaction. A true isolation run needs the approval in §5.
* **Multi-core performance.** The short-run 15.6–15.9 fps is not a delivery claim; the long-run
  result is a panic on both TB sizes.
* **Anything on the V100.** Both runs are QEMU TCG with the lavapipe host; the V100 was not used.

## 7. Evidence and hashes

| item | value |
|---|---|
| report | `/tmp/mc-t2a-evidence/T38-REPORT.md`, sha256 `1de100d25b8c9e555864d3831d735c34a74323cd7cb11c2a4db392700f5ddea4` |
| T2 serial log | `/home/lshan/DSH-desktop/mt-work/logs/mc-t2a/serial.log`, sha256 `5287cc29d72a85418f99035f5fbc8c21f4c2d814fbacf08cde71f6158e554a2f` |
| control serial log | `/home/lshan/DSH-desktop/mt-work/logs/mc-ctrl/serial.log`, sha256 `725f926ff20a97245b02c928e2723beb73fba8b14b4da5f3301951214d36a864` |
| T2 host log / missing | `/tmp/mc-t2a-metalwire-host.log` (`e138ac03…`), `/tmp/mc-t2a-missing.txt` (`bb052cca…`, `frames 2, pipelines 0, skipped draws 0`) |
| control host log / missing | `/tmp/mc-ctrl-metalwire-host.log` (`4919d15d…`, `client disconnected after 2035 frame(s)`), `/tmp/mc-ctrl-missing.txt` (`0ad65c27…`, `frames 2035, pipelines 0, skipped draws 0`) |
| T2 monitor | `/tmp/mc-t2a-evidence/monitor2.txt`, sha256 `cf9c1276af9fba96bbc17e54eec9f0f4971b142fca6c244fca9524cc40733ad1` (25 samples); `final-monitor.txt` is the same content |
| control monitor | `/tmp/mc-ctrl-evidence/monitor2.txt`, sha256 `eacadf1a92295182d8914d90d7d9c18280f11d081fd0673be306c2c6e7367f1b` (63 samples) |
| launch scripts | `/tmp/mc-t2a-evidence/launch.sh` `a710e406…`; `/tmp/mc-ctrl-evidence/launch.sh` `9d6bc798…` |
| `metalwire_host` (both runs) | md5 `ad778a064bea462a11f6c67e552eacdb` |
| `host_glwire` | md5 `5b28249a37cb11207f3ab160bcf4ac23` |
| `HakiosMetal` plug-in | md5 `bcaf99c6392d24d1d96b76586df9e246` |
| `system.apfs` / `root.apfs` | `49ae3cf4fd53334b475a921eb9915008` / `a4a6e2ce50e6d0324e26a49ea20fbd84` (marker OFF) |
| `libcainject.dylib` | `aa123ccdd6ee4661fa335ac91311bbb1` |
| clients / vars | 35 B; vars 0xFF sha256 `dd30d9e07e89c1749cd420e998190ab9e31d4b43d27b5862887320ba2a2b8b0f` |

The T2 launch also left the full config in `/tmp/mc-t2a-vars0xff.fd` (64 MiB all-0xFF source) and the
CSV headers in `/tmp/mc-t2a.csv` / `/tmp/mc-ctrl.csv`; the boots were run with `QEMU_ACCEL=tcg,thread=multi`
and `QEMU_TB_MB=4096` / `1536` from the launch scripts above.

### 7.1 Repository gates

`make test` is green at this tree: provenance clean (482 tracked files), the proto suite passes, and
the lavapipe vkbridge probe reports every operation `[ok]`; the translator's suite is **15 PASS**.
`make style` keeps the pre-existing failures recorded in the earlier stage notes (legacy SPDX
headers, archived link targets); this record adds no new failure of either kind.

## 8. Files changed in this record

- `docs/stage44-multicore.md` — this note.
- `docs/state-of-the-port.md` — the SMP>1 row in "What does not work yet" (stage 44 result and the
  T5/isolation boundary).
- `README.md` — the stage-44 milestone row, the English and Chinese multi-core status rows, and the
  documentation index.

No `guest/`, `host/`, `tools/` or kernel file is changed: this record is documentation of a
read-only experiment.

## 9. Reproduce

```sh
# T2: the TB-cache experiment (runs until the guest panics; the window is 1800 s)
env IMG=userland/system.apfs ROOT=userland/root.apfs QEMU_RAM_GB=7 QEMU_INSTANCE=3 \
    QEMU_NET=1 QEMU_SMP=2 QEMU_TB_MB=4096 QEMU_ACCEL=tcg,thread=multi \
    QEMU_EFI="$HAKIOS/qemu/efi64.fd" QEMU_VARS=/tmp/mc-vars0xff.fd \
    CMDLINE="cpus=2 fb_beat=0 fbscale=2 metal=1" \
    taskset -c 26,27,28,29 timeout 1860 bash qemu/run-darwin.sh 1800

# control: same command with QEMU_TB_MB=1536 (the delivery value)

# the host side (either run)
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  host/metalwire/metalwire_host --gpu 0 --port 9111 \
    --ram .../mt-work/hakios/qemu/ram.img --shm /dev/shm/metalwire-frame \
    --out /tmp/mc-frames --png-every 1 --verbose \
    --missing-report /tmp/mc-missing.txt
```

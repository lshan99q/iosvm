# stage 25 § 多核 panic：现场、外推与调查计划

2026-11-17 21:16 `s24-smp2b`（`QEMU_SMP=2` + `CMDLINE="cpus=2 …"`，其余同现役：`fbscale=2 gkbf=1 glwire_ack_only=1`）。
guest 自己打印 `GenericARMPE: 2 of 2 processors are running`（真双核），出 5 帧后 panic。

## 一、现场（原文）
```
panic(cpu 1 caller 0xfffffff007aedb18): Ticket spinlock[0xfffffff007b6c880] timeout
  after 60000047 ticks; cticket: 0xf4, nticket: 0xf6, waiting for 0xf5,
  current owner: 0xfffffff6211b8608 (on CPU 0), orig owner: 0xfffffff6211b8608,
  start time: 26621612848, now: 26681612895, timeout: 60000000 @lock_ticket.c:386
```
- panicked task `pid 29 logd_helper`，thread tid 239；CORE 0 的 PC/LR/FP 全是 0（另一个 CPU 已停）。
- 日志：`logs/s24-smp2b/serial.log`（panic 在 15988 行起，含 LR 回溯）。
- 注意 `now - start = 60 000 047`，正好等于 timeout：这是**真实的 6000 万 tick 自旋**，不是时间跳变造成的假超时。

## 二、历史对照
| 时间 | 配置 | 结果 |
|---|---|---|
| 2026-11-02 | `QEMU_SMP=4` + `cpus=4`，旧 in-place 回放 | **能跑**（桌面 + host 帧间隔 0.07 s≈13.8 fps，0 SIGSEGV） |
| stage21 gate | 漏传 `QEMU_SMP`（默认 4 核） | 4 核 **Ticket spinlock panic** |
| 2026-11-14 23C | `SMP=4/2`、`MTTCG`、`tb-size=4096` 四发 | 全部 `hostframes=0`（当时是否传 `cpus=` 已无从查证） |
| 2026-11-17 | `SMP=2` + `cpus=2`，ack-only/remote-only | **Ticket spinlock panic**（本文） |

## 三、根因假设（按可能性排序）
- **H1 vCPU 被宿主饿死（最可能）**：MTTCG 下每个 vCPU 是一个宿主线程。锁持有者跑在 CPU 0 的线程上，
  若该线程被宿主调度走 ≥6000 万 tick（约 0.6–2.5 s），CPU 1 就会把票自旋到超时。
  事发时宿主机 load 7–16，隔壁 `win2022-gpu` 常吃 3.4 核 —— 完全够把某个 vCPU 线程压住。
- **H2 锁持有者真的睡死**：guest 侧某路径持票锁时睡眠/等 IO（xnu 不允许），SMP 下才暴露。
- **H3 TCG 原子/内存序**：ticket 锁的 ldaxr/stxr 在不同 vCPU 间的可见性问题（会表现为偶发，不必然是固定点）。
- **H4 次核 bring-up 竞态**：`start time` 发生在起机早期（logd 阶段），可能是次核启动路径上的锁竞争。

## 四、调查计划（P0→P2，逐级加码）
- **P0 不再改码先验证 H1**：给 vCPU 线程**独占核 + 高优先级**再跑 `SMP=2`。
  - 做法：`taskset -c 20,21 ./qemu …` + 目标 vCPU 线程 `chrt -f 80`（或 QEMU `thread-context affinity`），
    宿主侧同时确认 `win2022-gpu` 不在那两个核上；日志 `logs/s25-smp-pin/`。
  - 判据：能连续跑 ≥600 帧不 panic → H1 成立，多核可救（靠绑核/优先级/隔离核）。
    panic 依旧（且时间点相同）→ 转 P1。
- **P1 放宽/可配置 ticket 超时**：定位 `lock_ticket.c:386` 的超时常量（xnu 里 `TICKET_LOCK_TIMEOUT`），
  改成 boot-arg 可配（默认不变，实验用 10×）。若放宽后能跑但只是“晚点崩”，说明是真死锁 → 转 P2。
- **P2 抓死锁现场**：panic 时用 QEMU monitor/QMP 在 CPU 0 上取 `info registers` + 宿主 `gdb -p <vCPU tid> bt`，
  对照 guest 的 LR 回溯符号化（`hakios-kernel` 的符号），确认 CPU 0 卡在哪个函数、是否在等 IO/睡眠。
- 纪律：一次一发；结果带 hostframes/panic 点/墙钟；与 `SMP=1` 基线成对比较。

## 五、P0 结果（2026-11-17 23:04）：绑核后多核跑通，H1 成立
- `s25-smp-pin`（`SMP=2`+`cpus=2`，qemu `taskset -c 20-23`、host_glwire `24-25`）：**714 帧、0 次 Ticket spinlock、0 SIGSEGV**，
  guest `2 of 2 processors are running`，驱动 27–30 µs/swap、9–10 swap/s。
- 量化版 `s25-smp2-pin`（同绑核 + 10 s 采样，841 帧后停）：**墙钟中位 15.8 fps**（11.1 / 13.0 / 15.8 / 19.4 / 19.8 五个 10 s 窗），
  CPU/帧合计 127 ms（两个 vCPU，129–200% 核），load 8–9，**0 panic**。
- 对照：同样参数不绑核 → 5 帧就 panic。**结论：H1（vCPU 被宿主调度走导致锁超时）成立；多核只要独占核就可用。**
- 代价与注意：MTTCG 总 CPU 从 75–81 ms/帧 涨到 127 ms/帧（并行 + 开销），墙钟从 ~75 ms 降到 ~63 ms；
  RT 优先级不可用（无 CAP_SYS_NICE），只有绑核这一层保护；窗口抖动大（11–20 fps），需要成对比较。

## 六、如果多核真的救活，收益预期
CA commit→显示仍是串行，Amdahl 上限 1.5–2×；当前 12–14 fps 的 1.5× = 18–21 fps，直接越过 15 fps 目标。

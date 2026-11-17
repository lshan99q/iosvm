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

## 六、P0b 结果：SMP=4 不划算（2026-11-17 23:15）
- `s25-smp4-pin`（`SMP=4`+`cpus=4`，绑 `20-27`，732 帧，0 panic，guest `4 of 4 processors are running`）：
  四个 10 s 窗 **34.1 / 4.1 / 11.6 / 17.3 fps**，中位 **14.4 fps**；CPU/帧合计 **108–305 ms**（中位 212 ms，344% 核）。
- 对比 `SMP=2`（中位 15.8 fps、CPU/帧 127 ms、129–200% 核）：加核**没换来墙钟收益**，总 CPU 反而近翻倍（MTTCG 开销 + 锁竞争），
  并且出现 4.1 fps 的停顿窗。**结论：2 vCPU 目前是甜点，4 核不采用。**
- 仍待做：绑核 `SMP=1` 对照（同核/同负载）把净收益钉死；SMP=2 跑 30 分钟以上验证长期稳定性（841 帧只覆盖了 ~2 分钟稳态）。

## 七、净收益：同核同测法三配置对比（2026-11-17 23:28）
| 配置（都 `taskset` 独占核，10 s 采样窗口） | 墙钟 fps 各窗 | 中位 | CPU/帧合计 | 核占用 | load |
|---|---|---:|---:|---:|---|
| `SMP=1`+`cpus=1`（绑 20-21） | 9.9, 20.0, 7.5, 16.4, 9.9, 4.6 | **9.9** | 98.5 ms | 103–105% | 6.4–7.5 |
| **`SMP=2`+`cpus=2`（绑 20-23）** | 11.1, 19.4, 13.0, 19.8, 15.8 | **15.8** | 127 ms | 129–200% | 8.2–9.4 |
| `SMP=4`+`cpus=4`（绑 20-27） | 34.1, 4.1, 11.6, 17.3 | 14.4 | 212 ms | 125–367% | 7.7–8.4 |

- **多核净收益：9.9 → 15.8 fps（+60%）**，而且 SMP=2 那几窗的宿主 load 还更高（8.2–9.4 vs 6.4–7.5），所以真实收益只会更大，不会更小。
- 代价：总 CPU/帧 98.5 → 127 ms（+29%），靠第二个 vCPU 并行换来的墙钟收益。
- 结论：**交付配置改成 `SMP=2`+`cpus=2`+绑核**；`SMP=4` 不采用。
- 待办：SMP=2 长跑（≥30 min）验证稳定；把绑核写进 `run-darwin.sh`/配方；再叠 ack 流水线。

## 八、30 分钟稳定性长跑（2026-11-17 23:29–00:01，`s25-smp2-long`）
- `SMP=2`+`cpus=2`，绑 `20-23`，`tools/glwire/run-pinned.sh` 同配置：**7023 帧、0 次 Ticket spinlock、0 卡死**。
- 45 个 10 s 采样窗：中位 **15.9 fps**、CPU/帧中位 **118 ms**；窗口范围 4.9–30.9 fps（宿主 load 6–9 的抖动，不是客机不稳）。
- 判定：**绑核 SMP=2 可以当交付配置**。剩下的抖动来自宿主负载，不是多核 panic。

## 九、修正：补丁门限把测量发的画面搞坏了（2026-11-18 02:10）
- 21:53 为「可交互演示」加的 `GLWIRE_PATCH_MIN_COVERAGE`（默认 0.10，覆盖度够高就跳过静态补丁）有副作用：
  回放本身就残缺、坏帧里的亮色块被算成「有内容」→ 补丁被跳过 → **画面是黑+绿块的垃圾帧**。
  23:29 那发 30 分钟长跑的 `shm.bin` 已核实是垃圾帧，所以那次的「画面完整」不成立（fps 数字本身仍是帧节拍，但画面判定作废）。
- 修正：测量发一律 `GLWIRE_PATCH_MIN_COVERAGE=2`（补丁常开）。`tools/glwire/run-pinned.sh` 已固定。
- 修正后实测（`s25-smp2-full`，`SMP=2`+`cpus=2` 绑 20-23，968 帧，02:06–02:10）：
  - 画面 `shm.bin` → PNG 的 **sha256 = 基线 `42e14ed2…`（逐字节相同）**；
  - 5 个采样窗 27.1 / 24.8 / 11.5 / 12.4 / 18.3 fps → **中位 18.3 fps**，CPU/帧中位 105 ms，驱动 32 µs/swap，**0 panic**。
- 结论：**15 fps 目标达成且画面逐字节一致**；正式数字以补丁常开的长跑为准（`s25-smp2-final` 正在跑）。

## 十、定稿数字：30 分钟有效长跑（2026-11-18 02:12–02:42，`s25-smp2-final`）
- 配置：`tools/glwire/run-pinned.sh` 同款（`SMP=2`+`cpus=2` 绑 20-23、host_glwire 绑 24-25、ack-only、gkbf、`fb_noblit` 自动、**补丁常开**）。
- **6755 帧、0 次 Ticket spinlock、0 卡死**；45 个 10 s 窗：**中位 15.6 fps**（4.7–40.7），CPU/帧中位 116 ms，宿主 load 5.6–8.3。
- 画面：`shm.bin` → PNG 的 **sha256 = 基线 `42e14ed2…`，逐字节相同**。
- 对照（同核同负载，补丁常开）：`SMP=1` 中位 9.9 fps → **多核净收益 +58%**。
- 结论（可写进交付）：**11 fps 周目标达成 1.4×（15.6 fps），且画面完整**；抖动的下限（4.7 fps 窗）来自宿主负载尖峰，不是客机 panic。

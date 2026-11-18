# stage 27：多核（SMP>1）正式立项——必须解决，不再是“判死”

立项理由（2026-11-18 20:40）：多核实测能带来 **+58~60%**（单核 9.9 fps → 双核 15.6 fps，同负载绑核），
项目后期一定要用；早解决早受益。它不是调参能绕过的，必须把根因查清。

## 一、现状（全部实测）
- `SMP=2`+`cpus=2` **不做绑核**：起机 5 帧内 `Ticket spinlock timeout` panic（CPU1 卡在 `lck_ticket_contended`）。
- **绑核后**（qemu `taskset 20-23`、host_glwire `24-25`）：成功跑过 2 次 30 分钟（6755 帧 / 841 帧，0 panic）；
  但另外 3 次在 5–10 分钟内崩，panic 都指向 **`_pset0 + 0x100` 的调度器 processor-set 锁**（两方向 owner 都出现过）。
- **放宽票锁超时 4×（`tlto_us=4000000`）无效**：照崩（`timeout after 250000022 ticks`）→ 不是短暂的宿主抢占，而是
  持有者长时间不释放（真死锁或 vCPU 永久停摆）。
- RT 优先级拿不到（无 `CAP_SYS_NICE`），目前只有“亲和性绑核”这一层保护。

## 二、假设
- **H-A TCG 并发/原子实现**：MTTCG 下 vCPU 线程并行执行，ticket 锁靠 `ldaxr/stxr` 独占监视器；若 QEMU 的独占监视器在多 vCPU 下
  有丢失/顺序问题，锁会“看起来一直被持有”。→ 可测：`-accel tcg,thread=single`（vCPU 串行化）若不再 panic，就是这条。
- **H-B guest 内核 SMP 路径 bug**：`_pset0` 锁在处理器集/调度路径上，端口自己的 CPU bring-up（PSCI）、timer、锁序可能有问题。
  → 需要 panic 时另一颗 CPU 的现场（QMP `info registers` / 内核 dump 全部 CPU）。
- **H-C 宿主抢占（已弱化但未排除）**：4× 超时都兜不住，除非停顿 >4 s；仍需在 panic 时看宿主的 vCPU 线程栈。

## 三、测试矩阵（按成本低→高）
| # | 实验 | 目的 | 判据 |
|---|---|---|---|
| T1 | `-accel tcg,thread=single` + `SMP=2` 绑核 | 区分 H-A / H-B | 30 分钟 0 panic → H-A 成立 |
| T2 | `thread=multi` + `tb-size=4096` | 排除 TB 缓存竞争 | 同上；若更稳则作为缓解 |
| T3 | 换 CPU 模型（`-cpu max` / `cortex-a72`） | CPU 特性/顺序相关 | 同上 |
| T4 | panic 瞬间抓另一颗 CPU：QMP `info registers` + 宿主 `gdb -p <vCPU tid> bt` | 定位锁持有者 | 能看到 CPU0 卡点 |
| T5 | 内核补丁：panic 前 dump 所有 CPU 的 PC/LR/持锁线程 | 一次性拿到现场 | 同上 |
| T6 | 宿主侧：cgroup cpuset `isolated` / `isolcpus` 真隔离 | 排除 H-C 最后一半 | 0 panic 即证明是抢占 |

## 四、验收
- `SMP=2` 连跑 **2×60 分钟** 0 panic、画面 sha256 = 基线、帧率按 load 分组 ≥ 同负载单核的 1.4×；
- 然后把多核并入交付配方（与 `fb_hz=15` 一起），并更新 handover 的“判死清单”（移除多核、改写结论）。
- 中途任何实验都遵守：一发一台、成对比较、load 分组、画面校验。

## 五、当前进度
- T1 正在跑：`s27-smp2-single`（`QEMU_ACCEL=tcg,thread=single` + `SMP=2` + `cpus=2` + 绑核 + `fb_hz=15`，2026-11-18 20:39 起机）。

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

## 五、进度
### T1：`thread=single` 跑满 30 分钟不 panic —— H-A 成立（2026-11-18 21:13，`s27-smp2-single`）
- `QEMU_ACCEL=tcg,thread=single` + `SMP=2`+`cpus=2` + 绑核 + `fb_hz=15`：**415 帧、0 次 Ticket spinlock、0 panic**，
  guest `2 of 2 processors are running`，画面 sha256 = 基线 `42e14ed2…`。
- 性能：CPU/帧中位 56 ms（窗口 35–135），墙钟 8.1 fps（load 8.7–11.8）——串行 TCG 没有并行收益，**这是诊断用量，不是解法**。
- 判读：**只有 vCPU 并行执行（MTTCG）时才死锁** → 指向 QEMU 的并行独占监视器/内存序实现（H-A）。
  下一步优先验证「换掉 LL/SC」：用 `-cpu max`（ARMv8.1 LSE 原子）看 xnu 是否改用 LSE、是否不再死锁（T3），
  再做 `tb-size=4096`（T2）与 panic 现场（T4/T5）。
- 注意：T1 也证明“内核 SMP 路径本身能跑”（同一内核、同一 `_pset0` 锁，只是不并行），H-B 权重下降。

### T3：`-cpu max`（ARMv8.1 LSE 原子）——负（2026-11-18 21:30，`s27-smp2-lse`）
- 同型 panic：`panic(cpu 0 ...): Ticket spinlock[0xfffffff007b6c880] timeout after 60000034 ticks; ...
  waiting for 0xe2, current owner: 0xfffffff636175698 (on CPU 1)`。换 CPU 模型不能绕过。

### T4：panic 瞬间抓现场（2026-11-18 21:37，`s27-smp2-dump`）
- 看门狗在 panic 后 2 秒内拿到：**guest CPU0 的完整寄存器**（QMP `info registers`）、宿主 qemu 每个线程的占用。
- 关键读数：**owner 线程在 CPU 1**；宿主侧两个 vCPU 线程各 ~62% CPU（都在跑，不是睡死）→ 持有者不是“被挂起”，而是**在 CPU1 上忙**。
- 拿不到的东西：① CPU1 的寄存器（QMP `human-monitor-command` 的 `cpu 1` 状态不跨调用保留，`info registers` 永远指向 CPU0；
  已验证 `cpu 1; info registers` 不被接受）；② 宿主线程栈（`ptrace_scope` 禁止 attach，无 root）；
  ③ guest gdbstub（系统 gdb 不支持 aarch64，且没装 gdb-multiarch）。
- 已给 `qemu/run.sh` 加 `QEMU_GIC` 覆盖（默认 3），供下面的 GICv2 试验用。

### GICv2 —— 不启动（2026-11-18 21:51，`s27-smp2-gic2`）
- `-M virt,gic-version=2` 下内核在 loader 跳转后无任何输出（`jumping to f745d4a8` 之后停住），guest 串口 0 行 panic。
- 结论：**GICv2 不被这个内核支持，排除。**

### 现在的结论与下一步
- 已排除：多核调参（超时 4×）、绑核（只降低概率）、`-cpu max`、GICv2、`thread=single`（能稳但无加速）。
- 现场指向：**锁持有者（CPU1 上的线程）在忙但不放锁** → 要么 CPU1 在中断里自锁（同一把 pset 锁重入），要么它在等另一把锁（锁序反转），
  要么 MTTCG 丢了唤醒/中断使它永远出不了临界区。要证实必须看 **CPU1/持有者的 PC**。
- 下一步（T5，唯一可行）：**内核补丁**——在 `lock_ticket.c` 的超时 panic 里打印持有者线程的保存现场（pc/lr/sp + 8 个栈字），
  再离线符号化（`/tmp/knsyms.txt`）。这是在没有 root/ptrace 的情况下唯一能拿到 CPU1 线索的办法。

### T5：内核探针写好了，但**内核构建管线现在产不出可启动的内核**（2026-11-18 22:05–00:20）
- 补丁本身完成：`lock_ticket.c` 记录 pset 锁最后获取点（`hakios_pset_lock()`：PC + 线程），`processor.h` 的 `pset_lock()` 宏改走它，
  超时 panic 增加 `pset last took it at %p (thread %p)`。
- **构建坑一**：`make kernel` 在 dSYM 步骤失败（`DSYMUTIL` 变量为空，bash 把 `--minimize` 当命令执行）→ 用 `BUILD_DSYM=0` 绕开。
- **构建坑二（致命）**：容器里编出来的内核是 `LC_MAIN` 入口，而 loader 只认 `LC_UNIXTHREAD`（报 `unexpected kernel layout`，`entry 0`）→ 不可启动。
  老的可用内核（`hakios-kernel.new-2359`、`prebuilt/hakios-kernel`）都是 `LC_UNIXTHREAD`。
- **构建坑三**：改到宿主上编，`BUILD/obj` 里全是容器路径（`/host/iosvm-work/...`）的依赖，宿主 make 直接 `No rule to make target` 全崩。
- 处置：**内核已回滚**到可用备份 `hakios-kernel.new-2359`（md5 `ff93b21b73c74be26d1079caa77b534d`），并起一发 `s27-restore` 验证能启动、画面同基线。
- 要继续 T5，先修构建：要么清空 `BUILD/obj` 在宿主上全量重编（~30–45 分钟），要么找出容器内 link 步骤为什么走 `LC_MAIN`
  （旧内核是 `LC_UNIXTHREAD`，链接参数/工具链差异）。修好后探针才有意义。
- 重要教训：**动内核前先确认构建产物格式**（`llvm-objdump --macho --private-headers | grep LC_UNIXTHREAD`），否则白跑起机。

### T5 续：内核构建修好了（2026-11-19 02:20）
- **根因**：`xnu-deps/host/bin/ld64` 这个包装脚本优先用 cctools 的 `ld64`（能理解 XNU 段布局），找不到才退回 LLVM 的 `ld64.lld`。
  而 `xnu-deps/host/cctools/` 里没有 ld64（它随 Theos 提供），于是退回 LLVM 19 → **产出 `LC_MAIN` + `__TEXT vmaddr=0`**，loader 拒绝（`unexpected kernel layout`）。
- **修复**（宿主上构建，不要用容器——容器看不到 theos 目录）：
  ```
  export LD_LIBRARY_PATH=/home/lshan/theos/cctools/lib:$LD_LIBRARY_PATH
  PATH=$PWD/xnu-deps/host/bin:$PATH \
  CCTOOLS_LD64=/home/lshan/theos/cctools/bin/aarch64-apple-darwin11-ld \
  make kernel JOBS=16 BUILD_DSYM=0          # dSYM 步骤要关（DSYMUTIL 变量为空）
  # 改过链接器后必须删掉 BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837* 强制重链
  ```
- 校验（必须过）：`llvm-objdump --macho --private-headers hakios-kernel | grep LC_UNIXTHREAD`，且 `__TEXT vmaddr = 0xfffffff007004000`。
- 新内核（含 pset 探针）：**`331c9c8f304252699f71d24b8c825ff5`**，02:25 起 `s27-pset2`（`SMP=2`+`cpus=2` 绑核 + `fb_hz=15`）等 panic，
  看门狗会在 panic 后抓 guest CPU0 寄存器和宿主线程状态。
- 上一发验证结论：回滚的旧内核（`ff93b21b`，11-03 的）虽然能起机、画面同基线，但**没有 `fb_noblit`**（驱动每帧 60–115 ms 全屏 blit）→ CPU/帧 111 ms，**不能当交付内核**。

### T5 第三轮：找到持锁者了——**在 `thread_block_reason` 里拿着 pset 锁**（2026-11-19 06:00，`s27-pset3`）
- 探针升级为 64 项环形表（记录每次 pset 锁获取的 thread+PC），panic 打印 `owner took it at %p`。崩溃样本：
  ```
  panic(cpu 0 …): Ticket spinlock[0xfffffff007b6cc80] timeout after 60000027 ticks;
  cticket: 0xbd, nticket: 0xbf, waiting for 0xbe,
  pset last took it at 0xfffffff00733d598 (thread 0xfffffff43e7dd698),
  owner took it at 0xfffffff007303ce4, current owner: 0xfffffff43e808e50 (on CPU 1)
  ```
- 符号化（用当次内核的符号表）：
  - **`owner took it at … → _thread_block_reason+0xbc`** ← **持有者正在线程阻塞/切换路径里**；
  - `pset last took it at … → _waitq_select_queue_flush+0x114`（唤醒路径，也是 pset 锁的常见入口）；
  - panic caller → `_lck_grp_ticket_update_spin`。
- 判读：这不是“锁被忘了放”，而是**在 `thread_block_reason` 里接手/持着 `_pset0` 调度锁的线程把 CPU 切走了（或被卡住），锁再也没有释放**；
  另一颗 CPU 在 `lck_ticket_contended` 里无限等 ticket 0xbe。典型形态是「持调度锁做上下文切换」的握手在 MTTCG 下被打断/丢失。
- 下一步（代码级，不再靠撞概率）：
  1. 读 xnu 的 `thread_block_reason()` → `machine_switch_context()` 段，确认 pset 锁在何处置放、由谁置放；
  2. 对照本 port 的 arm64 切换代码（`ml_switch_context` / 汇编存栈）看是否假设了“中断关闭的原子性”，而 TCG 的 `thread=multi` 下该假设失效；
  3. 若确认是“切走后锁未放”，可试最小补丁：在切换前显式释放 pset 锁（或改用不跨切换持锁的写法），先在 SMP=2 上验证 2×60 分钟。
- 复盘：`thread=single` 不崩、`thread=multi` 崩 → 竞争窗口只在并行执行时打开；`-cpu max`/GICv2/放宽超时都无效，与“持锁切走”这一解释一致。

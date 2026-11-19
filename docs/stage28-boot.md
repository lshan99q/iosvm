# stage 28：缩短 iOS 虚拟机开机时间（立项）

## 一、为什么现在做
- 每次实验都要等起机：单核 + `fb_hz=15` 下落盘到首帧 **12–20 分钟**（宿主 load 高时更久），
  一天只能跑十几发，这是当前迭代速度的最大瓶颈；多核/交互测试还要更多耐心。
- 目标：**把“开机到首帧”从 ~15 分钟压到 ≤5 分钟**（先看 3 分钟级是否现实），并且不改画面/帧率结论。

## 二、怎么量（先有尺子）
- 从 `logs/<tag>/serial.log` 抽取带时间戳的里程碑：loader 交接 → 内核起 → launchd 起 → 逐个 `com.apple.*` 服务 →
  SpringBoard 起 → 第一帧 `hakios-gl: f`。
- 定义 **T_first_frame**（发起 qemu → 首帧出现，用宿主墙钟）与 **T_guest_boot**（内核第一条日志 → 首帧）。
- 每次报：T_first_frame、T_guest_boot、load、镜像 md5；对比只认同 load 分档。

## 三、候选杠杆（按“收益/风险”排序，待测量后定）
1. **裁掉不参与本场景的 launchd 服务**（`JOBS=` 子集；先统计哪些服务在启动窗口里耗时最多）；
2. **跳过不必要的等待/超时**（已有 `launchd_disable_all_timeouts=1`；核对是否还有 sleep/重试）；
3. **减小镜像与 IO**：system.apfs 2.4 GB 的读取/映射；dyld 共享缓存、日志、语言资源可裁剪；
4. **内核/loader 侧**：启动阶段的驱动探测、console 输出（serial=3 很啰嗦，可降级）；
5. **QEMU 侧**：`tb-size`、`-m` 大小、`-smp 1`（已单核）、io 线程/缓存模式、预分配 ram.img；
6. **宿主侧**：起机阶段提高 CPU 配额/亲和性（现在只有 taskset，无 RT）。

## 四、纪律
- 一次只改一个杠杆；每改一次记录 T_first_frame 与画面 sha256（确保没把系统改坏）。
- 开机时间与帧率分开报，避免两项收益互相污染。
- 与 stage26/27 并行推进：UX 帧率测量、多核修复不阻塞本项。

## 五、当前
- 待测基线：`s27-ux`（单核 + `fb_hz=15`）的起机时间与首帧时间；两个子代理分别整理“启动时间线”和“可裁清单”。

## 六、第一次测量（子代理产出，2026-11-19 13:30）
数据来自 `logs/s27-ux`、`s26-final`、`s25-smp1-final`（报告原文 `/tmp/boot-timeline.md`、`/tmp/boot-levers.md`）。
- 时间线（t0 = 内核 boot-args 的 `epoch=`）：**launchd 在 +4.7~5.2 s 就起来了**，SpringBoard 拉活在 +7.2~10.5 s；
  但**首帧在 +83~98 s**，而 **f2→f3 之间有一个 418 / 543 / 711 s 的巨大空档**（占“到首帧”时间的 83–88%）。
- 那个空档里在干什么：**lsd（LaunchServices，pid 32）对 2240 个路径做了 6358 次 `stat`**（widget appex 路径重复 9–14 次），
  并且**每一次 stat 都往串口打一行 `hakios: stat-begin/stat-done`**（一次启动 1.1 MB / 12.9k 行串口输出），
  另有 702 条 “denied attempt to escape CPU monitor”。空档期间 CPU 占用 95–101% → **是计算+串口 IO，不是等 IO**。
- 结论：**开机慢的主因是 launchd/lsd 的启动扫描 + 每条 stat 的串口日志，而不是 QEMU/loader/内核**（后三者在 5 s 内就完成）。

## 七、候选杠杆（子代理清单，按收益/风险排序）
1. **把 `/private/var` 持久化到 virtio-blk 上的 HFS+ 镜像**（`virtio_blk=1 var_hfs=rvblk0`，init 已支持）：第二次起机可跳过 LaunchServices 数据库重建，预计 **2–4 分钟**；风险：HFS+ 写路径在 TCG 下、system.apfs 重建后缓存失效。
2. **裁 launchd 服务集**（`WIFID=0 CHRONOD=0`，去掉 analyticsd/diagnosticd/fseventsd/syslogd/pkd/profiled 等 + 我们自己的 boot-only jobs）+ `QUIET=1`：**30–120 s**，风险低。
3. **裁 system.apfs 里的 appex/PlugIns**（`select-system.py:148-149`）：**2–4 分钟**；风险：widget/扩展/截图行为变化。
4. **关掉内核调试日志**：本次已做第一步（`hakios_stat_trace=1→0`，少 12.9k 行）；进一步可去 `debug=0x8`：**5–20 s**，风险低（丢内核日志）。
5. **SMP=2 绑核**：用户态阶段 1.5–2×（3–5 分钟）；风险高（`_pset0` panic 3/5 长跑）——等多核修好再上。
- 不追的方向：`-m 12G→7G`（只影响宿主内存）、`tb-size 4096`（0 收益）、`-cpu max`（无收益）。

## 八、动作
- 已改并重编内核：`hakios_stat_trace = 0`（`bsd/vfs/vfs_syscalls.c:99`），新内核 **`e7ea15fe7639829b579d560c2f2d8407`**（`LC_UNIXTHREAD` ✓），下一步用它量一次 `T_home` 与旧内核对比。
- 其次按 2 → 3 → 1 的顺序试（先低风险高收益）。

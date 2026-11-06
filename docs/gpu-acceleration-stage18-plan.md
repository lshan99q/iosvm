<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十八阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十八阶段工程文档

> 建立时间：2026-11-06 20:00
> 前置：第十七阶段（结果见 `IOSVM-GPU加速第十七阶段工程.md` §八–§十二：读回目标修复、逐笔整幅哈希、
> widget/壁纸证据链、启动不稳定）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **一个真 bug 修掉了，但它不是标签的最后一环**：42.4% 的帧把「最小的那层（64x19 label tile）」当画面上传
   （s16tile 实测 1419/3350）。`GLWIRE_READ_PANEL`（默认开）改成从「本帧最后画进去的、面板尺寸的」目标读回；
   s17probe4 的 298 帧里小层回读 **0 帧**。截图 245,994 B，**标签仍未出现**。
2. **第十六阶段 §十六 的读法被推翻**：UI 帧逐笔整幅哈希显示，最后一笔（program 0）**不改面板**，合成之后的 6 笔
   只抹掉 4,887 个白像素里的 467 个（9.6%）。「每合一个 label 整个面板重建一遍」不成立。
3. **新的头号阻塞是客人机内核**：`input_poll -> thread_call_enter_delayed_internal -> timer_call_enqueue_deadline_unlocked`
   的 ticket spinlock 超时 panic。一天 6 发里 5 发崩在它上面（2–9 分钟不等），**与 GL 无关**（崩的那几发只跑了 2 帧）。
4. **壁纸不是上游缺件**：全量镜像有 `com.apple.PosterBoard` / `LaunchAngels` / 48 张 HEIC；启动镜像 `build.sh` 把
   `LaunchAngels` 删空了，`root.apfs` 只有 32 个 job、没有 PosterBoard → SpringBoard 两次 lookup 失败 → 宿主 0 张壁纸纹理。
5. **widget 两个面不由我们这条 GL 链路画**：GL 库只被 backboardd `dlopen`，SpringBoard 只有 sb 库；`312x202` /
   `184x124` 从未被 `texImageIOSurface:`/`attachImage:` 绑定，也从未进 28 张 image 列表。
6. **圆角**：legacy 格式（GL_ALPHA/LUMINANCE）在本机 4.6 兼容上下文里**原生可传**，已改成「先原生、拒了再展开」；
   待截图统一口径测 875/1600。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | **先修启动稳定性**（spinlock）vs 先攻标签 | 先攻标签：每发 15 分钟里 5 发崩，实验吞吐接近 0；先修稳定性：可能花 1–2 天在「不是本项目的 bug」上 | 可重复的 900 s 测量 |
| D2 | 标签的下一层：**客人机侧显示面写入**（picture 拿到了但没有/被覆盖）vs **宿主侧影子补丁** | 影子补丁：快，但会把「中间态」当最终态；客人机写入：要回到 eagl/驱动，链路长 | 稳定标签 |
| D3 | 壁纸：**补回 PosterBoard job**（改启动镜像）vs 先做拉面/widget | 补 job：可能连带启动更多守护进程、拉长启动时间；拉面：直接补内容但要把 ciq 库建进镜像 | 壁纸或 widget 之一 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| S0 | 启动稳定 | **连续 3 发**都能跑到 UI（`ui at` 非 0）且不 panic；同一 `QEMU_SMP` 口径 | 全部实验 |
| P0 | 标签上屏 | 连续 3 发里 ≥2 发首页有 App 名称；截图 ≥300 KB；同帧 `diffdump --tolerance 8` ≥95% | 可用性 |
| P1 | 文字质量与圆角 | 无重影；widget 左上角 40x40 白像素回到 **875/1600**（与展开版 1600/1600 对照）；标签为白 | 可用性 |
| P2 | 壁纸 | 主屏背景非纯黑；或给出「补回 job 后仍不出」的下一层证据（宿主收到 1125x2436 纹理 / 或 poster 进程崩在哪） | 可用性 |
| P3 | widget | `312x202`/`184x124` 以纹理进入宿主（日志有 attach/upload 行）；否则交付「拉面」的设计+最小实现（时间盒 2 天） | 可用性 |
| P4 | 收尾 | 四格表（纯本地/按引用/W1.5/W1.5+W3）各一次 20 s 滚动窗口；`0x502` **宿主侧 0**；`make test`；文档与推送 | — |

---

## 三、实验计划（按顺序，每步停下来写结论）

### S0：启动稳定性（0.5–1 天，先做，否则什么都测不了）

**已完成的两件事（第十七阶段末尾，这里只记结果）**：

| 试验 | 配置 | 结果 |
|---|---|---|
| 四发对照 | `QEMU_SMP=4`，宿主另有 400% CPU 的 Windows 虚拟机 | 6 发 5 崩，2–9 分钟不等 |
| 减 vCPU | `QEMU_SMP=2`（s17fix1） | **仍然崩**（3 帧后，20:02）→ 少 vCPU 不是解 |
| 关输入设备 | `virtio_input=0`（s17noinput） | **仍然崩**，但栈换了（wifid / kqueue / workq） |

**根因（两发对照后修正）**：**不是 `input_poll` 专属**。第一次的栈是 `timer_call_enqueue_deadline_unlocked`
（input_poll 的 8 ms 自排期正好撞上），关掉输入设备后仍崩、改成 `__psynch_mutexwait -> kqueue_scan ->
filt_wlprocess -> workq_thread_set_max_qos` 的 ticket spinlock。共同点是：**vCPU 持锁期间被宿主调度走**。
QEMU 配置是 `-accel tcg,tb-size=384`（无 `thread=`，即默认 **MTTCG**，每 vCPU 一个宿主线程），宿主另有
一个 400% CPU 的 Windows 虚拟机。

1. **四个候选（按代价）**：
   - 宿主侧给 qemu 让路：`nice -n -10` / `chrt` / 关掉那个 Windows 虚拟机（后者要主人同意）；
   - `-accel tcg,thread=single`：去掉 vCPU 线程之间的抢占（慢，但确定）；
   - 减 vCPU（已试 `QEMU_SMP=2`，**没用**）；
   - 客人机内核把 ticket spinlock 改成可让出的锁（代价最大）。
2. **判据**：连续 3 发到 UI、0 panic；每发记录 `QEMU_SMP`、accel 参数、宿主负载、`ui at`、结束原因。
3. **注意**：`-accel` 已经在 `qemu/run.sh` 里写过，加第二次会被 QEMU 拒绝，要在 `run.sh` 里改那一行（或加一个
   `QEMU_ACCEL` 变量），改完必须记进文档（测量口径变了）。
3. **客人机侧（真修）**：`qemu/run-darwin.sh` 的 `virtio_input=1` 与内核 `input_poll` 的定时器；查 `xnu/files` 里
   `input_poll` 的调用频率与 `thread_call_enter_delayed_internal` 的锁。可先做「关掉输入轮询」的一发对照
   （没有触摸就等于没有输入，但能证明归属）。
4. **判据**：连续 3 发到 UI 且 0 panic；记录每发的 `QEMU_SMP`、宿主负载、`ui at`、结束原因。

### P0：标签（1–2 天）

1. **先把「画面里到底有没有那 4,887 个白像素」钉死**：UI 帧同时 dump **shadow（合成后第一笔）**与 **picture（帧末）**，
   两张图做像素差；若 shadow 有、picture 没有 → 是「后续 draw 覆盖」；若两张都没有 → 是「合成没画进去」。
   （`GLWIRE_PANEL_SHADOW=1 GLWIRE_SHADOW_DUMP=<prefix> GLWIRE_SHADOW_EVERY=25`，影子只记不应用。）
2. **若 shadow 有、picture 没有**：`GLWIRE_SHADOW_MODE=1/2` 二选一上屏，三发验证；同时查「那一帧的 6 笔重建」是哪一层
   （program 5/17/23 的着色器源码已能 dump，`GLWIRE_PROGRAM_ALL=1`）。
3. **若两张都没有**：回到客人机侧，查 `eagl_remote_surface()` 写显示面的 rows 与 `IOSurfaceLock` 时序
   （W3 路径），以及 program 13 的 `darkenSover` 是否把 alpha 乘成 0（`GLWIRE_LABEL_ALPHA`）。
4. **判据**：≥2/3 发首页有 App 名称、截图 ≥300 KB、diff ≥95%。

### P1：圆角与文字质量（0.5–1 天）

1. **圆角**：legacy 原生上传已默认（先原生、拒了再展开）。判据：同一截图区域内白像素 **875/1600**（±2%），
   且标签仍正确；与 `GLWIRE_LEGACY_FORMATS=1`（强制展开，1600/1600）对照。
2. **重影**：同一帧 dump panel 与 shadow，判断「同一批字画两次」还是「合成两份」。
3. **标签颜色**：program 15 的 mask 合成色是白还是 shader 的 color——用截图判定。

### P2：壁纸（1 天，先做判定再动手）

1. **补回 job**：`root.apfs` 的作业表加 `com.apple.PosterBoard`（`build.sh` 的 `rm -rf LaunchAngels` 要看清理由），
   或把 `LaunchAngels/com.apple.PosterBoard.plist` 复制进 root.apfs 的 `/System/Library/LaunchAngels`。
2. **判据**：SpringBoard 的 lookup 不再失败；宿主日志里出现壁纸尺寸纹理（1125x2436 或 750x1624 的全屏底图）；
   主屏背景非纯黑。失败也要给出「poster 进程崩在哪」的一行。

### P3：widget（时间盒 2 天，Spike）

1. 先回答「SpringBoard 的面到底怎么到渲染服务器」：`CAImageQueueRegisterIOSurfaceBuffer` 钩子（`inject_ciq_probe.c`
   现成）→ 拿 `{ID,w,h,stride,format,屏幕矩形}`；把这四元组经 XPC/共享内存交给 backboardd。
2. 宿主：`dispatch.inc` 按 201 的模式加一条「按 surface ID 拉取」，段表用 `remote_vtop()`，读成纹理贴在面板上。
3. **时间盒到期只允许三种结论**：可行 / 不可行 / 需要更多信息（附缺什么）。代码不进主干，除非有测试价值。

### P4：四格表与收尾（1 天）

四份构建（纯本地 / 按引用 / W1.5 / W1.5+W3）各一次 20 s 滚动窗口；`0x502` 按「宿主侧 0」口径记账；
`make test`；文档同步；推 GitHub。

---

## 四、不做（已被数据否掉，别重试）

1. **不再把「tile 有像素」当文字画进去了**：`GLWIRE_BACKDROP=1` 会把 64x19 tile 填成上一帧面板的缩略图，
   1,216 个像素全非零，与文字无关（第十七阶段 §十一）。
2. **不再说「合完 label 整个面板被重建」**：逐笔整幅哈希显示最后一笔不改面板、后 6 笔只改 9.6% 的白像素。
3. **不再猜 binding**：客人机自报 `e->framebuffer` 已上线（第十五阶段）。
4. **不再用「第二条 context / 第二个进程」解释任何东西**：20000 笔 draw 全 ctx 0，一条连接。
5. **不动内核做 GPU 的事**：本阶段唯一允许的客人机改动是 `input_poll` 的定时器（S0），且必须先有对照。

---

## 五、验证环境与操作规范

- 一把 VM 锁 `/tmp/iosvm-vm.lock`；同一时间只跑一发；每发 900 s（探针发 760 s 够）。
- 启动：`tools/glwire/harness/boot-measure.sh <label> <root.apfs> <secs>`；`QEMU_SMP` 默认 4，S0 的效率对比用 2。
- 用户态重建：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`。
- 宿主探针：`GLWIRE_KEEP_BINDING`（默认开）、`GLWIRE_BACKDROP`、`GLWIRE_READ_PANEL`（默认开）、
  `GLWIRE_PANEL_PROBE`、`GLWIRE_PANEL_SHADOW` / `GLWIRE_SHADOW_APPLY` / `GLWIRE_SHADOW_MODE` /
  `GLWIRE_SHADOW_DUMP` / `GLWIRE_SHADOW_EVERY`、`GLWIRE_PROGRAM_ALL`、`GLWIRE_LEGACY_FORMATS`（0 原生优先 /
  1 强制展开 / 2 只原生）、`GLWIRE_DRAW_LOG`、`GLWIRE_FBO_LOG/TRACE`、`GLWIRE_TILE_TRACE`。
- 证据图放 `iosvm/docs/demo/`（名字带日期与场景）。**崩了的发也要留日志**（`/tmp/<label>-host.log`、串口日志）。

---

## 六、里程碑

| 天 | 目标 | 产出 |
|---|---|---|
| D1 | S0 归属 + 两个变量 | 3 发不崩的配方（或「宿主负载」的量化结论） |
| D2 | P0 shadow/picture 成对图 | 「合成没画进去」还是「被覆盖」的判定 |
| D3 | P0 修法 + 3 发验证 | 标签上屏的截图（或下一层证据） |
| D4 | P1 圆角/重影 | 875/1600 的截图与对照 |
| D5 | P2 壁纸 job | lookup 不再失败 / 壁纸纹理进宿主 |
| D6 | P3 widget spike | 可行/不可行结论（时间盒） |
| D7 | P4 四格表 | 四个数字 + `make test` + 推送 |

---

## 七、诚实的预期

- **S0 已经不是我们自己的 bug**：两发对照（减 vCPU、关输入）都证明是 MTTCG 下 vCPU 被宿主调度走的
  ticket-spinlock 超时。本阶段的判据改成「在安静宿主上 3 发不崩」并记录宿主负载与 accel 参数。
- **P0 的 4,887 个白像素有一半概率不是标签**（可能是 widget 背景或图标描边）。shadow/picture 成对图能一次分开。
- **P2 补回 poster job 大概率要重建 root.apfs**，会拉长启动；判据里要同时记 `ui at` 是否变差。
- **P3 的拉面是唯一能让 widget 有内容的路线**，但它跨进程，时间盒到期就按规范收结论，不硬做。

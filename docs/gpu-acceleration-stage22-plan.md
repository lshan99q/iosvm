# Stage 22 计划：extension check-in（壁纸 + 真 widget 的共同前置）

> **状态：计划，未执行。** 只写目标、判据、预算和边界；不预写结论。
> 背景：Stage 19 P2 壁纸未达成、P3 真 widget 不可行，共同阻塞是 extension
> check-in；Stage 20 E1 判 C（不可判/不可行）。本阶段只查这一件事。

## 1. 目标

搞清 extension 在 **exec 后 5–13 s 被杀**的根因，让 **CheckIn 能发出**；解锁：

- 壁纸（P2）：PosterBoard/extension 路径；
- 真 widget（P3）：extension 托管的实时 widget 内容。

**不**在本阶段直接做壁纸/真 widget 的视觉交付；先解 check-in 前置。

## 2. 已排除（写清，防重复烧时间）

以下均已试过，**不再作为本阶段变量**：

1. `ExtensionWatchdogTimeout=120`：无效。
2. `launchd_proxyless_spawn=0`：无效。
3. 四类 job 对照（profiled/powerlog/Video/ImageConversion 等）：无效。
4. `ExtensionWatchdog=false`：**让 boot 停住**（700 s 仍 2 帧/无 UI/extension 未 spawn），不是修复。

## 3. 现象（要解释的对象）

- extension **exec 后 ~0.5 s**：
  `Failed to send exception EXC_CORPSE_NOTIFY. error code: 5`
  → 随后 `Extension is hanging on launch. Killing.`
- **`CheckIn` 从未发出**（不是 CheckIn 超时后才被杀；是根本没走到 CheckIn）。
- 被杀时间窗：**exec 后 5–13 s**（不同 extension 不同）。
- 需要回答：**谁**在什么时刻、根据什么条件发起 kill；corpse 与 kill 的先后/因果关系。

## 4. 预算与节奏（硬约束）

- **最多 3 发 boot**（不是 3 轮无限重试）。
- **每发必须先写结论，再跑下一发**；没有结论不发下一发。
- 3 发仍不能判定 → 写 **「需更多信息」**，列出所需工具/权限，停止烧 boot。
- 每发必须记录 **system md5 + lib md5**；不写视为不可比。
- 起步 base 用 Stage 20 E1 control 的配置（见 `docs/gpu-acceleration-stage20-e1-control.md`）；改 base 必须显式说明。

### 每发结论的最小内容

1. 本发唯一变量（只动一个）；
2. system/lib/root md5；
3. spawn → INIT → corpse → kill 的时间线（含 pid、发起者）；
4. CheckIn 是否出现；extension 存活秒数；
5. 支持/否定哪个假设，下一发怎么证伪。

## 5. 判据（三条，按顺序）

1. **完整生命周期时间线**：spawn → INIT → corpse → kill，含**发起者**（launchd/xpcproxy/watchdog/其他）和时刻；不是只有“被杀了”。
2. **排序假设 + 证伪方式**：至少给出排序后的假设列表，每条写“什么证据会支持/否定它”；不能只列现象。
3. **成功判据（任一发）**：extension **活过 60 s** 且日志出现 **`checked in as alive`**。

> 注意：判据 1/2 是“查清”，判据 3 是“修通”。三条不必同一发完成；但 3 发预算内至少要拿到 1/2，否则按 §4 收口为「需更多信息」。

## 6. 发次结构（不预设结论）

- **Boot 1（基线，不改变量）**：只采 lifecycle + 发起者 + CheckIn 缺失点；按结果给假设排序。
- **Boot 2（第一假设，单变量）**：只改一个点（如 dyld/签名/entitlement/persona 或 launchd/XPC 路径之一；由 Boot 1 定），验证是否让 CheckIn 出现/存活变长。
- **Boot 3（第二假设或确认）**：只改一个点；若前两发已指向明确根因，本发做确认；否则做第二假设证伪。

- 三发都不动 timeout/job/watchdog（见 §7）。
- 不以“多跑几次看看”代替假设；每发必须能回答一个具体问题。

## 7. 不做

1. **不再调 timeout**（`ExtensionWatchdogTimeout` 已证无效）。
2. **不再调 job**（四类 job 对照已证无效）。
3. **不再翻 watchdog 布尔**（`ExtensionWatchdog=false` 让 boot 停住）。
4. **不碰宿主 GPU**（不装驱动、不 vfio/IOMMU/GRUB）。
5. **不启动 virtio-gpu**（大工程，仅评估）。
6. 不在 check-in 未解前重开 P2/P3 的视觉交付；不写「壁纸/真 widget 已可用」。

## 8. 如果 3 发不判定：需要什么

> 可复用的 grep 串/事件表见 `docs/s22-extension-checkin-observability.md`（草稿，未 boot）。

按“缺什么写什么”列，不泛泛写“继续查”：

- **launchd 侧**：verbose/serial 日志、job 的 `ProgramArguments`/`EnvironmentVariables`/`MachServices` 全量、spawn 失败返回码。
- **xpcproxy/CheckIn 侧**：`xpcproxy` 的 exec/boostrap 日志、CheckIn 协议报文、`xpc_main` 调用栈。
- **进程侧**：dyld 加载失败、codesign/entitlement/persona 校验、`taskgated` 日志、`EXC_CORPSE_NOTIFY` 的发送者与 reason。
- **内核侧**：如需要，XNU 调度/异常路径 trace（需内核符号/调试权限）。
- **工具/权限**：guest 内可用的 `launchctl`/`log`/`dtrace`（或等价）权限；宿主侧能读 guest 串口/panic log；必要时可改 launchd/extension 二进制的构建链。

## 9. 结论（3 发预算用完，2026-11-12）

三发固定为一条结论链（**不再 boot，等 code 的内核修**）：

| 发 | 设计 | 结果 | 计入结论 |
|---|---|---|---|
| **ext1** | `launchd_disable_all_timeouts=1` + `launchd_log_serial=1` | acceptance 660 s 早于首个 extension 666 s，且串口只剩 2.3K loader 头 | **否**：没走到 extension 阶段、串口丢失 → 设计问题，不计入 |
| **ext2** | 去掉 `log_serial`；`MINFRAMES=999999`、`UI_GRACE=400` | 全部 timeout 关掉后 **5 s 杀消失**（`Extension is hanging` 0、`CHECK_IN_TIMEOUT` 0），6 个 widget 全部 spawn 无 hang；但 **`checked in as alive` 仍 0** | **是：H1 部分成立** —— timeout 不是唯一原因 |
| **ext3** | ext2 + `ustacks=PodcastsWidget`（680 s 起 90 次） | 90 次采样全部 `_mach_msg2_trap → _host_info → ___pthread_init → _libSystem_initializer`；首采样 `_task_self_trap → libkernel_init → libSystem_initializer`；pid 45 全程活着 | **是：H2 确认、H3 排除** —— 卡在 libSystem 初始化，从未到 main/CheckIn；无 xpc_main/bootstrap_check_in 帧 |

**最终判定**：

- **H1 部分成立**：5 s launch watchdog 关掉后不再杀 extension，但 CheckIn 仍不发 → timeout 不是唯一原因。
- **H2 确认**：extension 活着但卡在 `libSystem` 初始化（`pthread_init → host_info → mach_msg2_trap`），从未到 main/CheckIn。
- **H3 排除**：没有 `xpc_main`/`bootstrap_check_in`/`xpc_connection` 帧；`mach_msg` 是 `host_info` 内部。
- 根因指向 `libsystem_pthread` 的 `pthread_init`/`host_info`（及内核 host_info trap），属**源码/重建修复**，不是 boot-arg。

**预算：3 发已用完。** 下一步 = 内核侧 `host_info` 修；修好后另给 **2 发（ext4/ext5）**：

1. **ext4**：只验 `host_info` 是否立即返回 —— 判据：日志里 extension 进程不再停在同一栈（可复用 ustacks 采样）；
2. **ext5**：验 CheckIn —— 判据：出现 `checked in as alive`（extension 活过 60 s），或 `Extension is hanging` 次数显著下降。

每发归档 `logs/s22-ext/ext4|ext5/`，每发之间先写结论。**等 code 的内核补丁再起 boot，不空跑。**

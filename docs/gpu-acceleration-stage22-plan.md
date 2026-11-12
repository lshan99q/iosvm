# Stage 22 计划：extension check-in（壁纸 + 真 widget 的共同前置）

> **状态：执行中（ext1–ext3 已跑，H2 确诊）。** 原 3 发预算已用完；因 H2 是
> 源码级可修，追加 ext4/ext5 两发。只记录有证据的结论，不预写后续结论。
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

## 4. 已执行结果（ext1–ext3）

### ext1：无信息（未到 extension 阶段）

- `launchd_disable_all_timeouts=1 launchd_log_serial=1` 进了 cmdline，boot 到
  `end=acceptance ui=660s`、截图 320K、host 242 帧；但 guest 串口只落 2.3K
  boot 头，extension 五个观测项全 0。
- **结论：本发不能判 H1**；只证明 root/system 能到 UI、boot-arg 进了 cmdline。
- 证据：`docs/s22-ext-01.md`、`logs/s22-ext/ext1/`。

### ext2：H1 部分成立（watchdog 是门，CheckIn 仍 0）

- `launchd_disable_all_timeouts=1`（去掉 serial）生效：
  `Extension is hanging=0`、`CHECK_IN_TIMEOUT=0`、`killing extension=0`。
- 但 `checked in as alive` 仍 **0**；6 个 widget 全部 `spawned` 后停在
  `uncorking exec`。
- **结论：H1 部分成立**——5 s launch watchdog 是杀进程的门；关掉后仍没有
  CheckIn，剩 H2/H3。
- 证据：`docs/s22-ext-02.md`、`logs/s22-ext/ext2/`。

### ext3：H2 确诊，H3 排除

- live `PodcastsWidget[45]`，680 s 起 90 次 ustacks，thread 0x223 每次 16 帧；
  pid 在采样期间未死，`checked in as alive=0`。
- 符号化后，后续 90 次全部：
  `_mach_msg2_trap + 8` ← `_host_info + 228` ← `___pthread_init + 164`
  ← `_libSystem_initializer + 132` ← dyld initializers。
- 首个采样是 `_task_self_trap + 8` ← `___libkernel_init + 52` ←
  `_libSystem_initializer + 72`。
- **H2 确诊**：extension 活着但卡在 dyld/libSystem 初始化，**从未到 main/XPC/CheckIn**；
  `mach_msg` 在 `pthread` 的 `host_info` 调用内部。
- **H3 排除**：栈里没有 `xpc_main` / `bootstrap_check_in` / `xpc_connection` 帧；
  不是 CheckIn 等待。
- 修复目标是 `libsystem_pthread` 的 `pthread_init`/`host_info`（及内核
  `host_info` trap），是**源码/重建修复**，不是 boot-arg。
- 证据：`docs/gpu-acceleration-stage22-ext3-symbols.md`、`docs/s22-ext-03-symbolized.txt`、
  `docs/s22-ext-03.md`、`logs/s22-ext/ext3/`。

## 5. 预算与节奏（更新）

- 原预算 **3 发已用完**（ext1/ext2/ext3）；因 H2 是源码级可修，**追加 ext4/ext5 两发**，共 5 发。
- **每发必须先写结论，再跑下一发**；没有结论不发下一发。
- ext4/ext5 仍不判定 → 写 **「需更多信息」**（见 §9），停止烧 boot。
- 每发必须记录 **system md5 + lib md5**；不写视为不可比。
- base 仍用 Stage 20 E1 control 配置（`docs/gpu-acceleration-stage20-e1-control.md`）；改 base 显式说明。

### 每发结论的最小内容

1. 本发唯一变量（只动一个）；
2. system/lib/root md5；
3. spawn → INIT → corpse → kill 的时间线（含 pid、发起者）；
4. CheckIn 是否出现；extension 存活秒数；
5. 支持/否定哪个假设，下一发怎么证伪。

## 6. 判据（更新）

### 已拿到（ext1–ext3）

1. **完整生命周期时间线**：spawn → INIT → corpse → kill，含**发起者**和时刻；
   ext2/ext3 已给到 extension 停在 `uncorking exec`、pid 45 存活、无 CheckIn。
2. **排序假设 + 证伪方式**：H1（watchdog）部分成立；H2（libSystem/pthread
   host_info）确诊；H3（xpc/CheckIn 路径）排除。

### ext4/ext5 的判据

- **ext4**：`host_info` **立即返回**——栈不再停在
  `host_info + 228` / `pthread_init + 164` / `mach_msg2_trap`。
- **ext5**：出现 **`checked in as alive`**，或 `Extension is hanging` 显著下降
  （与 ext2 的 0 次对比时，以 CheckIn 出现为准；不再只比 hanging 计数）。
- 最终成功判据（任一发）：extension **活过 60 s** 且 **`checked in as alive`**。

## 7. ext4/ext5 发次结构（不预设结论）

- **ext4（修 host_info，单变量）**：只改 `libsystem_pthread` 的
  `pthread_init`/`host_info` 路径（host port 的 MIG 生成/返回值），验证
  host_info 是否立即返回、栈是否离开该处。
- **ext5（确认 CheckIn）**：在 ext4 有效的前提下，只改一个点（如 host port
  MIG 返回/内核 host_info 语义），验证 `checked in as alive` 是否出现或
  hanging 是否显著下降。
- 两发都不动 timeout/job/watchdog（见 §8）；不以“多跑几次”代替假设。

### 7.1 构建前置（内核只能在宿主编）

- **容器内没有内核工具链**：缺 `bison`/`flex`/`m4` 与 `uuid` 头（`uuid/uuid.h`）；内核必须在**宿主**编。
- **`ld64.lld` 不能用来链内核**：Ubuntu LLD 18 对 `-image_base` 只给
  `warning: Option '-image_base' is not yet implemented`，链出来 **vmaddr=0**，
  loader 直接 `unexpected kernel layout`（ext4 第一次失败的真因）。
- **正确链接器**（仓库自带 cctools）：
  `/home/lshan/DSH-desktop/iosvm-work/toolchain/cctools/bin/aarch64-apple-darwin11-ld`，
  用 `CCTOOLS_LD64=<该路径>` 传给 `make kernel`。
- **必须先删中间产物再编**，否则 make 直接复制旧内核：
  ```sh
  cd /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
  rm -f hakios-kernel \
    xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837{,.unstripped}
  CCTOOLS_LD64=/home/lshan/DSH-desktop/iosvm-work/toolchain/cctools/bin/aarch64-apple-darwin11-ld \
    make kernel -j8
  ```
- **自检两条（缺一不可）**：
  1. `segments[0] = 0xfffffff007004000`；
  2. `entry_va != 0`。
  并 `strings hakios-kernel | grep -c "hakios: host_info"` = **2**。
- 本次可用内核：**15,789,552 B / md5 `4d9f32f5325a30d82e09cd8d2ac78e28`（13:12）**。
- **`qemu/payload.bin` 需重新生成**，否则跑的还是旧内核（这次踩过）。

## 8. 不做

1. **不再调 timeout**（`ExtensionWatchdogTimeout` 已证无效）。
2. **不再调 job**（四类 job 对照已证无效）。
3. **不再翻 watchdog 布尔**（`ExtensionWatchdog=false` 让 boot 停住）。
4. **不经 boot-arg 试**：H2 是源码级问题，不再用 boot-arg 绕过。
5. **不绕过 libSystem 行为**：要修就修 `host_info` 本身，不用 shim/替换库绕过。
6. **不碰宿主 GPU**（不装驱动、不 vfio/IOMMU/GRUB）。
7. **不启动 virtio-gpu**（大工程，仅评估）。
8. 不在 check-in 未解前重开 P2/P3 的视觉交付；不写「壁纸/真 widget 已可用」。

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

## 10. 如果 ext4/ext5 仍不判定：需要什么

按“缺什么写什么”列，不泛泛写“继续查”：

- **host port / MIG**：`host_info` 的 host port 定义、MIG 生成代码/返回值、
  内核 `host_info` trap 的语义与超时行为。
- **内核调试**：能读 guest 内核栈/符号、在 `host_info` trap 打断点或 trace；
  需要内核符号/调试权限。
- **libSystem 侧**：`pthread_init`/`host_info` 源码、返回值断言、调用者；
  能重建 `libsystem_pthread` 并替换进 root。
- **launchd/xpcproxy 侧**（仅作旁证）：spawn 返回码、job 环境、CheckIn 协议报文。
- **工具/权限**：guest `launchctl`/`log`/`dtrace`（或等价）；宿主串口/panic log；
  重建 libSystem 的构建链。

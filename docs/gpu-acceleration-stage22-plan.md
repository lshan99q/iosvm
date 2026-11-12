# Stage 22 计划：extension check-in（收口）

> **状态：主收口（2026-11-12）；ext14b 更正：扩展确实 exec，卡在 exec 后。**
> ext1 工具无效 / ext2 H1 部分 / ext3 H2 瞬时 / ext4 trace 0 / ext5 工具错误；
> ext8 host port 健康；ext9 spawn 后无 exec/CheckIn；ext11 probe2 host_info 全部 kr=0。
> 目标（未达成）：搞清 extension exec 后 5–13 s 被杀的根因，让 CheckIn 能发出；
> 解锁壁纸（P2）与真 widget（P3）。**P2/P3 不因本阶段重开。**

## 1. 已排除（防重复烧时间）

1. `ExtensionWatchdogTimeout=120`：无效。
2. `launchd_proxyless_spawn=0`：无效。
3. 四类 job 对照（profiled/powerlog/Video/ImageConversion 等）：无效。
4. `ExtensionWatchdog=false`：让 boot 停住（700 s 仍 2 帧/无 UI/extension 未 spawn）。
5. `ld64.lld` 不能链内核（见 §5）；不再走 LLD。

## 2. 现象（本阶段要解释的对象）

- extension exec 后 ~0.5 s：`Failed to send exception EXC_CORPSE_NOTIFY. error code: 5`
  → `Extension is hanging on launch. Killing.`
- **CheckIn 从未发出**；被杀窗 5–13 s（不同 extension 不同）。

## 3. ext1–ext3（前 3 发）

| 发 | 设计 | 结果 | 结论 |
|---|---|---|---|
| **ext1** | `launchd_disable_all_timeouts=1` + `launchd_log_serial=1` | acceptance 660 s 早于首个 extension，串口只剩 2.3K loader 头 | **工具无效**：未到 extension 阶段，不能判 H1 |
| **ext2** | 去掉 serial；延长 UI/帧窗口 | timeout 全关后 5 s 杀消失（hanging/CHECK_IN_TIMEOUT/killing 全 0），6 个 widget spawn 后停在 `uncorking exec`；CheckIn **0** | **H1 部分成立**：watchdog 是门，但不是唯一原因 |
| **ext3** | ext2 + `ustacks=PodcastsWidget` 90 次 | pid 45 全程活着；90 次栈全部 `_mach_msg2_trap → _host_info + 228 → ___pthread_init + 164 → _libSystem_initializer + 132`；无 `xpc_main`/`bootstrap_check_in`/`xpc_connection` | **H2 确认、H3 排除**：卡在 libSystem 初始化，从未到 main/CheckIn |

证据：`docs/s22-ext-01.md`、`docs/s22-ext-02.md`、`docs/s22-ext-03.md`、
`docs/gpu-acceleration-stage22-ext3-symbols.md`、`docs/s22-ext-03-symbolized.txt`。

## 4. ext4–ext5（追加 2 发）

| 发 | 设计 | 结果 | 结论 |
|---|---|---|---|
| **ext4** | 正确布局内核（15,789,552 B / md5 `4d9f32f5325a30d82e09cd8d2ac78e28`）+ `hakios_host_info_trace=1` | `__TEXT=0xfffffff007004000`，boot 到 UI 840 s；cmdline 含 trace（269/607）；`hakios: host_info` **0 次**；15 个 widget INIT/running；CheckIn 0 | **trace 0**：extension 没走到内核 `host_info()`（不是 boot-arg 没进） |
| **ext5** | `ustacks=FindMyWidgetPeo` 120 次 | 全部 `no process called FindMyWidgetPeo`；实际 `p_comm` 是 **`PeopleWidget_iO`**（pid 49）；无有效栈；CheckIn 0 | **工具错误**：目标名错，不计入 H2/H3 |

证据：`docs/s22-ext-04.md`、`docs/s22-ext-04b.md`、`docs/s22-ext-05.md`、
`docs/gpu-acceleration-stage22-ext4-ustacks.md`。

## 5. 得到的结论（ext9 + ext11 定论）

1. **`host_info` 完全正常（ext11/probe2）**：BASIC `kr=0 ncpu=1`、LOAD `kr=0`、
   `host_statistics64 kr=0`；「扩展卡在 host_info」**彻底排除**。
2. **ext3 的 host_info 栈是那个 boot 的瞬时状态**，不能当普遍根因。
3. **ext14b 更正：扩展确实 exec，卡在 exec 之后、host_info 之前。**
   - 内核 `7a4dff5340ad26786e9dc70f26e69016` 打印 `hakios: exec pid=%d path=%s`；
     ext14b 看到 exec pid 45–53（Books/People/Weather/mobilenotes/AppStore/Passbook/FindMy/Sleep）。
   - elide 只跳过 xpcproxy，仍直接 exec appex；缺 `uncorking exec` 只是日志差异。
   - exec 后无 exit/crash（仍活着）、CheckIn 0、全 log host_info 0 → 卡在 extension 自身启动。
   - ext12/13 采样 0 命中是 p_comm 名字错；exec 后 p_comm 就是真名；ext15 用真名采样。
4. **exit 78 = `EX_CONFIG`**：ext9 的 `com.iosvm.reg` 缺 `/redwood/bin/reg_app`
   （build.sh 没编），**与扩展无关**，独立小修。
5. **EXC_CORPSE_NOTIFY error 5 for pid 24**：pid 24 是 `configd`，不是扩展；
   corpse 无 crash metadata → `KERN_FAILURE`，旁证，不是 CheckIn 阻塞。
6. **已知异常（不追）**：空 header、无 reply 的 `mach_msg` 会让进程崩，probe2 复现；
   记入已知异常，不进本阶段任务。

## 5.1 ext6/ext11：host_info 探针（已由 ext11 完成）

> ext11/probe2 已证明 host_info 全部 kr=0；ext6 不再单开。以下保留探针定义作参考。

**目的**：把「extension 特有阻塞」与「host port/MIG 普遍阻塞」分开。

**工具**：repo `guest/hostinfo_probe.c` → 复制到 worktree `userland/src/hostinfo_probe.c`
（照 `net_test.m` 模板：worker 线程 + 1 s send timeout + **5 s 总超时**；输出走
**serial**）；编译到 `root/bin/`，镜像内是 `/redwood/bin/hostinfo_probe`，
`rc.darwin` 加一行跑一次。**120 s 足够，不等 UI**；记录 system/lib/root md5。
准备/注入步骤见 `docs/s22-ext-06-plan.md`、`docs/gpu-acceleration-stage22-hostinfo-probe.md`。

**判据（三种输出对应三种结论）**：

| 输出 | 结论 |
|---|---|
| `hostinfo: OK ncpu=N mem=M` | host port/MIG 一般可用；阻塞是 **extension 特有**（或 extension 走的路径特有），不是 host_info 本身普遍坏 |
| `hostinfo: BLOCKED after 5s` | host port/MIG **普遍阻塞**；extension 只是撞上同一个问题，不是 extension 特有 |
| `hostinfo: KR=…` | 请求已到内核并返回错误码（不是卡住）；按错误码查（如 flavor/参数/权限），结论是“到达但回错”，不是 MIG 无回复 |
| `hostport msg id=100` | 消息已到内核 → 问题在 **MIG 侧**（注册/分发/回复），不是用户态没发 |
| 有 hostport 行、无 `id=100` | 请求到了内核，但不是 host_info → **extension 没查 host_info** |
| 完全没有 hostport 行 | 消息没出用户态 → **阻塞在用户态 IPC** |

**已有事实（ext8 健康性 + ext9 主症状）**：

- H2：ext3 的 90 次采样指向 `pthread_init → host_info → mach_msg2`；H3 排除。
- ext8：hostport 35 行（id 200–245）、host_info 15 行全部 ret=0 → **内核服务端正常**；
- ext9：扩展 spawn/INIT 但 hostport/host_info 0、CheckIn 0、hanging 0；
- ext4 trace 0 归因于 boot-arg 截断/末尾参数，不再作为“请求没到内核”的证据；
- 因此 ext6 探针现在只作**正向对照**（预期 `hostinfo: OK`），不是主判据；
  主战场是 extension 的 bootstrap 命名空间/端口代理/sandbox/entitlement。

## 6. 交接项（给下一阶段）

### 6.1 内核侧追踪点

- `mach_msg2` 的 MIG 分发入口/返回：确认请求是否进入 `_Xhost_info`。
- `osfmk/mach/mach_host.defs` 的 `host_info` routine 注册；
  `mach_host_server.c` 的 `_Xhost_info` 是否被调用。
- 未知 routine / 未注册 MIG 路径：是否直接回错或丢弃（调用方表现为等回复）。
- 可复用 ext4 的 `hakios_host_info_trace=1`；但先证明 trace 点能被打到。

### 6.2 最小 guest 探针

- 写一个**不经过 extension** 的最小程序，直接调 `host_info(host, HOST_PRIORITY_INFO, ...)`；
- 判据：同一 system/root 下，探针能返回 vs extension 卡住；用来分离
  “MIG/host port 本身坏” 与 “extension 的启动路径坏”。
- 探针必须记录 system/lib md5，且不依赖 launchd extension 生命周期。

### 6.3 需要的构建能力

- **宿主完整 SDK**：cctools `aarch64-apple-darwin11-ld`、`libcc_kext`（容器里没有）；
- 内核重链流程：删 `hakios-kernel` 与 `mach.development.bcm2837{,.unstripped}`，
  用 `CCTOOLS_LD64`；自检 `segments[0]=0xfffffff007004000`、`entry_va != 0`、
  `strings ... | grep -c "hakios: host_info"`=2；
- `qemu/payload.bin` 自动重生，避免跑旧内核。

## 7. 基础设施收获（下次可直接用）

### 7.1 一键内核重编

`tools/kernel-rebuild.sh [marker...]`：

- 自动删 `mach.development.bcm2837{,.unstripped}`；
- 用 cctools ld64 链接（不是 `ld64.lld`）；
- 校验 `__TEXT vmaddr=0xfffffff007004000`、`entry_va != 0`、marker 串；
- 最后打印 `hakios-kernel` 的 md5，避免再手动算/拿错内核。

### 7.2 新内核与两条追踪

- ext8 内核：**md5 `1f8c489f2ca93043d53b20aa04739109`**（追踪默认开）；
  早前 `759e0363196c31dd2ebb0cb01044aff1` 为同系列带追踪内核。
- 含两个追踪：
  1. `hakios: host_info`（`osfmk/kern/host.c`）；
  2. **`hakios: hostport msg id=%d`**（`osfmk/kern/ipc_kobject.c`，
     开关 `hakios_hostport_trace=1`）。
- 用途：ext4/6 看请求有没有到内核 hostport；ext3 的 H2 栈可与此对照。

### 7.3 编译坑：kprintf 头

- 这棵 XNU 树**没有** `kern/printf.h`；`kprintf` 原型在
  `pexpert/pexpert.h`（osfmk 侧）与 `bsd/sys/kauth.h`。
- **不要再加 `#include <kern/printf.h>`**，否则编译直接失败。
- 旧基线：15,789,552 B / md5 `4d9f32f5325a30d82e09cd8d2ac78e28`（13:12）；
  新内核以 §7.2 的 `759e0363…` 为准。
- **`ld64.lld` 不可用**：Ubuntu LLD 18 不实现 `-image_base`/`-segaddr`，
  链出 vmaddr=0，loader `unexpected kernel layout`；已记入不做。
- payload 自动重生仍必须保留，避免跑旧内核。

## 8. 不做

1. 不再调 timeout（`ExtensionWatchdogTimeout` 已证无效）。
2. 不再调 job（四类 job 对照已证无效）。
3. 不再翻 watchdog 布尔（`ExtensionWatchdog=false` 让 boot 停住）。
4. 不经 boot-arg 试；不绕过 libSystem；**不再拿 timeout/job/watchdog/host_info 当解释**。
5. **不再用「elide = 不 exec」解释**：ext14b 已证 exec 发生。
6. **不再用 ext3 单发栈作结论**（瞬时状态）。
7. **不用 `ld64.lld` 链内核**（vmaddr=0 / unexpected layout）。
8. 不碰宿主 GPU；不启动 virtio-gpu。
9. 不在 check-in 未解前重开 P2/P3 的视觉交付；不写「壁纸/真 widget 已可用」。

## 9. 预算与收口

- 主预算 5 发已用完：ext1 工具无效、ext2 H1 部分、ext3 H2 瞬时、ext4 trace 0、ext5 工具错误。
- **ext8**：host port/MIG + host_info 健康；**ext9**：扩展 spawn/INIT、CheckIn 0；**ext11**：probe2 host_info 全部 kr=0；**ext14b**：扩展确实 exec，卡在 exec 后。
- 下一阶段主战场 = **exec 后、host_info 前的 extension 自身启动**；不再修 kernel host_info、不再用 elide=不 exec。
- 已知异常：空 header `mach_msg` 崩；不进本阶段任务。
- 未达成成功判据：没有任何一发出现 `checked in as alive`（extension 活过 60 s）。

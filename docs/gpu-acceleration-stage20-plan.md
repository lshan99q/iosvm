# Stage 20 计划与 E1 收口：extension check-in

> **状态：E1 已跑，终局 C（不可判/不可行），control 完成；P2/P3 未重开；fallback5 仍是 P3 交付；system 恢复 6be494f1。** 本文件定义目标、E1、退出标准、不做清单、重开判据，并记录 E1 结果（§8）；**不预写 P2/P3 结论**。
> 输入来自 stage 19 的 D4 移交：`docs/gpu-acceleration-stage19-decisions.md`、`docs/gpu-acceleration-stage19-closure.md`。

## 1. 目标

**唯一目标**：查清并（若可能）修复 **extension 在 `CheckIn` 前死亡** 的问题，使 extension 进程能活到 launchd 的 check-in。

- 这是 **P2（壁纸）与 P3（真 widget 内容）的共同前置**：stage 19 证明，extension 在 spawn 后 5–13s 被 `Extension is hanging on launch. Killing.` 杀，`checked in as alive` 0 次；P2/P3 的内容路径都卡在这里。
- Stage 20 不直接承诺 P2/P3 通过；E1 只回答「watchdog 关掉后，extension 能不能活/暴露什么 exit code」。
- 本阶段先只做 **E1**；E1 的结果决定是否进入 P2/P3 重开判据。

## 2. E1 实验：`ExtensionWatchdog=false`（bool）

### 2.1 假设

`ExtensionWatchdog` 是 bool（不是 timeout）。把它设为 `false` 关闭 watchdog，可能让 extension 不被提前杀，从而暴露真实 exit code/`CheckIn` 失败点；它不等于修好 check-in。

### 2.2 做法（一次只改这一项）

- **只改** `ExtensionWatchdog=false`；不动 `ExtensionWatchdogTimeout`，不动任何 daemon job，不动 P2/P3 的 surface/CAContext 代码。
- **30 分钟限时**：从第一发 boot 开始计时；超时即按退出标准处理，不无限深挖。
- 同 stage 19 的稳定配方：`QEMU_SMP=1`、单发、无并发 build/boot；镜像与 baseline 记录清楚。
- 记录：
  - launchd 日志中的 `extension checked in as alive` 次数；
  - `Extension is hanging on launch. Killing.` / `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT` 次数；
  - 每个目标 extension 的 pid、spawn→kill 时间、exit code/signal；
  - 是否出现 `EXC_CORPSE_NOTIFY`；`CheckIn` 是否发出。
- 产物：`logs/s20-e1/`（guest/host log、launchd 过滤、meta、结论表）；不提交二进制/日志。

### 2.3 需要的条件

- 能设置 launchd 的 `ExtensionWatchdog`（现有 limits 读取路径）。
- 能注入/编译 extension 或 launchd 侧（现有 guest 工具链 `IOSVM_USE_LLD=1`）；当前没有改 extension 二进制的产物，若 E1 需要该权限，先写清缺口再申请。
- 一次只跑一个实验；GPU 独占。

## 3. 退出标准（三条，只选一条）

| # | 观察 | 后续 |
|---|---|---|
| **A** | extension **活过 60s** 且日志出现 **`checked in as alive`** | 可进入 §5 的 P2/P3 重开判据；E1 算「watchdog 是阻塞点」 |
| **B** | 没活到 check-in，但拿到**明确 exit code / signal / 崩溃点** | 在 **30 分钟内**评估是否可在同一阶段修；能修则做一次最小 follow-up；不能修则按 C 收口 |
| **C** | 既没活过 60s/checked in，也没有明确 exit code | 收口为 **「不可行或需更多信息」**；不再投入 stage 20，除非 owner 另定 |

> **不预写结论**：A/B/C 都可能；以 E1 实测为准。E1 实际落 **C**（§8）；A/B 未发生，因此没有 A 不能重开 P2/P3，也没有 B 不能称「已定位 bug」。

## 4. 不做清单

1. **不调 `ExtensionWatchdogTimeout`**（timeout 120 已证无效；本阶段不重复）。
2. **不改 daemon job**（profiled/powerlog/MediaConversion 已证不是原因；不重复 P2d）。
3. **不盲调 `createImageSlot:` / CAContext 私有 selector**；P3 真路径的负结果仍有效（D1）。
4. **不在 E1 前跑 P2/P3**；不并发 boot/build；不用 4 vCPU。
5. **不把 E1 的 A 当成 P2/P3 已通过**；A 只是允许进入重开判据。
6. **不提交日志/截图/二进制**；只提交 `.md` 和可复现命令。
7. **不倒腾 `ExtensionWatchdog` 布尔**：E1 已判 C；不再试 false/true 组合（干净单变量未做、当前不做）。
8. **不为 byref/w15 追加性能实验**；P1 判 A、修复中，**不查 host sampler / linear / clamp**（B 已排除），只查 guest glyph/label tile 生成/行映射。
9. **不擅自改宿主 GPU 驱动 / vfio**：直通评估判不可行；不 unbind/bind、不重装驱动、不改 IOMMU/GRUB。
10. **不再 scrub glyph last row**：`glyph_last_row=1` 已 FAIL；P1 只改 label tile row61/62。
11. **不再单独调 host 侧 socket buffer**：必须 guest `setsockopt` 成功且两侧同时生效，才判候选 1。

## 5. 若 E1 = A：P2/P3 重开判据

### 5.1 P2（壁纸）重开判据

只有同时满足才重开 P2：

- E1 的 A 成立：目标 extension 活过 60s、`checked in as alive`；
- 同 boot SpringBoard 的 `posterboardservices` lookup 不再 `No such process`；
- `PosterBoard.app/PosterBoard` 启动并 running；
- host 出现 **1125x2436**（或全屏）壁纸纹理，主屏背景非纯黑；
- 不重跑 P2d 四 job；P2d/P2e 的负结果仍作为边界。

若 A 成立但上述任一不成立：P2 保持关闭，只记录「extension 活了但壁纸内容仍不满足」。

### 5.2 P3（真 widget 内容）重开判据

只有同时满足才重开 P3 真路径：

- E1 的 A 成立：widget extension 活过 60s、`checked in as alive`；
- 同 boot 层树能看到 155x155 `CALayerHost`，且能拿到**同一进程内**的 CAContext 或可用快照；
- 能证明当前 widget 内容（截图/快照非纯白/非旧内容），或出现新的零参快照 API；
- D1 的四条负结果被新证据推翻（surface 枚举/ allContexts/零参 snapshot/CheckIn 至少一条不再成立）。

否则：

- P3 真路径保持 **不可行**；
- fallback5 的 guest-side 占位仍是交付项，**不因 E1 回退**；
- 不重试 surface 3 的 rect/selector 或 `createImageSlot:` 盲调。

## 6. 输入证据 / 复现入口

- D4 移交：`docs/gpu-acceleration-stage19-decisions.md`（D4、阶段 20 移交清单）。
- 收口包：`docs/gpu-acceleration-stage19-closure.md`。
- 证据：E1 surface 枚举（`logs/s19o-p3-surface-enum/`）、E2 allContexts（§4.28）、E3 无零参 snapshot（§4.28）、E4 CheckIn/`EXC_CORPSE_NOTIFY`（§4.24/§4.25、附录 extension check-in）、E5 fallback5（`logs/s19o-p3-fallback5/`）。
- 已试负结果：`ExtensionWatchdogTimeout=120` 无效；force xpcproxy 仍死；P2d job 无关。详见 stage 19 D4。

## 7. 交付物

1. E1 结果表（A/B/C + 原始日志路径）；
2. 若 A：P2/P3 重开判据的逐条对照（不预写通过）；
3. 若 B：exit code/崩溃点 + 30 分钟评估结论；
4. 若 C：一句「不可行或需更多信息」+ 已试边界；
5. 文档更新（本文件 + stage 19 closure 的状态引用）；不提交日志/截图/二进制。

## 8. E1 结果（终局 C：不可判/不可行；control 完成）

> **判定：C（不可判/不可行）。** `ExtensionWatchdog=false` 不让 extension 活到 CheckIn，反而让 boot/extension 阶段停住；C1-limits control 正常到 UI 且 extension 阶段重演 10 次 timeout、0 alive。check-in timeout **未解决**，不是单个 limits 布尔能修。
> 来源：`docs/gpu-acceleration-stage20-e1-watchdog.md`、`docs/gpu-acceleration-stage20-e1-control.md`、`docs/gpu-acceleration-stage20-e1-rebuild-confound.md`；归档 `logs/s20-e1-watchdog/`、`logs/s20-e1-control/`。

### 8.1 E1 构建 / 配置

- `system.apfs`：**15167c8a01fe2e074caad610686df895**（20:51:10Z，2,476,752,896 B；旧 `8bf5f838`），`SYSTEM=1` 重建。
- limits：`ExtensionWatchdog=false`、`ExtensionWatchdogTimeout=120`、**`CrashOnSIGTERMTimeout=false`**（C1 是 true；第二变量）。
- root 未动（`785bcbac`）；boot 用 `/tmp/root-byref.apfs` `3207a35e`。

### 8.2 E1 boot（700s，未到 extension）

`end=qemu-exited ui=0 biggest=24960 guestframes=2 hostframes=2 panic=0`；SpringBoard 21:00:34 spawn 但无 UI；configd corpse/faults 143/166s。

| 项 | E1（false/false） | 旧 system 同 root |
|---|---|---|
| UI | **0s / 24,960 B（boot logo）** | 660s / 327,177 B |
| host frames | **2** | 1,080 |
| `checked in as alive` | **0** | 0 |
| `Extension is hanging` | **0** | **12**（first 611s） |
| `CHECK_IN_TIMEOUT` | **0** | **11** |
| `EXC_CORPSE_NOTIFY` | **4，全是 configd pid22** | configd 类 |
| extension 服务名 | **0** | present |

- `WILL_SPAWN` 68 条全是 system jobs；**extension 根本没 spawn**，0/0 不是 watchdog 生效证据。

### 8.3 Control（C1 limits，system 6be494f1）：重建不是瓶颈

- system **6be494f1**（23:07:50Z，2,476,752,896 B）；plist `CrashOnSIGTERMTimeout=true`、`ExtensionWatchdog=true`、`Timeout=120`（C1 三键）。
- boot 900s：`ui=720s ui_bytes=309280 guestframes=32 hostframes=304 MATCH=56 DIFF=248 panic=0 biggest=456750`。
- extension：hanging **10**（first PhotosReliveWidget[45]，666s）、CHECK_IN_TIMEOUT **10**、**checked in as alive 0**；exit `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT 0x3`；EXC_CORPSE 含 configd pid22 + extension 47–50。
- → **`SYSTEM=1` 重建本身不是瓶颈**：control 正常到 UI，extension 阶段也正常重演 timeout。

### 8.4 单变量旁证：`ExtensionWatchdog=false` 是坏键

- system `a3a895b0`：`ExtensionWatchdog=true`、`CrashOnSIGTERMTimeout=false`、Timeout 120；与 E1 **只差 Watchdog**。
- 650s 已出现第一条 hanging（`PeopleViewService.LockScreenPeopleWidget[44]`），共 6 条；E1（false/false）700s 仍 2 帧/无 UI/无 extension。
- 所以 **`CrashOnSIGTERMTimeout=false` 和重建都不是**；**`ExtensionWatchdog=false` 让 boot/extension 停住**。

### 8.5 为什么最终是 C

- E1 false/false：700s 仍 2 帧/无 UI/无 extension，**没有** `checked in as alive`，也没有 extension exit code；watchdog=false 不是解锁，而是坏键。
- Control 正常到 UI，但仍 **0 alive + 10 CHECK_IN_TIMEOUT / 0x3**：check-in timeout 未解决，不是单 limits 布尔能修。
- 30 分钟盒内无法再分辨「干净单变量 Watchdog=false（Crash 保持 true）」；按退出标准收口 **C（不可判/不可行）**，不再投入。

### 8.6 当前状态 / 下一步（未执行，当前不做）

- **system 已恢复 `6be494f1`**（C1 等价、可复现）；坏 `15167c8a` 备份 `/tmp/system.apfs.bak-s20-bad`，**不要复用**。
- **P2/P3 保持关闭、不重开**；P3 fallback5 仍是交付项，不回退。
- 若 owner 仍要干净单变量：working C1 base + **只改 `ExtensionWatchdog=false`**（`CrashOnSIGTERMTimeout=true`）900–1200s；预计仍 C，当前不做。
- 不写「watchdog 已修」「extension 已活」「P2/P3 已通过」。


## 9. 帧同步审计（只读，未执行）

> 来源：`docs/gpu-acceleration-stage19-gpu-route-audit.md`（code 分支 `e7b869c`，未合 main）。这里只列候选与最小实验，**不预写结论**；P4 最终数字仍以 D7 为准。
> 前提：不碰 V100 直通；只查现有 glwire 的 guest↔host 帧同步。

| # | 候选同步点 | 代码/日志证据 | 一次 boot 证伪 |
|---|---|---|---|
| 1 | **每帧 4.9 MB picture/panel 往返 + 默认 TCP 缓冲**：guest `send` 记录后 `recv` 回复；无 in-place 时 host 回送整张 4,872,000 B picture；byref 上传整张 panel；只有 `TCP_NODELAY`，**没有 SO_RCVBUF/SNDBUF** | `src/eagl_remote.c:462`（send/recv）、`:499`（仅 TCP_NODELAY）；`src/eagl.c:1339/1426`；host `serve_frame` 4048–4054 | cell3 开 `glframes=1 touch_lat=1`：看 `hakios-remote: f N wait <us>` 是否占帧周期绝大部分；`glwire_noflush=1` 若 fps 跳升 → 网络路径 |
| 2 | **逐帧 lockstep**：guest 发完必须等 host replay 完才收 reply；host 单线程 `poll`/`serve_frame`，无第二帧在途/双缓冲 | `eagl_remote_flush` 的 send 后 recv；host `serve_frame` 只在 replay/readback 后 reply；cell4 无 picture 回传仍远低于本地 | host 加 `GLWIRE_FRAME_DELAY_MS=1000` 一发：guest 帧周期若 +≈1s → lockstep；不变则不是等 host |
| 3 | **guest 本地 CA/显示等待**：`glFinish`/`glFenceSync`/`IOSurfaceLock`、TCG 软件光栅 | `frame_end` 1304–1319；`MiniIOMobileFramebuffer ... waiting for the GPU`；cell1 10.91 fps 说明本地绘制本身不慢 | `glwire_noflush=1` 仍低 → 本地/显示；跳升 → 网络/同步 |

**最小可测改动（一处逻辑：socket 缓冲）**

- guest `src/eagl_remote.c:499` 的 `TCP_NODELAY` 后设 `SO_RCVBUF`（可同时 `SO_SNDBUF`）为 8 MB；host `host_glwire.c:4340` 的 accepted fd 设 `SO_SNDBUF`。
- **用 env 开关控制，默认不变**（具体变量名按 code 实现）；不改渲染/协议/镜像；cell1 没有 socket，不受影响。
- 对照实验：cell3（`GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1`）+ **cell1 护栏**（确认本地不受影响）；记录 guest `f N wait`、host `frame ... ms`、`0x502` 仍为 0。
- 若 socket 缓冲无提升：候选 1 降级，再测 `glwire_noflush=1`（候选 3）和 `GLWIRE_FRAME_DELAY_MS=1000`（候选 2）；**不要同时改协议和缓冲**。
- **第一次验证（2026-11-10）：候选 1 未成立。** host 8MB 生效，但 guest `setsockopt` **errno 55 (ENOBUFS)**，只有单侧生效；cell3 host 窗口 **1.07 → 0.46 fps（更差）**，cell1 护栏 **10.91 fps 不变**。见 `logs/s20-framesync/report.md`。**不写已优化**；先修 guest ENOBUFS 再判候选 1。
- 未跑完；不把审计当「已修复」。

> 后续路线图（文字修好之后）：`docs/gpu-acceleration-stage21-plan.md`；Phase 0 从 P1 最后定向修 + 帧同步 clamp 复测开始。

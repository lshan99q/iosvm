# Stage 20 计划与 E1 收口：extension check-in

> **状态：E1 已跑，终局 C/需更多信息；P2/P3 未重开，计划其余未执行。** 本文件定义目标、E1、退出标准、不做清单、重开判据，并记录 E1 结果（§8）；**不预写 P2/P3 结论**。
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

## 8. E1 结果（终局 C / 需更多信息）

> **判定：C/需更多信息，不是 A/B，也不是「不可行」。** E1 没有到达 extension 阶段，`ExtensionWatchdog=false` 是否解锁 extension **未能判定**。
> 来源：`docs/gpu-acceleration-stage20-e1-watchdog.md`；归档 `logs/s20-e1-watchdog/`（`s20-e1` 同目录）。

### 8.1 构建 / 配置

- `system.apfs`：**15167c8a01fe2e074caad610686df895**（20:51:10Z，2,476,752,896 B；旧 `8bf5f838`），用 `SYSTEM=1` 重建。
- 镜像内 limits plist：`ExtensionWatchdog=false`、`ExtensionWatchdogTimeout=120`，另有 **`CrashOnSIGTERMTimeout=false`**（C1 是 true；code 20:45 改的，**第二变量**）。
- root 未动（`785bcbac`）；本 boot 用 `/tmp/root-byref.apfs` `3207a35e`（C1 的 P2d root 已不在）。

### 8.2 boot 实测

`end=qemu-exited ui=0 biggest=24960 guestframes=2 hostframes=2 panic=0`（700s）。SpringBoard 21:00:34 spawn，但无 UI；kernel 见 `Full corpse enqueued for configd`、`configd[22] Corpse failure, too many faults 2`（143/166s）。

| 项 | E1 | 旧 system 同 root（对照） |
|---|---|---|
| UI | **0s / 24,960 B（boot logo）** | 660s / 327,177 B |
| host frames | **2** | 1,080 |
| `checked in as alive` | **0** | 0 |
| `Extension is hanging` | **0** | **12**（first 611s） |
| `CHECK_IN_TIMEOUT` | **0** | **11** |
| `EXC_CORPSE_NOTIFY` | **4，全是 configd pid22** | configd 类 |
| extension 服务名（mobilecal/news.tag/ScreenTime/WidgetExtension） | **0** | present |

- `WILL_SPAWN` 68 条全是 system jobs；**extension 根本没 spawn**。所以 0/0 不是「watchdog 生效」的证据，只是没进入 extension 阶段。
- configd 的 corpse/faults 伴随新 system boot，不是 extension exit code。

### 8.3 为什么是 C（不是 A/B）

- 新 system（15167c8a）停在 2 帧/无 UI；旧 system 同 root 660s 到 UI、611s 出现首次 extension hanging。**`SYSTEM=1` 重建是 confound**：无法区分是 `ExtensionWatchdog=false`、`CrashOnSIGTERMTimeout=false`，还是重建本身导致 boot 停住。
- 30 分钟到点（21:13:46），没有做 control，也没有更长 boot；按既定退出标准收口 C，不再投入。

### 8.4 下一步 control（未执行；当前不跑）

1. **Control**：新 system + 旧 plist（`ExtensionWatchdog=true` / `CrashOnSIGTERMTimeout=true`），700–900s。若仍停 2 帧 → 重建/镜像问题，不是 watchdog；若到 UI/extension → 再做 false 组。
2. **False 组（仅 control 到 UI/extension 后）**：900–1200s，等 611s 首次 hanging 后再 **≥60s**，记录 `checked in as alive` / hanging / CHECK_IN_TIMEOUT / exit code。
3. 不并发 boot；不改 job/timeout；一次只动 `ExtensionWatchdog`。

### 8.5 当前影响

- **P2/P3 不重开**：E1 是 C，没有 A，§5 重开判据不触发。
- **P3 fallback5 仍是交付项**：不因 E1 回退，也不写 P3 真路径通过。
- 不写「watchdog 已修」「extension 已活」「P2/P3 已通过」。

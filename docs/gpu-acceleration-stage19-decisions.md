# Stage 19 收口决定（D1–D9）

- **Status:** Accepted
- **Date:** 2026-11-09
- **Owner:** lshan（总执行官）
- **Scope:** stage 19 P2/P3 收口、extension check-in 移交、剩余时间转 P4
- **Supersedes:** 无（本文件是 stage 19 的收口决定，不推翻历史实验记录）
- **Related:** `docs/gpu-acceleration-stage19-p0-runbook.md`、`docs/gpu-acceleration-stage19-plan.md`、`docs/gpu-acceleration-stage19-closure.md`、`docs/gpu-acceleration-gpu-passthrough-assessment.md`

> 本文件只记录决定与依据；实验数字仍以 runbook 各节的原始日志/报告为准。每条决定都给出「何时可推翻」，避免以后把收口当成永久结论。

---

## D1. P3 真路径 = 不可行，提前收口

**决定**：P3 的「surface → opcode 202 → 真 widget」与「CAContext 快照」两条真路径都判 **不可行**，**提前收口**，不等原时间盒 2026-11-10 14:06。

**依据**（四条独立证据）：

- **E1 — surface 枚举 30 个面无候选**：900s 枚举到 30 个存活 MiniIOSurface；id1/2 是 750x1624 显示面，id3 是 **184x124 BGRA**（stage11 旧 widget snapshot），id4 是 **312x202 LA08**（文字覆膜），id5–30 是 backboardd 的 180x180 图标/装饰面。**没有 155x155 pt（310x310@2x / 465x465@3x）或任何 widget 尺寸候选**；同 boot 950s 层树 155x155 `CALayerHost` = `0x500b566f`/`0x7f535c4c`，与枚举表任何 id 无对应。见 runbook §4.30；提交 `44f602a` / `2acbd0d`；日志 `logs/s19o-p3-surface-enum/`。
- **E2 — `+allContexts` 按进程隔离**：backboardd 只看到自己的 1 个 context（`0xc0cfe455`，与 950s 层树 155x155 `0xbf11dcae`/`0x7702cd25` 不匹配）；SpringBoard 的 `+allContexts` 有 16 个（含 6 个自己的 window context），但**不含** 155x155 widget host（`0x4741275c`/`0xccf65063`）。见 runbook §4.28 D2/D3；提交 `64c8896`、`f339014`。
- **E3 — 这代 CAContext 没有零参快照**：没有零参 `createImage`/`createCGImage`/`snapshot`；只有 `createImageSlot:hasAlpha:`（创建 slot，不是取图）。见 runbook §4.28 D3；提交 `f339014`。
- **E4 — extension exec 后 ~0.5s 死亡，`CheckIn` 未发出**：C2 force xpcproxy 日志里，pid 57 到 `running`/`INIT` 后出现 **`Failed to send exception EXC_CORPSE_NOTIFY. error code: 5`**，约 0.5s 后 `Extension is hanging on launch. Killing.`；`CheckIn` 从未发出 → 实时 widget 内容不可得。见 runbook「附录：extension check-in 判定点」；提交 `9ce6e5b`；P2e/C2 §4.24/§4.25（`f1261b4`/`eac8128`）。

**影响**：

- 停止增加 surface/CAContext/202/rect 实验；不再猜 `createImageSlot` 参数。
- P3 剩余时间只做 **guest-side placeholder（D2）**，然后转 P4（D5）。
- P2/P3 的共同前置变成「extension 进程必须活到 `CheckIn`」（D4，阶段 20）。

**何时可推翻**：

1. extension check-in 修复（D4），且同 boot 能拿到 widget extension 的实时内容；或
2. Apple/OS 侧出现新的、可用的零参快照/跨进程 render-server API，并有可复现证据；或
3. 有新的 surface 在 widget 尺寸出现（310x310/465x465 或明确 155x155 pt），并与层树 host 对上。

---

## D2. P3 交付物 = guest-side placeholder（拉面）

**决定**：P3 的交付物定为 **guest-side placeholder**（拉面），不是真 widget 内容。P3 最终结论写 **「不可行（真路径）+ 已交付 fallback」**，不写「可行」。**fallback5 已 PASS**，fallback 交付完成；仍不要写「P3 真路径通过」。

**依据**：

- host 侧 fallback2/3 都执行了（日志 `widget fallback ...`），但 **QEMU 截图来自 guest scanout（SpringBoard/panel），host fallback 画在 offscreen picture**，两者不是同一块像素；host-replay 模式也改不了。见 runbook §4.31；提交 `c3b0371`；代码 `e8a8ee7`、`2de8e65`/`baa0283`。
- 因此唯一能在 QEMU 截图里看到的路径是 **guest 侧画**：`inject_sb.c` 的 `widget_placeholder=1`，UI 后在 `_SBWallpaperSecureWindow`/root view 的 widget 区域插圆角 `UIView`（24px 棋盘 + 3px 白边框 + 两条黄对角线），日志 `static widget placeholder ... (NOT the real widget path)`。
- fallback4 已证明占位加太早（12:25:47，早于 UI marker 12:27:00）会被盖住；A1c 改成 30s 首试/每 3s 最多 300 次、找 155x155 host 加到 host.superview 并 bringToFront，sb 110,656 B/md5 `abc8f4f1…`。
- **E5 — fallback5 PASS**：日志 `static widget placeholder at 0.0,155.0 155.0x155.0 parent BSUIScrollView window SBHomeScreenWindow level -2 (NOT the real widget path)`；截图 widget 区 24px 棋盘 + 白框 + 两条黄对角线，yellow 13,733 / white 11,174 / black 1,825；见 §4.31、`logs/s19o-p3-fallback5/report.md`。
- 验收判据：截图 widget 区出现 **24px 棋盘 + 3px 白框 + 两条黄对角线**；必须标注 **「占位，不是真 widget 内容」**。

**影响**：

- P3 交付完成：**真路径不可行 + fallback 已交付（fallback5 PASS）**；可见占位不得冒充真 widget。
- host 侧 fallback 保留为分析用，不再作为视觉交付路径。

**何时可推翻**：

1. fallback5 回归为不可见（截图无棋盘/白框/黄线）→ fallback 交付失效，D2 需重开；
2. D1 的推翻条件成立，真 widget 内容可得 → 用真内容替代占位，D2 失效。

---

## D3. P2 = 关闭，记「未达成」

**决定**：P2 关闭，记 **未达成**；不再投入 P2 的 timeout/xpcproxy/daemon job 方向。

**依据**：

- 四条常规路径走完：
  1. **P2d 四 job**（profiled/powerlogHelperd/Video+ImageConversion）：仅 profiled 成功，powerlogHelperd plist 被 launchd 拒绝，Photos 仍 error 22；层树 contents 仍 0 → 依赖不是原因。见 §4.22；提交 `fdf964a`。
  2. **P2e C1**（`ExtensionWatchdogTimeout=120`）：limits plist 在镜像里且可解析，但 ~9s 仍被杀，120s 未生效。见 §4.24；提交 `f1261b4`。
  3. **P2e C2**（`launchd_proxyless_spawn=0` 强制 xpcproxy）：boot-arg 生效（`xpcproxy spawned`、`elide xpcproxy`=0），但 Batteries 7.2s、Gradient 11.4s 后仍 `CHECK_IN_TIMEOUT`。见 §4.25；提交 `eac8128`。
  4. **P2 根因审计**：extension check-in timeout 是系统性（18 个扩展被 5–13s 杀），与 P3 同源。见 §4.23/§4.24/§4.25；提交 `e3957d3`/`f1261b4`/`eac8128`。
- 结论：P2 的 lookup 已修好、PosterBoard 能起来，但壁纸内容需要 extension 存活；这与 P3 共同阻塞。

**影响**：

- P2 不再占用 GPU/构建时间；不阻塞 P4。
- 若 extension 修复，P2 可重开（lookup/geometry 的成果仍在）。

**何时可推翻**：

1. extension check-in 修复（D4）后，P2d/P2e 的条件重新具备；或
2. 找到不依赖 extension 的壁纸静态路径（且不违反 D1/D2 的收口）。

---

## D4. extension check-in 独立立项（阶段 20 候选）

**决定**：把 **extension check-in** 作为独立问题移交阶段 20 候选，不在 stage 19 P2/P3 内继续投入。

**问题陈述**：

- **现象**：同一 boot **18 个 extension 在 5–13s 被 `Extension is hanging on launch. Killing.` 杀**；GradientPoster/Batteries/WallpaperKit.CollectionsPoster 等都在 spawn 后 7–11s 收到 `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`；`checked in as alive` 0 次。
- **已试负结果**：
  - `ExtensionWatchdogTimeout=120`（limits plist）**无效**；C1 仍是 ~9s 被杀（§4.24）。
  - **force xpcproxy**（`launchd_proxyless_spawn=0`）**不是 elision**；xpcproxy 路径下仍 7–11s 死（§4.25）。
  - **profiled/powerlog/MediaConversion job 无关**；P2d 四 job 只让 profiled 起来，壁纸层仍 contents=0（§4.22）。
- **唯一值得试的开关**：`ExtensionWatchdog=false`（bool，不是 timeout）关闭 watchdog，暴露真实 exit code；用于确认进程是 exec 后崩溃还是 check-in 协议问题。
- **需要的权限/工具**：能注入/编译 extension 侧或 launchd 侧（现有 guest 工具链 `IOSVM_USE_LLD=1`、`/private/etc/com.apple.xpc.launchd.limits.plist`）；当前没有直接改 extension 二进制的权限/产物。
- **判定点**：launchd 等待的是 `CheckIn` 消息；C2 日志显示进程在 `exec` 后/进入 `xpc_main` 前出现 `EXC_CORPSE_NOTIFY error 5`，不是「动作太慢」。见 runbook「附录：extension check-in 判定点」（`9ce6e5b`）。

**影响**：

- stage 19 不再为 extension 花时间；P2/P3 的剩余工作转 P4。
- 阶段 20 若立项，先做 `ExtensionWatchdog=false` 的 30 分钟限时实验，不做无限深挖。

**何时可推翻**：

1. 找到不需要 extension 存活的替代内容路径；或
2. `ExtensionWatchdog=false` 暴露的 exit code 指向一个可在阶段 19 时间盒内修的具体 bug（需 owner 重新排期）。

---

## D5. 剩余时间全部转 P4 四格表实跑

**决定**：stage 19 剩余时间全部转 **P4 四格表实跑**（三份镜像/四格 × 20s 滚动窗口、host `0x502` 记账、`make test`、文档/推送）。

**依据/口径**：

- 三份镜像、四格定义与命令见 runbook §八：cell1 纯本地（`GL=1 CONFIGD=1`）、cell2 按引用（`GLWIRE_LOCAL=1`）、cell3 W1.5（host `GLWIRE_NO_INPLACE=1`）、cell4 W1.5+W3（同 cell3 镜像，host 不带 NO_INPLACE）。
- 每格同 `QEMU_SMP=1`、同触摸脚本、`WINDOW_AFTER=60`、`WINDOW_FRAMES=0`（约 20s）；`host 0x502` 只数宿主日志，判据 0；`make test` = `test-open` + `test-gpu`。
- `four-cell.sh` 的 `--dry-run` 只验证调度/记账，**不是实测**；四发真 QEMU 后才填四格表。驱动 `e8a8ee7`；dry-run 自检见 `9ce6e5b`。
- 实测镜像：cell1 `/tmp/root-local.apfs` f39b9105…、cell2 `root-byref.apfs` 3207a35e…、cell3=cell4 `root-w15.apfs` 785bcbac…；`GLWIRE_NO_INPLACE` 是 **host 运行 env**，cell3/cell4 同镜像是预期。
- `drive-scroll.sh` 第 39 行硬编码 `IOSVM` 会覆盖 four-cell 传入；exp 用 `/tmp/p4-harness/` 副本，code 正修 `${IOSVM:-...}`；数字来自副本/补丁要注明。
- `make test-open` PASS；`make test` 的 `test-gpu` 当前因 `/usr/share/vulkan/icd.d/lvp_icd.json` 缺失失败，是环境问题，补 lavapipe ICD 后重跑；不阻塞四格。

**影响**：

- P2/P3/D 系列不再新增实验；P4 是 stage 19 的最终交付项。
- 第二次实跑（13:25–14:27Z）已完成：四格 `host 0x502` 全 0，**P4 的 0x502 判据通过**；但 20s 窗口 fps 0.00/0.10/0.46/0.20，瓶颈是 guest 送帧（host 单帧 29.6–59.0ms），**不写性能达标**；稳定态另按 D6 的 120 帧计数窗口重跑。cell1 上一轮被误杀已复盘，本轮加了残留检查。

**何时可推翻**：

1. 三份镜像无法构建/无 GPU 资源，P4 被硬阻塞（需 owner 决定）；
2. owner 重新排期，优先 D4 的 extension 实验。

---

## D6. P4 的 fps 判据改为 120 帧计数窗口；60s 窗口 fps 不可比

**决定**：P4 的 fps 判据不再用 60s/20s 滚动窗口的 fps；改用 **`WINDOW_FRAMES=120` 帧计数窗口**；cell1（纯本地、无 recorder）用 **`WINDOW_CLOCK=guest`**。当前第二次实跑的 20s 表只用于 **`0x502` 判据**，不用于 fps 比较。

**依据**：

- 第二次实跑 20s 窗口内 host 只有 **0/2/9/4 帧**，fps 0.00/0.10/0.46/0.20；窗口仍落在 UI 重建期（`WINDOW_AFTER=60`），不是稳定态。
- host 单帧回放 **29.6–59.0ms**（16–33 fps 能力），瓶颈是 guest 送帧（cell3 帧间隔 0.77–25.87s、cell4 0.38–17.99s）；用 20s 的帧数除时间不能代表回放性能。
- `0x502` 四格全 0，P4 的 0x502 判据已通过；fps 语义另算。
- 见 runbook §8.0、`logs/s19o-p4-cells/report.md`；提交 `ea835b7`。

**影响**：

- 现有四格表只写「0x502 通过 + 测量口径已修」；**不写性能达标**。
- 下一次 P4 测量必须用 120 帧窗口；cell1 用 guest 时钟，其余可用 host 时钟；结果另表。

**何时可推翻**：

1. 120 帧窗口仍无法在合理时长内完成（guest 送帧太慢）→ 改帧计数/更长时间窗口并记录原因；
2. 有证据表明 20s 窗口已处于稳定态（需连续帧间隔/UI marker 证明）。

## D7. P4 收口口径固定：纯本地可用，by-reference / w15 不具可用性

**决定**：P4 最终结论固定为 **「纯本地可用；by-reference / w15 在 TCG 单核 + softpipe 下不具可用性（guest 送帧停摆，不是没测）」**；不写「性能达标」。

**依据**（最终数字）：

- **cell1-pure**：**120 帧 / 11.00s = 10.91 fps（guest 时钟）**，是四格中**唯一可用数字**。
- **cell2-by-reference**：跑到 1800s 只到 **183/343** 帧（≈**0.04 fps**）。
- **cell3-w15**：guest 驱动行 **2 帧 / 772,366 ms（≈0.003 fps）**；host 只到 **frame 250**。
- **cell4-w15w3**：**未跑是判决，不是漏跑**——远端路径（by-reference/w15）已证不可用，再跑 cell4 只会重复同一送帧停摆，省约 50 分钟 GPU。
- **`0x502` 四格全 0**（cell4 为早前 20s 跑，0；attempt3 未补跑）（判据通过）；这是唯一通过的 P4 判据。
- 见 runbook §8.0/§8.7、`logs/s19o-p4-cells/attempt3-framewindow/`；代码 `93f5d2a`。

**影响**：

- P4 收口：纯本地画是可用路径；by-reference/w15 在 TCG 单核 + softpipe 下不具可用性。
- **本阶段不再为 by-reference / w15 追加性能实验**，除非换配置（多核/真 GPU 直通）另立项。
- 不写「性能达标」；cell4 不补跑，除非配置变化或 owner 重新立项。

**何时可推翻**：

1. 换成多核/真 GPU 直通配置，重跑 by-reference/w15 得到不同的送帧/可用性结果；
2. cell1 的 10.91 fps 在复跑中不成立，或 guest 时钟口径被证明有误。

## D8. Stage 20 E1 = C（不可判/不可行）；P2/P3 不重开

**决定**：Stage 20 的 E1（`ExtensionWatchdog=false`）最终判 **C（不可判/不可行）**；control 已完成，P2/P3 不重开，fallback5 仍是 P3 交付，system 恢复 `6be494f1`。

**依据**：

- E1（system `15167c8a…`，false/false）：700s `ui=0` / `biggest=24,960` / guestframes 2 / hostframes 2；`checked in as alive` 0、hanging 0、CHECK_IN_TIMEOUT 0；`EXC_CORPSE_NOTIFY` 4 条全是 configd pid22；extension 服务 0、`WILL_SPAWN` 68 全是 system jobs → **extension 根本没 spawn**。
- Control（system `6be494f1…`，C1 三键）：900s 到 `ui=720s`、`ui_bytes=309280`、guestframes 32/hostframes 304；hanging **10**、CHECK_IN_TIMEOUT **10**、alive **0**、exit `0x3`；首个 PhotosReliveWidget[45] 666s → **重建不是瓶颈**。
- 单变量 `a3a895b0`（Watchdog=true/Crash=false，只差 Watchdog）：650s 首 hanging `PeopleViewService.LockScreenPeopleWidget[44]`，共 6 条 → **`ExtensionWatchdog=false` 是坏键**；Crash false/重建都不是。
- 结论：watchdog=false 不让 extension 活到 CheckIn，反而停 boot；control 仍 0 alive + 10 timeout/0x3 → **check-in timeout 未解决，不是单个 limits 布尔能修**。
- system 已恢复 `6be494f1`；坏 `15167c8a` 备份 `/tmp/system.apfs.bak-s20-bad`。

**影响**：

- P2/P3 保持关闭、不重开；P3 fallback5 仍是交付项，不回退。
- 不再倒腾 `ExtensionWatchdog` 布尔、不改 job、不盲调 `createImageSlot:`；不为 byref/w15 追加性能实验；P1 不续（除非满足 90 高定向 trim 判据）。
- 干净单变量（C1 base + 只改 Watchdog=false、Crash=true）900–1200s 未做，当前不做；若 owner 另定再开。

**何时可推翻**：

1. check-in 协议/launchd 侧有可复现修复，且 extension 活过 60s/checked in；
2. 干净单变量测试出现不同结果（需 owner 重新立项）。

## D9. P0 历史 3/3 通过，但当前不可稳定复现

**决定**：P0 **历史 3/3 通过**（v7 99.85%、v9 99.98%、p0-3 98.81%，system `8bf5f838`），但**当前不可稳定复现 ≥95%**；交付基线降级为「**历史证据 + 复现性未达标**」。不写「P0 已稳定/当前可复现」，也不写「P0 3/3 被推翻」。

**依据**：

- 历史：8bf5 上 v7/v9/p0-3 三发通过；p0-3 98.82%。
- 8bf5 同配方（root 3207a35e + host b1e87d + DUMP_AT=800 PAIR/QEMU_SMP=1/glwire_local=1）：12:52 **99.99%**，15:42 **89.56% <95**（missing 9.60%，bbox 46,142–729,1603；f-picture `20640c60` 非黑，check_panel PASS，截图 316,377B）。
- 6be 多次 ~89.50%。→ **8bf5 是必要条件但不是充分条件**；99.99 是单次好跑，不能当基线。
- 证据：`logs/s20-p0-repro/`、`logs/s20-p0-8bf5-recheck/`、`logs/s20-p0-recheck/`、`isolateA/B`、`hostfix`；commits `fba31f8`/`767053f`/`fff2678`/`fda19b3`/`6935ed3`。
- 原件：`/tmp/system.apfs.bak-s20`；keep `/tmp/system-8bf5f838.keep`；6be keep `/tmp/system-6be494f1.keep`；不许覆盖。

**影响**：

- P0 不能作为**当前可复现**交付基线；只能作为历史证据。门禁至少 **2–3 次 boot 统计**，单次 99.99 不算。
- **P0 判据待重定义**：由「单次同帧 diff ≥95%」改为「最佳相邻配对 ≥95%」（若 exp 矩阵证明有这种配对），否则降级为内容判据（≥5 可读名称 + 截图 ≥300KB + panel/picture MATCH + 无 panic）。口径未定前只写「历史证据，复现不稳」。
- 任何实验 report 必须写 **system md5 + lib md5**，否则不可比。
- 若要把 6be 或别的 system 定为基线，必须**重新定义 P0 判据**（按该 system 的 ~89.5% 重设阈值，或改成不依赖同帧配对）——**待定**。

**何时可推翻**：

1. 8bf5 同配方连续 2–3 发都 ≥95%，且 P0 三判据稳定；
2. owner 决定重定义 P0 判据（按 6be/其他 system 的阈值或非配对判据）。

## 阶段 20 移交接手清单（D4）

- **最终证据编号**：**E5**（fallback5 PASS，P3 fallback 已交付）；E1–E4 见 D1（真路径不可行）。
- **输入证据**：`logs/s19o-p2e/`（C1 limits）、`logs/s19o-p2e-c2/`（force xpcproxy）、`logs/s19o-p3b-d3/`（CAContext 枚举）、`logs/s19o-p3-fallback3/`（host offscreen）、`logs/s19o-p3-fallback5/`（E5：P3 fallback 已交付）、`logs/s19o-p3-surface-enum/`；runbook §4.23–§4.25、§4.28、§4.30、附录 extension check-in；本文件 D4。
- **第一个实验（30 分钟限时）**：`ExtensionWatchdog=false`（bool，不是 timeout），看是否能暴露 extension 的真实 exit code / `CheckIn` 失败点；不要先加 timeout、不要先改 job。计划骨架：`docs/gpu-acceleration-stage20-plan.md`。
- **需要的权限/工具**：能改/注入 extension 侧或 launchd 侧；当前只有 guest 工具链（`IOSVM_USE_LLD=1`）和 launchd limits plist 路径，**没有直接改 extension 二进制的产物**。
- **判定点**：launchd 等的是 `CheckIn` 消息；C2 已见 `exec` 后 ~0.5s `EXC_CORPSE_NOTIFY error 5`，不是「动作慢」。
- **退出标准**：extension 活过 60s 且 `checked in as alive` 出现 → 可回到 P2/P3；暴露明确 exit code/bug → 评估 30 分钟内能否修；两者都没有 → 写「不可行/需更多信息」，不再投入。
- **不做**：不在 stage 19 内继续加 `ExtensionWatchdogTimeout`、不盲调 `createImageSlot:`、不重复 P2d job 对照。

## 追溯表

| 决定 | 主要证据 | runbook 章节 | 关键提交 |
|---|---|---|---|
| D1 | E1 surface 枚举；E2 allContexts；E3 无零参 snapshot；E4 CheckIn 未发出 | §4.28、§4.30、附录 extension check-in | `44f602a`、`2acbd0d`、`64c8896`、`f339014`、`9ce6e5b` |
| D2 | host fallback2/3 offscreen；E5 fallback5 PASS（guest-side 可见） | §4.29、§4.31 | `e8a8ee7`、`2de8e65`、`c3b0371`、`logs/s19o-p3-fallback5` |
| D3 | P2d/P2e C1/C2 四路径 | §4.22–§4.25 | `fdf964a`、`f1261b4`、`eac8128`、`b07d2da` |
| D4 | 18 extension 被 5–13s 杀；timeout/xpcproxy/job 负结果 | §4.24、§4.25、附录 extension check-in | `9ce6e5b`、`f1261b4`、`eac8128` |
| D5 | 四格口径/脚本/dry-run | §八 | `e8a8ee7`、`9ce6e5b`、`b07d2da` |
| D6 | 20s fps 不可比；改 120 帧窗口、cell1 guest 时钟 | §8.0 | `ea835b7` |
| D7 | P4 终局：纯本地可用；byref/w15 不具可用性；cell4 未跑是判决 | §8.0、§8.7 | `93f5d2a`、`logs/s19o-p4-cells/attempt3-framewindow` |
| D8 | Stage 20 E1=C；control 完成；P2/P3 不重开；system 6be494f1 | §8、`stage20-e1-control.md` | `cac2a73`、`6be494f1` |
| D9 | P0 历史 3/3；当前不稳定（8bf 99.99/89.56）；6be 89.50 | §4.33、closure、handoff | `fba31f8`、`767053f`、`fff2678`、`fda19b3`、`6935ed3`、`logs/s20-p0-*` |

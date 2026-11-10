# Stage 19 + 20 最终移交

> **状态：最终移交，2026-11-10 修订。** Stage 19 的 P0 **历史 3/3 通过**，但**当前不可稳定复现**：8bf5 同配方 12:52 **99.99%**、15:42 **89.56%**；6be 约 89.50%。交付基线 = 「历史证据 + 复现性未达标」。Stage 20 E1 收口 C（不可判/不可行）。P2/P3 保持关闭，P3 以 fallback5 占位交付；P1 判 A、修复中。
> 本页只做结论、证据索引、未解决项与「不做」清单；细节以 runbook/decisions/logs 为准。
> 不写 P2/P3 真路径通过，不写性能达标，不写 watchdog 已修。

## 0. P0 参考 system 与复现性（必读）

| 用途 | system md5 | 结果 | 位置/规则 |
|---|---|---|---|
| **历史 P0 system** | **`8bf5f838d52e46d6c8bf77f13a5ad946`** | **99.99%**（12:52 单次）与 **89.56%**（15:42） | 原件 `/tmp/system.apfs.bak-s20`；keep `/tmp/system-8bf5f838.keep`；**不许覆盖** |
| **对照** | **`6be494f1…`**（C1） | **89.50%** | keep `/tmp/system-6be494f1.keep` |

- **P0 历史 3/3 通过，但当前不可稳定复现**：8bf5 同配方两次 99.99/89.56；6be ~89.50。**8bf5 是必要条件但不是充分条件**；单次 99.99 不能当基线。
- 门禁至少要 **2–3 次 boot 统计**；任何实验 report 必须写 **system md5 + lib md5**，否则不可比。
- **P0 判据待重定义**：由「单次同帧 diff ≥95%」改为「**最佳相邻配对 ≥95%**（若 exp 配对矩阵证明确有这种配对），否则降级为**内容判据**（≥5 可读名称 + 截图 ≥300KB + panel/picture MATCH + 无 panic）。口径未定前，P0 一律写「历史证据，复现不稳」。
- 不写「P0 已稳定/当前可复现」；也不写「P0 3/3 被推翻」——是「历史通过、当前不稳定」。
- 若要用 6be 或别的 system 当基线，必须**重新定义 P0 判据**（按该 system 的 ~89.5% 重设阈值，或改成不依赖同帧配对）——**待定**。

## 1. 两个阶段的终局结论

### Stage 19

| 项 | 终局 | 一句话依据 |
|---|---|---|
| **P0** | **历史 3/3；当前不可稳定复现；判据待重定义** | 历史：v7 99.85%、v9 99.98%、p0-3 98.81%（8bf5）。当前 8bf5 同配方：12:52 **99.99%**、15:42 **89.56%**（missing 9.60%，bbox 46,142–729,1603）；6be ~89.50%。→ 单配对指标不稳；**P0 判据待改为最佳相邻配对或内容判据**，口径未定前只写「历史证据，复现不稳」。 |
| **P1** | **重开后判 A、修复中（未修复）** | 虚线段在 90 高 label tile 底部行：Photos row61 54px/alpha247、FaceTime 61–62 47px、Wallet 58–61、label-4 53–56；B 采样放大已排除。第一次 `glyph_last_row=1` FAIL（Weather 掉笔画、虚线仍在）；`label_dash_rows=53-62` crash；`61-62` 无效果。修法只走 label tile row61/62，验收 guest 截图；P1 未修复。 |
| **P2** | **关闭/未达成** | lookup 已修、PosterBoard running，但壁纸 window/PBUI 远程 VC 链在屏而 contents=0；根因与 P3 同源：extension check-in timeout；四路径走完，不再投入 |
| **P3** | **真路径不可行 + fallback5 已交付** | surface 枚举 30 面无 widget 候选；allContexts 按进程；无零参 snapshot；extension exec 后 ~0.5s 死亡；guest-side 占位（24px 棋盘+白框+双黄对角线）PASS，明确非真 widget |
| **P4** | **`0x502` 通过；仅 cell1 可用** | 四格/三格历史 `0x502` 全 0；cell1 120 帧/11.00s=10.91 fps（guest）唯一可用；cell2/3 远程路径不可用；cell4 NOT RUN 是判决；不写性能达标 |

### Stage 20（E1）

| 项 | 终局 | 一句话依据 |
|---|---|---|
| **E1** | **C：不可判/不可行** | `ExtensionWatchdog=false` 的 system `15167c8a` boot 700s 仍 2 帧/无 UI/extension 未 spawn；control（C1 limits，system `6be494f1`）到 UI 且 extension 阶段正常，但仍 `checked in as alive` 0、`CHECK_IN_TIMEOUT` 10、exit `0x3` → check-in timeout 未解决，不是单 limits 布尔能修 |
| **P2/P3** | **不重开** | E1 未提供可用 extension/CheckIn；P3 仍 fallback5 占位交付；P2 仍关闭 |

- 单变量旁证：`a3a895b0`（Watchdog=true、CrashOnSIGTERMTimeout=false）与 E1 只差 Watchdog，650s 已出现第一条 hanging；E1（false/false）700s 无 UI/无 extension → **`ExtensionWatchdog=false` 是坏键**，会让 boot/extension 停住。
- **环境已恢复**：当前 `system.apfs = 6be494f1…`（C1 等价、可复现）；坏 `15167c8a` 备份 `/tmp/system.apfs.bak-s20-bad`，不要复用。

## 2. 证据索引

| 结论 | 文档 | 日志/产物 | 关键 commit |
|---|---|---|---|
| P0 历史 3/3（当前不稳定） | runbook §4.33、`docs/gpu-acceleration-stage19-p0-3-results.md`、closure §1、decisions D9 | `logs/s19o-p0-3/`、`logs/s20-p0-repro/`、`logs/s20-p0-8bf5-recheck/` | `498157c`、`66a7d54`、`024c10e`、`fda19b3`、`6935ed3` |
| P0 当前复现性 | runbook §4.33、decisions D9、closure §1 | `logs/s20-p0-8bf5-recheck/`、`logs/s20-p0-recheck/`、`logs/s20-p0-isolateA/`、`logs/s20-p0-isolateB/`、`logs/s20-p0-hostfix/` | `fba31f8`、`767053f`、`fff2678`、`6935ed3` |
| P1 重开 A/修复中 | runbook §4.32、`docs/gpu-acceleration-stage19-p1-ab-minimal.md`、closure §1 | `logs/s20-p1-54px-dash/`、`logs/s20-p1-validation/`、`logs/s20-p1-labeldash/` | `841bd93`、`a75efd1`、`c139a19` |
| P2 未达成 | runbook §4.15/§4.17/§4.24/§4.25、decisions D3、closure §1 | `logs/s19o-p2/`、`logs/s19o-p2d/`、`logs/s19o-p2e/`、`logs/s19o-p2e-c2/` | `d73c1e8`、`f57eeb8`、`eac8128` |
| P3 真路径不可行 | runbook §4.26–§4.30、decisions D1、closure §1 | `logs/s19o-p3a/`、`logs/s19o-p3b-d1/`、`logs/s19o-p3b-d2/`、`logs/s19o-p3b-d3/`、`logs/s19o-p3-surface-enum/` | `c2e9d23`、`97506d9`、`f339014`、`2acbd0d` |
| P3 fallback5 PASS | runbook §4.31、decisions D2/E5、closure §1 | `logs/s19o-p3-fallback5/`（`widget-area.png`、report） | `453e9ae`、`66a7d54`、`024c10e` |
| P4 收口 | runbook §八/§8.7、`docs/gpu-acceleration-stage19-p4-framewindow-results.md`、`docs/gpu-acceleration-stage19-p4-w15-partial.md`、decisions D7 | `logs/s19o-p4-cells/`（attempt3-framewindow、attempt1-1256、harness） | `93f5d2a`、`f30c8d2`、`c863842` |
| Stage 20 E1 C | `docs/gpu-acceleration-stage20-plan.md` §8、`docs/gpu-acceleration-stage20-e1-watchdog.md`、`docs/gpu-acceleration-stage20-e1-rebuild-confound.md`、`docs/gpu-acceleration-stage20-e1-control.md` | `logs/s20-e1-watchdog/`、`logs/s20-e1-control/`、`/tmp/system.apfs.bak-s20-bad` | `c601765`、`daaf627`、`cac2a73`、`f0e54aa` |
| 编译环境 | runbook 附录 C | `/host/iosvm-work/toolchain/` | `cefbe6f` |

补充：Stage 19 一页收口 `docs/gpu-acceleration-stage19-closure.md`；决策 `docs/gpu-acceleration-stage19-decisions.md`（D1–D9）；计划 `docs/gpu-acceleration-stage19-plan.md`、`docs/gpu-acceleration-stage20-plan.md`。
交付自检（镜像/测试/归档/分支）：`docs/gpu-acceleration-stage19-20-delivery-selfcheck.md`。

## 3. 未解决项与建议

### 3.1 extension check-in（平台能力；需更多信息）

这是 P2/P3 的共同硬阻塞：18 个 extension 在 spawn 后 5–13s 被 watchdog 杀；C2 见 `EXC_CORPSE_NOTIFY error 5`（exec 后 ~0.5s），`CheckIn` 从未发出；E1 control 仍 0 alive + 10 次 `CHECK_IN_TIMEOUT`/exit `0x3`。

**若有人继续查，从这里开始**：
1. 用 **system `6be494f1`**（C1 等价）作 base，不要用 `15167c8a`；root 用 `3207a35e`（v9 类），boot `glwire_local=1`、NO_PANEL、900–1200s。
2. 只改 **`ExtensionWatchdog=false`** 作为干净单变量（`CrashOnSIGTERMTimeout` 保持 true、Timeout 120 不动），不要改 job、不要调 Timeout；预计仍 C，但可确认 false 在这个 base 上是否同样停 boot。
3. 沿 `xpcproxy`/`xpc_main`/CheckIn 协议查：为什么进程 exec 后 0.5s `EXC_CORPSE_NOTIFY`；是否有 dyld/签名/entitlement/persona 的早期失败；`launchd_log_verbose/serial` 是否给更早的 trace。
4. 退出标准：extension 活过 60s + `checked in as alive` → 回 P2/P3 重开判据；明确 exit code/backtrace → 评估单点修复；都没有 → 写「不可行/需更多信息」，不再投入。
5. 权限/工具：guest 工具链 `IOSVM_USE_LLD=1`（clang 19.1.7 + THEOS SDK）；launchd limits 路径 `/private/etc/com.apple.xpc.launchd.limits.plist`；当前没有改 extension 二进制的产物。

### 3.2 P1 文字质量（未修，修复中）

P1 重开后判 **A（tile 内容/行映射）**：虚线段写在 90 高 label tile 底部行（Photos row61 54px/alpha247；FaceTime 61–62 47px；Wallet 58–61；label-4 53–56）；B（采样放大）已排除。修在 **guest label tile 生成/行映射**，不是 host 合成。

**已失败**：`glyph_last_row=1` 清到 42 高 glyph 笔画底部（Weather 掉笔画、虚线仍在）；`label_dash_rows=53-62` crash；`61-62` 无可见效果。

**下一步**：只改 label tile 虚线行（row61/62，实际还出现 61–63，按 per-tile offset），验收 guest 截图；**P1 未修复**，不写已修。不要再用全局 `GLYPH_TRIM`/`TC0_SANITIZE`/skip13 盲试。

### 3.3 by-reference / w15 性能

cell2/3 在 TCG 单核 + softpipe 下 guest 送帧停摆（cell2 ≈0.044 fps、cell3 2 帧/772s），不是 host 回放能力问题（host 单帧 29.6–59.0 ms，16–33 fps 能力）。**要谈性能必须换配置**：多核 guest、GPU 直通/更快的 GL 路径、或减少 wire 数据量；当前配置下不再追加性能实验。

### 3.4 ack 实验状态 = 未判定（tooling 坏）

- `s20-ack785` / `s20-ack785b` 的 **UI 没起**，是 lib **`4a7cd6` 的 boot 回归**，**不是 ack 假设被否**；不能写「ack 不是瓶颈」。
- 状态：**未判定（tooling 坏）**；需在 lib 修复/回退后重跑，先 isolate 再 ack 版。
- **lib 时间线**：`4a7cd6` 会让 UI 起不来；已回退/待修。所有实验 report 必须写 **lib md5 + system md5**。
- 来源：`docs/gpu-acceleration-stage20-ack785.md`、`docs/gpu-acceleration-stage20-ack785b.md`、`docs/gpu-acceleration-stage20-iso785.md`、`logs/s20-ack785b/`。

## 4. 「不做」清单（避免下一阶段重复烧时间）

1. P1 只查 label tile row61/62；不再 scrub glyph last row、不再查 host sampler/linear/clamp、不再试全局 `GLYPH_TRIM`/`TC0_SANITIZE`/skip13。
2. 不再调 `ExtensionWatchdogTimeout`、不再改 job（profiled/powerlog/Video/ImageConversion 已证无关）。
3. 不再用 `+allContexts` 猜 widget host（按进程隔离；backboardd/SpringBoard 都看不到）。
4. 不再猜 CAContext `createImageSlot:hasAlpha:` 参数（它是创建 slot，不是快照；这代无零参 snapshot）。
5. 不再点 surface 3 当 widget（184×124 旧 snapshot/空白块；surface 枚举 30 面无 155×155 候选）。
6. 不再用 host 侧 `GLWIRE_WIDGET_FALLBACK` 做视觉交付（host 画在 offscreen picture，QEMU 截图为 guest scanout；fallback2/3 已证）。
7. 不再为 by-ref/w15 追加性能实验（除非换多核/真 GPU 直通）。
8. 不再用坏 system `15167c8a`；不要覆盖当前 `6be494f1` 前不备份。
9. 不把 P0 3/3 当 P2/P3 通过；不把 fallback5 占位当真 widget；不写性能达标。

## 5. 能否作为交付基线

**P0 不能作为当前可复现交付基线**：历史 3/3 通过；当前 8bf5 同配方 99.99%（12:52）/89.56%（15:42）不一致，6be ~89.50%；**单配对指标不稳，判据待重定义**。8bf5 原件/keep 保留，但 8bf5 只是必要条件；门禁需 2–3 次 boot 统计，report 必须写 system md5 + lib md5。

**可作为历史交付证据**：P3 有明确标注的非真 widget 占位；P4 有 `0x502=0` 与唯一可用 cell1 fps。

**不能作为「完整桌面/壁纸/真 widget/性能」的交付基线**：P2 未达成、P3 真路径不可行、P1 有可见点带、P4 仅 cell1 可用；这些边界已在 closure/decisions 与本页写清。

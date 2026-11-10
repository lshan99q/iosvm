# Stage 19 + 20 最终移交

> **状态：最终移交，2026-11-10。** Stage 19 收口于 P0 3/3；Stage 20 的 E1 收口于 C（不可判/不可行）。P2/P3 保持关闭，P3 以 fallback5 占位交付。
> 本页只做结论、证据索引、未解决项与「不做」清单；细节以 runbook/decisions/logs 为准。
> 不写 P2/P3 真路径通过，不写性能达标，不写 watchdog 已修。

## 1. 两个阶段的终局结论

### Stage 19

| 项 | 终局 | 一句话依据 |
|---|---|---|
| **P0** | **3/3 PASS** | v7 330,931 B/99.85% SAME；v9 326,461 B/99.98%；p0-3 327,177 B/≥14 可读/98.81%；三发均 panel 真实、`0x502=0` |
| **P1** | **暂停/未修复（终局 C）** | 点带在 **42 高 glyph/label tile 底部行**；178×90 tile row61=54 px 虚线、rows62–89=0，row61 是 130×42 glyph 纹理最后一行；不是 750×200 平铺、不是背板采样；按判据不续 |
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
| P0 3/3 | runbook §4.33、`docs/gpu-acceleration-stage19-p0-3-results.md`、closure §1 | `logs/s19o-p0-3/`（含 `p0-3-weather-band2.png`） | `498157c`、`66a7d54`、`024c10e` |
| P1 终局 C | runbook §4.32、`docs/gpu-acceleration-stage19-p1-backdrop-or-tile.md`、closure §1 | `logs/s19o-p1c/`、`logs/s19o-p1d/`、`logs/s19o-p1e/`、`logs/s19o-p0-3/` | `841bd93`、`a75efd1`、`c139a19` |
| P2 未达成 | runbook §4.15/§4.17/§4.24/§4.25、decisions D3、closure §1 | `logs/s19o-p2/`、`logs/s19o-p2d/`、`logs/s19o-p2e/`、`logs/s19o-p2e-c2/` | `d73c1e8`、`f57eeb8`、`eac8128` |
| P3 真路径不可行 | runbook §4.26–§4.30、decisions D1、closure §1 | `logs/s19o-p3a/`、`logs/s19o-p3b-d1/`、`logs/s19o-p3b-d2/`、`logs/s19o-p3b-d3/`、`logs/s19o-p3-surface-enum/` | `c2e9d23`、`97506d9`、`f339014`、`2acbd0d` |
| P3 fallback5 PASS | runbook §4.31、decisions D2/E5、closure §1 | `logs/s19o-p3-fallback5/`（`widget-area.png`、report） | `453e9ae`、`66a7d54`、`024c10e` |
| P4 收口 | runbook §八/§8.7、`docs/gpu-acceleration-stage19-p4-framewindow-results.md`、`docs/gpu-acceleration-stage19-p4-w15-partial.md`、decisions D7 | `logs/s19o-p4-cells/`（attempt3-framewindow、attempt1-1256、harness） | `93f5d2a`、`f30c8d2`、`c863842` |
| Stage 20 E1 C | `docs/gpu-acceleration-stage20-plan.md` §8、`docs/gpu-acceleration-stage20-e1-watchdog.md`、`docs/gpu-acceleration-stage20-e1-rebuild-confound.md`、`docs/gpu-acceleration-stage20-e1-control.md` | `logs/s20-e1-watchdog/`、`logs/s20-e1-control/`、`/tmp/system.apfs.bak-s20-bad` | `c601765`、`daaf627`、`cac2a73`、`f0e54aa` |
| 编译环境 | runbook 附录 C | `/host/iosvm-work/toolchain/` | `cefbe6f` |

补充：Stage 19 一页收口 `docs/gpu-acceleration-stage19-closure.md`；决策 `docs/gpu-acceleration-stage19-decisions.md`（D1–D7）；计划 `docs/gpu-acceleration-stage19-plan.md`、`docs/gpu-acceleration-stage20-plan.md`。
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

### 3.2 P1 文字质量（未修，续查判据明确）

点带是 42 高 glyph/label tile 底部 alpha（178×90 tile row61=54 px；P1c 101×42 Notes 底部 8 行同源）。修在 **guest glyph tile 生成/裁剪**，不是 host 合成。

**唯一续查判据**：对 90 高 tile 定向 trim 后，至少两个 90 高标签带下降 ≥50%，且 42 高 Notes 不变 → 才继续查 90 高生成；否则保持暂停（当前已判不续）。不要再用全局 `GLYPH_TRIM`/`TC0_SANITIZE`/skip13 盲试（均为重分布或无效）。

### 3.3 by-reference / w15 性能

cell2/3 在 TCG 单核 + softpipe 下 guest 送帧停摆（cell2 ≈0.044 fps、cell3 2 帧/772s），不是 host 回放能力问题（host 单帧 29.6–59.0 ms，16–33 fps 能力）。**要谈性能必须换配置**：多核 guest、GPU 直通/更快的 GL 路径、或减少 wire 数据量；当前配置下不再追加性能实验。

## 4. 「不做」清单（避免下一阶段重复烧时间）

1. 不再为 P1 起 boot；不再试 `GLYPH_TRIM` 全局/`TC0_SANITIZE`/skip13 作为修复。
2. 不再调 `ExtensionWatchdogTimeout`、不再改 job（profiled/powerlog/Video/ImageConversion 已证无关）。
3. 不再用 `+allContexts` 猜 widget host（按进程隔离；backboardd/SpringBoard 都看不到）。
4. 不再猜 CAContext `createImageSlot:hasAlpha:` 参数（它是创建 slot，不是快照；这代无零参 snapshot）。
5. 不再点 surface 3 当 widget（184×124 旧 snapshot/空白块；surface 枚举 30 面无 155×155 候选）。
6. 不再用 host 侧 `GLWIRE_WIDGET_FALLBACK` 做视觉交付（host 画在 offscreen picture，QEMU 截图为 guest scanout；fallback2/3 已证）。
7. 不再为 by-ref/w15 追加性能实验（除非换多核/真 GPU 直通）。
8. 不再用坏 system `15167c8a`；不要覆盖当前 `6be494f1` 前不备份。
9. 不把 P0 3/3 当 P2/P3 通过；不把 fallback5 占位当真 widget；不写性能达标。

## 5. 能否作为交付基线

**可以作为「Stage 19 P0 + P3 fallback + P4 0x502」的交付基线**：P0 3/3 可复现（同一配方三发 PASS）；P3 有明确标注的非真 widget 占位；P4 有 `0x502=0` 与唯一可用 cell1 fps。

**不能作为「完整桌面/壁纸/真 widget/性能」的交付基线**：P2 未达成、P3 真路径不可行、P1 有可见点带、P4 仅 cell1 可用；这些边界已在 closure/decisions 与本页写清。

# Stage 19 收口包（最终状态 / 未达成 / 移交 / 复现）

> **状态：最终，2026-11-09。** P0 3/3 PASS；P1 暂停/未修复（终局 C）；P2 未达成；P3 真路径不可行 + fallback5 已交付；P4 0x502 通过。
> 本页只做索引和结论，不重复实验细节；所有数字以 runbook/decisions/logs 为准。
> 不写 P1/P2/P3 真路径通过；不写性能达标。
> **下一阶段计划**：`docs/gpu-acceleration-stage20-plan.md`（E1 `ExtensionWatchdog=false`，30 分钟限时；**E1 终局 C（不可判/不可行），control 完成；P2/P3 未重开；fallback5 仍是 P3 交付；system 恢复 6be494f1**）。

## 1. 一页状态

| 项 | 最终状态 | 判据/证据 |
|---|---|---|
| **P0** | **3/3 PASS** | v7 330,931 B/10+2/99.85%；v9 326,461 B/10+2/99.98%；**p0-3 327,177 B/≥14/98.81%**（`end=ui` 660s, `ui_bytes=310605`, hostframes 1080, panic 0）。不是 v8 89.53%；extra 14,555 px（1.19%）只是小 UI；overlay/右缘首帧竞态不影响三判据。见 §4.33、`docs/gpu-acceleration-stage19-p0-3-results.md`。 |
| **P1** | **暂停/未修复（终局 C）** | 点带是 **42 高 glyph/label tile 底部行**：`#549 prog13 178x90` / `#550 prog15 130x42`；178x90 tile row61 = **54 px 虚线**，rows62–89=0；row61 是 130x42 glyph 纹理最后一行（同 P1c 101x42 Notes 底部 8 行）。早帧 750x200 readrect 184x90/texmat1,1 无 wrap/repeat。**不是 A 平铺、不是 B 背板采样**；修在 guest glyph tile 生成。P1 未修复、不续。见 §4.32、`docs/gpu-acceleration-stage19-p0-3-results.md`。 |
| **P2** | **关闭，未达成** | P2d 四 job、P2e C1 limits、C2 xpcproxy、根因审计四路径走完；根因与 P3 同源 **extension check-in timeout**。不再投入（D3）。 |
| **P3** | **真路径不可行 + fallback5 已交付** | 真路径 D1 不可行；guest-side placeholder（拉面）fallback5 PASS，yellow 13,733/white 11,174/black 1,825；**是占位，不是真 widget**。见 §4.29/§4.31、D1/D2。 |
| **P4** | **0x502 通过；仅 cell1 可用** | `0x502` 四格历史全 0；**cell1-pure 120 帧/11.00s=10.91 fps（guest）唯一可用**；cell2 183/343≈0.04 fps、cell3 2 帧/772,366ms≈0.003 fps，均不可用；**cell4 NOT RUN 是判决，不是漏跑**。见 §8.7、D7。 |

## 2. 未达成项 / 边界

1. **P0 已 3/3 PASS**：第三发 p0-3 补齐；v8 仍不计入。
2. **P1 未修复、暂停/不续**：终局 **C/other（glyph/label tile 底部 alpha）**；42 高 tile 直接贡献，90 高 tile 可能是第二来源，但不是 750x200 平铺/背板采样。只有「90 高定向 trim 后至少两个 90 高标签带下降 ≥50% 且 42 高 Notes 不变」才续查。当前不续，不再为 P1 起 boot。
3. **P2 未达成**：壁纸没出；不是 lookup/geometry 问题，卡在 extension check-in。
4. **P3 真路径不可行**：surface/CAContext 实时 widget 内容不可得；交付的是**占位**，不是真 widget。
5. **P4 不是性能达标**：只是 `0x502` 判据通过；by-reference/w15 在 TCG 单核 + softpipe 下不具可用性；cell4 未跑。

## 3. 阶段 20 移交（D4）

**问题陈述**：18 个 extension 在 spawn 后 5–13s 被 `Extension is hanging on launch. Killing.` 杀；C2 日志见 `EXC_CORPSE_NOTIFY error 5`（exec 后 ~0.5s），`CheckIn` 从未发出；`checked in as alive` 0 次。不是「动作太慢」，是进程没活到 check-in。

**证据编号**：

- **E1** surface 枚举 30 个面无 widget 尺寸候选（`logs/s19o-p3-surface-enum/`、runbook §4.30）。
- **E2** `+allContexts` 按进程隔离（D2/D3；backboardd 1、SpringBoard 16 都不含 155x155 host；§4.28）。
- **E3** 这代 CAContext 无零参 `createImage`/`snapshot`，只有 `createImageSlot:hasAlpha:`（§4.28 D3）。
- **E4** extension exec 后 ~0.5s `EXC_CORPSE_NOTIFY error 5`、`CheckIn` 未发出（runbook 附录 extension check-in、§4.24/§4.25）。
- **E5** fallback5 PASS（P3 占位已交付；`logs/s19o-p3-fallback5/`、§4.31）。

**已试负结果**：`ExtensionWatchdogTimeout=120` 无效（仍 ~9s 被杀）；force xpcproxy（`launchd_proxyless_spawn=0`）不是 elision，仍 7–11s 死；profiled/powerlog/MediaConversion job 无关（只让 profiled 起来，壁纸层仍 0）。

**唯一值得试**：`ExtensionWatchdog=false`（bool，不是 timeout）关闭 watchdog，暴露真实 exit code；**30 分钟限时**。
**需要的权限/工具**：能注入/编译 extension 或 launchd 侧；现有 guest 工具链 `IOSVM_USE_LLD=1`、launchd limits plist 路径；当前没有改 extension 二进制的产物。
**输入日志**：`logs/s19o-p2e/`、`logs/s19o-p2e-c2/`、`logs/s19o-p3b-d3/`、`logs/s19o-p3-surface-enum/`、`logs/s19o-p3-fallback5/`、`logs/s19o-p4-cells/`。
**退出标准**：extension 活过 60s 且 `checked in as alive` → 回 P2/P3；暴露明确 exit code/bug → 评估 30 分钟内能否修；都没有 → 写「不可行/需更多信息」，不再投入。

## 4. 复现入口

- 决策：`docs/gpu-acceleration-stage19-decisions.md`（D1–D8，含每条的依据/影响/何时可推翻）。
- 实验手册：`docs/gpu-acceleration-stage19-p0-runbook.md`（§4.29 P3 收口、§4.31 fallback5、§八 P4、附录 extension check-in）。
- 计划：`docs/gpu-acceleration-stage19-plan.md`（P0–P4 状态、不做清单、D7 终局）。
- 路线评估：`docs/gpu-acceleration-gpu-passthrough-assessment.md`（为什么不做 GPU 直通；glwire 现状；三条路线与代价）。
- P4 归档：`logs/s19o-p4-cells/`（`four-cell.md`、`report.md`、`attempt3-framewindow/`、`attempt1-1256/`）。
- P3 归档：`logs/s19o-p3-fallback5/`、`logs/s19o-p3-surface-enum/`、`logs/s19o-p3b-d3/`。
- `make test-open` PASS；`make test` 的 `test-gpu` 因缺 `/usr/share/vulkan/icd.d/lvp_icd.json` 失败（环境问题，不是代码）。

## 5. 回填状态

**全部已回填。** P0 3/3、P1 C 终局、E1 C/control 均已入档；后续只按新实验更新。

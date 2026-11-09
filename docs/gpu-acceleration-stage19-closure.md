# Stage 19 收口包（最终状态 / 未达成 / 移交 / 复现）

> **状态：草稿，2026-11-09。** P0 第三发未出；P1 verdict 已出（否/不续）。其余项按已归档证据收口。
> 本页只做索引和结论，不重复实验细节；所有数字以 runbook/decisions/logs 为准。
> 不写 P1/P2/P3 真路径通过；不写性能达标。

## 1. 一页状态

| 项 | 最终状态 | 判据/证据 |
|---|---|---|
| **P0** | **2/3（等第三发；PASS→3/3，FAIL→2/3）** | v7 99.85%、v9 99.98% 同帧 SAME FRAME；v8 89.53% NOT，因 dump 落在 UI 加载动画，不计入。见 runbook §4.8/§4.9。 |
| **P1** | **暂停：否/不续** | **90 高背板不是直接/全局原因**；42 高 tile（101x42 Notes，底部 8 行 29–30 px）已直接贡献；90 高 tile 下缘点线也是 tile 自身 alpha，不是 750x200 平铺；750x200 只是 prog13 unit-1 暗化采样。见 §4.32、`docs/gpu-acceleration-stage19-p1-backdrop-or-tile.md`。P1 未修复，不续。 |
| **P2** | **关闭，未达成** | P2d 四 job、P2e C1 limits、C2 xpcproxy、根因审计四路径走完；根因与 P3 同源 **extension check-in timeout**。不再投入（D3）。 |
| **P3** | **真路径不可行 + fallback5 已交付** | 真路径 D1 不可行；guest-side placeholder（拉面）fallback5 PASS，yellow 13,733/white 11,174/black 1,825；**是占位，不是真 widget**。见 §4.29/§4.31、D1/D2。 |
| **P4** | **0x502 通过；仅 cell1 可用** | `0x502` 四格历史全 0；**cell1-pure 120 帧/11.00s=10.91 fps（guest）唯一可用**；cell2 183/343≈0.04 fps、cell3 2 帧/772,366ms≈0.003 fps，均不可用；**cell4 NOT RUN 是判决，不是漏跑**。见 §8.7、D7。 |

## 2. 未达成项 / 边界

1. **P0 3/3 未定**：第三发结果出来回填；当前只能写 2/3。
2. **P1 未修复、暂停/不续**：90 高背板判 **否（不是直接/全局原因）**；42 高 tile 直接贡献；只有「90 高定向 trim 后至少两个 90 高标签带下降 ≥50% 且 42 高 Notes 不变」才续查。当前不续，不再为 P1 起 boot。
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

- 决策：`docs/gpu-acceleration-stage19-decisions.md`（D1–D7，含每条的依据/影响/何时可推翻）。
- 实验手册：`docs/gpu-acceleration-stage19-p0-runbook.md`（§4.29 P3 收口、§4.31 fallback5、§八 P4、附录 extension check-in）。
- 计划：`docs/gpu-acceleration-stage19-plan.md`（P0–P4 状态、不做清单、D7 终局）。
- P4 归档：`logs/s19o-p4-cells/`（`four-cell.md`、`report.md`、`attempt3-framewindow/`、`attempt1-1256/`）。
- P3 归档：`logs/s19o-p3-fallback5/`、`logs/s19o-p3-surface-enum/`、`logs/s19o-p3b-d3/`。
- `make test-open` PASS；`make test` 的 `test-gpu` 因缺 `/usr/share/vulkan/icd.d/lvp_icd.json` 失败（环境问题，不是代码）。

## 5. 待回填（只改这一处，不改结论）

- [ ] **P0 第三发**：PASS → 3/3；FAIL → 2/3（附截图/日志时间）。

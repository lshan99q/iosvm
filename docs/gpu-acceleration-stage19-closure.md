# Stage 19 收口包（最终状态 / 未达成 / 移交 / 复现）

> **状态：2026-11-10 更新。** P0 **3/3 PASS**，**唯一工作基线 system = `8bf5f838`**（复现 99.99%）；`6be494f1` 仅对照（89.50%，不可复现 P0）；P1 重开后判 A、修复中（未修复）；P2 未达成；P3 真路径不可行 + fallback5 已交付；P4 0x502 通过。
> 本页只做索引和结论，不重复实验细节；所有数字以 runbook/decisions/logs 为准。
> 不写 P1/P2/P3 真路径通过；不写性能达标。
> **下一阶段计划**：`docs/gpu-acceleration-stage20-plan.md`、`docs/gpu-acceleration-stage21-plan.md`（E1 `ExtensionWatchdog=false`，30 分钟限时；**E1 终局 C（不可判/不可行），control 完成；P2/P3 未重开；fallback5 仍是 P3 交付；system 恢复 6be494f1**）。

## 1. 一页状态

| 项 | 最终状态 | 判据/证据 |
|---|---|---|
| **P0** | **3/3 PASS（工作基线 8bf5）** | **唯一工作基线 system = `8bf5f838`**：v7 99.85%、v9 99.98%、p0-3 98.81%；复现发 8bf5 + root 3207a35e + host b1e87d = **99.99% SAME**（missing 0/extra 0），P0 三判据 PASS。`6be494f1` 仅对照：89.50%，不可复现 P0；不再当工作基线。原件 `/tmp/system.apfs.bak-s20` + `/tmp/system-8bf5f838.keep`；对照 keep `/tmp/system-6be494f1.keep`。见 D9、`logs/s20-p0-repro/`。 |
| **P1** | **重开后判 A，修复中（未修复）** | 虚线段写在 **90 高 label tile 底部行**（guest 生成/行映射）：Photos row61 **54 px/alpha 247**；FaceTime row61–62 **47 px/alpha 241,253**；Wallet 58–61；label-4 53–56。**B（采样放大）已排除**（tile 行不是空行）。修复方向在 guest glyph/label tile 生成侧，不是 host sampler/linear/clamp。**第一次修复 FAIL**：`glyph_last_row=1` 清到 42 高 glyph 最后一行，Weather 掉笔画、虚线仍在；改向 label tile row61/62，验收看 guest 截图（`logs/s20-p1-validation/report.md`）。见 §4.32、`logs/s20-p1-54px-dash/report.md`。**P1 未修复，不许写已修。** |
| **P2** | **关闭，未达成** | P2d 四 job、P2e C1 limits、C2 xpcproxy、根因审计四路径走完；根因与 P3 同源 **extension check-in timeout**。不再投入（D3）。 |
| **P3** | **真路径不可行 + fallback5 已交付** | 真路径 D1 不可行；guest-side placeholder（拉面）fallback5 PASS，yellow 13,733/white 11,174/black 1,825；**是占位，不是真 widget**。见 §4.29/§4.31、D1/D2。 |
| **P4** | **0x502 通过；仅 cell1 可用** | `0x502` 四格历史全 0；**cell1-pure 120 帧/11.00s=10.91 fps（guest）唯一可用**；cell2 183/343≈0.04 fps、cell3 2 帧/772,366ms≈0.003 fps，均不可用；**cell4 NOT RUN 是判决，不是漏跑**；`/tmp/root-w15w3.apfs`（cell4 root）当前缺失，后续要用需重建。见 §8.7、D7。 |

## 2. 未达成项 / 边界

1. **P0 工作基线 = `8bf5f838`**：复现 99.99%，3/3 PASS；6be 仅对照 89.50%，不再当工作基线。原件 `/tmp/system.apfs.bak-s20` / `/tmp/system-8bf5f838.keep`；6be keep `/tmp/system-6be494f1.keep`。任何实验 report 必须写 system md5，否则不可比。
2. **P1 重开后判 A、修复中**：虚线段在 90 高 label tile 底部行（Photos 54px/alpha247、FaceTime 47px、Wallet 58–61、label-4 53–56）；B（采样放大）已排除。修复实验进行中；**P1 未修复**，不写已修。
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

- 决策：`docs/gpu-acceleration-stage19-decisions.md`（D1–D9，含每条的依据/影响/何时可推翻）。
- 实验手册：`docs/gpu-acceleration-stage19-p0-runbook.md`（§4.29 P3 收口、§4.31 fallback5、§八 P4、附录 extension check-in）。
- 计划：`docs/gpu-acceleration-stage19-plan.md`（P0–P4 状态、不做清单、D7 终局）。
- 路线评估：`docs/gpu-acceleration-gpu-passthrough-assessment.md`（为什么不做 GPU 直通；glwire 现状；三条路线与代价）。
- P0 复现性：`logs/s20-p0-repro/`（8bf5 99.99%）、`logs/s20-p0-recheck/`、`logs/s20-p0-isolateA/B/`、`logs/s20-p0-hostfix/`（commits `fba31f8`/`767053f`/`fff2678`）。
- P4 归档：`logs/s19o-p4-cells/`（`four-cell.md`、`report.md`、`attempt3-framewindow/`、`attempt1-1256/`）。
- P3 归档：`logs/s19o-p3-fallback5/`、`logs/s19o-p3-surface-enum/`、`logs/s19o-p3b-d3/`。
- `make test-open` PASS；`make test` 的 `test-gpu` 因缺 `/usr/share/vulkan/icd.d/lvp_icd.json` 失败（环境问题，不是代码）。

## 5. 回填状态

**全部已回填。** P0 3/3、P1 C 终局、E1 C/control 均已入档；后续只按新实验更新。

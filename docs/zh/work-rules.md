# 设计基线 · 工作规范

> 状态：仍然有效，已并入 CONTRIBUTING.md。见 docs/zh/README.md。

# IOSVM 工作规范

| 项 | 值 |
|---|---|
| 版本 | v0.1 |
| 适用 | IOSVM 项目的日常开发、决策、验收、发布 |
| 配套 | `IOSVM-工程文档.md`、`IOSVM-阶段文档.md`、`IOSVM-Git规范.md` |

---

## 1. 三条铁律

排在任何技术规范之前，因为违反任一条项目就死：

| # | 铁律 | 违反后果 |
|---|---|---|
| 1 | **仓库不含 Apple 二进制/镜像/固件** | Apple DMCA，项目立即消失 |
| 2 | **不写任何冒充真机通过 Apple 服务器校验的代码** | 同上 + 个人法律责任 |
| 3 | **仓库不含任何凭证** | 账号沦陷，历史永久污染 |

这三条是 review 的一票否决项，不做「下不为例」。

---

## 2. 工作项与目录约定

### 2.1 文档职责

| 文件 | 职责 | 更新时机 |
|---|---|---|
| `docs/architecture.md` | 架构总览 | 架构变更时 |
| `docs/adr/ADR-NNN-*.md` | **单条决策**及其理由 | 每次做非显然决策时 |
| `docs/adr/D1-*.md` | 决策门结论（含证据） | 决策门关闭时 |
| `docs/phases.md` | 里程碑与验收 | 阶段开始/结束时 |
| `docs/demo/` | 验收证据（截图/视频） | 每里程碑 |
| `PROVENANCE.md` | 声明的二进制来源 | 每次新增二进制 |
| `NOTICE` | 第三方代码登记 | 每次借用代码 |

**ADR 一文件一决策**，不合并、不改历史 ADR 的结论（推翻就新写一条 `Supersedes: ADR-003`）。

### 2.2 ADR 模板

```markdown
# ADR-003: 用 IPSW kernelcache 而非自编译 XNU

- 状态：Accepted
- 日期：2026-11-XX
- Supersedes: —
- Superseded by: —

## 背景
（问题是什么，约束是什么）

## 决策
（一句话结论）

## 权衡过的选项
| 选项 | 优点 | 缺点 |
|---|---|---|

## 后果
（正面/负面/需要后续跟进的事）

## 证据
（实测数据、日志、链接；没有证据就写「推测」并标注）
```

**没有「证据」栏的 ADR 不算数。** 写「推测」也可以，但必须显式标注，不许伪装成结论。

---

## 3. 任务流

### 3.1 Issue 三类

| 模板 | 用途 | 必须包含 |
|---|---|---|
| **Bug** | 行为不符预期 | 复现步骤、期望/实际、宿主环境、`firmware` SHA256 前缀 |
| **Task** | 有明确做法的实现工作 | 验收标准（命令或检查项）、依赖的 issue |
| **Spike** | 未知数研究，**时间盒** | 时间盒长度、要回答的具体问题、产出物形态 |

### 3.2 Spike 规范（最重要的流程）

因为项目 M1/M3 有大量未知数，spike 是主要工作方式。

| 规则 | 规定 |
|---|---|
| **时间盒** | 默认 **3 天**，最多 5 天。到期**必须**出结论，无论成败 |
| 产出 | 一份 `docs/adr/`，写清「试了什么 / 结果 / 结论 / 下一步」 |
| 结论类型 | 只允许三种：`可行` / `不可行` / `需要更多信息（附具体缺什么）` |
| 禁止 | 时间盒到期后「再试一天」（这是项目最常见的死法） |
| 分支 | `spike/<名>`，结束后**删分支**，结论进文档 |
| 代码 | spike 代码**不进主干**，除非它本身有测试价值 |

**为什么严格**：M1 的 D1 决策门就是 2 个 spike（各 3–4 天）。spike 一旦失控，整个项目就变成无限期研究。

### 3.3 决策门（Go / Pivot）

决策门关闭时必须在 `docs/adr/D<N>-*.md` 里留下：

1. 所有候选方案
2. 每个方案的**实测证据**（不是推测）
3. 选择理由
4. 选择错了的回头成本（如果 pivot 代价 < 继续的代价，就 pivot）

已定义的决策门：

| 门 | 触发时机 | 后果 |
|---|---|---|
| **D1** 显示路径 | M1 第 4–5 周 | C1/C2 可行 → 继续；都不可行 → pivot 到 C3 |
| **D2** AIR 是否被签名校验 | M3 第 1 月 | 无校验 → 继续；有校验 → **停止并做法务评估**（不得顺手绕过） |

---

## 4. 每日 / 每周节奏

| 频率 | 事 | 完成标准 |
|---|---|---|
| 每次提交前 | `make test` | 绿 |
| 每天 | 至少推进一个可验证的小步 | 能跑命令证明 |
| 每天 | 更新 issue 状态或留一行进度 | 不写「继续调研」这种空话 |
| 每周 | `probe` 增加一行检查，或修一个 flaky | 有 diff |
| 每阶段 | demo 截图/GIF + ADR + tag | 见阶段文档验收 |
| 每里程碑 | 技术博客一篇 | 见下 |

### 博客节奏（项目存活策略）

工程文档 R6 是「单人续航」风险，对抗它的唯一办法是**把中间成果公开发布**：

| 里程碑 | 博客主题 | 为什么有读者 |
|---|---|---|
| M1 | 《在 QEMU 里把 iOS 26 的用户空间跑起来了》 | 极少数人做到过 |
| **M2** | 《第一个把 Metal 命令流打到宿主 GPU 的开源虚拟机》 | **本身即成果，M3 失败也成立** |
| M3 | 《iOS 26 第一帧》 | 若成，GitHub Trending |

**M2 是项目的"保险单"**：哪怕后面全部失败，M2 的成果 + 博客也能让项目有独立价值。

---

## 5. 代码规范

### 5.1 许可头（强制）

每个新增源文件第一行必须有 SPDX 标识：

```c
/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 <owner> */
```

| 目录 | SPDX |
|---|---|
| `hw/` `proto/` `host/` `guest/` `tools/` | `MIT` |
| `qemu/**`（改动文件） | 保持 QEMU 原有（`GPL-2.0-or-later`） |
| XNU 补丁 `xnu/*.patch` | `APSL-2.0` |

**APSL-2.0 的硬义务**：对 XNU 的修改必须以源码形式发布。不能用「只发 patch 里的二进制」的方式规避。

### 5.2 QEMU 侧规范

`hw/` 下的代码要和上游 QEMU 风格一致，否则未来合并代价高（R7）：

| 项 | 规定 |
|---|---|
| 风格 | 遵循 `qemu/docs/devel/style.rst`，由 `scripts/checkpatch.pl` 检查 |
| 依赖 | 不用 C++ 异常/RTTI；不用 STL |
| 设备模型 | 用 QOM（`TYPE_DEVICE`），`DeviceClass::realize` 里初始化 |
| 内存 | 用 `memory_region_init_*`，不自己 `mmap` |
| 线程 | 遵循 QEMU 的 iothread/AIO 模型，不用裸 pthread |
| 错误 | `error_setg()`，不 `printf` |
| 调试 | `trace_events`，不用 `fprintf` 埋点 |

### 5.3 C++ / ObjC 侧（`host/` `guest/` `proto/`）

| 项 | 规定 |
|---|---|
| 标准 | C++20；ObjC 用 ARC |
| 格式 | `clang-format`（配置随仓库 `.clang-format`） |
| 命名 | 类型 `PascalCase`，函数 `camelCase`，成员 `m_`，常量 `kPascalCase` |
| 错误处理 | 返回 `Result`/`std::expected`，异常不用 |
| 日志 | 统一 `iosvm::log(level, fmt, ...)`，直连 stderr，不散落 |
| 断言 | 测试用 `CHECK()`；生产路径用 `VERIFY()`（不可恢复即 abort 并 dump ring 状态） |

### 5.4 协议定义（`proto/`）

- 所有结构体显式 `#pragma pack` 或 8 字节对齐，**不得依赖编译器默认布局**
- 所有字段用定宽类型（`uint32_t` 而非 `int`）
- 显式小端；跨架构用 `le32toh` 系列
- **协议变更必须升版本号**，且保持旧版 Guest 能拒绝新版 Host（`version` 字段先比较）
- `proto/` 下每个改动必须有对应单测（序列化 + 边界值）

### 5.5 脚本（`tools/`）

- Bash 用 `#!/usr/bin/env bash` + `set -euo pipefail`
- Python 用 3.10+，不引入需编译的依赖
- 所有脚本必须能 `--help`，且幂等（重复运行不产生额外副作用）
- 涉及写盘的脚本**默认 dry-run**，需要显式 `--apply`（参照 HakiOS 的 `userdata-carve.py` 做法）

---

## 6. 合规工作流（每个 PR 走一遍）

### 6.1 新增文件检查

```
□ SPDX 头正确
□ 若借用第三方代码 → NOTICE 已登记（项目名、URL、许可、借用了什么）
□ 若是二进制 → PROVENANCE.md 有来源说明，且 provenance-check 通过
□ 若 import 了 Apple 的头文件（SDK） → 只在 guest/ 且确认不随仓库分发
```

### 6.2 合规红线检查（一票否决）

```
□ 没有 firmware/ userland/ 下的文件
□ 没有 *.ipsw / *.dmg / *.apfs / bootkc / dtree
□ 没有 token / key / 证书 / .env
□ 没有实现激活/身份伪造/服务器校验绕过的代码
□ 没有「自动下载 Apple 内容」的脚本（使用者必须自己提供）
```

最后一条容易踩：`tools/` 里**绝不能出现自动下载 IPSW 的逻辑**，只能接受使用者给的文件路径。

### 6.3 CI 强制

| 检查 | 失败即阻断 |
|---|---|
| `gitleaks` | 凭证泄露 |
| `provenance-check.py` | 未申明二进制 / 禁止文件类型 |
| `checkpatch.pl`（`hw/`） | QEMU 风格 |
| `clang-format --dry-run --Werror` | 格式 |
| `make test` | 单测 + fake-gpu |
| `make test-open`（无 Apple 文件路径） | 贡献者门槛保证 |

**`make test-open` 必须永远可用**——这是没有 iPhone/Mac 的贡献者唯一的入口（R5）。

---

## 7. 验收规范

### 7.1 阶段验收（DoD）

每阶段的验收**必须是可执行命令**，不许是「看起来可以了」。

```sh
# 反例（不接受）
"桌面能显示了"

# 正例（接受）
./tools/screendump.sh && python3 tools/check-frame.py logs/screen.png --min-unique-colors 64
```

### 7.2 发布前 checklist

```
□ 阶段文档里该阶段所有交付物勾完
□ 验收命令在干净环境下重跑通过
□ PROVENANCE.md / NOTICE 无遗漏
□ CHANGELOG 写了「可用 / 不可用 / 不含什么」
□ release notes 含 firmware SHA256（不含文件）
□ 至少一份 demo 证据（截图或视频）进 docs/demo/
□ 已知问题登记为 issue，并在 release notes 里列「已知不可用」
```

### 7.3 数据记录规范

任何性能或正确性结论必须能复现：

| 记什么 | 格式 |
|---|---|
| 宿主环境 | `uname -a`、发行版、Vulkan 驱动版本（`vulkaninfo --summary`） |
| Guest 环境 | IPSW 版本号、设备标识（`iPhone17,3`）、kernelcache SHA256 前 12 位 |
| 结果 | 数字 + 单位 + 样本量（不写「很快」，写 `4.2 ms, n=100`） |
| 结论 | 与哪个基线比较 |

**禁止在文档里写「接近真机性能」这类不可验证的话。**

---

## 8. 沟通规范

| 项 | 规定 |
|---|---|
| Issue / PR 语言 | 英文优先（项目面向国际），中文可接受 |
| Commit | 英文 |
| 文档 | `docs/` 英文为主，中文版可选；ADR 至少英文 |
| 回复节奏 | 个人项目：**不承诺 SLA**，在 README 写明「best effort」 |
| 技术分歧 | 写 ADR 对比选项，不在 PR 评论里拉锯 |
| 拒绝贡献 | 引用 §6.2 条款编号，不针对人 |

---

## 9. 数据与备份

| 项 | 规定 |
|---|---|
| `firmware/` `userland/` | 不入库。**自己离线备份**，丢了就要重新走 Mac 提取流程（ADR-007 的代价） |
| 关键产出 | ADR、probe 输出、golden 图像、性能数字 —— 全部进仓库或用 issue 附件 |
| 实验脚本 | 一次性的放 `tools/scratch/`（gitignore），**有价值的必须整理进 `tools/`** |
| 崩溃/panic 日志 | 脱敏后作为 issue 附件，不进仓库 |

---

## 10. 立刻要做的三件事

按顺序：

1. **撤销已泄露的 GitHub token**（见 `IOSVM-Git规范.md` §5.4 的 SOP）。撤销后重新用 `gh auth login` 或 SSH key 配置，**不要把 token 写进任何文件或聊天**。
2. **开启仓库的 Secret scanning + Push protection**（GitHub → Settings → Code security）。
3. 按 `IOSVM-阶段文档.md` 的 M0 开工：建仓骨架 → `PROVENANCE.md` + CI → 再写第一行功能代码。

---

## 11. 规范自身的维护

| 项 | 规定 |
|---|---|
| 修改 `PROVENANCE.md`、`LICENSE*`、`ci/`、任一份规范文档 | 必须单独 PR，且说明理由 |
| 规范与实现冲突 | **规范让步**，但要在同一次 PR 里改规范 |
| 规范过期 | 每季度过一遍 §6.2 与 §7，删掉不再适用的条款 |

# 设计基线（中文原始文档）

这里是 IOSVM 最初的设计文档集，写于 2026-10-31。它们**仍然是可读的设计依据**，但其中几处
判断已被后续的实测与上游证据推翻。阅读时以英文 ADR 为准，本目录用来追溯"当初是怎么想的"。

| 文件 | 内容 | 现状 |
|---|---|---|
| `design-baseline.md` | 可行性调研全记录 | 事实部分仍成立；「必须 Mac」与「C3 是备选」两条已被 ADR-008 / ADR-009 取代 |
| `engineering.md` | 架构、ADR、协议草案、仓库结构 | 架构与协议仍是 M2 的草案；ADR-003/007 已被取代 |
| `phases.md` | M0–M4 分解 | 已被 `docs/phases.md` 取代（M1 范围收窄，D1 换成 D1'） |
| `git-rules.md` | 分支、提交、凭证管理 | 仍然有效，已并入 `CONTRIBUTING.md` 与 ADR-006 |
| `work-rules.md` | 三条铁律、spike 流程、代码规范 | 仍然有效，已并入 `CONTRIBUTING.md` |

## 被推翻的两条，逐条说明

1. **「提取固件必须有 Mac」（原 ADR-007）** —— 不成立。HakiOS 的 `sysimage/unpack.sh` 全程
   Linux（`python3` + `7z>=23` + `cc`），本机已验证 7-Zip 23.01 可读 APFS。见 ADR-009。
2. **「C3（自编译 XNU + 自写驱动）是最后备选，D1 决策门才能定」** —— 反了。HakiOS 已经在
   QEMU `virt` 上用自编译 XNU + 自写驱动跑到主屏（Darwin 16.5），C3 是**起点**，不是退路。
   本机实测：同一份内核在 6.4 秒内走到 BSD 初始化（`docs/m0-host-report.md`）。见 ADR-008。

英文文档优先，冲突时以英文为准。

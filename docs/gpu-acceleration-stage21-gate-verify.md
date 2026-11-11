# Stage 21 gate.sh 端到端验证（PASS + FAIL 两发）

**结论：gate.sh 通过端到端验证。**
- **PASS**：`GATE: PASS  (checks: shot=427K names=18 0x502=0 panic=0)`（cell1 纯本地基线，`QEMU_SMP=1`）。
- **FAIL**：`GATE: FAIL  (checks: shot=435K names=18 0x502=0 panic=0)` +
  `GATE: FAIL reason: screenshot 445074 B < 10485760 B`（`--minshot 10MB` 故意缺项，精确指出哪一项不满足）。

## PASS（cell1）

- root `/tmp/root-local.apfs` `f39b910543189be4934a48c4d495e928`；system `8bf5f838…`；
- `gate.sh --root /tmp/root-local.apfs --out /tmp/gate-cell1`；
- 输出：`GATE: PASS (checks: shot=427K names=18 0x502=0 panic=0)`；
- `gate.md`：screenshot 436770 B / names 18 / 0x502 0 / panic 0 / SIGSEGV 0 / artifacts PASS；
- `boot.log`：`end=ui ui=870s ui_bytes=436770 guestframes=22 hostframes=0 panic=0 biggest=436770`；
- 产物：`shot.png`（427K）、`guest.log`、`host.log`、`boot.log`、`md5s.txt`、`gate.md`。

## FAIL（--minshot 10MB）

- `gate.sh --root /tmp/root-local.apfs --minshot 10MB --out /tmp/gate-fail`；
- 输出：`GATE: FAIL (checks: shot=435K names=18 0x502=0 panic=0)`；
- `GATE: FAIL reason: screenshot 445074 B < 10485760 B`（只有 screenshot 一项 FAIL，
  names/0x502/panic 仍 PASS —— 说明脚本能指出具体失败项，而不是只给 FAIL）；
- `gate.md` 表格同样只把 screenshot 标 FAIL。

## 调用侧修复记录（验证中发现并已由 code 修）

| # | 问题 | 修复 |
|---|---|---|
| 1 | `boot-measure.sh` 的 `IOSVM` 默认 `/home/lshan/...` | gate.sh 内置并 export `IOSVM` |
| 2 | `run.sh` 缺 `QEMU_EFI/QEMU_VARS` | gate.sh 内置并 export |
| 3 | `boot-measure.sh` 的 `GLWIRE` 默认 `/home/lshan/...` | 运行时用 `GLWIRE=` 覆盖（建议同样内置） |
| 4 | `QEMU_SMP` 未传导致 cell1 用 4 核、Ticket spinlock panic | gate.sh 默认 `QEMU_SMP=1` |

- 第 1/2/4 已由 code 的 `006c44e` 修掉；第 3 我用 `GLWIRE=/host/iosvm/tools/glwire`
  覆盖跑通，建议 gate.sh 也内置 `GLWIRE` 默认值。

## 归档

`logs/s20-gate-verify/cell1-pass/`（PASS）、`logs/s20-gate-verify/cell1-fail-10mb/`（FAIL）、
`logs/s20-gate-verify/cell1/`（早期 panic 失败记录）。

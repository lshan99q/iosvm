# Stage 21 gate.sh 端到端验证（cell1 分支未 PASS）

**结论：gate.sh 本身能跑通、产物与 FAIL 明细正确；但 cell1 基线在这台机上三次
都没 PASS —— 前两次是 gate.sh 调用侧环境（IOSVM / QEMU_EFI / QEMU_VARS）没传，
第三次进 UI 阶段 panic（Ticket spinlock timeout）+ hostframes=0、截图 20K。**

## 三次尝试

| # | 问题 | 结果 |
|---|---|---|
| 1 | `boot-measure.sh` 的 `IOSVM` 默认 `/home/lshan/...`，gate.sh 未导出 | `no such directory`，立即 FAIL |
| 2 | 补 `IOSVM`：`run.sh: no UEFI firmware found`（gate.sh 未传 QEMU_EFI/QEMU_VARS） | qemu 起不来，0 帧 |
| 3 | 补 `QEMU_EFI/QEMU_VARS` | UI 阶段 **panic=1**（`Ticket spinlock[...] timeout @lock_ticket.c:386`）、hostframes=0、shot=20K |

## 第三发关键日志

- guest：`panic(cpu 1 caller 0xfffffff007af0024): Ticket spinlock[0xfffffff007b0d880] timeout after 60000047 ticks ... @lock_ticket.c:386`
- boot：`end=crash ui=0s ui_bytes=0 guestframes=3 hostframes=0 panic=1 biggest=19602`
- gate.md：screenshot 20K < 300K、names 0 < 5、0x502=0、panic=1。

## 判断

- **gate.sh 的产物/明细是对的**：out 下有 `gate.md`、`md5s.txt`、`shot.png`、
  `guest.log`、`host.log`、`boot.log`，FAIL 能指出 screenshot/names/panic 三项。
- 前两次 FAIL 是**脚本读错/缺失环境**（符合用户「先判断是不是脚本读错产物路径」），
  不是判据问题；第三次是 cell1 本身 panic，未复现 10.91 fps 基线。
- 建议 code：gate.sh 内置 `IOSVM` / `QEMU_EFI` / `QEMU_VARS` 默认值；并核对
  cell1 当前 root（`f39b9105`）+ system（`8bf5f838`）组合是否仍能 PASS。

## 未完成

- FAIL 分支的「故意缺项」用 `--minshot 10MB` 还没单独跑（cell1 已 FAIL，未获得
  干净 PASS 基线对照）；等 code 修 gate.sh 默认值 + cell1 组合后可补。

Artifacts: `/host/iosvm/logs/s20-gate-verify/cell1/`.

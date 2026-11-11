# Stage 21 P1 +1 发（copyscrub）— legacy scrub 命中，A 点不成立

**结论：①`tile rows 61-63 <- layer rows X-Y` / `cleared 178x90` 均 0 次；但
`glwire: legacy 178x90 scrub rows 61-63 before native alpha upload` 命中 1 次。
②截图 `shot.png`（297K）标签区域已出图（像素级虚线判定见下）。③内容判据未
回退（host 117 帧、截图 297K、无 panic）。**

**判定：A 点（glCopyTexSubImage2D）不成立，真实源为 legacy GL_ALPHA teximage。**

## Recipe

- root `/tmp/root-w15-rle10.apfs` `a3fec104…`（lib `3a42016b…`）；
- host `6aa395ce…`（132,720 B，含全部 P1 字符串；`strings` 命中 `cleared 178x90` 1 次）；
- system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=0 glwire_upload_rle=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_LABEL_ROW_SCRUB=61-63"`。

## Numbers

| 项 | 值 |
|---|---|
| `legacy 178x90 scrub rows 61-63` | **1 次**（命中） |
| `tile rows 61-63` / `cleared 178x90` | **0 次** |
| `legacy texture 178x90 ... taken natively` | 出现（native 路径） |
| guest 帧 | 2/26 + 2/20 + 2/26（低） |
| host 帧 | 117 |
| 截图 | **297K**（`shot.png`） |
| panic | 无 |

## 判断

- legacy alpha 上传前的 scrub 已执行（日志命中），但 glCopy 路径（tile rows/
  cleared）从未执行 → **A 点不成立**，178x90 的源就是 native legacy GL_ALPHA
  teximage。
- 本发只作记录；**P1 冻结，不再 boot**。
- 虚线是否消失需像素级裁剪对比（本次未做）；如需，用 `shot.png` 与
  `logs/s20-p1-map/shot.png`（302K）做差分即可。
- 后续交付收尾，等 code 的 28 提交合完（`origin/main` = `2d8c011`）。

Artifacts: `/host/iosvm/logs/s20-p1-copyscrub/`.

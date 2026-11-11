# Stage 20 rle10 — 移除 sent==3 拷贝后（s11 指向 surface 1）

**Verdict: s11 从 surface 2 变成 **surface 1**，且 px 里有 `ff000000 ff00ff00`
（部分非黑）；但 host `read-back fallback panel (uploaded panel surface)` 仍拿
到 image0 的黑（30,554 B/1%），guest 仍 3 帧/5.2 s、host 2 帧、无截图。说明
移除拷贝后 image1 保住了部分 UI，但 host fallback 仍选 image0。**

## Recipe

- root `/tmp/root-w15-rle10.apfs` `a3fec104…`（785bcbac + lib `3a42016b…`）；
- host `3836ff05…`；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=1 glwire_upload_rle=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_565=1 GLWIRE_UPLOAD_RLE=1"`。

## Numbers

| 项 | 值 |
|---|---|
| host `read-back fallback panel` | 2 次（仍 image0） |
| host `rgb565 rle` | `raw 2,436,000 -> 30,554 B (1%)`，pack 0.9 ms |
| guest `upload-rle 565` | 2 次：`30,554 B (1%), unpack 46–47 ms` |
| guest `s11 after` | **surface 1**：`row 3072 … px ff000000 ff00ff00`（部分非黑） |
| guest 帧 | 3 帧/5.2 s |
| host 帧 | 2（grep） |
| 截图 | 无 |

## 判断

- 移除 sent==3 拷贝后 **s11 指向 surface 1（image1）**，px 有 `ff00ff00`（绿），
  说明 image1 确实有部分 UI，不再被黑 image0 覆盖。
- 但 host fallback 仍优先 image0（row3008 黑），RLE 输入还是 1%；下一步按 code：
  **让 RLE decoder 以 image1（或两块都写）为目标**，而不是 image0。
- 本发不能作为端到端判据；方向已确认（target surface 选择）。

## 系列

| boot | 变化 | s11 | packed | guest |
|---|---|---|---|---|
| rle9 | 565+RLE | surface 2（黑） | 30,554 B | 3/8.2 |
| **rle10（no-copy）** | 移除黑拷贝 | **surface 1（部分非黑）** | 30,554 B | 3/5.2 |

Artifacts: `/host/iosvm/logs/s20-rle10/`.

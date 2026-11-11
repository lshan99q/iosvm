# Stage 20 rle11 — host 读 guest surface2 segments（path 生效，但仍 1%）

**Verdict: `read-back fallback guest surface2` 出现 2 次（host 已改为直接读
image1 的 segments2），但 packed 仍是 30,554 B (1%) —— segments2 内容本身
基本全黑。s11 仍是 surface 1、`px ff000000 ff00ff00`（只有部分非黑）。
guest 4 帧/493 s、host 3 帧、无截图。**

## Recipe

- root `/tmp/root-w15-rle10.apfs` `a3fec104…`（lib `3a42016b…`）；
- host `e5c442f4…`（guest surface2 读）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=1 glwire_upload_rle=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_565=1 GLWIRE_UPLOAD_RLE=1"`。

## Numbers

| 项 | 值 |
|---|---|
| `read-back fallback guest surface2` | **2 次**（新路径生效） |
| host `rgb565 rle` | `raw 2,436,000 -> 30,554 B (1%)`，pack 0.8–1.4 ms |
| guest `upload-rle 565` | `30,554 B (1%), unpack 44–47 ms` |
| guest `s11 after` | surface 1：`row 3072 … 301e18b8 301e18b8 86072291 02440778 … px ff000000 ff00ff00` |
| guest 帧 | 4 帧/493 s（0.008 fps） |
| host 帧 | 3（grep） |
| 截图 | 无 |

## 判断

- guest surface2 的读取路径已生效，但 **segments2 内容本身基本全黑**（1%），
  与 s11 的 `ff000000` 一致；只有少量 `ff00ff00`（部分 UI）。
- 按 code 分支：segments2 内容黑 → **下一步从 image1 的 texture（而非 RAM）取图**。
- 本发不能作为端到端判据。

## 系列

| boot | 变化 | fallback | packed | guest |
|---|---|---|---|---|
| rle10 | no-copy | panel(image0) | 30,554 B | 3/5.2 |
| **rle11（guest surface2）** | 读 segments2 | **guest surface2** | 30,554 B | 4/493 |

Artifacts: `/host/iosvm/logs/s20-rle11/`.

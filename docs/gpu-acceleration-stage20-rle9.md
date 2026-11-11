# Stage 20 rle9 — 565+RLE 决定性组合（输入仍是黑）

**Verdict: 565+RLE 走的是 2 字节 565 RLE（host `rgb565 rle raw 2,436,000 ->
packed 30,554 B (1%)`，guest `upload-rle 565 ... unpack 44–54 ms`），但 packed
仍是 1%（30 KB）—— 输入仍是黑图。565 单独正常（2,436,000/329 帧），说明
RLE 的输入在 565+RLE 组合下也黑，不是 RLE 把 BGRA 当 565。**

## Recipe

- root `/tmp/root-w15-rle9.apfs` `db5b32ac…`（785bcbac + lib `608d8c27…` target 修复）；
- host `3836ff05…`（nonblack 缓存）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=1 glwire_upload_rle=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_565=1 GLWIRE_UPLOAD_RLE=1"`。

## Numbers

| 项 | 值 |
|---|---|
| host `rgb565 rle` | `raw 2,436,000 -> packed 30,554 B (1%)`，pack 0.8–1.3 ms |
| guest `upload-rle 565` | `2,436,000 -> 30,554 B (1%), unpack 44–54 ms` |
| guest 帧 | 3 帧/8.2 s（0.37 fps） |
| host 帧 | 3（grep） |
| 截图 | 无 |

## 判断

- 565+RLE 的 2 字节路径工作（host/guest 日志匹配），但 **packed 1%（30 KB）**
  → 输入画面仍是黑；565 单独（无 RLE）2,436,000/329 帧正常。
- 说明问题不是「RLE 与 565 接线」或「BGRA 当 565」，而是 **RLE 使能时输入
  变成黑**（host picture/panel 数据在 RLE 分支下不对），或 target 仍未就绪。
- 需要 code 查 RLE 分支下 host picture 的来源；本发不能作为 RLE 端到端判据。

## 系列

| boot | 组合 | packed | guest |
|---|---|---|---|
| rle8 | RLE(BGRA) | 51,202 B (1%) | 3/5.0 |
| **rle9（决定性）** | **565+RLE** | **30,554 B (1%)** | 3/8.2 |
| 5653（对照） | 565 alone | 2,436,000 B（正常） | 5.3–5.9（静止段） |

Artifacts: `/host/iosvm/logs/s20-rle9/`.

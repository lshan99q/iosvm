# Stage 20 rle12 — image1 texture 直接读（tracked 命中，但 fallback 未走 image1）

**Verdict: `tracked image1 texture 1 row 3072` 出现 1 次（host 已跟踪 image1
texture），但 `read-back fallback image1 texture` **0 次**，packed 仍 30,554 B
(1%)。说明 fallback 的优先级/条件仍没选到 image1 texture，RLE 输入还是黑。**

## Recipe

- root `/tmp/root-w15-rle10.apfs` `a3fec104…`（lib `3a42016b…`）；
- host `2ace6bf0…`（track image1 texture）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=1 glwire_upload_rle=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_565=1 GLWIRE_UPLOAD_RLE=1"`。

## Numbers

| 项 | 值 |
|---|---|
| `tracked image1 texture` | **1 次**（row 3072） |
| `read-back fallback image1 texture` | **0 次** |
| host `rgb565 rle` | `raw 2,436,000 -> 30,554 B (1%)`，pack 0.8–0.9 ms |
| guest `upload-rle 565` | `30,554 B (1%), unpack 44–49 ms` |
| guest `s11 after` | surface 1：`row 3072 … px ff000000 ff00ff00` |
| guest 帧 | 4 帧/528 s（0.008 fps） |
| host 帧 | 3（grep） |
| 截图 | 无 |

## 判断

- image1 texture 被跟踪到（1 次），但 fallback 没选它（0 次）→ host 的
  fallback 优先级/条件问题，不是 texture 内容问题。
- 需要 code 查：为什么 `read-back fallback image1 texture` 没打印（是否
  condition 与 tracked 不一致、或顺序被 panel 分支先吃掉）。
- 本发不能作为端到端判据。

## 系列

| boot | 变化 | tracked | fallback image1 | packed |
|---|---|---|---|---|
| rle11 | guest surface2 | — | — | 30,554 B |
| **rle12（image1 texture）** | track row3072 | **1** | **0** | 30,554 B |

Artifacts: `/host/iosvm/logs/s20-rle12/`.

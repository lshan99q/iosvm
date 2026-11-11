# Stage 20 rle7 — RLE fallback 顺序修复（fallback 触发，但仍是 51KB）

**Verdict: fallback 触发了（`read-back fallback texture 1/4` ×2），`bgra rle`
也出了（×2），但 packed 仍是 **51,202 B (1%)** —— 和之前黑帧一样大。说明
fallback 到的 texture 内容仍接近全黑（或 RLE 的 95x 不是黑帧而是真实内容，
需用截图确认）。guest 仍 4 帧/505 s、host 3 帧、无截图。**

## Recipe

- root `/tmp/root-w15-rle5.apfs` `cdd8b100…`（lib `ff3f63d5…`）；
- host `0779e5d7…`（201 顺序修复）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_rle=1 glwire_upload_565=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_RLE=1 GLWIRE_UPLOAD_565=0"`。

## Numbers

| 项 | 值 |
|---|---|
| `read-back fallback texture` | **2 次**（texture 1/4） |
| host `bgra rle` | 2 次：`raw 4872000 -> packed 51,202 B (1%)`，pack 1.0/1.6 ms |
| guest `upload-rle` | 2 次：`packed 51,202 B (1%), unpack 84–89 ms` |
| guest 帧 | 4 帧/505 s（0.008 fps） |
| host 帧 | 3（grep） |
| 截图 | 无 |

## 判断

- 顺序 bug 修好后 fallback 确实触发了，但 **packed 仍 51 KB（1%）**，与黑帧
  一致；texture 1/4 内容仍接近全黑（或 RLE 的 95x 异常来自黑内容）。
- 下一步应由 code 确认 texture 1/4 是否真的承载 UI 像素（格式/通道/宽高），
  以及 host 3 帧/client closed 的位置；本发仍不能作为端到端 fps 判据。

## 系列

| boot | 变化 | fallback | bgra rle | packed | guest |
|---|---|---|---|---|---|
| rle6 | texture fallback | 0 | 0 | — | 3/9.3 |
| **rle7（顺序修复）** | 201 预扫前初始化 | **2** | **2** | **51,202 B (1%)** | 4/505 |

Artifacts: `/host/iosvm/logs/s20-rle7/`.

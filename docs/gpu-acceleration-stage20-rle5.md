# Stage 20 rle5 — RLE keep-alive（skip empty readback）

**Verdict: skip 生效（`rle skip empty readback` ×3），bgra rle 不再对黑图打包；
但 guest 仍 4 帧/505 s、host 3 帧（grep）、无截图。问题已不在 RLE unpack，
而在 host 晚段 `viewport 0x0`（没有 panel draw）的 present/readback 路径。**

## Recipe

- root `/tmp/root-w15-rle5.apfs` `cdd8b100…`（785bcbac + lib `ff3f63d5…`）；
- host `3562ed74…`（RLE skip empty）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_rle=1 glwire_upload_565=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_RLE=1 GLWIRE_UPLOAD_565=0"`。

## Numbers

| 项 | 值 |
|---|---|
| `rle skip empty readback` | **3 次** |
| host `bgra rle` | **0 次**（readback 一直 0x0，没打包） |
| guest `upload-rle` | 仅 `upload-rle=1`（无数据行） |
| guest 帧 | 4 帧/505 s（0.008 fps，仍饥饿） |
| host 帧 | 3（grep） |
| 截图 | 无 |

## 判断

- skip 修复按设计工作（不再把黑图 RLE 成 51 KB），但 **guest 仍只 4 帧**，
  说明 host 晚段一直 `viewport 0x0`（没有 panel draw），guest keep-alive 也救
  不回显示。
- 问题已不在 RLE unpack/pack，而在 **host 晚段没有 panel draw / present 路径**；
  需要 code 查 host 为何晚段 viewport 0x0，以及 present/client 关闭。

## 系列

| boot | 变化 | skip | bgra rle | guest | host |
|---|---|---|---|---|---|
| rle4 | display 修复 | — | 3（黑图 51KB） | 3/7.7 | 3 |
| **rle5（skip empty）** | 黑图不打包 | **3** | **0** | 4/505 | 3 |

Artifacts: `/host/iosvm/logs/s20-rle5/`.

# Stage 20 rle6 — RLE texture-fallback 复验（fallback 未触发）

**Verdict: host 没出现 `read-back fallback texture`（0 次），`bgra rle` 也 0 次；
guest 仅 `upload-rle=1`、3 帧/9.3 s；host 3 帧（grep）；无截图。1ff043 的
fallback 分支没走到。**

## Recipe

- root `/tmp/root-w15-rle5.apfs` `cdd8b100…`（785bcbac + lib `ff3f63d5…`）；
- host `1ff04394…`（texture fallback）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_rle=1 glwire_upload_565=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_RLE=1 GLWIRE_UPLOAD_565=0"`。

## Numbers

| 项 | 值 |
|---|---|
| `read-back fallback texture` | **0 次** |
| host `bgra rle` | **0 次** |
| guest `upload-rle` | 仅 `upload-rle=1`（无数据行） |
| guest 帧 | 3 帧/9.3 s（0.32 fps，仍饥饿） |
| host 帧 | 3（grep） |
| 截图 | 无 |

## 判断

- texture fallback 分支未触发（0 行）；host 仍是 viewport 0x0 的 readback，
  没有 fallback 到 texture。
- 需要 code 确认：本帧是否真的记录了 750x1624 opcode-201 上传、fallback 的
  条件/顺序（viewport 0x0 时是否在 RLE 分支前）、以及 host 5 帧/`client closed`
  的实际位置。

## 系列

| boot | 变化 | fallback | bgra rle | guest | host |
|---|---|---|---|---|---|
| rle5 | skip empty | — | 0 | 4/505 | 3 |
| **rle6（texture fallback）** | readback→texture | **0** | **0** | 3/9.3 | 3 |

Artifacts: `/host/iosvm/logs/s20-rle6/`.

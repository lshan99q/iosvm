# Stage 20 rle8 — RLE 非黑 texture 缓存复验（nonblack 未命中，仍 51KB）

**Verdict: `stored nonblack panel upload texture` 0 次，fallback 仍走
`read-back fallback panel (uploaded panel surface)`，packed 仍 51,202 B (1%)。
说明两块 panel surface 都没有非黑的全屏 texture 被缓存（或判定没触发），
和 code 的「两块 surface 都是黑的」分支一致。guest 3 帧/5.0 s、host 3 帧。**

## Recipe

- root `/tmp/root-w15-rle5.apfs` `cdd8b100…`（lib `ff3f63d5…`）；
- host `3836ff05…`（nonblack 缓存）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_rle=1 glwire_upload_565=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_RLE=1 GLWIRE_UPLOAD_565=0"`。

## Numbers

| 项 | 值 |
|---|---|
| `stored nonblack panel upload texture` | **0 次** |
| `read-back fallback` | 3 次：`fallback panel (uploaded panel surface)` |
| host `bgra rle` | 3 次：`raw 4872000 -> packed 51,202 B (1%)`，pack 0.9–1.1 ms |
| guest `upload-rle` | 3 次：`51,202 B (1%), unpack 84–87 ms` |
| guest 帧 | 3 帧/5.0 s（0.6 fps） |
| host 帧 | 3（grep） |
| 截图 | 无 |

## 判断

- 非黑缓存没有命中（0 次）→ 两块 panel surface 的上传都被判定为黑（或
  判定条件没覆盖实际格式/尺寸）；fallback 仍拿到黑图，packed 51 KB。
- 按 code 的分支：**两块 surface 都是黑的**，问题在 guest 侧 present。
- 本发仍不能作为 RLE 端到端判据；需要 code 查 guest present 为什么把 panel
  画到黑 surface。

## 系列

| boot | 变化 | nonblack | packed | guest |
|---|---|---|---|---|
| rle7 | 顺序修复 | — | 51,202 B | 4/505 |
| **rle8（nonblack 缓存）** | 非黑才缓存 | **0** | **51,202 B** | 3/5.0 |

Artifacts: `/host/iosvm/logs/s20-rle8/`.

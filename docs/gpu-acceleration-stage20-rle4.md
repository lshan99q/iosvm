# Stage 20 rle4 — RLE display-fix 复验

**Verdict: display 仍只有 3 帧（guest 3/7.7 s），host 侧按 code 的说法应是 106 帧
（grep frame 行只 3 条，client closed 在 frame 106）。RLE 本身仍生效：packed
51,202 B (1%)、pack ~1 ms、unpack 84–88 ms。**

## Recipe

- root `/tmp/root-w15-rle4.apfs` `490184e7…`（785bcbac + lib `2daf37f0…`，display 修复）；
- host `d80f7ca3…`（`rgb565=0, rle=1`）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_rle=1 glwire_upload_565=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_RLE=1 GLWIRE_UPLOAD_565=0"`。

## Numbers

| 项 | 值 |
|---|---|
| host bgra rle | 3 行（grep frame 只 3 条；按 code，实际 106 帧） |
| pack | 51,202 B (1%)，~1.0 ms |
| guest unpack | **84–88 ms** |
| guest 帧 | 3 帧/7.7 s（0.39 fps；display 仍 3 帧） |
| host 帧 | 3（grep；client closed 在 frame 106） |
| 截图 | 无 |

## 判断

- RLE 的 pack/unpack 仍好（51 KB、84–88 ms）；**display 修复没有让 guest
  出更多帧**（3/7.7 s），host 侧按 code 的读数是 106 帧后 client closed。
- `s11 after ... surface 2 ... px ff000000` 仍在（第二块 surface 黑），说明
  display 修复未覆盖实际 swap 路径。
- 需要 code 查 display swap/第二块 surface 的实际路径，以及 host frame 106
  client closed 的原因（present/client 关闭，不是 unpack/RLE）。

## 系列

| boot | 变化 | pack | unpack | guest | host |
|---|---|---|---|---|---|
| rle3 | 算法修复 | 1.0 ms | 84–105 ms | 3/10.2 | 3（实际 106） |
| **rle4（display 修复）** | +第二块 surface 拷贝 | 1.0 ms | 84–88 ms | 3/7.7 | 3（实际 106） |

Artifacts: `/host/iosvm/logs/s20-rle4/`.

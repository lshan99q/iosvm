# Stage 20 rle2 — RLE alone 真生效（BGRA run 压缩）

**Verdict: RLE 生效了（12 字段修复后）。** host `bgra rle raw 4872000 ->
packed 51202 B (1%), pack 1.0 ms`；guest `upload-rle bgra raw 4872000 ->
packed 51202 B (1%), unpack 396–451 ms`。**压缩比 ~95x（远超预期 13.5x）**，
但 **unpack 400 ms 仍把 guest 拖死**（4 帧/507 s，host 3 帧）。

## Recipe

- root `/tmp/root-w15-rle2.apfs` `2149f58a…`（785bcbac + lib `673a2560…`，12 字段修复）；
- host `d80f7ca3…`（`rgb565=0, rle=1`）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_rle=1 glwire_upload_565=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_RLE=1 GLWIRE_UPLOAD_565=0"`。

## Numbers

| 项 | 值 |
|---|---|
| host pack | 51,202 B / 4,872,000 B = **1%**；`pack 1.0 ms` |
| guest unpack | `unpack 396–451 ms` |
| guest | `upload-rle=1`；4 帧/507 s（0.008 fps，饥饿） |
| host 帧 | 3（停） |
| 截图 | 无 |

## 判断

- **RLE 本身成立**：95x 压缩、pack 1 ms；与 RLE 相关的 wire 字节降到 51 KB。
- **瓶颈转移到 unpack**：guest 端 396–451 ms/帧，把渲染线程拖死（与 565 的
  同步转换同型，但量级更大）。下一步必须把 RLE unpack 放 worker/异步，或
  再降 unpack 成本。
- 本发不作为端到端 fps 判据；host 3 帧是 guest 饥饿的连带。

## 与 rle-run 区分

- `s20-rle-run`：RLE 未生效（guest request 10 字段丢参数），已判 **工具问题**；
- `s20-rle2`：12 字段修复后 RLE 真正生效，压缩 95x、但 unpack 400 ms 饥饿。

Artifacts: `/host/iosvm/logs/s20-rle2/`.

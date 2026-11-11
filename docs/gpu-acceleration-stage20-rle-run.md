# Stage 20 rle-run — RLE 验收（开关未生效）

**Verdict: RLE 开关没生效。** host 构建标记有 `rgb565=1, rle=1`，但
`bgra rle`/`upload-rle` 都是 0 行；guest 仍 4 帧/494 s，host 3 帧，无截图。
lib `974a0e52…` / root `3212f6a5…`；host `d80f7ca3…`。

## Recipe

- root `/tmp/root-w15-rle.apfs` `3212f6a5…`（785bcbac + lib 974a0e52）；
- host `d80f7ca3…`（`rgb565=1, rle=1`）；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=1 glwire_upload_rle=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_565=1 GLWIRE_UPLOAD_RLE=1"`。

## 证据

- host `build marker ... rgb565=1, rle=1`（host 侧开关被识别）；
- host `bgra rle` = **0 行**；guest `upload-rle` = **0 行**；
- guest 4/494 s（饥饿）；host 3 帧；无截图；
- guest cmdline 607 字符（`glwire_upload_rle` 是否在 2048 缓冲内待 code 确认）。

## 判断

- RLE 未生效（guest 侧 0 行），本发不能作为 RLE 判据；与 5651 的 boot-arg
  截断问题同型，请 code 确认 `glwire_upload_rle` 的 boot-arg 是否被解析/是否
  在 2048 缓冲内，以及 565+rle 同时开是否被互斥逻辑挡住。

Artifacts: `/host/iosvm/logs/s20-rle-run/`.

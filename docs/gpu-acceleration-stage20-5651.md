# Stage 20 5651 — full-frame RGB565/fast 一发（开关未生效，但 guest 10–14 fps）

**Verdict: 565 开关未生效（host `rgb565` 与 guest `upload565` 都是 0 行），
但 guest 这次反而到 **10–14 fps**（49–74 帧/5.0 s），host 191 帧，UI 305 K。
新 lib `95607e…` + fast 的组合已把 guest 端大幅改善；需要 code 确认 565 开关
为何没打上（guest boot-arg 名/解析没接）。**

## Recipe

- root `/tmp/root-w15-565.apfs` `aabb30e9…`（785bcbac + lib `95607e10…`）；
- host `01c5fc50…`（565 版）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_565=1"`。

## Numbers

- **565 未生效**：host `rgb565` = 0 行；guest `upload565` = 0 行；
  guest 仍 `upload 4872000 B in 977–1516 ms`（5.0/3.2 MB/s）。
- guest `MiniIOMobileFramebuffer`：**49–74 帧/5.0 s ≈ 10–14 fps**，
  `swaps` 27–74。
- host 总帧 **191**；UI 截图 305 K（01:54）。
- 对比：fast1（lib e06c91）窗口 0.18 fps；本发 lib 95607e 的 guest 10–14 fps，
  但 host 仍 191。

## 判断

- 565 没打上，本发不能作为 565 判据；但 **guest 10–14 fps 是新 lib 95607e +
  fast 的正向信号**（比 fast1 的 0.18 大幅改善）。
- 需要 code 查 565 开关未生效的原因（`glwire_upload_565` 是否被解析）；
  host `rgb565` 0 行也说明 host env 没进 565 分支。
- host 191 帧仍是独立问题（与 fast1 的 382 同量级）。

Artifacts: `/host/iosvm/logs/s20-5651/`.

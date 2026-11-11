# Stage 20 fast1 — 全帧 upload fast 路径（direct surface rows）

**Verdict: fast 路径生效（`upload fast path (direct surface rows)`）但
**没有缩短 upload 时间**：715–1543 ms（中位 886 ms / 5.5 MB/s），窗口 0.18 fps。
说明 740 ms 不是 host 行拷贝，**剩余瓶颈是 QEMU TCP 传输 4.9 MB**。**

## Recipe

- root `/tmp/root-w15-fast.apfs` `bcc36605…`（785bcbac + lib `e06c9197…`）；
- host `c9f357c8…`（fallback 已回退）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16"`（无 scale/dirty/ack）。

## Numbers

- upload：`upload fast path (direct surface rows)`；381 次 `4,872,000 B in
  715–1543 ms (中位 886 ms / 5.5 MB/s)`（基线旧路径 ~739 ms / 6.6 MB/s → 无改善）。
- `f 1 wait`：0.82–1.19 s（比基线 ~830 ms 略慢）。
- 窗口：`ui=660s`，`120 frames in 662.04s -> 0.18 fps`（host 143→382，guest 15→143）。
- host 382 帧（未卡 12/13，但也没 >600）；UI 截图 313 K（01:09）。
- guest `MiniIOMobileFramebuffer` 2–3 帧/23–188 s（0.01–0.13 fps，饥饿）。

## 判断

- fast 去掉中间缓冲/二次拷贝，但 upload 仍 715–1543 ms → **瓶颈在 QEMU TCP
  的 4.9 MB 传输**（中位 5.5 MB/s），不是行拷贝。
- 按 code 的判据：仍 ~700 ms 级别 → 下一改 RGB565/主机侧转换，不再动 row loop。
- 本发不计入正收益；对比：基线 739 ms/6.6 MB/s、fast 中位 886 ms/5.5 MB/s。

Artifacts: `/host/iosvm/logs/s20-fast1/`.

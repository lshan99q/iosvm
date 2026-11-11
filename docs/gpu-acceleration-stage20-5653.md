# Stage 20 5653 — RGB565 双侧生效（bootarg 2048 + host 565）

**Verdict: 565 真正生效。** host `rgb565 sends 2436000 of 4872000 picture B`
（329 次），guest `upload565 2436000 B in 479–1135 ms`（380 次，中位 547 ms /
4.5 MB/s）—— 字节减半、单帧上传时间也大约减半（旧 977–1516 ms → 中位 547 ms）。
但 host 只 **329 帧**、guest 0.08–0.5 fps（饥饿），端到端仍远未到 ≥3 fps。

## Recipe

- root `/tmp/root-w15-5653.apfs` `4be95c75…`（785bcbac + lib `58e0ce2f…`）；
- host `d17265ae…`（含 `rgb565=1`）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_565=1"`。

## Numbers

| 项 | 值 |
|---|---|
| host rgb565 | 329 次 `2436000 of 4872000 B` |
| guest upload565 | 380 次 `2436000 B in 479–1135 ms`（中位 547 ms / 4.5 MB/s） |
| 字节 | **2.44 MB（减半）** |
| 单帧上传时间 | **中位 547 ms**（旧 977–1516 ms，约减半） |
| host 帧 | 329（不到 600） |
| guest 帧 | 2–5 帧/9.7–24 s（0.08–0.5 fps，饥饿） |
| UI | 287 K @03:11 |

## 判断

- 565 生效且字节/时间都减半，验证 payload 减半方向；但 **TCP 吞吐仍只有
  ~4.5 MB/s**，2.44 MB 仍要 ~550 ms → 端到端 fps 仍 <1。
- host 329 帧（不是 12/13 停，但也不到 600）与 guest 饥饿并存，还需要
  guest/协议侧的流水线优化，不能只靠 565。
- 565 截图 287 K 已出；颜色量化对比待与全彩基线做数值（±8/255、无整块错色）。

## 系列对比

| boot | payload | 上传时间 | guest fps | host 帧 |
|---|---|---|---|---|
| fast1（BGRA+fast） | 4.87 MB | 中位 886 ms / 5.5 MB/s | 0.18（窗口） | 382 |
| 5651（BGRA，565 未生效） | 4.87 MB | 977–1516 ms | 10–14（静止段） | 191 |
| **5653（RGB565 双侧）** | **2.44 MB** | **中位 547 ms / 4.5 MB/s** | 0.08–0.5 | 329 |

Artifacts: `/host/iosvm/logs/s20-5653/`.

## 饥饿归因（owner 口径）

- 对比 `s20-5651`（565 未生效，BGRA 4.87MB）：guest 静止段 **10–14 fps**；
  本发 `s20-5653`（565 生效，2.44MB）：guest **0.08–0.5 fps**。
- 结论：**565 的同步转换卡住了渲染线程**（不是 wire 变慢）。565 省了带宽，
  但 guest 在 upload 路径里同步展开/转换，帧线程被它拖住。
- **wire 上限**：实测 4.5 MB/s；2.44 MB 全帧 → 理论天花板
  **~1.8 fps**（2.436/4.5 ≈ 0.54 s/帧）。即使转换不阻塞，也到不了 ≥3 fps，
  除非进一步压缩或异步。

## 下一轮（等 code 两个开关）

1. **异步 565**：判据 guest 回到 10–14 fps 量级、host 持续出帧；
2. **565 + 压缩**：判据 packed 后字节 ≥3x 压缩比、host 端到端 **≥3 fps**
   （阶段底线）、截图色差给数值、cell1 10.91 不变；
3. 若压缩后仍上不去：给「需要吞吐 vs 实测吞吐」对比，不硬凑。

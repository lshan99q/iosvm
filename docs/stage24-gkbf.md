# stage 24：gkbf=1 有效负结果 —— 帧节拍不是 CA，是客机 display 驱动的全屏 blit

2026-11-17 14:09Z 实测，remote-only + 全屏补丁 + host `GLWIRE_ACK_ONLY=1` + guest `glwire_ack_only=1` + `gkbf=1`（唯一变量）。
root `aeff0a62ae882bee821f9a1d0f35dc0e`（= ackonly3 同一 root，lib 同）、system `49ae3cf4fd53334b475a921eb9915008`；日志 `logs/s24-gkbf2/`。

| 指标 | ackonly3（gkbf=0，基线） | gkbf2（gkbf=1） |
|---|---:|---:|
| fps（末 300 帧，帧时间戳算） | **8.04** | **6.86** |
| 帧间隔中位 / p90 | 116 / 182 ms | 123 / 208 ms |
| 宿主回放 CPU（约） | 41 ms/帧（panel 35.8） | **1.4 ms/帧** |
| records/帧 | ~90 KB | **4–19 KB** |
| 客机 display 驱动自报（`us each in the driver`） | 61–79 ms | **69.7–71.5 ms** |
| 画面（`/dev/shm` 快照） | shm.png | 与基线 **sha256 完全相同** |

结论（有效负结果）：`gkbf=1` 把 CA 记录的 GL 工作量砍到 1/5–1/30（宿主从 41 ms/帧掉到 1.4 ms/帧），
**帧率完全不动**（6.86 vs 8.04，本发宿主负荷更重）。所以帧节拍不是 CA 记录、也不是宿主回放决定。

新的定性（修正 handover 的归因）：交接单里那个「CA 71.0 ms」实测就是
`MiniIOMobileFramebuffer::drawLayer()` 的全屏 blit —— 750×1624 BGRA 逐像素走 `fColumn` 表，
**70 ms 稳**，切 gkbf 前后一模一样。它才是唯一没动过的大块。
证据：`grep 'us each in the driver' logs/s24-gkbf2/serial.log`（5/s, 69.7–71.5 ms）+
`swap ... from 0,0 750x1624 to 0,0 750x1624`（每帧都是 1:1 全屏）。

下一步：给 `drawLayer()` 加 1:1 整块/按行 memcpy 快速路径（已派 code）。
预期 70 → 10 ms 级，帧节拍 ≈ 70（余量）→ 目标 ≤91 ms 可达；否则再拆 swap 内部的 record/CA 两段。

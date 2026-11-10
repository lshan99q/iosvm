# Stage 20 payload scale=4 — 1/16 面积对照

**Verdict: guest 5.3–5.9 fps（1/16 面积继续上升），但 host 这一发卡在 12 帧
（guest 的 `f 1 wait … sent 0 rows`，host 回放没跟上），是新的 host 侧现象，
不是 guest 限流。**

## Recipe

- root `/tmp/root-w15-payload.apfs` `ecc18d68…`（785bcbac 配置 + lib 27f627）；
- system `8bf5f838`；host `fd6667ad…`（`upload scale 4`）；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_scale=4 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_SCALE=4"`。

## Numbers

| scale | 上传字节 | guest fps | f 1 wait | host |
|---|---|---|---|---|
| 1（原） | 4,872,000 | ~1.2 | ~830 ms | 1.46–1.60 fps |
| 2（1/4） | 1,218,000 | 2.0–3.4 | 206–354 ms | 2.56–4.08 fps |
| 4（1/16） | 303,688 | **5.3–5.9** | **4.4–5.3 ms** | **卡在 12 帧**（`sent 0 rows`） |

- guest：`upload scale 4 (1/16 of the picture)`；`MiniIOMobileFramebuffer`
  27–30 帧/5.1–5.3 s。
- host：`upload scale 4 sends 303688 of 4872000 picture B`；帧 11/12 的
  interval 0.03–0.04 s（26.9–32.5 fps），但总数停在 12；guest 后续
  `f 1 wait … sent 0 rows`（guest 侧没有新行可送，host 也没新帧）。
- 趋势：面积 1/4→1/16（4x）时 guest 2.5→5.5（~2.2x），**不是严格线性**；
  `f 1 wait` 从 200 ms 级降到 5 ms 级（约 40x），说明拷贝/序列化确实是主项，
  但 guest 的 CA/display 固定开销开始占主导（拐点）。
- UI：本发无截图（scale=4 实验用；且 host 卡 12 帧）。
- cell1 护栏：P4 10.91 fps 未动。

## 判断

- payload 是主瓶颈：1/4→1/16 后 guest 已到 5.3–5.9 fps；`f 1 wait` 5 ms 级。
- 2→4 不严格线性：`f wait` 已接近 0，guest 的固定开销（CA/display/present）
  成为下一层上限；host 卡 12 帧是 host replay 的独立问题（`sent 0 rows`）。

Artifacts: `/host/iosvm/logs/s20-payload4/`.

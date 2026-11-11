# s20 dirty scroll2 — fallback 版 host 的滚动窗口（回归）

**Verdict: fallback 版 host 在滚动窗口反而更差：host 到 frame 113 后停
（poll idle 664 s），guest 4/486 s（饥饿）。比未 fallback 版（639 帧）倒退。**

## Recipe

- root `/tmp/root-w15-dirty.apfs` `13cf2350…`（785bcbac + lib 35d51b50）；
- host `58d5b876…`（fallback 版，`strings` 含 `dirty fallback ... -> full R`）；
- system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_dirty_upload=1 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16"`（无 scale）。

## Numbers

- host 到 **frame 113** 后停（frame 1–113 都有完整 frame 行；之后
  `poll idle` 664 行）；`dirty fallback 312/312 tiles, 4,872,000 B -> full R`
  只在首帧出现；之后 `dirty upload 0/312`、`8/312`（131,072 B）。
- guest `MiniIOMobileFramebuffer` 4/486 s（饥饿）。
- 对照：未 fallback 版 host `c9f357…` 同一滚动窗口 639 帧、guest 0.72 fps。
  **fallback 版把 host 从 639 帧打回 113 帧。**
- 同一 fallback host 的静止 UI 发正常（2342 帧、guest 4.6–5.0、dirty 1–2 tiles）。

## 判断

- fallback 版 host 在滚动/大脏区路径**回归**（113 帧后停），静止路径正常。
- 需要 code 查 `dirty fallback ... -> full R` 之后的 host 路径（为何在 113
  帧停、poll idle 664 s）；本轮不再调 tile。

Artifacts: `/host/iosvm/logs/s20-dirty-scroll2/`.

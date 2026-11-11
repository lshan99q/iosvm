# s20 dirty idle — 静止 UI（不滚动、dirty=1、无 scale）

**Verdict: 静止 UI 达到 ~5 fps，dirty 只有 1–2 tiles/16–32 KB，host 2342 帧正常，
UI 321,741 B（630 s）。这是 dirty 的正确收益场景。**

## Recipe

- root `/tmp/root-w15-dirty.apfs` `13cf2350…`（785bcbac + lib 35d51b50）；
- host `58d5b876…`（fallback 版）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_dirty_upload=1 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16"`（无 scale）；
- 静止（不开 scroll）：boot-measure，`ui=630s`。

## Numbers

- `xb: end=ui ui=630s ui_bytes=321,741 hostframes=2342`；无 crash。
- host 总帧 **2342**（不再 12/13 停）。
- guest `MiniIOMobileFramebuffer` 23–25 帧/5.0–5.1 s ≈ **4.6–5.0 fps**。
- dirty（静止段）：`1 tile/16,384 B`、`2 tiles/32,768 B`、`3–4 tiles/49–65 KB`
  （0.3–0.7% 面积）。
- 首帧 fallback 正常：`dirty fallback 312/312 tiles, 4,872,000 B -> full R`。
- 截图 323 K；UI 630 s。

## 判据

| 判据 | 结果 |
|---|---|
| 静止 UI ~5 fps | **通过**（4.6–5.0） |
| 脏区小（0/1/16KB 量级） | **通过**（1–2 tiles/16–32 KB） |
| host 持续出帧 | **通过**（2342 帧） |
| 截图无陈旧 | 截图 323 K；待看图 |

## 判断

- **dirty 在静止 UI 上成立**：约 5 fps，脏区 0.3–0.7%，host 稳定。
- 滚动场景仍是全屏脏（上一发 310/4.8 MB），fallback 版 host 的滚动验证见
  `gpu-acceleration-stage20-dirty-scroll2.md`（回归）。

Artifacts: `/host/iosvm/logs/s20-dirty-idle/`.

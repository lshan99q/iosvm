# Stage 20 dirty1 — DIRTY_UPLOAD 一发（cell3，ack=0，不开 scale）

**Verdict: host 停帧修好了、UI 起来了、静态脏区极小（1 tile/16 KB），但滚动
窗口的脏区几乎是全屏（310 tiles / 4.8 MB），guest 只有 0.72 fps —— 滚动场景
dirty 没有带来收益；静止场景接近纯本地。**

## Recipe

- root `/tmp/root-w15-dirty.apfs` `13cf2350…`（785bcbac 配置 + lib `35d51b50…`）；
- host `c9f357c8…`；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_dirty_upload=1 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16"`（无 UPLOAD_SCALE）。

## Numbers

- `drive: ui=680s frame-window 120 frames in 167.33s -> 0.72 fps`
  （host 397→639，guest 163→256）；host 总帧 **639，不再 12/13 停**。
- UI 截图 316 K（23:47 出图）。
- dirty 序列（host/guest 完全对应）：
  - 首帧 `312/312 tiles, 4,872,000 B`；
  - 静止段：`0 tiles / 0 B`、`1 tile / 16,384 B`、`3 tiles / 49,152 B`；
  - 滚动段：`48/768,000 B`、`310/4,839,232 B`、`134/2,048,320 B`、`298/4,652,864 B`
    —— 滚动时几乎每 tile 都变，等价全帧。
- guest `MiniIOMobileFramebuffer` 3–5 帧/5.5–11.5 s（0.4–0.6 fps），窗口 0.72 fps；
  基线（全帧无 dirty）为 1.2–1.23 fps。

## 判据

| 判据 | 结果 |
|---|---|
| guest ≥5 fps | **不通过**（0.72 fps） |
| 截图无陈旧块 | 截图已出（316 K）；滚动期间 dirty 覆盖全屏，理论无陈旧；待看图 |
| host 不再 12/13 停 | **通过**（639 帧） |
| cell1 10.91 不变 | 未受影响（未跑 cell1） |

## 判断

- **host 停帧的根因在旧的整帧 stride/上载路径**：dirty 路径 host 639 帧正常。
- **静态 UI 脏区极小**：1 tile/16 KB（0.3%），接近纯本地。
- **滚动窗口仍是全屏脏**：310 tiles/4.8 MB，guest 0.72 fps，没有收益；滚动时
  整屏内容都在变（滚动 + 状态栏/时钟），tile 命中率低。
- guest 0.72 比全帧基线 1.23 还低，可能是 dirty 的 64x64 memcmp/patch 开销 +
  这发本身慢；需再对照。

Artifacts: `/host/iosvm/logs/s20-dirty1/`.

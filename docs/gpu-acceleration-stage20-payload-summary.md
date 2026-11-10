# Stage 20 payload 面积-帧率总结（scale 2/4 + uploadscale）

## 面积-帧率表（实际字节，不是名义面积）

| 开关 | 上传字节 | guest fps | `f 1 wait` | host | 结论 |
|---|---|---|---|---|---|
| 默认 1（全帧） | 4,872,000 | ~1.2 | ~830 ms | 1.46–1.60 fps | 基线 |
| scale=2（左上 **1/4**，375×812） | 1,218,000 | **2.0–3.4** | 206–354 ms | 2.56–4.08 fps | **≥2 ✓ payload 瓶颈确认** |
| scale=4（左上 **1/16**，187×406） | 303,688 | **5.3–5.9** | **4.4–5.3 ms** | **只出 12 帧卡住** | guest 收益成立，host 未修好不算数 |

> 注：scale=2 是 1/4 面积（1,218,000 / 4,872,000 B），scale=4 是 1/16（303,688 B）。
> 用户口径里的「1/2 面积 → 2–3.4 fps」按实际字节应读作 1/4 面积。

## host 卡死那一发的证据（scale=4）

- `host_glwire: frame 11/12` 之后不再有 frame 行；帧间隔 0.03–0.04 s
  （26.9–32.5 fps）突然停住，总帧数 12。
- guest 后续 `hakios-remote: f 1 wait … sent 0 rows`（guest 侧没有新行可送），
  guest `MiniIOMobileFramebuffer` 仍 27–30 帧/5.1–5.3 s（5.3–5.9 fps）。
- 即：guest 渲染/上载在跑，host replay 端停了 —— **host 侧 bug**，不是
  payload 收益虚高。证据：`logs/s20-payload4/host.log`、`host-upload-lines.txt`。

## 判断

- **payload/拷贝是主瓶颈**：面积 4.87 MB → 1.22 MB（1/4）guest 从 ~1.2 升到
  2.0–3.4 fps；`f 1 wait` 从 ~830 降到 206–354 ms；host 仍 ~24 ms/帧。
- **1/4 面积（303,688 B）理论可达 5–6 fps**（guest 已实测 5.3–5.9），
  **但 host 修好前不算数**。
- 面积 1/4→1/16（4x）时 guest 2.5→5.5（~2.2x），**非严格线性**；`f wait` 已
  接近 0，guest 的 CA/display/present 固定开销成为下一层上限（拐点）。
- 后续：等 code 的 `GLWIRE_DIRTY_UPLOAD`，在滚动窗口里跑脏区上载；判据
  guest ≥5 fps + 截图内容正确（陈旧块）+ host 帧数正常 + cell1 10.91 不变。

## 归档

`logs/s20-payload2/`、`logs/s20-payload4/`、`logs/s20-uploadscale/`
（code 的 host `fd6667ad…` / lib `27f627af…` 产物）。

# Stage 20 ack 三发收口 — ack 不是限流点

## 结论

**ack 不能提升客机帧率**：ack=0 基线本来就是 **1.23 fps**（`s20-acksteady0`，
UI 600 s，guest 10→12）；ack=50 改到 arm-after-5 后 guest 到 **~1.3 fps**
（`s20-ack5`，5–8 帧/5–6 s，host 1254 帧/1.5 fps），但 **UI 没出图**（30 min
无截图）。后者是 **独立的 UI 判定失败**，不能混进 ack 结论。

下一步不是 ack，而是**每帧 4.9 MB 的 payload 拷贝/序列化**——看面积缩小
是否让 guest 帧率上去。

## 三发数据

| boot | 开关 | UI | 窗口/帧率 | guest | host | ack 行 |
|---|---|---|---|---|---|---|
| s20-acksteady0 | ack=0（lib 3160eed） | **600 s ✓** | 120/97.87 s = **1.23 fps** | 10→12 | 371 | **0** |
| s20-acksteady50 | ack=50（lib 3160eed） | **none** | — | 2/19–28 s | 3 | 5（armed after 1，dropped 1/3、2/3） |
| s20-ack5 | ack=50（lib 2f8fbc，ARM_AFTER=5） | **none**（独立问题） | — | 5–8/5–6 s (~1.3 fps) | 1254（1.5 fps） | 6（armed after 5、3/3、disabled） |
| （参照）isolate785 | 无 ack / 原 lib | 600 s ✓ | 120/152.76 s = 0.79 fps | 14→117（103） | 366 | 0 |

- ack=0 的 A 与原始 lib 的 785 isolate 同为「UI 正常、~0.8–1.2 fps」；ack 不在
  这条路径的瓶颈里。
- ack=50 的 arm-after-5 修复有效（首 5 帧完整、3 次 drop 后 disabled、guest
  不再饥饿），但 UI 仍无截图 —— 记为 **UI/present 判定失败**，另案。
- 归档：`logs/s20-acksteady0/`、`logs/s20-acksteady50/`、`logs/s20-ack5/`。

## 下一发（等 code 的 GLWIRE_UPLOAD_REGION/scale）

- 同 cell3 root、同 host env，只切面积（1/4）；判据：**guest ≥2 fps = payload
  是瓶颈**；记每帧上传字节、host 帧率、cell1 10.91 fps 护栏。
- 若 ≥2 fps，再做 1/16 验证线性。

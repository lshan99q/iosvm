# Stage 20 payload4 stridefix — eagl.c 行距补丁后 scale=4 复验

**Verdict: 行距补丁未解决 host 停帧；host 仍停（13 帧），guest 4.0–4.1 fps。**
新 lib `811e826d…`（旧 `27f627af…`），root `9b401d22…`；其余同上一发
（785bcbac 配置 + scale=4 + ack=0 + poll16）。

## 判据

| 判据 | 结果 |
|---|---|
| A：host 帧不卡 12 | **不通过** —— 共 **13** 帧（frame 12/13 后停，间隔 0.02 s / 41 fps 突然中断） |
| B：guest ~5–6 fps | **不通过** —— 4.0–4.1 fps（21–22 帧/5.4 s；上一发 5.3–5.9） |
| C：截图不花 | 无截图（UI 未起，无法判） |

## 证据

- guest：`upload scale 4 (1/16 of the picture)`；`MiniIOMobileFramebuffer`
  21–22 帧/5.4 s（4.0–4.1 fps）；`f 1 wait` 5.7–8.2 ms，`sent 0 rows`。
- host：`upload scale 4 sends 303688 of 4872000 picture B`；frame 12/13
  24 ms（40.97/41.60 fps）后不再出帧。
- 新 lib：`811e826d73f15076c07717909f44169e`（401,808 B）；旧 lib
  `27f627afff539eb537c76aa4505f3d7c`（401,776 B）；root `9b401d22…`。

## 判断

- eagl.c 的 `copy_width % scale` 行距修复**没有**让 host 恢复；host 仍停在
  12→13 帧，说明 host 停帧的根因不是（只）guest 行距，或该补丁未覆盖到实际
  上载路径。guest 也从 5.3–5.9 降到 4.0–4.1（可能与行距变化有关）。
- 需要 code 再确认补丁命中的路径，以及 host `client closed`/停帧的真实原因；
  本发不计入有效 payload 收益。

Artifacts: `/host/iosvm/logs/s20-payload4-stridefix/`.

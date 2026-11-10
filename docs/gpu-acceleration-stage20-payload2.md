# Stage 20 payload scale=2 — 1/4 面积对照

**Verdict: guest 2–3.4 fps（≥2）成立 → payload/拷贝是瓶颈。** 同 cell3
785bcbac root + new lib `27f627` + host `fd6667`，只把上传面积切到左上 1/4
（host/guest 两侧 scale=2），ack=0。

## Recipe

- root `/tmp/root-w15-payload.apfs` `ecc18d68…`（785bcbac 配置 + lib 27f627）；
- system `8bf5f838`；host `fd6667ad…`（`upload scale 2`）；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_scale=2 glwire_ack_ms=0"`；
- `HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_SCALE=2"`。

## Numbers

- guest：`upload scale 2 (1/4 of the picture)`；`MiniIOMobileFramebuffer`
  10–17 帧/5.0–5.4 s ≈ **2.0–3.4 fps**（基线 0.05–1.2）。
- host：`upload scale 2 sends 1218000 of 4872000 picture B`；frames 23.6–24.4 ms，
  interval 2.56–4.08 fps（回放仍 ~24 ms，未变）。
- guest `hakios-remote: f 1 wait`：**206–354 ms**（原 ~830 ms；<400 达标）。
- UI：本发无截图（scale=2 只更新左上 1/4，实验用途，不做 UI/P0 验收）。
- cell1 护栏：P4 10.91 fps 未动（payload 开关不作用于 cell1）。

## 结论

- 面积 4,872,000 → 1,218,000 B（1/4）后 guest 从 ~1.2 升到 2–3.4 fps
  → **每帧 4.9 MB 的拷贝/序列化是瓶颈**（ack 已证不是）。
- 下一步：scale=4（1/16）看是否线性继续上升。

Artifacts: `/host/iosvm/logs/s20-payload2/`.

# Stage 21 P1 scrub 61-63 — 未触发（判死）

**Verdict: `GLWIRE_LABEL_ROW_SCRUB=61-63` 已开（host env），但 `cleared 178x90`
0 次、`glCopyTexSubImage2D`/tile mapping 行也 0 次。加上第一发（默认，mapping
0），P1 两发都没拿到 tile 行映射/清理 → 按用户规则 P1 判死，不再起 boot。**

## Recipe

- root `/tmp/root-w15-rle10.apfs` `a3fec104…`（lib `3a42016b…`）；
- host `85794fff…`（78c135 gate + P1 代码）；system `8bf5f838`；
- `XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=0 glwire_upload_rle=0 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0"`；
- 第二发 `HOST_ENV="... GLWIRE_LABEL_ROW_SCRUB=61-63"`。

## Numbers（两发）

| 发 | 设置 | mapping 行 | cleared 178x90 | guest | host | 截图 |
|---|---|---|---|---|---|---|
| 1 | 默认 | **0** | 0 | 2/26 | 127 | 302K |
| 2 | scrub 61-63 | **0** | **0** | 3/8.5 | 3 | 无 |

## 判断

- 85794 的 P1 代码在 `dispatch.inc:66` 的 `glCopyTexSubImage2D` 上打映射行，
  但两发都没出现 → 这条 tile copy 路径实际没有执行（可能 750x200 层走别的
  blit/合成路径，或 `sent`/`flush` 路径不同）。
- 按用户规则：**P1 判死，不再起 boot**。需要 code 确认 `glCopyTexSubImage2D`
  是否真的是标签 tile 的实际路径（当前证据显示不是）。
- 第一发截图 `logs/s20-p1-map/shot.png`（302K）作为现行 UI 证据。

## 预算

- P1 两发已用完（默认 + scrub 61-63），均未触发 → 不再起 boot。
- 可选 fbscale=1.5 试过一发：host 0 帧、无 UI（疑似 fbscale 1.5 不被支持），
  未形成性能结论，已停。

Artifacts: `/host/iosvm/logs/s20-p1-map/`、`/host/iosvm/logs/s20-p1-scrub/`.

## P1 真实路径（code 11:32 确认）

- 178x90 tile **不是** `glCopyTexSubImage2D`；真实入口是 `host_glwire.c:387
  host_legacy_teximage()`，日志 `glwire: legacy texture 178x90 internal 0x1906
  format 0x1906: taken natively`（`:436` native upload）。
- 正确 scrub 已放在 legacy alpha 上传前（同一 `GLWIRE_LABEL_ROW_SCRUB=61-63`，
  默认关）：`glwire: legacy 178x90 scrub rows 61-63 before native alpha upload`。
- **P1 两发预算已用完，判死；不再 boot**，除非 lead 重开。
- host 最新 `6aa395ce…`（132,720 B）；565-only 默认路径不受影响
  （P1/RLE 默认关）。壁纸静态兜底 workaround 仍不成立。

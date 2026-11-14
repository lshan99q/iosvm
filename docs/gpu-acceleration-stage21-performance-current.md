# Stage 21 性能现状表（收口，2026-11-11）

## 结论链（RLE，rle3–rle13）

| boot | 变化 | 关键结果 |
|---|---|---|
| rle3 | unpack 算法修复 | **unpack 396–451 → 84–105 ms**（达标）；guest 仍 3/10.2 |
| rle4 | display 修复 | 仍 3/7.7；surface2 黑 |
| rle5 | skip empty | skip 3 次、bgra rle 0；仍 4/505 |
| rle6 | texture fallback | fallback 0；仍 3/9.3 |
| rle7 | 201 顺序修复 | fallback 2 次，但 packed **51,202 B (1%)**（黑） |
| rle8 | nonblack 缓存 | `stored nonblack` 0；仍 51 KB |
| rle9 | **565+RLE** | 2-byte 路径正确，**30,554 B (1%)** 黑 |
| rle10 | 移除黑拷贝 | s11 → surface 1（部分 `ff00ff00`）；仍 30 KB |
| rle11 | guest surface2 | 路径生效，segments2 仍黑（1%） |
| rle12 | image1 texture | tracked 1 次、fallback image1 **0**（顺序） |
| rle13 | image1 优先 | **fallback image1 1 次**（选中），但仍 1%（被 glReadPixels 覆盖） |

**判定：RLE 接线未收敛（13 发）。** 解压性能与压缩比本身达标；黑图根因在
host 源缓冲选择/覆盖（image0 黑、fallback 被 readback 覆盖），**RLE 源缓冲
不是 565 发送的那块**。若 code 最后一发（6ea0fa）仍 skip/黑，按用户的规则
**RLE 判死，565-only 收口**。

## 性能数字（按模式分开，2026-11-14 修正）

> **产品路径 = 默认路径**：`glwire_local=1`、**不带 `GLWIRE_NO_PANEL=1`**、guest 合成 panel。
> `GLWIRE_NO_PANEL=1` 是**实验模式**（host 回放记录、不画 guest panel），画面缺
> dock/Search 右半/右下，**不能当产品性能或产品画面**。

| 模式 | 场景 | 画面完整性 | 数字 |
|---|---|---|---|
| **产品默认** | 稳态 | **完整**（dock 通栏、Search 居中、右下不黑） | **2.0–3.3 fps**；ext37 3823 帧；ext38 3480 帧 / 1700 s ≈ **2.05 fps 平均** |
| **产品默认** | 启动 R4 | 完整 | 慢窗口 f3 **503 s**、f4 **70 s**、f5 **25 s**、f6 3.9 s、f7 4.3 s；f8 起 0.49 s（2.03 fps） |
| 实验 `NO_PANEL=1` | 静止/dirty | **缺** dock/Search 右半/右下 | 4.6–5.0 fps（host 回放记录，不是产品） |
| 实验 `NO_PANEL=1` | 滚动 | **缺** | ~1.8 fps（565-only；不是产品） |
| 实验 `NO_PANEL=1` | 纯本地 cell1 | 非产品路径 | 10.91 fps（仅供实验对照） |

- **产品路径稳态约 2–3.3 fps**；ext37/38 都是 `hostframes MATCH=all, DIFF=0, panic=0`。
- 之前记的 **4.6–5.0 fps / 10.91 fps 是 `NO_PANEL=1` 实验模式**，不能当产品性能。
- 慢窗口 f3=503 s 是**启动期**（f3 前 guest 还在 boot、没出画面），不是渲染 bug；
  真正可用性问题是 **R4：启动后前约 10 分钟极慢**。
- 565 生效、字节减半（2.44 MB）；RLE 判死；瓶颈仍是 wire/启动，不是 host 回放。

## 归档

`logs/s20-rle3` … `logs/s20-rle13`、`logs/s20-dirty-idle`、`logs/s20-5653`、
`logs/s20-payload2`、`logs/s20-payload4`；system `8bf5f838` 未覆盖。

## RLE 最终判定（2026-11-11 10:19，code 确认）

- **RLE 判死**：image1 texture 被选中，但输入断言 `nonblack=0.00%`；FBO/
  panel/segments2/image1 四个源全黑，host 源已穷尽。按规则不再跑 RLE。
- 收口：性能线用 **565-only**（`GLWIRE_UPLOAD_565=1` / `glwire_upload_565=1`），
  证据 2,436,000 B、329 host 帧、UI 正常。
- code 侧：RLE 代码保留但默认关（`rle_wanted=0` 不执行任何 RLE fallback/assert），
  默认 565 路径恢复无条件 glReadPixels，逐字节等价；e2e default PASS。
- host 收口版 `78c135e2…`（132,592 B）；guest lib 仍 `3a42016b…`；branch 1723b7f。
- 后续若还优化，是 guest present/frame_end 写 surface，不是 payload codec。

## 下一阶段

- 后续性能工作见 **stage 23**：`docs/virtio-gpu-stage23-plan.md`（virtio-gpu 传输墙 + TCG 优化翻译墙）。

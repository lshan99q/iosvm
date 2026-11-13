# 已知渲染缺陷清单（待修复）

按主人要求：**先记录，不打断当前任务；当前任务（stage 22：extension check-in / widget 渲染）收口后再修。**

| id | 现象 | 影响 | 状态 | 证据 |
|---|---|---|---|---|
| **R1** | **dock + Search 胶囊只渲染左半**：GL 路径下，桌面底部这条带在 x≈375/750（面板坐标，截图像素 x≈562/1125）处**垂直切断**，右侧全黑；dock 只剩 2 个图标（Phone、Safari），Search 只剩「Se…」 | 桌面观感明显缺损；不是扩展相关，任何一次 boot 都在 | **待修复**（当前任务结束后处理） | `ext24-last.png`（坏）、主人提供的 CPU 渲染图（好，dock 通栏 4 图标） |
| **R2** | **壁纸纯黑** | 桌面背景空白 | 已知（P2 未达成）；扩展 check-in 已能发生，是否随之可解待验 | stage 19/20 P2 记录 |
| **R3** | 状态栏有红色 `rdar:45025538` 调试条 | 观感 | 已知（调试 overlay） | 所有截图 |

## R1 的进一步定位：**右下象限整块没被渲染/上传**（2026-11-13 复查）

把两张截图按象限统计非黑比例（阈值 sum>30）：

| 截图 | 左上 TL | 右上 TR | 左下 BL | **右下 BR** |
|---|---|---|---|---|
| ext24（`/tmp/ext24-last.png`） | 60.1% | 60.1% | 43.7% | **0.8%（几乎全黑）** |
| ext26（`/tmp/ext26-last.png`） | 69.8% | 70.6% | 56.8% | **100% 纯绿（未初始化）** |

- 象限边界：x≈562.5（=1125/2）、y≈1218（=2436/2），换算到面板坐标正好是 **375 / 812**（面板 750×1624 的一半）；
- 所以 **R1 不是「dock 层画错」，而是「右下象限整块没进最终画面」**——ext24 里它是黑的（被清过），ext26 里是绿的（未初始化）；
- 这一条同时解释了 dock 右半、Search 右半、以及 ext26 截图里的大绿块。

待验证的假设：**帧的合成/上载按 2×2 分块，右下块被跳过**（循环边界 off-by-one、或脏区列表只到 3/4、或最后一块的拷贝条件写错）。这是本次修复的第一靶点。

## R1 的 ext27 诊断（2026-11-13，排除附件尺寸）

- **panel FBO 附件 = 750×1624 全幅**（不是小附件）；
- **draw viewport = 0,0 750x1624**，**scissor 关闭**；
- `GLWIRE_SAVE_RECORDS` 存了 **3 帧 records**（`/tmp/r1-*.records`），待 code 离线分析；
- 另记：texture 上传 `row 3072`（768 px 填充，比 750 多 18 px），**stride 疑点待核**。

**结论：R1 仍是「待修复」；「附件尺寸不足」这一假设已排除。** 下一步看 records
（3 帧）和 768 vs 750 的 stride/边界，确认右下象限是在合成、上载还是读取时丢的。

## R1 的初步定位（原始记录，保留）

- **CPU 渲染路径是完整的**（主人提供的图：dock 通栏、Search 居中、图标 4 列铺满）。
- **GL / glwire 路径只画了左半**，且切断位置 = 面板宽 750 的一半（375），说明这条带的绘制或上载只覆盖左半。
- 之前被误判为「正常」的原因：对比基线（`s20-5653`、`s20-p1-map`）本身就是同一条坏路径的产物。**教训：渲染类问题必须用 CPU 路径或真机作基线，不能用同一条 GL 路径的历史截图。**

## R1 修复计划（当前任务后执行）

1. **对照复现**：同镜像分别 `metal=0`（CPU）与 GL 各跑一发，只 diff dock 带（y≈2000–2436）；
2. **定位层**：找 dock+Search 的 surface/layer，打印其 GL 绘制矩形与上载矩形：
   - 若矩形宽 = 375 → scissor/viewport 设错（画错）；
   - 若矩形宽 = 750 但只上载一半 → 合成/上载截断（传错）；
3. 按定位结果改最小处（guest GL 侧 vs glwire 侧），判据：**dock 通栏 4 图标 + Search 居中 + 右半不再黑**，且 P0 内容判据不回退。

## 记账约定

- 新增渲染类缺陷一律进本表，带 id、现象、证据路径、状态；
- 「同路径历史截图」不能作为正常基线（R1 的误判就是这么来的）。

## R1 代码走查（2026-11-13，只读；追加，不改渲染逻辑）

> **ext27 实测更新**：panel FBO 附件 = **750×1624 全幅**，draw viewport = **0,0 750x1624**、scissor **关闭**；因此下面 #1「附件小于全幅」的假设**已排除**，保留为代码风险记录。待查：3 帧 records（`/tmp/r1-*.records`）与 texture `row 3072`（768 vs 750）的 stride。

针对「2×2 分块、右下块被跳过」的假设，逐处检查了 glwire 的合成/上载路径，**没有找到 2×2 或四块循环**：

- `host_glwire.c` 的 dirty upload 是 **64×64 固定 tile**，`tiles[1024]`；750×1624 最多 12×26=**312** 块，循环是 `ty < height`/`tx < width`，没有 `<3`/`<=3`；
- `eagl_remote.rle5.c` 的 `remote_surface_segments` 最多 2048 段；`eagl_remote_surface()`/`surface2()` 都要求 `covered == bytes` 才走 direct，否则回退 packed picture，不可能只写 3/4；
- `host_glwire.c:3470` 的 `read_picture_rows()` 按行覆盖完整 `width*4`，越界返回 0 并回退；
- `eagl.c:1474` 的 surface 写回循环是 `for (y = 0; y < copy_height; y++)` + `copy_used = copy_width*4`，没有象限分支；
- `dispatch.inc:66` 的 `glCopyTexSubImage2D` 按原参数整幅转发；`eagl.c` 的 `glBlitFramebuffer` 也是 `image->width/height` 整幅；
- `glwire_upload_scale=2` 是 **top-left 1/4** 的负载实验，默认 1；误开它会丢 TR/BL/BR，不会只丢 BR。

### 最可能的三处（按优先级）

1. **（ext27 已排除）panel FBO 的附件小于 750×1624，readback 却按整帧读。**
   `host_glwire.c:4028` 的 `frame_readback_ok` 只检查 `viewport[2] > 0 && viewport[3] > 0`，**不检查附件实际尺寸**；`host_glwire.c:4137` 仍 `glReadPixels(0,0,width,height)`。越界区域是 undefined，正好可能表现为 BR 绿/黑。ext25 host log 已见
   `read-back framebuffer 6 ... viewport 0,0 750x200`、`reading from 2/3 instead, viewport 0,0 750x200 (panel)`，但没有附件尺寸；这是最符合「只缺 BR」的一处。
2. **最后一个 `glScissor`/`glViewport` 是 375×812**，BR 不在 scissor 内；`glScissor` 是 `!local`，由 host 执行，但 guest 记录的值可能来自 CA 的 dirty rect，需要 `glframes=1` 的 box 证实。
3. **surface2 写回/读回只覆盖第一块 panel surface 的 3/4**；`covered == bytes` 使这种路径很难悄悄发生，优先级低于 1/2。

`glCopyTexSubImage2D` 没有 2×2 分块，host/guest 都传完整 width/height；**没有证据**指向它被截成 375。

### 最小可测观测（同一发，不改代码）

```text
glframes=1
GLWIRE_FBO_LOG=1
GLWIRE_DRAW_LOG=1
GLWIRE_SAVE_RECORDS=/tmp/r1
```

- `glframes=1`：`hakios-gl: f ... box <mode> x,y wxh`，看 dock/Search 帧的 scissor box；
- `GLWIRE_FBO_LOG=1`：`glwire: fbo bind ... attachment ... WxH`，确认 panel FBO（2/3）的附件尺寸；
- `GLWIRE_DRAW_LOG=1`：逐 draw 的 fbo/program/viewport/scissor，判断 BR 是没画、被裁、还是读越界；
- `GLWIRE_SAVE_RECORDS`：离线复查该帧的 `glViewport`/`glScissor`/`glClear`/`glBlitFramebuffer`。

判据与修法分支：
- panel 附件 < 750×1624 → 修 readback 目标/尺寸检查（用完整 panel texture，或在读前断言附件 ≥ 全幅）；
- scissor box = 375×812 → 修 CA/GL 的 scissor 来源，不动 readback；
- BR 不在任何 draw/scissor → 修 CA dirty rect/合成顺序；
- FBO/draw 都完整、只有 readback 越界 → 只加 readback 的附件尺寸校验。

修复后判据固定为：**BR 非黑、dock 通栏 4 图标、Search 居中**，且 P0 内容判据不回退；**先不改代码、不起 boot**。

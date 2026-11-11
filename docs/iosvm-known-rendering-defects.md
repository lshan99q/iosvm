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

针对「2×2 分块、右下块被跳过」的假设，逐处检查了 glwire 的合成/上载路径，**没有找到 2×2 或四块循环**：

- `host_glwire.c` 的 dirty upload 是 **64×64 固定 tile**，`tiles[1024]`；750×1624 最多 12×26=**312** 块，循环是 `ty < height`/`tx < width`，没有 `<3`/`<=3`；
- `eagl_remote.rle5.c` 的 `remote_surface_segments` 最多 2048 段；`eagl_remote_surface()`/`surface2()` 都要求 `covered == bytes` 才走 direct，否则回退 packed picture，不可能只写 3/4；
- `host_glwire.c:3470` 的 `read_picture_rows()` 按行覆盖完整 `width*4`，越界返回 0 并回退；
- `eagl.c:1474` 的 surface 写回循环是 `for (y = 0; y < copy_height; y++)` + `copy_used = copy_width*4`，没有象限分支；
- `dispatch.inc:66` 的 `glCopyTexSubImage2D` 按原参数整幅转发；`eagl.c` 的 `glBlitFramebuffer` 也是 `image->width/height` 整幅；
- `glwire_upload_scale=2` 是 **top-left 1/4** 的负载实验，默认 1；误开它会丢 TR/BL/BR，不会只丢 BR。

### 最可能的三处（按优先级）

1. **panel FBO 的附件小于 750×1624，readback 却按整帧读。**  
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

## R1 ext27 追加：FBO/Viewport/Scissor 已排除，问题缩到 draw 几何/clear/readback（2026-11-13）

ext27（`glframes=1 GLWIRE_FBO_LOG=1 GLWIRE_DRAW_LOG=1 GLWIRE_SAVE_RECORDS=/tmp/r1`）拿到：

- `fbo attach ... kind 0x1702 name 1 ... 750x1624 internal 0x1908` → panel FBO 附件是**全幅**；
- `draw#1/#2/#3 ... fbo 2 viewport 0,0 750x1624 scissor off 0,0 16x16 status 0x8cd5` →
  viewport 全幅、scissor 关闭、FBO 完整；`0x8cd5` = `GL_FRAMEBUFFER_COMPLETE`；
- frame1 记录 17393 B / 3 draws / 6 triangles / **0 clears**。

因此前面最可疑的两条被排除：**不是小 FBO、不是 scissor 375×812、不是 viewport 半幅**。
**BR 纯绿 = 这块从未被任何 clear/draw 写过**（如果 3 个 draw 的顶点覆盖全屏，就不应绿）。

### `row 3072` 的核对

`upload texture ... 750x1624 ... row 3072`：3072 / 4 = **768 px**，比 750 多 **18 px padding**。
`host_glwire.c` 的 `write_picture_rows()` 每行只写 `width*4 = 3000` 字节，`glReadPixels` 也按 750 读；
`row 3072` 只是 IOSurface 的 stride，**不会让 BR（x>375）整块缺失**，最多解释右缘 18 px 的错位/花边。
所以 stride 是**次要嫌疑**，先不要按它改。

### 剩下的分支（需 records 定案）

1. **3 个 draw 的顶点几何只覆盖 TL/TR/BL**（full viewport 但顶点 quad 只有 3/4，或投影矩阵把右下裁掉）→ 修 guest CA/GL 的绘制几何；
2. **draw 覆盖了 BR，但之后被 clear/覆盖**；frame1 0 clears 使其概率低，仍需看 `glClear` 与 draw 的顺序；
3. **draw 写入 BR，但 FBO 2 的 readback 读的是另一张/未初始化 attachment**；附件已是全幅，需看 `glBindFramebuffer`/`glReadPixels` 目标；
4. **upload 只覆盖 3/4**：`remote_surface` 的 `covered==bytes` 和 `write_picture_rows` 都是全幅，概率低。

### 离线解析计划（records 到位后）

- 解析 record header（version 2：12 字节），按 `spec.def` opcode 还原
  `glViewport`/`glScissor`/`glClear`/`glClearColor`/`glBindFramebuffer`/
  `glDrawArrays`/`glDrawElements` 的 payload；
- 对 `glDrawArrays/Elements` 取 count/type，并结合 opcode 200 的顶点数组求 **x/y bbox**；
- 检查 BR（x>375,y>812）是否落在任一 draw 的 bbox 内、是否在某个 clear 之后；
- 若所有 draw bbox 都只到 375/812 → 改 guest 几何；若 bbox 覆盖 BR 但画面绿 → 改 readback/attachment 目标。

### 最小修法（按解析结果二选一）

- **几何缺 BR**：在 guest 的 CA/GL 侧修投影/quad，不碰 glwire 上传；
- **readback 目标错**：在 `host_glwire.c` 读回前断言 attachment 尺寸/identity，或强制从完整 panel texture 读；
- **stride**：若需要，host `glPixelStorei(GL_UNPACK_ROW_LENGTH, row/4)`，但这只修右缘 18 px，不修 BR。

**当前状态：records 未挂载到本容器**（`/tmp/r1-*` 与 `logs/s22-ext/ext27/` 均不存在），已请 exp 复制到
`/host/iosvm/logs/s22-ext/ext27/records/`；拿到后补最终判定。仍**不改代码、不 boot**。

## R1 records 1–3 解析结果（2026-11-13）

解析 `/host/iosvm/logs/s22-ext/ext27/records/r1-{1,2,3}-750x1624-*.records`（version 2，12-byte header；opcode 顺序按 `spec.def`）：

### 三帧都是全屏 draw

| 帧 | FBO | viewport | clear | draw | 顶点 |
|---|---|---|---|---|---|
| r1-1 | 1 | 0,0 750x1624 | **无** | draw#1 program 3, #2 program 5, #3 program 0 | 4 顶点 quad，bbox **0,0–750,1624** |
| r1-2 | 3 | 0,0 750x1624 | **无** | 3 draws（program 3/5/0） | 同上 |
| r1-3 | 1 | 0,0 750x1624 | **无** | 2 draws（program 3/0） | 同上 |

- `glDrawElements(mode=GL_TRIANGLES, count=6, type=GL_UNSIGNED_SHORT, indices=[0,1,2,2,3,0])`，四个顶点位置是
  `(0,0) (750,0) (750,1624) (0,1624)`；**BR（x>375,y>812）在这三个 quad 内**；
- `glUniformMatrix4fv`（prefix 13 字节后 16 floats）是
  `[2/750,0,0,0, 0,2/1624,0,0, 0,0,0,0, -1,-1,0,1]`，**完整正交投影**，没有把 BR 裁掉；
- `glScissor(0,0,0,0)` 且 scissor test 关闭；viewport 全幅；`glClear` **0 次**；
- vertex color 全 1（r1-1/2）或全 0（r1-3），不是绿色。

**结论：这三帧的 BR 有 draw 覆盖，且矩阵/视口/scissor 都不会裁掉 BR。**
因此 BR 绿/黑不是这三帧“没画”；要么是**后续帧只做了部分更新**（CA 的 dirty rect 不再画 BR，而 FBO 从未 clear），要么是**读回/上载**在后续路径里出错。

### stride 3072 的核对

`upload texture ... row 3072` 是 **768 px** stride（750 多 18 px padding）。`write_picture_rows()` 每行只写
`width*4=3000` 字节，`glReadPixels` 也按 750 读；**它最多解释右缘 18 px**，不解释整块 BR。不要先按它改。

### 下一步（最小）

这三个 records 是**开头三帧**（`GLWIRE_SAVE_RECORDS` 默认只存前 3 帧），不能证明结束截图那一帧。
下一发只加 **`GLWIRE_SAVE_AT=<结束前的帧号>`**（host.log 里 frame 768 附近）存**后期帧**，仍用
`glframes=1 GLWIRE_FBO_LOG=1 GLWIRE_DRAW_LOG=1`：

- 若后期帧的 draw bbox 只到 375/812，或 `glClear` 只覆盖部分 → 修 CA/渲染的 dirty rect 或补全幅 clear；
- 若后期帧 draw 仍全幅而 BR 绿 → 转 readback/upload（检查 `glReadPixels` 目标 attachment 与 `write_picture_rows` 的 row/segment）；
- 若后期帧有 `glClear` 但只清 3/4 → 查 clear 的 scissor/viewport（虽然 ext27 前 3 帧 scissor off）。

**当前不能把 R1 归因为 2×2 循环；records 证明前 3 帧是全屏 quad。** 仍不改代码、不 boot。

## R1 的决定性证据：**guest 上载的 panel surface 自己就是空的**（2026-11-13，ext30）

用 `GLWIRE_DUMP=/tmp/r1dump GLWIRE_DUMP_PAIR=1 GLWIRE_DUMP_AT=760` 在同一帧同时 dump
**guest 上载的 panel**（RGBA）与 **host 读回的 picture**（BGRA），两者 750×1624 原始像素，
按象限统计（阈值 sum>30）：

| 表面 | TL | TR | BL | **BR** | BR 平均亮度 |
|---|---|---|---|---|---|
| **panel（guest 上载）** | 57.7% | 56.8% | 43.9% | **0.3%** | **0.2** |
| picture（host 读回） | 57.7% | 58.2% | 43.9% | 0.7% | 0.7 |

**结论：host 侧无责。** guest 送上来的 panel surface 里 BR 象限就已经是黑的（平均亮度 0.2，
不是绿、不是未初始化），host 只是把它原样合成/读回。→ **R1 是 guest 侧渲染/合成没有填满 BR 象限。**

- 排除：FBO 附件（750×1624）、viewport（全幅）、scissor（关闭）、host 上载/读回；
- 下一步靶点：guest 侧「谁负责把这一帧画进 panel surface」——CA/EAGL 的合成渲染目标是否只填了 3/4；
  可按 `GLWIRE_DRAW_LOG` + 逐 draw 几何/状态（fbo、viewport、scissor、纹理来源）检查，
  或 dump CA 自己的 FBO（如果它是先把各层合成到中间 FBO 再整幅采样）。
- 副作用证据：ext26 里 BR 是绿（未初始化）而不是黑，说明该象限有时连一次清屏都没覆盖到。

### guest 侧的帧结构

ext27 的 `glframes=1` 行（1154 行）显示：

```
hakios-gl: f 152 ... 9 draws, 0 clears; display surface 0, 0 draws into it, box off 0,0 0x0;
          0 in place, 1 copied; ...
hakios-gl: f 153 ... 2 draws, 1 clears; display surface 0, 0 draws into it, ...
hakios-gl: f 154 ... 9 draws, 0 clears; display surface 0, 0 draws into it, ...
```

- **`display surface 0`、`0 draws into it`**：CA 不直接画 panel surface；panel 是 guest 把 host 的 picture 写回/合成出来的（`eagl_remote_flush` 的 surface 路径）。
- **晚帧是 `1 clear + 2/9 draws`**（不是开头三帧的 `0 clear + 3 full draws`）；
- `hakios-remote: s11 ...` 的采样行在晚帧最后几行是 `... 301e18b8 301e18b8 301e18b8 px ff000000 ff000000`：**最后一行/右侧像素是黑**，与 panel dump 一致。

### 机制判断

1. **不是「没画」**：开头 3 帧 4 顶点全屏 quad、索引 `[0,1,2,2,3,0]`、正交矩阵全幅；第 5/6 帧起出现 **clear**。
2. **BR 黑而不是绿**：说明 **有一次全幅/近全幅的黑色 clear**（clearColor 0,0,0,0）覆盖了 BR；随后晚帧的 **partial draw 不再覆盖 BR**，于是 BR 保持黑色。若 clear 只覆盖 3/4，BR 应保持绿；实际平均亮度 0.2 → **BR 被黑 clear 过**。
3. 所以缺口在 **guest 侧 CA 的合成/重绘矩形**：晚帧只重画了 TL/TR/BL（dirty rect），BR 没有新的 draw；host 的 readback/上载只是忠实搬运。

### 最小观测（不改渲染逻辑）

- 下一发把 **late frame** 存下来，而不是只存前 3 帧：
  ```text
  glframes=1 GLWIRE_DUMP_PAIR=1 GLWIRE_DUMP_AT=760
  GLWIRE_SAVE_RECORDS=/tmp/r1late
  GLWIRE_SAVE_AT=760          # 或结束前最后一帧
  GLWIRE_DRAW_LOG=1
  ```
- 在 late records 里检查：
  - `glClear` 的 **scissor/viewport**（是否只清 3/4，还是全幅）；
  - 后续 `glDrawElements` 的 **bbox**（是否只到 375/812）；
  - `glScissor` 是否在 clear 后又被设成 375×812；
- 同时看 `hakios-gl: f` 的 `clears` 与 `display surface` 行，和 `s11` 最后一行像素。

### 最小修法（按 late 结果）

- **clear 只覆盖 3/4** → 修 clear 前的 scissor/viewport，恢复全幅 clear；
- **clear 全幅、后续 draw 只到 3/4** → 修 CA 的 dirty rect/重绘范围，强制 BR 也重画（或重新提交全屏背景）；
- **clear 全幅 + draw 全幅但 panel 仍黑** → 只可能是 guest 的 blit/写回矩形，检查 `glBlitFramebuffer`/`eagl_remote_surface2` 的 `copy_width/height`（目前代码是 750×1624，需日志确认）；
- **不再**改 host readback/upload，也不再猜 FBO/scissor（已排除）。

修复判据不变：**BR 非黑、dock 通栏 4 图标、Search 居中**，P0 内容判据不回退。仍不改代码、不 boot。

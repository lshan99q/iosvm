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

## R1 late-frame records 解析受阻（2026-11-13）

尝试解析 `/host/iosvm-work/hakios-xnu-kernel/logs/r1late-100.records`（23,765 B）失败：

- `decrec.py` 在 2–3 条记录报 unknown opcode（367/7939）；
- 我们的解析器按 **12-byte version-2 header**、**8-byte version-1 header**、以及 **8/12/16 字节头 + 0..127 偏移** 扫描，**都无法形成连续有效记录流**；
- 文件开头是：

```text
0f 00 00 00 00 00 00 00 00 00 00 00 c8 00 00 00 d5 00 00 00 ...
```

  这里 `0f`/`c8`/`d5` 不像 `spec.def` 的 opcode+length 组合（例如 `glClearStencil` 的 payload 应为 4，而这里是 0），后面还直接出现 `1406`（`GL_FLOAT`）、`3000`、`c000`、`c0000000`、`3f000000` 这类 **GL 参数/浮点数据**，更像**从某条记录的 payload 中间开始**，或文件被截断/重命名/二次编码，而不是 `host_glwire` 原样写出的 `records`。

因此**这一份文件无法回答**：
- `glClear` 次数/clearColor/scissor/viewport；
- late draws 的 bbox；
- clear 后 `glScissor` 是否变成 375×812。

### 需要的原始文件/日志

1. `host_glwire: saved ...` 的**原始路径**（正常应为
   `/tmp/r1late-100-750x1624-23765.records`，不要改名/截断/拼接）；
2. 该发的 `host.log`（如果有 `GLWIRE_DRAW_LOG=1`，直接有 late draw 的 fbo/program/viewport/scissor）；
3. 若 `GLWIRE_SAVE_RECORDS` 是从 **另一个 host build** 写的，给出该 build 的 `spec.def`/version。

在拿到原始 records 前，R1 的结论仍停在：
- host 无责（guest panel BR 黑）；
- 晚帧 `1 clear + 2/9 draws`；
- 缺口在 guest CA 的 full clear 后 partial redraw；
- **不 boot、不改代码**。

## R1 late records 的原始文件仍是 96×96 子帧（2026-11-13）

原始名 `r1late-100-750x1624-23765.records`（`/host/iosvm/logs/s22-ext/ext28/records/`）与旧副本
**md5 相同**（`ada89556385f5e4279e0ed40480671a1`），不是改名/截断问题；文件本身就不是 panel 全帧。

按 version-2 头解析出的内容：

- `viewport 0,0 750x1624`，但 **`glScissor(384,704,96,96)`**；
- draws：`count=6` ×3 与 `count=54` ×4，**顶点 bbox 全是 (384,704)–(480,800)**，即 **96×96 的 widget/图标子帧**；
- 没有覆盖 panel 的 **full clear**，也没有 BR（x>375,y>812）的 draw；
- 文件开头的 `0f 00…` 不是 panel 帧的 `glGetString/glClear` 序列，说明它是一条 **client 子帧**，不是 dock 那条带。

host 日志对不上 `100`：

```text
host_glwire: frame 100, 750x1624, 9019 records B ...
host_glwire: frame 255, 750x1624, 23765 records B ...
```

文件是 **23765 B → global frame 255**（或 client frame 100 → global 255），**不是 frame 100 的 9019 B**。
`GLWIRE_SAVE_AT=100` 命中的是 **client 的 frame 100**，这一帧是 96×96 子帧，所以没有 BR 可分析。

### 需要的下一份

- 保存一个 **全 panel 帧**：host 日志 `records_bytes = 7241/9019` 的帧，或 **client frame 99/101**（不要用 client 100）；
- 或者临时把 `GLWIRE_SAVE_RECORDS` 从“前 3 帧/单帧”改为“每 N 帧保存一次”（这是 host 侧小改，需另批）；
- 同时保留 `host.log` 的 `frame N` 行，按 **records_bytes** 对齐，不要按 `SAVE_AT` 数字猜。

**当前这份 records 不能回答 clear/scissor/draw bbox；R1 结论仍停在 host 无责、guest panel BR 黑。**
不改代码、不 boot。

## R1 真正的 panel 晚帧：panel-1 全幅、panel-3 只画 310×310（2026-11-13）

`decrec.py` 假设旧的 8-byte 头；当前 records 是 **version 2, 12-byte 头**（opcode, length, context）。
文件以 `glFlush(15, len 0, ctx 0)` 开头时，旧解码器把 context 当 opcode，才报 367/7939。
新增 `tools/glwire/harness/decrec2.py`（自动选 8/12，按 `spec.def` 解码，输出 clear/viewport/scissor/draw/attr0 bbox）。

### panel-1（7,381 B）

```
bindFramebuffer 5; viewport 0,0 750x1624
drawElements count=6 x2, attr0 bbox 0,0 .. 750,1624
bindFramebuffer 31; viewport 0,0 750x200
CLEAR mask=0x4000 color=(0,0,0,0) viewport=(0,0,750,200) scissor=None
glScissor 0,0 750x200
```

- **1 次 clear**，只在 **FBO31（750×200）**，不是 panel FBO5；
- panel FBO5 的 **两次 draw 都是全幅 0..750/0..1624**，BR 被覆盖；
- clear 后的 scissor 是 **0,0 750×200**，不是 375×812。

### panel-3（21,457 B）

```
bindFramebuffer 5; viewport 0,0 750x1624
drawElements count=6/24: attr0 bbox 46,142 .. 356,452（或 113,433..291,523）
glScissor 46,142 310x310
...
bindFramebuffer 31; viewport 0,0 750x200
CLEAR mask=0x4000 viewport=(0,0,750,200) scissor=(46,142,310,310)
```

- **1 次 clear**，仍在 **FBO31（750×200）**；**panel FBO5 没有 clear**；
- **panel FBO5 的 draw 最大 bbox 只有 46..356 / 142..452（310×310）**，**BR（x>375,y>812）没有任何 draw**；
- clear 后的 scissor 是 **46,142 310×310**，不是 375×812。

### 结论

1. **clear**：两帧各 1 次，`mask=0x4000`，`clearColor` 为 panel-1 的 `(0,0,0,0)`（panel-3 未在本帧重设，沿用）；视口都是 **750×200 的 FBO31**，**不是 panel FBO5**。
2. **draw bbox**：panel-1 全幅；panel-3 只到 **x=356,y=452**，**没有 BR**。
3. **clear 后 scissor**：panel-1 `(0,0,750,200)`，panel-3 `(46,142,310,310)`；**没有 375×812**。

因此 R1 是 **CA 的 partial redraw**：panel-3 只重画左/上 310×310 区域，panel FBO5 又从不 clear，BR 保留旧的黑/绿。
**最小修法**：让 guest 每帧对 panel FBO5 做全幅 clear 或重画全屏背景，再让 CA 的 partial draw 叠加；**不要**改 host readback/upload，也**不要**按 375 的 scissor 猜。
判据仍为 **BR 非黑 + dock 通栏 4 图标 + Search 居中 + P0 不回退**。不改代码、不 boot。

## R1 gl=0/gl=1 对照：GL 路径特有，缺失边界 = 375×812 pt（2026-11-13）

- gl=0（`inject.c:1624`，CA 保持软件渲染）：dock 4 图标、Search 居中、图标 4 列，BR 非黑 **39.3%**；
- gl=1（Mesa/GL）：BR 非黑 **0.7%**，dock 只剩 2 图标；
- panel 750×1624 px = 375×812 pt @2x，缺失边界 **x=375,y=812** 正好是点数。

### 绘制者

dock、Search、backdrop 不是某一笔 GL draw，而是 **SpringBoard 的 CoreAnimation 层**（`SBDockView`/`SBSearch`/backdrop 层）由 CA 的 EAGL renderer 合成；我们的 `eagl.c` 在
`CA::WindowServer::EAGLServer::renderer()` 下面接管 GL。CA 软件路径按 px 直接画；GL 路径由 CA 把 layer 的 bounds（pt）交给 EAGL renderer，若 `contentsScale` 被当成 1，layer 只有 375×812，右下自然空白。

### 代码检查（只读）

- `inject.c` 的 `gl=0` 逻辑只负责“不加载 eagl.c，让 CA 用软件 renderer”（`inject.c:1624`）；
- `inject.c:83` 的 `widget_scale` 只给 **widget_placeholder** 的 750-space 矩形用，和 dock/Search 无关；
- `inject.c:1235` 的 `EDGE_SIDE 16/375`、`EDGE_TOP 16/812` 是状态栏安全区 shader 的 pt 常量，不是绘制目标；
- `eagl_remote.rle5.c:638` 的 `remote_upload_scale` 只影响 **host 返回图像的缩放**（默认 1），不改 guest 的 viewport/scissor；
- `eagl.c:1477` 的 `copy_width/height /= scale` 也只用于 surface 回写，不参与 CA 的绘制裁剪；
- 全树没有在 CA 的 window/layer 上设置 `contentsScale`/`contentScaleFactor` 的代码；`inject_sb.c:1106` 只**打印** `layer.scale`，没有设置。

→ **最可能：CA 的 GL renderer 拿到的 layer bounds 是 375×812 pt，`contentsScale` 没有按 2x 换算，于是只覆盖 TL/TR/BL 的一部分，BR 缺失。** 这不是 eagl 的 upload scale，也不是 host 读回。

### 最小可测

```text
gl=1
GLWIRE_DRAW_LOG=1
GLWIRE_DUMP_TEXTURES=/tmp/r1tex
GLWIRE_SAVE_WH=750x1624 GLWIRE_SAVE_AFTER=700
GLWIRE_SAVE_RECORDS=/tmp/r1late
```

- `GLWIRE_DUMP_TEXTURES` 里找 **宽度≈375** 或右半全黑的纹理；`GLWIRE_DRAW_LOG` 找 **viewport/scissor 375×812** 的 pass；
- `inject_sb.c` 的 `layers_at`（或 layer dump）打印 dock/Search/backdrop 的 **frame/contentsScale**；对照 CPU/GL 两发；
- 若 `layers` 的 dock/Search frame 是 375×812（pt）而 `contentsScale` 为 1，即确认 pt 当 px。

### 最小修法候选

1. 在 SpringBoard 注入里对 **window/layer 设 `contentsScale=2`**（或 `UIScreen.mainScreen.scale` 保持 2），确保 CA 的 GL renderer 得到 750×1624；
2. 或对 dock/Search/backdrop 这些 **pt bounds 的 layer** 在交给 GL 前按 `contentsScale` 换算成 px；
3. 不改 `eagl_remote_upload_scale`（它只管上载），也不改 host readback；
4. 判据：**BR 非黑、dock 通栏 4 图标、Search 居中、P0 内容判据不回退**。

不改代码、不 boot。

## R1 ext35：默认路径 558s 不是 guest↔host 环；先分清 slow vs deadlock（2026-11-13）

### 代码上的等待关系

- `eagl.c:1271 frame_end()`：
  - `composition_off = eagl_remote_no_local()`（`eagl_remote.c:463`，返回 `remote_only`）；
  - `remote_only = wire_vtop_hook != NULL && !remote_wants_local()`（`eagl_remote.c:394`）；
  - `composition_off` 只让 `frame_end` **跳过** `target_to_surface()`、`surface_to_texture()`、panel upload（`eagl.c:1298/1327/1343`），然后调用 `upload_changed_layers()`；
  - 它**不等待 host**，也没有把 host 的输出接回这一步。
- `eagl_remote_flush()`（`eagl_remote.c:930`）：
  - `send(request + records + segments)` 后 **等 host 的 reply**；
  - host 在 `serve_frame()` 里 replay、`glReadPixels`、写 picture，然后 reply；
  - 没有“host 等 guest 的 composition”这一半：guest 在 `flush` 之前已经把 `upload_changed_layers()` 的 image refs 和 records 发出去了。
- 所以 **不是**「guest 等 host 的 picture + host 等 guest 的 composition」的环；`composition_off` 只是少做本地工作，不会让 host 多等一个 guest 条件。

### 558s 更像哪种

1. **host 慢**：`serve_frame()` 的 replay/readback 卡住（尤其默认路径要 panel upload/readback），guest 在 `eagl_remote_flush` 的 `recv` 里等；
2. **host 等 guest**：如果 guest 的 records/segments 没发完，host 会卡在 `recv_all`；但 `send()` 失败会立刻 `close`，不会 558s；
3. **guest 等 host**：`recv` 无 ack timeout（`GLWIRE_ACK_MS` 默认 0 时是无限等）；host 若在 `glReadPixels`/GPU 上卡住，guest 就 558s。

**判定**：不是代码环，是 **host 侧一次 panel/readback 极慢**（或 guest `recv` 无限等）。`GLWIRE_NO_PANEL` 能跑，是因为 host 不做 panel 那一半。

### 默认路径修好后 R1 会不会自然消失？

**取决于 `remote_only`：**

- 若 `remote_only=1`（没有 `glwire_local`）：host 仍然用 records 重建，**R1 仍在**；
- 若 `remote_only=0`（`glwire_local=1` 或 `/redwood/etc/glwire_local`）：guest 自己合成 panel，`panel_with_remote`/panel upload 把它交给 host，**R1 应自然消失**（画面来自 guest 合成，不是记录重建）。
- 因此「默认路径」若只是去掉 `GLWIRE_NO_PANEL` 而仍是 `remote_only`，**不会**自动修 R1；需要 **`glwire_local=1` + panel upload**（或 `glwire_panel_remote=1`）。

### 记录流缺什么

- `image refs` 只有 **1 个 4,988,928 B**（750×1624 的 panel target）；
- `upload_changed_layers()` 在 `eagl.c:895` 明确 **`if (image->target) continue`**，所以 **panel target 不按 image ref 发**；
- 缺的是 **dock/Search/backdrop 这些非 target 图层的 IOSurface 内容或它们的 layer 纹理引用**，不是 draw 参数（panel-1 的 draw 已是全幅）。
- 如果 host 只有 1 个 image ref，dock 带的图层纹理确实**没被引用/上载**，record 重建自然补不出来。

### 最小可测（一发区分 slow/deadlock）

同一发 boot 内：

```text
glframes=1
GLWIRE_ACK_MS=5000          # 只用于判 deadlock：guest recv 超时就报错，不要无限等
GLWIRE_DRAW_LOG=1
GLWIRE_DUMP_TEXTURES=/tmp/r1tex
```

日志点：

- `hakios-remote: end of frame ... panel_up=%d` → 当前是否 guest 合成；
- `hakios-remote: s11 f N images ...` → image refs/图层数量；
- `hakios-remote: flush -> %d ...` + `f N wait %llu us sent ...` → guest 在 flush 等多久；
- host `host_glwire: frame N, ...` 的间隔 → host 是慢还是没收到；
- 若 guest 卡在 `recv` 而 host 没有 `frame N` 行 → host 等 guest（records/segments 不完整）；
- 若 host 有 `frame N` 但间隔 558s → host replay/readback 慢，不是环。

`GLWIRE_ACK_MS` 是判定用，不是修复；**不改代码、不 boot**。

## R1 ext35 host 分阶段计时（2026-11-13，只读设计）

现有 `host_glwire: frame N ... X ms` 的 `took = now_seconds() - began` 从 **records 收完之后**开始，
包含 replay、`glFinish`、`glReadPixels`、panel hash/in-place，**不含 request/segments/records 的 recv，也不含 reply send**。
558 s 被平均进一个数，无法定位。

### 建议打点（`serve_frame()`，host_glwire.c:3691）

```c
double t0 = now_seconds();              /* entry, before request line */
/* after request line parsed */        double t_req = now_seconds();
/* after segments recv */              double t_seg = now_seconds();
/* after records recv */               double t_rec = now_seconds();
double t_replay = 0, t_read = 0, t_panel = 0, t_reply = 0;
...
glFinish();                            t_replay = now_seconds();
...
glReadPixels(...);                     t_read = now_seconds();
...
panel_hash/in-place/write_picture_rows; t_panel = now_seconds();
...
send_all(done) + send_all(picture);    t_reply = now_seconds();
```

一行/帧（只在 `total > 1000 ms` 或每 100 帧打印，避免日志爆炸）：

```c
printf("host_glwire: stages frame %llu req=%.1f seg=%.1f rec=%.1f replay=%.1f "
       "read=%.1f panel=%.1f reply=%.1f total=%.1f ms in_place=%d refs=%lu\n",
       frames_replayed, (t_req-t0)*1e3, (t_seg-t_req)*1e3,
       (t_rec-t_seg)*1e3, (t_replay-t_rec)*1e3, (t_read-t_replay)*1e3,
       (t_panel-t_read)*1e3, (t_reply-t_panel)*1e3, (t_reply-t0)*1e3,
       in_place, wire_image_refs);
```

要点：
- `t_rec` 是**旧 `began`** 的位置；`rec` 大 = guest 的 request/segments/records 没到齐；
- `replay` 包含 **record dispatch** 和 **glFinish**；**guest 的 panel upload 走 records（op201），也在这段**；
- `read` 只包 `glReadPixels`；`panel` 包 hash/in-place/`write_picture_rows`；`reply` 包 `send_all`；
- `seg`/`rec` 为 0 但 `req` 大 = 等 guest 连接/请求；`reply` 大 = guest 不读/网络背压。

### `GLWIRE_ACK_MS` 的推荐值

`eagl_remote.c`：
- `remote_ack_effective_ms()` 只有在 **`ack_armed` 且未 disabled** 时返回 timeout；
- **前 5 个完整帧不武装**（`REMOTE_ACK_ARM_AFTER=5`），第 6 帧起 `SO_RCVTIMEO` 生效；
- **连续 3 次超时**（`REMOTE_ACK_MAX_DROPS=3`）后 **`remote_ack_disabled=1`**，回退到 **无限等**；
- 超时那一次：`recv` 失败 → **close socket、return 0**，该帧丢弃，下一帧重连。

推荐 **`GLWIRE_ACK_MS=30000`**（30 s）用于诊断：正常帧 5–30 s 不会误杀，558 s 会在 30 s 被抓住；**只用于判别**，不是修复。若设 5000，前 5 帧不生效，且 3 次慢帧后就自动回无限等，容易误判。需要更稳定可临时把 `REMOTE_ACK_MAX_DROPS` 调大，但那是代码改动，先不做。

### 判据与修法方向

| 计时 | 说明 | 最小修法方向 |
|---|---|---|
| `rec` 大 | guest 没发完 request/segments/records | 查 guest `send`/`upload_changed_layers`；不是 host replay |
| `replay` 大 | record dispatch + `glFinish` + guest panel upload | 减少记录量/跳过 panel upload 回放；查 `glslots`/多余 draw |
| `read` 大 | `glReadPixels` 等 GPU/读大 FBO | 用 `GLWIRE_READ_PANEL`/正确 panel target；避免读 64×19/越界大图 |
| `panel` 大 | panel hash + `write_picture_rows` | 只在需要时 hash；in-place 行写分块/跳过 |
| `reply` 大 | guest 不收/网络背压 | 查 guest `recv`/socket buffer；ACK 超时 |
| `total` 大但各段小 | 段间空档/日志/外部 | 查 host 主循环 poll/其他 client |

**注意**：`panel upload` 是 **guest→host 的 records**，不是 host 的上传；host 的 `replay` 段已包含它。若要看 guest 侧 panel upload 本身，用 `hakios-remote: end of frame` + `flush ->` + `f N wait`（`eagl.c`），不要把它和 host `readPixels` 混在一起。

不改代码、不 boot；打点按上面片段落到 `serve_frame()` 即可。

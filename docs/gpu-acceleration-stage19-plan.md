<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十九阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十九阶段工程文档

> 建立时间：2026-11-07 00:20
> 前置：第十八阶段（S0 稳定配方 `QEMU_SMP=1`；P0 影子把标签层送上屏、`texmat0` 从顶点推导成功、
> UV 数据在 UI 阶段损坏；见 `IOSVM-GPU加速第十八阶段工程.md`）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **启动已经可测**：`QEMU_SMP=1` 连跑 5 发 0 panic（最长 7248 帧、20 分钟），界面都能到。宿主侧 Windows 虚拟机
   限到 2 核、保持运行。
2. **标签层已经能上屏**：影子取「本帧第一笔标签合成」后，每个 App 图标下方出现标签层的红条（位置正确）。
3. **`texmat0` 推导成功**：从 draw 顶点推出 `0.3750 0.0000 0.1250 0.1250`（图集子矩形），红条消失、状态栏恢复、
   主屏多出三个图标；**但标签仍不是文字**。
4. **根因方向已经明确**：UI 阶段的 texcoord 属性数组在线上就是坏的（`0/4.7969`、`0/nan`、`6.1e28/0`，
   像半浮点被当浮点抄），同一块数组在启动早期是好的（`0.125..0.5`）。wire 的顶点数组按**客户端指针**直读，
   而 CA 在 UI 阶段很可能把数组放进 **VBO**（`glVertexAttribPointer` 的 pointer 是偏移）。
5. 壁纸仍是「启动镜像作业表缺 PosterBoard」；widget 两个面仍不由 GL 链路画；四格表未刷新。
6. **P0 已达成（2/3）：v7 99.85%、v9 99.98% 同帧 SAME FRAME；v8 因 dump 落在 UI 加载动画 89.53% NOT，不计入。** 详见 `docs/gpu-acceleration-stage19-p0-runbook.md` §4.8/§4.9。P1/P2/P3 仍未完成。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 顶点数组的修法：**给 wire 加 VBO 影子**（拦 `glBindBuffer`/`glBufferData`/`glBufferSubData`）vs **按指针快照**（在 `glVertexAttribPointer` 时就把数组抄下来） | 影子 VBO：改动大（客人机 + spec 生成器）；快照：数组可能在 pointer 之后才填，快照会抄到旧内容 | 文字标签 |
| D2 | 标签不可见的第二半：**修 backdrop**（darkenSover 的 D 用真实面板）vs **先绕开 darken**（临时关掉 program 13 的 darken 项看文字） | 先绕开：能快速判定「文字本身画对了没有」；修 backdrop：要理解 CA 的 backdrop 生命周期 | 可读的 App 名称 |
| D3 | 壁纸：**重建 system.apfs 补 `LaunchAngels→/redwood/LaunchAngels`** vs 不做 | 重建：一次 10–20 分钟 + 启动时间变长；不做：主屏永远黑底 | 壁纸 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0 | 标签是文字（**已达成 2/3：v7+v9**） | 连续 3 发（`QEMU_SMP=1`）里 ≥2 发首页能读出 ≥5 个 App 名称；截图 ≥300 KB；同帧 `diffdump --tolerance 8` ≥95% | 可用性 |
| P1 | 文字质量（**暂停：P1e `TRIM_FROM=start` 清空白顶行，trim 不是干净修复；转查 90 高背板/复合层；P1 未修复**） | 无重影；widget 左上角 40x40 白像素 875/1600（±2%）；标签为白 | 可用性 |
| P2 | 壁纸（**停止：C2 已证明不是 elision（xpcproxy 生效但仍 7–11s 被杀；16 CHECK_IN_TIMEOUT / 0 alive）；P2 性价比低/需更多信息；转 P3；P2 未达成**） | SpringBoard 的 `posterboardservices` lookup 不再 `No such process`；宿主出现 1125x2436（或全屏）壁纸纹理；主屏背景非纯黑 | 可用性 |
| P3 | widget（**进行中：P3a 链路通但内容空白；D1 `widget_cacontext` 探针已备/待跑；P3 未完成**） | `312x202`/`184x124` 以纹理进宿主；否则交付「拉面」的最小实现（时间盒 2 天） | 可用性 |
| P4 | 收尾 | 四格表（纯本地/按引用/W1.5/W1.5+W3）各一次 20 s 滚动窗口；宿主侧 `0x502` = 0；`make test`；文档与推送 | — |

---

## 三、实验计划（按顺序，每步停下来写结论）

### P0：顶点数组的语义（1–2 天）

**第一发（s19a，00:24–00:44）已经把 VBO 假设否掉**：客人机在每次 `glVertexAttribPointer` 时记下
`GL_ARRAY_BUFFER_BINDING`，日志里 **buffer 全是 0**，指针是稳定的客户端地址（四个属性同一次 draw 都在
`0x16f41c870` 起的 48 字节步长里：attrib0 base+0、attrib2 base+16、attrib3 base+24、attrib1 base+32），
**没有 VBO**。所以坏的不是「偏移当地址」，而是**这一块客户端内存在 draw 时刻的内容**，或者 CA 对这几笔
label draw 本来就填了非 texcoord 的数据。

1. **抓 UI 时刻（做过一发，翻车）**：改成每 500 笔 draw 打一行 attrib 3 的原始 8 字节后（s19b/s19c），
   s19d 那一发里客人机的 **backboardd 直接重启**（宿主看到 `client 22` 新连接、客人机 `pid=40`，界面没起来）——
   原因八成是诊断代码 `memcpy(raw, pointer, 8)` 读了一个**偏移量当地址**。教训：客人机诊断不许直接解引用指针，
   先判 `buffer == 0` 且地址在用户态范围（例如 > 0x100000000）再读；镜像已回滚到 s19a 那一版。
2. **backdrop 的来源已经查清（s19c，实测）**：label draw 在 unit 1 上绑的是 **texture 11，750x200**（不是我们填的
   64x19），`texmatD` 在两次 draw 之间在 `(0.5,0.5,0.5,0.5)` 与 `(0,0,0,0)` 之间跳；`(0,0,0,0)` 意味着整块采样同一个
   纹素。也就是说暗化项用的是 **CA 自己的 750x200 图层**，而我们填的 64x19 backdrop 根本不在这一步用。
3. **darken 关掉的实验（s19e，已做）**：宿主把 label 片元着色器的暗化调用改写成恒等（`GLWIRE_DARKEN_OFF=1`）。
   结果：**tile 变成纯黑**（`grey 0..0`、alpha 恒为 4/255、rgb 全 0），截图上的红条也消失了。读法：
   - **字形采样没有变化**：alpha 在整块 tile 上恒等于 4/255 → 图集采样是一个常数，不是字形覆盖率；
   - 顶点颜色在两次 draw 之间在 `(0,0,0,1)` 与 `(1,1,1,1)` 之间跳（`attrib 1` 的 half-float 数据本身是对的）；
   - 也就是说，红条 = **暗化项把「背板」（750x200 的图层）铺满了标签位置**，而文字本身从来没被采样出来。
   结论：**问题不在暗化，而在「纹理坐标 → 图集」这一步**（texmat 退化：推导出的 y 跨度是 0，说明 UI 时刻 attrib 3
   的三个角 v 相同；而同一数组在启动早期是 `0.125/0.5` 的正常子矩形）。
2. **修法（二选一，按 D1）**：
   - **VBO 影子**：拦 `glBindBuffer`/`glBufferData`/`glBufferSubData`（spec 加 3 个 opcode），客人机保留影子副本，
     draw 时按 `binding + offset` 取；
   - **快照**：`glVertexAttribPointer` 时不知道数量，但可以在**第一笔 draw** 时把「从 pointer 起 `elements*stride` 字节」
     抄一份留到后续 draw（同一 pointer 复用时不重抄）。
3. **判据**：宿主日志里 attrib 3 在 UI 阶段的值回到 0.125..0.5；标签出现文字。

### 2026-11-07 下午：三个动作

1. **四顶点修正**：`s19g` 的宿主日志显示 label draw 的 attrib 3 三个角全是 `v = 0.125`，所以从三个顶点推出来的
   texmat 的 v 跨度是 0（图集只采一行）——这就是 tile alpha 恒为 4/255 的直接原因。**四边形有四个顶点，v 跨度在第四个上**。
   宿主已改成读四个顶点（`GLWIRE_TEXMAT_FROM_ATTRIB`）。
2. **客人机侧问「NaN 是谁算的」**：`remote.inc` 的 `glUniform4fv`/`glUniform4f` 现在会在参数里出现 NaN 时打印
   `uniform4fv location N count M sends nan: ...`（各 40 条上限）。判据：若界面阶段出现这些行 → NaN 是 CA 自己算的
   （宿主侧替换成推导值即可）；若一条都没有 → NaN 是线上的（去查录制）。
3. **显卡被占用时的退路**：宿主加了 `GLWIRE_SOFTWARE_GL=1`（跳过 EGL 设备枚举、用默认 display，配
   `LIBGL_ALWAYS_SOFTWARE=1` 走 Mesa llvmpipe）。实测这台机器仍解析到 NVIDIA 驱动，所以这个开关暂时只是留了接口。

### 2026-11-07 下午的实验结果（s19h/s19i）

**s19h（四顶点 + 客人机 NaN 诊断，跑到界面）**：
- 客人机 **一次都没有** 发送 NaN 的 `glUniform4fv`/`glUniform4f`（0 行）→ **NaN 不是客人机算的**。
- 四顶点推导出的 texmat0 = `0.7500 0.0000 0.1250 0.1250`——**v 跨度仍是 0**（第四个顶点的 v 也是 0.125），
  说明被选中的那条属性数组本来就只有一行 v；图集子矩形并不在它里面。
- 宿主日志里 `label state program 13 texmat0 nan nan nan` 依旧——**而这一发里我们自己也会往 texmat0 写值**
  （推导），所以「NaN 是谁写的」必须排除我们自己的写入。

**s19i（关掉所有 texmat 试验，只留 `GLWIRE_UNIFORM_LOG`，1835 帧到界面）**：
- **texmat0 仍然是 NaN**（`label state program 13 texmat0 nan nan nan 0.0000`）→ **NaN 不是我们的推导写进去的**，
  它在流里。
- 但客人机的 4fv/4f 一次都没发过 NaN（s19h 0 行）→ 说明写 NaN 的不是这两种调用。
- `GLWIRE_UNIFORM_LOG` 的头几行暴露了**位置映射的碰撞**：
  `uniform 1f programme 11 location guest 4 host 0`、`uniform 1f programme 9 location guest 3 host 0`、
  `uniform 1f programme 35 location guest 5 host 1`——多个客人机位置被映射到同一个宿主位置。
  如果 texmat0 的宿主位置正好是某一个被别的 uniform 写到的位置，NaN 就有来源了（或至少是「写错位置」）。
- 结论：下一步必须**看见「谁写了 texmat0 的宿主位置」**——已给宿主加环形记录（每笔 uniform 写记
  programme/guest location/host location/四个值），标签状态读到 NaN 时把命中该宿主位置的最近若干笔打印出来。
  这一发就是 s19j。

**tile 的脾气（s19h 的 88x19 dump）**：左半是红/橙/黄竖条（面板缩略图被拉满），右半是黑底 + 白色横条 + 一点蓝——
这是**单行切片**被垂直拉伸的样子，和「v 跨度 0、只采样图集一行」完全一致。也就是说：标签的 UV 映射退化成了一维。



- **启动到第一次 draw 的时间在变长**：08:57 那一发 11 分钟、09:21 那一发 15 分钟还没到界面，`boot-measure.sh` 的
  `NOUI_BY=780`（13 分钟没有界面就停）会把慢启动直接掐掉（s19f `end=noui`）。
  **以后功能实验用 `NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500`**，别让脚本提前收工。
- 每次 boot 前确认没有残留 qemu；连续跑多发的会话里，宿主的 I/O 也会让客人机变慢。

### P0 的补充证据（2026-11-07 上午，实测）

| 观察 | 数据 | 读法 |
|---|---|---|
| 标签 tile 的像素 | `GLWIRE_TILE_DUMP` 写出的 64x19 tile = 红底 + 黄条/粉条（`grey 95..214`）；184x90 的标签层 = 红底 + 较亮的竖条（主色 `(191,52,43)`，另含 `(236,160,22)`） | 64x19 里的颜色就是**我们填进去的面板缩略图**（红=状态栏、橙=图标），不是文字 |
| 暗化项采样的是谁 | `label state texmatD` + unit1 纹理：**texture 11，750x200**；`texmatD` 在 `(0.5,0.5,0.5,0.5)` 与 `(0,0,0,0)` 之间跳 | 暗化用 CA 自己的 750x200 图层，`(0,0,0,0)` 时整块采样同一个纹素 |
| 关掉暗化 | tile 变纯黑：`grey 0..0`、alpha 恒为 **4/255**、rgb 全 0；截图里的红条消失 | **字形采样是常数**——纹理坐标→图集的映射退化，红条只是暗化把背板铺满了标签位 |
| 顶点颜色 | `attrib 1`（half-float）在两次 draw 之间在 `(0,0,0,1)` 与 `(1,1,1,1)` 之间跳 | 颜色数据本身是对的，不是黑字的问题 |
| 推导出的 texmat0 | `0.3750 0.0000 0.1250 0.1250` | **v 跨度是 0**（三个角 v 相同）→ 采样只落在图集的一行上，这正是 alpha 恒定的原因 |

**一句话**：现在的瓶颈不是暗化、不是顶点颜色，而是 **UI 时刻 attrib 3（图集子矩形）的数据**——它决定 texmat 的 v 跨度。

### 下午四发的账（s19h–s19k）

| 发 | 内容 | 结果 |
|---|---|---|
| s19h | 四顶点推导 + 客人机 NaN 诊断 | 客人机 **0** 次发送 NaN；推导值 `0.75 0 0.125 0.125`（v 跨度仍 0）；标签状态仍 NaN |
| s19i | 关掉全部 texmat 试验 + `UNIFORM_LOG` | **texmat0 仍 NaN** → NaN 在流里；日志暴露位置映射碰撞（guest 3/4/5 → host 0/0/1） |
| s19j | 宿主加 uniform 环形记录 | **环形记录没打印**（NaN 读到 1574 次）——怀疑跑的是旧二进制 |
| s19k | 加「启动标记 + NaN 显式打印」后重跑 | 启动标记确认新二进制；等界面阶段看环形是否触发 |

**教训**：宿主二进制在 `qemu` 还跑着的时候被 `make` 覆盖会 `ETXTBSY` 失败，而 grep 只筛 `error` 会漏掉它——
**以后每次 boot 前先看 `host_glwire` 的 mtime 和日志里的 `build marker`**（s19k 起宿主启动就打印这一行）。

### 运行经验

- 启动到第一次 draw 要 **11–15 分钟**，`boot-measure.sh` 的 `NOUI_BY=780` 会把慢启动掐掉；功能实验用
  `NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500`。
- 每次 boot 前确认没有残留 qemu/host_glwire（`ps -eo pid,comm | grep -E "qemu-system|host_glwire"`）。
- 显卡被别的任务占满时（如 hashcat），宿主渲染会跟着变慢、客人机更容易卡；这种时段只做离线分析和编译。

### 2026-11-07 15:16 的关键数据（s19l）：texmat0 其实是单位阵！

```
glwire: texmat0 bits 3f800000 3f800000 00000000 00000000 location 0 programme 13 error 0
glwire: uniform writes to host location 0, newest first:
glwire:   programme 13 guest location 1 (4fv): 1.0000 1.0000 0.0000 0.0000
...
glwire: label state program 13 texmat0 nan nan nan 0.0000 ...
```

- **按位读出来的是 `1.0 1.0 0 0`（单位阵）**，环形记录也显示 CA 用 `glUniform4fv` 往 program 13 的
  location 1 写了 `(1,1,0,0)`——所以「NaN」根本不是 texmat0 的值，**是同一段代码里那行老 printf 的显示问题**
  （按位打印和它的值对不上，说明那行 printf 的取值路径有蹊跷，已加「状态行前再打一次位」来钉死）。
- 也就是说：**标签的纹理坐标映射是恒等映射**——`texcoord0_0 = vertex_texcoord0`（单位正方形），
  于是采样覆盖整张「图集」纹理，而不是一个字形子矩形。这解释了标签 tile 里为什么是整片拉伸的颜色：
  **采样的不是字形，是整张纹理被压到标签位置**。
- `label sample texture 13 189x90 internal 0x1906: 16748 nonzero, 37 with alpha`——被采样的纹理几乎完全不透明
  （alpha 只有 37 个像素），而暗化项要的是 alpha 覆盖率。**纹理本身就不是字形掩码。**

**下一步（s19m，运行中）**：`GLWIRE_ATLAS_DUMP` 把这张纹理原样写出来看看到底是什么（图标？面板？文字层？）。

### 15:48 的 s19m 收尾数据：NaN 是真的，图集也不是字形

```
glwire: texmat0 bits at the state line 7fffff80 7fffff80 7fffff80 00000000   (1090 次，全部)
glwire: label state program 13 texmat0 nan nan nan 0.0000                    (同一批 draw)
glwire: legacy texture 161x90 internal 0x1906 format 0x1906: taken natively
glwire: atlas dump /tmp/s19m/atlas-187x90.bgra 187x90
```

- `0x7fffff80` 的指数位全 1、尾数非零，**它确实是一个 NaN**——所以状态行的 `nan` 不是打印错误，
  早先那 3 次 `3f800000`（=1.0）是极早期的 draw，之后全部是 NaN。
- **被采样纹理的 alpha 通道**（渲染出来看）是一团**模糊的圆角矩形**加底部一排小点——不是字形掩码。
  也就是说，这些 tag 位置的 draw 采样的是「模糊背板/阴影」那类纹理，**不是文字图集**。
- 结合「v 跨度 0、只看一行」：这些 pass 更像 CA 的**阴影/模糊 pass**，文字本身的 pass 还没被我们认出来。

**下一步**：在宿主里按 `program` + 绑定的纹理尺寸/内容分类 label 相关的 draw（模糊 pass vs 文字 pass），
先确认「文字 pass 是否真的存在」，再谈修哪一段。

### 16:00 的新认识：**文字 draw 找到了（programme 15），但它一像素都不落**

从 s19m 的宿主日志按 program 分类：

| program | 界面阶段 draw 次数 | 是什么 |
|---|---|---|
| 17 | 7316 | 面板/图标合成 |
| 5 / 3 | 3951 / 3749 | 其它合成 |
| 0 | 1839 | 清屏 |
| 7 | 1367 | 面板缩放到窗口 |
| **13** | **1333** | 暗化（标签背板/阴影） |
| **15** | **256** | **文字（遮罩）** |

programme 15 的 draw 关键字段（实测一条）：

```
draw#27 ... program 15 ... viewport 0,0 750x1624 scissor on,0,0 161x90
label sample texture 15 113x42 internal 0x1906: 1158 nonzero, 476 with alpha
attrib 0: (406,647) (519,647) (519,689)   <- 标签位置，113x42 的四边形
attrib 1: (1,1,1,1)                        <- 白色
attrib 2: (0,0) (1,0) (1,1)                <- 单位正方形
texmat0 bits: 3f800000 3f800000 0 0        <- 前几笔是单位阵
label draw #2 ... 750x200: 0 nonzero pixels
```

**这就是文字绘制**：小尺寸 GL_ALPHA 纹理（113x42 / 98x42 / 64x42，alpha 覆盖正常：1158 里 476 个有 alpha）、
位置正确、颜色白色、texcoord 是单位正方形——**但落到目标上 0 个像素**。
之后的 draw 里 texmat0 变成 `0x7fffff80`（NaN），采样未定义 → 什么都写不出来。

**因此这一发的判据（s19n，运行中）**：在绘制前把 NaN 的 texmat0 换成单位阵
（`GLWIRE_TEXMAT_NAN_TO_IDENTITY=1`）。若 `label draw #N` 的 nonzero 从 0 变成上千、截图里出现 App 名称，
标签链就通了。

### 16:24 里程碑：**屏幕上第一次出现真正的字形**

`s19n`（`GLWIRE_TEXMAT_NAN_TO_IDENTITY=1`：在 label/文字 draw 之前，把 NaN 的 texmat0 换成单位阵）：

```
glwire: texmat0 was a NaN (programme 15), set to (1,1,0,0)        x5
glwire: label draw #70 framebuffer 4 attachment texture 8 750x1624: 1799 nonzero pixels, 1799 with alpha
glwire: label draw #72 ... 1693 nonzero pixels
glwire: label draw #74 ...  866 nonzero pixels
```

截图里出现了 **「Se」两个灰色抗锯齿字母**（见 `docs/demo/s19-label-glyphs-20261107.png` 与其放大图）——
这是整个十七/十八/十九阶段第一次让**真正的字形**落到屏幕上。

**为什么只有两个字母**：多数 label draw 仍然落在 0 像素（它们的 scissor 是 `0,0 178x90`，而顶点是面板坐标
`(406,647)` 这类），只有少数 draw 的几何正好落在自己的 scissor 里。**所以下一步不是 texmat，而是「谁在裁剪谁」**：
把「落了像素的 draw」和「落 0 像素的 draw」的 scissor / viewport / 顶点矩阵逐字段对比，找出那一处不一致。

**当前判据状态**：标签链已经「打通到字形」，剩下的是**几何/裁剪的一致性问题**，不再是格式或采样问题。

### P1：backdrop 与文字可见性（1 天）

1. **绕开 darken**：给宿主加一个 `GLWIRE_PROGRAM_SKIP=<program>`（跳过该 program 的 draw）或
   `GLWIRE_DARKEN_OFF=1`（把 `darkenSover` 的 `S *= y^8` 去掉），判定「文字本身画对了没有」。
2. **修 backdrop**：program 13 的 `texture1` 用的是「面板缩略图」；试真实面板（750x1624 原尺寸，`texture2DProj`
   的坐标本来就是屏幕投影），或让客人机上传真正的 backdrop 面。
3. **判据**：截图里能读出 App 名称；`GLWIRE_LEGACY_FORMATS` 默认（原生）下圆角 875/1600。

### P2：壁纸（1 天）

1. `build.sh` 里 `rm -rf sysroot/System/Library/LaunchAngels` 之后加
   `ln -s /redwood/LaunchAngels sysroot/System/Library/LaunchAngels`，并在 root.apfs 里建 `/redwood/LaunchAngels`，
   拷 `com.apple.PosterBoard.plist`。
2. **判据**：lookup 不再失败；宿主出现壁纸尺寸纹理；主屏背景非纯黑。失败也要给出「poster 进程崩在哪」。

### P3：widget（时间盒 2 天）

1. `inject_ciq_probe.c` 的钩子拿 `{ID,w,h,stride,format,屏幕矩形}` → XPC/共享内存 → backboardd → 宿主按 surface ID
   拉取（照 opcode 201 的段表，`remote_vtop()`）。
2. 时间盒到期只允许三种结论：可行 / 不可行 / 需要更多信息。

### P4：四格表与收尾（1 天）

四份构建各一次 20 s 滚动窗口（同 `QEMU_SMP=1`、同触摸脚本）；`0x502` 按「宿主侧 0」记账；`make test`；文档；推送。

---

## 四、不做（已被数据否掉，别重试）

1. **不再用 4 vCPU 做功能实验**：MTTCG 的 ticket-spinlock panic 是环境性的，`QEMU_SMP=1` 才稳。
2. **不再猜 `texmat0` 的值**：它已经从顶点推出来了（0.375/0/0.125/0.125）；问题在数组数据的来源。
3. **不再把「tile 有像素」当文字**：backdrop 填充会让 64x19 tile 全非零。
4. **不再说「合完 label 整个面板重建」**：逐笔哈希已推翻（第十七阶段 §十一）。
5. **不动内核**：本阶段是客人机用户态 + 宿主。

---

## 五、验证环境与操作规范

- 标准配方：**`QEMU_SMP=1`**（0 panic），每发 1100–1500 s；界面在 10–15 分钟到。
- 启动：`QEMU_SMP=1 HOST_ENV="..." tools/glwire/harness/boot-measure.sh <label> <root.apfs> <secs>`。
- 用户态重建：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`
  （改 `userland/src/eagl_remote.c`/`remote.inc` 都要重建；重建前把 `userland/root.apfs` 备份到 /tmp）。
- 宿主探针：`GLWIRE_BACKDROP`、`GLWIRE_READ_PANEL`（默认开）、`GLWIRE_PANEL_SHADOW` / `GLWIRE_SHADOW_APPLY` /
  `GLWIRE_SHADOW_MODE`、`GLWIRE_TEXMAT_FROM_ATTRIB`（从顶点推导）、`GLWIRE_TEXMAT_IDENTITY`、
  `GLWIRE_FIX_TEXMAT`（诊断）、`GLWIRE_PROGRAM_ALL`、`GLWIRE_DRAW_LOG`、`GLWIRE_LABEL_DUMP`。
- 证据图放 `iosvm/docs/demo/`，每张配 `NOTES.md` 的一条。

---

## 六、里程碑

| 天 | 目标 | 产出 |
|---|---|---|
| D1 | P0 诊断 + VBO 影子/快照 | 宿主日志 attrib 3 回到 0.125..0.5 |
| D2 | P1 绕开 darken → 修 backdrop | 能读出 App 名称的截图；**实际 P1a：`SKIP=13` Weather 562→44；全标签 101.7%（重分布）；P1b `TC0_SANITIZE` 53.9% 但 Weather 719→741；P1c 点带在 glyph tile alpha；P1d end 清底部出竖条；P1e start 清空白顶行、band 73.6% 但分标签有升有降；trim 方向停止，P1 暂停** |
| D3 | 三发验证 | ≥2/3 发标签为文字、截图 ≥300 KB、diff ≥95%；**实际 v7 99.85%、v9 99.98%（2/3）；v8 89.53% 因 dump 落在 UI 加载动画，不计入** |
| D4 | P2 壁纸 | lookup 不失败 / 壁纸纹理进宿主；**实际 P2：lookup 0 失败、PosterBoard running；P2c-timer 层树通但 contents=0；P2d 依赖对照失败；P2e C1 limits 未生效；C2 `launchd_proxyless_spawn=0` 生效但 extension 仍 7–11s 被杀（16 CHECK_IN_TIMEOUT、0 alive）；5,249 张 750x1624 全 panel；991,040 B 截图右下 1/4 纯绿伪影（683,954 px），不是壁纸；不是 elision；P2 未达成/性价比低** |
| D5–D6 | P3 widget（时间盒） | 可行/不可行结论；**实际 P3a：链路通、surface 3 空白白块；D1 `widget_cacontext` 探针已备，待跑** |
| D7 | P4 四格表 + 收尾 | 四个数字 + `make test` + 推送 |

### 6.1 下一阶段入口（P0 之后）

P0 已达成 2/3（v7+v9），但只覆盖「名称上屏」。下一阶段按 P1 → P2 → P3：

- **P1 文字质量**：标签下方虚线伪影；Search 完整性（当前多数时刻仍是「Se」，更晚加载才出现完整 Search）；widget 左上角 40x40 白像素 **875/1600（±2%）**；标签为白。**P1e：FNV 证明 upload row0=视觉顶部，`TRIM_FROM=start` 清的是空白顶行；band 6989→5146（73.6%）但 Wallet +1244%、Health +70%、Photos +81%，trim 不是干净修复。停止 trim，转查 90 高背板/复合层；P1 方向暂停，优先 P2 依赖/P3。P1 未修复。**
- **P2 壁纸**：P2 修好 lookup、PosterBoard running；P2b 触发失败；P2c-alt/alt2 证明跨进程 notify 不可用。P2c-timer timer/层树通但 contents=0；P2d 四 job 仅 profiled 成功，依赖不是原因。**根因（§4.23）**：18 个 extension 全被 check-in watchdog 杀（5–13s；GradientPoster spawn→kill 7.0s）；900s 层树 contents=0 是时序，不是永久不接。**P2e C1 失败（§4.24）**：limits plist 691 B 可解析，但 `ExtensionWatchdogTimeout=120` 未生效；~9s 仍被杀；50 次 CHECK_IN_TIMEOUT、0 次 checked in as alive。**C2 失败（§4.25）**：`launchd_proxyless_spawn=0` 生效（`service will always use xpcproxy`、`xpcproxy spawned`、`elide xpcproxy`=0），但 Batteries 7.2s、Gradient 11.4s 后仍 `CHECK_IN_TIMEOUT`；全 log 32 次 hanging、16 次 CHECK_IN_TIMEOUT、0 次 alive；1800s 层树 contents=0、5,249 张 750x1624 全 panel；991,040 B 截图右下 1/4 纯绿 `#00FF00`（683,954 px）是渲染伪影，不是壁纸。**不是 elision；停止 P2 timeout/xpcproxy 方向，P2 性价比低/需更多信息**；P2 不阻塞 P3，P1 暂停，P0 仍 2/3。
- **P3 widget**：**P3a 已跑（§4.26）**：root `f2472194…`、system 未动；guest 1 条 `widget surface 3 184x124 -> texture 0xffff1003 rect 46,142 309x309`，host 1 条 upload（bytes 91,264）+ 4 条 `widget compose`，无 removed；**链路通**。但截图 widget 区域 mean RGB **251**、nonblack 211,753/214,369 → **纯白圆角块**；surface 3 的 92x62 pt 与层树 155x155 不符 → 链路通、语义不对，不再调 rect。**D1（§4.28，待跑）**：`widget_cacontext=0x<id>`；runtime 按 type encoding 试 `contextWithID:`/`objectForContextID:`/`contextForID:`，再试 `createImage`/`createCGImage`/`snapshot`；打印类/CGImage 尺寸/bpp/bpr/前 16 字节/nonzero；`CARenderServerSnapshot` 只 dlsym present/absent。库 `libhakios.dylib` 92,208 B（P3a 90,720，备份 `/tmp/libhakios.p3a.dylib`），gl 400,720，patch 263 行；boot `widget_cacontext=0x<id>`、host **不开 `GLWIRE_WIDGET`**、NO_PANEL、UI_GRACE=600。判据 selector 打印 + 对象非空 + 尺寸合理 + **nonzero>0**；纯白/纯黑不算。放弃：无类/selector、连续 3 次全空、阻塞/崩溃、1–2 天。P3 未完成。
- **回归基线**：不要用 v8 的动画帧当基线（v8 的 panel 已完整、picture 还停在前一状态，89.53% 是采样时刻问题）；用 v7/v9 这类静态时刻，或给 dump 加「连续两帧 panel/picture 稳定」门槛。

---

## 七、诚实的预期

- **P0 的证据已经很强**：坏的是 UI 阶段的那块数组，而 wire 按客户端指针直读——VBO 影子是标准解法，判据也清楚。
  但客人机改动要重建镜像，一次 10–20 分钟，别在一个 boot 里反复改。
- **P1 的「绕开 darken」大概率立刻见文字**（红条已经证明标签层在画），风险是文字颜色/alpha 与背景相同。
- **P2 要重建 system.apfs**，可能顺带改变启动时间与内存布局，四格表的口径要记清楚。
- **P3 跨进程**，时间盒到期就收结论，不硬做。

---

## 八、暂停记录（2026-11-07 09:57）

**主人要求暂停：其他任务要用显卡。** 已经做的收尾：

- `qemu-system-aarch64` 与 `host_glwire` 全部结束；`nvidia-smi` 显示 **0% / 0 MiB**，GPU 已空出。
- 为实验把 Windows 虚拟机限到的 `CPUQuota=200%` **已清除**（`cpu.max` 回到 `max 100000`），虚拟机仍在运行。
- 代码与文档全部提交并推到 GitHub main（最后一个提交 `e15e96f`）。

**恢复时要接的位置**：

| 项 | 状态 / 位置 |
|---|---|
| 启动配方 | `QEMU_SMP=1` + `NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500`（今天的启动要 11–15 分钟才到第一次 draw） |
| 客人机镜像 | `userland/root.apfs` = 带**安全版**周期诊断（`remote_emit_count % 500` 打 attrib 3 的 buffer/stride/前 8 字节；只读客户端数组） |
| 备用镜像 | `/tmp/root.apfs.s19a`（已知可跑、无指针读取）、`/tmp/root.apfs.stage18-good` |
| 宿主探针 | `GLWIRE_TILE_DUMP[_FROM]`、`GLWIRE_DARKEN_OFF`、`GLWIRE_TEXMAT_FROM_ATTRIB`、`GLWIRE_LABEL_DUMP`（现在会打 `texmatD` 与 unit1 纹理） |
| 已确认的结论 | 红条 = 暗化项铺的背板；字形采样是常数（alpha 恒 4/255）；texmat 的 v 跨度是 0；顶点颜色正常 |
| 下一步 | 跑一发 `s19g` 的替代：确认 UI 时刻 attrib 3 的 `buffer` 是否为 0——若是 0 且字节仍是半浮点样的垃圾，就在客人机侧查 `glVertexAttribPointer` 的录制；若不为 0，则 wire 需要 VBO 影子 |
| 未完成的实验 | s19f/s19g 都在界面之前被脚本或手动停掉，没有拿到 UI 时刻的 emit 行 |

**恢复顺序建议**：先确认 GPU 空闲 → `tools/glwire` 重新 `make host_glwire`（当前二进制已是最新）→
`ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1300 MINFRAMES=99999 ... boot-measure.sh s19h .../root.apfs 1500`。

### 八点一、补充：s19g 其实跑到了界面（10:03 取出数据）

停掉之前那一发（s19g）已经到过界面：**1555 帧、31 条 emit、30 个 tile dump、截图 235,791 B**。控制台里最有用的几条：

```
hakios-remote: emit 500  attrib 3 buffer 0 stride 48 raw a0ffff3d 5f00003e float 0.1250 0.1250
hakios-remote: emit 1000 attrib 3 buffer 0 stride 48 raw d900003e defeff3d float 0.1250 0.1250
hakios-remote: emit 1500 attrib 3 buffer 0 stride 48 raw 00000000 00005840 float 0.0000 3.3750
hakios-remote: emit 2000 attrib 3 buffer 0 ... float 0.1250 0.1250
```

**读法**：
1. **`buffer` 在界面阶段也全是 0** —— VBO 假设彻底否掉（不是偏移当地址）。
2. 很多 draw 的 attrib 3 第一个顶点是**合法的 `0.1250 0.1250`**（原始字节 `a0ffff3d` 就是 0.125 的 float）。
   也就是说客人机这一侧的数组内容**不总是垃圾**；宿主看到的 `0/4.7969`、`0/nan`、`6.1e28/0` 至少有一部分是
   **别的 draw**（宿主的属性打印只在 glyph draw 上、上限 20 行；两条线不一定对的是同一笔）。
3. 仍有 `0.0000 3.3750` 这类异常组合（多出现在 `(0.0, 3.375)`），下一发要**把宿主与客人机的打印按同一笔 draw 对齐**
   （例如都带 `emit_count` / draw 序号），才能说「同一笔的属性到底是不是垃圾」。

**下一发（恢复后第一件事）**：把 `GLWIRE_TILE_DUMP` 的 dump 与 guest 的 emit 行用同一个计数器关联，判据：
同一笔 label draw 上，客人机读到的 attrib 3 与宿主收到的 attrib 3 是否一致。

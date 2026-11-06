<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十五阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十五阶段工程文档

> 建立时间：2026-11-06 10:00
> 前置：第十四阶段（结果见 `IOSVM-GPU加速第十四阶段工程.md` §九：fbo 1 是宿主替身；backboardd 一次启动两条
> EAGL context 合流；1313 笔标签 draw 全 0 像素；0x502 归零）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **界面、图标、双面写回稳定**：截图 247,349 B，稳态 0.02–0.16 s/帧（约 7–50 fps 抖动，中位 9 fps）。
2. **缺的三样内容（标签 / widget / 壁纸）已经被同一条证据串起来**：像素在「另一条 GL context」或
   「另一个进程」里，而 wire 只录了 backboardd 的一条合并流、宿主只有一个 GL context。
3. **标签的链路已经完全解码**：program 15 片元 = `vec4(glyph.a)`（纯 mask）；program 13 片元 =
   `darkenSover(vertex_color*glyph.a, backdrop)`，其中 `S = S * y^4`（y = backdrop 亮度），
   那张 64x19 是**屏幕背景的缩略**（`texcoordD = NDC*0.5+0.5`）——backdrop 全 0 时输出恒 0，
   十三阶段「填白不变」是必然结果。
4. **fbo 1 = 宿主替身**：它不是图层表面、也不是第二面板，是 `client->framebuffer`，替客人机的 **FBO 0
   （EAGL 默认 framebuffer，即屏幕）**。标签 pass 是每帧第一笔 draw，落在它上面，画完不读回也不合成。
5. **widget/壁纸的入口**：SpringBoard 建了 `surface 3 (184x124 BGRA)`、`surface 4 (312x202 LA08)`，
   但 backboardd 的 28 张 image 列表里没有它们，也没有任何壁纸尺寸的面。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 上下文隔离：**wire 头加 `u32 context` + 宿主每 context 一个 EGL context（不共享）** vs **只在 context 切换时重发状态快照** | 快照：状态面太宽（FBO/VAO/uniform/纹理单位/混合/scissor），漏一项就还是错的，而且不可验证；per-context：要动 wire 格式与宿主结构，但语义是教科书式正确的 | 标签 pass 拿到自己的 state（P1 的全部） |
| D2 | FBO 0 的语义：**请求行带 drawable 纹理名，宿主给 FBO 0 建 alias FBO** vs **继续用 renderbuffer 替身、帧末合成** | 替身+合成：与 CA 的真实布局不一致，且 alpha/mask 语义会错；alias：要客人机先说出 drawable 是谁 | 标签直接写进面板纹理，读回即见 |
| D3 | 另一个进程的面：**按 surface ID 主动拉取上传** vs **录第二个进程的 GL** | 录第二进程：若它不用 GL，白干；拉面：需要客人机侧找到 surface 的 base/stride | widget 内容与壁纸 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0'''''''' | 上下文隔离 | 一次启动的宿主日志里，每条 context 有**自己的** current program / FBO / uniform（`ctx=0` 与 `ctx=1` 交替的行不互相覆盖）；标签 draw 的 `scissor`/`matrix4x4` 与它自己的流一致（打印 16 值）；无 0x500/0x502 | P1'''''''' |
| P1'''''''' | FBO 0 指向面板纹理 | `GLWIRE_FBO_LOG=1` 打出 `fbo 0 alias texture guest N`，且 N = 该帧面板 FBO 的 attachment；标签 draw 后该纹理非零像素 ≥1000 | P2'''''''' |
| P2'''''''' | 标签上屏 | 首页截图里图标下方出现 App 名称（与 `dsh.MD/IOSVM-GPU加速第十二阶段工程.md` 的基线逐区域对比）；截图 **≥300 KB**；同帧 diff ≥95% | 项目可用性 |
| P3'''''''' | widget 与壁纸 | `312x202`/`184x124` 或壁纸面以纹理进入宿主（或证明它们不由 GL 生成并给证据）；widget 方块内出现内容；主屏背景不是纯黑 | 项目可用性 |
| P4'''''''' | 收尾 | `0x502` 每次启动 **0 帧**；四格表刷新；widget 圆角不回归；`make test` 通过 | — |

---

## 三、实验计划（按顺序，每步都有停下来写结论的检查点）

### P0''''''''：上下文隔离（3–4 天）

1. **wire 头**：`u32 opcode | u32 length | u32 context`。
   - `encode.inc` 的 `wire_open(&w, opcode)` 加第三个字段；`remote.inc` 的每个 wrapper 第一个参数就是
     `void *context`（EAGL 表的约定），把它 intern 成 0..7 的编号写进头，不需要改 eagl.c 的调用点。
   - 请求行加版本（`R2 ...`）：宿主与客人机不同版本直接拒绝，避免旧镜像配新宿主。
2. **宿主**：`struct client` 里放 `struct ctx { EGLContext context; EGLSurface surface; GLuint framebuffer;
   struct glstate names; int width, height; } ctx[8]`；按需 `eglCreateContext(..., EGL_NO_CONTEXT, NULL)`
   （客人机两条 context 不共享，宿主也不共享）；记录里的 context 号变了就 `eglMakeCurrent` 切过去。
3. **帧**：请求行带「发送者 context 号」；读回从该 context 的 `drawn_framebuffer` 取；第二面板段表不变。
4. **回退开关**：`GLWIRE_CONTEXT_MERGE=1` 保持第十四阶段行为（一条流），A/B 与排障用。
5. **判据**：见 P0'''''''' 表。**先做本步、单独一发**：只要日志里两条 context 的 program/uniform 各自独立，
   这一步就算成——它不依赖标签。

### P1''''''''：FBO 0 与屏幕（1–2 天）

1. **客人机**：请求行加 drawable 纹理名（该 context 的 `e->colour`，或 `e->images[0].texture`）。
2. **宿主**：收到后为 FBO 0 建 alias：`glGenFramebuffers` + `glFramebufferTexture2D(COLOR_ATTACHMENT0,
   drawable_tex, 0)`；流里的 `glBindFramebuffer(GL_FRAMEBUFFER, 0)` 映射到这个 alias。
3. **判据**：P1''''''''/P2''''''''。若客人机给不出 drawable（eagl.c 的 `e->colour` 为 0 的路径），
   退路是「帧末把各 context 的替身 FBO 按 alpha 合到 picture」，只做定性并记录差异。

### P2''''''''：另一个进程的面（2 天，与 P0/P1 并行）

1. **先判定**：SpringBoard 的 312x202/184x124 是否由 GL 画（该进程有没有加载我们的 eagl、有没有第二条连接）。
   证伪则走「拉面」。
2. **若是 GL**：让第二个进程也连宿主（`MAX_CLIENTS=16` 已经支持）；它的画面写回自己的 IOSurface。
3. **若不是 GL**：wire 加一个「按 surface ID 拉取」的 opcode：客人机侧用 `IOSurfaceGetBaseAddress/Row` 找到像素，
   宿主按 pa 上传成纹理；backboardd 的 `texImageIOSurface:` 列表之外再补一轮绑定。
4. **判据**：P3''''''''。

### P3''''''''：收尾（1 天）

四格表（四份构建：纯本地 / 按引用 / W1.5 / W1.5+W3）、0x502=0、圆角不回归、`make test`、文档、推送。

---

## 四、不做（已被数据否掉，别重试）

1. **不再猜「64x19 是文字颜色」**：已证它是屏幕背景缩略（`texcoordD = NDC*0.5+0.5`），填白不改变画面是必然。
2. **不再把 legacy 格式展开当 0x502 的修复**：0x502 来自 `glDeleteProgram` 用未映射的客人机名字，展开只弄坏圆角
   （875→1600 白像素），十四阶段已把它保持默认关。
3. **不再用「替身 FBO + 事后合成」当最终方案**：标签 pass 的 scissor/matrix 来自另一条 context，合成不修状态。
4. **不再在一条 GL context 里混跑两条 EAGL context 的流**。
5. **不动内核**：本阶段是纯用户态 + 宿主（`tools/glwire`）工作。

---

## 五、验证环境与操作规范

- 一把 VM 锁 `/tmp/iosvm-vm.lock`；同一时间只跑一发。
- 启动：`tools/glwire/harness/boot-measure.sh <label> <root.apfs> <secs>`，`ZERO_RAM=1 MINFRAMES=… MINSHOT=… XCMD="…"`。
  **`MINSHOT` 同时是「界面到了」的阈值**：要跑满 900 s 又拿界面时刻，用 `MINFRAMES=999999 MINSHOT=200000`。
- 宿主探针（十四阶段新增，已进仓库）：`GLWIRE_FBO_LOG`、`GLWIRE_FBO_TRACE`、`GLWIRE_LABEL_DUMP`、
  `GLWIRE_PROGRAM_SOURCE`、`GLWIRE_ERROR_LOG`；旧的 `GLWIRE_DUMP_TEXTURES/DRAWS`、`GLWIRE_DRAW_LOG`、
  `GLWIRE_WATCH`、`GLWIRE_UNIFORM_LOG`、`GLWIRE_UNIFORM_PROGRAM`、`GLWIRE_FILL` 都留。
- 用户态重建：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`
  （wire 格式改动必须重建 root.apfs；宿主与客人机一起换）。
- 内核不动。

---

## 六、里程碑

| 天 | 目标 | 产出 |
|---|---|---|
| D1–D2 | P0'''''''' wire 头 + 宿主 per-context | 两条 context 各自的 program/uniform 日志 |
| D3 | P0'''''''' 标签 draw 的 state 对齐 | scissor/matrix 16 值 + 覆盖像素数 |
| D4 | P1'''''''' FBO 0 alias | `fbo 0 alias texture guest N` |
| D5 | P2'''''''' 标签上屏 | 截图 ≥300 KB + 逐区域对比 |
| D6 | P3'''''''' 入口结论 | widget/壁纸的「GL / 非 GL」判定与证据 |
| D7 | P4'''''''' 收尾 | 四格表、0x502=0、文档、推送 |

---

## 七、诚实的预期

- **P0'''''''' 是确定能做的一半**（结构改动、可编译可测），但它会暴露「对象名跨 context 别名」的旧账：
  现在宿主一个 context 的名字表要拆成 context 各一份，图标/字形纹理可能因此变错——所以回退开关是第一位的。
- **P1'''''''' 依赖客人机说出 drawable 是谁**：eagl.c 里 `e->colour` 与 `e->images[0].texture` 都有，
  但「哪条 context 的 drawable 是屏幕」需要一条显式协议（请求行加一个 u32）。
- **标签上屏有六成概率在 P1'''''''' 后达成**；剩下四成会卡在「CA 把标签画到默认 framebuffer 之后，到底谁负责合成」——
  那会把问题改写成「默认 framebuffer 的合成者是谁」，而不是「文字画到哪」。
- **P3'''''''' 的「不是 GL」概率高**：第二个进程一次都没连我们，所以按「主动拉面」设计。
- 本阶段**不追求**整机 fps 刷新：四格表仍按 120 帧/格，先把上下文隔离做对。

---

## 八、与第十四阶段的分工

| 项 | 第十四阶段 | 第十五阶段 |
|---|---|---|
| fbo 1 的身份 | **定案（宿主替身 / 客人机 FBO 0）** | 变成面板纹理的 alias（P1''''''''） |
| 标签 pass 为什么 0 像素 | **定位（两条 context 合流）** | 隔离后重测（P0''''''''） |
| 文字 shader | **解码（15=mask / 13=darken backdrop）** | — |
| 0x502 | **归零（glDeleteProgram 守卫）** | 保持 |
| widget/壁纸 | **入口定位（另一个进程的面）** | 拉取/录制（P2''''''''） |
| 标签上屏 | 未达成（247,349 B） | **P1''''''''/P2''''''''** |
| 四格表 | 未刷新 | **P4''''''''** |

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

---

## 九、起步（2026-11-06 10:35）

**已落地（本阶段第一笔代码）**

- **wire 头有了 context 字段**：`u32 opcode | u32 length | u32 context`（版本 2，12 字节）；版本 1 仍是 8 字节。
  客人机在请求行里声明版本；只给 8 个字段的旧客人机走版本 1、context 0——**第十四阶段的镜像还能启动/重放**。
  读取方显式收版本（`wire_header_bytes(version)`、`wire_header_context(record, version)`），因为版本是每条连接的
  事实，不是进程全局。`GLWIRE_CONTEXT_LOG=1` 打印一帧里每条 context 的第一条记录（第十五阶段 P0'''''''' 要的数据）。
  提交 `f4b08af`。
- **测试两种布局同一画面**：`map_test`/`map_stream`/`frame_replay` 记版本 2；`e2e_test.py` 把同一份记录发两遍
  （带 context 头 / 剥掉 context 头），要求两遍的 picture 逐字节相同——**PASS**。`make test` 现在先建
  `host_glwire` 再跑 e2e，避免旧二进制混过测试。
- **ADR-011**（`docs/adr/ADR-011-gl-context-in-the-wire.md`）与本计划一起提交。

**下一步（P0'''''''' 的剩余，按顺序）**

1. 客人机：`remote.inc` 把每个 trampoline 已经收到的 `void *context` intern 成 0..7 写进头；请求行发版本 2。
   重建镜像：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`。
2. 宿主：`struct ctx { EGLContext; GLuint framebuffer; struct glstate names; }` 每 context 一份（`eglCreateContext`
   不共享），记录里的 context 号变了就 `eglMakeCurrent`；`GLWIRE_CONTEXT_MERGE=1` 回退成第十四阶段行为。
3. 第一发验证：`GLWIRE_CONTEXT_LOG=1` + 900 s，看两条 context 的 program/uniform 各自独立、标签 draw 的
   scissor 与顶点自洽（P0'''''''' 判据）。
4. 然后才做 P1''''''''（FBO 0 alias）与 P2''''''''（标签上屏）——顺序不能倒：没有隔离，alias 只会把错误的状态
   写进正确的纹理。

---

## 十、进展（2026-11-06 12:00，上下文测量与跨帧 binding）

**做了什么**

1. **客人机侧 context 标记已上线**：`remote.inc` 的 57 个 wrapper 各加一条
   `remote_note_context(context)`，`eagl_remote_begin()` 设 `wire_set_record_version(2)`，请求行第九字段 = 2。
   重建 `root.apfs`（`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`，BUILD_RC=0）。
2. **一发 900 s 的测量**（`/tmp/s14ctx.sh`）：3700 帧，每条记录都带 context，宿主打印
   `client 17 speaks record version 2`；**全部 draw 都是 ctx 0**（20000 条 draw 日志），
   `glwire: context 0 at opcode 19, record 0 of frame 1` 之后没有第二条 context 的行。
   → 第二条 `EAGLContext` 被创建但从未录到 GL 调用。**ADR-011 的动机被推翻**（见其 Amendment）。
3. **发现真正的状态丢失**：`eagl.c` 用 `glFlush` 结束一帧，CA 的 pass 会跨过去继续；宿主每帧开头强制绑回
   自己的 framebuffer，续接的 draw 就落到替身上。加 `GLWIRE_KEEP_BINDING=1` 跨帧保留客人机的 binding 后：
   label draw 的 fbo 从 1 变成真正的层 FBO（guest 6 `750x200`、guest 17 `64x19`、guest 23 `64x9`…），
   矩阵与 scissor 自洽。记录在 **ADR-012**。
4. **覆盖实验**：`GLWIRE_LABEL_WHITE=1` 用同一批顶点 + 纯品红片元程序重画 label draw：
   单笔 11,520–17,010 个非零像素，706 笔里 412 笔非零，截图上品红块正好在字形位置。
   → 几何/目标/裁剪全对；换成 CA 的程序则恒 0，所以缺的是**片元采样（或乘子）**。
5. **采样实验（在跑）**：`GLWIRE_LABEL_SAMPLE=1` 把 u0 纹理读回（178x90：14,166 非零、alpha>127 只有 2；
   130x42：591 个 alpha），`GLWIRE_LABEL_ALPHA=1` 把 atlas alpha 直接画成红色，用来区分「采样到 0」
   与「程序/乘子是 0」。

**这一节的结论**：P0'''''''' 的「上下文隔离」**不是**本阶段的答案（只有一条 context 在录）；本阶段的
P1'''''''' 实际由 **ADR-012（跨帧 binding）** 开启，标签问题上移一层——从「画到哪」推进到「片元采样」。

**下一步**

1. 完成 `GLWIRE_LABEL_ALPHA=1` 的判定：若红色块出现，问题在 CA 的片元程序/ uniform；若仍为 0，
   问题在采样器或纹理绑定（用 2D 采样 vs 纹理目标、sampler 的 min filter）。
2. 修好后，`GLWIRE_KEEP_BINDING` 默认打开，重跑 P2'''''''' 的截图判据（≥300 KB、diff ≥95%）。
3. 四格表与 P3'''''''' 收尾按原计划。

**13:04 的补充（标签一次上屏，未稳定）**

一发 `GLWIRE_KEEP_BINDING=1 GLWIRE_LEGACY_FORMATS=1` 的启动里标签全部出现：截图 **291,909 B**，
Weather/Calendar（widget 标题）与 FaceTime/Calendar/Photos/Mail/Notes/Reminders/TV/Podcasts/Health/Wallet
（App 名称）齐全，证据图 `docs/demo/s15-labels-20261106.png`；同一发日志里 283 笔 label draw 落在 750x1624
的层 FBO、118 笔产生了带 alpha 的像素。但同 env 复跑与只带 KEEP+LEGACY 的一发都是 238–239 KB、无标签
（0 笔大目标 draw）。**结论**：跨帧保留 binding 是对的，但「保留到哪一次 bind」仍是竞态。

因此 P0'''''''' 的最后一步改成：**客人机自报 binding**——`eagl.c` 本来就在每个 context 里跟踪
`e->framebuffer`，在每帧的 flush 路径里给下一帧开头补一条 `glBindFramebuffer(FRAMEBUFFER, e->framebuffer)`
（或一条带 binding 的帧头字段），宿主照单绑定，不再猜。这样 frame boundary 与 CA 的 pass 再无关系。

<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十四阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十四阶段工程文档

> 建立时间：2026-11-06 08:00
> 前置：第十三阶段（`IOSVM-GPU加速第十三阶段工程.md` §八 P1 审计：文字程序无颜色 uniform、64x19 填白无变化、
> 文字 draw 落在 fbo 1 且面板里没有一笔采样它）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **屏幕能起、图标能画、双面写回稳定**：界面后 239 KB 级截图、同帧 diff 97.7%，主屏与图标都在。
2. **缺三样内容**：App 标签文字、widget 内容、wallpaper。第十三阶段把文字这条链收到最后一步：
   文字 draw（program 13）落在 **fbo 1**（750x1624），写出后**全 0**；面板（fbo 3）里**没有任何一笔 draw 采样 fbo 1**。
3. **十三阶段排除掉的可能**：uniform 映射错位（值都到对了）、scissor 裁剪（稳态 off）、64x19 纹理是颜色来源
   （填白后屏幕无变化）、wire 缺 `glTexSubImage2D`/`glBlitFramebuffer`（都不是文字颜色的入口）。
4. **两条并行的旧账**：460 s 启动空洞 = lsd 的串行 LaunchServices 工作（已量清、未修）；宿主 legacy 格式
   （`glerror 0x502`，每次启动 8 帧）未收尾。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | **fbo 1 是谁**：图层表面（需要合成到面板）vs 双缓冲的另一块面板（需要写回两面） | 判错方向：把时间花在「补合成」上，而其实是写回只写了一面 | P0 的全部工作方向 |
| D2 | 若确认是图层表面且 CA 不自合成：**自己补合成**（宿主把 fbo 1 按图层矩形叠到面板）vs **修 CA 的合成路径**（改 backboardd 的 CA 或 wire 补 opcode） | 自己补：快速看到文字，但可能与 CA 的真实布局不一致；修 CA：正确但深不见底 | 标签上屏的可控路径 |
| D3 | 内容与性能的次序：**先内容后四格表** vs 并行 | 并行：四格表的数字会被「缺内容」污染（第十二阶段已踩过） | 一张可信的结论表 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0''''''' | fbo 1 是谁 | 一次启动里，`GLWIRE_FBO_LOG=1` 打出 fbo 1 的 **尺寸、附件类型（纹理/renderbuffer）、附件名**，且与 `drawn_framebuffer`/读回目标对得上；结论写成「图层表面」或「第二面板」其一，附 3 条原样日志 | P1''''''' |
| P1''''''' | 标签文字上屏 | 首页截图里，图标下方出现 App 名称（与 `dsh.MD/IOSVM-GPU加速第十二阶段工程.md` 的基线截图逐区域对比）；截图 **≥300 KB**；同帧 diff ≥95% | P2'''''''、P3''''''' |
| P2''''''' | wallpaper 与 widget 内容 | 主屏背景不再是纯黑（wallpaper 出现），两个 widget 方块内出现内容（不是纯白）；判据同 P1（截图 + 区域对比） | 项目可用性 |
| P3''''''' | 460 s 空洞 | 禁用 lsd 与预置 csstore 两组对照各一次启动，**启动到界面 −30%**；做不到写清为什么 | 整段 fps |
| P4''''''' | 四格表与宿主 legacy 格式 | 四格（纯本地/按引用/W1.5/W1.5+W3）各 120 帧/X 秒；`glerror 0x502` 每次启动 **0 帧**；widget 圆角不回归 | 结论表 |
| P5''''''' | 文档与提交 | `make test` 通过、`dsh.MD` 与 `iosvm/docs` 同步、推 GitHub | — |

---

## 三、实验计划（按顺序，每步都有停下来写结论的检查点）

### P0'''''''：fbo 1 的身份（半天）

1. 宿主加 `GLWIRE_FBO_LOG=1`：`glBindFramebuffer` 时对每个首次出现的 fbo 打印
   `glGetFramebufferAttachmentParameteriv(COLOR_ATTACHMENT0)` 的类型/名字、纹理尺寸、以及本帧的 `drawn_framebuffer`。
2. 一发 900 s（`/tmp/s13scis.sh` 的形态，加该变量），确认：
   - fbo 1 的附件名是否等于某块 750x1624 的**纹理**（图层表面），或 renderbuffer（面板）；
   - 它是否与 `eagl_remote_surface2`（第二块面板）或 `client->framebuffer`（宿主面板 FBO）对应。
3. 判据：见 P0'''''''。若 fbo 1 = 宿主面板 FBO，则问题变成「面板渲染被读回目标覆盖」——直接跳到 P1 的第二分支。

### P1'''''''：标签文字上屏（1–2 天）

- **分支 A（fbo 1 是图层表面）**：在宿主把 fbo 1 的内容按图层矩形合成到面板（先全屏叠加做定性实验，再对矩形）。
  判据：标签出现。随后再决定是否改成「修 CA 合成」。
- **分支 B（fbo 1 是第二面板）**：查 `eagl_remote_surface2` 的写回是否覆盖两面（第十二阶段的双面写回只对面板 1/2），
  把第三块/第二块面板也纳入写回。
- **分支 C（两边都不是）**：贴出证据行，把「文字到底画到哪」重新定义，再定方案。

### P2'''''''：wallpaper 与 widget（1 天）

1. wallpaper：查它是不是一张面（第十二阶段的 24 张面里没有 wallpaper 尺寸），或由某个没录到的 CPU 绘制完成；
   若是后者，抓一张「客人机自己的合成结果」对照（本地模式 `glwire_local=1` 截图）。
2. widget：白块是 widget 层的背景色；内容要么来自 SpringBoard 的快照面（`184x124`），要么来自内联绘制。
   先确认「快照面有没有被绑成纹理」，再决定补合成还是补绘制。

### P3'''''''：460 s 空洞（1 天，与 P1/P2 并行）

- 对照组 A：禁用 lsd（`launchctl unload` 或 launchd 里 Disabled），900 s 一发，记启动到界面时间；
- 对照组 B：预置 csstore（若镜像里有），同上；
- 判据：−30%，否则写清「lsd 是 SpringBoard 的硬依赖」等具体原因。

### P4'''''''：四格表 + legacy 格式（1 天）

- 四格：纯本地 / 按引用 / W1.5 / W1.5+W3，各 120 帧，记「120 帧 / X 秒」；
- legacy：把 `host_legacy_teximage` 从「rgb=0」改成**按采样语义分叉**（mask 用 rgb、文字用 a），或按 env 覆盖；
  判据：`0x502` 归零、圆角不回归。

### P5'''''''：收尾（半天）

`make test`、两份文档同步、commit/push。

---

## 四、不做（已被数据否掉，别重试）

1. **不再试 CAImageQueue 的 interpose / fishhook**（0 调用，SpringBoard 会崩）。
2. **不再试 `CA_DEBUG_TRANSACTIONS`**（这版 QuartzCore 不认）。
3. **不再用 `ld64.lld` 链接内核**（`__TEXT` 落 0，loader 拒载）。
4. **不再把「宿主 legacy 格式展开成白」当修复**（圆角变直角，标签不动）。
5. **不再把「填白 64x19」当修复**（十三阶段已证：屏幕无变化）。

---

## 五、验证环境与操作规范

- 一把 VM 锁 `/tmp/iosvm-vm.lock`；同一时间只跑一发。
- 启动：`tools/glwire/harness/boot-measure.sh <label> <root.apfs> <secs>`，`ZERO_RAM=1 MINFRAMES=… MINSHOT=… XCMD="…"`。
- 客人机探针：`surfsample=<id>`、`texlog=1`、`surftrace_of=<name>`、`ustacks=top`。
- 宿主探针：`GLWIRE_DUMP_TEXTURES`、`GLWIRE_DUMP_DRAWS`、`GLWIRE_DRAW_LOG`、`GLWIRE_WATCH=<WxH>`、
  `GLWIRE_UNIFORM_LOG`、`GLWIRE_UNIFORM_PROGRAM`、`GLWIRE_FILL`（本阶段新增 `GLWIRE_FBO_LOG`）。
- 内核构建：`LANG=C LC_ALL=C IOSVM_USE_LLD=0 tools/make-kernel.sh -j8`（cctools 在 `/tmp/cctools`，重启会丢）。
- 用户态构建：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`。

---

## 六、里程碑

| 天 | 目标 | 产出 |
|---|---|---|
| D1 | P0''''''' 定案 | fbo 1 的身份（3 条证据行） |
| D2–D3 | P1''''''' 标签上屏 | 截图 + 逐区域对比 |
| D4 | P2''''''' wallpaper/widget | 截图 + 内容项清单 |
| D5 | P3'''''''/P4'''''''/P5''''''' | 启动时间数字、四格表、0x502 归零、文档推送 |

---

## 七、诚实的预期

- **P0''''''' 是确定能出结论的**（纯探针），但它可能给出「fbo 1 是第二面板」这种意外答案——那会推翻第十三阶段
  「合成缺失」的推断，反而更容易修。
- **P1''''''' 有两成概率卡在「客人机自己也不画」**：那时只能写清「CA 的哪个状态让它不画」，并把交付改成
  「内容缺失的可复现最小案例」。
- **P2''''''' 依赖 P1''''''' 的结论**：如果标签是靠「自己补合成」上去的，wallpaper/widget 也能用同一招；否则要另找入口。
- **P3''''''' 大概率查得清、修不动**（lsd 是 SpringBoard 依赖）。
- 本阶段**不追求**「整机 fps 世界纪录」：四格表的判据仍是 W1.5+W3 ≥4 fps，先保证可信。

---

## 八、与第十三阶段的分工

| 项 | 第十三阶段 | 第十四阶段 |
|---|---|---|
| 文字程序颜色 | 审计完成（无颜色 uniform） | — |
| fbo 1 身份 | 未做 | **P0'''''''** |
| 标签上屏 | 未完成 | **P1'''''''** |
| wallpaper/widget | 未做 | **P2'''''''** |
| 460 s 空洞 | 未做 | **P3'''''''** |
| 四格表 / 0x502 | 未做 | **P4'''''''** |
| 文档推送 | 已推（`cc0863a`、`e6641e3`） | **P5'''''''** |

---

## 九、结果（2026-11-06 09:45）—— 三件做完，标签卡在「两条 context 合流」

> 本节的每一行都来自这次启动的日志/转储；命令与产物路径写在行内。

### 9.1 P0'''''''：fbo 1 的身份 —— **宿主替身，对应客人机的 FBO 0（屏幕）**

不是图层表面，也不是第二面板。三条证据：

```
glwire: fbo gen client=17 n=1: guest 1 -> host 2
glwire: fbo bind client=17 target 0x8d40 guest 1 host 2 attachment none ... complete 0x8cd7 drawn-so-far 0
glwire: fbo attach client=17 fbo host 2 guest 1 attachment 0x8ce0 kind 0x1702 name 1 texture guest 1 host 1 level 0 750x1624 internal 0x1908
```

每帧一笔 `glGenFramebuffers`，客人机的 FBO 名 1/2/… 映射到宿主 2/3/…，颜色附件就是该帧刚上传的面板纹理
（`internal 0x1908`，750x1624）。而 `host 1` 是本渲染器自己的 framebuffer（`make_frame_buffer()` 拿到的第一个名字），
summary 把它记成 `own 1`，并且把**客人机默认 framebuffer 0** 的 draw 记在它名下：

```
glwire: fbo summary frame frame 1200 client 17 own 1 drawn 2 readback-ready 0:
  [guest 1 host 2 texture 1 750x1624 binds 3 draws 5] [guest 3 host 2 texture 7 750x1624 binds 606 draws 3433]
  [guest 4 host 3 texture 4 750x1624 binds 602 draws 3432] ... [guest 0 host 1 none 0 0x0 binds 0 draws 648]
```

标签 pass 是**每帧的第一笔 draw**，在任何 `glBindFramebuffer` 之前：

```
glwire: draw#55 elements ... program 11 (guest 11) fbo 1 viewport 0,0 750x1624 scissor on 0,0 750x200 status 0x8cd5 drawbuffer 0x8ce0 u0:host12(guest16) ...
glwire: draw#57 elements ... program 13 (guest 13) fbo 1 viewport 0,0 750x1624 scissor on 0,0 184x90 ... u0:host13(guest17) u1:host11(guest15) u2:host10(guest14)
glwire: draw#58 elements ... program 15 (guest 15) fbo 1 viewport 0,0 750x1624 scissor on 0,0 184x90 ... u0:host15(guest18) u1:host11(guest15) u2:host10(guest14)
```

（长行按排版折行，字段原样。）

**结论**：`fbo 1` = `client->framebuffer`，是客人机 **FBO 0（EAGL 默认 framebuffer，也就是屏幕）** 的替身。
它画完既不读回、也不参与合成——十三阶段「fbo 1 不被任何一笔 draw 采样」到这里有了名字。

### 9.2 P1'''''''：标签为什么是 0 像素 —— **两处已澄清，一处待解**

> **2026-11-06 12:00 修订**：第十五阶段的测量（记录头带 context）推翻了本节原先「两条 EAGL context
> 合流」的结论。下面按最新证据重写；ADR-011 保留 wire 格式与版本交涉（它们仍有用），但它的动机作废，
> 真正的修复记录在 ADR-012。

1. **文字 shader 的解码不变**：program 15 片元 = `vec4(t0.a)`（纯 mask）；program 13 片元 =
   `darkenSover(color*glyph.a, backdrop)`，其中 `S = S * y^4`（y = backdrop 亮度），backdrop 全 0 时输出恒 0。
   那张 64x19 是屏幕背景缩略（`texcoordD = NDC*0.5+0.5`），不是颜色。
2. **一：eagl.c 的 frame boundary 不是 CA 的 pass boundary。** `eagl.c` 在 `glFlush` 结束一帧，而
   CoreAnimation 的 pass 会跨过这次 flush 继续下去；宿主每帧开头强制绑回自己的 framebuffer，于是**续接的
   draw**落在 750x1624 的替身上。证据：`GLWIRE_KEEP_BINDING=1` 之后，label draw 的 fbo 从 1 变成真正的层
   FBO（guest 6 `750x200`、guest 17 `64x19`、guest 23 `64x9`…），并且状态自洽——`64x19` 目标 +
   `scissor 0,0 64x19` + `vertex_matrix 0.0312…-15.0 / 0.1053…-86.58` 把顶点精确映射进该目标，status complete。
   **修**：跨帧保留客人机的 binding（ADR-012，`GLWIRE_KEEP_BINDING`）。
3. **二：只有一条 context 真的在录。** 一次 3700 帧的启动，每条记录都带 context（版本 2），**全部是 ctx 0**；
   第二条 `EAGLContext` 被创建（两条 `an OpenGL ES 3 context for CoreAnimation`）但从未发出一条 GL 调用。
   所以「两条流互相覆盖」不成立——上一版据此下的结论作废。
4. **三：几何、目标、裁剪都对，缺的是片元采样。** 用同一批顶点、把片元程序换成纯品红
   （`GLWIRE_LABEL_WHITE=1`）：单笔 label draw 覆盖 **11,520–17,010** 像素（706 笔里 412 笔非零），
   截图上的品红块正好落在字形位置。换成 CA 自己的程序则恒 0——所以问题不是「画到哪」、也不是「有没有覆盖」，
   是**采样（或把采样乘没的那一项）**。
5. **仍在查**：glyph atlas（`GL_ALPHA`，宿主上经 legacy 展开成 RGBA8）读回是有内容的（178x90：14,166 个
   非零、alpha>127 只有 2；130x42：591 个 alpha），texcoord 是 `[0,1]` 全图，但 CA 的片元程序采样后仍为 0。
   正在用 `GLWIRE_LABEL_ALPHA=1`（把 atlas 的 alpha 直接画成红色）区分「采样到 0」与「程序/乘子是 0」。
6. **判定**：标签的链路已经从「不知道画到哪」推进到「目标已修、覆盖已证、只剩片元采样」。
7. **四：KEEP+LEGACY 的一发里标签真的上屏了，但不稳。** 2026-11-06 13:04 的一发
   （`GLWIRE_KEEP_BINDING=1 GLWIRE_LEGACY_FORMATS=1`）：截图 **291,909 B**，Weather / Calendar（widget 标题）
   与 FaceTime / Calendar / Photos / Mail / Notes / Reminders / TV / Podcasts / Health / Wallet 全部出现
   （`docs/demo/s15-labels-20261106.png`）。同一发的日志：**283 笔 label draw 落在 750x1624 的层 FBO 上，
   其中 118 笔产生 858–1,696 个带 alpha 的像素**。但同 env 复跑（13:09）与只带 KEEP+LEGACY 的一发（12:04）
   都是 **0 笔大目标 draw、238–239 KB、无标签**。所以「标签上屏」是一次真实的成功，却**不是稳定状态**：
   每帧开头保留下来的 binding 有时正好是标签层、有时不是——**frame boundary 相对 CA 的 pass 仍然是任意的**。
   下一步（第十五阶段）不是继续猜 binding，而是**让客人机在每帧开头自报 `e->framebuffer`**（eagl.c 本来就知道），
   宿主照单绑定，剪掉这个竞态。

### 9.3 P2'''''''：widget 与壁纸的入口 —— 在**另一个进程**里

```
MiniIOSurface: SpringBoard[10] created surface 3: 184x124 'BGRA', 736 bytes per row, 0 planes, 98304 bytes
MiniIOSurface: SpringBoard[10] created surface 4: 312x202 'LA08', 640 bytes per row, 0 planes, 131072 bytes
hakios-remote: s11 f 1 images 28 0:2T- 750x1624/3008 1:1T- 750x1624/3072 2:5T- 16x16/64 3:6-- 180x180/720 ... 16:19-- 28x65/128 ... 23:26T- 19x19/76 ...
```

- 界面帧的 28 张 image 里：两张 750x1624 面板、一批 180x180 图标、28x65/28x95/28x28/6x85/6x6/19x19/16x16，
  **没有 312x202、没有 184x124，也没有任何壁纸尺寸的面**。
- 两块 widget 白块是 widget 层的背景色（矩形填充），内容面由 SpringBoard 创建却从没以纹理进 backboardd 的流；
  壁纸同理。它们和标签同源：像素在「另一个进程/另一条 context」里，wire 只录了 backboardd 的一条合并流。

### 9.4 P3'''''''：460 s 空洞（lsd 对照组）

- **窗口与 T_ui**（`/tmp/s12ca.log`）：帧 2 `20:12:02.383` → 帧 3 `20:18:37.931`（`[Display] Set Blanked: YES`，
  `interval 395.96 s`）→ 帧 4 `20:19:43.921`（`Set Blanked: NO`，66.08 s）。**T_ui = 507.9 s**。
- **lsd 的行为**：`ipc (mach)` 按需拉起（13 个 MachServices）；窗口里 **702 条**
  `process lsd[32] denied attempt to escape CPU monitor (missing required entitlement)`（五次启动全是 702 条，
  首条 20:11:55、末条 20:18:33，与窗口同起同落，内核 `task_policy.c:2867`）；
  启动期 CPU 中位 124%、≥4 核仅 7%（470 个 1 s 样本），lsd 累计 27,884%（样本和，峰值 3.2 核）。
  SpringBoard 20:11:22 拉起后**静默 434 s**，20:18:36 才向 backboardd 报到 → 「在等，不在算」。
- **对照 B（预置 csstore）跑不了**：全盘 0 个 csstore；`/private/var` 每次启动是空 ramfs（`111 directories, 8 files`）；
  无数据卷/无 shell/只读 md0，取不出客人机里的库。补它需要数据卷或导出程序，不在本阶段预算内。
- **对照 A 可跑**：唯一改动 `userland/root/LaunchDaemons/com.apple.lsd.plist` 顶层 `Disabled=true`；
  顺序必须先 build 再改 plist 再 mkapfs（`build.sh:454` 的 mkjobs.py 每次重写该 plist）。
  硬链接克隆 + `mkapfs.py`（不编译，1.26 s）：未改动的树重建出的镜像与 19:14 的 `root.apfs` **逐字节相同**；
  改后只差 3435 B，`apfs.py cat` 读回 `Disabled=True`。
- **两次对照的数字**（同一天、同一脚本、同一 env；base 09:40:17 起、disabled 09:56:21 起）：
  - **base**：lsd spawn 1、702 denials；帧 2→3 **549.28 s**、3→4 **96.99 s**；**T_ui = 705.2 s**
    （`Set Blanked: NO` 09:52:02.177）；界面截图 236,083 B（690 s 时）。
  - **disabled**（镜像只差 `Disabled=true`，3435 B）：lsd spawn **0**、denials **0**；帧 2→3 **6.83 s**
    （对比 base 的 549.28 s，**−98.8%**）。**但界面没来**：900 s 内 SpringBoard 被拉起 **106 次**、清理 **131 次**，
    650 条 `com.apple.lsd.* ... 3: No such process`（`mapdb` 等），宿主只有 3 帧、最大截图 14,316 B（黑屏），
    没有 `Set Blanked: NO`。
- **判据结论**：460 s 空洞的**成因**是 lsd，这一点被两组对照坐死（去 lsd → 帧间空洞 549 s → 6.8 s）；
  但「启动到界面 −30%」**不成立**——SpringBoard 依赖 LaunchServices，禁用 lsd 后进不了界面。这就是「查得清、修不动」：
  真正的修复是让 lsd 的 702 个串行动作变快（预置 csstore / 并行化），不是去掉它；而 csstore 预置在本镜像不可用（上一段）。
- **判据**：见 P3''''''' 表；单发 A/B 只有在效果远大于抖动（五次空洞 288.84–459.44 s，T_ui 一天从 507.9 漂到 566.8）
  时才可判；否则按「不可判 + 两次启动」记录。

### 9.5 P4'''''''：0x502 归零 + 圆角不回归（**达成**）

- **0x502 的真正来源不是 legacy 格式**：`GLWIRE_ERROR_LOG=1` 逐 opcode 查错，0x502 只出现在 opcode 32
  （`glDeleteProgram`）：
  ```
  glwire: glerror 0x502 after opcode 32 (6705 records in)
  glwire: glerror 0x502 after opcode 32 (90590 records in)
  glwire: glerror 0x502 after opcode 32 (9132 records in)
  ```
  `dispatch.inc` 的 case 32 用 `names_program_take()`；它对**未映射**的客人机名字回落成「客人机名字」，
  拿这个数去删宿主的对象 → `GL_INVALID_OPERATION`（还可能删错对象）。
- **修**：未映射就 no-op（`dispatch.inc` case 32）。验证运行（`/tmp/s14p4.sh`，`GLWIRE_LEGACY_FORMATS` 保持默认关）：
  **整次启动 0 条 0x502**（首 12 帧逐 opcode + 帧 1500 的 dump 帧）；`0x500` 只剩一条 `glGetString`
  （不属本判据）。
- **圆角**：widget 左上角 40x40 的白像素 **875/1600**，与 s12ca 基线（未展开）逐值相同；打开
  `GLWIRE_LEGACY_FORMATS=1` 的 s14p0/s14p0b 是 **1600/1600**（直角）。截图 **247,349 B**，界面正常。
  结论：legacy 展开保持默认关；它不是 0x502 的修复，rgb=0 版本同样破坏圆角。
- **四格表**：**本阶段没有刷新**。四格要四份客人机构建（纯本地 / 按引用 / W1.5 / W1.5+W3），本阶段时间用在
  上下文隔离的证据上；当前构建（按引用 + W1.5 + W3）的稳态是 0.02–0.16 s/帧（7–50 fps 抖动，中位约 9 fps），
  第十一阶段的四格表仍是唯一完整表，明确标注「未刷新」。

### 9.6 判据对照

| # | 判据 | 结果 |
|---|---|---|
| P0''''''' | fbo 1 的尺寸/附件/名字 + 与读回目标对得上 + 3 条原样日志 | **达成**：宿主替身 / 客人机 FBO 0；§9.1 |
| P1''''''' | 标签上屏、截图 ≥300 KB、diff ≥95% | **部分达成**：一发里标签全部上屏（291,909 B，`docs/demo/s15-labels-20261106.png`），但同 env 复跑无标签（238 KB）——跨帧 binding 已修，稳定性待「客人机自报 binding」，§9.2/§9.7 |
| P2''''''' | 壁纸不再是纯黑、widget 内有内容 | **未达成**；入口定位到「另一个进程的面」，§9.3 |
| P3''''''' | 两组对照、启动到界面 −30%，否则写清为什么 | **部分**：csstore 不可能；lsd 对照 A 见 §9.4 的数字/结论 |
| P4''''''' | 四格表 + 0x502=0 + 圆角不回归 | **0x502 与圆角达成**（§9.5）；四格表未刷新（已说明） |
| P5''''''' | `make test`、两份文档同步、推 GitHub | 见本节提交记录 |

### 9.7 这一阶段真正的收获

1. **fbo 1 从「未知对象」变成「有名字的替身」**：客人机 FBO 0 = 屏幕；标签 pass 的落点与它不读回的事实都清楚了。
2. **文字链路闭合**：两个片元程序 + 64x19 的真实语义（屏幕背景缩略）+ 1313 笔 0 像素的实测。
3. **缺内容的统一解释**：标签 = 另一条 context；widget/壁纸 = 另一个进程。不是三个互不相干的问题。
4. **0x502 收干净**：一个真实的「用客人机名字删宿主对象」的 bug，守卫 + 归零。
5. **P3 的判据本身被修正**：`MINSHOT` 同时是界面阈值（陷阱）；单发 A/B 的抖动 1.6 倍，必须先跑同日 base。

### 9.8 提交与推送

| commit | 内容 |
|---|---|
| `d0d311c` | `tools(glwire)`：探针进了 `dispatch.inc` 之后，`frame_replay`/`map_test` 的编译被修好（`probe_stubs.inc`），`make test` 重新可跑 |
| `5257977` | `host(glwire)`：`GLWIRE_FBO_LOG`/`FBO_TRACE`/`LABEL_DUMP`/`PROGRAM_SOURCE`/`ERROR_LOG` + draw 的 completeness/drawbuffer/16 值矩阵 |
| `585fde4` | `fix(glwire)`：`glDeleteProgram` 对未映射的客人机名字 no-op（0x502 归零） |
| `7ba537a` | `docs(stage 14)`：本节的结果 |
| `873db74` | `docs(stage 15)`：第十五阶段计划 + ADR-011 |
| `f4b08af` | `tools(glwire)`：记录头带 context（版本 2），两种布局都过测试——第十五阶段第一笔代码 |
| `945ea0c` | `docs(stage 15)`：起步记录 |

推送：`origin/main`（`3d5e12a..` 之后全部）。

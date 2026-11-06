<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十六阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十六阶段工程文档

> 建立时间：2026-11-06 14:00
> 前置：第十五阶段（结果见 `IOSVM-GPU加速第十五阶段工程.md` §十/§十一：wire 带 context、客人机自报 binding、
> 一发里标签全部上屏 291,909 B 但不稳定）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **标签在 13:04 的一发里全部上屏**：截图 291,909 B，Weather / Calendar（widget 标题）与十个 App 名称齐全
   （`iosvm/docs/demo/s15-labels-20261106.png`）。但同 env 复跑是 238 KB、无标签——**不是稳定状态**。
2. **不稳定的机理已定位，但自报 binding 没有解决它**：`eagl.c` 用 `glFlush` 结束「帧」，CA 的 pass 会跨过去继续；
   宿主原来每帧强绑自己的 FBO。第十五阶段先改成「宿主保留上一帧的 binding」，再改成「客人机自报 `e->framebuffer`」
   （`glBindFramebuffer` 是 `!local`，trampoline 不调本地实现，所以加了 `eagl_note_bind()`）。
   验证（s15bind2，14:01）：**889 笔 label draw、34 笔大层、14 笔有像素、截图 239,632 B、无标签**——自报只是让宿主
   不再猜，CA 在这发里绑的是小块 label surface，而它们画出来仍为 0。
   **进一步分类（14:40，四发对照）**：品红对照 850/850 全非零（覆盖已证）；真程序下 program 13 全 0（backdrop 空，
   设计使然）、program 15 只在大层上有效（s14dia 125/283）；小块 label surface **只收到 program 13**，所以永远 0。
   3 ms 帧延迟（s16delay）没有带来大层 draw——**主机速度假设否掉**。
3. **文字链路已解码**：program 15 = `vec4(glyph.a)`（纯 mask）；program 13 = `darkenSover(color*glyph.a, backdrop)`，
   `S *= y^4`、backdrop 为空则恒 0。片元采样（atlas alpha / texmat0）是标签链上的最后一个未知。
4. **widget 内容与壁纸仍缺**：SpringBoard 建了 `312x202 LA08`、`184x124 BGRA` 两面，但它们从未进入 backboardd 的
   28 张 image 列表；壁纸所需的服务（`posterboardservices`）在这版镜像里查不到。
5. **圆角回归**：legacy 格式展开（GL_ALPHA → RGBA）让 widget 圆角从 875/1600 白像素变成 1600/1600（直角）；
   0x502 已归零，四格表未刷新。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 标签稳定之后：**继续查片元采样** vs **宿主端「补字」**（拿 glyph atlas 自己画） | 补字：快、但一旦 CA 的真实路径修好就变成第二份字；查采样：可能还要挖几层 | 真实的 App 名称 |
| D2 | 圆角：**把 legacy 展开改成保留采样语义的分叉**（`GL_ALPHA` 用它该有的 alpha、rgb 不动内容）vs 接受直角 | 接受：画面回归，判据写不过；分叉：要弄清那张面给谁用 | widget 圆角 + 标签同时正确 |
| D3 | widget/壁纸：**按 surface ID 主动拉取** vs **录第二个进程** | 录进程：它不用 GL 就白干；拉面：要客人机侧找到 surface 的 base/stride | widget 内容与壁纸 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0''''''''' | 标签稳定 | **连续 3 发启动**，首页都有 App 名称（截图 ≥300 KB、同帧 diff ≥95%）；3 发里 ≥2 发达标即记为「达成」 | P1''''''''' |
| P1''''''''' | 文字质量 | 无重影（Reminders 处只有一份清晰的字）；widget 圆角 **875/1600**；标签颜色为白（不是 mask 灰） | 项目可用性 |
| P2''''''''' | widget 内容 | `312x202`/`184x124` 面以纹理进入宿主（日志里有 attach/upload 行），widget 方块内出现内容；否则给出「不由 GL 生成」的证据链 | 项目可用性 |
| P3''''''''' | 壁纸 | 主屏背景不是纯黑；或给出「上游没有这张图」的完整证据链（服务不存在 + 镜像里没有资源） | 项目可用性 |
| P4''''''''' | 性能与收尾 | 四格表（纯本地/按引用/W1.5/W1.5+W3）各 120 帧；`0x502` 每次启动 0 帧；`make test` 通过；文档同步；推 GitHub | — |

---

## 三、实验计划（按顺序，每步都有停下来写结论的检查点）

### P0'''''''''：标签稳定（1–2 天）

**自报 binding 的验证已经做过一发（s15bind2，见 §零.2）：没有标签。所以这一阶段的 P0 不是再压 binding，而是查
「CA 为什么选小块 surface」。**

1. **两发的对照（已做）**：13:04 的发（283 笔大层 draw、118 笔有像素、291,909 B、有标签）vs 14:01 的发（34 笔大层、14 笔
   有像素、239,632 B、无标签）——差的是 CA 的 surface 策略；3 ms 延迟已排除「主机速度」。
   **下一步**：查 program 13 的 backdrop（64x19）由谁填——我们流里 `glCopyTexSubImage2D`/`glTexSubImage2D` 都是 0 次，
   要么在第二个 context/进程，要么是上游缺失的一步；用 `GLWIRE_TEXLOG`/attach 日志确认它是否被任何 FBO 渲染过。
2. **小块 surface 的采样**：`GLWIRE_LABEL_ALPHA=1` 把 atlas alpha 画成红色；若仍为 0，查 sampler 状态
   （min filter/wrap/纹理目标）与 texmat0 的录制 payload。
3. **判据**：3 发里 ≥2 发首页有 App 名称、截图 ≥300 KB；同帧 `diffdump --tolerance 8` ≥95%。
4. **记录**：每一发都要有「标签是否出现 + 大目标 label draw 数 + 小块 surface 的采样读数」三行。

### P1'''''''''：片元采样、重影、圆角（1–2 天）

1. **采样**：`GLWIRE_LABEL_ALPHA=1`（把 atlas alpha 画成红色）判定「采样到 0」还是「程序/乘子为 0」；
   若是 texmat0=NaN，查 `glUniform4fv` 的录制 payload（指针/长度/guest location 映射）。
2. **重影**：同一帧 dump panel texture 与 label layer，判断「同一批字画两次」还是「合成两份」。
3. **圆角**：把 `host_legacy_teximage` 的 `GL_ALPHA` 展开改成按用途分叉（mask 用 alpha、采样语义用 swizzle），
   判据：圆角 875/1600 且标签仍正确；0x502 保持 0。
4. **标签颜色**：program 15 的 mask 是纯 alpha，合成时用 shader 的 color 还是固定白——用截图判定。

### P2'''''''''：widget 与壁纸（2 天）

1. **先判定**：SpringBoard 的 312x202/184x124 面是否由 GL 画（它有没有加载我们的 eagl、有没有第二条连接）。
   证伪则走「拉面」。
2. **拉面**：wire 加一个「按 surface ID 拉取」的 opcode：客人机侧用 `IOSurfaceGetBaseAddress/Row` 找像素，
   宿主按 pa 上传成纹理；backboardd 的 `texImageIOSurface:` 列表外再补一轮绑定。
3. **壁纸**：`posterboardservices` 不存在 + 镜像里没有壁纸资源 → 判为上游缺件；判据改成「证据链完整」。

### P3'''''''''：四格表与收尾（1 天）

四份构建（纯本地 / 按引用 / W1.5 / W1.5+W3）各 120 帧；`0x502` 保持 0；`make test`；文档；推送。

---

## 四、不做（已被数据否掉，别重试）

1. **不再猜绑定**：客人机已经自报 `e->framebuffer`，宿主照单绑定。
2. **不再用「两条 EAGL context 合流」解释标签**：3700 帧全部 ctx 0，第二条 context 从未录到 GL 调用（ADR-011 Amendment）。
3. **不再把 legacy 展开当 0x502 的修复**：0x502 来自 `glDeleteProgram`，已归零；展开只影响圆角。
4. **不再把 64x19 当文字颜色**：它是屏幕背景缩略（`S *= y^4`），填白不改画面。
5. **不动内核**：本阶段是纯用户态 + 宿主。

---

## 五、验证环境与操作规范

- 一把 VM 锁 `/tmp/iosvm-vm.lock`；同一时间只跑一发。
- 启动：`tools/glwire/harness/boot-measure.sh <label> <root.apfs> <secs>`；要跑满 900 s 又拿界面时刻用
  `MINFRAMES=999999 MINSHOT=200000`（`MINSHOT` 同时是界面阈值）。
- 用户态重建：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`（wire 或 eagl 改动都要重建）。
- 宿主探针：`GLWIRE_KEEP_BINDING`（默认开）、`GLWIRE_LABEL_DUMP`、`GLWIRE_LABEL_ALPHA`、`GLWIRE_LABEL_WHITE`、
  `GLWIRE_CONTEXT_LOG`、`GLWIRE_FBO_LOG/TRACE`、`GLWIRE_DRAW_LOG`、`GLWIRE_ERROR_LOG`、`GLWIRE_UNIFORM_LOG`。
- 证据图放 `iosvm/docs/demo/`（名字带日期与场景）。

---

## 六、里程碑

| 天 | 目标 | 产出 |
|---|---|---|
| D1 | P0''''''''' 标签稳定 | 3 发标签结论 + 截图 |
| D2 | P1''''''''' 采样判定 | `GLWIRE_LABEL_ALPHA` 的读数与结论 |
| D3 | P1''''''''' 重影/圆角 | 圆角 875/1600、无重影的截图 |
| D4–D5 | P2''''''''' widget/壁纸 | 面进流的日志或「不由 GL 生成」的证据链 |
| D6 | P3''''''''' 四格表 | 四格数字 + 结论表 |
| D7 | 收尾 | `make test`、0x502=0、文档、推送 |

---

## 七、诚实的预期

- **P0''''''''' 是确定能出结论的**：三发里要么有标签、要么没有；两种都写清楚。「自报 binding」把猜测成分从宿主移到了
  客人机自己的记录，剩下的不确定性只有 CA 的 flush 时机。
- **P1''''''''' 的采样一半概率是 sampler 状态**（min filter/wrap/纹理目标），一半是 uniform 录制；两种都能用现有探针判。
- **重影**大概率是「同一批字被画两次」（panel texture 里已有 + label layer 再画），修法在宿主侧。
- **P2''''''''' 的「不是 GL」概率高**：第二个进程一次都没连我们，所以按「主动拉面」设计。
- **P3''''''''' 的壁纸可能判为不可修**：`posterboardservices` 不存在，这版镜像里没有那张图。

---

## 十三、backdrop 实验：darken pass 修好了，但还没上屏（2026-11-06 17:13）

**做了什么**：`GLWIRE_BACKDROP=1` 在每帧第一次见到 64x19 的 backdrop 时，把上一张 750x1624 的面板
`glBlitFramebuffer` 进去（线性缩小）。运行：`s16bd`，`ui at 420 s`、截图 **238,980 B**、3583 帧。

**结果**：`label draw` 1410 笔，**1116 笔产生像素**（之前 0）——且全部落在**小块**上：
181×64x19、119×64x9、12×96x9、6×96x19、3×88x19、4×128x9、2×127x9。也就是说
**program 13 的 darken pass 现在真的在写标签 tile 了**。

**但截图仍无标签**（238 KB）。所以链路变成：tile 有内容 → **tile 的合成没上屏**。下一步查：
这些 tile 纹理被哪些 draw 采样（之前统计过 host8 被采样 2874 次）、采样时的 blend/alpha 是否把内容乘掉，
以及面板合成时它们的顶点位置是否落在屏幕上。

---

## 十四、tile 的合成：静态日志被 host 名字复用干扰（2026-11-06 17:35，**结论修正**）

在 s16bd 日志里按 host 名字追「有像素的 64x19 tile 之后有没有 draw 采样它」：

- 第一版看法是「没有」——因为唯一大量出现的引用是 `u0:host8(guest87)` 这类；
- 但同一份日志里也有 `u0:host8(guest86)`、`u1:host8(guest86)` 的 program 7 全屏 draw（draw#1279/1280），
  而 host 8 在不同时刻分别对应 **guest 86 / 87 / 110 / 121 / 132**（64x19 的 attach 行可以作证）。

**所以 host 名字会被回收复用，按 host 名字追 tile 是不成立的**；上面的「没有被采样」结论**作废**。
要回答「tile 有没有被合成」，必须按 **guest 名字**追：在 label draw 时记下目标 tile 的 guest texture id，
再在之后的 draw 里匹配 `hostN(guestT)`。

**下一步**（比再跑一发更省）：给宿主加一个 `GLWIRE_NAME_TRACE=<guest>`——把某个 guest 纹理的
创建/删除/绑定/采样全打出来；用一发 900 s 抓「tile 的 guest id 生命周期」，再决定是合成缺失还是名字复用。

---

## 十五、按 guest 名字追：tile 确实被采样（2026-11-06 17:50）

`GLWIRE_TILE_TRACE=1` 的一发（s16tile，ui at 510 s，240,101 B，1126 笔 label draw / 844 笔有像素）：
tile 的 **guest 名字**在 0–3 帧内被后续 draw 采样，采样点是 u0 45 次、u1 155 次；采样它们的程序是
**program 5 / 7（全屏搬运）**、13 / 15（文字）、19 / 21 / 23（合成），目标 fbo 2 / 3 / 5 / 11。

**结论**：tile 不是「没人消费」，**合成 draw 就在同一帧或下一帧里**。所以问题不在「没有合成」，而在
**合成之后为什么屏幕上没有**——候选：blend/alpha 把内容乘掉（我们的 backdrop 是**不透明**的，alpha=1，
合成时整块 tile 会盖住底图）、或者合成目标是临时面（fbo 3），随后被真正的面板覆盖。

**下一步（继续第十六阶段 P1）**：在 `host_fbo_draw` 里对程序 5/7 的合成 draw 记录「源 tile 的 alpha 与目标 fbo 的
内容哈希」，看合成后目标面是否真的变亮；同时把 backdrop 的 blit 改成**保留 alpha**（我们用整屏 blit，alpha 被写成 1，
这会让合成变成不透明贴片）。

---

## 十六、合成顺序（从 s16tile 日志直读，2026-11-06 18:05）

取 UI 阶段的一帧（draw#3759 起），顺序是：

```
draw#3760 program 3  fbo 2
draw#3761 program 13 fbo 7     ← label 画进 tile（64x19，1216 像素）
draw#3762 program 7  fbo 2     ← tile 立刻被合成回面板
draw#3763 program 5  fbo 2
draw#3764..67 program 17 fbo 2
draw#3768 program 5  fbo 2
draw#3769 program 0  fbo 2
draw#3770 program 3  fbo 3
draw#3772 program 13 fbo 7     ← 下一个 label
draw#3773 program 7  fbo 3
...
```

**读法**：每个 label 都是「先画进 tile（fbo 7）→ 立刻被 program 7 合成进面板（fbo 2/3）」，**同一帧内、顺序正确**。
所以合成不是缺失，是**每个 label 之后整个面板又被重画一遍**（program 3/5/17/0 把 fbo 2/3 重建），
上一笔的合成结果被覆盖——最后一帧的面板里最多只剩最后一个 label。

这正是「一次性 pass 被 flush 切成很多小步」的另一种表现：CA 的 pass 里每一步都重画整块面板，我们收到的每一帧
只保留最后一步的合成。**下一步**（第十六阶段 P1 的收尾，也是第十七阶段 P0）是验证「面板重画是否真的覆盖了合成」——
在 program 3/5 的 draw 前后对 fbo 2 做区域哈希，能直接看到标签区域的像素被抹掉。

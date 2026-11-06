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

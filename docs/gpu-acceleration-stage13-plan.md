<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十三阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十三阶段工程文档

> 建立时间：2026-11-05 22:20
> 前置：第十二阶段（`IOSVM-GPU加速第十二阶段工程.md`，收尾在 §八：判据逐条对照、本阶段真正的收获、留给本阶段的三个靶子）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **界面能起、图标能画、双面写回稳定**（第十二阶段：界面后 245 KB 级截图、同帧 diff 97.7%）。
2. **缺三样内容**：App 标签文字、widget 内容、wallpaper。第十二阶段把文字的链条走到了最后一步：
   `frame 1500 draw 0`（program 13、`blend off`、`u0 = 146x90 字形块`）落盘后 **FBO 里 0 个非零像素**——着色器写出来就是透明黑。
3. 文字 shader 的 `u1` 是一张 **64x19 RGBA8 纹理**，客人机只分配（`glTexImage2D(..., pixels=NULL)`）**从不填充**
   （`GLWIRE_WATCH=64x19`：4 次创建、0 次填充、0 次 sub-upload），宿主上它是全 0。
   所以颜色只有三种可能：**uniform 映射错位**、**另一条没录到的填充路径**、**它本来就不该用 u1**。
4. **460 s 启动空洞已量清但没修**：lsd 的串行 LaunchServices 工作（启动期 470 样本，busy 中位 124%、≥4 核仅 7%）。
5. 宿主 `glerror 0x502`（每次启动 8 帧，GL_ALPHA/LUMINANCE legacy 格式）**还没收干净**。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 文字颜色走 **uniform 映射审计**（打印 guest/host location 与值，逐个对）还是 **换一条路**（在宿主把 program 13 的着色器输出强制为白色、或让客人机把颜色烘进顶点色） | 审计：可能发现是别的东西（比如顶点数据），多花一天；换路：可能把「文字颜色」和「图标渐变」一起弄坏，且不解决上游 | 标签上屏（P0 的全部收益） |
| D2 | 460 s 空洞修 **（csstore 预置 / 禁用 lsd 对照）** 还是接受 | 不修：启动 8 分钟永远在；修错：SpringBoard 起不来（lsd 是 LaunchServices 的库） | 启动时间与整段 fps |
| D3 | 300 KB 判据是否继续当唯一口径（第十二阶段提醒：245 KB 差在**内容**，不是传输） | 继续只看 300 KB：可能用「白色填充」刷过判据而内容仍是假的 | 判据改成「内容项清单 + KB」两条 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P1'''''' | 文字颜色输入（program 13） | 一次启动里，program 13 的每一笔 draw 前，宿主打印的 **uniform（名字/值）** 与客人机 `glUniform*` 行**逐条对得上**，且颜色 uniform 的 alpha ≠ 0；若最终判定「不是 uniform」，必须给出那条真实路径的 ≥1 条证据行 | P0'''''' |
| P0'''''' | 内容上屏 | 首页截图 **≥300 KB** 且与同帧 host picture 的 `diffdump --tolerance 8` **≥95%**；同时列「内容项清单」（标签 / widget / wallpaper 各自在/不在） | 项目可用性 |
| P2'''''' | 460 s 空洞 | 两组对照（禁用 lsd / 预置 csstore）各一次启动，**启动到界面 −30%**；做不到写清卡在哪一步、为什么修不了 | 整段 fps |
| P3'''''' | 宿主 legacy 格式收尾 | `glerror 0x502` 每次启动 **0 帧**；标签行像素有变化、widget 圆角**不回归**（与第十二阶段 `corner-before/after` 对比） | 画面正确性 |
| P4'''''' | 文档与提交 | `make test` 通过、`dsh.MD` 与 `iosvm/docs` 同步、推送到 GitHub | — |

---

## 三、实验计划（按顺序，每步都有停下来写结论的检查点）

### P1''''''：program 13 的 uniform 审计（0.5–1 天）

1. 宿主 `dispatch.inc` 的 `glUniform1f/4f/1i/3fv/4fv` 分支各打一行：`guest location -> host location, values`（前 200 条）。
2. `host_note_draw` 里只对 program 13 打印当前程序里**所有 active uniform** 的名字与值（`glGetActiveUniform` + `glGetUniformfv`）。
3. 对照：`glwire: uniform ...`（录到的）与 `glwire: draw#... program 13 uniform ...`（宿主实际状态）。若 location 映射错位，
   检查 `names_uniform` 是否在 **program 重链**后失效（CA 会重链程序，location 会换）。
4. 判据：见 P1'''''' 表。若 24 小时内查不出，**停止并写清**：颜色到底从哪来、还缺什么证据。

### P0''''''：内容上屏（P1 之后，0.5 天）

1. P1 修好或找到填充路径后，跑一发 900 s（`/tmp/s12dump.sh` 的形态即可），看首页截图：
   `ls -l /tmp/xb-s13-*`、`python3 iosvm/tools/glwire/harness/frame_match.py` 对照。
2. 判据：≥300 KB 且 diff ≥95%；同时逐项记录「标签 / widget / wallpaper」。
3. wallpaper 若仍缺：查它是不是一张面（第十二阶段 24 张面里没有 wallpaper 尺寸）、或由一个没录到的 CPU 绘制完成。

### P2''''''：460 s 空洞（1 天，与 P1 并行）

1. 对照组 A：禁用 lsd（`launchctl unload` 或在 launchd 里把它设为 Disabled），900 s 一发，记「启动到界面」时间。
2. 对照组 B：若 rootfs 里有 csstore 文件，预置到 `/var/mobile/Library/…`，同样一发。
3. 判据：**启动到界面 −30%**（第十二阶段基线约 460–600 s 的洞）。若 lsd 是 SpringBoard 的硬依赖而起不来，写清并标为「不可修」。

### P3''''''：宿主 legacy 格式（0.5 天）

1. 把 `host_legacy_teximage` 的 `GL_ALPHA` 展开从「rgb=0」改成**按采样语义分叉**：
   先查该纹理的 `GL_TEXTURE_SWIZZLE_*`/采样方式；若无法判定，保留「rgb=0 + alpha」，并**同时**支持 env 覆盖（`GLWIRE_LEGACY_MODE=black|white`）。
2. 判据：`glerror 0x502` 0 帧；圆角不回归；文字行若有变化记录下来。

### P4''''''：收尾（0.5 天）

`make test`、两份文档同步、commit/push。

---

## 四、不做（已被数据否掉，别重试）

1. **不再试 CAImageQueue 的 interpose / fishhook**：第十二阶段 0 调用行，且 SpringBoard 一崩就整机黑。
2. **不再试 `CA_DEBUG_TRANSACTIONS`**：500+ 帧里一条 CA 事务日志都没有，这版 QuartzCore 不认它。
3. **不再用 `ld64.lld` 链接内核**：`__TEXT` 落在 0，loader 判 `unexpected kernel layout`；`-image_base`/`-segaddr` 都是 not implemented。内核只走 cctools。
4. **不再猜「白块是 wallpaper」**：第十二阶段的纹理转储里没有 wallpaper 尺寸的面，白块是 widget 层的背景色。
5. **不再把「宿主 legacy 格式展开成白」当修复**：它把圆角变成直角，且标签不动。

---

## 五、验证环境与操作规范

- 一把 VM 锁：`/tmp/iosvm-vm.lock`（`mkdir`，`trap` 里删）；同一时间只跑一发。
- 启动：`tools/glwire/harness/boot-measure.sh <label> <root.apfs> <secs>`，环境 `ZERO_RAM=1 MINFRAMES=… MINSHOT=… XCMD="…"`。
- 观察：`host_glwire` 的每一行（frame/interval/image refs/glerror）；客人机串口（`/tmp/<label>.log`）。
- 探针（都已进仓库）：
  - 客人机：`surfsample=<id>`（内核侧 surface 采样）、`texlog=1`（大纹理上传）、`surftrace_of=<name>`（surface 端口追踪）、`ustacks=top`（进程 top）；
  - 宿主：`GLWIRE_DUMP_TEXTURES`、`GLWIRE_DUMP_DRAWS`、`GLWIRE_DRAW_LOG`、`GLWIRE_WATCH=<WxH>`、`GLWIRE_LEGACY_FORMATS=1`。
- 内核构建：`LANG=C LC_ALL=C IOSVM_USE_LLD=0 tools/make-kernel.sh -j8`（cctools 在 `/tmp/cctools`，重启会丢，重建步骤见第十二阶段 §工具债）。
- 用户态构建：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`。

---

## 六、里程碑

| 天 | 目标 | 产出 |
|---|---|---|
| D1 | P1'''''' 审计完成 | 一份 uniform/颜色路径结论（含证据行） |
| D2 | P0'''''' 演示 | 首页截图 + 内容项清单 |
| D3 | P2'''''' 两组对照 | 启动时间数字（或「不可修」的原因） |
| D4 | P3''''''/P4'''''' 收尾 | 0x502 归零、四格表、文档与推送 |

---

## 七、诚实的预期

- **P0'''''' 有一半概率在 D2 达成**：整条链只剩一笔 draw 的输入，证据密度已经很高；但如果颜色来自某个我们完全没录的路径
  （例如 CA 自己把颜色烘进顶点缓冲的某个属性），可能要多花 1–2 天。
- **P2'''''' 大概率「查得清、修不动」**：lsd 是 SpringBoard 的 LaunchServices 依赖，禁用大概率起不来；csstore 预置需要镜像里有
  对应文件。若两组都失败，本阶段按「写清为什么」收尾。
- **P3'''''' 是确定能做完的**（纯宿主代码），但要注意别再动圆角。
- 本阶段**不追求**「四格表刷新」到新镜像（那是收尾项，数据可沿用第十一阶段，注明即可）。

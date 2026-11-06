<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十七阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十七阶段工程文档

> 建立时间：2026-11-06 18:10
> 前置：第十五阶段（wire 带 context、客人机自报 binding）、第十六阶段（label 链路分类、backdrop 补填、
> tile 按 guest 名字追踪、合成顺序直读；见 `IOSVM-GPU加速第十六阶段工程.md` §十二–§十六）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **标签链路已经打通到「合成」这一步**：`GLWIRE_BACKDROP=1` 补上 program 13 的 64x19 backdrop 后，
   64x19/64x9 的 label tile 真的有像素了（s16bd：1116/1410 笔；s16tile：844/1126 笔）。
2. **合成顺序也对**：s16tile 的 draw 顺序是「label → tile（program 13，fbo 7）→ program 7 把 tile 合成回面板
   （fbo 2/3）」，同一帧内紧接着发生。
3. **但屏幕仍没有标签**（238–240 KB；有标签的那发 291,909 B 是 CA 偶然走大层路径）。读日志得到的原因：
   **每合完一个 label，整个面板又被 program 3/5/17/0 重建一遍**，上一笔的合成被覆盖——最后一帧最多只剩最后一个 label。
4. widget 内容与壁纸仍缺；legacy 展开让 widget 圆角从 875/1600 变 1600/1600；四格表未刷新。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 主攻方向：**宿主侧「最终合成缓冲」**（把 label 合成那一步的结果冻结，后续面板重建不回滚它）vs **客人机侧对齐 CA 的 pass** | 客人机对齐：改动大、风险高（可能死等 flush）；宿主冻结：可能把「本来会被覆盖的合成」错当成最终态 | 稳定标签 |
| D2 | 圆角：**legacy 展开改成保留采样语义**（alpha 来源可配 / swizzle）vs 接受直角 | 接受：画面回归；分叉：要弄清那张面给谁用 | 圆角 + 标签同时正确 |
| D3 | widget/壁纸：**按 surface ID 主动拉取** vs **录第二个进程** | 录进程：它不用 GL 就白干；拉面：要找到 surface 的 base/stride | widget 内容与壁纸 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0'''''''''' | 标签上屏且稳定 | **连续 3 发启动**，≥2 发首页有 App 名称；截图 ≥300 KB；同帧 `diffdump --tolerance 8` ≥95% | P1'''''''''' |
| P1'''''''''' | 文字质量 | 无重影（Reminders 处只有一份清晰的字）；widget 圆角 **875/1600**；标签为白色 | 可用性 |
| P2'''''''''' | widget 内容 | `312x202`/`184x124` 面进流（attach/upload 行），widget 方块内有内容；或给出「不由 GL 生成」的证据链 | 可用性 |
| P3'''''''''' | 壁纸 | 主屏背景非纯黑；或给出「上游没有这张图」的完整证据链 | 可用性 |
| P4'''''''''' | 性能与收尾 | 四格表（纯本地/按引用/W1.5/W1.5+W3）各 120 帧；`0x502` 每发 0；`make test` 通过；文档与推送 | — |

---

## 三、实验计划（按顺序，每步都停下来写结论）

### P0''''''''''：标签的「合成被重建覆盖」（1–2 天）

1. **确认覆盖**：在 program 3/5 的 draw 前后，对 fbo 2/3 的标签区域做哈希（宿主侧，20 次封顶），
   看标签像素是否被抹掉。这是 P0 的第一证据。
2. **宿主侧「最终合成缓冲」**：识别 program 7 把 tile 合成进面板的那一笔，把该笔的结果额外写进一张
   750x1624 的 shadow 面；帧末如果这一帧发生过 label 合成，用 shadow 作为 picture 写回（否则用原逻辑）。
   开关 `GLWIRE_PANEL_SHADOW=1`，先做实验，稳了再默认。
3. **判据**：连续 3 发，≥2 发有 App 名称、≥300 KB、diff ≥95%。
4. **记录**：每发「标签是否出现 + 合成笔数 + 覆盖哈希是否变化」。

### P1''''''''''：文字质量（1–2 天）

1. **重影**：同一帧 dump panel 与 shadow，判断「同一批字画两次」还是「合成两份」。
2. **圆角**：`host_legacy_teximage` 的 GL_ALPHA 展开改成按用途分叉（mask 用 alpha、采样语义用 swizzle）；
   判据：圆角 875/1600 且标签仍正确。
3. **颜色**：program 15 的 mask 合成色是白还是 shader 的 color——用截图判定。

### P2''''''''''：widget 与壁纸（2 天）

1. 先判定 SpringBoard 的 312x202/184x124 面是否由 GL 画（它有没有加载我们的 eagl、有没有第二条连接）。
2. 证伪则走「拉面」：wire 加「按 surface ID 拉取」opcode，用 `IOSurfaceGetBaseAddress/Row` 上传成纹理。
3. 壁纸：`posterboardservices` 不存在 + 镜像无资源 → 判为上游缺件，给证据链。

### P3''''''''''：四格表与收尾（1 天）

四份构建各 120 帧；`0x502` 保持 0；`make test`；文档；推送。

---

## 四、不做（已被数据否掉，别重试）

1. **不再猜 binding**：客人机自报 `e->framebuffer`（第十五阶段 §十一）。
2. **不再用「两条 context 合流」解释标签**：3700 帧全部 ctx 0（ADR-011 Amendment）。
3. **不再把 legacy 展开当 0x502 的修复**：0x502 来自 `glDeleteProgram`，已归零。
4. **不再按 host 名字追纹理**：host 名字会被回收复用（第十六阶段 §十四），一律按 guest 名字追。
5. **不动内核**：本阶段是纯用户态 + 宿主。

---

## 五、验证环境与操作规范

- 一把 VM 锁 `/tmp/iosvm-vm.lock`；同一时间只跑一发；每发 900 s。
- 启动：`tools/glwire/harness/boot-measure.sh <label> <root.apfs> <secs>`；跑满 900 s 又拿界面时刻用
  `MINFRAMES=999999 MINSHOT=200000`。
- 用户态重建：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`。
- 宿主探针：`GLWIRE_KEEP_BINDING`（默认开）、`GLWIRE_BACKDROP`、`GLWIRE_TILE_TRACE`、`GLWIRE_LABEL_DUMP`、
  `GLWIRE_LABEL_ALPHA`、`GLWIRE_LABEL_WHITE`、`GLWIRE_PANEL_SHADOW`（本阶段新增）、`GLWIRE_FRAME_DELAY_MS`、
  `GLWIRE_CONTEXT_LOG`、`GLWIRE_FBO_LOG/TRACE`、`GLWIRE_DRAW_LOG`。
- 证据图放 `iosvm/docs/demo/`（名字带日期与场景）。

---

## 六、里程碑

| 天 | 目标 | 产出 |
|---|---|---|
| D1 | P0'''''''''' 覆盖证据 | 面板区域哈希前后对比 |
| D2 | P0'''''''''' shadow 实验 | 3 发标签结论 + 截图 |
| D3 | P1'''''''''' 圆角/重影 | 875/1600 圆角、无重影的截图 |
| D4–D5 | P2'''''''''' widget/壁纸 | 面进流的日志或证据链 |
| D6 | P3'''''''''' 四格表 | 四格数字 + 结论表 |
| D7 | 收尾 | `make test`、0x502=0、文档、推送 |

---

## 七、诚实的预期

- **P0'''''''''' 有明确的可测假设**：合成被重建覆盖。区域哈希能一锤定音；shadow 方案是对症的，但有「把中间态当最终态」
  的风险，所以先做开关实验、三发验证。
- **P1'''''''''' 的圆角**大概率是 legacy 展开的 alpha 语义问题，修法在宿主侧，低风险。
- **P2'''''''''' 的「不是 GL」概率高**：第二个进程一次都没连我们。
- **P3'''''''''' 的壁纸可能判为不可修**：这版镜像里没有那张图。

---

## 八、P2''''：312x202 / 184x124 两个面不由 GL 画（2026-11-06 19:10，证据等级：实测）

**结论**：SpringBoard 建的 `312x202 'LA08'`（surface 3）与 `184x124 'BGRA'`（surface 4）**不是**由我们转发的
GL 链路（eagl + glwire）画的；它们连「渲染服务器的纹理绑定」这一步都没进。四类独立证据：

| # | 证据 | 复核命令 | 结果 |
|---|---|---|---|
| 1 | 注入表里没有 GL 库 | `python3 ../tools/apfs/apfs.py cat root.apfs /etc/inject.conf`；运行时 `init: library insertion`（s16tile.log:472） | GL 库只被 `inject.c` 的 `opengl_es()` 在 `getprogname()=="backboardd"` 时 `dlopen`；SpringBoard 只有 `libhakios.sb.dylib`，对 `HAKIOS_GL` 只 `access()` 不加载 |
| 2 | 宿主只有一条连接 | `grep -c "^host_glwire: client" /tmp/s16tile-host.log`；`grep -o "client=[0-9]*"` | `client 17 connected`/`closed` 各一行；3350 帧全归 fd 17；20000 笔 draw 全 `ctx 0`。不存在第二个进程 |
| 3 | 两张面从未被 CA 绑定 | `grep -o "hakios-gl: -texImageIOSurface: surface [0-9]*"`；`grep "hakios-gl: -attachImage"` | 24 条 texImageIOSurface 的 id 是 5–30（无 3/4），6 条 attachImage 是 1/2/18/26；`eagl.c` 的 `image_new()` 只在这两个方法里调用 → 不进 28 张 image 列表（3318 条列表里无 3/4/312x202/184x124） |
| 4 | 宿主侧上传/附件尺寸表 | `grep -o "upload texture .* [0-9]*x[0-9]*"`；`grep "fbo attach"` | 30 条 upload、44 条 attach 中没有 312x202/184x124；严格 `grep` 为 0 次 |

**读法**：三号面/四号面是 SpringBoard 的 `MiniIOSurface`，创建之后在整份客人机日志里再没被提到过一次
（`grep "surface 3\b\|surface 4\b"` 只命中创建行）。它们既没被上传成纹理，也没进 backboardd 的 image 列表，
所以「widget 黑」在 GL 链路上是**必然**的——不是采样错、不是合成丢。

**不知道的**：这两张面里到底有没有像素、由谁画。这一发没开 `surfsample`，`libhakios.ciq.dylib` 也没构建进镜像
（`/lib` 下没有），所以只能判「GL 侧不可能有内容」，不能判「面是空的」。

**若要走「拉面」**（P2 的第二条路，没做）：SpringBoard 侧用 `inject_ciq_probe.c` 现成的
`CAImageQueueRegisterIOSurfaceBuffer` / `IOSurfaceCreate` 钩子拿 `{ID,w,h,stride,format,屏幕矩形}`（需先在
`build.sh` 把 ciq 库建出来并恢复 `INJECT` 的 ciq 行），经 XPC/共享内存交给 backboardd；backboardd 在
`eagl_remote.c` 里照 opcode 201 加一条新记录（段表用现成 `remote_vtop()`，不需要 GL 上下文）；宿主在
`dispatch.inc` 按 201 模式读成纹理，再在合成 draw 里替换/覆盖到面板。退路是 backboardd 按固定 ID
`IOSurfaceLookup(3)/(4)` 轮询拉，但会丢「何时更新 / 贴哪里」。

---

## 九、P3''''：壁纸不是「上游缺件」，是启动镜像的作业表缺件（2026-11-06 19:18，文件证据=实测，出图=未知）

**结论**：`posterboardservices` 确实不存在，但**不是镜像里没有**——是**启动镜像构建时把作业表裁掉了**。

| 层 | 有什么 | 证据（复核命令） |
|---|---|---|
| 上游全量（`iosvm/build/images/rootfs`、6.36 GB 的 `iosvm/userland/system.apfs`） | `System/Library/LaunchAngels/com.apple.PosterBoard.plist`（1000 B，`MachService=com.apple.posterboardservices.services`）、`PosterBoard.app/PosterBoard`（119,520 B）、`PosterBoardServices`/`PosterBoardUI`/`WallpaperKit` 框架、`Library/Wallpaper`（234 文件 / 27 MB，48 张 HEIC） | 目录清单与 `stat` |
| 实际启动的 2.47 GB 镜像（`iosvm-work/.../userland/system.apfs`，2,476,748,800 B） | `LaunchAngels` **是空目录**（0 children）；`build.sh:636` 主动 `rm -rf` 它 | `python3 tools/apfs/apfs.py ls`、`build.sh:636` |
| 本次启动的 `root.apfs` | 作业表 32 个 job（25 stock + 7 项目），**没有 `com.apple.PosterBoard`** | `python3 ../tools/apfs/apfs.py ... /Library/LaunchDaemons` |
| 客人机 | `failed lookup: name = com.apple.posterboardservices.services ... requestor = SpringBoard[10], error = 3: No such process`（s16tile 17:42:38.9 / 39.3） | `grep "posterboardservices" /tmp/s16tile.log` |
| 宿主 | 3350 帧全 750x1624；30 条 upload、44 条 attach，**0 条 1125x2436**；面板 3318 次 `px ff000000` | `grep -o "upload texture .*"`、`grep "px ff000000"` |

**所以**：壁纸缺件的位置在「启动镜像的 job 列表 / LaunchAngels」，不在 glwire 的绘制链路；补回 job 后是否真出壁纸
**仍未验证**（要重建镜像再启动）。这比第十六阶段的「上游没有这张图」判断更准确：**上游有，启动镜像裁掉了**。

**顺带一条（影响 P4 的口径）**：`0x502` 在 s16tile 里出现 **36 次，全是客人机侧 `hakios-gl: glUniform2fv/1fv: error 0x502`**
（17:42:27 前后），宿主侧 `glerror` 全 0。「每发 0x502=0」应明确写成**宿主侧 0**；客人机侧这 36 次是 OSMesa 自己的
uniform location 不匹配，属另一条线。

---

## 十、P0'''' 第一证据：42.4% 的帧把「小块层」当画面上传（2026-11-06 19:40，实测，宿主侧）

**怎么发现的**：宿主每帧的收尾是「从客人机最后画进去的那个 framebuffer 读回画面」。第十六阶段一直按这个规则，
日志里一直有 `read-back framebuffer N` 与 `reading from N instead` 两行，但从没有人按「N 是多大」统计过。
按尺寸统计 s16tile 的 3350 帧：

| 实际回读来源 | 帧数 | 这个 framebuffer 是什么 |
|---|---:|---|
| fbo 2 | 966 | 750x1624 面板（正确） |
| fbo 3 | 965 | 750x1624 面板（正确） |
| **fbo 7** | **915** | **64x19 / 64x9 / 96x9 / 122x19 的 label tile** |
| **fbo 8** | **312** | **88x19 / 122x19 / 90x19 的 label tile** |
| **fbo 11** | **126** | 750x200 的标签层 |
| **fbo 5** | **42** | 750x200 的标签层 |
| **fbo 10** | **24** | 108x29 / 122x19 的 tile |

**1419 / 3350 = 42.4% 的帧**，回读来源是一张小块层；而 `glReadPixels(0,0,750,1624)` 在一个 64x19 的
framebuffer 上只能读到 64x19，其余是未定义/黑。这一帧的画面（4.8 MB）随后被**原样写进 750x1624 的显示面**
（`in-place rows`），所以那 42.4% 的帧等于往屏幕上刷了一张几乎全黑的图。

**为什么以前没被当成问题**：每次截图都是隔几十秒抽一张，抽到好帧就看不出；而「标签所在的帧」恰好以 label
tile 收尾——**有标签的那一帧，正好是画面被黑掉的那一帧**。

**修法（已实现，默认开）**：`GLWIRE_READ_PANEL`（默认 1）从**本帧最后画进去的、面板尺寸的** framebuffer 读回，
而不是「最后画进去的任意 framebuffer」；`host_fbo_draw()` 给每个被画的目标记尺寸，面板尺寸 = 请求的
`width x height`。`GLWIRE_READ_PANEL=0` 回到旧行为（对照用）。

**验证（第 4 发 s17probe4，800 s）**：`reading from` 的分布变成 **fbo 2: 142 / fbo 3: 141，小层 0 帧**，
宿主 298 帧全程没有再从 tile 回读。截图 **245,994 B**（比之前的 238–240 KB 大，但还没到 300 KB，标签仍未出现）。
这一发在 19:32 因客人机 ticket-spinlock panic 结束（见 §十二）。

---

## 十一、P0 探针的一发（s17probe4，800 s）：读回修好了，标签仍不在，但拿到了逐笔哈希（2026-11-06 19:55）

**这一发是什么**：`GLWIRE_BACKDROP=1 GLWIRE_PANEL_PROBE=1 GLWIRE_PANEL_SHADOW=1`（影子只记不应用）、
`GLWIRE_DRAW_LOG=1 GLWIRE_PROGRAM_ALL=1`，800 s。UI 阶段每帧 8 笔 draw，**实测 298 帧**，截图 **245,994 B**，
19:32:23 因客人机 ticket-spinlock panic 结束（§十二）。

**读回目标的修复生效**：298 帧的回读来源是 **fbo 2: 142 / fbo 3: 141 / fbo 5: 15**（那 15 帧本帧没有画进面板层，
按回退规则读 750x200 的层），**没有一帧再从 64x19 的 tile 回读**。对照 s16tile 的 42.4%（§十）。

**逐笔整幅哈希（frames 296/298，每帧 8 笔）**：

| 笔 | program | 整幅哈希变化 | bright（≥200 的白像素） |
|---|---|---|---:|
| 1 | 7 | 新值 | 275,986 |
| 2 | 5 | 变 | **280,873（+4,887）** |
| 3 | 23 | 变 | 280,756 |
| 4 | 23 | 变 | 280,544 |
| 5 | 23 | 变 | 280,515 |
| 6 | 23 | 变 | 280,406 |
| 7 | 5 | 变 | 280,400 |
| 8 | 0 | **不变** | 280,400 |

**读法**：本帧的最后一笔（program 0）**没有再改面板**；后 6 笔合计只抹掉 467 个白像素（4,887 的 9.6%）。
也就是说，在 UI 这一帧里「合成之后整个面板被重建」**不成立**——重建只改了一小片，白像素基本留住了。
这推翻了第十六阶段 §十六 的读法（「每合完一个 label，整个面板又被重建一遍」）。标签仍不见的下一层原因，
要么是那 4,887 个白像素不是标签，要么是**它们没被回读/没被写进显示面**。这两件事分别由
（a）影子/画面的成对 dump、（b）客人机侧的 surface 写入日志 判。

**同时也说明**：第十六阶段「伪造 backdrop」那条路把 64x19 tile 填成了「上一帧面板的缩略图」，tile 的 1,216 个像素
全部非零（`label draw #1498 ... 1216 nonzero`），所以「tile 有像素」这个判据本身不代表文字画进去了。

---

## 十二、新的头号阻塞：客人机 ticket-spinlock panic（2026-11-06 20:00，栈=实测，根因=推断）

今天晚上 6 发里 5 发崩了，全部是同一条：

```
panic(cpu N): Ticket spinlock[...] timeout after 60000022 ticks ... @lock_ticket.c:386
```

**栈（用 `qemu/kernel.syms` 解析 s17probe4 的 20 个 LR）**：

```
_lck_ticket_contended+0x48
_timer_call_enqueue_deadline_unlocked+0x58
_timer_call_enter_internal+0xfc
__arm_delayed_call_timer+0xc0
_thread_call_enter_delayed_internal+0x224
_input_poll+0x514                      ← drivers/input/hakios.virtio_input.c
_thread_call_invoke+0x74
_thread_call_thread+0x220
_Call_continuation+0x40
```

**根因（推断，证据在上面）**：`input_poll()` 每次结束时都会 `clock_interval_to_deadline(8, NSEC_PER_MSEC)` +
`thread_call_enter_delayed(poll_call, deadline)` **把自己 8 ms 后再排一次**（`hakios.virtio_input.c:268-296`）。
每一次排期都要拿 timer-call 的全局锁；在 QEMU **TCG（x86 宿主上跑 arm64）4 vCPU** 上，只要一个 vCPU 在持锁
期间被宿主调度走超过 60 s（宿主有另一个 400% CPU 的 Windows 虚拟机在跑，实测 `qemu-system-x86_64` 占 4 核），
另一个 vCPU 的 ticket spinlock 就超时。

**为什么判定与 GL 无关**：崩的几发里宿主只处理了 **2 帧**（`s17probe2/s17p5/s17p6` 的 `hostframes=2`），
`input_poll` 在任何 GL 调用之前就已经在 8 ms 一次地排期；s17probe4 跑到第 298 帧才崩，也不是崩在 glwire 的哪一笔上。

**下一阶段 S0 的三个候选修法**（按代价排序，见第十八阶段文档 §三）：
1. **把 8 ms 改大**（20–30 ms）：一行改动，timer 压力降 3–4 倍；输入延迟从 8 ms 变 30 ms，对这套「主机侧画帧」
   的延迟预算可以接受。
2. **无设备时不排期**：`device_count == 0` 直接不排（现在总会排）。
3. **真修：用 virtio 中断而不是轮询**（`VIRTIO_QUEUE_NOTIFY` 那侧已有中断线，驱动没有注册 ISR）；
   代价最大，但 8 ms 轮询本身就不该是最终形态。

**这一阶段的实际影响**：P0 的三发验收、P4 的四格表都没能在今晚完成；已有数据（§十、§十一、§八、§九）是
在崩溃之前拿到的，日志与截图都留在 `/tmp/s17*`。

---

## 十三、P1 圆角与 P4 收尾的实际状态（2026-11-06 20:05）

### 13.1 圆角：原生上传已实现，口径待统一

`host_legacy_teximage()` 以前只有「展开成 RGBA」一条路（`GLWIRE_LEGACY_FORMATS=1` 才开，默认连传都不传，
于是 0x502）。本阶段先测了上下文：**V100 的 4.6 兼容 profile 接受 `GL_ALPHA`/`GL_ALPHA8`/`GL_LUMINANCE`/
`GL_LUMINANCE8`/`GL_LUMINANCE_ALPHA` 原生上传，全部 `glerror 0`**（`/tmp/alphatest.c`，40 行，EGL pbuffer）。

所以改成：**先原生传，被拒了再展开**（`GLWIRE_LEGACY_FORMATS` 默认 0=原生优先，1=强制展开，2=只原生）。
原生上传的采样与客人机 GL ES 完全一致，这是圆角回归的根因方向。

**测量口径要与旧基线对齐**：旧基线「widget 左上角 40x40 白像素 875/1600」是在某一版布局下测的；
s17probe4 的截图（245,994 B，UI 已起）用 `(64,210)` 的 40x40 框测得 **495/1600**——圆角明显在（不是 1600），
但数值与 875 不可直接比。下一阶段第一件事是把「框的位置」写死成相对 widget 左上角的偏移，再比 875/1600。

### 13.2 0x502 的口径（重要）

- **宿主侧**：s16tile / s16bd 两份宿主日志 `glerror` 全 0；本阶段新增的 legacy 原生上传也没有制造错误。
- **客人机侧**：s16tile 里有 **36 次** `hakios-gl: glUniform2fv/1fv: error 0x502`（17:42:27 前后，Mesa/OSMesa 自己的
  uniform location 不匹配），s16bd 30 次。这与宿主无关，是客人机本地 GL 的问题。

所以「每发 0x502 = 0」从本阶段起写成：**宿主侧 0**；客人机侧单独记账（现在是 30–36 次，属 P1 文字质量那条线）。

### 13.3 四格表：未刷新（诚实记账）

P4 要的四份构建（纯本地 / 按引用 / W1.5 / W1.5+W3）各一次 20 s 滚动窗口，本阶段**没有做**：
启动不稳定（§十二，6 发 5 崩）把可用窗口吃掉了。已有的唯一数字是 s16tile/s16probe4 的宿主帧间隔
（UI 稳态 **0.02 s/帧 ≈ 50 fps**，来自 `host_glwire: frame ... interval` 行），对应 W1.5+W3 这一格，
但没有 20 s 滚动窗口的口径，不写入四格表。下一阶段 P4 重跑。

### 13.4 `make test`

`tools/glwire`：`make test` → `map_test: PASS`、`e2e_test: PASS`（EGL/V100 上实跑，2026-11-06 19:05）。

---

## 十四、本阶段结论（交付清单）

| 项 | 状态 | 证据 |
|---|---|---|
| P0'''' 标签上屏 | **未达成** | 42.4% 小层回读的 bug 已修（§十），逐笔哈希推翻「整面板重建」（§十一），标签仍未出现；三发验收被启动 panic 打断（§十二） |
| P1'''' 文字质量/圆角 | **部分** | 原生 legacy 上传已实现（§13.1），圆角在但口径待统一；重影/颜色未做 |
| P2'''' widget | **证据链完成** | 两个面不由 GL 链路画（§八，四类独立证据） |
| P3'''' 壁纸 | **判定完成（比上一阶段更进一步）** | 不是上游缺件，是启动镜像删了 `LaunchAngels`、作业表没有 PosterBoard（§九） |
| P4'''' 性能与收尾 | **部分** | `make test` 通过；0x502 口径改为「宿主侧 0」；四格表未刷新（§13.3） |

**最有价值的三个结果**：① 42.4% 的帧把 tile 当画面上传（真 bug，已修，有前后对比）；
② 「合完 label 整个面板重建」被逐笔哈希推翻，标签问题移到「显示面写入」或「白像素不是标签」；
③ 启动不稳定被定位到客人机内核 `input_poll` 的 8 ms 自排期 timer，与 GL 无关。

**留给第十八阶段的第一件事**：先让 3 发启动不崩（§十二的三个候选修法），否则任何三发验收都做不了。

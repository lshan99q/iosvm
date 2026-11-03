<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第五阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第五阶段工程文档

> 建立时间：2026-11-03（第四阶段收尾之后）
> 前置阅读：`IOSVM-GPU加速第四阶段.md`（这一阶段实测的数字与结论）、
> `IOSVM-GPU加速交接.md`（架构/坑）、`IOSVM-GPU加速后续优化.md` §零
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 一、一句话现状

第四阶段把「记录器装了就崩」拆掉了（`glTexImage2D(pixels = NULL)` 被记录器当成 4.87 MB 去
`memcpy`），**5 次冷启动 5 次通过**：226/265/262/283/233 帧、100% `MATCH in-place rows`、
0 SIGSEGV、0 panic、448–449 KB 桌面截图。

但**「画面到底是谁画的」这个问题还没有一个干净的答案**。现在成立的事实只有三条：

1. `--no-panel` 下宿主重放 CA 的录制流，能画出一张桌面（11064 色、38.1% 非黑）；
2. 那张画面与客户机上传的 `0xFFFF0001` 面板「差一帧相位」：同帧相等 17/141，
   `panel(N) == picture(N-1)` 72/140，最长连续 20 帧；
3. 客户机一停止上传纹理（W1+W2 的 `remote_only`），宿主就黑屏 —— 但**帧率是够的**
   （2,536 帧、10–11 fps、记录 2.3–19 KB、`in-place rows` 都在），黑的原因写得很直白：
   那一配置下**一帧里没有任何纹理上传**（`0 image refs`）。

所以第五阶段的第一需求不是提速，是**把「宿主独立出图」这件事从「看起来成立」变成「证明成立」**，
再把 W1 与纹理上传拆开，让它真的能跑。

---

## 二、目标与验收（按优先级，全部可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| S1 | 判定 `--no-panel` 那张桌面是不是「面板自证」 | 停掉 `0xFFFF0001` 的上传、别的都不动，再跑 `--no-panel`：**画面还在** → 独立出图成立；**黑了** → 现有 MATCH 来自面板，写进文档并转 S2 的第二段 | 一切关于 W2 的结论 |
| S2 | W1.5：`remote_only` 时保留纹理上传、只跳过本地 draw/合成 | 该配置下每帧记录里 ≥1 个非面板纹理 ref（≥4 MB）；宿主 `--no-panel` 画面 >30% 非黑、截图 >300 KB；≥5 fps；记录 <100 KB；0 panic | R3/R4 |
| S3 | 一帧相位的来源与消灭 | 要么 `panel(N) == picture(N)` 连续 ≥100 帧；要么证明相位来自客户机 present 时机并把验收写成「picture(N) vs panel(N+1)」且给出证据 | R2 判据 |
| S4 | 触摸路径的稳定性 | 带触摸镜像冷启动 5 次 ≥4 次到界面、0 panic；滚动设置列表 20 s 期间 0 panic | R5 |
| S5 | 对照表与结论 | 四个配置（纯本地 / 按引用+面板 / W1.5 / W1.5+W3）各一行「滚动设置列表 20 s 的驱动稳态帧率」，同机同镜像；纯本地基线重测 | 结论 |
| S6 | 回归与可维护性 | `make test` 通过；请求行 4/5/6 字段兼容；每阶段一次提交且信息带前后数字；文档同步两处 | — |

> S1 是这一阶段的第一件事，而且只要一次启动。它决定 S2 和 S3 往哪边做：
> 如果那张桌面是宿主自己画的，S3 只是相位问题；如果不是，S2 就得先把「客户机合成的画面」
> 换成「宿主画出来的画面」再谈。

---

## 三、需求详述

### S1 判定「面板自证」（一次启动，半天）

**为什么必须先做这一步**：第四阶段的 `--no-panel` 里，客户机仍然把合成好的显示面
（`0xFFFF0001`，行跨度 3072、4,988,928 B 按引用）传给了宿主，而且它是**在重放之前上传**的
（`serve_frame` 的 "uploads first" 段），所以重放的 draw 完全有可能采样到它。需求文档里
「不做把客户机的画面当输入再画一遍」这一条，现在**既没有证据说不是、也没有证据说是**。

**做法**：加一个只停面板上传、不动本地合成的开关（例如 `glwire_nopanel_up=1`，走
`remote_boot_arg()`），跑一次 `--no-panel`：

- 画面还在（>30% 非黑、有图标/文字）→ 宿主确实在用 CA 的层纹理独立出图，S3 只是相位问题；
- 画面黑了 → 第四阶段那张桌面是面板自证；S2 必须先「把层纹理送到宿主」，否则 W2 无从谈起。

**判据**：一句话回答 + 一张存图（`GLWIRE_DUMP`）+ 该帧的 `replay … opcodes` 行。
**涉及**：`userland/src/eagl.c`（`frame_end()` 里面板上传那一段的开关）、`userland/src/eagl_remote.c`。
**回退**：开关缺省关，行为不变。

### S2 W1.5：保留上传、只跳过绘制（1–2 天，两三次启动）

**现状证据**（`/tmp/r3.log`，09:00 起机，`remote_only`）：

```
host_glwire: replay 18572 records B, opcodes 1:10 2:2 10:2 12:9 13:3 15:1 17:1 21:2 26:2 27:5 29:4 31:4 39:5 49:2 54:1 200:36, glerror 0x0
host_glwire: replay 2260 records B, opcodes 1:2 5:1 6:1 12:2 … 0 image refs
```

- 有 draw（12:9）、有顶点数组（200:36），**没有一条纹理上传**（opcode 22 / 201 都不出现）；
- 宿主于是「往不存在的纹理上画」，屏幕 8–14 KB（纯黑），帧率 10–11 fps。

**根因**：层纹理是跟着客户机**本地合成那条路**上传的（`frame_end()` 的 `surface_to_texture()`
与面板上传），`composition_off` 把这条路整个关掉了。W1 想「不画」，但代码里的开关是「不合成 +
不画」，两者绑在一起。

**做法**：把 `remote_only` 拆成两个动作：

1. **不执行**：`!local` 的那 36 条（draw/texImage/clear/blit…）照旧跳过本地执行——这是 R3 的收益；
2. **照旧上传**：把「IOSurface → 纹理」这件事换成**按引用记进记录流**（opcode 201 已经能带
   `(pa, len)` 段表），让宿主拿到层纹理的像素，而不是让客户机在 Mesa 里 `glTexImage2D`。

风险与已知坑：

- `glBindTexture` 必须本地执行（否则 `-attachImage:` 拿 `texture 0`、`images=0`、宿主收不到帧，
  第三轮踩过，`spec.def` 里已经写明）；
- 段数上限 2048（16 KB 页），超了会**静默回退内联**，症状是 `refs=0` 但记录 MB 级；
- 上传的源是 IOSurface，页面必须常驻，否则 `vtop` 整块 `EFAULT`；回退是内联（慢但正确）。

**判据**：

- 每帧 `≥1` 个非面板纹理 ref（≥4 MB），记录仍 <100 KB；
- 宿主 `--no-panel` 的存图 >30% 非黑、有桌面元素，截图 >300 KB；
- 宿主帧间隔 <200 ms（≥5 fps，`remote_only` 这一侧本来就 10–11 fps，别退化）；
- 0 SIGSEGV、0 panic。

**涉及**：`userland/src/eagl.c`（`frame_end()` 的合成/上传拆分）、`userland/src/eagl_remote.c`
（开关与 `remote_only` 语义）、`tools/glwire/{host_glwire.c,dispatch.inc,names.h}`（如果上传形状要改）。
**回退**：`/redwood/etc/glwire_local` 与 `glwire_local=1` 仍然把 W1 关掉。

### S3 一帧的相位（半天到一天，两三次启动）

**现状**（`/tmp/np3-host.log`，141 帧）：

| 关系 | 帧数 |
|---|---|
| 同帧 `panel(N) == picture(N)` | 17 / 141 |
| `panel(N) == picture(N-1)` | 72 / 140 |
| 最长连续相位对齐段 | 20 帧（到第 48 帧） |

**做法**：先定位，不要先改。

1. 把 `frame_end()` 的时间线打印出来（`glframes=1` 已有每帧一行；再加 present/upload 的时刻），
   确认面板上传发生在本帧 draw 之前、之后还是 present 之后；
2. 若上传在 present 之前，把 `0xFFFF0001` 的上传挪到 present 之后（或等 `glFenceSync`），
   再看 `panel(N) == picture(N)` 是否成立；
3. 若相位来自「宿主的画面写回客户机表面」，用 `GLWIRE_NO_INPLACE=1` + `glwire_nopicture=1`
   的干净对照确认（第四阶段那次对照碰上启动卡死，没做完）。

**判据**：连续 ≥100 帧同帧 `MATCH`；做不到就给出「相位 = 客户机 present 时机」的证据，
并把验收条件改成 `picture(N) vs panel(N+1)` 连续 ≥100 帧（这需要需求文档里明确修改，不能默认放宽）。

**涉及**：`userland/src/eagl.c`、`tools/glwire/host_glwire.c`。
**风险**：挪上传会让面板晚一帧，可能影响 R1 那条「0 崩溃」的现成证据——每次改动都要重跑一次冷启动。

### S4 触摸路径的稳定性（半天，先定位）

**现状证据（本阶段最重要的一条「别的东西坏了」）**：

| 镜像 | 触摸 | 冷启动结果 |
|---|---|---|
| `rc.drive`（rwtouch 在 120/150/480 s 起手，6 个进程） | 有 | **4/4 失败**：3 次 `Ticket spinlock[…] timeout` panic，1 次 `lsd` 刷屏卡死 |
| `rc.vtop`（同一内核，不带触摸） | 无 | **5/5 成功**（见 §一） |

**做法**：把变量一个个摘掉，别猜：

1. 只留**一个** rwtouch 进程、第一次动作放在 600 s 之后（`rc.drive` 的 6 条改成 1 条）；
2. 去掉 `pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE)` 再跑一次；
3. 用 `ustrace=1`（不是 `systrace`）抓 panic 前 `current owner` 那个 CPU 在干什么；
4. 看 `/dev/hakios-touch` 的驱动锁（`drivers/input/hakios.virtio_input.c`）是否在
   IRQ/工作线程里持锁跨了长操作。

**判据**：带触摸的镜像冷启动 5 次 ≥4 次到界面、0 panic；`rwtouch: mark scroll-start/scroll-end`
能在串口日志里成对出现，窗口内驱动自报帧率可读。
**回退**：R1 的冷启动只用 `rc.vtop`（现在就是这样）。

### S5 对照表与结论（两天，四次启动 + 四次纯本地基线）

四个配置，每格都是「滚动设置列表 20 s 的驱动稳态帧率」（`MiniIOMobileFramebuffer: N frames in M ms`
落在 `scroll-start..scroll-end` 窗口内的那几条；`tools/glwire/harness/fps.py` 直接算）：

| 配置 | 构建 | 期望（有保留） |
|---|---|---|
| 纯本地基线 | `GL=1 CONFIGD=1 RC=rc.vtop`（不装记录器） | 7–9 fps（第三轮数字，**本轮必须同机同镜像重测**） |
| 按引用 + 面板 + 行写 | `GLWIRE_LOCAL=1` | 4.0–6.0 fps（第四阶段空载数字） |
| W1.5（S2） | 不带 `GLWIRE_LOCAL` | ≥5 fps（空载已经 10–11 fps，滚动会掉） |
| W1.5 + W3 | 同上（W3 已默认在） | 同上，`copying out` 应接近 0 |

**判据**：一张表 + 一句话结论（能不能超过 9 fps、CA 自身 CPU 成本是多少）。
**风险**：S4 不通过就没有驱动稳态，这一行只能写「未测」。

### S6 回归与可维护性（贯穿）

1. `make test`（`map_test` + `e2e_test.py`）必过；宿主读回/上传形状一改就要跑。
2. 宿主接受 4 / 5 / 6 字段的请求行（向后兼容）。
3. 每阶段一次提交，提交信息带前后数字；文档同步到 `/home/lshan/dsh.MD/` 与 `iosvm/docs/`。
4. 开关清单（不许再加隐形开关）：`GLWIRE_INLINE`、`GLWIRE_LOCAL`、`glwire_local`、`glwire_mlock`、
   `glwire_nopicture`、`glwire_noflush`、`glwire_norecord`、`glwire_nopanel_up`(新)、
   `GLWIRE_NO_INPLACE`、`GLWIRE_NO_PANEL`/`--no-panel`、`GLWIRE_DUMP`、`GLWIRE_DUMP_FROM`、
   `GLWIRE_SAVE_RECORDS`。
5. 宿主诊断别删：`replay … opcodes` 行、`read-back framebuffer … last drawn into` 行、
   `panel … picture … previous picture` 三个哈希——这一阶段的每条结论都靠它们。

---

## 四、不做（已被数据否掉，别重试）

- 承诺 24 fps（CA 自身 CPU 成本在 TCG 下还没量出来，先量再说）。
- 像素去重（CA 每帧整屏重合成）。
- 客户机内启用 GL 加速渲染器 / llvmpipe（缺 arm64-iOS 的 libLLVM，且不在 CA 合成链上）。
- 只降分辨率（fbscale 3→1 只换来 2 倍，瓶颈是模拟执行）。
- 把客户机的画面当输入再画一遍（**第四阶段没证明不是这样，S1 就是来证明它的**）。
- `qemu/touch.py stacks`（本 QEMU 配置不能 halt CPU，会 panic）；`pkill -f`（会杀掉自己的 shell）。
- 并行跑第二台虚拟机 / 启动时编译（两次 kernel panic 都发生在并行任务期间）。

---

## 五、验证环境与操作规范（照这个做）

### 5.1 构建（约 70 s）

```bash
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
export THEOS=$HOME/theos IOSVM_USE_LLD=1
export PATH=$IOSVM/xnu-deps/host/bin:/tmp/cctools/bin:$PATH
cd $IOSVM/userland
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop GLWIRE_LOCAL=1 ./build.sh root.apfs   # 客户机照旧本地画（R2/R5 的一列）
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs                  # W1+W2/W1.5
GL=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs                             # 纯本地基线
```

### 5.2 一次冷启动 = 一次测量

脚本已经入库（`iosvm/tools/glwire/harness/`）：

```bash
cd /home/lshan/DSH-desktop/iosvm/tools/glwire
make                      # host_glwire / map_test / frame_replay
make test                 # 协议回归

# 一次冷启动，结束条件就是判据本身（宿主 ≥200 帧且截图 >300 KB；780 s 无 100 KB 截图 = 没界面，重来）
./harness/boot-measure.sh r2 /tmp/root-fixed-net.apfs 1000 --no-panel

# R1 那样的重复性验收（连跑到 N 次通过为止，卡死的那次不算）
IMAGE=/tmp/root-fixed-net.apfs WANT=5 ./harness/cold-boots.sh
```

**硬性禁忌**（第四阶段复现过）：同一时间只能有一台虚拟机（`payload.bin`、`ram.img`、端口 9110 共享）；
一个启动器没退出前不要起下一个（它的收尾会 `kill -9` 掉新虚拟机，第四阶段踩过）；
测量期间不要编译。

### 5.3 判读

```bash
grep -a "host_glwire: frame" /tmp/<label>-host.log | tail -3      # records / picture / panel / previous picture
grep -a "replay " /tmp/<label>-host.log | tail -1                 # 这一帧重放了哪些调用
grep -a "read-back framebuffer" /tmp/<label>-host.log | tail -1   # 读回的 FBO 与最后一次画进去的 FBO
grep -a "SIGSEGV\|panic(cpu\|usertrap:" /tmp/<label>.log | tail   # 崩溃
python3 harness/bgra2png.py /tmp/<dump>/f                         # 存图转 PNG + 非黑像素/颜色数
python3 harness/fps.py /tmp/<label>.log /tmp/<label>-host.log      # 窗口内驱动稳态
```

### 5.4 每个结论的证据要求

- 一个结论 = 一张日志行（原样贴）+ 一个数字 + 一次可复现的启动参数；
- 「没做到」也要写清楚卡在哪一行日志，不要写成「还需要优化」。

---

## 六、里程碑（每个一次提交，信息带前后数字）

| 里程碑 | 内容 | 判据 |
|---|---|---|
| M5.1 | S1：面板上传停掉后还有没有画面 | 一句话结论 + 存图 + `replay` 行 |
| M5.2 | S2：W1.5 跑起来 | 非面板 ref ≥1/帧、>30% 非黑、≥5 fps、0 panic |
| M5.3 | S3：相位定位并消灭（或改验收） | 连续 ≥100 帧同帧 MATCH，或相位来源的证据 |
| M5.4 | S4：触摸稳定的镜像 | 带触摸 5 次冷启动 ≥4 次到界面、0 panic |
| M5.5 | S5：四配置对照表 | 每格都是滚动 20 s 的驱动稳态，同机同镜像 |
| M5.6 | 文档 + 提示 | `/home/lshan/dsh.MD/` 与 `iosvm/docs/` 同步 |

---

## 七、诚实的预期

- **能确定的**：S1 和 S4 只要各一次/几次启动，能把「画面是谁画的」和「为什么带触摸就崩」
  这两件现在含糊的事变成有证据的事实。
- **不确定的**：S2 的收益。W1.5 让客户机不再做 texstore/光栅化，但 CA 自己的 CPU 代码仍在 TCG 下跑；
  第四阶段测到的「空载 10–11 fps」是在黑屏下量的，画面真的画出来之后会掉多少，只能实测。
- **不能承诺的**：超过 9 fps（纯本地基线的量级）。S5 的表出来之前，任何 fps 的说法都只是猜测。

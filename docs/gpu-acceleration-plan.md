# IOSVM GPU 加速 —— 后续优化工程文档

> 建立时间：2026-11-02
> 适用范围：`tools/glwire` 的按引用管道（已在 2026-11-02 那一轮打通）之后的提速工作
> 前置阅读：`/home/lshan/dsh.MD/IOSVM-GPU加速交接.md`（架构、证据、坑、环境手册）
> 本文只回答一件事：**接下来改什么、改哪里、怎么验、预期多少、怎么回退**

---

## 一、目标与验收

### 1.1 目标

| 阶段 | 判定 | 期望 |
|---|---|---|
| W0 | 按引用路径能连续跑 ≥ 200 帧，界面真的显示出来 | 阻塞项，不做完其它数字都不可信 |
| W1 | 记录器不再本地执行「重」调用 | 一帧 < 500 ms（≥ 2 fps） |
| W2 | 客户机不再本地合成（宿主自己出图） | 一帧 < 200 ms（≥ 5 fps） |
| W3 | 画面直接写进显示用 IOSurface | 一帧 < 125 ms（≥ 8 fps） |
| 24 fps | 不在本计划的承诺里 | CA 自身 CPU 代码在 TCG 下的成本尚未测过 |

### 1.2 每一阶段都必须保持的硬约束（回归项）

1. 宿主日志 `panel <hash> picture <hash> MATCH`（宿主的画面与客户机上传的画面逐字节一致）
2. 宿主日志 `records B` 仍是 KB 级（< 100 KB），`in-place` 仍在
3. `/redwood/etc/glwire_inline` 仍然能把一切退回旧的内联路径
4. 不新开虚拟机工程；不碰 `ipsw`；编译产物写 `/tmp`
5. 每一阶段单独提交，提交信息里带实测数字

### 1.3 基线（2026-11-02 第二轮实测，750×1624）

| 项目 | 数值 |
|---|---|
| 一帧记录 | 13,651 B |
| 回传画面 | 0（原地写） |
| 客户机交给宿主 | 99 µs |
| 一帧总耗时（客户机自报） | 1,852,902 µs |
| ├ 画面拷回 IOSurface | 58,234 µs |
| ├ IOSurface → 本地纹理回填 | 108,397 µs |
| └ 其余（本地执行 GL） | 1,686,168 µs |
| 帧间隔（第 2 帧） | 2.17 s（0.46 fps） |

> 上面这条「其余 1.69 s」就是 W1 的靶子，58 ms 是 W3 的靶子，108 ms 是 W2 的靶子。

---

## 二、W0：先让按引用这条路活下来（阻塞项）

### 2.1 现象

按引用之后，4 次启动（11:50 / 11:53 / 12:04 / 12:07）都只画到前两帧：`hakios-gl: frame 1/2` 正常、
`flush -> 1`、宿主 `MATCH in-place`，然后不再出帧，面板停在 Apple logo，串口被
`process lsd[32] denied attempt to escape CPU monitor` 刷屏。按值（内联）的启动在 11:16 跑过 1124 帧。

### 2.2 要做的隔离实验（按成本从低到高）

**(a) 内联对照 —— 需要给 `build.sh` 加一个 2 行的开关**

```bash
# userland/build.sh，root/ 组装完之后、mkapfs 之前
[ -n "$GLWIRE_INLINE" ] && : > root/etc/glwire_inline
```
```bash
cd userland && GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop GLWIRE_INLINE=1 ./build.sh root.apfs
```
判读：内联跑得动 → 卡死是本轮新路引入的（按引用 / 原地写 / 556 三者之一）；
内联照样卡 → 是本轮无关的既有卡死概率（40% 连中四次 ≈ 2.6%，偏巧但不排除）。

**(b) 只关「原地写」—— 需要给 `host_glwire` 加一个 3 行的开关**

```c
/* main() 里 */
if (getenv("GLWIRE_NO_INPLACE") != NULL) {
        no_in_place = 1;              /* 段表照样收，回复照旧发画面 */
}
/* serve_frame() 里算 in_place 之前 */
if (no_in_place) segments = NULL;
```
```bash
GLWIRE_NO_INPLACE=1 ./host_glwire 9110 qemu/ram.img
```
判读：只有关掉原地写才不卡 → 问题在「宿主往客户机内存里写」这一侧（考虑页框在查询后被
压缩/搬移，见 2.4 风险）；仍卡 → 问题在按引用读或 556 本身。

**(c) 卡住时抓栈**

卡住的 QEMU 还活着时抓客户机进程栈（做法见 `docs/research/linux-kernel-build.md` 的 stackshot
附录：`qemu/symstack.py`）。要回答的问题：SpringBoard 是停在 `syscall(556)` 里，还是停在显示驱动
的 `swap` 等待上。

### 2.3 已知的两个可疑点

1. **页框可能在查询与写回之间变动**：`hakios_vtop` 只保证「查询那一刻」的页表；客户机 12 GB 有
   压缩器，若页被压缩/搬移，宿主写到的就是旧页。窗口只有「flush 发出到宿主写回」这一段（毫秒级），
   但 SpringBoard 是空闲等待，页面完全可能被压缩。
   对策候选：查询后由客户机 `mlock`/`wire` 住这段内存（用户态可用 `madvise(MADV_WILLNEED)` 或
   内核侧加一个「钉住到帧结束」的调用），或让宿主先写回、客户机收到 `R 0` 后**校验**一次内容
   标记（在缓冲区尾部放一个序号，宿主写回时一并写，客户机核对）。
2. **556 在持锁路径里被调用**：`eagl.c` 的上传发生在 `IOSurfaceLock` 期间；`hakios_vtop` 内部
   `kalloc_data(Z_WAITOK)` 可能睡眠。虽然睡眠本身合法，但配上 pmap 锁要逐案确认。对策：把 556 的
   实现改成不分配（调用方给缓冲区），或在上传前把段表算好、不要在锁内查。

### 2.4 验收

- 冷启动 5 次，≥ 4 次能连续跑 ≥ 200 帧，截图有界面（> 300 KB 的 PPM/PNG）
- 宿主日志连续 200 帧 `MATCH in-place`
- 把结论写回本文与交接文档

---

## 三、W1：记录器不再本地执行「重」调用（最大收益）

### 3.1 动机

`remote.inc` 的每个中转现在长这样（`gen.py: emit_slots()`）：

```c
if (local_entry[i] != NULL) ((void (*)(...))local_entry[i])(context, ...);   /* 真执行 */
gw_glXxx(...);                                                              /* 记录 */
```

按引用之后，宿主已经用同一块像素把这一帧画好了，本地这次执行是纯浪费：一帧 64 条调用里那张
4.98 MB 的纹理上传，在 Mesa softpipe 里做 texstore 就是 > 1 s。

### 3.2 分类（依据：宿主是否需要客户机的本地状态来做名字对齐）

| 类别 | 调用 | 本地要不要跑 |
|---|---|---|
| 重、纯绘制/上传 | `glTexImage2D`、`glTexSubImage2D`、`glDrawArrays`、`glDrawElements`、`glClear`、`glClearColor`、`glScissor`、`glViewport`、`glFinish`、`glFlush`、`glCopyTexSubImage2D`、`glGenerateMipmap`、`glBlendFunc*`、`glEnable/Disable`、`glBindTexture`、`glTexParameter*`、`glUniform*`、`glUseProgram`、`glVertexAttribPointer`、`glEnable/DisableVertexAttribArray`、`glPixelStorei`、`glActiveTexture`、`glAlphaFunc` | **不跑** |
| 回答名字/状态 | `glGenTextures`、`glGenFramebuffers`、`glCreateShader`、`glCreateProgram`、`glGetUniformLocation`、`glGetString`、`glGetIntegerv`、`glGetError`、`glFenceSync`、`glClientWaitSync`、`glDeleteTextures`、`glDeleteFramebuffers`、`glDeleteProgram`、`glDeleteSync` | 必须跑 |
| 需要观察 | `glCompileShader`、`glLinkProgram`、`glAttachShader`、`glBindAttribLocation`、`glShaderSource` | 先保留，W1 之后单独测 |

> 第二类为什么必须跑：客户机的对象名是权威（宿主用它做 `names.h` 的映射），`glGetIntegerv`
> 等还要把客户机的状态答给 CA。

### 3.3 改动清单

1. **`tools/glwire/spec.def`**：给「不本地执行」的调用加标记，例如行尾 `!local`
   ```
   glTexImage2D(E, I, I, S, S, I, E, E, x) !local
   ```
2. **`tools/glwire/gen.py`**
   - `parse()`（约 122 行）的正则加一段可选后缀，`Call` 增加 `self.no_local`
   - `emit_slots()`（约 258 行）：标记过的调用把本地调用包起来
     ```c
     if (!remote_only && local_entry[N] != NULL) { ... }
     ```
     （返回值的调用：`result` 初值仍是 0，客户机不再回答，宿主也不需要它）
   - 不发 `remote_only` 的东西：它由 `eagl_remote.c` 定义
3. **`userland/src/eagl_remote.c`**
   ```c
   /* 宿主在画整帧时，客户机不再执行这些调用；回退到内联时自动关掉 */
   static int remote_only;
   ...
   remote_only = (wire_vtop_hook != NULL);
   ```
   再给一个强制关闭的开关 `/redwood/etc/glwire_local`（A/B 用）。
4. **重新生成**（在 `tools/glwire` 里，然后把产物拷进 `userland/src`）
   ```bash
   python3 gen.py spec.def slots   > remote.inc
   python3 gen.py spec.def encode  > encode.inc
   python3 gen.py spec.def dispatch > dispatch.inc
   python3 gen.py spec.def table ../../iosvm-work/hakios-xnu-kernel/userland/src/eagl_slots.txt > remote_table.inc
   ```
5. **`userland/src/eagl.c`**：`surface_to_texture()`（约 705–780）里那次本地 `glTexImage2D` 已经
   是直达指针，不在表里 —— 它属于 W2 的范围，这里不动。

### 3.4 验证

- 客户机日志 `hakios-gl: frame N:` 的「其余」项应当大幅下降（现在 1.69 s）
- 宿主 `records B` 仍是 KB 级、`MATCH` 仍在（W1 只让客户机少画，没有改像素来源）
- 用 `glwire_local` 开关做 A/B，同一次启动内各跑 30 帧对比

### 3.5 预期与风险

- 预期：去掉 1.69 s 里的大部分 softpipe texstore / 光栅化；一帧落到 0.2–0.5 s 量级
- 风险 A：某些调用被 CA 用来「读回自己刚画的东西」（`glReadPixels` 不在 62 个里，但
  `glCopyTexSubImage2D` 在）→ 已列入不执行，若出画面异常先把它单独放回
- 风险 B：客户机的 GL 对象不再有其实体（没有 storage 的纹理），若某条后续调用因此报错，
  `glGetError` 会把错误答给 CA → 打开客户机 `glGetError` 的计数日志定位

---

## 四、W2：客户机不再本地合成（前置：宿主自己要能出图）

### 4.1 动机

`eagl.c: frame_end()` 现在仍然：`target_to_surface()`、`show_texture()`（约 1096–1104）、
`surface_to_texture()` 的本地回填（108 ms）、以及最后把宿主画面拷进 IOSurface（58 ms）。
这些只对「本地绘制」有意义。

### 4.2 先做一个决定性的实验（不需要改代码）

宿主现在**一定会**用客户机上传的面板（`names.h` 里的 `0xFFFF0001`）来出图；把这条关掉，让它只重放
CA 的绘制流，然后比对：

```bash
./host_glwire --no-panel 9110 qemu/ram.img        # 需要 3 行改动：面板仍捕获用于哈希，但不参与绘制
```
- `panel … picture … MATCH` → 宿主能独立出图，W2 可以做，且客户机的合成可以停
- `DIFF` / 黑屏 → 宿主还画不出界面，W2 只能做到「跳过 surface_to_texture，保留上传」
  （能拿到 108 ms，拿不到合成那部分）

> 上一轮观察到「重放 CA 流会 fault」是按值时代的记录（上传与 FBO 的顺序问题）；按引用之后
> 上传不再占用大块内存，值得重测一次。

### 4.3 改动清单（前提：实验给出 MATCH）

1. `userland/src/eagl.c: frame_end()`：`remote_only` 时跳过 `target_to_surface()`、
   `show_texture()`、`surface_to_texture()` 的本地上传与面板上传（`0xFFFF0001` 那条），
   只保留 flush + 画面拷入 IOSurface
2. `userland/src/eagl_remote.c`：`eagl_remote_upload()` 与面板上传不再被调用（代码留着）
3. 宿主：`draw_panel()` 不再被使用，`names.h` 的面板捕获保留（验收哈希要用）

### 4.4 验证

- `MATCH` 仍在（现在比的是「宿主重放 vs 宿主重放」，意义变弱 → 改为与客户机自身的
  framebuffer 比对：`qemu/screendump.sh` 与宿主保存的 picture 做逐像素比较，脚本 `panel_probe.py`
  已有读取逻辑可复用）
- 截图有人机可用界面（> 300 KB）

### 4.5 预期与风险

- 预期：去掉 108 ms 回填 + 本地 draw 的时间（后者未单独测）
- 风险：CA 里可能有「依赖本地 GL 结果的路径」（例如用 `glGetIntegerv` 读 viewport 后算命中）——
  W1 保留的那一类调用已经覆盖；若出现错位/闪烁，先怀疑这里

---

## 五、W3：画面直接写进显示用 IOSurface（省 58 ms）

### 5.1 改动清单

1. **客户机** `eagl.c`：拿着 `IOSurfaceGetBaseAddress(e->images[0].surface)` 与
   `IOSurfaceGetBytesPerRow()`，在 flush 前用 556 取段表；把「画面段 + 行跨度 + 高」放进请求行。
   注意 `IOSurfaceLock(surface, 0, NULL)`（可写）包住整个交换过程。
2. **协议**：请求行加一个字段
   ```
   -> R <w> <h> <records B> <picture B> <row bytes> <segments>
   ```
   宿主按 `row_bytes` 分行写；一段可能横跨两行，需要先建「虚拟偏移 → 文件偏移」的区间表。
3. **宿主** `host_glwire.c: serve_frame()`：把 `glReadPixels` 的结果按行写到段表指向的内存，
   `R 0` 回复照旧；段不覆盖整帧时回退到「写进客户机给的打包缓冲」（现状）。
4. **客户机** flush 后不再 `memcpy` 进 IOSurface（`eagl.c` 约 1143–1146 那段循环删掉）。

### 5.2 验证与风险

- 验证：`picture` 与截图的逐像素比较；客户机日志的「copying out」应降到 ~0
- 风险：往 IOSurface 的物理页写需要该内存**在位且被锁住**；被压缩/换出时 556 会 EFAULT，
  回退路径必须有（保留现在的打包缓冲写法）

---

## 六、顺序、依赖与里程碑

```
W0（卡死隔离）──┬──> W1（不本地执行）──┬──> W2（宿主出图）──> W3（直写 surface）
                └──> 若 W0 证明是既有卡死概率，W1 可并行开工
```

- W0 没结论就做 W1 也可以（W1 只改本地执行），但**帧率数字一律标注「单帧样本」**
- W2 是否值得做，取决于 4.2 的实验结果；不值得就只做「跳过 surface_to_texture」那一半
- 每个里程碑各提交一次，提交信息带前后数字

---

## 七、测试矩阵

| 维度 | 取值 |
|---|---|
| 分辨率 | `fbscale=2`（750×1624，主测）/ `fbscale=3`（1125×2436，压测） |
| 宿主能力 | 有 `ram.img`（按引用）/ 无 `ram.img`（内联回退） |
| 客户机开关 | `/redwood/etc/glwire_inline`（内联）/ 无（按引用）/ `/redwood/etc/glwire_local`（W1 后：本地执行开） |
| 场景 | 首页静止、设置列表滚动 20 s、打开 Control Center（模糊，最重） |

每次测三样：客户机 `hakios-gl: frame N` 的分解、宿主 `records/refs/MATCH/in-place/interval`、
驱动 `frames in` 的稳态帧率（≥ 50 帧再取）。

---

## 八、度量方法（照这个读日志）

```bash
log=$(ls -t /tmp/tmp.* | head -1)                       # 客户机串口
grep -a "hakios-gl: frame " $log | tail -5              # 每帧分解：handing on / waiting / copying out / 其余
grep -a "frames in " $log | tail -3                     # 稳态帧率
grep -a "host_glwire: frame" /tmp/glwire*.log | tail -5 # records B / refs / MATCH / in-place / interval
python3 tools/glwire/vtop_verify.py $log qemu/ram.img   # 内核接口（每次启动都跑一次）
```

判读要点：
- `records B` 变大 → 某条上传又退回内联（先查段数上限与 `image refs`）
- `refs` 为 0 且记录是 MB 级 → 段数上限或 556 不可用（见交接文档 §7.1）
- `MATCH` 变 `DIFF` → 宿主画出来的和客户机上传的不一样，先查 W1/W2 有没有动像素来源
- 分解里「其余」不降 → W1 没生效（`remote_only` 没打开，或镜像没重建）

---

## 九、不要做的（已有数据否掉）

- 像素去重（CA 每帧整屏重合成）
- 客户机内启用 GL 加速渲染器（softpipe 在 TCG 里 7 分钟出不了第一帧）
- llvmpipe（缺 arm64-iOS 的 libLLVM，且不在 CA 合成链上）
- 只降分辨率（fbscale 3→1 只换来 2 倍，瓶颈是模拟执行）
- 把 CA 的绘制流「搬到宿主再回传整帧」而不解决客户机本地执行 —— W1 不做，其它都是白费

---

## 十、文档与代码索引

| 位置 | 内容 |
|---|---|
| `/home/lshan/dsh.MD/IOSVM-GPU加速交接.md` | 架构、证据、坑、环境手册（本文的前置） |
| `iosvm/docs/gpu-acceleration-handoff.md` | 同上（仓库副本，内容一致） |
| `iosvm/patches/0009-hakios-vtop-syscall.patch` | 内核侧 syscall 556 |
| `iosvm/tools/glwire/gen.py` | 两侧代码的生成器（W1 改这里） |
| `iosvm/tools/glwire/spec.def` | 62 个调用的唯一描述（W1 加标记） |
| `userland/src/eagl_remote.c` | 客户机记录器与 flush（W1 的 `remote_only`） |
| `userland/src/eagl.c` | 本地合成与画面回填（W2/W3） |
| `iosvm/tools/glwire/host_glwire.c` | 宿主重放、映射、原地写（W3、2.2(b)、4.2） |

---

## 十一、诚实的预期

- W1 是收益最大、也最像「真实工程」的一步：它把「客户机不该画」这件事变成代码，而不是再猜。
- 但 **24 fps 不是这份计划能承诺的**。本轮分解里「其余 1.69 s」只测到总量，没有拆到
  「CA 的 CPU 代码」与「softpipe 的 texstore」各自多少；W1 做完才知道剩下的地板在哪。
- 按当前实测，做完 W1+W2+W3 的理论地板是「58 ms 拷回（W3 可省）+ 宿主 6–14 ms」，
  但如果 CA 自己的 CPU 部分就要 100 ms+，那 24 fps 需要换的是执行层（例如把 CA 的热点
  编译成本机代码、或改用硬件加速的 guest 渲染），而不是这条路继续挤。

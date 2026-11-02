# IOSVM GPU 加速 —— 后续优化工程文档

> 建立时间：2026-11-02
> 适用范围：`tools/glwire` 的按引用管道（已在 2026-11-02 那一轮打通）之后的提速工作
> 前置阅读：`/home/lshan/dsh.MD/IOSVM-GPU加速交接.md`（架构、证据、坑、环境手册）
> 本文只回答一件事：**接下来改什么、改哪里、怎么验、预期多少、怎么回退**

---

## 零、执行记录（2026-11-02 第三轮，本轮实测）

### 0.1 一句话

W1/W2/W3 的代码全部落地、每一条都有开关可以单独关掉（下面 0.4）；但 **「按引用这条路活不下来」没有解决**，
而且这一轮把它定位得更准了：卡死与 vtop、原地写、mlock **都无关** ——

> 同一个镜像，去掉 `GLREMOTE=1`（完全不装记录器、不 flush），**桌面正常出来**，驱动自报 7–9 fps，
> 670 s 内 0 次崩溃；把记录器装回去（按值或按引用，有/无原地写，有/无 mlock），**全部停在
> 「Apple logo → 转圈」**，`backboardd` 反复 `exited due to SIGSEGV`（pid 16/80/99/105…），
> 有时内核直接死在 `Ticket spinlock[...] timeout` 上。

所以 W0 的靶子是「记录器装上了 + 每帧 flush」这条共同路径，不是这一轮新加的任何一半。

### 0.2 阶段状态

| 阶段 | 状态 | 依据 |
|---|---|---|
| W0 卡死隔离 | ❌ 未达标（界面没出来），✅ 定位完成 | 五种配置的对照，见 0.3；`GLWIRE_INLINE` / `GLWIRE_NO_INPLACE` / `glwire_mlock=0` 都试过，都照样崩 |
| W1 记录器不本地执行 | ✅ 代码完成（36/62 条 `!local`） | `spec.def` + `gen.py` + `eagl_remote.c:remote_only`；第一次实测踩到 `-attachImage:` 回归（`images=0`，宿主一帧都收不到），修法：`glBindTexture` 必须本地执行，已在 spec.def 里写明白 |
| W2 客户机不本地合成 | ✅ 代码完成 | `frame_end()` 里 `composition_off`：不 `target_to_surface`、不 `show_texture`、不上传 `0xFFFF0001` 面板 |
| W3 直写显示用 IOSurface | ✅ 代码完成，⚠️ 端到端未验证 | 请求行第 6 字段带行跨度，宿主按行写、客户机不再 memcpy；实测 `flush -> 2 (rows into the surface)` 生效，但那一机画面本身是黑的（W2 的后果），所以「行写对不对」还没有一张正确的图来验收 |

> §4.2 那个决定性实验的答案：**DIFF**。不做面板上传之后，宿主只重放 CA 的绘制流，
> 屏幕是纯黑（截图 8,061 B）。所以 W1 与 W2 目前**不能同时开**：W1 让客户机不再画、
> W2 让宿主没有面板可抄，两边都不出图。

### 0.3 实测数字（750×1624，`fbscale=2 metal=1`，QEMU/TCG + Tesla V100）

| 配置 | 一帧记录 | 宿主 | 客户机 | 结果 |
|---|---|---|---|---|
| 无远程渲染（`GL=1` 不带 `GLREMOTE`） | — | — | 驱动 7–9 fps | ✅ 桌面出来，0 崩溃，截图 447,226 B |
| 按引用 + 原地写 | 8,373 B，1 个 image ref（4,988,928 B） | 0.04 ms/帧 | 单帧 77,159 µs，其中画面拷进 surface 48,681 µs | 137 帧全 `MATCH in-place`，帧间隔 0.08–0.12 s（8.7–12.6 fps，驱动自报 11 fps），**但 6 分钟后 backboardd 开始 SIGSEGV，界面不出** |
| 按引用 + 不原地写（`GLWIRE_NO_INPLACE=1`） | 8,373 B | 0.04 ms/帧 | 单帧 2.78 s（其中 530 ms 收 4.87 MB 画面） | 1.65 fps，1 次 SIGSEGV，界面不出 |
| 按值（`GLWIRE_INLINE=1`） | 4,992,421 B | 0.04 ms/帧 | 单帧 1.35 s | 0.74 fps，2 次 SIGSEGV，界面不出 |
| 按引用、不启动宿主 | — | — | 单帧 3.27–3.65 s | 照样卡住，并死在 `Ticket spinlock` panic 上 |
| W1+W2+W3（`remote_only`） | 2,424–3,444 B | 19–30 fps | 不画、不合成、不拷贝 | `flush -> 2 (rows into the surface)`，**屏幕黑**，之后又崩溃 |

对照上一轮（本文件 §1.3）：一帧 1,852,902 µs → 现在 77,159 µs（24 倍），
「其余 1.69 s」消失了，「画面拷回 IOSurface」58 ms + 「IOSurface 回填」108 ms 也一并消失
（回填变 0，拷贝那 48.7 ms 正好是 W3 要去掉的部分）。**注意**：这 77 ms 是转圈动画那一帧
（44 条调用、0 次纹理回填），不是主界面滚动。

### 0.4 这一轮新增的开关（都在，出问题不用重编）

| 开关 | 位置 | 作用 |
|---|---|---|
| `GLWIRE_INLINE=1`（构建） | `userland/build.sh` | 放进 `/redwood/etc/glwire_inline`：按值（旧线格式） |
| `GLWIRE_LOCAL=1`（构建） | `userland/build.sh` | 放进 `/redwood/etc/glwire_local`：客户机照旧本地绘制（W1 关） |
| `glwire_local=0/1`（启动参数） | `eagl_remote.c` | 同上的运行时版本，A/B 不用换镜像 |
| `glwire_mlock=0`（启动参数） | `eagl_remote.c` | 不给画面缓冲 mlock |
| `glwire_nopicture=1`（启动参数） | `eagl.c` | 宿主照画，但画面不写回（隔离「画面回填」这一半） |
| `GLWIRE_NO_INPLACE=1` / `--no-panel`（宿主） | `host_glwire.c` | 不往客户机内存里写 / 不画面板、只重放记录 |
| `--save DIR`、`GLWIRE_DUMP=前缀`（宿主） | `host_glwire.c` | 每帧/首帧画面存盘（现在没有面板也会存） |

### 0.5 下一步（按这个顺序）

1. **把 backboardd 的 SIGSEGV 抓到具体地址。** 这一轮试过 `systrace=1`（会连系统调用一起打印，
   太吵、把机器拖慢），`qemu/touch.py stacks`（**会把本端口带进 debugger，然后 panic**，见 0.6）。
   更靠谱的下一步：给镜像加一个 `EXTRA=` 的小程序，在 backboardd 重启后把
   `/private/var/mobile/Library/Logs/CrashReporter/*.ips` 打到串口；或者让 `user_exception_trace`
   只对 `backboardd` 打印（它已经有 `usertrap:` 的格式，缺的是一只过滤器）。
2. **把「记录器」与「flush」分开。** 现在两者一起开，一起崩。第一步只装记录器、`eagl_remote_flush()`
   直接 `return 0`（一帧都不发），看是否还崩：不崩 → 崩在 flush/画面回填那一侧；还崩 → 崩在
   62 个槽位被替换这件事本身（那就是槽位偏移或某个签名的问题，用二分法缩小到具体调用）。
3. **W2 的前提要重新确认。** 现在宿主只会画面板，`--no-panel` 黑屏。要么补宿主的 CA 重放
   （shader/状态/纹理上传顺序，这是 §7.2 说的那件事），要么承认「客户机合成 + 面板上传」暂时留着，
   只做 W3 省那 48.7 ms。

### 0.6 坑（本轮新增，别重走）

1. **`qemu/touch.py stacks NAME` 会让本端口 panic。** 它走 debugger 入口，而这个 QEMU 配置下
   `Attempting to forcibly halt cpu N … halt not supported for this configuration`，随后
   `Debugger synchronization timed out` → `Ticket spinlock timeout` → panic。要栈就用启动参数
   `ustacks=NAME ustacks_at=…`，它在定时器里采样，不打扰其他 CPU（本轮用过 120 次，正常）。
2. **主机忙的时候启动会更容易「卡死」。** 本轮两次 panic 都发生在我在同一台机器上并行编译
   userland / 跑第二台 QEMU 的时候；`cpus=4` 的 TCG 里一个 vCPU 被调度走几秒，就会触发
   ticket spinlock 超时。要量数字就只跑一台机器、别同时编译。
3. **`glBindTexture` 一旦不本地执行，`-attachImage:` 就拿到 `texture 0`**（eagl.c 靠自己的
   binder 记 `bound[]`），于是 `images=0`、宿主一帧都收不到。spec.def 里已经不标它。
4. **`systrace=1` 不只是异常跟踪**，它把 syscall 也全打出来（`systrace: lsd[32] fcntl(...)`），
   日志几 MB、机器明显变慢。只在抓崩溃时短开。
5. **`GLWIRE_DUMP` 原来只在有面板时才存图**，W2 之后就没有面板了，所以改了：
   现在没有面板也存 `-picture-` 那一份。

### 0.7 第四轮：记录器与 flush 拆开了，还是崩（2026-11-02 15:32）

**做法**：新开关 `/redwood/etc/glwire_noflush`（`GLWIRE_NOFLUSH=1` 构建，或 boot-arg
`glwire_noflush=1`）。装记录器、62 个槽位照旧替换、每条调用照旧记录；`eagl_remote_flush()`
一进来就 `wire_reset()` 然后 `return 0`，`eagl_remote_surface()` 也直接返回 0 ——
没有 socket、没有 556 vtop、没有 IOSurfaceLock、没有画面回拷。镜像
`/tmp/root-noflush-local.apfs`（`glremote_on` + `glwire_local`：客户机仍然本地绘制，
只留下「记录器」这一个变量）；内核 `hakios-kernel` 15:23（带 `ustrace` 过滤器）。

**实测**（750×1624，`fbscale=2 metal=1`，`QEMU_SMP=4`，
`CMDLINE="cpus=4 fb_beat=0 fbscale=2 metal=1"`，`/tmp/iosvm-1500.log`）：

- 15:26:20 起机；日志 **297 次** `hakios-remote: flush -> 0`，没有 host_glwire、没有连接；
- 15:32:40 `(user/501/com.apple.backboardd [14]) exited due to SIGSEGV`（起机后 **6 分 20 秒**），
  15:32:41 以 pid 77 重启，之后 `frames in` 14 fps；
- 截图 8,061 B（纯黑，符合预期：没有画面回填）。

**结论**：**崩不在 flush，也不在画面回填**。一帧都没发、一次 vtop 都没做，backboardd 照样在
同一时间尺度上 SIGSEGV；变量就是「62 个槽位被替换 + 每次调用都记录」这件事本身。
下一个二分对象是槽位/recorder，而不是网络或 IOSurface。

**坑（新）**：`IOSVM_USE_LLD=1` 会让内核走 `ld64.lld`，而它不实现
`-image_base` / `-segaddr`（只 warning），链出来的 `__TEXT` 在 0，loader 直接
`unexpected kernel layout`。用 cctools 的 `aarch64-apple-darwin11-ld`（`IOSVM_USE_LLD=0`），
并且删掉 `BUILD/obj/.../mach.development.bcm2837{,.unstripped}` 强制重链。

### 0.8 第四轮：ustrace 抓到调用点，NULL 像素（2026-11-02 15:44）

**内核过滤器**：`osfmk/arm64/user_exception_trace.c` 新增两个独立启动参数 ——
`ustrace=1` 只开异常跟踪（不碰 `systrace` 的 syscall 打印），`ustrace_name=NAME` 只打印该进程。
命令是 `ustrace=1 ustrace_name=backboardd`，其余配置与 §0.7 完全相同（noflush、本地绘制）。
**没有用 `qemu/touch.py stacks`。**

15:44:16 抓到（`/tmp/iosvm-1501.log`，shared cache slide `0x49b34000`）：

```
usertrap: backboardd[14] exception 1 code 0x1 0x0:
  pc 0x21a76fdf4 lr 0x10285dc04 sp 0x16ffaf960, shared cache slide 0x49b34000
  called from 0x10285f928
  called from 0x1d1f321b0 ... (共享缓存里的 CA 帧)
hakios: signal 11 at 0x0, pc 0x1d0c3bdf4, lr 0xb8d29c04,
  called from 0xb8d2b928 0x1883fe1b0 0x188333e00 ...
```

把 slide 减掉后的链条：

| 地址 | 模块 | 含义 |
|---|---|---|
| `pc 0x1d0c3bdf4` | `libsystem_platform.dylib`（基址 `0x1d0c3b000`，+`0xdf4`） | `memcpy` 循环里的 `ldnp x12,x13,[x1]`，`exception 1 code 0x1` = 读地址 0 |
| `lr 0xb8d29c04` | `libhakios.gl.dylib`（基址 `0xb8ccc000`，+`0x5dc04`） | `_wire_image` 里 `bl _memcpy`（`0x5dc00`）的下一条 |
| `0xb8d2b928` | 同上，+`0x5f928` | `_gw_glTexImage2D` 里 `bl _wire_image`（`0x5f924`）的下一条 |

崩溃前最后一条客户机日志正是：

```
hakios-gl: a new kind of texture: empty, internal format 0x1908, format 0x1908,
           type 0x1401, 750 x 1624
```

**根因**：`glTexImage2D(..., pixels = NULL)` 是合法的「只分配存储」；但 `wire.h:wire_image()`
用 `width * height * 4` 算出非零 `count`，只在 vtop 分支检查了 `pixels != NULL`，落到
inline 分支后照样 `wire_put(w, pixels, count)` → `memcpy(dst, NULL, 4.87 MB)`。
不是槽位偏移、不是签名，也不是 flush；是一条 recorder 自己把 NULL 当数据。

**修法**：`wire_image()` 与 `wire_bytes()` 的 size 都改成
`count > 0 && bytes != NULL` 才算数；NULL 就记 0 字节。宿主收到 0 字节，照 GL 语义分配空纹理。

**二分**：`ustrace` 已经把 PC/栈/调用点指到 `gw_glTexImage2D`，比半槽二分更直接，所以没有
再花两轮启动去二分。`glwire_slots=MASK` 开关已经落地（`gen.py` 生成
`remote_slot_offset[]`，`eagl_remote_install()` 把 mask 外的槽位还原为 `local_entry`），
`glTexImage2D` 是 spec 索引 22，下一轮若要半槽复现可以直接用
`glwire_slots=0x3fffffff80000000`（只留 31–61，不含 22）。

### 0.9 第四轮：修完之后的 noflush 对照（2026-11-02 16:16）

同一套 noflush + 本地绘制 + `ustrace=backboardd`，镜像 `/tmp/root-fixed-local.apfs`
（`wire.h` 已修），boot-arg 加 `glwire_noflush=1`，`/tmp/iosvm-1502.log`：

- 16:01:07 起机，跑满 15 分钟到 16:16:03；
- **0 次 `SIGSEGV`，0 条 `usertrap: backboardd`**；
- `flush -> 0` **4,583 次**；
- 空纹理 `750 x 1624` 照常出现（日志有那一行），不再崩；
- `frames in` 6–8 fps，驱动侧 22 ms/帧。

对照 §0.7（同配置未修）：6 分 20 秒时 `backboardd [14]` SIGSEGV。修的是同一行，结论成立。

### 0.10 第四轮：修完的 flush-on 实测，桌面出来了（2026-11-02 16:38）

这次连宿主一起跑，验证完整的「记录器 → flush → 宿主重放 → 行写回 IOSurface」路径：

- 镜像 `/tmp/root-fixed-net.apfs`：`GL=1 GLREMOTE=1 GLWIRE_LOCAL=1 CONFIGD=1 RC=rc.vtop`，
  即客户机仍本地绘制、记录器全开、`rc.vtop` 用 `netcfg` 把 en0 配成 10.0.2.15/10.0.2.2；
- 启动：`QEMU_SMP=4`、`CMDLINE="cpus=4 fb_beat=0 fbscale=2 metal=1 ustrace=1 ustrace_name=backboardd"`，
  日志 `/tmp/iosvm-1504.log`；宿主 `host_glwire 9110 qemu/ram.img`，日志 `/tmp/iosvm-1504-host.log`；
- 16:28:53 起机，宿主 `ESTAB` 到 9110；
- 前 3 帧慢（已知的启动停顿，第 3 帧间隔 271 s），之后恢复；
- **16:37 截图 448,734 B，桌面出来了**：Dock、图标、搜索框、顶部 `rdar:45025538`；
- 16:38 宿主 `frame 152, 750x1624, 7439 records B, 4872000 picture B, 0.03 ms,
  1 image refs (4884992 B), panel … picture … MATCH in-place rows`，帧间隔 0.07 s（**13.83 fps**）；
- 客户机 `frame 140`：66 GL calls / 9 draws，142,231 µs；
- **0 次 `SIGSEGV`，0 条 `usertrap: backboardd`**；空纹理 `750 x 1624` 照常出现并被安全处理。

对照 §0.7/0.8（同一路径未修）：6 分 20 秒 SIGSEGV、界面不出。修 `wire_image()` 的 NULL 之后，
同一条路径连续 152 帧、宿主逐帧 `MATCH in-place rows`、桌面可用。**R1 的阻塞点已经拆掉**；
剩下的是重复性验收（冷启动 5 次 ≥4 次）和速度，不再是「记录器装了就崩」。

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

## 二、W0：测量可靠性（不是代码缺陷，已查清一半）

### 2.1 现象与结论

前两帧之后不再出帧，屏幕停在 Apple 开机 logo，lsd 刷屏。**两个实验已经把它与本轮改动切开：**

| 实验 | 开关 | 结果 |
|---|---|---|
| 把画面缓冲钉住（排除「页被搬移后被写错地方」） | `userland/src/eagl_remote.c` 的 `mlock()` | `picture buffer 4872000 B + 8, mlock 0 (wired)`，**照样只画两帧** |
| 走旧的内联线（排除「按引用」本身） | `GLWIRE_INLINE=1 ./build.sh root.apfs` | `records 9,959,387 B, 0 image refs`，**同样只画两帧** |

**但这两条还不够**：12:50 之后的第五个对照把结论改了 —— 同一个镜像，**去掉 `GLREMOTE=1`
（完全不装记录器、不 flush、没有 `glwire_inline`/`glwire_local` 这些文件）**，那台机器
**正常启动到桌面**：驱动自报 7–9 fps，670 s 内 900+ 帧，0 次 `SIGSEGV`、0 次 panic，
截图 447,226 B（有小组件、Dock、图标）。所以「两帧之后卡死」**不是本端口既有的概率问题**，
而是「记录器装上了 + 每帧 flush」这条路径带来的：

| 对照 | 记录器 | flush | 结果 |
|---|---|---|---|
| 无远程渲染 | 不装 | 不调 | ✅ 桌面，0 崩溃 |
| 按引用 + 原地写 | 装 | 调 | ❌ 6 分钟后 backboardd 开始 `exited due to SIGSEGV`，界面不出 |
| 按引用 + 不原地写 | 装 | 调 | ❌ 同样，1 次 SIGSEGV |
| 按值（内联） | 装 | 调 | ❌ 同样，2 次 SIGSEGV，0.74 fps |
| 按引用 + `glwire_mlock=0` | 装 | 调 | ❌ 同样 |
| 按引用 + 不启动宿主 | 装 | 调（连接失败） | ❌ 同样，并死在 `Ticket spinlock` panic |

「宿主内存压力」这一条仍然成立为**放大因素**（两次 panic 都发生在我并行编译的时候，见 0.6-2），
但它不是根因：安静环境下纯本地渲染跑 670 s 一点事没有。

### 2.2 每次测量前要做的事

1. `free -g`：`available` 要远大于 12 GB（`ram.img` 是 12.9 GB 的文件，客户机一换页就要读它）
2. 用 `qemu/wait-for-ui.sh` 启动，让它自己重试（它检测「两分钟没写日志」与「没有帧」两类卡死）
3. 出了两帧就停的启动**直接重开**，别把卡死前两帧的数字当稳态

### 2.3 下一步（根治方向）

不是「抓一次栈」，而是两步二分，见 §0.5：

1. 把「记录器安装」与「每帧 flush」分开（只装记录器、`eagl_remote_flush()` 直接 return 0）。
2. 抓到 backboardd 崩在哪：`usertrap:` 的打印已经有了，缺的只是「只对 backboardd 输出」的过滤器，
   或者一个 `EXTRA=` 的小程序把 `CrashReporter/*.ips` 打到串口。
   **不要用 `qemu/touch.py stacks`**：它走 debugger 入口，本 QEMU 配置不支持 halt → panic（§0.6-1）。

---

## 三、W1：记录器不再本地执行「重」调用（最大收益，但不能单独做）

> **顺序修正（2026-11-02 12:45，走查代码后的结论）**：宿主现在画出来的那一块「面板」，是
> **客户机本地画出来的**：CA 的 62 个调用经 `local_entry[]` 在客户机里真执行 → 画进 IOSurface →
> `eagl_remote_upload(0xFFFF0001, …)` 把 IOSurface 的内存按引用交给宿主 → 宿主把它画成一个全屏
> 四边形。
>
> 所以「跳过本地执行」如果单独做，本地那张 IOSurface 就没人画 → 上传给宿主的是空白/花屏。
> **W1 必须排在 §4.2 的实验（宿主能不能从 CA 的录制流自己出图）之后**：那个实验给了 MATCH，
> W1 才有意义；给不了，就先只做「跳过 `surface_to_texture` 的本地回填」这一小块。
>
> **§4.2 的实验已经做了（第三轮，W2 + `--no-panel`）：答案是 DIFF。** 客户机不上传面板之后，
> 宿主只重放 CA 的绘制流，屏幕是纯黑（截图 8,061 B）；同一次启动宿主一侧 19–30 fps、
> 记录 2.4–3.4 KB、`flush -> 2 (rows into the surface)` —— 管道很健康，是**内容**画不出来。
> 所以 W1+W2 现在不能一起开；W1 的代码保留并有开关（`GLWIRE_LOCAL=1` / `glwire_local=0`），
> 等宿主能自己出图再打开。

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
W0（测量可靠性：腾内存 + 重试）—— 每个阶段都要，不是一次性任务
        │
        ▼
W2a（宿主能不能只靠 CA 的录制流出图？3 行开关 + 一次比对）
        │
   ┌────┴─────┐
 MATCH       DIFF / 黑屏
   │            │
   ▼            ▼
W2b（客户机停止本地合成）     只做「跳过 surface_to_texture 的本地回填」
   │            │
   ▼            ▼
W1（不再本地执行重调用）      停在这里，先修宿主的重放（上传/FBO 顺序）
   │
   ▼
W3（直写显示用 IOSurface）
```

- W0 是每个测量前的例行动作，不是要交付的代码
- **W2a 是岔路口**，一次比对就能定；没定之前不要改 W1
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

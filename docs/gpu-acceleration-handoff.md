# IOSVM GPU 加速 —— 工程交接文档

> 交接时间：2026-11-02（第二轮）
> 接手目标：把界面渲染从「客户机软件光栅化」换成「宿主 V100 渲染」，目标 24 fps
> 当前状态：**管道已经修好，图象不再过线；帧率仍未达标，瓶颈已移到「客户机自己还在本地执行 GL」**

---

## 一、一句话现状

上一轮：一帧 10 MB 记录穿过 TCG，4.49 s/帧（0.22 fps），比本地渲染的 1.4 fps 还慢。

这一轮做完了「纹理按引用传递」：内核新增只读的系统调用 **556 `hakios_vtop`**（用户虚拟地址 → 物理
地址，按页表走、跨页分段），客户机记录里只带**物理地址与长度**；宿主直接在自己映射的 `qemu/ram.img`
里读同一块内存画帧，画好的画面也**原地写回**客户机缓冲区，不再回传。

实测（同一份镜像、同一次启动配置，fbscale=2）：

| 项目 | 上一轮 | 这一轮 |
|---|---|---|
| 一帧记录（750×1624） | 9,959,384 B | **13,651 B**（729 倍） |
| 一帧回传画面 | 4,872,000 B | **0**（原地写） |
| 客户机「把帧交给宿主」耗时 | ≈ 4.4 s | **99 µs** |
| 一帧记录（1125×2436） | 22,457,301 B | **29,077 B**（772 倍） |

**管道不再是瓶颈。24 fps 仍然没有达到，原因是另一个**：记录器在中转函数里**先调用本地实现、再记录**
（`remote.inc` 的 `local_entry[]`），所以 CoreAnimation 每一条 GL 调用仍然在客户机里用 Mesa softpipe
真执行一遍。一帧 64–135 条调用里，那张 4.98 MB 的纹理上传在 softpipe 里做 texstore 就要 > 1 s。

一帧 1.85 s 的实测分解（客户机自己的日志，见 §5）：交给宿主 99 µs、画面拷回 58 ms、IOSurface 纹理
回填 108 ms、**其余 1.69 s 是这一帧的 GL 调用在本地执行**。

> **先说清楚这批数字的来历**：按引用之后，4 次启动都只画到前两帧就卡死（屏幕上停在 Apple logo，
> 见 §7.8），所以上表来自**卡死前的第 1、2 帧**。管道指标（记录字节、回传字节、flush 耗时）是
> 精确的；帧间隔只有单帧的分量，不能当稳态帧率——接手第一件事是先把这个卡死查清。

---

## 二、关键数字（全部实测）

### 2.1 这一轮（2026-11-02）

| 项目 | 数值 | 测量方式 |
|---|---|---|
| 一帧记录，按引用（750×1624） | **13,651 B** | 宿主日志 `records B` |
| 一帧记录，按引用（1125×2436） | **29,077 B** | 同上 |
| 按引用传出的象素数 | 9,873,920 B / 22,450,176 B | 宿主日志 `2 image refs (N B)` |
| 回传画面 | **0 B，原地写** | 宿主日志 `in-place`；客户机 `picture copied out` 只拷本地缓冲 |
| 客户机交给宿主的耗时（flush） | **99 µs** | 客户机日志 `handing on` |
| 帧间隔（750×1624，第 2 帧） | 2.17 s（0.46 fps） | 宿主日志 `interval` |
| 帧间隔（1125×2436，第 2 帧） | 4.33 s（0.23 fps） | 同上 |
| vtop 查询（vtop_test） | 49,207 B / 2–3 段 | 内核串口日志 `vtop_test: seg=` |
| vtop 校验（vtop_verify.py） | 客户机 md5 = 宿主 md5 | `04dfd7c8188d1e36be996f9969ae0818` |
| 宿主画一帧 | 6–14 ms | `tools/host-glquad/gpu_host_ram.c`（未变） |

### 2.2 上一轮的基线（保留，用于比较）

| 项目 | 数值 | 测量方式 |
|---|---|---|
| 界面当前帧率（本地渲染） | 1.4 fps | 滚动设置列表 20 s，驱动日志 `frames in N ms` |
| 宿主 V100 画一整帧（含回读） | 1.9 ms | `tools/host-glquad/glquad.c` |
| 宿主原地渲染（内存直读直写） | 6–14 ms | `tools/host-glquad/gpu_host_ram.c` |
| 端到端每帧开销（按值） | 4.4–4.5 s | 宿主日志 `interval` |
| 每帧记录体积（按值） | 约 10 MB | 宿主日志 `records B` |
| 画面体积（750×1624 BGRA） | 4,872,000 B | — |
| 24 fps 预算 | 41.7 ms | — |

### 2.3 同分辨率 A/B（1125×2436，同一个启动配置，都是第 2 帧）

| | 记录 B | 回传画面 B | 帧间隔 |
|---|---|---|---|
| 按值（旧路径） | 22,298,323 | 10,962,000 | 8.00 s（0.12 fps） |
| 按引用（新路径） | 25,827 | 0（原地写） | 4.33 s（0.23 fps） |

---

## 三、架构与数据流

```
CoreAnimation (客户机 SpringBoard/backboardd)
      │  GLES 调用（62 个函数）
      ▼
EAGL 分发表（eagl.c 里 void *table[SLOTS]）
      │  这些槽被换成 remote_* 记录器
      ▼
记录器 remote_inc（生成代码）→ 二进制记录流
      │  u32 opcode + u32 length + payload
      │  纹理像素不再内联：payload 里是 kind=1 + 段数 + (pa, len) 列表
      ▼
eagl_remote.c：攒一帧 → TCP 发到 10.0.2.2:9110（QEMU user-net）
      │  请求行带上「画面缓冲区的物理段数」，段表随后，然后才是记录
      ▼
宿主 host_glwire（Linux）
      │  映射 qemu/ram.img（QEMU 以 share=on 打开，所以它就是客户机物理内存）
      │  按 pa 直接从 ram.img 读像素 → 重放到真实 GL 上下文（Tesla V100）
      │  glReadPixels → 按客户机给的段表 memcpy 回客户机的画面缓冲区
      ▼
客户机 eagl.c：把画面 memcpy 进 IOSurface → 驱动呈现
```

**双通道设计**：命令与段表走 TCP（KB 级），像素与画面走共享内存（`qemu/ram.img` + `hakios_vtop`
给出的物理地址）。

### 3.1 线格式（新增部分）

一段纹理指针（`wire.h` 的 `wire_image` / 宿主的 `wire_take_image`）：

```
u8  kind         0 = 后面跟着旧的「u32 长度 + 字节」
                 1 = 后面跟着 u32 段数 n，再是 n × { u64 pa; u32 len; u32 reserved }
```

`glTexImage2D` / `glTexSubImage2D` / 自定义的 opcode 201（eagl.c 上传 IOSurface）都走这一个形状。
客户机只有在**整块缓冲区都被翻译出来**（各段长度之和 == 字节数）时才用 kind=1，否则原样内联——
半个纹理比慢更糟。

画面回传（`R` 请求行与回复）：

```
-> R <w> <h> <records B> <picture B> <segments>     5 个字段；旧的是 4 个，宿主接受 4 个
   [segments × { u64 pa; u32 len; u32 reserved }]    画面缓冲区的物理段（可省）
   <records>
<- R 0            画面已经写进客户机内存（原地）
   R <bytes>      否则按老路把画面发回来
```

### 3.2 内核接口（syscall 556）

```c
int hakios_vtop(user_addr_t uva, user_size_t len, user_addr_t segments,
                uint32_t max_segments, user_addr_t count);
struct hakios_vtop_segment { uint64_t pa; uint64_t len; };   /* 16 字节 */
#define HAKIOS_VTOP_MAX_SEGMENTS 2048
```

只读、只答当前进程自己的映射；按 `pmap_find_phys()` 一页一页走，物理连续的页合并成段；只要有
一页没驻留（或被压缩/换出）就整块返回 `EFAULT` ——**不返回半个答案**，调用方只信任「全段覆盖」。
文件：`drivers/core/hakios.vtop.c`（实现在自研内核树里），`bsd/kern/syscalls.master` 第 556 行，
`bsd/sys/hakios.vtop.h`。

---

## 四、这一轮做完并验证的部分

### 4.1 内核侧

| 文件 | 作用 | 验证 |
|---|---|---|
| `drivers/core/hakios.vtop.c` | syscall 556 实现：页表走查、分段合并、整块覆盖否则 EFAULT | `vtop_test` 打印 2–3 段、49,207 B |
| `drivers/core/hakios.vtop.h`、`bsd/sys/hakios.vtop.h` | 段结构与 `HAKIOS_VTOP_MAX_SEGMENTS`（2048） | — |
| `drivers/core/conf` | 把 `hakios.vtop.c` 编进内核 | 内核二进制里有 `hakios_vtop` 符号 |
| `bsd/kern/syscalls.master` | 556 号系统调用 | 用户态 `syscall(556, …)` 返回 0 |

### 4.2 客户机侧

| 文件 | 作用 | 状态 |
|---|---|---|
| `userland/src/vtop_test.c` | 用户态小测试：填一块带图案的缓冲区，问内核物理地址，打印段表 | ✅ 每帧启动都跑，sleep 120 s 给宿主校验 |
| `userland/src/eagl_remote.c` | `remote_vtop()`（syscall 封装）、`remote_vtop_available()`（自检探针）、画面段表、`glwire_inline` 回退开关 | ✅ |
| `userland/src/wire.h` | `wire_image()`：能翻译就按引用，否则内联；`wire_segment` | ✅ |
| `userland/src/encode.inc`、`remote.inc`、`remote_table.inc` | 由 `spec.def` 重新生成（`x`/`y`/201 改走 `wire_image`） | ✅ |
| `userland/src/eagl.c` | 不变（帧末 flush / 画面回填的接入点沿用） | ✅ |

**回退开关**（两种都保留旧路径，出问题不用重编）：
- `/redwood/etc/glwire_inline` 存在 → 一律内联；
- 内核没有 556（旧内核）→ `remote_vtop_available()` 自检失败 → 自动内联。

### 4.3 宿主侧

| 文件 | 作用 | 验证 |
|---|---|---|
| `tools/glwire/host_glwire.c` | `map_ram()` 映射 ram.img；`wire_take_image` 把段拼成连续像素；画面原地写回；`MATCH/DIFF` 与 `in-place` 日志 | `image refs` 非 0、`in-place` |
| `tools/glwire/wire.h` | `wire_set_ram()`、`wire_take_image()`（越界或不可跟随 → 丢弃该调用，不画带洞的纹理） | — |
| `tools/glwire/names.h` | 面板纹理（name `0xFFFF0001`）任何分辨率都留作「客户机合成的整块画面」 | 750×1624 下也能 `MATCH` |
| `tools/glwire/vtop_verify.py` | 用串口日志里的段表去 ram.img 里取字节，与客户机的图案比 md5 | **PASS** |

参数：`host_glwire [port] [ram.img]`（或 `--ram <path>`）。不给 ram.img 时只跑旧的内联路径，
并在启动时说明「texture references will fall back to inline bytes」。

### 4.4 验收证据（真实日志）

**内核接口**（客户机串口 + 宿主校验）：

```
$ /redwood/bin/vtop_test &
vtop_test: buf=0x100268000 bytes=66313
vtop_test: va=0x10026807b len=49207
vtop_test: count=2
vtop_test: seg=0 pa=0x19cc9007b len=49029
vtop_test: seg=1 pa=0x19cc8c000 len=178
vtop_test: covered=49207
$ python3 tools/glwire/vtop_verify.py <串口日志> qemu/ram.img
vtop_verify: 2 segment(s), 49207 bytes, physical range 0x19cc9007b..0x19cc8c0b2
vtop_verify: guest md5 04dfd7c8188d1e36be996f9969ae0818
vtop_verify: host  md5 04dfd7c8188d1e36be996f9969ae0818
vtop_verify: PASS: the bytes at the physical addresses are the guest's buffer
```

（另一次启动拿到 3 段，md5 同样对上 —— 分段数随页表状态变，覆盖的字节数不变。）

**按引用、原地写、画面一致**（宿主日志，750×1624）：

```
host_glwire: qemu/ram.img mapped, 12884901888 bytes, physical base 0x40000000
host_glwire: frame 1, 750x1624, 16837 records B, 4872000 picture B, 0.09 ms, interval 0.00 s (0.00 fps), 2 image refs (9977856 B), panel de84f190b9e8f739 picture de84f190b9e8f739 MATCH in-place
host_glwire: frame 2, 750x1624, 13651 records B, 4872000 picture B, 0.05 ms, interval 2.17 s (0.46 fps), 2 image refs (9873920 B), panel de84f190b9e8f739 picture de84f190b9e8f739 MATCH in-place
```

`panel` 是客户机自己合成的整块画面（随记录传过来的那块纹理），`picture` 是宿主把它画一遍再读回来的
结果：两个 FNV 哈希都是 `de84f190b9e8f739`，**逐字节一致**（宿主的日志在算这两个哈希）。

**同机 A/B**（1125×2436，旧路径 → 新路径）：

```
host_glwire: frame 2, 1125x2436, 22298323 records B, 10962000 picture B, 0.05 ms, interval 8.00 s (0.12 fps), 0 image refs (0 B)
host_glwire: frame 2, 1125x2436, 25827 records B, 10962000 picture B, 0.06 ms, interval 4.33 s (0.23 fps), 2 image refs (22294272 B) in-place
```

---

## 五、现在的瓶颈（精确位置，实测分解）

客户机的 `hakios-gl: frame N:` 每帧一行，给出这一帧的去处（750×1624，第 2 帧）：

| 去处 | 耗时 | 说明 |
|---|---|---|
| 总 | **1,852,902 µs** | 一帧 |
| 交给宿主（flush） | **99 µs** | 记录 + 段表 + 等回复，管道已经是零成本 |
| 等宿主 | 4 µs | — |
| 画面拷回 IOSurface | 58,234 µs | 5 MB memcpy（宿主机写到我们的缓冲区，再拷进显示面） |
| IOSurface → 本地纹理回填 | 108,397 µs | `surface_to_texture()`，eagl.c 自己那条路 |
| **其余** | **1,686,168 µs** | 这一帧 64 条 GL 调用在本地 softpipe 里执行（含 CA 的 4.98 MB 纹理上传 texstore） |

结论：**管道修好之后，时间全部回到「客户机执行 GL」这一侧**。上一轮的「10 MB 过 TCG」已不存在
（13,651 B + 0 B），所以下一步不是再优化线路，而是**别让客户机画**。

---

## 六、下一步（按顺序，含预期收益）

> 这一节是概要；**详细的工程方案在 `/home/lshan/dsh.MD/IOSVM-GPU加速后续优化.md`**
> （仓库副本 `iosvm/docs/gpu-acceleration-plan.md`）：每一步的改动文件、伪代码、验证方法、
> 风险与回退，以及 W0 卡死隔离的具体实验和开关。

### 步骤 1：记录器不再执行「重」的本地调用（**最值钱的一步**）

现在 `remote.inc` 每个中转函数都是：

```c
if (local_entry[i] != NULL) local_entry[i](context, ...);   /* ← 真执行 */
gw_glXxx(...);                                              /* ← 记录 */
```

在远程模式下，`glTexImage2D`/`glTexSubImage2D`/`glDrawArrays`/`glDrawElements`/`glClear`/`glFinish`
的本地执行是纯浪费（宿主已经按引用读到同一块像素并画了）。保留本地执行的只有**会回答名字或状态**
的那几个：`glGenTextures`、`glGenFramebuffers`、`glCreateShader`、`glCreateProgram`、
`glGetUniformLocation`、`glGetString`、`glGetIntegerv`、`glGetError`、`glFenceSync`、
`glClientWaitSync`、`glDelete*`。

改动点：`tools/glwire/spec.def` 给每个调用加一个标记（例如 `glTexImage2D(...) [local=no]`），
`gen.py` 的 `emit_slots` 按标记生成 `if (!remote_only) local_entry[i](...)`，`eagl_remote.c` 里加
一个 `remote_only`（默认开，`glwire_inline` 或自检失败时关）。

预期：去掉 1.69 s 里的大部分（softpipe 的 texstore 与光栅化），到一个数量级以内。

### 步骤 2：客户机不再做本地合成

`eagl.c` 的 `frame_end()` 里仍然会 `target_to_surface()` / `show_texture()` /
`surface_to_texture()`（108 ms + 本地 draw）。远程模式下这些也都是浪费：宿主拿到的已经是
`0xFFFF0001` 那块整面板画面。

风险：宿主目前**只会**画这块上传的面板（CA 的绘制流重放并不足以画出界面，见 §7.2）。所以这一步
要么保持「客户机合成 → 上传面板」，要么把 host 的重放做成能独立出图。**先做步骤 1，再决定。**

### 步骤 3：画面直接写进显示用 IOSurface（省 58 ms）

`eagl.c` 知道 IOSurface 的基址与行跨度；再调一次 556 就有它的物理段，宿主可以 `glReadPixels`
直接写进去，客户机连那次 memcpy 都不用。

### 步骤 4（小项）

- `HAKIOS_VTOP_MAX_SEGMENTS` 目前 2048；1125×2436 的 11.07 MB 上传在 16 KB 页下是 692 页，
  余量够，但更宽的页表碎片会逼近上限——超了会**静默回退内联**（见 §7.1）。
- `tools/glwire/gen.py` 的 `x`/`y`/201 三处已经统一走 `wire_image`，新增 opcode 时照抄。
- 宿主日志已经有 `panel … picture … MATCH/DIFF` 与 `image refs`；做 A/B 时先看这两个字段。

### 不要重试的（有数据）

- **去重**：CA 每帧整屏重合成，像素确实在变（9.96 MB 一丝没少）。
- **客户机内启用 GL 加速渲染器**：Mesa softpipe 在 TCG 里 7 分钟出不了第一帧。
- **llvmpipe**：需要 arm64-iOS 的 libLLVM，本机只有 x86_64，且 Mesa 不在 CA 合成链上。
- **只降分辨率**：fbscale 3→1 只换来 2 倍帧率，瓶颈是模拟执行不是像素。

---

## 七、坑（本轮新增 6 条，**不要重走**）

### 7.1 段数上限会「静默回退」，看上去像功能没生效
16 KB 页下 11.07 MB 的上传 = 692 页；第一版上限 512，内核返回 `ENOMEM`，`wire_image` 老老实实
退回内联——日志上表现成 `0 image refs (0 B)` + 22.46 MB 记录，代码看起来完全正确。上限现在 2048
（内核与 `wire.h` 的数组必须同时改）。**症状：refs 为 0 但记录仍是 MB 级。**

### 7.2 镜像里的 dylib 是旧的，行为就像新代码没写
`libhakios.gl.dylib` 在 `userland/root.apfs` 里；改完 `eagl_remote.c`/`wire.h` 必须重跑
`userland/build.sh root.apfs`（约 1–2 分钟）。`run-darwin.sh` 每次启动都会把内核 + root.apfs
重新拼成 `qemu/payload.bin`，所以只有 root.apfs 需要你自己重建。**11:39 那台就是旧镜像，
量到 22.46 MB / 0 refs；同一份源码在 11:43 那台量到 29 KB。**

### 7.3 面板纹理的名字
宿主只把 **`0xFFFF0001`** 那次上传当作「客户机合成的整块画面」（`names.h`）。上一版写死
`1125×2436`，于是 fbscale=2 的启动永远没有面板，宿主只好重放整条 CA 流（画不出界面）。

### 7.4 vvfat 的临时文件与文件沙箱
`-drive file=fat:rw:esp` 要写 `/var/tmp/vl.XXXX`；沙箱不允许时每次启动 0.2 秒就死：
`Could not open temporary file '/var/tmp/vl.T996X3': Permission denied`。绕法：先做一个 64 MB 的
raw FAT 镜像（`qemu/esp.img`），启动时 `QEMU_ESP_IMAGE=$PWD/qemu/esp.img`。

### 7.5 `check_all_exports` 失败是既有的噪声，不是你的改动
`xnu-build.sh` 结尾一定会出现 `exported name not in import list: …`（约 220 行）和
`config_all … 错误 2`。内核二进制 `hakios-kernel` 在此之前就已经生成，`payload.bin` 用的就是它；
11 月 1 日的构建日志里同样有 220 行。**判断成功看 `ls -l hakios-kernel` 与
`strings hakios-kernel | grep hakios_vtop`。**

### 7.6 `pkill -f` 会杀掉自己的 shell（再次验证）
`pkill -f "host_glwire 9110"` 连自己的 bash 命令行一起匹配，把 shell 杀了。只按进程名：
`ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'`。

### 7.7 其余（第一轮就踩过的，仍然有效）
- **内部 GL 必须绕过 EAGL 表**：`eagl_direct.h` 的 49 个直达指针不能删，否则本地没有纹理。
- **`share=off` 是死路**：`QEMU_RAM_SHARE=on` 否则宿主读 ram.img 看不到实时数据。
- **工具参数顺序**：`host_glwire <端口> <ram.img>`（现在两种都认，但别指望默认值）。
- **启动 6–9 分钟，约 40% 概率卡在 lsd 上**（`lsd denied attempt to escape CPU monitor` 刷屏、
  面板空白）；`qemu/wait-for-ui.sh` 会自动重试，卡住就重起。

### 7.8 卡死：前两帧之后不再出帧（**已查清一半：与本轮改动无关**）

现象：客户机起来后画两帧（`hakios-gl: frame 1/2:` 正常，`flush -> 1`、宿主 `MATCH in-place`），
之后不再出帧，屏幕停在 Apple 开机 logo，串口被
`process lsd[32] denied attempt to escape CPU monitor` 刷屏。12:33 与 12:38 两次都复现。

**已经排除的（实测，别再重做）：**

| 假设 | 实验 | 结果 |
|---|---|---|
| 画面缓冲区被压缩/搬移，宿主写进旧页 | `eagl_remote.c` 里 `mlock()` 把画面缓冲钉住 | `picture buffer 4872000 B + 8, mlock 0 (wired)`，**照样卡** |
| 按引用这条新路本身 | `build.sh` 加 `GLWIRE_INLINE=1` 重建镜像（客户机走旧的内联线） | 内联对照同样**只画两帧就停**（`records 9,959,387 B, 0 image refs`） |
| 死在某次 vtop 调用里 | 卡住后串口不再有 SpringBoard 的输出（无悬空的 `end of frame`） | SpringBoard 不是卡在 flush/recv，是停在整个绘图循环上 |

**结论**：这是本端口既有的卡死（`docs/state-of-the-port.md` 记的「约 40% 的启动会卡」，症状是
lsd 刷屏、面板空白、约两分钟处停），不是按引用引入的。今天命中率异常高（7 次连续），而彼时宿主机
有 5 GB 已换出、还跑着一台 24 GB 的 Windows 虚拟机——**先怀疑宿主内存压力**：客户机的换页走的是
`qemu/ram.img`，宿主一慢，客户机的定时器就跟着错乱，launchd 的 CPU 监视器就报 lsd。

**测量前的建议**：

1. 跑帧率测量前先腾出宿主内存（停掉别的虚拟机/大进程），`free -g` 里 `available` 要远大于 12 GB；
2. 用 `qemu/wait-for-ui.sh` 启动并让它重试（它会检测「两分钟没写日志」和「没有帧」两类卡死）；
3. 连续两帧之后停住就重开，不要拿卡死前两帧的数字当稳态（本文所有按引用数字都属于这一类，
   只有「管道字节数」是精确的）。

**还没做的**（如果想根治）：卡住时抓客户机进程栈（`docs/research/linux-kernel-build.md` 的
stackshot 附录、`qemu/symstack.py`），看 SpringBoard 是死在 CA 的渲染线程里还是别处。

---

---

## 八、环境与操作手册

### 8.1 目录
- 虚拟机工程（内核 + userland）：`/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel`
- 对外仓库（文档 + 工具）：`/home/lshan/DSH-desktop/iosvm`（github.com/lshan99q/iosvm）

### 8.2 交叉编译工具链
```bash
export THEOS=$HOME/theos IOSVM_USE_LLD=1
export PATH=<repo>/xnu-deps/host/bin:/tmp/cctools/bin:$PATH
```

### 8.3 重建内核（加了 syscall 才需要）
```bash
cd /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
bash xnu-build.sh 2>&1 | tee /tmp/kernel-build.log   # 结尾的 export 报错是既有噪声
ls -l hakios-kernel && strings hakios-kernel | grep -c hakios_vtop   # 应为 4
```

### 8.4 重建小镜像（改 userland 后，约 1–2 分钟）
```bash
cd userland
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs
```
- `GL=1`：`libhakios.gl.dylib`（EAGL 实现，本轮的记录器都在里面）；
- `GLREMOTE=1`：生成 `/redwood/bin/glremote_on`（远程渲染的开关文件）；
- `RC=rc.vtop`：`etc/rc` 先跑 `/redwood/bin/vtop_test`（打印 `VTOP-BEGIN…VTOP-END` 后 sleep 120 s，
  留给 `vtop_verify.py` 校验），再配网，最后交给 launchd；
- `CONFIGD=1`：带上 configd/mDNSResponder，QEMU 的 user-net 才拿得到地址。
- `vtop_test` 由 `build.sh` 自己编（`src/vtop_test.c` → `obj/vtop_test` → `root/bin/`），不用 `EXTRA`。

**改完 `eagl_remote.c` / `wire.h` 必须重跑这一步**：镜像里那份 dylib 不重建，跑的还是旧代码（§7.2）。

### 8.5 启动（**必须 `metal=1`**；`fbscale=2` 是 750×1624）
```bash
cd /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
KEEP=1 QEMU_NET=1 TRIES=3 MAX_WAIT=1800 NO_FRAMES_BY=200 \
  QEMU_ESP_IMAGE=$PWD/qemu/esp.img \
  CMDLINE_EXTRA="fbscale=2 metal=1" \
  bash qemu/wait-for-ui.sh /tmp/gpu.png
```
`KEEP=1` 保留虚拟机给后面的测量用；去掉它就拍张图然后关机。

### 8.6 启动宿主重放器（**必须在 QEMU 起来之后**：ram.img 会被重新拼）
```bash
cd /home/lshan/DSH-desktop/iosvm/tools/glwire
make
./host_glwire 9110 /home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel/qemu/ram.img
```
启动后应看到两行：`… ram.img mapped, 12884901888 bytes, physical base 0x40000000` 与
`listening on 9110`。

### 8.7 看日志
```bash
# 客户机（wait-for-ui.sh 用的是 mktemp，取最新的）
log=$(ls -t /tmp/tmp.* | head -1)
grep -a "hakios-remote:" $log          # 帧末 flush
grep -a "hakios-gl: frame " $log       # 每帧的去处分解（handing on / waiting / copying out）
grep -a "frames in " $log              # 驱动帧率

# 宿主
grep -a "host_glwire: frame" /tmp/glwire*.log    # records B / refs / in-place / MATCH
```

### 8.8 验收 vtop（内核接口本身）
```bash
python3 tools/glwire/vtop_verify.py $(ls -t /tmp/tmp.* | head -1) qemu/ram.img
```

### 8.9 截图
```bash
bash qemu/screendump.sh /tmp/shot.png
```
注意：ramfb 的分辨率固定是面板的 1125×2436，与 `fbscale` 无关；`fbscale=2` 只让客户机的
渲染面变小。

### 8.10 重要注意事项
- **启动约 6–9 分钟**，约 40% 概率卡死（lsd 刷屏、无帧），杀掉 qemu 重试即可。
- 杀进程只按进程名：`ps -eo pid,comm | awk '$2 ~ /^qemu-system-aar/ {print $1}'`；
  **绝对不要** `pkill -f`（会杀掉自己的 shell）。
- `payload.bin` 与 `ram.img` 是共享的：**同一时间只能有一台虚拟机在跑**。
- 宿主重放器也要独占 9110；旧的实例没退出时，新的会报 `cannot listen`。

---

## 九、配置开关速查

| 开关 | 作用 |
|---|---|
| `metal=1` | 让 CoreAnimation 使用 GL 路径（**必需**） |
| `fbscale=2` | 渲染分辨率 750×1624（3 是默认 1125×2436，1 是 375×812） |
| `/redwood/bin/glremote_on`（文件存在即开） | 启用远程渲染 |
| `/redwood/etc/glwire_inline`（文件存在即回退） | 强制走内联（旧）路径，出问题时的开关 |
| 内核 syscall 556 | 按引用传递的前提；没有它时自动回退内联 |
| `QEMU_RAM_SHARE=on` | 默认已开，宿主才能读客户机内存 |
| `QEMU_ESP_IMAGE=<img>` | 用 raw FAT 镜像代替 vvfat（沙箱禁用 `/var/tmp` 时必需） |
| `GL=1`（构建时） | 把 `libhakios.gl.dylib` + `libOSMesa` 放进镜像 |
| `GLREMOTE=1`（构建时） | 放进 `/redwood/bin/glremote_on` |

---

## 十、验收标准（本轮达成情况）

| # | 标准 | 结果 |
|---|---|---|
| 1 | 客户机日志每帧 `hakios-remote: flush -> 1` | ✅ |
| 2 | 宿主 `records B` < 100 KB | ✅（13,651 B / 29,077 B） |
| 3 | 画面不再回传（`in-place`） | ✅（4,872,000 B → 0） |
| 4 | 宿主 `interval` < 60 ms | ❌（2.17 s；管道只占 99 µs，其余是客户机本地执行） |
| 5 | 帧率 ≥ 20 fps | ❌（0.46 fps @750×1624；本地渲染基线是 1.4 fps，因为客户机还在双份工作） |
| 6 | 画面与客户机自绘一致 | ✅（`panel … picture … MATCH`，`de84f190b9e8f739`；vtop 的 md5 也已对上） |
| 7 | 内核接口可被用户态调用并验证 | ✅（`vtop_test` + `vtop_verify.py` PASS） |
| 8 | 界面真的出现在面板上 | ⚠️ **未验证**：本轮 4 次按引用的启动都只画到前两帧（logo 阶段）就卡住，见 §7.8 |

---

## 十一、相关文档索引

| 文档 | 内容 |
|---|---|
| `iosvm/tools/glwire/README.md` | GL 线协议、生成器、宿主重放、瓶颈分析（需要按本轮更新） |
| `iosvm/tools/glwire/vtop_verify.py` | 用串口日志里的段表去 `ram.img` 取字节、与客户机图案比 md5（内核接口的验收） |
| `iosvm/patches/0009-hakios-vtop-syscall.patch` | 内核侧的全部改动（syscall 556、驱动、`conf`、`syscalls.master`） |
| `iosvm/tools/host-glquad/README.md` | 宿主渲染各阶段的实测数据 |
| `iosvm/docs/research/linux-kernel-build.md` 附录 33–35a | 渲染瓶颈的完整调查（含被否掉的路线） |
| `iosvm/docs/research/linux-kernel-build.md` 附录 36–36a | 外网修复、双代理共用一棵树的 ROOT 规则 |
| `iosvm/docs/state-of-the-port.md` | 端口总体状态（含 `cpus=4`、`fb_beat=0` 两个必需启动参数） |
| `/home/lshan/dsh.MD/IOSVM.md` | 项目总体结论 |

---

## 十二、诚实的状态声明

- **已完成**：内核 syscall 556（并已用 `vtop_test` + `vtop_verify.py` 从宿主侧逐字节验证）；
  纹理按引用传递；画面原地写回；旧路径双向回退；按引用后一帧记录 13.6 KB、画面零回传、
  交给宿主 99 µs；全部数字均有日志。
- **未完成**：**帧率没有到 24 fps**。750×1624 下 0.46 fps，比本地渲染的 1.4 fps 还低——
  因为客户机现在**既在本地执行全部 GL、又把同样的调用交给宿主**，等于双份工作；管道这一份已经
  快得不计，本地那一份就是全部。
- **未完成（更要紧）**：按引用这条路的**长跑没通过**。4 次启动都只在前两帧之后卡死（§7.8），
  所以「界面真的显示出来」一次都还没验证。管道指标（13 KB、零回传、99 µs）来自卡死前的两帧，
  它们本身是准的；但一切关于 fps 的说法都只有单帧样本的分量。
- **不能声称**：不能说「GPU 加速已经完成」，也不能说「界面现在更快了」。这一轮完成的是
  「线路与传送方式」，把最后一个未知（客户机能不能给出物理地址、宿主能不能读同一块内存）变成了
  已测的已知；让界面真正变快还差 §6 的步骤 1（记录器不再本地执行）。
- **下一步的预期要有保留**：步骤 1 能去掉 softpipe 的 texstore/光栅化，但 CA 自己的 CPU 代码仍在
  TCG 下跑，余下多少只能实测。按本轮分解，理论下限是「画面拷回 58 ms + IOSurface 回填 108 ms
  ≈ 0.17 s/帧（6 fps）」，再往上要靠步骤 2/3。

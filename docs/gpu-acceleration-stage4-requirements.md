# IOSVM GPU 加速 —— 第四阶段需求文档

> 建立时间：2026-11-02（第三轮之后）
> 前置阅读：`IOSVM-GPU加速交接.md`（架构/坑）、`IOSVM-GPU加速后续优化.md` §零（第三轮执行记录与全部实测数字）
> 本文只回答：**下一阶段要做什么、每件事的判据是什么、怎么验、不做什么**

---

## 一、一句话现状

管道已经不再是瓶颈（一帧 8.4 KB 记录、宿主 0.04 ms、画面原地写、137 帧逐字节 MATCH、单帧 1.85 s → 77 ms），
但**远程渲染这条路一启用，启动就停在 logo/转圈**：`backboardd` 反复 `exited due to SIGSEGV`；
去掉 `GLREMOTE` 的同一份镜像能正常到桌面并在 7–9 fps 运行。
W1/W2/W3 的代码都已落地并在开关后面（`!local` / `remote_only` / `composition_off` / 请求行行跨度）。

**所以第四阶段的第一需求不是提速，是让这条路能稳定启动，并让宿主的输出被称为「界面」。**

---

## 二、目标与验收（按优先级，全部可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| R1 | 定位并修掉「记录器装了就崩」 | 冷启动 5 次，≥4 次连续跑 ≥200 帧且截图 > 300 KB；0 次 `SIGSEGV`/panic | 全部 |
| R2 | 宿主能只靠 CA 录制流画出界面 | `--no-panel` 下 `panel … picture … MATCH` 连续 ≥100 帧 | W2/W1 |
| R3 | W1+W2 端到端可用 | ≥200 帧、桌面截图 > 300 KB、帧间隔 < 200 ms（≥5 fps）、记录 < 100 KB、回退开关有效 | 提速 |
| R4 | W3 行写验收 | 日志 `in-place rows`、画面与客户机自绘逐字节一致、客户机 `copying out` ≈ 0、一帧 < 125 ms（≥8 fps） | 提速 |
| R5 | 追上纯本地基线 | 滚动设置列表 20 s 的驱动稳态帧率 ≥ 9 fps | 结论 |

> R5 是这一阶段的**诚实目标**：不承诺 24 fps（`IOSVM-GPU加速后续优化.md` §十一 已说明为什么）。
> 能不能超过 9 fps，取决于 R1 之后量出来的「CA 自身 CPU 成本」。

---

## 三、需求详述

### R1 定位「记录器装了就崩」（阻塞项）

现状证据（第三轮，同一镜像）：

| 配置 | 结果 |
|---|---|
| 不装记录器（无 `GLREMOTE`） | ✅ 桌面，7–9 fps，900+ 帧，0 崩溃 |
| 装记录器 + 每帧 flush（按值/按引用、原地写/不原地写、mlock/不 mlock） | ❌ 停在 logo→转圈，`backboardd` 反复 SIGSEGV，有时 `Ticket spinlock timeout` panic |

#### R1.1 把「装记录器」与「flush」分开（半天，一个开关一次启动）

- 需求：加一个启动参数 `glwire_nosend=1`，让 `eagl_remote_flush()` 在记录完一帧后 `return 0`（`wire_reset` 照旧），
  一帧都不发、不回填画面。
- 判据：
  - **不崩且进桌面** → 崩在 flush / 画面回填那一侧，转 R1.2 的「只开回填」对照；
  - **照样崩** → 崩在「62 个 EAGL 槽位被替换」这件事本身，转 R1.3。
- 涉及：`tools/glwire/eagl_remote.c`（新开关，走 `remote_boot_arg()`）；镜像重建一条命令（见 §五）。

#### R1.2 只开画面回填（与 R1.1 互补，一个开关）

- 需求：`glwire_norecord=1`：记录器的槽位不装（或装了但 `gw_*` 不写），只保留 flush 与画面回填。
- 判据：分开两边哪一侧单独就崩。

#### R1.3 拿到崩溃地址与回溯（R1.1/R1.2 之后必须做）

- 需求：让 `usertrap:` 的输出只针对 `backboardd`（`osfmk/arm64/user_exception_trace.c` 已有格式与
  `plausible_user_address()`，缺的只是一只进程名过滤器）；或加一个 `EXTRA=` 小程序把
  `/private/var/mobile/Library/Logs/CrashReporter/*.ips` 打到串口。
- 判据：拿到 `pc`/`lr` + ≥8 层回溯，并能回答「崩在共享缓存里的哪一帧 / Mesa / libhakios.gl / 匿名 JIT」。
- **禁止**：用 `qemu/touch.py stacks`（本 QEMU 配置不支持 debugger halt，会直接 panic，见优化文档 §0.6-1）。

#### R1.4 修（依赖 R1.3 结论）

- 判据（R1 总判据）：冷启动 5 次 ≥4 次 ≥200 帧 + 桌面截图 > 300 KB + 宿主日志连续 200 帧 `MATCH`。
- 回退：`/redwood/etc/glwire_inline` 仍能把一切退回旧的内联路径（保留）。

### R2 宿主能自己出图（W2 的前置）

现状：`--no-panel`（不做面板上传）时屏幕纯黑（截图 8,061 B）；宿主一侧 19–30 fps、记录 2.4–3.4 KB，
说明**管道健康、内容画不出**。

- 需求：
  1. 先量化黑屏：宿主 `GLWIRE_DUMP=前缀` 存下它的 picture，判断是「纯黑」还是「缺层/错位/花屏」。
  2. 按判断修：纹理上传与 FBO 的顺序、`glGenTextures` 覆盖已上传纹理（`names_*_put_new`）、
     缺失的 GL 状态、shader 编译/链接失败、`names.h` 映射漏项——每修一条都要有「哪一帧哪一层」的证据。
- 判据：`--no-panel` 下 `panel … picture … MATCH` 连续 ≥100 帧（面板只用于哈希，不参与绘制）。
- 不做：任何形式的后处理/糊图凑哈希；不做「把客户机的画面当输入再画一遍」（那是自证）。

### R3 W1+W2 端到端（依赖 R1、R2）

- 需求：`remote_only` 默认打开（`GLWIRE_LOCAL=1` / `glwire_local=0` 保留为关）。
- 判据：
  - ≥200 帧、桌面截图 > 300 KB（人机可用界面）
  - 帧间隔 < 200 ms（≥5 fps），滚动设置列表 20 s 的稳态值
  - 记录 < 100 KB、`in-place` 仍在
  - A/B 有效：同一次启动内 `glwire_local` 两侧各跑 30 帧对比
- 风险（已知）：W1 关掉本地绘制后，任何「CA 依赖本地 GL 结果」的路径都会露出来；
  `glBindTexture` 必须本地执行（否则 `-attachImage:` 拿 `texture 0`，`images=0`，宿主收不到帧——第三轮踩过）。

### R4 W3 行写验收

- 需求：请求行第 6 字段（行跨度）+ 宿主按行写 + 客户机不再 memcpy（**代码已完成**，差验收）。
- 判据：
  - 宿主日志 `in-place rows`
  - 画面与客户机自绘逐字节一致（`panel … picture … MATCH`，面板来自客户机自身合成时）
  - 客户机 `hakios-gl: frame N` 的 `copying out` ≈ 0（现在 48,681 µs）
  - 一帧 < 125 ms（≥8 fps）
- 回退：`row_bytes = 0` 自动回到打包缓冲（现成），段表覆盖不全也自动回退。

### R5 基线对照与结论

- 需求：一张对照表（纯本地 / 按引用+面板 / W1+W2 / W1+W2+W3），每格都是「滚动设置列表 20 s 的驱动稳态帧率」。
- 判据：**纯本地基线必须同机同镜像重测**（第三轮实测 7–9 fps，交接文档旧的 1.4 fps 是重负载数字，不能再用）。

### R6 回归与可维护性（贯穿）

1. `map_test`、`e2e_test.py` 必过（宿主端协议改动后）。
2. 宿主接受 4 / 5 / 6 字段的请求行（向后兼容，旧客户端不炸）。
3. 每阶段一次提交，提交信息带前后数字；文档同步到 `/home/lshan/dsh.MD/` 与 `iosvm/docs/`。
4. 开关清单（不许再加新的隐形开关）：`GLWIRE_INLINE`、`GLWIRE_LOCAL`、`glwire_local`、`glwire_mlock`、
   `glwire_nopicture`、`glwire_nosend`(新)、`glwire_norecord`(新)、`GLWIRE_NO_INPLACE`、`--no-panel`。

---

## 四、不做（已被数据否掉，别重试）

- 像素去重（CA 每帧整屏重合成）
- 客户机内启用 GL 加速渲染器 / llvmpipe（缺 arm64-iOS 的 libLLVM，且不在 CA 合成链上）
- 只降分辨率（fbscale 3→1 只换来 2 倍，瓶颈是模拟执行）
- 承诺 24 fps
- 用「客户机先画、宿主再抄」的方式冒充 W1/W2 的提速（第三轮已证明那是双份工作）

---

## 五、验证环境与操作规范（照这个做，别自创）

### 5.1 构建（约 25 s）

```bash
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
export THEOS=$HOME/theos IOSVM_USE_LLD=1
export PATH=$IOSVM/xnu-deps/host/bin:/tmp/cctools/bin:$PATH
cd $IOSVM/userland
# 按引用，客户机不再画（W1+W2，默认）
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs
# 关掉 W1，保持旧的本地绘制（A/B 用）
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop GLWIRE_LOCAL=1 ./build.sh root.apfs
# 纯净基线（完全不装记录器，必须能到桌面）
GL=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs
```

### 5.2 启动（**一次只跑一台，别同时编译**）

```bash
cd $IOSVM
QEMU_SMP=4 QEMU_TB_MB=1536 QEMU_RAM_GB=12 QEMU_NET=1 QEMU_ESP_IMAGE=$PWD/qemu/esp.img \
  CMDLINE="cpus=4 fb_beat=0 fbscale=2 metal=1" \
  timeout 1200 bash qemu/run-darwin.sh 1000 > /tmp/<label>-guest.log 2>&1 &
# 等 QEMU 把 ram.img 打开之后再起宿主（见 §5.3）
```

### 5.3 宿主

```bash
cd /home/lshan/DSH-desktop/iosvm/tools/glwire && make
./host_glwire 9110 $IOSVM/qemu/ram.img                 # 正常
./host_glwire --no-panel 9110 $IOSVM/qemu/ram.img      # R2：不画面板
GLWIRE_NO_INPLACE=1 ./host_glwire 9110 $IOSVM/qemu/ram.img
GLWIRE_DUMP=/tmp/frame ./host_glwire 9110 $IOSVM/qemu/ram.img   # 存首帧画面
```

### 5.4 判读

```bash
grep -a "hakios-gl: frame " /tmp/<label>-guest.log | tail -5   # 客户机每帧去处
grep -a "frames in "       /tmp/<label>-guest.log | tail -3   # 驱动稳态帧率
grep -a "^host_glwire: frame" /tmp/<label>-host.log | tail -5  # records/refs/MATCH/in-place rows/interval
grep -a "SIGSEGV\|panic("  /tmp/<label>-guest.log | tail -5    # 崩溃
qemu/screendump.sh /tmp/<label>.png                            # <300 KB = 没界面
```

### 5.5 硬性禁忌

1. **不要在测量启动时并行编译或跑第二台虚拟机**（两次内核 panic 都发生在并行任务期间）。
2. 不要用 `qemu/touch.py stacks`（会 panic）；要栈用 `ustacks=NAME ustacks_at=…ustacks_ms=…ustacks_count=…`。
3. 不要 `pkill -f`（会杀掉自己的 shell）；按进程名杀。
4. `payload.bin` 与 `ram.img` 是共享的，同一时间只能有一台虚拟机。

---

## 六、里程碑

| 里程碑 | 内容 | 判据 |
|---|---|---|
| M4.1 | R1.1/R1.2 两侧对照跑完 | 一句话说清崩在哪一侧，附日志 |
| M4.2 | R1.3 栈 + R1.4 修 | 冷启动 5 次 ≥4 次 ≥200 帧 + 桌面截图 |
| M4.3 | R2 宿主独立出图 | `--no-panel` 连续 100 帧 MATCH |
| M4.4 | R3 W1+W2 打开 | ≥5 fps、截图 > 300 KB |
| M4.5 | R4 W3 行写验收 | `in-place rows`、`copying out` ≈ 0、≥8 fps |
| M4.6 | R5 对照表 + 文档 | 四个配置一张表，数字同机同镜像 |

每个里程碑一次提交（`iosvm` 仓库），提交信息带前后数字；文档同步 `/home/lshan/dsh.MD/` 与 `iosvm/docs/`。

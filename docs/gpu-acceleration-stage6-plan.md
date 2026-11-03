<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第六阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第六阶段工程文档

> 建立时间：2026-11-03（第五阶段收尾之后）
> 前置阅读：`IOSVM-GPU加速第五阶段.md`（这一阶段实测的数字与结论）、
> `IOSVM-GPU加速第五阶段工程.md`（S1–S6 的目标）、`IOSVM-GPU加速交接.md`（架构/坑）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 一、一句话现状

第五阶段把两件事钉死了：

1. **宿主独立出图成立**：停掉 `0xFFFF0001` 的面板上传，`--no-panel` 仍然重放出一张
   38.1% 非黑、10,937 色的桌面（369 帧、448 KB 截图、0 panic）；那次启动里宿主一次也没收到面板。
2. **W1.5 跑起来了**：`remote_only` 下宿主拿到层纹理（图标纹理 129,600 B 一张、texture 7 4.88 MB），
   708/1,090/1,481 帧、宿主 10–66 fps、记录 952 B–93,388 B（全部 < 100 KB）、
   滚动窗口的驱动稳态 **6.74 fps**。

但同一轮量出两件新事实，它们就是这一阶段的主线：

1. **记录流覆盖不到 CA 的软件路径**：每帧只有 9 个 draw、36 条顶点数组；图标下面的文字、
   Dock 的底、搜索框从来不在记录里。于是 `remote_only` 的屏幕只有宿主的画面，245 KB、缺字。
2. **启动本身只有约 22% 的通过率**：18 次冷启动 4 次到界面，其余死在 5–6 分钟的
   `Ticket spinlock[…] timeout` 上（同一个全局锁 `0xfffffff007b68880` 出现最多）。
   带触摸 1/6、不带触摸 2/11——**触摸不是原因**，任何「5 次 ≥4 次」的判据在任何配置下都过不了。

所以第六阶段的第一件事不是提速，是**把「一次启动能活着到界面」变成常态**，
再把「宿主画出来的画面缺了什么」补上（或者明确不补）。

---

## 二、目标与验收（按优先级，全部可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| T0 | 启动稳定性 | 同一镜像 10 次冷启动 ≥8 次到界面、**0 panic**；每次 panic 前有一条栈或一个锁地址归因 | 所有测量 |
| T1 | W1.5 的画面完整度（记录器的覆盖） | `remote_only` 的截图 > 400 KB，且存图里有图标文字、Dock、搜索框；记录仍 < 100 KB；≥5 fps | W2 的结论 |
| T2 | 一帧相位的来源 | 连续 ≥100 帧同帧 `panel(N) == picture(N)`；做不到就给出相位的证据并把验收改成 `picture(N) vs panel(N+1)` 连续 ≥100 帧 | R2 判据 |
| T3 | 四配置滚动稳态（第五阶段没补齐的表） | 同机同镜像，四个配置各一次成功的 20 s 窗口，每格一个数字 | 结论 |
| T4 | CA 自身的 CPU 成本 | 一帧的客户机侧时间拆开（CA 记录/上传 vs 宿主重放），给出 TCG 下的毫秒数 | 24 fps 的任何说法 |

> T0 是一票否决：它不过，T1–T4 的数字都只是「哪次运气好」。
> T1 是这一阶段最大的架构选择（见 §三 T1 的三条路），要在动代码之前先选定。

---

## 三、需求详述

### T0 启动稳定性（先做，1–2 天）

**现状证据**（第五阶段 §七，22 次冷启动）：

| 配置 | 冷启动 | 到界面 | panic |
|---|---|---|---|
| `rc.vtop`（不带触摸） | 11 | 2 | 9 |
| `rc.touch1`（一个 rwtouch 进程，600 s） | 8 | 1 | 7 |
| `rc.touch1` 纯本地（不装记录器） | 3 | 0 | 3 |

panic 的形状一致：

```
panic(cpu 1 caller 0xfffffff007aefad0): Ticket spinlock[0xfffffff007b68880] timeout after 60001808 ticks;
    cticket: 0x79, nticket: 0x7a, waiting for 0x76, current owner: 0 (on CPU -1), orig owner: 0, ... @lock_ticket.c:386
```

**这把锁已经定位了**（2026-11-03 15:20，离线符号化 + 结构体布局）：

- `llvm-nm -n hakios-kernel` 给出 `_pset0` 在 `0xfffffff007b68780`，panic 里的
  `0xfffffff007b68880` 正好是 `_pset0 + 0x100`；
- `pset0` 是结构体（不是一个指针）：`xnu-src/xnu-8796.121.2/osfmk/kern/processor.c:106`
  `struct processor_set pset0;`；
- `struct processor_set` 里 `__attribute__((aligned(128))) lck_ticket_t sched_lock` 前面的字段
  （6 个 int + 负载平均数组 + 6 个 `cpumap_t`）合计 0xD0，**128 对齐后正好落在 +0x100**；
- 所以 panic 等的是 **`pset0.sched_lock`，进程集 0 的全局调度票据锁**。

不是驱动锁、不是 GL、不是触摸——是**调度器**。

`current owner` 是 0 或 CPU -1 时看起来像「持有者不在了」，但同一把锁的另外几次是
`current owner: 0xfffffff8efec64e8 (on CPU 3)`——持有者活着。两种都出现过，说明不是一种固定死法。

**超时是 1 秒，不是 60 秒**：`timeout: 60000000` 是 `mach_absolute_time` 的 tick 数。
用 panic 块自己的两个时标换算（`Boot: 0x6ae95bc8` / `Calendar: 0x6ae95d53` → 395 s，
`mach_absolute_time: 0x5e3028e2a` → 25,266,103,338）得到约 **62.5 MHz**，
`60e6` ticks ≈ **0.96 s**（QEMU 的 ARM 通用定时器默认就是这个频率）。

一个持有调度锁的 vCPU 只要被宿主晚调度约一秒，客户机就 panic。这更像是 **TCG 的调度抖动**
而不是死锁——也解释了为什么 22 次里 19 次都死在同一把锁、而且和不带触摸/纯本地无关。

**做法**（一次只摘一个变量，别猜；前三条是按「1 秒超时」这个事实排的）：

1. **宿主优先级**（最便宜、最可能）：同一个镜像、同一组参数，把 QEMU 放在
   `nice -n -20`（或 `chrt` 的实时类）下跑 3 次。如果通过率从 14% 跳到一半以上，
   结论就是「宿主没及时调度 vCPU」，修复点在启动脚本，不在内核。
2. **`cpus=2` vs `cpus=4`**：vCPU 越少，锁的争用和 TCG 的线程切换越少。第四阶段说 `cpus=2`
   会卡在 spinner 上 35 分钟——先看它在 5–15 分钟里是同一个死法还是另一个。
3. **`-accel tcg,thread=single`**：4 个 vCPU 交给一个宿主线程轮转，锁的持有者不可能被并行
   切走。如果它不 panic（只会慢），就把「并行 vCPU 的调度抖动」钉死。
4. **只用来判定的实验**：把 `lock_ticket.c` 的 timeout 从 60e6 抬到 240e6（约 4 s），跑 3 次。
   如果 panic 变成「慢但对」→ 是延迟；如果照旧死 → 才是真死锁。
5. **`ustrace=1` + 串口**：`touch.py stacks` 会让 CPU 停不下来（第四阶段踩过），
   用内核自己打印的那条路（`ustacks`/`ustrace`，`qemu/symstack.py` 解）。

**第一轮实测（2026-11-03 15:20–16:00，`cpus=2`）**：

| 启动 | 配置 | 结果 |
|---|---|---|
| t0a | `cpus=2`，第四阶段镜像，`--no-panel` | **acceptance**：235 帧、451,885 B、0 panic |
| t0a2 | 同上（同样的参数再跑一次） | **panic**（4 帧，同一把锁 `pset0.sched_lock`，`current owner: 0xfffffff46ef49ee0 (on CPU 0)` 还活着） |
| t0a3 | `cpus=2`，带触摸的镜像（`rc.touch1`） | **acceptance**：249 帧、451,601 B、0 panic |

`cpus=2`：**2/3**；第四阶段的基线（`cpus=4`）：**3/22 = 14%**。Fisher 精确检验 p ≈ 0.06——
**有希望，但还不能说解决**。而且 t0a2 的 panic 说明「vCPU 少就不会被延迟」不成立：
持有者活着、就是迟迟不放手。

副作用要记住：`cpus=2` 把客户机的核数减半，启动更慢（约 10–18 分钟）、帧率 5.5–9 fps
（`cpus=4` 是 9.9–12 fps）。**所以它是稳定性实验的配置，不是 fps 对照的配置**——
任何跨配置的帧率比较都必须同一 `cpus`。

**判据**：10 次冷启动 ≥8 次到界面、0 panic；每次 panic 有归因（锁名或栈）。
**涉及**：`osfmk/arm64/sleh.c`/`lock_ticket.c`（只读不改，除非第 4 条要临时改 timeout）、
`drivers/input/hakios.virtio_input.c`、`userland/rc.*`（实验镜像）。
**回退**：实验镜像不进正式判据，`rc.vtop` 仍是 R1 的启动方式。

### T1 W1.5 的画面完整度（1–2 天，先定方向再动手）

**现状证据**（第五阶段 §2.4）：`remote_only` 下宿主每帧只有 9 个 draw、36 条顶点数组；
客户机自己的帧行是 `28 GL calls, 2 draws, 4 triangles, 1 clears`。存图里**有图标、没有文字/
Dock/搜索框**，截图 245 KB（本地配置 448 KB）。

**三条路，按代价排**：

1. **W1.6：remote_only 也上传一次面板（最省）**。客户机保留 `target_to_surface()` 那一次
   合成结果的复制（它已经在显示面里），把 `0xFFFF0001` 照旧送到宿主；宿主拿它当
   「已合成的那一层」，用来补自己的重放漏掉的部分（文字/Dock），或者干脆两边取其一。
   代价：一次 4.88 MB 的按引用上传（约 10 KB 记录）+ 客户机不画层（W1 的收益还在）。
   风险：两条来源的画面会打架（谁在上、谁透明），要先定合成规则。
2. **把 CA 的软件路径记进流**：先找到 CA 把文字写进显示面的那条路（不在 GL 里：可能是
   `CA::Render::update_...` 的 memcpy/blit，也可能走 eagl 的 plain draw）。找到之后按 opcode
   201 的形状送「按引用的一段行」，宿主直接写进显示面。代价最大，收益是 W2 真正成立。
3. **承认不完整**：`remote_only` 只当提速模式，显示仍用面板（第四阶段的形状），
   把「宿主独立出图」写成「记录器覆盖到的那部分」。代价：W2 的描述要改。

**判据**：选定一条，`remote_only` 截图 > 400 KB、存图里有文字与 Dock；记录仍 < 100 KB；≥5 fps；0 panic。
**涉及**：`userland/src/eagl.c`（`frame_end()`/`upload_changed_layers()`）、
`userland/src/eagl_remote.c`、`tools/glwire/{host_glwire.c,spec.def,dispatch.inc}`（如果 opcode 要加）。
**回退**：`glwire_local=1` 一直能把 W1 关掉。

### T2 一帧相位的来源（半天，一次启动）

第五阶段三次 clean control（`--no-panel` + `GLWIRE_NO_INPLACE=1` + `glwire_nopicture=1`）
全部死在启动（2、2、4 帧），一次都没到界面。T0 过了之后这个实验只要一次启动：

- 形如 `MINFRAMES=120 MINSHOT=1 XCMD="glwire_nopicture=1" HOST_ENV="GLWIRE_NO_INPLACE=1" ./harness/boot-measure.sh s3 <image> 900 --no-panel`；
- 看 `panel … picture … MATCH/DIFF` 的同帧率，与第四阶段 `np3` 的 17/141、72/140 比；
- 如果同帧率跳上去 → 相位来自宿主写回（W3 的 in-place）；没变 → 相位来自客户机的 present 时机。

**判据**：连续 ≥100 帧同帧 MATCH；或者给出相位的证据并把验收改成
`picture(N) vs panel(N+1)` 连续 ≥100 帧（要改需求文档，不能默认放宽）。

### T3 四配置滚动稳态（1 天，四次启动）

第五阶段只拿到 W1.5 的一格（**6.74 fps**，290 帧 / 43,031 ms）。剩下三格要重跑，
跑法已经改好：`rc.touch1`（一个 rwtouch 进程，600 s 起手、`mark scroll-start`、20 s zigzag、
`mark scroll-end`），启动时**把提前收工关掉**（`MINFRAMES=999999`），否则 l1 那样在 242 帧
就 acceptance 收工、根本到不了 600 s。

| 配置 | 构建 | 期望 |
|---|---|---|
| 纯本地基线 | `GL=1 CONFIGD=1 RC=rc.touch1` | 7–9 fps（第三轮的数字，本阶段第 18、19 次启动没跑到窗口） |
| 按引用 + 面板 + 行写 | `GLWIRE_LOCAL=1` | 4–6 fps（第四阶段空闲桌面；滚动窗口未测到） |
| W1.5 | `glwire_local=0` | **6.74 fps**（已测，w2） |
| W1.5 + W3 | 同上（W3 默认） | 同 W1.5 |

**判据**：一张表，每格一个数字 + 一次启动；缺的那格写「未测」，不许用别的配置的数字代替。

### T4 CA 自身的 CPU 成本（半天，纯软件测量）

「能不能到 24 fps」这个问题一直被推迟，现在要量：

1. `glframes=1` + `touch_lat=1` 已经有每帧的 `began/ended` 与 `copying out` 拆分；
2. 在 `remote_only` 下再拆一次：客户机一侧「记录 + flush 等待」的时间 vs 宿主一侧 `took`
   （`host_glwire: frame …, X ms` 已有）；
3. 结论写成「一帧 100 ms 里，CA 自己占多少、宿主占多少、TCG 的解释器占多少」。

**判据**：一帧的时间账（三个数字相加 ≈ 帧间隔），以及一句「要超过 9 fps 必须先消掉哪一段」。

---

## 四、不做（已被数据否掉，别重试）

- 不再承诺 24 fps（T4 出来之前，任何 fps 的说法都只是猜测）。
- **不再把「同一天的两次启动」当 A/B**：本轮启动通过率 22%，一次成功的启动证明不了配置优劣。
  每个配置至少 3 次，或者先在 T0 里把通过率提上去。
- 像素去重（CA 每帧整屏重合成）。
- 客户机内启用 GL 加速渲染器 / llvmpipe（缺 arm64-iOS 的 libLLVM，且不在 CA 合成链上）。
- 只降分辨率（fbscale 3→1 只换来 2 倍，瓶颈是模拟执行）。
- `qemu/touch.py stacks`（本 QEMU 配置不能 halt CPU，会 panic）；`pkill -f`（第五阶段踩过：
  它把自己的 shell 也杀了）。
- 并行跑第二台虚拟机 / 启动时编译（两次 kernel panic 都发生在并行任务期间）。

---

## 五、验证环境与操作规范

### 5.1 构建（约 25–70 s）

```bash
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
export THEOS=$HOME/theos IOSVM_USE_LLD=1
export PATH=$IOSVM/xnu-deps/host/bin:/tmp/cctools/bin:$PATH
cd $IOSVM/userland
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop GLWIRE_LOCAL=1 ./build.sh root.apfs   # 客户机照旧本地画
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs                  # W1.5（加 glwire_local=0）
GL=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs                             # 纯本地基线
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.touch1 ./build.sh root.apfs                # 带一次触摸（S4/S5）
```

### 5.2 一次冷启动 = 一次测量

```bash
cd /home/lshan/DSH-desktop/iosvm/tools/glwire
make && make test                       # 宿主与协议回归

# T0 的概率：连跑，失败的也算一次
IMAGE=/tmp/root-fixed-net.apfs WANT=8 MAXTRIES=10 ./harness/cold-boots.sh

# 一次测量（结束条件就是判据本身）
./harness/boot-measure.sh r2 /tmp/root-fixed-net.apfs 1000 --no-panel

# S5 的窗口（不设提前收工，否则到不了 600 s 的触摸）
MINFRAMES=999999 MINSHOT=999999999 ./harness/boot-measure.sh w2 /tmp/root-touch-w15.apfs 800
```

**硬性禁忌**：同一时间只能有一台虚拟机（`payload.bin`、`ram.img`、端口 9110 共享）；
一个启动器没退出前不要起下一个；测量期间不要编译；`pkill -f` 会杀自己的 shell。

### 5.3 判读

```bash
grep -a "host_glwire: frame" /tmp/<label>-host.log | tail -3      # records / picture / panel
grep -a "replay " /tmp/<label>-host.log | tail -1                 # 这一帧重放了哪些调用
grep -a "SIGSEGV\|panic(cpu\|usertrap:" /tmp/<label>.log | tail   # 崩溃
python3 harness/bgra2png.py /tmp/<dump>/f                         # 存图 + 非黑像素/颜色数
python3 harness/fps.py /tmp/<label>.log /tmp/<label>-host.log     # 有内核时间戳时的窗口
python3 /tmp/window_fps.py /tmp/<label>.log                       # 没有时间戳时按行位置取窗口
```

### 5.4 每个结论的证据要求

一个结论 = 一张日志行（原样贴）+ 一个数字 + 一次可复现的启动参数；
「没做到」也要写清楚卡在哪一行日志，不要写成「还需要优化」。

---

## 六、里程碑（每个一次提交，信息带前后数字）

| 里程碑 | 内容 | 判据 |
|---|---|---|
| M6.0 | T0：启动稳定性 | 10 次 ≥8 次到界面、0 panic、panic 有归因 |
| M6.1 | T1：选定 W1.5 的补全路线并实现 | >400 KB 截图、有文字与 Dock、记录 <100 KB |
| M6.2 | T2：相位定位并消灭（或改验收） | 连续 ≥100 帧同帧 MATCH，或相位来源的证据 |
| M6.3 | T3：四配置滚动稳态 | 四格都有数字（或明确的「未测」） |
| M6.4 | T4：CA 的 CPU 成本账 | 一帧三个数字相加 ≈ 帧间隔 |
| M6.5 | 文档 + 提示 | `/home/lshan/dsh.MD/` 与 `iosvm/docs/` 同步 |

---

## 七、诚实的预期

- **能确定的**：T0 只要肯花 10 次启动就能量出概率和锁的归因；T2 一次干净启动就能定相位；
  T3 的四格只要各有一次成功启动。
- **不确定的**：T1。第 3 条路（承认不完整）最省但改结论；第 1 条路（W1.6）能补上画面但要定
  合成规则；第 2 条路最贵。**在 T1 动代码之前，先写清楚选哪条、为什么。**
- **不能承诺的**：超过 9 fps（纯本地基线的量级），以及「宿主独立出图」在 remote_only 下
  能画出完整画面——第五阶段已经量到它缺文字，这是记录器的边界，不是调参能解决的。

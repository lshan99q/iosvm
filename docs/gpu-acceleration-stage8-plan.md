<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第八阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第八阶段工程文档

> 建立时间：2026-11-03 20:20（第七阶段结论写完、提交推送之后）
> 前置阅读：`IOSVM-GPU加速第七阶段工程.md`（目标）、`IOSVM-GPU加速第七阶段.md`（那一轮的数字与结论）、
> `iosvm/tools/glwire/harness/README.md`（判据与命令）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状（三个数字）

1. **启动**：`cpus=4 … tlto_us=30000000` 下这一轮 **5 次启动 0 panic**，界面 420–660 s；
   但**停顿本身还在**（宿主看到的帧间隔 >1 s 有 37–41 次、最大 16.96 s），
   「10 次 ≥8 次到界面、0 panic」的验收**还没跑**。
2. **画面**：`remote_only` 下客人机的显示面**一个像素都没被画**（面板 dump 0/1,218,000），
   所以「拿面板补漏」（A 路）判死；缺的文字由一条**不在 62 个槽位里**的 GL 路径画出来，
   B 路（把它记进流里）是唯一还站着的路，且有一次性定位的做法。
3. **测量**：哈希判据量不了相位（0/128 同帧、0/112 ±1 帧）；P3 的触摸固定在 600 s 而界面在 420–660 s
   才出来；P4 一帧中位 20.0 ms（等宿主 7.6 + 客人机/空闲 11.9），**瓶颈是停顿不是帧成本**。

所以这一阶段只做三件事：**把启动的 10 次验收跑完并钉死停顿的来源**、**把 B 路定位出来**、
**把 P3 的窗口挪到界面之后**。其余都是它们的下游。

---

## 一、这一阶段的三个决定（不是十件事，是三个）

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 停顿：先做**宿主侧对照**（不重写 `ram.img`、I/O 空闲窗口）还是直接进内核采样（`ustacks`）？ | 直接进内核：一头扎进 guest 栈，可能查的是宿主 I/O 的影子；先做对照：多花 3–4 次启动 | P1' 的归因，以及「9 fps 以上」能不能谈 |
| D2 | 画面：先做 **B 路定位**（一次启动）还是先补 **P3 四配置表**？ | 先 P3：四格数字拿到手，但画面停在 C（`remote_only` 只是提速模式）；先 B：可能白花一次启动（万一入口不在 EGL 表里） | W2 的定义、以及 P1 的验收能不能过 |
| D3 | P3 的窗口：改 `rwtouch` **等界面**（代码）还是用 `qemu/touch.py swipe` **手动驱动**（脚本）？ | 改代码：多一次构建（1–2 min）+ 一次验证启动；手动：每个配置要人在跟前按时间点驱动 | 四配置表的可信度 |

> 三个决定都要**先写判据再动手**，都允许选「不做」，但选了就要把结论写死。

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0' | 启动验收（第七阶段留下的） | 同一套参数（`cpus=4 … tlto_us=30000000`、`/tmp/root-fixed-net.apfs`）**10 次冷启动 ≥8 次到界面、0 panic**；每格的启动预算写进文档 | 一切测量 |
| P1' | 停顿归因 | 至少两组对照（宿主 I/O 空闲窗口 vs 负载窗口、`ZERO_RAM=0` 复用 `ram.img` vs `=1`），给出「停顿跟着谁走」的数字；若仍不清晰，给出 `ustacks` 采样到的、停顿时刻客人机在哪 | 9 fps 以上的任何说法 |
| P2' | 相位（换尺子） | 有容差比较（每通道差 ≤8 的像素 ≥95% 视为同帧）下 `picture(N)` vs `panel(N+1)` 的比率，≥100 帧样本；比率与同帧比率一起给出 | R2 判据 |
| P3' | 四配置滚动表 | 四格各一次 20 s 窗口（触摸发生在界面之后），每格一个数字；缺的写「未测」+ 卡在哪一行 | 结论 |
| P4' | B 路定位 | 一次启动里**指名**画 glyph 的 EGL 入口（`spec.def` 之外的槽位名 + 调用次数）；能加进 `spec.def` 就加，并给出加/不加密的判据 | P1 的验收 |
| P5' | 文档与提交 | `/home/lshan/dsh.MD/` 与 `iosvm/docs/` 同步；`make test` 通过；每个决定一个提交；**推送到 GitHub**（`iosvm` 仓库） | — |

> P0' 与 P1' 先做（启动不钉死，任何「一秒的差别」都测不出来）；P4' 与 P3' 可以并行在两次启动上安排。

---

## 三、需求详述

### P0' 启动验收（半天，10 次启动 ≈ 2 小时）

- 一条命令：`IMAGE=/tmp/root-fixed-net.apfs WANT=8 TRIES=12 TAG=m8p0 XCMD="tlto_us=30000000" UI_GRACE=300 ./harness/boots.sh`
- 判据：`pass ≥ 8/10`（`end=ui` + 0 panic + 0 SIGSEGV），并把**每次的 ui 秒数**记下来（这一轮是 420–660 s）。
- 失败要归因：`boots.sh` 已经把 panic 的锁地址写进失败行；还差一条——**失败时的截图**（`-crash.png` / `-final.png`）留档。
- 若 8/10 不过：把每次失败归类（没界面 / panic / 别的），给出「每格测量要几次启动」的预算，写进 `harness/README.md`。

### P1' 停顿归因（1 天，最贵的未知）

现状：`tlto_us=30000000` 只是让 panic 变成「等过去」，停顿 10–17 s 还在；`/proc/pressure/io` 在这一轮
启动期间 some avg300 = 20%；`ZERO_RAM=1` 每轮重写 2.4 GB 的 `ram.img`。

做法（一次只改一个变量，每组 ≥3 次启动）：

1. **宿主的 I/O**：`/proc/pressure/io` 的 some/full 在每轮启动前后各记一次；同时 `iostat -x 1` 采样 5 分钟，
   把停顿时刻（宿主帧间隔 >1 s）和磁盘活动对齐。如果停顿全部落在磁盘活动上 → 宿主 I/O；
2. **`ZERO_RAM` 的代价**：`ZERO_RAM=0`（复用上一轮 `ram.img`，只用「启动前把上一轮界面区域清零」的补丁）
   × 3 次，和 `ZERO_RAM=1` 的 3 次比停顿数与 ui 时间。这一条同时会回答「重写 2.4 GB 是不是停顿的来源」；
3. **客人机侧**：若前两条都不成立，用 `ustacks=<proc> ustacks_at=<秒>`（**不要用 `touch.py stacks`**）
   在停顿窗口采样两三次栈，看卡在哪个子系统。

**判据**：一张表（对照 × 停顿次数 / 最长停顿 / ui 秒数），以及一句「停顿跟着谁走」；做不到就给「三条都不成立」
和采样到的栈。**不做**：在没有对照的情况下改内核。

### P2' 相位（半天，一次启动 + 一个脚本）

- 新工具：`harness/diffdump.py` 已经有逐像素差，加 `--tolerance 8`：每个通道差 ≤8 的像素占比 ≥95% 算同帧；
- 一次启动：`XCMD="tlto_us=30000000 glwire_nopicture=1" HOST_ENV="GLWIRE_NO_INPLACE=1 GLWIRE_DUMP=/tmp/m8/p2/f GLWIRE_DUMP_AT=600" ./harness/boot-measure.sh m8p2 /tmp/root-fixed-net.apfs 800 --no-panel`（**先 `mkdir -p`**）；
- 判据：dump 的一对图上给出「同帧 / 后一帧」的容差比较结果，并与第七阶段的哈希结果并列；
- 回退：这一项不影响别的测量，做不出来就写「尺子还没换成」。

### P3' 四配置滚动表（1 天，四次成功启动）

- 触摸等界面：`rwtouch` 加 `wait-frame N`（等 `MiniIOMobileFramebuffer` 报出 N 帧再 wait），或者用
  `qemu/touch.py swipe X0 Y0 X1 Y1 MS` 在界面出现后手动驱动（**优先手动**：不改镜像，能立即做）；
- 每格：`MINFRAMES=999999 MINSHOT=999999999 MAXSECS=800`，窗口用 `harness/window_fps.py`；
- 判据：四格各一个数字（或「未测」+ 卡在哪一行）；同一 `cpus`，同一触摸脚本。

### P4' B 路定位（1 天，两次启动）

做法：

1. **只读代码**：`eagl_slots.txt` 里哪些槽位没被 `spec.def` 覆盖、且是「画」的候选
   （`glDrawArraysInstanced`、`glDrawElementsInstanced`、`glDrawRangeElements`、`glMultiDrawArrays`…）；
2. **接计数**：给这些槽位套一层计数（`say` 一行，第 N 帧打印一次），构建（**先 `touch obj/stage0`**）；
3. **一次启动**：`XCMD="tlto_us=30000000 glframes=1"`，看哪几个计数在一帧里非零、且与 glyph 的 span 数吻合；
4. 找到了就按 `spec.def` 的形状加进记录器（`!local`），宿主重放；`remote_only` 的截图应出现文字/Dock；
5. 判据：一次启动的日志行（槽位名 + 次数）+ 加了之后的截图（> 400 KB、有文字、0 panic）；
   做不到就写「不在 EGL 表里」并把下一步指向 CA 的 Metal/软件路径。

---

## 四、不做（已被数据否掉，别重试）

- **不再只改 `src/eagl.c` 就跑**：`system.apfs` 不重打，客人机还是旧库（第七阶段白跑两次）。
  正确姿势：`touch obj/stage0 && ./build.sh root.apfs`。
- 不再用哈希比 panel/picture（0/128，两个渲染器不可能逐像素一致）。
- 不再在界面之前驱动触摸（600 s 的触摸，界面 420–660 s 才出来）。
- 不再拿「一次成功/失败的启动」判断配置优劣（同一天同一镜像，420–660 s 与 panic 都见过）。
- 不再承诺 24 fps（停顿没归因之前）。
- 像素去重；客户机内启用 GL 加速渲染器；只降分辨率；`touch.py stacks`；`pkill -f`；并行跑第二台虚拟机。

---

## 五、验证环境与操作规范

### 5.1 构建（改端口代码时）

```bash
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
export THEOS=$HOME/theos IOSVM_USE_LLD=1
export PATH=$IOSVM/xnu-deps/host/bin:/tmp/cctools/bin:$PATH
cd $IOSVM/userland
touch obj/stage0                       # 关键：否则 system.apfs 不重打，客人机还是旧库
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.touch1 ./build.sh root.apfs    # 约 60–90 s；系统镜像改动时 2–3 min
cp root.apfs /tmp/root-m8.apfs
```

### 5.2 一次冷启动 = 一次测量（**同一时间只有一台虚拟机**）

```bash
cd /home/lshan/DSH-desktop/iosvm/tools/glwire
make && make test
IMAGE=/tmp/root-fixed-net.apfs WANT=8 TRIES=12 TAG=m8p0 XCMD="tlto_us=30000000" UI_GRACE=300 ./harness/boots.sh
```

**团队协作的锁**：跑启动的人先 `mkdir /tmp/iosvm-vm.lock`（成功才启动），结束 `rmdir`；
要构建/改镜像的人等锁不存在且 `pgrep -f qemu-system-aarch64` 为空再动手。
**测量期间不编译**是硬规则（第七阶段的两次 panic 都发生在宿主忙的时候）。

### 5.3 判读

```sh
grep -a "^xb: " /tmp/boots.log | tail -5          # 每次的分类与 ui 秒数
grep -a "host_glwire: frame " /tmp/<label>-host.log | tail -3
python3 harness/window_fps.py /tmp/<label>.log /tmp/<label>-host.log
python3 harness/diffdump.py /tmp/m8/p2/f -o /tmp/m8/p2/diff --tolerance 8
```

每一条结论 = 一张日志行（原样贴）+ 一个数字 + 一次可复现的启动参数。

---

## 六、里程碑（每个一个提交，信息带前后数字）

| 里程碑 | 内容 | 判据 |
|---|---|---|
| M8.0 | P0' 启动验收 | 10 次 ≥8 到界面、0 panic，或明确的失败分类与启动预算 |
| M8.1 | P1' 停顿归因 | 对照表 + 一句「跟着谁走」，或三条都不成立的证据 |
| M8.2 | P2' 相位换尺子 | 容差比较的比率（同帧 / 后一帧） |
| M8.3 | P4' B 路定位 | 槽位名 + 次数（一次启动的日志），或「不在 EGL 表里」 |
| M8.4 | P3' 四配置表 | 四格（或明确的未测） |
| M8.5 | 文档 + `make test` + 推送 | GitHub `iosvm` 上有这一阶段的提交 |

---

## 七、诚实的预期

- **能确定的**：P0' 只要 2 小时启动就能给结论；P2' 一次启动 + 一个脚本；P3' 只要触摸挪到界面之后。
- **不确定的**：P1'。停顿可能是宿主的 I/O（`/proc/pressure/io` 有 20%），也可能是 TCG 下客户机自己停住；
  两组对照给不出方向的话，这一项会退回「采样到栈但不知道原因」。
- **不能承诺的**：B 路一定找得到——第七阶段只证到「那些 draw 不在记录流里」，还没证到它们在 EGL 表里；
  超过 9 fps；以及「宿主独立出图」画出完整画面。

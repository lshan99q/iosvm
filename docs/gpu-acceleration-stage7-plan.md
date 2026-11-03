<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第七阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第七阶段工程文档

> 建立时间：2026-11-03（第六阶段 T0 的第一次对照之后）
> 前置阅读：`IOSVM-GPU加速第五阶段.md`（S1–S6 的实测与结论）、
> `IOSVM-GPU加速第六阶段工程.md`（T0–T4 的目标；T0 的锁定位与 `cpus=2` 那次对照写在它的 §三）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状（三个数字）

1. **启动**：panic 等的是 `pset0.sched_lock`（`0xfffffff007b68880` = `_pset0 + 0x100`），
   超时 `60e6` mach ticks，按这台 VM 的 62.5 MHz 计 **约 1 秒**。`cpus=2` 三次过两次，
   `cpus=4` 二十二 次只过三次——**能压，没修好**。
2. **画面**：宿主确实独立出图（停掉面板上传仍画出 38.1% 非黑的桌面），
   但记录流里**没有图标文字、Dock、搜索框**，所以 `remote_only` 的屏幕只有 245 KB（本地合成是 448 KB）。
3. **测量**：滚动窗口只拿到 W1.5 一格（**6.74 fps**）；S3 的相位三次 clean control 都死在启动；
   S4 的判据把触摸排除掉了（不带触摸 2/11、纯本地 0/3）。

所以这一阶段不再铺新面，只做**三个决定**，其余都是它们的下游。

---

## 一、这一阶段的三个决定（不是十件事，是三个）

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 启动：修根因，还是把重试写进测量协议 | 每格测量要多烧 3–7 次启动（今天实测），任何 A/B 都测不出 1 fps 的差别 | 所有后续测量的可信度 |
| D2 | 画面：W1.6 补面板 / 记录 CA 的软件路径 / 承认不完整 | 选错就是又花两天写一个没人用的模式 | W2 的定义、以及「宿主独立出图」这句话的边界 |
| D3 | 测量：相位与四配置表要多高的启动预算 | 预算太低，表永远是「未测」 | R2 的验收、以及下一阶段能不能谈 24 fps |

> 三个决定都要**先写判据再动手**，而且都允许选「不做」，但选了就要把结论写死，
> 不许留「以后再说」——这一轮的教训是：含糊的判据比没有判据更贵。

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0 | D1：启动可预测 | **要么**选定的一套参数（镜像 + `QEMU_SMP` + `QEMU_EXTRA`）10 次 ≥8 次到界面、0 panic；**要么**交付「重试协议」：`harness/` 里一条命令能跑 N 次、失败的自动重来，并给出实测通过率 | 一切测量 |
| P1 | D2：画面完整度 | `remote_only` 截图 > 400 KB，存图里有图标文字、Dock、搜索框；记录仍 < 100 KB；≥5 fps；0 panic | W2 的结论 |
| P2 | 相位 | 连续 ≥100 帧同帧 `panel(N) == picture(N)`；做不到就给出相位证据，并把验收写成 `picture(N) vs panel(N+1)` 连续 ≥100 帧（要改需求文档，不能默认放宽） | R2 判据 |
| P3 | 四配置滚动表 | 四格各一次成功的 20 s 窗口，每格一个数字；缺的写「未测」+ 卡在哪一行 | 结论 |
| P4 | CA 自身 CPU 成本 | 一帧的时间拆成三段（记录/等待、宿主重放、TCG 解释），三个数字相加 ≈ 帧间隔 | 24 fps 的任何说法 |
| P5 | 文档与提交 | `/home/lshan/dsh.MD/` 与 `iosvm/docs/` 同步；`make test`（`tools/glwire`）通过；每个决定一个提交 | — |

> P0 先做，而且**只做 P0**，直到它有结论：今天的 22 次启动里 19 次死在同一把锁上，
> 在这条没定下来之前做的任何测量都只是抽样运气。

---

## 三、需求详述

### P0 启动可预测（D1，半天到一天）

**已有的账**：

| 配置 | 冷启动 | 到界面 | 通过率 |
|---|---|---|---|
| `cpus=4`（第四阶段以来的全部启动） | 22 | 3 | 14% |
| `cpus=2` | 3 | 2 | 67%（p ≈ 0.06，样本太小） |

**三条探针**（每条 3 次启动，一次只改一个变量；判据是「3 次都到界面」才算这条有戏）：

1. **`-accel tcg,thread=single`**（`QEMU_EXTRA="-accel tcg,thread=single"`）：
   4 个 vCPU 交给一个宿主线程轮转，锁的持有者不可能被并行切走。它若稳定，就把
   「MTTCG 下的调度抖动」钉死；代价是慢（启动可能 20–30 分钟，MAXSECS 要放到 1800）。
2. **宿主优先级**：`sudo -n nice -n -20 env … harness/boot-measure.sh …`。
   QEMU 的 vCPU 线程被宿主晚调度 ~1 秒就够 panic，提高优先级是最便宜的对照。
3. **只用于判定的实验**：把 `osfmk/kern/ticket_lock.c` 的超时从 `60e6` 抬到 `240e6`（约 4 s），
   重编内核跑 3 次。它**不能进正式判据**（那是把症状藏起来），只用来回答
   「到底是延迟还是真死锁」。若抬了就不 panic → 是延迟；照旧死 → 真死锁，那要去查
   `pset0.sched_lock` 的持有者在哪一段代码里睡着了。

**若三条都不灵**，就选「重试协议」，并把判据写成通过率而不是「每次都成」：

- `harness/` 加一条 `boots.sh WANT=4 TRIES=12`：失败（panic / 没界面）的自动重来，
  成功的记账；每格测量的启动预算写进文档；
- 判据变成「某配置的实测通过率 ≥ X%，且每格测量的重试次数 ≤ N」，X 与 N 要由实测给出，
  不许拍脑袋。

**两条都要记住的副作用**：

- `cpus=2` 把客户机核数减半：启动更慢（10–18 分钟）、帧率 5.5–9 fps（`cpus=4` 是 9.9–12）。
  **它是稳定性实验的配置，不是 fps 对照的配置**；跨配置比帧率必须同一 `cpus`。
- `thread=single` 更慢，同样不能拿来做 fps 对照。

**判据**：见 P0（二选一），并且把选定的一套参数写进 `harness/README.md`。

### P1 画面完整度（D2，1–2 天）

**现状**：`remote_only` 下每帧只有 9 个 draw、36 条顶点数组；图标是纹理，**文字/Dock/搜索框
不在这些调用里**。屏幕只剩宿主这张画面，于是 245 KB。

**三条路**（先选，再写代码）：

| 路 | 做法 | 代价 | 判据能不能过 |
|---|---|---|---|
| A：W1.6 补面板 | `remote_only` 也上传一次 `0xFFFF0001`（客户机保留 `target_to_surface()` 那一次复制），宿主拿它补自己的漏 | 一次 4.88 MB 按引用上传（约 10 KB 记录）；要定「两条来源谁在上」的合成规则 | 能（但 W2 的收益缩水） |
| B：记录软件路径 | 找到 CA 把文字写进显示面的那条路，按 201 的形状送「按引用的一段行」 | 最贵，要先定位那条路（不在 GL 里） | 能，且 W2 真正成立 |
| C：承认不完整 | `remote_only` 只当提速模式，显示仍用面板（第四阶段的形状） | 最省 | **不能**，要改结论 |

**建议顺序**：先花半天做 B 的定位（只读代码 + 一次 `gltrace`，不动结构），
定位成功就走 B；定位失败就在 A 与 C 之间选，并把「为什么不是 B」写进文档。

**判据**：`remote_only` 截图 > 400 KB、存图有文字与 Dock；记录 < 100 KB；≥5 fps；0 panic。
**回退**：`glwire_local=1` 一直能把 W1 关掉。

### P2 相位（半天，一次启动）

第六阶段三次 clean control 都死在启动（2、2、4 帧）。P0 有结论后，这个实验只要一次：

```sh
cd /home/lshan/DSH-desktop/iosvm/tools/glwire
MINFRAMES=120 MINSHOT=1 XCMD="glwire_nopicture=1" \
  HOST_ENV="GLWIRE_NO_INPLACE=1 GLWIRE_DUMP=/tmp/s3/f GLWIRE_DUMP_FROM=60" \
  ./harness/boot-measure.sh s3 <最稳的镜像> 900 --no-panel
```

读 `panel … picture … MATCH/DIFF` 的同帧率，与第四阶段 `np3` 的 17/141、72/140 比：
同帧率跳上去 → 相位来自宿主写回（W3 的 in-place）；没变 → 相位来自客户机的 present 时机。

**判据**：连续 ≥100 帧同帧 MATCH，或相位证据 + 验收改写。

### P3 四配置滚动表（1 天，四次成功启动）

跑法已经定死：`rc.touch1`（一个 rwtouch 进程，600 s 起手、`mark scroll-start`、20 s zigzag、
`mark scroll-end`），**关掉提前收工**（`MINFRAMES=999999`），用 `harness/window_fps.py` 算窗口。

| 配置 | 构建 | 已知 |
|---|---|---|
| 纯本地基线 | `GL=1 CONFIGD=1 RC=rc.touch1` | 未测（三次都没到窗口） |
| 按引用 + 面板 + 行写 | `GLWIRE_LOCAL=1` | 未测（一次 acceptance 收工、两次 panic） |
| W1.5 | 不带 `GLWIRE_LOCAL` + `glwire_local=0` | **6.74 fps**（290 帧 / 43,031 ms） |
| W1.5 + W3 | 同上 | 同 W1.5 |

**判据**：每格一次成功启动的数字；**同一 `QEMU_SMP`**；缺的写「未测」。

### P4 CA 自身 CPU 成本（半天）

`glframes=1` + `touch_lat=1` 已有每帧 `began/ended` 与 `copying out` 的拆分；
再补两段：客户机「记录 + flush 等待」与宿主 `took`（`host_glwire: frame … X ms`）。
三段相加 ≈ 帧间隔才算账做对了。

**判据**：一帧的三段数字 + 一句「要超过 9 fps 必须先消掉哪一段」。

---

## 四、不做（已被数据否掉，别重试）

- **不再用一次成功/失败的启动证明配置优劣**：通过率 14–67%，单次启动的噪声比要测的差别大。
- 不再承诺 24 fps（P4 出来之前）。
- 不在同一个阶段里同时改 `QEMU_SMP`、镜像与宿主参数——今天的 22 次里有一半的账因此读不清。
- 像素去重（CA 每帧整屏重合成）。
- 客户机内启用 GL 加速渲染器 / llvmpipe（缺 arm64-iOS 的 libLLVM，且不在 CA 合成链上）。
- 只降分辨率（fbscale 3→1 只换来 2 倍，瓶颈是模拟执行）。
- `qemu/touch.py stacks`（本 QEMU 配置不能 halt CPU，会 panic）；
  `pkill -f <pattern>`（第五、六阶段各踩过一次：它把自己的 shell 也杀了——用 `ps` 取 pid 再 kill）。
- 并行跑第二台虚拟机 / 测量期间编译。

---

## 五、验证环境与操作规范

### 5.1 构建（25–70 s）

```bash
IOSVM=/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel
export THEOS=$HOME/theos IOSVM_USE_LLD=1
export PATH=$IOSVM/xnu-deps/host/bin:/tmp/cctools/bin:$PATH
cd $IOSVM/userland
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.touch1 ./build.sh root.apfs                 # W1.5（加 glwire_local=0）
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.touch1 GLWIRE_LOCAL=1 ./build.sh root.apfs  # 本地画 + 面板 + 行写
GL=1 CONFIGD=1 RC=rc.touch1 ./build.sh root.apfs                            # 纯本地基线
```

### 5.2 一次冷启动 = 一次测量

```bash
cd /home/lshan/DSH-desktop/iosvm/tools/glwire
make && make test

# 默认（cpus=4）；第六阶段加的两个旋钮
QEMU_SMP=2 ./harness/boot-measure.sh a /tmp/root.apfs 1200
QEMU_EXTRA="-accel tcg,thread=single" ./harness/boot-measure.sh b /tmp/root.apfs 1800

# 四配置的滚动窗口（关掉提前收工）
MINFRAMES=999999 MINSHOT=999999999 ./harness/boot-measure.sh w2 /tmp/root-touch-w15.apfs 800
python3 harness/window_fps.py /tmp/w2.log /tmp/w2-host.log
```

### 5.3 判读与账

```bash
grep -a "host_glwire: frame" /tmp/<label>-host.log | tail -3      # records / panel / picture
grep -a "replay " /tmp/<label>-host.log | tail -1                 # 这一帧重放了什么
grep -a "SIGSEGV\|panic(cpu\|usertrap:" /tmp/<label>.log | tail   # 崩溃
python3 qemu/sym.py 0xfffffff007b68880                            # 锁 → _pset0 + 0x100
```

每一条结论 = 一张日志行（原样贴）+ 一个数字 + 一次可复现的启动参数；
「没做到」要写出卡在哪一行，不许写「还需要优化」。

---

## 六、里程碑（每个一个提交，信息带前后数字）

| 里程碑 | 内容 | 判据 |
|---|---|---|
| M7.0 | D1：启动的结论（参数或重试协议） | P0 的判据之一，并写进 `harness/README.md` |
| M7.1 | P2：相位（先做，只要一次启动） | 连续 ≥100 帧同帧 MATCH 或相位证据 |
| M7.2 | D2：三条路选一条并实现 | P1 的判据 |
| M7.3 | P3：四配置表 | 四格（或明确的未测） |
| M7.4 | P4：CPU 成本账 | 三段数字相加 ≈ 帧间隔 |
| M7.5 | 文档两处同步 + `make test` | 通过 |

---

## 七、诚实的预期

- **能确定的**：D1 的三条探针每条 3 次就能给结论（一条探针 = 3 次启动 ≈ 1 小时）；
  P2 一次启动就能定相位；P3 在启动稳定后一天能补齐。
- **不确定的**：P1 的 B 路（记录 CA 的软件路径）——今天只证到「那些绘制不在记录流里」，
  还没找到它们具体走哪条代码。**先定位再承诺。**
- **不能承诺的**：超过 9 fps；以及「宿主独立出图」在 `remote_only` 下画出完整画面——
  第三阶段以来所有证据都说这是记录器的边界，不是调参能跨过去的。

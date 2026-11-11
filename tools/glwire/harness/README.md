# harness — 一次冷启动、一次测量、一个结论

这一轮所有数字都是这几个脚本量出来的。它们的共同前提是**同一时间只有一台虚拟机**：
`payload.bin`、`qemu/ram.img` 与端口 9110 是共享的，第二台会把第一台带进
`Ticket spinlock[…] timeout` panic（`docs/gpu-acceleration-plan.md` §0.6-2）。

| 脚本 | 作用 |
|---|---|
| `boot-measure.sh LABEL ROOT.apfs MAX_SECONDS [HOST_ARGS]` | 一次冷启动 + 一个 `host_glwire` + 一对日志。**结束条件就是验收判据本身**：宿主 ≥200 帧且截图 > 300 KB；780 s 还没有 100 KB 的截图就判为没界面（卡在 `lsd` 的启动）直接收工。`HOST_ENV=…` 给宿主传环境（`GLWIRE_DUMP`、`GLWIRE_DUMP_AT`、`GLWIRE_SAVE_RECORDS`、`GLWIRE_NO_INPLACE`），`XCMD=…` 追加内核启动参数（例 `glwire_nopicture=1`、`tlto_us=3840000`），`QEMU_SMP=…`（默认 4，第六阶段 T0 用它量 `cpus=2`）与 `QEMU_EXTRA=…`（原样加到 QEMU 命令行，例 `-accel tcg,thread=single`）。第七阶段加了 `UI_GRACE=S`（界面出现后再跑 S 秒，`end=ui`）与 `ZERO_RAM=1`（每轮重建 ram.img，见下） |
| `boots.sh` | 第七阶段 P0 的**重试协议**：`IMAGE=… WANT=8 TRIES=12 ./harness/boots.sh` 连跑到够 N 次或次数用完，逐次分类（`end=ui` + 0 panic/SIGSEGV 才算 PASS），最后一行给 pass/attempts、interface seen、panic 数、每次的 ui 秒数与失败原因（含 panic 的锁地址） |
| `diffdump.py PREFIX [-o DIR]` | `GLWIRE_DUMP` 的 panel 与 picture 逐像素比：面板画了而重放没画的区域（"missing"）及其包围盒与 12x26 网格图，另存 panel/picture/diff 三张 PNG。P1 用它回答「记录流漏了什么」 |
| `four-cell.sh [--dry-run]` | P4：四份构建（纯本地/按引用/W1.5/W1.5+W3）各一次 20 s 滚动窗口，输出 `ui/frames/fps/span/shot bytes/host 0x502` 四行表；`CELL*_IMAGE` 给镜像，`CELL*_LABEL/XCMD/HOST_ENV` 可覆盖；纯本地 cell1 默认 `REQUIRE_HOST_FRAMES=0`（无 recorder，host 0 帧是设计），其余 1；cell3 默认 `GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1`，cell4 只 `GLWIRE_NO_PANEL=1`（同一镜像，host env 不同）；`WINDOW_FRAMES=120` 时用 120 帧计数窗口（cell1 默认 `WINDOW_CLOCK=guest`，其余 host，`CELLn_WINDOW_CLOCK` 可覆盖）；`--dry-run` 读已有 `/tmp/<label>-*` 日志只算表，不碰 QEMU |
| `cost.py GUEST.LOG HOST.LOG` | P4 的一帧三段账：`tl: g` 的 began/ended、`hakios-remote: f N wait` 的等待、宿主 `frame …, X ms` 与 interval，给出一帧里 TCG 工作 / 等宿主 / 其它 的均值与中位数 |
| `cold-boots.sh` | 连跑冷启动直到 N 次通过 R1 的判据（`IMAGE=…` 指定镜像） |
| `decrec.py FILE.records [-v]` | 把 `GLWIRE_SAVE_RECORDS` 存下来的记录流逐条解码（opcode 是 `spec.def` 的顺序） |
| `bgra2png.py PREFIX` | 把 `GLWIRE_DUMP` 的 `-picture-WxH.bgra` / `-panel-WxH.rgba` 转成 PNG（按屏幕方向）并统计非黑像素与颜色数 |
| `fps.py GUEST.LOG [HOST.LOG]` | 读 `rwtouch: mark` 划出的窗口，算窗口内驱动自报的稳态帧率（R5 用的就是这一列）。**要日志里有内核时间戳**；一处都没有（第五阶段的日志就是这样）时窗口会是空的 |
| `window_fps.py GUEST.LOG [HOST.LOG]` | 同一个窗口，**按两个 mark 在文件里的位置取**，不需要时间戳：把窗口内的 `MiniIOMobileFramebuffer: N frames in M ms` 加起来。第五阶段的 6.74 fps 是它算的 |

用过的两条命令：

```sh
# R1 的五次冷启动（镜像带记录器、客户机本地绘制、宿主画面板）
IMAGE=/tmp/root-fixed-net.apfs WANT=4 ./cold-boots.sh

# R2 的 --no-panel：宿主只重放录制流，面板只用于哈希
MINFRAMES=120 MINSHOT=1 HOST_ENV="GLWIRE_DUMP=/tmp/r2/f GLWIRE_DUMP_FROM=80" \
  ./boot-measure.sh r2 /tmp/root-fixed-net.apfs 1000 --no-panel

# S5 的那一格（20 s 滚动的驱动稳态）：带着 rc.touch1 的镜像，**关掉提前收工**，
# 否则它在 200 帧 + 300 KB 就 acceptance 收工，根本到不了 600 s 的触摸
MINFRAMES=999999 MINSHOT=999999999 ./boot-measure.sh w2 /tmp/root-touch-w15.apfs 800
python3 harness/window_fps.py /tmp/w2.log /tmp/w2-host.log     # -> 290 frames in 43031 ms = 6.74 fps
```

镜像用 `userland/build.sh` 出（约 25–70 s）：

```sh
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs            # W1+W2：客户机不画
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop GLWIRE_LOCAL=1 ./build.sh root.apfs  # 客户机照旧本地画
GL=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs                       # 纯本地基线（不装记录器）
GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.touch1 ./build.sh root.apfs          # 一次触摸（S4/S5 用）
```

`glwire_local=0`（`XCMD=…`）能在同一个镜像上把「客户机照旧本地画」关掉，等于 W1.5 那一列，
不用重编。


## 第七阶段：一次启动到底算不算「到界面」

第七阶段的第一件事是把**判据本身**修对，因为之前有三次启动的结论是假的：

1. **guest RAM 是 `qemu/ram.img` 的私有映射**，而那个文件不会每轮重建。上一轮的主界面还躺在里面，
   冷启动 30 秒时 `screendump` 拿到的就是它 —— 两次「ui at 30s / 448,195 B」都是上一轮的图
   （那一轮 launchd 还停在 UEFI）。所以 `ZERO_RAM=1`：每次启动前删 `ram.img`，
   `run-darwin.sh` 会把系统镜像重新放回装载地址；另外界面只在**宿主已经收到帧**之后才算。
2. **`fs.protected_regular=2`**：以 root 跑的脚本不能截断/覆写 `/tmp` 里属于别人的文件。
   `screendump` 的目标一直是 `/tmp/.xb-shot.png`，于是 root 的每一轮都写不进去，脚本读到的是
   15:57 留下的旧图。现在每次截图写本轮自己的路径（`/tmp/xb-$LABEL-$$.png`，先 `rm -f`）。
3. 判据改成两段：**界面**（第一张 ≥ MINSHOT 的截图，且宿主已有帧）与**活过宽限期**
   （`UI_GRACE=300`：界面之后 300 s 内 0 panic / 0 SIGSEGV / 0 usertrap）。`end=ui` 才算 PASS；
   `end=crash`、`end=noui`（780 s 无界面）、`end=noframes` 都是 FAIL，失败原因连同 panic 的锁地址一起记。

一次 P0 的账（2026-11-03 17:17–17:51，`nice -n -20`，`cpus=4`，`/tmp/root-fixed-net.apfs`）：

| 次 | 界面 | 结果 |
|---|---|---|
| 1 | 660 s（447,379 B） | 界面后 300 s 内 panic（`pset0.sched_lock`） |
| 2 | 420 s（448,409 B） | **PASS**：2,466 帧、0 panic |
| 3 | 无 | 5 分钟时 panic（同一把锁） |

同一台的稳态帧率（`host_glwire` 的最后 100 帧）：第 1 次 6.32 fps、第 2 次 7.32 fps。

探针 C 用的是 kernel 的 boot-arg，不用重编：`tlto_us=3840000` 把 ticket lock 的 deadline
从 60e6 ticks（约 0.96 s）抬到 3.84 s；`PE_parse_boot_argn("tlto_us")` 在
`ml_init_lock_timeout()` 里生效（`osfmk/arm64/machine_routines.c`）。

## 第九阶段：三族 deadline 一起抬（启动验收的口径）

第八阶段的 P0' 是 6/8：两次 panic 分别死在 `Spinlock`（`lock_panic_timeout`，251,658,240 ticks ≈4.03 s）
和 `waitq`（240,000,000 ticks ≈3.84 s），而 `tlto_us` 只抬了 ticket lock 那一族。三族各有各的开关，
一起抬才有「0 panic」：

| 族 | boot arg | 谁来写 | 60 s 配方 |
|---|---|---|---|
| ticket lock | `tlto_us`（µs） | 内核已有（`ml_init_lock_timeout()`） | `tlto_us=60000000` |
| 通用锁（waitq = 2×LockTimeOut） | `slto_us`（µs） | 内核已有 | `slto_us=30000000`（→ waitq 60 s） |
| `Spinlock[` / mutex 自旋 | `lpto_us`（µs） | **第八阶段新加**（`ml_init_lock_timeout()` 里写 `lock_panic_timeout`） | `lpto_us=60000000` |

```sh
cd /home/lshan/DSH-desktop/iosvm/tools/glwire
IMAGE=/tmp/root-fixed-net.apfs WANT=8 TRIES=12 TAG=m9p0 UI_GRACE=300 MAXSECS=1200 \
  XCMD="slto_us=30000000 tlto_us=60000000 lpto_us=60000000" ./harness/boots.sh
```

**口径两条**（第九阶段踩过）：
- `lpto_us` **没有收据行**：早期 `kprintf` 会在这个端口的 `STARTUP(TIMEOUTS)` 阶段把内核打死
  （症状：`jumping to …` → `kernel returned … 31 bytes`、消息缓冲空）。改内核之后**必须自己跑一次
  150 秒短启动**（有 ignition/launchd 行、没有 `kernel returned`）再交付；
- `lpto_us` 是否生效：看代码（只剩 `os_atomic_store`）+ 启动能过入口；若又出现 `Spinlock` panic，
  它的 `timeout:` 会是 **3,750,000,000 ticks**（60 s @62.5 MHz）而不是 251,658,240。


## Gate: 一条命令 PASS/FAIL（`gate.sh`）

```sh
./harness/gate.sh --root /tmp/root-local.apfs --out /tmp/gate-cell1
```

一条冷启动 + 固定检查，输出一行：

```
GATE: PASS  (checks: shot=302K names=7 0x502=0 panic=0)
```

`gate.md` 在 `--out` 目录，含每项明细、失败原因、`md5s.txt` 与截图/两份日志。
`names=` 是 `gate_names.py` 的白色文字行启发式（镜像无 OCR）；不是 OCR 值。

参数：

| 参数 | 说明 |
|---|---|
| `--root <root.apfs>` | 必填 |
| `--system <md5>` | 可选，校验 `userland/system.apfs` |
| `--window 20|120` | 首个截图后 UI 稳定窗口，默认 20 |
| `--out <dir>` | 产物目录，默认 `/tmp/gate-<UTC>` |
| `--minshot <bytes|300K|10M|1MB>` | 截图下限，默认 300000；环境 `MINSHOT` 同义 |

失败验证（不换 root，只把门槛抬高）：

```sh
./harness/gate.sh --root /tmp/root-local.apfs --minshot 10MB --out /tmp/gate-fail   2>&1 | tail -8
```

预期：`GATE: FAIL` + 每项 `GATE: FAIL reason: ...`（screenshot ... `<` ...），`gate.md` 里有同一行。
`gate.sh` 对纯本地 cell1 固定 `REQUIRE_HOST_FRAMES=0`、`ZERO_RAM=1`，所以无 recorder 的 root 也能判 UI；
远端 root 仍用同一命令，host 帧不是 gate 的 PASS 条件（0x502 与截图才是）。

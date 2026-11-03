# harness — 一次冷启动、一次测量、一个结论

这一轮所有数字都是这几个脚本量出来的。它们的共同前提是**同一时间只有一台虚拟机**：
`payload.bin`、`qemu/ram.img` 与端口 9110 是共享的，第二台会把第一台带进
`Ticket spinlock[…] timeout` panic（`docs/gpu-acceleration-plan.md` §0.6-2）。

| 脚本 | 作用 |
|---|---|
| `boot-measure.sh LABEL ROOT.apfs MAX_SECONDS [HOST_ARGS]` | 一次冷启动 + 一个 `host_glwire` + 一对日志。**结束条件就是验收判据本身**：宿主 ≥200 帧且截图 > 300 KB；780 s 还没有 100 KB 的截图就判为没界面（卡在 `lsd` 的启动）直接收工。`HOST_ENV=…` 给宿主传环境（`GLWIRE_DUMP`、`GLWIRE_SAVE_RECORDS`、`GLWIRE_NO_INPLACE`），`XCMD=…` 追加内核启动参数（例 `glwire_nopicture=1`），`QEMU_SMP=…`（默认 4，第六阶段 T0 用它量 `cpus=2`）与 `QEMU_EXTRA=…`（原样加到 QEMU 命令行，例 `-accel tcg,thread=single`） |
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


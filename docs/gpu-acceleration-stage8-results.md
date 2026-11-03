<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第八阶段.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第八阶段结论

> 时间：2026-11-03 20:20–（进行中；数字随队列补齐）
> 本文只写**这一轮量到的数字、每个决定的结论、以及卡在哪一行**；目标与判据看 `IOSVM-GPU加速第八阶段工程.md`。

---

## 零、一句话现状（跑完再定稿）

1. **启动**：`tlto_us=30000000` 下 P0' 队列进行中，第 1 次 **PASS**（ui 420 s、2,435 帧、0 panic）。
2. **画面**：B 路的定位代码已就位（`glslots=1`），等队列结束构建后跑一次定位启动。
3. **停顿**：`stall-boot.sh` 与 `stalls.py` 已就位（P1' 的两组对照等 P0' 之后跑）。

---

## 一、P0' 启动验收（10 次 ≥8 到界面、0 panic）

参数：`IMAGE=/tmp/root-fixed-net.apfs WANT=8 TRIES=12 TAG=m8p0 XCMD="tlto_us=30000000" UI_GRACE=300`，`cpus=4`。

| # | 界面 | 帧 | 结果 |
|---|---|---|---|
| 1 | 420 s（448,537 B） | 2,435 | **PASS**：0 panic |
| 2 | 420 s（448,100 B） | 2,382 | **PASS**：0 panic |
| 3 | 无 | 3 | **FAIL**：8 分钟 panic，**换了一类锁**：`Spinlock[0xfffffff709051f80]`，deadline 251,658,240 ticks（≈4 s），实际等 643,388,703 ticks（≈10.3 s），`current owner: 0 (on cpu -1)` |
| 4 | 420 s（448,358 B） | 2,488 | **PASS**：0 panic |
| 5 | 600 s（447,475 B） | 1,432 | **PASS**：0 panic |
| 6 | 420 s（448,235 B） | 2,063 | **PASS**：0 panic |
| 7 | 600 s（446,957 B） | 111 | **FAIL**：界面出来后 panic，**第三类 deadline**：`waitq(0xfffffff540e58048) lock timeout after 378,467,130 ticks`（≈6.05 s），deadline 240,000,000 ticks（≈3.84 s） |
| 8 | 510 s（448,071 B） | 1,371 | **PASS**：0 panic |

**结果：6 PASS / 8 次（75%），interface seen 7/8（87.5%），2 次 panic。**
「10 次 ≥8」按通过次数算达到 6（不足 8），「0 panic」未达成——两次 panic 分别打穿 `Spinlock`（4.03 s）
与 `waitq`（3.84 s）两族 deadline，而这两族的 deadline 不是 `tlto_us` 能抬的。

第 3 次那次 panic 的栈（`boots` 成员从 panic 块与符号表读出，`caller 0xfffffff0072eb3cc` =
`hw_spin_timeout_panic`）：`sched_timeshare_maintenance_continue → thread_update_process_threads →
hw_lock_lock_contended`，卡在**调度器 timeshare 维护路径上的一把堆锁**（`0xfffffff709051f80` 在 GEN3 zone，
不是静态符号）。同一个停顿既打穿 ticket lock 的 30 s，也打穿 spinlock 的 4 s。

第 7 次是**第三个 deadline**：`waitq` 的锁 3.84 s（240,000,000 ticks），实际等了 6.05 s。
三族锁、三个 deadline（ticket 60e6→30000000 µs、`lock_panic_timeout` 0xf000000≈4 s、waitq 240e6≈3.84 s），
一次 >6 s 的停顿把它们一起打穿——**这就是「0 panic」不可能的原因**，也是 P1' 要量的停顿长度：
至少 6 s、第 3 次那次是 10.3 s。

第 3 次给出的新事实：`tlto_us=30000000` 只抬了 **ticket lock** 的 deadline（60e6→240e6/30 s），
另一族锁（`Spinlock` / `lock_panic_timeout`）的 deadline 仍然是 **≈4 s**（251,658,240 ticks），
一次 >4 s 的停顿照样把它打穿，而且这次 owner 字段是 0、cpu -1（持有者不在了）。
**下一轮的候选动作**：把 `lock_panic_timeout` 一起抬（`locks_arm.c` 的 `MACHINE_TIMEOUT` 默认是 12.5e6，
端口上实际跑的是 0xf000000；设备树/内核一行都能改），或者干脆修停顿本身。

两次界面都出现在 **420 s**（同一参数、同一镜像），说明在 30 s deadline 下界面出现的时刻是稳定的。

---

## 二、P1' 停顿归因

每组一次启动：`ZERO_RAM=<0|1> ./harness/stall-boot.sh m8s1<组><次> /tmp/root-fixed-net.apfs 800`
（脚本会同时起 `hostio.sh` 每秒采样 `/proc/pressure/io` 与磁盘，结束后 `stalls.py` 把停顿和 I/O 放在一根时钟上。）

**A 组第 1 次（`m8s1a1`，22:59–23:12，`ZERO_RAM=1`）**：

```
xb: m8s1a1 end=qemu-exited ui=0s ... guestframes=80 hostframes=2406 ... panic=0 biggest=448459
  帧 1836: 停顿 6.92 s；帧 1992: 13.24 s；帧 2109: 13.34 s；帧 2255: 3.55 s（共 12 次 >1 s）
  io_some: inside stalls 2.2%   outside 9.0%
  io_full: inside stalls 2.0%   outside 8.2%
```

**A 组第 2 次（`m8s1a2`，23:13–23:27，`ZERO_RAM=1`）**：`hostframes=1673`、`panic=0`、`biggest=448762`，
`io_some: inside 7.7% / outside 19.1%`，`io_full: inside 7.1% / outside 17.6%`。

**B 组（`ZERO_RAM=0`，复用上一轮的 `ram.img`，不写 2.4 GB）**：

| 启动 | hostframes | biggest | io_some inside / outside | io_full inside / outside |
|---|---|---|---|---|
| `m8s1b1` | 918 | 448,520 | 16.1% / 26.6% | 14.9% / 24.1% |
| `m8s1b2` | 369 | 448,289 | 24.6% / 27.6% | 22.8% / 25.5% |

**四次的结论一致：停顿窗内的 I/O 压力都比窗外低**（2.2/9.0、7.7/19.1、16.1/26.6、24.6/27.6）——
**停顿不是宿主的 I/O**，`ZERO_RAM` 的 2.4 GB 重写也不是（B 组不写，停顿照旧）。
这一条把 P1' 的靶子从宿主挪到了客户机/QEMU 内部；下一步是 `ustacks` 在停顿窗口采样（第九阶段 P1''）。
（磁盘读写的 inside/outside 有偏差：`hostio.sh` 的第一条采样把绝对值当成了增量，57,111 是计数器本身；
已在脚本里修掉，B 组生效。）

## 三、P2' 相位（换尺子）

一次启动（`m8p2`，R1 镜像、宿主 `--no-panel` + `NO_INPLACE`、客人机 `glwire_nopicture=1`，第 700 秒落盘并
同时落 panel(N+1)）：

```
xb: m8p2 end=qemu-exited ui=0s ui_bytes=0 guestframes=27 hostframes=185 MATCH=0 DIFF=185 panic=0 biggest=448333
  painted: panel 598414 (49.1%), picture 464661 (38.1%)
  within 8 in every channel: 1039591/1218000 (85.35%) -> NOT the same frame (the line is 95%)
```

- **尺子先修了一次**：`--no-panel` 的 picture dump 实际是**顶行在前**（不是 `glReadPixels` 的底行在前），
  翻转着比是 31.26%、按原样比是 **85.35%**；`diffdump.py` 现在两种方向都算、取好的那个并打印用了哪一种
  （11 月 2 日的归档 dump 也支持这个方向：不翻转 100.00%）。
- 修正后仍然 **NOT the same frame**（85.35% < 95%），差的 **11.84%**（144,154 像素，box 覆盖整屏）
  正是缺的层：所有 app 标签、Weather/Calendar widget 的内容、Search 文字，以及 App Store/Maps/Settings/
  Messages/Music 五个图标整块没有；反方向只有 0.85%（10,401 像素）。**不是抗锯齿抖动，是记录流缺层。**
- `picture(N) vs panel(N+1)` 给出**一模一样**的 31.26%：因为落盘那一刻画面是静态的，`panel(N+1) == panel(N)`。
  所以这次**做不出相位结论**——要一帧「正在动」的时刻（滚动中落盘）才有意义，留给第九阶段或 P3' 的驱动窗口。
- 这不是「相位错了」，是尺子和采样时刻的问题；判据仍未达成，如实写在这里。

## 四、P3' 四配置表

（待：`harness/drive-scroll.sh`，触摸在界面之后自己驱动）

## 五、P4' B 路定位

代码已就位（`userland/src/eagl.c`，boot arg `glslots=1`，默认零行为变化）：盯住 12 个「记录表之外、
但可能把像素画下来」的槽位，第一次被调用打一行 `hakios-gl: slot <名字> calls 1`。

候选（都是 `eagl_slots.txt` 里的真实表项，且都不在 `spec.def`）：

| 槽位 | 偏移 | 为什么是候选 |
|---|---|---|
| `glDrawRangeElements` | `0x0cb0` | 索引 draw；`spec.def` 只收了 `glDrawElements` |
| `glDrawArraysInstanced` | `0x17d8` | 实例化 draw（表名 `…EXT`） |
| `glDrawElementsInstanced` | `0x17e0` | 实例化 draw（表名 `…EXT`） |
| `glVertexAttribDivisor` | `0x17d0` | 不是 draw，但实例化 draw 必须先调它——半条证据 |
| `glBlitFramebuffer` | `0x17a0` | 像素搬运 |
| `glCopyTexImage2D` | `0x01b8` | 整块 framebuffer 抄进新纹理 |
| `glClearBufferiv/uiv/fv/fi` | `0x1838/1840/1848/1850` | 清缓冲 |
| `glInvalidateFramebuffer`、`glInvalidateSubFramebuffer` | `0x2000/0x2028` | 丢像素（可能解释「显示面 0 像素」） |

反证（可以直接写进结论）：`glDrawTex*OES`（`0x1fc0–0x1ff8`）、`glResolveMultisampleFramebufferAPPLE`（`0x2018`）、
`glCopyTextureLevelsAPPLE`（`0x2020`）在生成表里是 `eagl_missing`（库没实现，调用会先打 `CALLED AND NOT THERE`），
不可能画出 glyph；`glMultiDrawArrays` / `glMultiDrawElements` / `glDrawArraysIndirect` / `glDrawElementsIndirect`
**不在 Darwin 的 EGL 表里**（`eagl_slots.txt` 没有偏移），透过 `OpenGLES.framework` 到不了。

一次定位启动的结果（`m8disc`，22:45–22:59，`XCMD="tlto_us=30000000 glslots=1 glframes=1"`）：

```
hakios-gl: glslots=1: 12 entries the wire does not carry are counted
xb: m8disc end=qemu-exited ui=0s ... hostframes=1594 ... panic=0 biggest=344538
$ grep -c "hakios-gl: slot" /tmp/m8disc.log
0
```

**1594 帧、界面确实起来了（截图 344,538 B）、0 次 slot 调用。** 也就是：
`glDrawRangeElements`、`glDrawArraysInstanced`、`glDrawElementsInstanced`、`glVertexAttribDivisor`、
`glBlitFramebuffer`、`glCopyTexImage2D`、`glClearBuffer*`、`glInvalidate*` 这 12 个入口**一次都没被调用**。

这条否定结论加上另外两条证据（`glDrawTex*OES` 等是 `eagl_missing`、`glMultiDraw*`/`glDraw*Indirect`
不在 Darwin 表里；`remote_only` 显示面 0 像素），可以写死：**glyph 不走 EGL 表里的任何入口**，
B 路不是「往 `spec.def` 加一行」能解决的——下一阶段要换靶子（CA 的 Metal/CoreGraphics 路径，
或者承认 `remote_only` 只覆盖记录器能到的那一层）。

## 六、这一轮的工具与提交

| 文件 | 用途 |
|---|---|
| `harness/hostio.sh` | 每秒采样 `/proc/pressure/io` 与磁盘（P1'） |
| `harness/stalls.py` | 把宿主帧间隔 >1 s 的停顿与采样放在同一根时钟上（P1'） |
| `harness/stall-boot.sh` | 一次对照启动 + 采样 + 停顿表（P1'） |
| `harness/drive-scroll.sh` | 等界面出现后自己驱动 20 s 滚动（P3'） |
| `harness/diffdump.py --tolerance 8` | 有容差的同帧判据（P2'） |
| 端口 `userland/src/eagl.c` | `glslots=1`：记录表之外的槽位计数与首次调用行（P4'） |

## 七、下一步

（队列跑完后按 §二–§五 补齐；每条结论 = 一行日志 + 一个数字 + 一次可复现的启动参数）

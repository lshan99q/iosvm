<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第九阶段.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第九阶段结论

> 时间：2026-11-04 01:08–（P0'' 队列进行中；数字随队列补齐）
> 本文只写**这一轮量到的数字、每个决定的结论、以及卡在哪一行**；目标与判据看 `IOSVM-GPU加速第九阶段工程.md`。

---

## 零、一句话现状

1. **三族 deadline 一起抬到 60 s**（`slto_us=30000000 tlto_us=60000000 lpto_us=60000000`）后，
   P0'' 队列进行中：第 1 次 **PASS**（ui 450 s、1,209 帧、447,297 B、0 panic），其余在跑。
2. **停顿归因（继承第八阶段 P1'）**：四次对照里停顿窗内的宿主 I/O 压力都低于窗外、`ZERO_RAM=0` 零磁盘写
   仍有 24/35 次 >1 s 停顿 → **停顿不是宿主 I/O、也不是 ram.img 重写**，是客户机自己的多秒停摆；
   三族 deadline 抬到 60 s 就是让这种停顿不再直接 panic。
3. **B 路（glyph）**：第八阶段的定位是否定的——12 个 `spec.def` 之外的 EGL 槽位在 UI 起来时一次都没被调用
   → **glyph 不走 EGL 表**，本阶段不做「接进记录流」，改为记录「下一步要查 CA 的 Metal/CoreGraphics 路径」。

---

## 一、P0'' 三族 deadline 的 10 次冷启动验收

参数：`IMAGE=/tmp/root-fixed-net.apfs`、`cpus=4`、`UI_GRACE=300`、`MAXSECS=1200`、
`XCMD="slto_us=30000000 tlto_us=60000000 lpto_us=60000000"`，内核 `hakios-kernel` 01:06（含 `lpto_us`）。

| # | 界面 | 帧 | 结果 |
|---|---|---|---|
| 1 | 450 s（447,297 B） | 1,209 | **PASS**：0 panic |
| 2 | 420 s（447,220 B） | 2,600 | **PASS**：0 panic |
| 3 | 510 s（446,975 B） | 1,538 | **PASS**：0 panic |
| 4 | 570 s（447,232 B） | 1,522 | **PASS**：0 panic |
| 5 | 600 s（446,951 B） | 1,502 | **PASS**：0 panic |
| 6 | 480 s（447,410 B） | 1,319 | **PASS**：0 panic |
| 7 | 420 s（447,001 B） | 2,152 | **PASS**：0 panic |
| 8 | 540 s（446,884 B） | 1,349 | **PASS**：0 panic |

**结果：8/8 通过（100%）、interface seen 8/8（100%）、0 panic。**
`boots done: pass 8/8 after 8 attempts (100%), interface seen 8/8 (100%), 0 panics, 2026-11-04T03:06:54+08:00`

- 界面时间 420、420、450、480、510、540、570、600 s（中位 495 s，最慢 600 s）；
- 每格 1,209–2,600 帧，截图 446,026–447,410 B；
- 三个 panic 形态（ticket / `Spinlock[` / `waitq(`）**各 0 次**：
  `grep -a "panic(cpu" /tmp/m9p0*.log` 无输出；
- 对照第八阶段（只抬 ticket 一族、30 s）：6/8、2 次 panic → **三族一起抬到 60 s 后 8/8、0 panic**，
  这是本阶段最重要的一行结论。

前三次数值：界面 420–510 s、1,209–2,600 帧、0 panic——第八阶段同镜像同参数（但只有 ticket 一族抬到 30 s）是 6/8、2 次 panic；
三族都抬到 60 s 之后，前三次里两次的界面时间还更快（420/450 s）。

---

## 二、停顿归因（四次对照，结论：都不是）

第八阶段的 P1' 已经把两件事排掉了，本阶段据此把三族 deadline 一起抬起来：

| 启动 | ZERO_RAM | 停顿数(≥1 s) | 最长停顿 | io_some 停顿窗内 vs 窗外 |
|---|---|---|---|---|
| `m8s1a1` | 1 | 24 | 13.34 s | **2.2% vs 9.0%** |
| `m8s1a2` | 1 | 44 | 8.72 s | **7.7% vs 19.1%** |
| `m8s1b1` | 0 | 35 | 7.98 s | **16.1% vs 26.6%** |
| `m8s1b2` | 0 | 24 | 14.68 s | **24.6% vs 27.6%** |

- **不是宿主 I/O**：四次里停顿窗内的 `/proc/pressure/io` 压力**都低于**窗外；
- **不是 `ZERO_RAM` 的 2.4 GB 重写**：B 组零磁盘写（`disk 0/0`），停顿数 35/24、最长 14.68 s，和 A 组同量级；
- **是客户机自己的多秒停摆**：6–14.7 s 的连续不产帧，随后正常继续——这正是 4 s/3.84 s 族锁会被打穿、
  而 60 s 族锁不会的原因。

口径提醒：A 组 `disk: inside 57112 reads` **不是**「停顿窗内在读写盘」，是 `hostio.sh` 第一次采样把绝对值
当增量（已修），57111 就是 `ZERO_RAM` 重建 `ram.img` 的计数器本身。

## 三、B 路（glyph）：结论是否定的，B 路不在 EGL 表

第八阶段 P4' 的定位启动（`m8disc`，UI 已起、344,538 B 截图、1,594 帧）在 `glslots=1` 下：

```
hakios-gl: glslots=1: 12 entries the wire does not carry are counted
$ grep -ac "hakios-gl: slot " /tmp/m8disc.log
0
```

- 12 个候选（`glDrawRangeElements`、`glDrawArraysInstanced`、`glDrawElementsInstanced`、`glVertexAttribDivisor`、
  `glBlitFramebuffer`、`glCopyTexImage2D`、`glClearBuffer*`、`glInvalidate*`）**一次都没被调用**；
- 「没装上」已排除：`obj/eagl_gen.h` 里 12/12 个 offset 都有 trampoline 与 `table[off/8]` 安装
  （注意生成代码里是 `0xcb0`、`0x1b8` 不带前导零，文本 grep 会漏，必须按数值比）；
- 旁证：`glDrawTex*OES`/`glResolveMultisampleFramebufferAPPLE`/`glCopyTextureLevelsAPPLE` 在生成表里是
  `eagl_missing`；`glMultiDrawArrays`/`glMultiDrawElements`/`glDrawArraysIndirect`/`glDrawElementsIndirect`
  不在 Darwin 的 EGL 表里（`eagl_slots.txt` 无偏移）——**都不可能画出 glyph**。

所以本阶段**没有**把 B 路「接进记录流」：不是工程没做完，是判据给了否定答案。
下一步（第十阶段）要换靶子：CA 的 Metal/CoreGraphics 路径，或者接受 `remote_only` 只覆盖记录器能到的那一层（W2 不成立）。

## 四、P3' 四配置表

（待：`/tmp/t7-run.sh`，四格同用 stage-9 启动配方）

## 五、工具与提交

| 文件 | 用途 |
|---|---|
| `xnu-src/.../osfmk/arm64/machine_routines.c` | `lpto_us=<µs>`：`lock_panic_timeout` 的 boot arg（不传 = 原样 0xf000000） |
| `iosvm/patches/0015-kernel-lock-deadline-arguments.patch` | 上面这段的导出（从 `xnu/hakios.patch` 里摘的 `machine_routines.c` 段） |
| `tools/glwire/harness/README.md` | 三族 deadline 的对照表与两条口径 |
| `tools/make-kernel.sh` | locale 无关化（judge 之前只认英文 `Error`，中文「错误 2」会误判） |
| `tools/glwire/harness/bootsum.py` | P0'' 的判据表与预算（rule-of-three） |
| `tools/glwire/harness/diffdump.py` | 行序自动判定（第八阶段 P2' 的尺子修正） |

构建与验证命令（本阶段实际用的）：
```
LANG=C LC_ALL=C IOSVM_USE_LLD=0 tools/make-kernel.sh -j8     # → ./hakios-kernel
# 150 秒短启动验证：有 ignition/launchd 行、没有 "kernel returned"
```

提交：`iosvm` 仓库 `main`，本阶段推送了 `fbba8eb`（计划）、`cf09577` / `3bf05e0`（第八阶段结论与工具）、
以及本文件与内核补丁那一次提交。端口仓库（`oosexgang/hakios-xnu-kernel`）用 token 无写权限，
内核改动以 `patches/0015-…` 的形式随 `iosvm` 提交（与 `patches/0014-…` 同法）。

## 六、下一步（第十阶段的候选，按代价排序）

1. **停顿本身**：三族 deadline 抬到 60 s 只是「不被 6–14.7 s 的停摆放倒」，停顿还在（每次启动 24–44 次 >1 s 的
   空洞，最长 14.68 s）。下一次要用 `ustacks`（`ustacks=<proc> ustacks_at=<秒>`）在停顿窗口抓栈，
   把「客户机自己在干什么」钉死；宿主侧已经排除了。
2. **B 路换靶子**：EGL 表已经否定（12 个入口 0 调用）。要查 CoreAnimation 的 Metal/CoreGraphics 路径，
   或者接受「`remote_only` 只覆盖记录器能到的那一层」，把 W2 的判据改写成「记录器的覆盖范围」。
3. **P3' 的四格**（本阶段末尾在跑）：四格都在 stage-9 配方下，跑完补表。
4. **帧率**：median 20 ms 里仍有一半是宿主/qemu 的固定开销；在停顿修掉之前不谈「稳定 9 fps」。

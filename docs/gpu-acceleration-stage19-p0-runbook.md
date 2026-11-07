<!-- 第十九阶段 P0 的执行手册。不是阶段计划，不替代 docs/gpu-acceleration-stage19-plan.md；两者冲突时以计划为准。 -->

# IOSVM 第十九阶段 P0 运行手册 —— 把「两个字母」变成「五个 App 名称」

> 建立时间：2026-11-07 14:20
> 执行基线：`f2035c6`（`tools(glwire): let boot-measure.sh be pointed at a checkout`）
> 内容基线：`cecbd2e`（`docs(stage 19): the first real glyphs on the screen`，已被 `f2035c6` 包含）
> 适用：离线准备、实验执行、结果复核。**本手册不要求改代码，只要求按同一配方取样并留下证据。**
> 本文只回答四件事：现在到哪了、怎么复现、每条判据怎么取数、结果往哪填。

---

## 零、边界（先读，避免白跑）

1. **P0 的剩余问题不是格式、不是采样，是几何/裁剪的一致性。** 采样链路已经在 `s19n` 打通：
   NaN 的 `texmat0` 换成单位阵后，文字 draw 从 0 像素变成上千像素，截图上出现了真正的字形。
   现在多数 label draw 仍然落 0 像素，因为 **scissor 是 `0,0 178x90` 这类小矩形，而顶点是面板坐标
   `(406,647)` 这类大坐标**——两者不在同一个坐标空间。下一步是逐笔对比「落像素的 draw」与「落 0 像素的 draw」
   的 scissor / viewport / 顶点，不是继续换纹素格式。
2. **P0 还没有通过。** `s19n` 只读出两个字母「Se」，截图 223,027 B（<300 KB），离「≥2 发读出 ≥5 个 App 名称」
   还差得远。本手册的基线复现是**为了拿到可比较的起点**，不是宣布完成。
3. **不要改 `docs/gpu-acceleration-stage19-plan.md`。** 本手册是执行附件；计划里的结论继续由 `iosvm-lead` 维护。
4. **本手册不新增探针、不改镜像，也不在文档任务里跑 qemu。** 实验时只做 §2.2 的宿主编译；需要新探针先回 `iosvm-lead` 定，再进计划。
5. 所有命令都在**实验宿主**上执行；文档任务本身不启动 qemu。

---

## 一、现状一页纸（内容基线 `cecbd2e`）

### 1.1 已经成立的三件事

| # | 事实 | 数据/出处 |
|---|---|---|
| 1 | **文字 draw 已经找到**：`programme 15` 是遮罩文字，纹理是 113x42 / 98x42 / 64x42 的 `GL_ALPHA`，alpha 覆盖正常（1158 个非零里 476 个有 alpha），顶点是标签位置，颜色白，texcoord 是单位正方形 | 计划 §三「16:00 的新认识」；`docs/gpu-acceleration-stage19-plan.md` |
| 2 | **NaN 的 `texmat0` 是挡在采样前的最后一层**：界面阶段 `texmat0` 为 `0x7fffff80`（NaN）；在 draw 前换成 `(1,1,0,0)` 后，`texmat0 was a NaN (programme 15), set to (1,1,0,0)` 打印 5 条 | `cecbd2e` 提交说明；计划 §三「16:24 里程碑」 |
| 3 | **真正的字形已经上屏**：`label draw #70/#72/#74` 分别落 **1799 / 1693 / 866** 个非零像素，截图里出现灰色抗锯齿的「Se」两个字母 | `docs/demo/s19-label-glyphs-20261107.png`（223,027 B）与 `s19-label-glyphs-closeup-20261107.png`（8,768 B），均随 `cecbd2e` 提交 |

### 1.2 还没成立的事：多数 label draw 仍然是 0 像素

`cecbd2e` 的提交说明写得很直白：

> Most draws still land nothing because their scissor (0,0 178x90) and their vertices
> (panel coordinates such as 406,647) disagree; that geometry is the next thing to compare,
> draw by draw.

翻成可执行的判断：

- 一笔 label draw 要落像素，**顶点变换后的屏幕矩形必须与它自己的 scissor 相交**。现在是 `178x90` 的
  scissor 对 `(406,647)` 的顶点，交集为空 → 整笔被裁掉 → `label draw #N ...: 0 nonzero pixels`。
- 少数 draw 恰好落在自己的 scissor 里，于是有了「Se」两个字母；这也解释了为什么字形对了、位置却只对了一小块。
- 因此 **P0 的下一步是坐标空间对齐**（谁把面板坐标写进了 scissor 的局部空间，或反过来），
  不是 `GL_ALPHA` 展开、不是纹素格式、不是 `texmat0` 的数值。

### 1.3 为什么可以排除「格式/采样」

| 被排除的假设 | 反证 | 出处 |
|---|---|---|
| 「图集不是字形」 | `label sample texture 15 113x42 internal 0x1906: 1158 nonzero pixels, 476 with alpha`——alpha 覆盖正常，是字形遮罩 | 计划 §三 16:00 |
| 「纹理坐标还是错的」 | `attrib 2` 是单位正方形 `(0,0)(1,0)(1,1)`；`texmat0` 被替换成单位阵后字形立刻出现 | 计划 §三 16:00、16:24 |
| 「顶点色/透明度把字抹掉了」 | `attrib 1` 是白色 `(1,1,1,1)`；字形可见 | 计划 §三 16:00 |
| 「NaN 是客人机算出来的」 | 客人机 `glUniform4fv/4f` 一次都没有发送 NaN（`s19h` 0 行），NaN 在录制流里 | 计划 §三 s19h |
| 「VBO 偏移当地址」 | 界面阶段 `glVertexAttribPointer` 的 `buffer` 全是 0（`s19g`） | 计划 §八点一 |

**一句话**：纹理、顶点色、UV、texmat 都已经有正解；现在挡住「多数 label」的是 **scissor 与顶点不在同一空间**。
本手册的验收就是看这个修复有没有把「≥5 个 App 名称」稳定送上去。

---

## 二、`s19n` 基线复现配方

### 2.1 路径约定（先核对，别在错的树上跑）

这一阶段有**两棵树**，别把它们当成一棵：

| 变量 | 容器里的路径 | 里面有什么 |
|---|---|---|
| `REPO` | `/host/iosvm` | 对外仓库：`docs/`、`tools/glwire/`（`host_glwire`、`boot-measure.sh`） |
| `KERNEL` | `/host/iosvm-work/hakios-xnu-kernel` | 虚拟机工程：`qemu/`、`userland/`、`root.apfs`、`system.apfs` |

`tools/glwire/harness/boot-measure.sh` 的两行是**默认值**，不是写死（`f2035c6` 起可覆盖）：

```sh
IOSVM=${IOSVM:-/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel}   # KERNEL 的默认值
GLWIRE=${GLWIRE:-/home/lshan/DSH-desktop/iosvm/tools/glwire}            # REPO/tools/glwire 的默认值
```

默认值在宿主机上有效；容器里用 `IOSVM=` / `GLWIRE=` 覆盖。**两者必须指向同一份树，别混用**：
`IOSVM` 是 KERNEL，`GLWIRE` 是 `$REPO/tools/glwire`。脚本会 `cd "$IOSVM"` 再跑 `qemu/run-darwin.sh`，
所以 `root.apfs` 参数请传绝对路径，不要依赖当前目录。

```sh
# 每开一个新 shell 先跑这一块（容器配方）
export REPO=/host/iosvm
export KERNEL=/host/iosvm-work/hakios-xnu-kernel
export GLWIRE=$REPO/tools/glwire
export ROOT=$KERNEL/userland/root.apfs

cd "$REPO"
git rev-parse --short HEAD                     # 期望 f2035c6（含 cecbd2e）
git -C "$KERNEL" rev-parse --short HEAD        # 内核/用户态的提交，一并记进证据表
grep -n '^IOSVM=\|^GLWIRE=' tools/glwire/harness/boot-measure.sh
test -d "$KERNEL/qemu" && test -d "$GLWIRE" && echo paths-ok
ls -l "$ROOT" "$KERNEL/userland/system.apfs"
```

期望：

- `REPO` 的 `git rev-parse --short HEAD` → `f2035c6`（或包含它的后续提交，并在证据表里写明实际提交）；
- `grep` 输出的两行是 `${IOSVM:-...}` / `${GLWIRE:-...}` 默认值；实际启动时由上面的环境变量覆盖；
- `paths-ok`，且 `$KERNEL/qemu`、`$GLWIRE` 都存在；
- `$ROOT` 存在；**`root.apfs` 不在 git 里**（GB 级镜像，`.gitignore` 已排除），每台机器各自构建；
- `$KERNEL/userland/system.apfs` 存在（启动镜像，同样不入库）。

### 2.2 宿主前置检查（每次 boot 前都做）

```sh
cd "$REPO"
# 1) 没有残留的 qemu / host_glwire（脚本也会杀，但先看一眼，别让旧进程吃掉 9110）
ps -eo pid,comm | grep -E 'qemu-system|host_glwire' || echo "clean"

# 2) 宿主二进制比源码新，且带本阶段的开关
make -C tools/glwire host_glwire
stat -c '%y %s %n' tools/glwire/host_glwire tools/glwire/host_glwire.c
strings tools/glwire/host_glwire | grep -c GLWIRE_TEXMAT_NAN_TO_IDENTITY   # 期望 >=1

# 3) 显卡空闲（被别的任务占满时启动会更慢，甚至把 boot 拖过 NOUI_BY）
nvidia-smi --query-gpu=utilization.gpu,memory.used --format=csv,noheader
```

判定：

- **残留进程**：`ps` 无输出才算干净。有输出先 `kill -9`，等 1 秒再开。
- **二进制 mtime**：`host_glwire` 必须**晚于** `host_glwire.c`（含 `wire.h`/`names.h`/`dispatch.inc`/`encode.inc`）。
  如果 `make` 报 `Text file busy`，说明还有 qemu 在跑，**不要忽略**——这正是 `s19j` 读到旧二进制的坑。
- **开关存在**：`strings ... | grep -c` 必须 ≥1。0 表示跑的是旧二进制，本发无效。
- **GPU**：利用率接近 0%、显存占用接近 0 才开；hashcat 等任务期间只做离线分析。

### 2.3 启动命令（基线复现，严格照抄）

```sh
cd "$REPO"

ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300 \
HOST_ENV="GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=/tmp/s19p0-1/label GLWIRE_DRAW_LOG=1" \
IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
tools/glwire/harness/boot-measure.sh s19p0-1 "$ROOT" 1500
```

参数逐条解释（少一个都会让结论变形）：

| 参数 | 作用 | 少了会怎样 |
|---|---|---|
| `ZERO_RAM=1` | 每次 boot 前删 `qemu/ram.img`，让客人机从零启动 | 第一张截图可能是上一轮的残留主屏，`ui at` 会假早 |
| `QEMU_SMP=1` | 单 vCPU，去掉 MTTCG 的 ticket-spinlock panic | 4 vCPU 下 6 发 5 崩，取样没有意义 |
| `NOUI_BY=1300` | 1300 s 还没有界面才判 `noui` | 默认 780 s 会把 11–15 分钟的慢启动直接掐掉（`s19f end=noui`） |
| `MINFRAMES=99999` | 关掉「200 帧即验收」的提前收工 | 到不了 UI 就 `acceptance` 结束 |
| `MAXSECS=1500` | 记下本发的时间预算 | **注意：脚本真正的上限是第三个位置参数 `1500`；环境变量会被 `${3:-1800}` 覆盖**，写在这里只是让整行自解释 |
| `UI_GRACE=300` | 界面出现后再活 300 s，`end=ui` 才算 PASS | 没有宽限期，界面刚出就收工，看不到稳定性 |
| `HOST_ENV` 里的 `GLWIRE_TEXMAT_NAN_TO_IDENTITY=1` | 在 label/文字 draw 前把 NaN 的 `texmat0` 换成 `(1,1,0,0)` | `texmat0` 仍是 NaN，采样未定义，字形不出现 |
| `HOST_ENV` 里的 `GLWIRE_LABEL_DUMP=<prefix>` | 开启 label draw 的像素读回；**只有它存在时才会打印 `label draw #N ... nonzero pixels`**（C5 依赖这一行） | C5 拿不到数据；前 4 笔还会写 `<prefix>-label-*.bgra` |
| `HOST_ENV` 里的 `GLWIRE_DRAW_LOG=1` | 打印每笔 `draw#N ... viewport/scissor`，是下一发 landed-vs-zero 对照的原始数据 | 只有 label 汇总，没有逐笔几何 |
| `IOSVM="$KERNEL" GLWIRE="$GLWIRE"` | 把 harness 指到容器里的两棵树 | 会回落到宿主默认路径，容器里通常直接失败 |
| 第三个位置参数 `1500` | 本发硬上限（秒） | 见 `MAXSECS` |

**运行时长预期**：界面在 10–15 分钟到；`UI_GRACE=300` 后结束，通常落在 900–1300 s。
每发之间留冷却，确认没有残留 qemu 再开下一发；连续跑时宿主 I/O 会拖慢客人机。

### 2.4 验收发：再加同帧 dump

P0 的第三条判据（`diffdump --tolerance 8`）要求同一发的 panel 与 picture。在基线命令的 `HOST_ENV`
里加上 dump 前缀和取样时刻：

```sh
cd "$REPO"
mkdir -p /tmp/s19p0-1
ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300 \
HOST_ENV="GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=/tmp/s19p0-1/label GLWIRE_DRAW_LOG=1 GLWIRE_DUMP=/tmp/s19p0-1/f GLWIRE_DUMP_AT=180" \
IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
tools/glwire/harness/boot-measure.sh s19p0-1 "$ROOT" 1500
```

- 四个探针**可以共存**：同一条 `HOST_ENV` 里用空格分隔即可，上面的命令已经这样写。
  `GLWIRE_LABEL_DUMP` 负责 `label draw` 行，`GLWIRE_DRAW_LOG` 负责逐笔 `draw#`，`GLWIRE_DUMP` 负责同帧 pair。
- `GLWIRE_DUMP=<prefix>`：写一次 `<prefix>-panel-WxH.rgba` 与 `<prefix>-picture-WxH.bgra`；
  前缀目录必须先存在（`host_glwire` 只 `fopen`，不建目录），所以上面先 `mkdir -p`；
- `GLWIRE_DUMP_AT=180`：**第一帧之后 180 s** 取这一对。这样通常已经过了「Se 刚出现」的时刻，
  又不会拖到 1500 s 上限之外；界面很快时也不会取到纯黑的首帧。
- 如果这一对是黑的/明显不到界面：把 `GLWIRE_DUMP_AT` 加大（例如 `480`）重跑一发，并在证据表里记下实际值。
- 判定 dump 是否发生在字形之后：看宿主日志里 `wrote ...-picture` 的行号是否大于第一条
  `nonzero pixels`（非零 label draw）的行号：

  ```sh
  grep -n 'nonzero pixels' /tmp/s19p0-1-host.log | grep -v ': 0 nonzero' | head
  grep -n 'wrote .*f-picture' /tmp/s19p0-1-host.log
  ```

  若 `wrote` 的行号更小，说明 dump 早于字形，这一发的 diff 不算数。

**`GLWIRE_DRAW_LOG` 的 20000 笔上限（重要）**

`host_glwire` 只记录前 **20000 笔** draw（`logged >= 20000` 就停），这个上限按**笔数**算，不按 dump 字节算。
界面阶段的 draw 量很大（programme 17 一项就 7316 笔），所以：

- 到界面时 `draw#` 很可能已经刷满；**刷满之后不再有 `draw#` 行，但 `label draw` 行仍在**（两条打印路径独立）。
- 大 dump 本身不“挤占”上限，但 dump 取得越晚，越可能已经超过 20000 笔。若要在同一发做 landed-vs-zero 的逐笔对照，
  必须在 `draw#` 仍有输出时取样；否则该发的逐笔对照记为“不可用”，不要硬凑。
- 检查是否刷满：

  ```sh
  grep -c '^glwire: draw#' /tmp/s19p0-1-host.log    # 20000 表示已到上限
  grep -n 'wrote .*f-picture' /tmp/s19p0-1-host.log
  ```

  若 `draw#` 在 dump 之前就停了，在 §4.2 备注里写明「draw# capped at 20000 before dump」，
  C5 的 label 计数不受影响，但下一发的几何对照要换取样窗口。

### 2.5 三发怎么排（可直接执行的循环）

```sh
cd "$REPO"
for n in 1 2 3; do
  echo "== s19p0-$n $(date -Is)"
  ps -eo pid,comm | grep -E 'qemu-system|host_glwire' && { echo "leftover; stop"; break; }
  mkdir -p /tmp/s19p0-${n}
  ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300 \
  HOST_ENV="GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=/tmp/s19p0-${n}/label GLWIRE_DRAW_LOG=1 GLWIRE_DUMP=/tmp/s19p0-${n}/f GLWIRE_DUMP_AT=180" \
  IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
  tools/glwire/harness/boot-measure.sh s19p0-${n} "$ROOT" 1500
  grep -E "^xb: s19p0-${n} end=" /tmp/s19p0-${n}.log
  sleep 60   # 让宿主的 I/O 落一落；确认没有残留 qemu 再开下一发
done
```

- 单发失败**不要**在同一轮里改代码重跑；先把失败原因记下来，三发跑完再决定。
- 三发之间不要同时跑 `make`、不要开第二个 qemu；端口 9110 与 `ram.img` 是共享的。
- 每发结束后先把 `/tmp/s19p0-${n}.log`、`-host.log`、`-shot.png` 复制到本轮的证据目录，
  再跑下一发（`/tmp` 会被下一次启动清掉同名文件）。

### 2.6 本发会在 `/tmp` 留下什么

| 文件 | 内容 |
|---|---|
| `/tmp/<label>.log` | 客人机串口日志 + 最后一行 `xb: <label> end=...` |
| `/tmp/<label>-host.log` | `host_glwire` 日志（build marker、label draw、NaN 替换、dump 行） |
| `/tmp/<label>-shot.png` | **最大**的一张截图（字节数判据用它） |
| `/tmp/<label>-last.png` | 最后一次轮询的截图 |
| `/tmp/<label>-final.png` | 收工前最后一张截图（读 App 名称优先用它） |
| `/tmp/<label>-crash.png` | 仅崩溃时存在 |
| `/tmp/<label>/label-*.bgra` | `GLWIRE_LABEL_DUMP` 写的前 4 笔 label draw 像素 |
| `/tmp/<label>/f-panel-*.rgba`、`f-picture-*.bgra` | 仅验收发，`diffdump` 的输入 |

---

## 三、P0 验收 checklist（照 §二 P0 逐条取数）

> 判据原文（计划 §二）：**连续 3 发（`QEMU_SMP=1`）里 ≥2 发首页能读出 ≥5 个 App 名称；
> 截图 ≥300 KB；同帧 `diffdump --tolerance 8` ≥95%。**
> 下面每条都给出「怎么取数 / 看哪一行 / 什么算通过」。三项全过才算 P0 通过；任一项不过，P0 不通过。

### C0 三发都是同一配方、同一提交

**怎么取数**

```sh
for n in 1 2 3; do
  echo "== s19p0-$n"
  grep -m1 '^===== ' /tmp/s19p0-$n.log
  grep -E "^xb: s19p0-${n} end=" /tmp/s19p0-$n.log
done
```

**看哪一行**：每发第一行的 `===== <时间> <label> root=... hostargs=[...]` 和最后一行的 `xb: ... end=...`。

**通过标准**：

- 三发的 `hostargs` 都含 `GLWIRE_TEXMAT_NAN_TO_IDENTITY`；
- 三发的 `root=` 指向同一个 `$ROOT`（记录 `stat` 的 size+mtime）；
- 三发的 `git rev-parse --short HEAD` 相同（或明确记录差异；HEAD 记在 §4.2 元数据，不在日志里）；
- 三发的 `end=ui`（`end=crash` / `end=noui` / `end=noframes` 都算这一发 FAIL）。

### C1 连续 3 发、`QEMU_SMP=1`

**怎么取数**

```sh
# 启动后另开一个终端，确认客人机真的只有 1 个 vCPU
ps -eo args | grep '[q]emu-system' | head -1 | grep -o 'cpus=[0-9]*'   # 期望 cpus=1

# 收工后从本发日志取稳定性
grep -c 'panic(' /tmp/s19p0-1.log
grep -c 'exited due to SIGSEGV' /tmp/s19p0-1.log
grep -c 'usertrap:' /tmp/s19p0-1.log
```

**看哪一行**：`xb: ... end=ui ... SIGSEGV=0 usertrap=0 panic=0 biggest=...`。
`QEMU_SMP=1` 本身以 C0 记录的命令行为准；`ps` 只是运行时的旁证。

**通过标准**：

- `end=ui`；
- `panic=0`、`SIGSEGV=0`、`usertrap=0`；
- 三发都满足，才算「连续 3 发」。

### C2 ≥2 发首页能读出 ≥5 个 App 名称

**怎么取数**

1. 打开 `/tmp/s19p0-<n>-final.png`（收工前最后一张）；若它不到 300 KB，再看 `/tmp/s19p0-<n>-shot.png`。
2. 逐张按肉眼数：能明确读出的 App 名称个数（例如「设置」「地图」「App Store」「时钟」「相机」……）。
3. 把读出的名字逐个列进证据表备注；**不要只写数字**，否则复核时无法判断是不是把图标形状当成了字。

**看哪一行**：没有日志行；这是人工判据。辅助证据是 `label draw #N ...: <非零> nonzero pixels`。

**通过标准**：

- 单发 ≥5 个**可读**名称（不是「有字形」「有标签块」）；
- 三发里 ≥2 发满足；
- P0 才算这一条通过。

### C3 截图 ≥300 KB

**怎么取数**

```sh
for n in 1 2 3; do stat -c '%s %n' /tmp/s19p0-$n-shot.png; done
```

**看哪一行**：`xb: ... biggest=<字节>`，以及 `stat` 的字节数。

**通过标准**：

- 每发**最大截图** `-shot.png` ≥ **300,000 字节**（是字节，不是 300 KiB；脚本的 `MINSHOT` 默认就是 `300000`）；
- 3 发都记录字节数；**计入 P0 的那 2 发必须都 ≥300,000 B**。未计入的发也照实记录（诊断用，不隐藏）。

### C4 同帧 `diffdump --tolerance 8` ≥95%

**怎么取数**

```sh
cd "$REPO"
n=1   # 依次改成 2、3
ls -l /tmp/s19p0-${n}/f-panel-*.rgba /tmp/s19p0-${n}/f-picture-*.bgra
python3 tools/glwire/harness/diffdump.py /tmp/s19p0-${n}/f \
  -o /tmp/s19p0-${n}/diff --tolerance 8
```

**看哪一行**：输出末尾附近

```
within 8 in every channel: <N>/<total> (<share>%) -> SAME FRAME / NOT the same frame
```

**通过标准**：

- `share >= 95.00%` 且结论是 `SAME FRAME`；
- dump 确实发生在界面/字形之后（§2.4 的行号检查）；
- 3 发各算一次；**计入 P0 的那 2 发必须都 ≥95%**。缺 dump 的发记为 `n/a`，
  不能让另一发的 diff 顶替；若 3 发只有 1 发有有效 dump，P0 不通过。

### C5 辅助证据：label draw 非零计数

**怎么取数**

```sh
n=1   # 依次改成 2、3
nz=/tmp/s19p0-${n}-nz.txt
grep -o '[0-9]\+ nonzero pixels' /tmp/s19p0-${n}-host.log | awk '{print $1}' > "$nz"
echo "draws: $(wc -l < "$nz")"
echo "nonzero draws: $(awk '$1>0' "$nz" | wc -l)"
echo "max: $(sort -n "$nz" | tail -1)"
echo "sum: $(awk '{s+=$1} END{print s+0}' "$nz")"
```

**看哪一行**：`glwire: label draw #<N> framebuffer 4 attachment texture 8 750x1624: <nonzero> nonzero pixels, <alpha> with alpha, glerror 0x0`。
这一行**只在 `HOST_ENV` 含 `GLWIRE_LABEL_DUMP` 时打印**（`host_glwire.c` 的 `if (getenv("GLWIRE_LABEL_DUMP") ...)`），
所以计数为 0 时先回去确认这个变量，而不是先怀疑没有 label draw。

**通过标准**：

- 只作诊断，不单独判定 P0；
- 底线：**至少 1 笔 >0**；若全部为 0，P0 必失败；
- 记录 `nonzero draws / total draws / max / sum` 四个数，方便比较「几何修复前后」的变化。

### C6 辅助证据：NaN→单位阵确实生效

**怎么取数**

```sh
n=1   # 依次改成 2、3
grep -c 'texmat0 was a NaN' /tmp/s19p0-${n}-host.log
grep -c 'label state program 15 texmat0 nan' /tmp/s19p0-${n}-host.log
grep -m1 'build marker' /tmp/s19p0-${n}-host.log
```

**看哪一行**：

```
glwire: texmat0 was a NaN (programme 15), set to (1,1,0,0)
host_glwire: build marker uniform-ring-v2
```

**通过标准**：

- `build marker` 是当前二进制的标记（当前为 `uniform-ring-v2`），且 `strings` 能查到开关；
- `texmat0 was a NaN` ≥1 条，说明替换实际触发；
- **注意打印上限是 5 条**（源码 `said_nan < 5`）：第 6 次以后不再打印，但替换仍在每次 NaN 时发生。
  所以这一列记「日志条数（上限 5）」，不要当成真实替换次数。
- 若某发 0 条但字形正常，说明那一发没有 NaN，不判失败；在表里注明。

### C7 结论行

每发末尾都有一行总账，复制进证据表：

```
xb: s19p0-1 end=ui ui=...s ui_bytes=... guestframes=... hostframes=... MATCH=... DIFF=... SIGSEGV=0 usertrap=0 panic=0 biggest=...
```

**通过标准**：`end=ui`、`panic=0`、`SIGSEGV=0`、`usertrap=0`、`biggest>=300000`；
再叠加 C2、C4 的人工/数值结论。**计入 P0 的必须是同样的 2 发**同时满足 C2、C3、C4——
不能拿第 1 发的名称数、第 2 发的截图、第 3 发的 diff 拼一个「通过」。三发里 ≥2 发各自全过 → P0 通过。

---

## 四、证据表模板（每发一行，直接填）

### 4.1 主表

| label | 时间（起–止） | end= | 截图字节 | label draw 非零（笔/最大/合计） | NaN→单位阵（日志条数，≤5） | 肉眼读出 App 名称数 | diffdump ≥95% | 结论 |
|---|---|---:|---:|---|---:|---:|---|---|
| **A1** `s19n-repro` | 2026-11-07 14:26:12–14:52:02Z | qemu-exited | 245,842 | 56 / 1,218,000 / 63,406,544（文字层 0） | 5（prog13×3、prog15×2） | 0 | n/a（无 dump） | **FAIL（P0 未过；视觉里程碑复现）** |
| s19p0-1 | 2026-11-07 __:__–__:__ | ui |  | __ / __ / __ |  |  | __% (SAME/NOT) | PASS/FAIL |
| s19p0-2 |  |  |  |  |  |  |  |  |
| s19p0-3 |  |  |  |  |  |  |  |  |

**结论写法**：`PASS`（该发 C1–C4 全过）/ `FAIL: <哪一条>`；不要写「基本通过」。
**P0 总结**：三发里 ≥2 发 `PASS` → P0 通过；否则写清「差在哪一条、下一发改什么」。
**A1 行**：`s19n-repro` 是基线复现，**不参与** P0 的「连续 3 发」计数；它的判读见 §4.4。
**A1 非零口径**：表里保留**逐行 56 行**；exp 的「8 条」是按 **4 个 138x138 批次 × 2 类**归并的说法，不是另一组日志。
另有 8 行 `label sample texture` 读数（221 / 636 / 912 / 1108 / 8526 / 12700 / 14466 / 15717），它们**不是 label draw**，不并入 56。

### 4.2 每发附带的元数据（贴在主表下面或单独一段）

```text
label:            s19p0-1
git HEAD:         REPO f2035c6 / KERNEL <hash>
host_glwire:      <mtime> <size>  build marker: uniform-ring-v2
root.apfs:        <size> <mtime>   （sha256 可选，GB 级会慢）
QEMU_SMP:         1
配方:             ZERO_RAM=1 NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300
HOST_ENV:         GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=... GLWIRE_DRAW_LOG=1 GLWIRE_DUMP=... GLWIRE_DUMP_AT=...
draw# 行数:       __（20000 表示已封顶；是否在 dump 之前封顶：是/否）
ui at:            <秒> / ui_bytes: <字节>
截图:             -shot.png <字节>；-final.png <字节>；判读用 <哪一个>
label draws:      total=__ nonzero=__ max=__ sum=__
NaN 替换:         __ 条（上限 5）
diffdump:         within 8 = __% -> SAME/NOT；dump 行号 __ vs 首个非零 label 行号 __
读出的名称:       设置 / 地图 / ...
备注:             崩溃、慢启动、GPU 被占、dump 太早等
```

### 4.3 失败分支（写进备注，不要静默重跑）

| 症状 | 先看 | 处理 |
|---|---|---|
| `end=noui` | `/tmp/<label>.log` 最后 50 行；`xb: ... ui at` | 确认 `NOUI_BY=1300`；确认没有残留 qemu；慢启动就重跑，不缩短预算 |
| `end=crash` | `panic(` / `SIGSEGV` / `usertrap:` 行；`/tmp/<label>-crash.png` | 记下 panic 的锁与帧号；不要在同一发里反复改代码 |
| `end=acceptance` 但没到 UI | `hostframes` 是否很早到 200 | 说明 `MINFRAMES=99999` 没生效；检查命令是否被 shell 换行拆坏 |
| 截图 <300 KB 但界面在 | `-shot.png` vs `-final.png`；是否纯黑/半黑 | 先确认不是 `ram.img` 残留；再看 label draw 是否全 0 |
| `label draw` 全 0 | 同一发 `texmat0 was a NaN` 是否为 0 | 若 0，检查 `HOST_ENV` 是否传进；若非 0，记录，交给几何修复那一轮 |
| diffdump 报 `NOT the same frame` | 行号对比（§2.4）；panel/picture 是否同帧 | dump 太早就调大 `GLWIRE_DUMP_AT` 重跑；不要用不同发的图拼 |
| `make` 报 `Text file busy` | `ps` 里的 qemu | 先杀 qemu 再 `make`；否则跑的是旧二进制（`s19j` 教训） |
| `draw#` 只有 20000 条 / dump 后没有 `draw#` | `grep -c '^glwire: draw#'`、`wrote` 的行号 | 该发逐笔对照记「不可用」；label 计数仍有效，下一发换更早的取样窗口 |

### 4.4 A1 实测与判读：`s19n-repro`（归档 `logs/s19n-repro/`）

> 口径：本节所有数字只从 `/host/iosvm/logs/s19n-repro/` 的一组归档读出（`s19n-repro-guest.log`、`s19n-repro-host.log`、`s19n-repro-meta.txt`、`s19n-repro-shot.png`、`s19n-repro-last.png`、`s19n-repro-boot-measure.sh` 快照、`label/`）。**这些日志与截图都不进 git**。
> 证据图**不提交**：s19n 的「Se」图已经随 `cecbd2e` 入库，这张重复且不是 P0 通过证据；`docs/demo/s19n-repro-home-20261107.png` 只作为归档路径记录。

**跑的是什么**

- label：`s19n-repro`；时间：**2026-11-07 14:26:12Z–14:52:02Z**（`xb` 收尾行）。
- 镜像：`root.apfs` 2026-11-07 05:18:12Z，5,902,336 B。
- 宿主二进制：`host_glwire` 2026-11-07 07:59:05Z，105,112 B，`build marker uniform-ring-v2`。
- 容器特例（**都不在仓库里**）：
  - `LD_LIBRARY_PATH=/tmp/glvnd/usr/lib/x86_64-linux-gnu`（GLVND 从 `/tmp` 取，仓库不带）；
  - UEFI 用 `$KERNEL/qemu/efi64.fd`（md5 `d105af9c…`）与 `$KERNEL/qemu/vars.fd`（本次 md5 `d90bea1f…`）；
    第一次没给固件时直接失败：`run.sh: no UEFI firmware found ...; set QEMU_EFI and QEMU_VARS`。
- 归档快照脚本 md5：`dfb778bbf30749e6020f030381573f7c`；它已支持 `IOSVM`/`GLWIRE` 覆盖。复原命令：

```sh
export LD_LIBRARY_PATH=/tmp/glvnd/usr/lib/x86_64-linux-gnu
export QEMU_EFI="$KERNEL/qemu/efi64.fd" QEMU_VARS="$KERNEL/qemu/vars.fd"
cd "$REPO"
ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300 \
HOST_ENV="LD_LIBRARY_PATH=/tmp/glvnd/usr/lib/x86_64-linux-gnu GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=/tmp/s19n/label GLWIRE_DRAW_LOG=1" \
IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
tools/glwire/harness/boot-measure.sh s19n-repro "$ROOT" 1500
```

**归档数字**

| 项 | 值 | 出处 / 读法 |
|---|---|---|
| `end=` | `qemu-exited` | `xb: s19n-repro end=qemu-exited ...` |
| `ui=` / `ui_bytes=` | `0s` / `0` | 截图 245,842 B < `MINSHOT=300000`，所以没被计为界面 |
| 最大截图 | **245,842 B**，1125x2436 | `s19n-repro-shot.png` |
| 宿主帧 | **7,913** | `hostframes=7913` |
| panic / SIGSEGV / usertrap | **0 / 0 / 0** | `xb` 收尾行 |
| `texmat0 was a NaN` | **5 条**：programme 13 ×3、programme 15 ×2 | `s19n-repro-host.log` |
| 文字层 label draw | 750x200 **435 条**、64x19 **585 条**、64x9 **445 条**，**全部 0 非零像素** | 同上 |
| 非零 label draw | **56 行**，合计 63,406,544 像素：4 行 138x138（每行 19,044）、4 行 750x1624（每行 1,216,592）、48 行 750x1624（每行 1,218,000） | 逐行统计；按 4 个 138x138 批次 × 2 类归并则为 exp 的「8 项」；**都不是字形** |
| `label sample texture` | 8 行读数：221 / 636 / 912 / 1108 / 8526 / 12700 / 14466 / 15717 | **不是 label draw**，不并入上面的 56 |
| `diffdump` | `n/a` | 本发 `HOST_ENV` 没有 `GLWIRE_DUMP`，没有同帧 panel/picture pair |
| 肉眼 App 名称 | **0 个** | 只有底部「Se」两个灰色字母，见下 |

**判读（必须照这个口径写）**

1. **`end=qemu-exited` 不是崩溃，是 1500 s 到点。** 日志里紧挨着的是
   `qemu-system-aarch64: terminating on signal 15 from pid 3247 (timeout)`；`panic=0`、`SIGSEGV=0`、`usertrap=0`。
2. **`ui=0` 不是没到界面，是最大截图 245,842 B 小于 300 KB 的门槛**，所以 `ui_at` 没被置位、`ui_bytes=0`。
   截图本身有内容：顶部红条的白字完整可读（`rdar:…`），底部「Search」位置能辨出灰色「Se」。
3. **P0 不通过，A1 只算「视觉里程碑复现」。** 没有 ≥5 个可读 App 名称、截图 <300 KB、没有同帧 diff；
   这三条都不满足，不能因为「有 Se」就写成 P0 通过。
4. **机械判据与主线一致**：文字层三类 label draw 全 0；56 条非零全部是 138x138 整块与 750x1624 全屏合成，
   不是字形。也就是说，字形能出现（`texmat0` 置单位阵后），但**绝大多数文字 draw 仍然被几何/裁剪挡掉**。
5. **宿主 `attrib 3` 的 20 行是早期 draw，不是 UI 时刻。** `host_attrib_state()` 只打印前 20 次调用；
   归档里这 20 行落在 frame 8–17，**前 12 行是 `4.7969 / nan / 1.19e29` 一类垃圾值，后 8 行已是
   `0.1250..0.5000`，均早于 UI**。**它们不能代表 UI 时刻的 tc0**；UI 时刻的 tc0 由 s19o 的
   `GLWIRE_TEXT_DRAW_LINE` 回答，本文不预设结论。

---

## 五、铁律与提交

### 5.1 仓库里不许出现的东西

1. **Apple 二进制 / 镜像 / 固件**：`root.apfs`、`system.apfs`、`ram.img`、`*.ipsw`、`payload.bin`、
   `esp.img` 一律不入库。它们由各机器本地构建/提供，仓库只放**能重建它们的脚本与说明**。
2. **凭证**：Apple ID、token、序列号、私钥、代理口令，截图和日志里都不许出现；`docs/demo/NOTES.template.md`
   明确要求截图不含个人信息。
3. **二进制只有一种例外**：`docs/demo/` 下的 `*.png` / `*.gif` 证据图（见 `docs/demo/README.md`）。
   其它二进制（`.apfs`、`.img`、`.bin`、`.records`、`.bgra`、`.rgba`、`.log`）都不进 git。

### 5.2 证据图怎么放

```sh
cd "$REPO"
mkdir -p docs/demo
cp /tmp/s19p0-1-final.png docs/demo/s19p0-1-home-20261107.png
ls -l docs/demo/s19p0-1-home-20261107.png
```

- 文件名带阶段、发次、日期：`s19p0-<n>-home-<YYYYMMDD>.png`；
- 只提交**真正用于判读**的那张；`-last/-final/-shot` 三张不要一起塞进去；
- 每张图必须在 `docs/demo/NOTES.md` 里有**一条**对应记录（不是只写文件名）：

```markdown
## s19 P0 run 1 — the home screen after the NaN-to-identity fix

- **Milestone**: stage 19 P0
- **Commit**: <本发实际 HEAD>
- **Guest firmware**: <Darwin 版本 / IPSW / 设备标识>
- **Host**: <发行版、CPU、GPU、驱动、QEMU TCG>
- **Command**: `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300 HOST_ENV="GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=... GLWIRE_DRAW_LOG=1 GLWIRE_DUMP=... GLWIRE_DUMP_AT=..." IOSVM="$KERNEL" GLWIRE="$GLWIRE" tools/glwire/harness/boot-measure.sh s19p0-1 <ROOT 的绝对路径> 1500`
- **What it shows**: <读出哪几个 App 名称；标签是不是文字>
- **What it does not show**: <例如「只有 3 个名称可读，其余仍是 0 像素 draw」>
- **Reproduced**: <这一配置跑了几发，几发相同>
```

### 5.3 提交前检查

```sh
git status --short
git diff --stat
git add docs/gpu-acceleration-stage19-p0-runbook.md docs/demo/NOTES.md docs/demo/<证据图>.png
git diff --cached --name-only        # 只能有 .md 与 docs/demo/*.png
```

- 只 `git add` 本手册、`docs/demo/*.png`、`docs/demo/NOTES.md`（上面一行按实际文件名改）；
- **不要** `git add -A`（会把 `userland/root.apfs`、`/tmp` 软链、`.scion/` 一起带进去）；
- 若只写文档、没有实验图，提交信息只描述文档，不要声称 P0 通过。

建议提交信息（有图时）：

```text
docs(stage 19): a P0 runbook and the first evidence run
```

（没有图时：`docs(stage 19): a P0 runbook for the geometry evidence`。）

---

## 六、给 `iosvm-lead` 的回执格式

每次实验结束，把下面这段填好发回，不要只发「跑完了」：

```text
s19p0-<n>: end=<ui/crash/noui> ui=<秒>s shot=<字节> label_nonzero=<笔>/<总> max=<像素>
NaN->I=<条> diff=<百分比/SAME|NOT> names=<个数: ...> => <PASS|FAIL: 原因>
```

三发汇总：

```text
P0: <2/3 或 3/3> PASS；阻塞：<几何/scissor 的哪一处>；下一发：<具体改动或实验>
```

---

## 附录 A：本手册引用的数字出处

| 数字/结论 | 出处 |
|---|---|
| `label draw #70/#72/#74` = 1799/1693/866 非零像素 | `cecbd2e` 提交说明；`docs/gpu-acceleration-stage19-plan.md` §三「16:24 里程碑」 |
| 截图 223,027 B / 8,768 B（「Se」与放大图） | `git show --stat cecbd2e`：`docs/demo/s19-label-glyphs-20261107.png`、`s19-label-glyphs-closeup-20261107.png` |
| `texmat0 was a NaN` 打印 5 条、替换为 `(1,1,0,0)` | `tools/glwire/host_glwire.c` 的 `GLWIRE_TEXMAT_NAN_TO_IDENTITY` 分支（`said_nan < 5`） |
| `programme 15` 是文字 draw；113x42 / 1158 非零 / 476 alpha | 计划 §三「16:00 的新认识」 |
| scissor `0,0 178x90` vs 顶点 `(406,647)` | `cecbd2e` 提交说明；计划 §三「16:24 里程碑」 |
| `NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300` 的理由 | 计划 §三「运行经验」、§八「恢复时要接的位置」 |
| `ZERO_RAM=1` 的必要性（ram.img 残留） | `tools/glwire/harness/README.md` §七 |
| `end=ui` / `ui_bytes` / `biggest` 的定义 | `tools/glwire/harness/boot-measure.sh` 末尾汇总行 |
| `IOSVM=` / `GLWIRE=` 可在容器里覆盖 | `f2035c6`（`boot-measure.sh` 第 31/32 行） |
| `GLWIRE_DRAW_LOG` 的前 20000 笔上限 | `tools/glwire/host_glwire.c` 的 `logged >= 20000` |
| `label draw` 行依赖 `GLWIRE_LABEL_DUMP` | `tools/glwire/host_glwire.c` 的 `if (getenv("GLWIRE_LABEL_DUMP") != NULL && last_draw_was_glyph)` |
| `diffdump --tolerance 8` 的 95% 线 | `tools/glwire/harness/diffdump.py` 的 `SAME FRAME` 分支；计划 §二 P0 |
| 证据图规则（只许 PNG/GIF、每张配 NOTES） | `docs/demo/README.md`、`docs/demo/NOTES.template.md`、`PROVENANCE.md` |

## 附录 B：本次不做什么

1. 不改 `docs/gpu-acceleration-stage19-plan.md`（避免和后续结论冲突）。
2. 不新增探针、不改 `host_glwire.c`、不重建 `root.apfs`（几何修复是另一条工作流）。
3. 不在文档任务里跑 qemu；本手册的所有命令都交给 `iosvm-exp` 执行。
4. 不提交 `root.apfs` / `system.apfs` / `ram.img` / 任何日志或 `.bgra` dump。
5. 不把「有字形」当「P0 通过」：通过线是 **≥2 发读出 ≥5 个 App 名称 + 截图 ≥300 KB + 同帧 diff ≥95%**。

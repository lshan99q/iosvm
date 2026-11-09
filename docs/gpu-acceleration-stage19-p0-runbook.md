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

### 2.4 C4 验收发：local composition + no-panel replay + 同帧 dump

> **⚠ C4 的配方前提（2026-11-08 v6 教训）**：`diffdump` 只有在 **guest 本地合成**（`XCMD="glwire_local=1"`）且 host **`GLWIRE_NO_PANEL=1` replay** 时才有意义。缺任一条，panel 可能不是每帧呈现面：
> - 没有 `glwire_local=1`：guest 不发 `0xFFFF0001` panel，host 抓到的 750x1624 上传可能是 boot 期 mask（v6 就是黑+纯绿 1/4 块），diff 是假 NOT；
> - 没有 `GLWIRE_NO_PANEL=1`：host 直接把 guest 上传的 panel 贴回去，`picture == panel`，diff 自证，等于没测 replay。

在 C4 验收命令里同时加 `XCMD="glwire_local=1"` 与 `HOST_ENV` 里的 `GLWIRE_NO_PANEL=1`、`GLWIRE_DUMP_PAIR=1`：

```sh
cd "$REPO"
mkdir -p /tmp/s19p0-1
XCMD="glwire_local=1" \
ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300 \
HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=/tmp/s19p0-1/label GLWIRE_DRAW_LOG=1 GLWIRE_DUMP=/tmp/s19p0-1/f GLWIRE_DUMP_AT=800 GLWIRE_DUMP_PAIR=1" \
IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
tools/glwire/harness/boot-measure.sh s19p0-1 "$ROOT" 1500
```

- **`XCMD="glwire_local=1"`**：guest 本地也合成。guest log 应出现
  `drawn here as well (glwire_local)`；host 每帧应出现 `upload texture 4294901761 ... 750x1624`
  （`0xFFFF0001` = guest 的合成面板）。没有这两条 → 不是 local composition，C4 配方无效。
- **`GLWIRE_NO_PANEL=1`**：host 不贴 guest panel，改为 replay 录制流；host 启动横幅应含
  `(no panel: the recorded calls are replayed instead)`。不设它，`picture` 与 `panel` 是同一份，diff 没有意义。
- **`GLWIRE_DUMP_PAIR=1`**：除 `f-panel`/`f-picture` 外再写 `f-n1-panel`/`f-n1-picture`；
  `diffdump` **用 `f-n1`**（panel/picture 差一帧的既有口径）。
- 四个探针**可以共存**：同一条 `HOST_ENV` 里空格分隔即可，上面的命令已经这样写。
  `GLWIRE_LABEL_DUMP` 负责 `label draw` 行，`GLWIRE_DRAW_LOG` 负责逐笔 `draw#`，`GLWIRE_DUMP` 负责同帧 pair。
- `GLWIRE_DUMP=<prefix>`：写 `<prefix>-panel-WxH.rgba` 与 `<prefix>-picture-WxH.bgra`；
  前缀目录必须先存在（`host_glwire` 只 `fopen`，不建目录），所以上面先 `mkdir -p`。
- `GLWIRE_DUMP_AT=800`：**第一帧之后 800 s** 取这一对（v6 实测 UI 在 600 s，800 s 已在 UI 之后）。
  若本发 UI 晚于 800 s，按上一发 `xb: ui at` 调大，并保证在 `UI_GRACE` 结束前完成 dump。
- 如果这一对是黑的/明显不到界面：先检查 `glwire_local`/`NO_PANEL` 是否真的生效，不要急着调大 AT。
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
  XCMD="glwire_local=1" \
  ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1300 MINFRAMES=99999 MAXSECS=1500 UI_GRACE=300 \
  HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_TEXMAT_NAN_TO_IDENTITY=1 GLWIRE_LABEL_DUMP=/tmp/s19p0-${n}/label GLWIRE_DRAW_LOG=1 GLWIRE_DUMP=/tmp/s19p0-${n}/f GLWIRE_DUMP_AT=800 GLWIRE_DUMP_PAIR=1" \
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

### 2.7 排障：`api.github.com` 403 不是代理故障

巡检时 `curl -x 127.0.0.1:7892 https://api.github.com` 返回 **403**，很容易被当成代理坏了；实际上**未认证访问 GitHub API 的正常限流**也会 403（响应头 `x-ratelimit-remaining: 0`），`github.com` 根域和仓库页仍是 200，`git ls-remote` 正常。

**判别（按顺序）**

```sh
# 1) 网页/仓库域是否正常
curl -sS -o /dev/null -w '%{http_code}\n' https://github.com
curl -sS -o /dev/null -w '%{http_code}\n' https://github.com/lshan99q/iosvm

# 2) git 协议是否正常
git ls-remote origin HEAD >/dev/null && echo ls-remote-ok

# 3) API 的 403 响应头/正文
curl -sS -D - https://api.github.com -o /tmp/gh-api-body.txt | head -20
head -5 /tmp/gh-api-body.txt
```

- 若 1) 是 **200**、2) 成功、3) 是 **403 + `x-ratelimit-remaining: 0`** → **不是代理/DNS/Git 凭证问题**，是 GitHub API 未认证限流；等一会儿或带认证再查，**不要改代理、不要重配 credential**。
- 若 1) 也失败（连接/DNS/TLS）→ 才按代理/网络故障排查。
- 不要把 GitHub token 写进命令历史/文档/日志；API 限流不需要 token 也能解释 403。


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

> **⚠ 先看配方，再看百分比。** C4 必须是 **local composition + no-panel replay**：
> `XCMD="glwire_local=1"` + `HOST_ENV` 里 `GLWIRE_NO_PANEL=1` + `GLWIRE_DUMP_PAIR=1`，用 `f-n1` 比较。
> 没有 `glwire_local=1` 时 guest 不发 `0xFFFF0001` panel，host 可能把 boot 期的 750x1624 mask 当 panel；
> 没有 `GLWIRE_NO_PANEL=1` 时 host 直接贴 guest panel，`picture == panel`，diff 自证。

**怎么取数**

```sh
cd "$REPO"
n=1   # 依次改成 2、3

# 1) 四件必须齐；C4 用 f-n1
ls -l /tmp/s19p0-${n}/f-n1-panel-*.rgba /tmp/s19p0-${n}/f-n1-picture-*.bgra

# 2) panel 合理性：两帧 md5 应不同；相同先怀疑抓的是静态 mask
md5sum /tmp/s19p0-${n}/f-panel-*.rgba /tmp/s19p0-${n}/f-n1-panel-*.rgba

# 3) panel 不能是单一色，也不能是黑 + 纯绿 mask（v6 的假 NOT 就是这种）
python3 - "$n" <<'PY'
import collections, glob, sys
n = sys.argv[1]
path = glob.glob(f"/tmp/s19p0-{n}/f-n1-panel-*.rgba")[0]
blob = open(path, "rb").read()
pixels = collections.Counter(blob[i:i + 4] for i in range(0, len(blob), 4))
print("distinct RGBA:", len(pixels))
for pixel, count in pixels.most_common(3):
    print(pixel.hex(), count)
if len(pixels) <= 2:
    print("SUSPECT: <=2 colours -- likely a mask, not a panel")
PY

# 4) 同帧比较（必须用 f-n1）
python3 tools/glwire/harness/diffdump.py /tmp/s19p0-${n}/f-n1 \
  -o /tmp/s19p0-${n}/diff --tolerance 8
```

**看哪一行**：输出末尾附近

```
within 8 in every channel: <N>/<total> (<share>%) -> SAME FRAME / NOT the same frame
```

**通过标准**：

- 配方前提成立：guest log 有 `drawn here as well (glwire_local)`；host 每帧有
  `upload texture 4294901761 ... 750x1624`；启动横幅有 `(no panel: the recorded calls are replayed instead)`。
- panel 合理：`f-panel` 与 `f-n1-panel` 的 md5 不同（静态画面除外，需另证）；不同时只有 2 种颜色、
  或主色是黑 + 纯绿 `(0,255,0)` → 这是 mask，C4 直接记 `n/a`，**不报百分比**。
- `share >= 95.00%` 且结论是 `SAME FRAME`；
- 3 发各算一次；**计入 P0 的那 2 发必须都 ≥95%**。缺 dump / panel 不合理的发记 `n/a`，
  不能让另一发的 diff 顶替；若 3 发只有 1 发有有效 dump，P0 不通过。

**v6 的教训（不要重复）**：2026-11-08 的 v6 在 **remote_only** 下只加了 `GLWIRE_NO_PANEL=1`，
`f-panel`/`f-n1-panel` md5 完全相同、只有黑与纯绿两色（绿块正好 1/4 屏），`diffdump` 得到
**49.12% NOT**——这是 **panel 源错导致的假 NOT**，不是 replay 画面问题：同一发的 `f-n1-picture`
与 `s19o-v6-last.png` 在 tol 8 下 **95.89%**，说明 host replay 本身是可信的。
v7 正在用上面的正确配方验证；**在 v7 结果出来前，不声称 C4 已过。**

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

### 4.5 s19o-v3 实测与判读：字形仍然全灭，viewport 待验

> **后记（v4/v5）**：本节保留 v3 当时的判读。后来 v4 用 `GLWIRE_VIEWPORT_TO_TARGET` 证实 viewport 才是根因，v5 用跨帧保留 viewport 让 prog15 从全 0 变成 223/223 >0。**v3 把 tc0 当成文字根因是误判**：v5 里 prog15 的 223 笔中有 36 笔带 NaN 仍能落像素，`tc0 v1.y NaN` 不阻断渲染；v3 的 tc0 问题实际主要是 prog13 背板/阴影路径的遗留。详见 §4.6、§4.7。
>
> 归档：`/host/iosvm/logs/s19o-v3/`（`report.md`、`text-draws.tsv`、`draws-landed.txt`、`draws-zero-*.txt`、`imgdiff-*.txt`、日志与截图）。**这些文件都不进 git**。本节只记录事实与未决点，**P0 仍未通过**。

**运行**

| 项 | 值 |
|---|---|
| label / 时间 | `s19o-v3` / 2026-11-08 03:30:02Z–04:00:48Z |
| 配方 | `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1500 MINFRAMES=99999 MAXSECS=1800 UI_GRACE=300`；`HOST_ENV="GLWIRE_TEXT_DRAW_LINE=1"` |
| `end=` | `qemu-exited`（1800 s 到点，`terminating on signal 15 ... (timeout)`），**不是崩溃** |
| `ui=` | `0s` / `ui_bytes=0`：最大截图 **251,125 B < `MINSHOT=300000`**，与 A1 的 `ui=0` 同口径 |
| 稳定性 | `panic=0`、`SIGSEGV=0`、`usertrap=0`；host frames **4,918** |
| 截图 | `s19o-v3-shot.png` 251,125 B；`s19o-v3-last.png` 245,084 B |
| text draw | **2,442 行**；UI 段（frame ≥ 1500）**1,435 行** |

**分组：字形层全 zero**

| 口径 | 总数 | landed (`nz>0`) | zero (`nz=0`) | skip (`nz=-`) |
|---|---:|---:|---:|---:|
| 全部 | 2,442 | **32** | **2,246** | **164** |
| UI 段 frame ≥ 1500 | 1,435 | **12** | **1,387** | **36** |

- `program 13` = 2,014 笔**全 zero**；`program 15` = 232 笔**全 zero**；两者就是字形层。
- landed 的 32 笔不是字形：`program 5` 图标（138x138 19,044 px；750x1624 94,692 px）与 `program 17/19` 合成（301,056 / 215,040 px）。
- UI 段 landed 12 笔：`prog5`×8、`prog17`×2、`prog19`×2。

**四维核对**

| 维度 | v3 实测 | 读法 |
|---|---|---|
| target | 字形 draw 只进 750x200 / 64x19 / 64x9 等小 tile；这些 target **没有一笔 landed**；landed 全在 750x1624 合成与 138x138 图标 | target 选对了，不是「画到别的层」 |
| matrix | 46 种矩阵，**0 个 NaN/Inf**；`m00=2/750`，`m11=2/200`（750x200 tile）把 bbox **精确映射进自己的 scissor**（#19: (374,1193)→clip(-1,-1)，(551,1283)→clip(-0.528,-0.1)） | 矩阵/几何不是本轮 zero 的原因 |
| scissor | 字形 102 笔（51 对）**100% 含住 bbox**；2,246 笔 zero 里 **2,144 笔 scissor 是 off** | scissor 基本排除；不是「被裁掉」 |
| `tc0` | **1,811 / 2,442（74.2%）含 NaN**；UI 段 **1,190 / 1,435（82.9%）**；NaN 固定在**顶点 2 的 y（v1.y）**，另有 `1.376e29` 垃圾值 | v3 当时误判为根因；v4/v5 证明 NaN 不阻断，真正原因是 viewport（§4.6/§4.7） |

`tc0` 的关键反例：`program 15` 的 UI 段 46 笔中，**41 笔不含 NaN**（24 笔是 `0.25..0.50` 图集网格、16 笔是 `0.125..0.50` 网格、1 笔仍带 `4.156`/`1.376e29` 垃圾值），**这 41 笔仍然全部落 0**。所以不能把问题简单写成「有 NaN 所以全灭」——**tc0 合法时也还没落像素**。v4/v5 已经回答：这批 0 是 viewport 被拉成 750x1624 后裁掉的，不是 tc0 阻断；v5 的 prog15 带 NaN 也能落 221–1174 像素。

**当时的未决点：viewport 与 target 的尺寸不一致（v4 已证实，v5 已修复）**

- v3 的 `program 15` 全部 232 笔的 viewport 都是 **750x1624**，而 target/matrix 是按 **200 高**（或 19 高）的小 tile 生成的。
- 运行时观察（lead 口径）：每帧只有 **2 次 `glViewport`**，帧末 viewport 又变成小 target 尺寸。若 text draw 发生在「viewport 还是全屏、target 已是小 tile」的窗口里，几何会按全屏比例落到小 target 上，矩阵再对也无法覆盖预期像素。
- 这是 v3 当时的未决假设；v4 已用 `GLWIRE_VIEWPORT_LOG` + `GLWIRE_VIEWPORT_TO_TARGET` 证实，v5 已用跨帧保留 viewport 修复。**v5 的 prog15 223/223 >0**，见 §4.7。
- v4 是探针发、v5 是根因验证发，都不是 P0 验收发；P0 仍要按 §三 连跑 3 发。

**Search 的「Se」：位置不重叠，无法像素级归属**

- 截图里「Se」的实测像素在 1125 空间 `(483,2019)–(562,2050)`，换算到 750 空间约 `(322,1346)–(375,1367)`。
- v3 的 text draw 全表里，**没有任何一笔的 bbox 覆盖这个位置**（bbox 的 y 最大到 1283）；唯一宽度 177 px 的 Search 标签是 frame 7 #19/#20，bbox (374,1193)–(551,1283)，`scissor-nonzero=0`。
- 本发**没有 `GLWIRE_LABEL_DUMP`**，无法做像素级归属；「Se」可能来自 tile 被其它复合 draw 复用/缓存。**不要把「Se」写成 text draw 画出来的**，只能记「屏幕上有 Se，归属未定」。

**imgdiff：这是截图回归，不是 P0 同帧 diff**

`s19o-v3` vs A1 基线（`s19n-repro`），`--tolerance 8`：

| 范围 | within | different | bbox |
|---|---:|---:|---|
| 全图 1125x2436 | **99.56%** | **0.44%** | `(91,80,769,2056)` |
| Search crop `(250,2000,650,300)` | **99.89%** | **0.11%** | `(487,2042,563,2056)` |

这是**截图回归**，说明 v3 与 A1 的画面很接近；它**不是 P0 的 C4 同帧 diff**（C4 要同一发的 panel vs picture，`diffdump` ≥95%）。v3 本发没有 `GLWIRE_DUMP`，C4 没有数据，不能拿 99.56% 充当 P0 通过。

**结论**

- 界面/稳定性：OK（4,918 帧、0 panic/SIGSEGV/usertrap；`ui=0` 只是 251 KB < 300 KB 的门槛）。
- 字形层：`prog13/15` **2,246/2,246 全 zero**；P0 **仍未通过**。
- 四维里 target、matrix、scissor 基本排除；`tc0` 的 NaN 是当时剩下的疑点，但 v4/v5 证明 **viewport 才是根因，tc0 v1.y NaN 不阻断**。v3 的「tc0 根因」结论作废，改为「prog13 背板/阴影路径的遗留问题」。
- 下一发 v4（viewport 探针）已完成；结果与 v5 根因修复见 §4.6、§4.7。

### 4.6 s19o-v4 实测：`end=ui` 首次达成，viewport 假设成立（P0 仍未通过）

> 归档：`/host/iosvm/logs/s19o-v4/`（`report.md`、`viewport-1590-1710.txt`、`imgdiff-*.txt`、日志与截图）。**这些文件都不进 git**。本节接 §4.5，记录 v4 的事实；**P0 仍未通过**。

**运行与结束**

| 项 | 值 |
|---|---|
| label / 时间 | `s19o-v4` / 2026-11-08 04:32:12Z–04:54:58Z |
| 配方 | `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1500 MINFRAMES=99999 MAXSECS=1800 UI_GRACE=300 MINSHOT=200000`；`HOST_ENV="GLWIRE_VIEWPORT_LOG=1 GLWIRE_VIEWPORT_TO_TARGET=1 GLWIRE_TEXT_DRAW_LINE=1 GLWIRE_TEXT_DRAW_FROM_FRAME=1500"` |
| `end=` | **`end=ui`（首次）**：`ui=630s`，`ui_bytes=295846`（≥ 本发 `MINSHOT=200000`） |
| 最大截图 | **301,883 B**（`s19o-v4-shot.png`；`-last.png` 298,296 B） |
| 稳定性 | `panic=0`、`SIGSEGV=0`、`usertrap=0`；host frames **3,086** |
| text draw | **638 行**，frame **1501–3038**（UI 段；`FROM_FRAME=1500` 把早期 draw 滤掉了） |

注意：本发 `MINSHOT=200000`，不是默认 300,000；`ui_bytes=295,846` 是按 200 KB 门槛记的，最大截图 301,883 B 才 >300 KB。不要把 `end=ui` 写成「300 KB 判据已满足」——300 KB 是 P0 的单发判据，本发最大图刚好过线，但 `ui` 本身是按 200 KB 门槛触发的。

**视觉：10 个 App 名称 + 2 个 widget 名称**

| 位置 | 可读名称 |
|---|---|
| widget 行 | **Weather**、**Calendar** |
| 第一行 | **FaceTime**、**Calendar**、**Photos**、**Mail** |
| 第二行 | **Notes**、**Reminders**（右侧 Clock、News 两个图标**没有名称**） |
| 第三行 | **TV**、**Podcasts** |
| 第四行 | **Health**、**Wallet** |
| dock / Search | dock 按 iOS 惯例不显示名称；Search 仍是「**Se**」两个字母 |

共 **11 个不同字符串 / 12 个标签**（Calendar 同时出现于 widget 与第一行）：10 个 App 名称 + 2 个 widget 名称。对比 A1（`s19n-repro`，0 个 App 名称、只有「Se」），这是首次把 App 名称真正写上屏。**缺口仍是 Clock、News 两个 label**；「Se」也还在。

**根因链：viewport 被每帧 replay 末尾冲掉（本发已验证假设）**

1. text draw 是**下一帧的第一条记录**，出现在该帧所有 `viewport` 命令之前；guest 在**上一帧末尾**才把目标 viewport 设成小尺寸（如 64x19）。
2. host 在每帧 replay 结束时执行 `glViewport(0,0,panel_w,panel_h)`（给 panel 合成用），把 guest 刚设好的小 viewport 冲掉。
3. 于是下一条 text draw 到达时，实际 `GL_VIEWPORT` 是 **750x1624**，而它的 matrix 是按 **200 高 / 19 高**的小 tile 生成的 → 纵向拉伸后被裁掉。
4. `GLWIRE_VIEWPORT_TO_TARGET=1` 在 glyph draw 前临时 `glViewport(0,0,target)`，draw 后恢复；override 日志 **20 条是打印上限，不是总次数**，全部是 `fbo 5 750x200 (was 0,0 750x1624)`，集中在 frame ~10–19。这批早期 750x200 标签 draw 被修正后，名称写进 label layer 并保留到 UI。

**这是诊断 override 验证了根因，不是根因修复。** 代码正在做的修复是「跨 frame 保留 guest viewport + panel 绘制前后 save/restore」；在 v5 给出数据前，不要写「viewport 已修好」。

**仍 zero 的 638 笔：UI 段全是 prog13，没有 prog15**

- UI 窗口 638 笔 text draw **全部是 `program 13`**（64x19/64x9、fbo 4），**`program 15` 出现 0 次**。
- 因为 `FROM_FRAME=1500` 把早期 draw 行滤掉了，这只说明 **UI 段没有 prog15**，**不能**据此写「prog15 的 nz 从 0 变成 >0」；本发无法按原样回答早期 prog15 的 nz。
- `u0` 采样的是字形图集，不是目标：`tex 49 146x90` ×441、`tex 45 161x90` ×189、`tex 13 203x90` ×8；**`u0==target` = 0 笔**，排除反馈环。
- `tc0`：**573/638 含 NaN**；其余 65 笔前两对是 `0.000,3.375`，但后两对仍是 `5.6579e28` 级垃圾。638/638 `scissor-nonzero=0`，这是**阴影/暗化路径的遗留问题**，不是「标签层已经全好」。

**imgdiff：v4 vs A1（截图回归，不是 P0 同帧 diff）**

| 范围 | within | different | bbox（right/bottom exclusive） |
|---|---:|---:|---|
| 全图 1125x2436 | **98.41%** | **1.59%** | `(65,80,1015,2056)` |
| Search crop `250,2000 650x300` | **99.89%** | **0.11%** | `(523,2048,563,2056)` |

全图差异从 v3 的 99.56% 降到 98.41%，**原因是名称出现了**（diff bbox 覆盖 label 区），不是画面回归；Search crop 几乎不变（Se 的差异 bbox 还从 `(487,2042,563,2056)` 缩到 `(523,2048,563,2056)`）。这是截图回归，**不是 C4**：v4 没有 `GLWIRE_DUMP` 的 panel/picture pair，同帧 `diffdump` 没有数据，不能拿 98.41% 充当 P0 通过。

**结论与边界**

- **里程碑**：`end=ui` 首次达成；10 个 App 名称 + 2 个 widget 名称；viewport 假设被 override 实验证实。
- **P0 不能算达标**：只有 **1 发**，没有同帧 `diffdump --tolerance 8` ≥95%，Clock/News 仍缺；「Se」也还在。
- **下一步**：code 做根因修复（跨 frame 保留 guest viewport，panel 绘制前后 save/restore），随后 **v5 验证**；v5 结果见 §4.7。v5 也不是验收发，P0 仍要按 §三 连跑 3 发、≥2 发过全部判据。

### 4.7 s19o-v5 实测：根因修复成立，但 diffdump 仍 n/a（P0 不能计入）

> 归档：`/host/iosvm/logs/s19o-v5/`（`report.md`、`rows.json`、`draws-early-prog15.txt`、`imgdiff-*.txt`、日志与截图）。**这些文件都不进 git**。本节只记录 v5 的事实；**P0 仍未通过，v5 暂不能计入 P0 发次**。

**运行与结束**

| 项 | 值 |
|---|---|
| label / 时间 | `s19o-v5` / 2026-11-08 05:52:20Z–06:07:58Z |
| 修复基线 | `204ae10`（`tools(glwire): keep the guest viewport across frame boundaries`）；不再设 `GLWIRE_VIEWPORT_TO_TARGET` |
| 配方 | `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1500 MINFRAMES=99999 MAXSECS=1800 UI_GRACE=300 MINSHOT=200000`；`HOST_ENV="GLWIRE_TEXT_DRAW_LINE=1 GLWIRE_DUMP=/tmp/s19o-v5/f GLWIRE_DUMP_AT=800 GLWIRE_DUMP_PAIR=1"` |
| `end=` | **`end=ui`**：`ui=600s`，`ui_bytes=308809` |
| 最大截图 | **322,481 B ≥ 300,000**（`s19o-v5-shot.png`；last 320,105 B、final 320,462 B） |
| 稳定性 | `panic=0`、`SIGSEGV=0`、`usertrap=0`；host frames **2,964** |
| text draw | **1,770 行**；`nz>0` **273** 笔 |

**视觉**

- 名称与 v4 相同：**10 个 App + 2 个 widget**（Weather、Calendar；FaceTime、Calendar、Photos、Mail；Notes、Reminders；TV、Podcasts；Health、Wallet）。
- **Clock、News 仍无名称**；Search 仍是「**Se**」。
- **新增正确元素**：Search 圆角药丸背景、dock 圆角背景这次画出来了（v4 该区域是纯黑）。

**根因修复成立：prog15 从全 0 变 223/223 >0**

- 本发**不设** `GLWIRE_VIEWPORT_TO_TARGET`，只靠跨帧保留 guest viewport。
- `program 15`：**223/223 全部 `nz>0`**；`program 13`：**1,283/1,283 全部 0**。
- `frame < 100` 的 **71 笔 prog15 全部 >0**（`nz` 221–1174）；viewport 已变成 **`0,0 750x200`**（不再是 v4 的 750x1624），对应 scissor 如 `0,0 178x90`。
- 代表行：`frame 9 #28 prog 15 ... viewport 0,0 750x200 scissor on 0,0 143x90 ... scissor-nonzero 585`。
- **Clock / News 的 prog15 draw 本身成功**：Clock `nz 888/888`、News `nz 536/536`，矩阵/bbox 都落在 750x200 tile 的 scissor 内。它们不上屏是 **draw 之后的合成/层**问题，**不要写成「字形没画出来」**。

**tc0 修正（v3 的结论作废）**

- v5 的 prog15 里只有 **36/223 笔 tc0 带 NaN**，其余是 `0.125/0.25` 的良性 texel 网格；**`tc0 v1.y` 的 NaN 不阻断渲染**（Clock/News 带 NaN 仍有 888/536 像素）。
- v3 的「tc0 是文字根因」应修正为 **`prog13` 背板/阴影路径的遗留问题**：v5 UI 段 **573 笔全 prog13、全 0**，是背板 pass，不是字形 pass。
- 名称是在**早期 frame 9–781** 一次性画进 750x200 标签层，之后 UI 只复用，不再重画字形。

**diffdump：n/a（不是 SAME/NOT）**

- `GLWIRE_DUMP_PAIR=1` 只写出了 `f-picture-750x1624.bgra` 与 `f-n1-picture-750x1624.bgra`，**没有 `f-panel` / `f-n1-panel`**；`diffdump.py` 报 `need both ...-panel-*.rgba and ...-picture-*.bgra`，所以 C4 **n/a**。
- 根因在宿主侧 `names.h:332`：只有 `guest == 0xFFFF0001` 或 **1125x2436** 时才把上传存进 `g->panel`；本发面板是 **750x1624**、纹理名 1/4/7/10，全日志 `1125x2436` 出现 0 次，`client->names.panel` 全程 NULL。**这是分辨率从 1125x2436 切到 750x1624 后 panel 落盘条件没跟着更新，不是 dump 太早。**
- dump 行号本身合格：首个非零 draw 在日志第 54 行（frame 9 #28），PAIR 写在 9836 行（frame 2082），晚于字形且晚于 UI。**缺的只是 panel 文件。**
- v6 已按 panel 条件重跑：四个文件都生成了，但 **remote_only 下 panel 抓到 boot 期黑+绿 mask**，`diffdump` 49.12% 是假 NOT（见 §二 C4 的 v6 教训）。正确配方是 **local composition + no-panel**，等 **v7** 验证；**不要把 n/a/假 NOT 写成 SAME。**

**imgdiff：v5 vs A1（截图回归，不是 C4）**

| 范围 | within | different | bbox（right/bottom exclusive） | 判定 |
|---|---:|---:|---|---|
| 全图 1125x2436 | **95.20%** | **4.80%** | `(29,80,988,2407)` | PASS（≥95%） |
| Search crop `250,2000 650x300` | **78.81%** | **21.19%** | `(250,2000,563,2300)` | **FAIL** |

- 全图差异来自名称出现 + 新画出的 Search 药丸/dock 圆角背景，不是画面回归；全图仍 ≥95%。
- Search crop 的 FAIL 也是**新生儿素**（药丸 2000–2080、dock 背景 2150–2300）造成的，**不是 Se 字形变差**。
- 这是截图回归，**不是 C4**：C4 要同发 panel/picture 的 `diffdump`，v5 的 panel 没落盘，C4 只能 n/a。

**P0 记分与边界**

| P0 判据 | v5 | 说明 |
|---|---|---|
| 首页 ≥5 个 App 名称 | ✅ 10 App + 2 widget | Clock/News 仍缺 |
| 截图 ≥300,000 B | ✅ 322,481 B | |
| 同帧 `diffdump --tolerance 8` ≥95% | ❌ **n/a** | panel 未落盘（`names.h` 条件过时），不是 SAME/NOT |
| `end=ui` / 无 panic | ✅ | |

- **v5 暂不能计入 P0 发次**；v6 补了 panel 文件但 panel 源仍错，C4 仍是假 NOT。
- v7 已按正确配方跑完：**C4 首次 SAME FRAME**，计入 P0 第一发，见 §4.8。

### 4.8 s19o-v7 实测：C4 首次 SAME FRAME，P0 计入 1/3

> 归档：`/host/iosvm/logs/s19o-v7/`（`report.md`、四张 dump、`diffdump.txt`、`check_panel.txt`、日志、截图、`aborted/`）。**这些文件都不进 git**。本节记录 v7 的事实；**P0 记为 1/3，不是已经通过**。

**口径（与 §2.4/C4 的修正配方一致）**

| 项 | 值 |
|---|---|
| label / 时间 | `s19o-v7` / 2026-11-08 07:08:34Z–07:25:04Z |
| XCMD | **`glwire_local=1`**；guest log：`the frame is drawn here as well (glwire_local); recorder mask 0xffffffffffffffff` |
| HOST_ENV | `GLWIRE_NO_PANEL=1 GLWIRE_TEXT_DRAW_LINE=1 GLWIRE_DUMP=/tmp/s19o-v7/f GLWIRE_DUMP_AT=800 GLWIRE_DUMP_PAIR=1` |
| 其他 env | `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MINFRAMES=99999 MAXSECS=2400 UI_GRACE=300 MINSHOT=200000` |
| 本地合成证据 | host `upload texture 4294901761 target 0xde1 internal 0x1908 750x1624 ...` **982 次 = 982 host 帧（1:1）**；panel hash 逐帧变化（v6 是恒定） |

**结束与视觉**

- 收尾：`xb: s19o-v7 end=ui ui=630s ui_bytes=321669 hostframes=982 MATCH=47 DIFF=935 SIGSEGV=0 usertrap=0 panic=0 biggest=330931`。
- 最大截图 **330,931 B ≥ 300,000**（shot 330,931 / last 330,085 / final 330,389）；`MINSHOT=200000` 只是本发的 `ui` 门槛，**不改变 P0 的 300 KB 单发判据**。
- dump 时刻（~800 s）名称：widget **Weather、Calendar**；App **FaceTime、Calendar、Photos、Mail、Notes、Reminders、TV、Podcasts、Health、Wallet**（10 个）；Clock、News 此时未显示；Search 仍「Se」。
- **时序不是永久缺失**：同配方的 aborted 发（`DUMP_AT=1200` 被终止）在更晚的 460,001 B 截图里出现了 **Clock、News、App Store、Maps、Settings 和完整「Search」**（`logs/s19o-v7/aborted/`）。所以 Clock/News 是 **UI 分批加载时序**，不是标签永久坏。

**prog15 与 Clock/News**

- text draw **640 行**（frame 8..973）：`nz>0` 106、`nz=0` 405、`nz=-` 129。
- `prog15`：**120 笔，86 笔 `nz>0`**，34 笔 skip（readrect 无效）；`prog13`：442 笔、**0 笔 `nz>0`**（背板 pass）。
- 早期 `frame<100`：139 笔（prog13 71 + prog15 68）；**prog15 有 34 笔 `nz>0`（221–1149）**，另 34 笔 skip。
- Clock：`frame 15 #55 prog15 ... 750x200 ... scissor on 0,0 146x90 ... nz 598`；News：`frame 21 #88 prog15 ... scissor on 0,0 144x90 ... nz 536`。两笔都成功，和 v5 的结论一致：**不上屏是之后的合成/加载，不是字形没画出来**。

**panel 真实性**

```text
f-panel-750x1624.rgba       a116b83d172f869bed2350dc0a882f2c
f-n1-panel-750x1624.rgba    deef639e7da66c94ac675236c0dcc4d4   (md5 不同)
check_panel: colours 6052; painted 293284/1218000 (24.1%)
             black 924716 (75.9%), pure green 0 (0.0%)
             verdict: looks like a picture
```

**C4：`diffdump /tmp/s19o-v7/f-n1 --tolerance 8`**

```text
painted: panel 293284 (24.1%), picture 294825 (24.2%)
missing: 48 (0.00%) box (46,142,355,451)
extra:   1589 (0.13%) box (47,491,642,878)
differing: 2040 (0.17%) box (46,142,642,878)
within 8 in every channel: 1216183/1218000 (99.85%) -> SAME FRAME
```

- **P0 口径：same-index `f-n1` = 99.85%，SAME FRAME，通过。**
- 交叉配对：same-index `f` 也是 99.85%；cross `f-n1-panel` vs `f-picture` = **98.68%**，作为 `picture(N)==panel(N+1)` 关系的佐证，不替代 P0 的 same-index 结果。
- 时序：dump 写入 frame **568/569（~800 s）**，晚于 UI（630 s）与首个非零 text draw（frame 8 #23，nz 1030），满足“dump 在界面/字形之后”。

**P0 记分**

| 判据 | v7 |
|---|---|
| `end=ui` / 无 panic | ✅ 630 s |
| 截图 ≥300,000 B | ✅ 330,931 B |
| ≥5 App 名称 | ✅ 10 App + 2 widget（更晚状态还有 Clock/News） |
| 同帧 `diffdump` ≥95% SAME | ✅ **99.85% SAME FRAME** |
| panel 真实 | ✅ 6052 色、0 绿、两帧 md5 不同、1:1 上传 |

**v7 计入 P0 第一发**；还差 **2 发同口径**（v8/v9 已派）。**现在 1/3，不能写「P0 已通过」**；要等 v8/v9 里至少再有一发满足全部判据，才达到「3 发里 ≥2 发」。

### 4.9 v8/v9 与 P0 判定：v7+v9 = 2/3，P0 达成

> 归档：`/host/iosvm/logs/s19o-v8/`、`/host/iosvm/logs/s19o-v9/`（`report.md`、四张 dump、`diffdump.txt`、`check_panel.txt`、日志、截图）。**这些文件都不进 git**。
> 结论按 P0 原判据：**连续 3 发里 ≥2 发满足名称、截图、同帧 diff** → **v7 + v9 = 2/3，P0 达成**。v8 不计入，原因是**采样时刻落在 UI 加载动画中**，不是渲染/配对回归。

**两发总表**

| 项 | v8 | v9 |
|---|---|---|
| 时间 | 2026-11-08 07:45:40Z–08:00:05Z | 2026-11-08 08:05:18Z–08:19:42Z |
| 口径 | `glwire_local=1` + `NO_PANEL=1` + `PAIR=1` + `AT=800` | 同 v8 |
| `end=` | `ui=660s`，`ui_bytes=320147` | `ui=630s`，`ui_bytes=309137` |
| 最大截图 | **327,753 B** | **326,461 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host 1,271 帧 | 0/0/0；host 1,060 帧 |
| 名称（dump 时） | 10 App + 2 widget；Clock/News 未显示，Search「Se」 | 10 App + 2 widget；Clock/News 未显示，Search「Se」 |
| panel | 15,975 色、pure green 0、两帧 md5 不同 | 11,774 色、pure green 0、两帧 md5 不同 |
| C4 `f-n1` tol 8 | **89.53% NOT** | **99.98% SAME FRAME** |
| 是否计入 P0 | **否** | **是** |

**v8：失败模式是「dump 抓到 UI 加载动画」**

```
painted: panel 608663 (50.0%), picture 493367 (40.5%)
missing (panel paints, picture black): 117079 (9.61%), box (46, 142, 729, 1603)
extra   (picture paints, panel black):   1783 (0.15%), box (44, 491, 626, 1258)
differing: 129296 (10.62%), box (44, 54, 729, 1603)
within 8: 1090498/1218000 (89.53%) -> NOT the same frame (the line is 95%)
```

- 四种配对都救不回来：same N = 89.53%、same N+1 = 89.53%、cross = **89.03%**、cross2 = **89.03%**。所以**不是配对选错**。
- 内容差一代：`diff/panel.png` 是**完整桌面**（16 个名称、完整 Search、4 个 dock 图标），`diff/picture.png` 还停在 **10 图标 + 「Se」+ 2 个 dock** 的状态。
- `missing` 的 9.61% 正好是「新加载出来的图标/名称/dock」。即 dump 落在 UI 从 10 图标向完整桌面加载的动画中，**host replay 落后于 guest 呈现**。
- v8 的 `end=ui`、327,753 B、10 名称、panel 真实都成立；**只有 C4 的采样时刻不成立**。v8 明确不计入 P0。

**v9：五项数据齐全，C4 99.98% SAME**

```
painted: panel 492858 (40.5%), picture 492858 (40.5%)
missing: 0 (0.00%)
extra:   0 (0.00%)
differing: 193 (0.02%), box (413, 769, 468, 785)
within 8 in every channel: 1217808/1218000 (99.98%) -> SAME FRAME
```

- `end=ui` 630 s；最大截图 **326,461 B**；10 App + 2 widget；panel 11,774 色、0 绿、两帧 md5 不同。
- P0 口径 `f-n1` = **99.98% SAME FRAME**；cross `f-n1-panel` vs `f-picture` = **100.00%** 作佐证。
- 注意：same N（`f-panel` vs `f-picture`）只有 **89.58%**，说明 `f` 那一帧也在加载动画上；P0 用的是 PAIR 的 `f-n1` 静态帧（99.98%），不是 same N。

**P0 判定**

- 原判据：**连续 3 发（`QEMU_SMP=1`）里 ≥2 发首页能读出 ≥5 个 App 名称；截图 ≥300 KB；同帧 `diffdump --tolerance 8` ≥95%**。
- 计数：**v7（99.85% SAME）+ v9（99.98% SAME）= 2/3 → P0 达成**。v8 因采样时刻 89.53% NOT，不计入。
- **诚实边界**：v8 不是渲染回归，也不是配对错误；P0 的 C4 对 **dump 时刻是否落在 UI 加载动画里**很敏感。后续验收要选**静态时刻**（例如 panel/picture 连续两帧稳定），或把「panel 与 picture 的加载状态一致」加为稳定性门槛；**不要用 v8 的动画帧当回归基线**。

### 4.10 s19o-p1a 实测：prog13 是标签噪声带的主来源（P1 诊断，未修复）

> 归档：`/host/iosvm/logs/s19o-p1a/`（`report.md`、`crop_v9.png`、`crop_p1a.png`、`crop_side_by_side*.png`、`imgdiff-crop.txt`、四张 dump、日志、截图）。**这些文件都不进 git**。
> 这是 **P1 诊断发**，不是 P0 验收发；**P1 仍未修复**。

**口径**

| 项 | 值 |
|---|---|
| label / 时间 | `s19o-p1a` / 2026-11-08 08:44:50Z–09:01:43Z |
| 基础口径 | 同 v7/v9：`XCMD=glwire_local=1`；`HOST_ENV` 含 `GLWIRE_NO_PANEL=1`、`GLWIRE_DUMP_PAIR=1`、`GLWIRE_DUMP_AT=800` |
| 本发新增 | **`GLWIRE_PROGRAM_SKIP=13`**（只跳过 program 13） |
| 其他 env | `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MINFRAMES=99999 MAXSECS=2400 UI_GRACE=300 MINSHOT=200000` |
| `end=` | `end=ui`：`ui=630s`，`ui_bytes=310246`；最大截图 **327,839 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **955** 帧 |

**skip 生效，prog15 未误伤**

- `program 13 draw skipped` = **5 条**；这是源码的**打印上限**（`said < 5`），只说明“跳过了 ≥5 笔”，不是总次数。
- 本发 `text draw ... program 13` 行数 = **0**（v9 同口径是 **416** 行）→ prog13 确实没有 dispatch。
- `text draw ... program 15` = **140 行，140 笔 `nz>0`** → prog15 没被误伤，仍然全部落像素。

**Weather 标签区并排对比（crop `80,620 420x180`）**

- 裁图：`crop_v9.png`（v9）、`crop_p1a.png`（P1a）；并排 `crop_side_by_side.png`、`crop_side_by_side_2x.png`。
- 量化区域：Weather 下方噪声带，绝对坐标 **y 730–760 / x 200–500**。

| | 非黑像素 | 亮像素（mean > 150） |
|---|---:|---:|
| v9（有噪声带） | **562 / 9000** | 47 |
| P1a（SKIP=13） | **44 / 9000** | 4 |

- **-92%**（562 → 44）；肉眼：v9 在「Weather」下有一条贯穿的白色虚线/点状带，P1a 只剩「er」下方一小簇残点，**文字完整、无缺笔画**。
- `imgdiff` v9 vs P1a，crop `80,620 420x180`，tol 8：**99.06% within、0.94% different**，diff bbox **(205,736,401,749)** 正好落在噪声带（y 736–749）。

**判读**

- **prog13 是标签下方虚线/点状噪声带的主来源**（-92%），不是 prog15 的 glyph tile；prog15 的 140 笔全部正常落像素。
- 残留 **44 px** 不是零：可能来自 prog15 或其它层，需另一发 `GLWIRE_PROGRAM_SKIP=15` 或 `GLWIRE_DARKEN_OFF` 对照才能定性；本轮没跑。
- **不要写成「P1 已修复」**：这只证明 prog13 是主来源，还没有闭环，也没有验证阴影/圆角的可接受度。

**下一步**

P1b 已做（见 §4.11）：`TC0_SANITIZE=1` **没有消掉噪声带，反而让它更多**（562→758，+35%），所以 sanitize 不是 P1 修复项；**P1 仍未修复**。

### 4.11 s19o-p1b 实测：`TC0_SANITIZE` 不是噪声带修复项（P1 仍未修复）

> 归档：`/host/iosvm/logs/s19o-p1b/`（`report.md`、`measure.txt`、`imgdiff-crop.txt`、`diffdump.txt`、`crop_*`、四张 dump、日志、截图）。**这些文件都不进 git**。
> 这是 **P1 诊断发**，不改变 P0 结论，也不代表 P1 已修好。
> 合并入 main：`85d0d97`（main 首次快进至 `ce80195`）。

**口径**

| 项 | 值 |
|---|---|
| label / 时间 | `s19o-p1b` / 2026-11-08 09:17:50Z–09:43:12Z |
| 基础口径 | 同 v7：`XCMD=glwire_local=1`；`HOST_ENV` 含 `GLWIRE_NO_PANEL=1`、`GLWIRE_DUMP_PAIR=1`、`GLWIRE_DUMP_AT=800`、`GLWIRE_TEXT_DRAW_LINE=1` |
| 本发新增 | **`GLWIRE_TC0_SANITIZE=1`**，**不 skip 13** |
| `end=` | `end=ui`：`ui=630s`，`ui_bytes=307531`；最大截图 **324,505 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **1,202** 帧 |

**sanitize 生效，但噪声带更明显**

- `tc0 sanitized` = **5 行**，是源码的**打印上限**，样例为 `3 vertices` ×3、`2 vertices` ×2；不是总次数。
- 例：`frame 7 #20 prog15 ... tc0 0.000,0.000 ...`，nz **1202** → sanitize 确实把垃圾 tc0 归零，prog15 仍能落像素。
- text draw **711**：`prog13` **544**（nz>0 **0**，zero 507，skip 37）、`prog15` **114**（nz>0 **80**，skip 34）；prog5 18、prog17/19 各 4、prog3/7 全 skip。
- 早期 frame<100 prog15：68 笔，34 笔 nz>0。

**Weather 噪声带（crop `80,620 420x180`；绝对坐标 y 730–760 / x 200–500）**

| 发次 | 非黑像素 | 亮像素（mean>150） | 相对 v9 |
|---|---:|---:|---:|
| v9（基线） | **562** | 47 | 100% |
| P1a（skip 13） | **44** | 4 | **7.8%** |
| **P1b（sanitize）** | **758** | **89** | **134.9%** |

- 形态：v9 是断续碎点；P1b 变成更整齐的**竖线梳齿**（tc0 归零后每列采同一个 texel），带没有消失。
- 文字「Weather」完整，无缺笔画。
- imgdiff v9 vs P1b，同 crop tol 8：**98.30% within、1.70% different**，diff bbox `(205,736,401,749)` 仍是这条带。

**panel / diffdump（非 P0，仅记录）**

- `f-panel` md5 `f9e60dd5…` ≠ `f-n1-panel` `206b6883…`；check_panel **6,361 色、pure green 0、looks like a picture**。
- `diffdump f-n1 --tolerance 8`：panel 25.4% / picture 26.5%，missing 0、extra **14,077（1.16%）** box `(54,902,173,1054)`，**98.85% SAME FRAME**；cross `f-n1-panel` vs `f-picture` = **100.00%**。这不是 P0 验收数据。

**widget 40x40：875/1600 是旧布局基线，需要重锚**

| 框 | white/1600 | 说明 |
|---|---:|---|
| `(64,210,40,40)` | **495** | 旧文档框；与 s17probe4 的 495 一致 |
| `(69,213,40,40)` | 747 | widget bbox 内缩 0 px |
| `(69,217,40,40)` | **886** | 实际圆角角点；在 875±2%（857.5–892.5）内，+1.26% |
| `(73,213,40,40)` | 886 | 同上 |

→ 当前几何下，旧框 `(64,210,40,40)` 读到 495，实际角点 `(69,217)/(73,213)` 读到 **886**。**875/1600 是旧布局基线，不能硬凑**；P1 的白像素判据需要按当前布局重锚。

**判读**

- **`TC0_SANITIZE` 生效，但不是噪声带的修复项**：非黑像素 562→758（+35%），亮像素 47→89；只是把碎点变成规则竖线。
- 结合 P1a（skip 13 → 44）：**噪声带来源是 program 13 的 backdrop/几何带，不是 tc0 的 NaN**。sanitize 只把垃圾 tc0 归零，没有动 prog13 的这条带。
- **P1 仍未修复**；不要写「sanitize 修好了虚线」或「P1 已通过」。

**下一步**

1. 全标签 band 统计已做（见 §4.12）：**skip13 不是全局修复，只是重分布**；第二来源疑在 prog15/glyph tile。下一步 P1c 用 `GLWIRE_ATLAS_DUMP` 查 glyph tile 底部残余行。
2. widget 白像素判据按当前布局重锚（旧 875/1600 不能直接沿用；先确定新的框与基线）。
3. P1 的 Search 完整性、标签颜色仍未收口；P1 继续保持「进行中」。

### 4.12 P1 全标签 band 统计：skip13 只是重分布，第二来源疑在 glyph tile

> 归档：`/host/iosvm/logs/s19o-p1-dots.md`（统计口径与结论）、`/host/iosvm/logs/s19o-p1-dots/`（`analysis*.txt`、`analyze_dots*.py`、`band_Weather.png`、`band_FaceTime.png`、`band_Photos.png` 等）。**这些文件都不进 git**。
> 这是离线统计，不 boot、不改代码；**P1 仍未修复**。
> 合并入 main：`ce80195`。

**口径**

- 取 11 个可见标签的 prog15 `text draw` bbox（guest 750x1624 空间），截图坐标 = ×1.5。
- **band = quad 底部 10 px**（文字下方、覆盖文字宽度）；每格记 `nonblack`、`bright>32`、`bright>64`（RGB 均值）。
- 对比三张已归档截图：v9、P1a（skip13）、P1b（sanitize）。

**合计（11 个标签）**

| 发次 | nonblack | >32 | >64 | nonblack 相对 v9 |
|---|---:|---:|---:|---:|
| v9（基线） | **3499** | 1900 | 1054 | 100% |
| P1a（skip13） | **3559** | 1679 | 886 | **101.7%** |
| P1b（sanitize） | **1885** | 1091 | 713 | **53.9%** |

**P1a 不是全局清理，而是重分布**：合计非黑像素反而 **+1.7%**，只有 >32/>64 降了约 12–16%。逐标签看：

| 标签 | v9 | P1a (skip13) | P1b (sanitize) | P1a 相对 v9 |
|---|---:|---:|---:|---:|
| Weather | 719 | **24** | 741 | **-97%** |
| FaceTime | 282 | **916** | 645 | **+225%** |
| Calendar | 494 | **684** | 14 | **+38%** |
| Photos | 539 | 590 | 143 | +9% |
| Mail | 424 | **44** | 23 | **-90%** |
| Notes | 361 | 354 | 0 | -2% |
| Reminders | 163 | 181 | 116 | +11% |
| TV | 0 | 0 | 0 | — |
| Podcasts | 131 | **697** | 131 | **+432%** |
| Health | 343 | **46** | 48 | **-87%** |
| Wallet | 43 | 23 | 24 | -47% |

- `band_Weather.png`：v9 碎点 → P1a **几乎干净** → P1b 规则竖线。
- `band_FaceTime.png`：v9 一排 → P1a **更密更长** → P1b 仍有一排。
- `band_Photos.png`：v9 点带 → P1a **更宽更密** → P1b 明显减少。

**判读**

- **skip13 不是全局修复**：它把点带从 Weather/Mail/Health 挪到 FaceTime/Calendar/Podcasts；prog13 在部分标签上是点带来源，在另一些标签上反而**遮住**了残留，去掉后别处暴露出来。
- **点带还有第二个来源**，最可能是 **prog15 / glyph texture 自带的残余行**；prog13 的 backdrop/darken 只是调制可见度。
- **P1b（tc0 sanitize）也不是修复**：合计压到 53.9%，但 Weather 719→741 且形态变成规则竖线；它改变的是 glyph 采样（tc0 归零→每列一个 texel），不是去掉残余行。
- **P1 仍未修复**；不要把 skip13 或 sanitize 写成「已解决虚线」。

**下一步：P1c 查 glyph tile**

P1c 已做（见 §4.13）：**点带确实烘在 glyph tile 的 alpha 里**（42 高 tile 底部 6–8 行、90 高 tile 背板下缘外）。修复方向转到 glyph 纹理生成/裁剪；P1d 用 `GLWIRE_GLYPH_TRIM=6` 验证。**P1 仍未修复**。

### 4.13 s19o-p1c 实测：点带烘在 glyph tile 的 alpha 里（P1 仍未修复）

> 归档：`/host/iosvm/logs/s19o-p1c/`（8 张 `atlas-*.bgra`、`atlas-alpha-stats.txt`、`atlas-report.txt`、`report/` 下的 raw/底部 12 行 8× 放大图、日志、截图）。**这些文件都不进 git**。
> 这是 **P1 诊断发**，不改变 P0 结论；**P1 仍未修复**。

**口径**

| 项 | 值 |
|---|---|
| label / 时间 | `s19o-p1c` / 2026-11-08 10:00:20Z–10:18:37Z |
| XCMD | `glwire_local=1` |
| HOST_ENV | `GLWIRE_NO_PANEL=1 GLWIRE_ATLAS_DUMP=/tmp/s19o-p1c/atlas`（**只开 atlas**，不开 LABEL_DUMP/TEXT_DRAW/DUMP） |
| `end=` | `end=ui`：`ui=630s`，`ui_bytes=311124`；最大截图 **323,737 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **1,097** 帧 |
| 输出 | **8 张 atlas**（`said < 8` 打印上限）：101x42、104x42、139x42、155x42、149x90、152x90、187x90、203x90 |

**关键格式与方向**

- tile 是 **GL_ALPHA**（internal `0x1906`）：RGB 三通道全 0，墨只在 **alpha**；按 RGB 看会误判为全黑。
- raw 文件顺序 = **屏幕正立方向**（`atlas-101x42` raw 是正立「Notes」，flip 后是倒着的「ǝsoN」）→ **文件末尾行 = tile 底部 = 屏幕文字下方**。

**底部 alpha 统计（nonblack / >32 / >64）**

| tile | 底部 1 行 | 底部 2 行 | 底部 6 行 | 底部 16 行 ink 剖面（上→下） |
|---|---|---|---|---|
| 101x42 | 30/21/18 | 60/45/35 | **180/141/103** | `45,38,12,0,0,0,0,29,30,30,30,30,30,30,30,30` |
| 104x42 | 18/12/12 | 32/20/20 | 134/83/80 | `43,39,10,0,0,0,0,0,19,22,33,24,21,24,14,18` |
| 139x42 | 28/18/18 | 95/77/77 | 154/115/105 | `71,62,24,0,0,0,0,0,0,0,13,0,33,13,67,28` |
| 155x42 | 0/0/0 | 23/18/16 | 23/18/16 | `75,68,17,0,0,0,0,0,0,0,0,0,0,0,23,0` |
| 149x90 | 24/13/12 | 46/23/22 | 128/61/60 | `149×9,133,16,19,23,24,22,24` |
| 152x90 | 44/44/42 | 45/44/42 | 270/53/49 | `144×6,142×5,83,0,0,1,44` |
| 187x90 | 12/0/0 | 19/0/0 | 676/27/25 | `177×11,167,167,146,7,12` |
| 203x90 | 0/0/0 | 12/10/10 | 463/354/285 | `193×7,134,6,177,203,108,34,106,12,0` |

- **42 高 tile**：文字 → 约 4 行空白 → **底部 6–8 行稀疏点带**（101x42 最典型：底部 1/2/6 行 nonblack **30/60/180**，最后 8 行每行 29–30 px）。这就是屏幕上「Notes」下方那排点。
- **90 高 tile**：整个 tile 是圆角背板（每行 144–193 px），**背板下缘之外还有一条独立点线**（149/152 最清楚）。
- 视觉证据：`report/atlas-*-alpha-raw.png`（正立）、`report/atlas-*-alpha-bottom12-raw.png`（底部 12 行 8× 放大）。

**判定**

- **点状/残余行已经烘在 glyph/label tile 的底部 alpha 里**，不是合成阶段或 prog13 的调制产物。
- 这解释了 P1a/P1b：skip13 只是少画/多画这些带点的 tile（重分布）；tc0 sanitize 改的是采样方式，带本身还在。
- **修复方向：glyph 纹理生成/裁剪侧（guest text 路径）**——生成 tile 时不要把文字下方点状行写进纹理，或在采样/裁剪时裁掉底部 6–8 行；**合成/prog13 侧不再调**。
- **P1 仍未修复**；不要写「tile 已裁」「P1 已解决」。

**下一步**

code 加 **`GLWIRE_GLYPH_TRIM=<n>`**（默认关闭，只对 GL_ALPHA 42/90 高纹理的**末尾 n 行清零**）。**P1d** 用 `TRIM=6` 验证：点带消失，且 descender（如 `p` 的降部）不被削。P1d 结果出来前，P1 保持「进行中」。

**P1d 结果（`GLWIRE_GLYPH_TRIM=6`，已跑）**

P1d 已运行，trim 执行但**清错端**，band 3499→6246；完整数据与判读见 §4.14。P1d **不计入 P1 修复**。

### 4.14 s19o-p1d 实测：`GLYPH_TRIM=6` 清错端，P1 仍未修复

> 归档：`/host/iosvm/logs/s19o-p1d/`（`report.md`、`bands.txt`、`report/cmp_*.png`、`report/desc_*.png`、四张 dump、日志、截图）。**这些文件都不进 git**。
> 这是 **P1 诊断发**；P1 仍未修复。

**口径**

| 项 | 值 |
|---|---|
| label / 时间 | `s19o-p1d` / 2026-11-08 10:52:40Z–11:09:31Z |
| 口径 | `XCMD=glwire_local=1`；`HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_GLYPH_TRIM=6 GLWIRE_DUMP=/tmp/s19o-p1d/f GLWIRE_DUMP_AT=900 GLWIRE_DUMP_PAIR=1"`（不 skip、不 sanitize、无 ATLAS/TEXT） |
| `end=` | `end=ui`：`ui=690s`，`ui_bytes=313488`；最大截图 **331,028 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **1,160** 帧 |

**trim 日志**

`grep -c 'glyph trim'` = **8**（打印上限），全部 `rows 6`：

```text
161x90  113x42  184x90  136x42  144x90  96x42  149x90  101x42
```

尺寸与 P1c 的 glyph tile 同族（42/90 高）；说明 trim 确实执行，但执行位置不对。

**14 标签 band（quad 底部 10 px，ref=v9）**

| 标签 | v9 (nb/32/64) | P1d | 变化 |
|---|---|---|---|
| Weather | 719/371/233 | 745/470/374 | +3.6%（更高的竖条） |
| FaceTime | 282/125/60 | 242/83/37 | -14% |
| Calendar | 494/279/142 | 306/195/116 | -38% |
| Photos | 539/247/147 | 294/128/76 | -45% |
| Mail | 424/253/135 | 460/289/219 | +8.5% |
| Notes | 361/216/111 | **993/644/438** | **+175%** |
| Reminders | 163/90/45 | 62/20/4 | -62% |
| TV | 0/0/0 | 0/0/0 | — |
| Podcasts | 131/72/37 | 95/25/5 | -27% |
| Health | 343/245/144 | **1498/1421/922** | **+337%** |
| Wallet | 43/2/0 | **1551/461/0** | 大幅增加（竖条拉长） |
| App Store / Maps / Settings | 0/0/0（此刻未显示） | 0/0/0 | — |
| **合计** | **3499/1900/1054** | **6246/3736/2191** | **178.5% / 196.6% / 207.9%** |

- 目标「显著低于 3499」未达成，反而升到 **6246（178.5%）**。
- 视觉：v9 的断续碎点 → P1d 变成**更高的规则竖条**（Weather/FaceTime/Photos 以及 Notes/Health/Wallet 都出现更规则的竖条；其中 Notes/Health/Wallet 数值增幅最大，见 `report/cmp_*.png`）。
- 部分标签（FaceTime/Calendar/Photos/Reminders/Podcasts）确实下降，说明 trim 清掉了一些行，但整体被 Notes/Health/Wallet 的竖条拉高——**不是全局清理**。

**descender 专项（没有被削）**

| 标签 | v9 ink (nb/>32/>64) | P1d ink | 结论 |
|---|---|---|---|
| Photos | 1968/1514/1329 | 1968/1514/1329 | 完全不变 |
| Podcasts | 2628/2082/1783 | 2628/2082/1783 | 完全不变 |

→ `g/p/y` 的降部没有被削，文字本体完整；App Store/Maps/Settings 此刻未上屏，无法比。

**dump / panel（非 P1 判据，顺带记录）**

- `f`/`f-n1` 四件齐全；`diffdump f-n1 --tolerance 8` = **99.98% SAME**（missing 0、extra 0、differing 191/0.02%）。
- panel 真实：11,769 色、pure green 0、f 与 f-n1 md5 不同；cross 100.00%。

**判读（重要）**

- **`GLWIRE_GLYPH_TRIM=6` 按现状不是修复**：合计 band 反而 +78%，点带变成更高的竖条；文字/descender 完好。
- 当前实现清的是**上传缓冲的「最后 n 行」**（`names_glyph_trim` 对 `y=h-n..h-1` 置零）。但 P1c 的 `glReadPixels` dump 证明 **raw 文件序 = 屏幕正立，点带在文件末尾**；如果上传缓冲行序与 glReadPixels 相反（GL 自底向上 vs guest 自顶向下），「最后 6 行」清到的就是**视觉顶部**（42 高 tile 顶部本来空白；90 高 tile 清到背板顶边）——既没清掉点带，又改变了背板合成，解释了「竖条变大」。
- 因此**大概率是清错端**，不是「trim 无效」；P1 仍未修复，不要把本条写成修复。

**下一步**

1. code 复核上传缓冲行序与 P1c dump 的对应关系（建议 trim 前后各做一次 ATLAS_DUMP，或打印被清第一/最后一行的指纹）。
2. 增加 **`GLWIRE_GLYPH_TRIM_FROM=start|end`**（或等价参数），明确清「视觉底部 6–8 行」而不是固定「缓冲最后 n 行」；注意 90 高背板 tile 的点线在背板下缘之外，行数可能不同。
3. **P1e** 用 `TRIM_FROM=start`（或其他修正后的 TRIM）验证点带消失且 descender 不被削；P1e 数据前 P1 保持「进行中」。

### 4.15 s19o-p2 实测：lookup 修好、PosterBoard 起来了，但壁纸未出现（P2 未达成）

> 归档：`/host/iosvm/logs/s19o-p2/`（`report.md`、`build-p2.log`、`apfs-verify.txt`、`bands.txt`、`report/`、日志、截图）。**这些文件都不进 git**。
> P2 第一次 boot；**lookup 链修好，但壁纸判据三项里只过第一项**，P2 未达成。

**构建与镜像**

| 项 | 值 |
|---|---|
| 改动 | `userland/build.sh` 三处：`root/LaunchAngels` + 拷 `com.apple.PosterBoard.plist`；`sysroot/System/Library/LaunchAngels -> /redwood/LaunchAngels` |
| 命令 | `PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop SYSTEM=1 ./build.sh` |
| 产物 | `root.apfs` **5,902,336 B**；`system.apfs` **2,476,748,800 B** |
| 核验 | system：`LaunchAngels` = symlink → `/redwood/LaunchAngels`；root：`/LaunchAngels/com.apple.PosterBoard.plist` 存在（1000 B）；`/lib` 的 libhakios 90,128 / libhakios.sb 108,040 / libhakios.gl 401,816 / libOSMesa 14,805,208 与备份树逐项相同，**无回退** |
| 备份 | `/tmp/system.apfs.bak-P2`、`/tmp/root.apfs.bak-P2`、`/tmp/userland-root.bak-P2`、`/tmp/prebuilt-userland.bak-P2` |

**Boot**

| 项 | 值 |
|---|---|
| 配方 | `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MAXSECS=2400 UI_GRACE=300 MINSHOT=200000`；`XCMD=glwire_local=1`；`HOST_ENV=GLWIRE_NO_PANEL=1`（trim/sanitize/dump 全关） |
| `end=` | `end=ui`：`ui=660s`，`ui_bytes=313636`；最大截图 **324,944 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **1,023** 帧 |

**判据逐条**

| 判据 | 结果 | 证据 |
|---|---|---|
| `posterboardservices` lookup 不再失败 | ✅ | P2 日志中 `posterboardservices` **0 次**；v9 有 2 次 `No such process` |
| PosterBoard 起来并持续运行 | ✅ | `Successfully spawned PosterBoard[43] because launch job demand`；`service state: running`；无 exit/kill |
| 宿主出现 1125x2436 或全屏非 panel 纹理 | ❌ | host 上传 1027×`750x1624`（panel `4294901761`）+ 180x180/28x95/6x85 等图标；**无 1125x2436，也无第二张全屏非 panel 纹理** |
| 主屏背景非纯黑 | ❌ | 截图左右边缘（x0..60 / x1060..1120，y700..1300）非黑像素 **0**；顶部只有红条，背景纯黑 |

**为什么没出壁纸：PosterBoard 没崩，缺依赖/流程**

PosterBoard（pid 43）日志：

- `failed lookup: com.apple.managedconfiguration.profiled.public`（**10 次**）
- `failed lookup: com.apple.powerlog.plxpclogger.xpc`（**4 次**）
- `Invalid XPCService dictionary` + `Could not get domain target from bundle: .../MediaConversionService.framework/XPCServices/com.apple.photos.VideoConversionService.xpc`（**error 22: Invalid argument**）；ImageConversionService 同样
- `Unable to find persona with type 6: 3 - No such process`（**2 次**，11:58）
- SpringBoard 侧 shim：`poster switcher not offered`
- **没有 SIGSEGV/exit**：进程一直 running；也就是说它起来了，但**没有生成 poster/wallpaper 表面**，也没有上传壁纸纹理。

**重要副作用：重建后 P1 基线变了**

- 本发 **trim/sanitize 都没开**（host log `glyph trim`=0、`tc0 sanitized`=0），但 14 标签 band 合计 = **6989/3970/2652（v9 3499 的 199.7%）**，与 P1d（trim=6，6246）同量级。
- 说明重建后的镜像或该次 UI 状态改变了标签渲染；**不能把变化归因于 trim**。P1e 进一步把漂移指向 **host binary 版本（`ec129181`→`6584cdf`）或 UI 时刻**，而不是镜像/字体；P1 对比要固定 host binary。
- **P1e 起用 `6989` 作新 baseline**；旧的 v9 `3499` 不再直接可比。若 P0/P1 要基于新镜像继续，先做一次「旧镜像 + 新 host」或「新镜像 + 旧 host」的对照，确认点带变化来源；旧镜像备份在 `/tmp`。

**结论与下一步**

- lookup 链修好、PosterBoard 启动并 running，但**壁纸未出现，P2 未达成**。
- 不是 PosterBoard 崩溃，而是依赖/流程没走完：上述 lookup/XPC/persona 失败 + SpringBoard shim 是下一层入口。
- 下一步：code 离线查 PosterBoard 依赖链（managedconfiguration/powerlog/MediaConversion/persona）；不要在没有新证据时重复重建镜像。
- **P1e** 已用 P2 的新 baseline `6989` 验证 `TRIM_FROM=start`（结果见 §4.16）：trim 不是干净修复，trim 方向停止；P1 方向暂停，优先 P2 依赖/P3。

### 4.16 s19o-p1e 实测：`TRIM_FROM=start` 清的是空白顶行，trim 方向停止

> 归档：`/host/iosvm/logs/s19o-p1e/`（`report.md`、`bands.txt`、`fnv.txt`、`report/cmp_*.png`、`report/desc_*.png`、四张 dump、日志、截图）。**这些文件都不进 git**。
> P1 诊断发；**P1 仍未修复，trim 方向到此停止**。

**口径**

| 项 | 值 |
|---|---|
| label / 时间 | `s19o-p1e` / 2026-11-08 11:45 前后（host binary 11:45:43Z） |
| host binary | `6584cdf`（`GLWIRE_GLYPH_TRIM_FROM`，默认关） |
| 口径 | `XCMD=glwire_local=1`；`HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_GLYPH_TRIM=6 GLWIRE_GLYPH_TRIM_FROM=start GLWIRE_DUMP=/tmp/s19o-p1e/f GLWIRE_DUMP_AT=900 GLWIRE_DUMP_PAIR=1"` |
| 镜像 | P2 重建后的 `root.apfs` 5,902,336 B / `system.apfs` 2,476,748,800 B；baseline = P2 同镜像、trim 关的 **6989/3970/2652** |
| `end=` | `end=ui`：`ui=660s`，`ui_bytes=312638`；最大截图 **331,781 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **980** 帧 |

**trim 日志与 FNV 行序**

8 行，均 `rows 6 from start`：

```text
152x90 first=6e46998a last=cbaa45e1   104x42 first=2d2af4e5 last=2d2af4e5
187x90 first=f9bdc11b last=1535f4d6   139x42 first=f4fcb597 last=f4fcb597
196x90 first=3e641207 last=a55e6bd7   148x42 first=9d899e55 last=9d899e55
189x90 first=0619a9fb last=a234f521   141x42 first=bf5e70ef last=bf5e70ef
```

与 P1c atlas raw 行做 FNV-1a：

| tile | trim first | P1c dump row 0 | 结论 |
|---|---|---|---|
| 152x90 | `6e46998a` | **`6e46998a` ✅** | upload row0 = atlas raw row0 |
| 187x90 | `f9bdc11b` | **`f9bdc11b` ✅** | 同上 |
| 104x42 | `2d2af4e5` | **`2d2af4e5` ✅** | rows 0–5 是**空白行**（= 104 零字节的 FNV） |
| 139x42 | `f4fcb597` | **`f4fcb597` ✅** | rows 0–5 是**空白行**（139 零行） |

→ **upload 缓冲 row 0 = P1c atlas raw row 0 = 视觉顶部**。`from=start` 清的是**顶部 6 行**（42 高 tile 本来就空白；90 高 tile 清到背板顶边），**点带在视觉底部，start 清不到**。P1d 的 `from=end` 才清视觉底部，但会把剩余点行拉成更高竖条。

**band：P2 同镜像 baseline vs P1e（14 标签）**

| 标签 | P2 baseline | P1e (start) | 变化 |
|---|---|---|---|
| Weather | 743/472/360 | 538/390/219 | **-28%** |
| FaceTime | 1385/1031/663 | 967/480/297 | **-30%** |
| Calendar | 1281/962/610 | 420/245/154 | **-67%** |
| Photos | 79/37/27 | 143/69/43 | **+81%** |
| Mail | 694/591/443 | 474/300/215 | **-32%** |
| Notes | 753/457/277 | 626/181/101 | -17% |
| Reminders | 218/135/98 | 186/109/67 | -15% |
| TV | 0/0/0 | 0/0/0 | — |
| Podcasts | 1302/111/69 | 565/223/110 | **-57%** |
| Health | 507/174/105 | 864/630/459 | **+70%** |
| Wallet | 27/0/0 | 363/222/126 | **+1244%** |
| App Store/Maps/Settings | 0/0/0（此刻未显示） | 0/0/0 | — |
| **合计** | **6989/3970/2652** | **5146/2849/1791** | **73.6% / 71.8% / 67.5%** |

- 合计降了约 26%，但**分标签有升有降**：Calendar -67%、Podcasts -57%、Mail -32%、FaceTime -30%、Weather -28%；**Wallet +1244%、Health +70%、Photos +81%**。
- 视觉：Weather/Calendar/Podcasts 变干净；Wallet/Health 从干净变成一排竖条。全图 P2 vs P1e：different 14,608（0.53%），within 8 = 99.47%，差异集中在标签带。
- descender：Photos `1968/1514/1329`、Podcasts `2628/2082/1783` 与 P2 **完全一致** → g/p/y 未削。

**判定**

- **trim 不是干净修复**：start 清空白顶行、end 清视觉底部但产生更高竖条；点带可见性由 **90 高背板 / 复合层（prog13/17/19）**共同决定，不是「上传 tile 底部固定 6 行」这么简单。
- **停止 trim 方向**：不再用 `GLYPH_TRIM=6`/固定行数重试。下一步转查 **90 高背板 tile / 复合层的点带来源**，或对 u0 纹理做「点带行位置」测量（按位置清，而不是固定 6 行）。
- **P1 方向暂停**，优先 P2 依赖修复与 P3；P1 仍未修复。

**旁注：baseline 漂移更像 host binary/UI 时刻**

- P1c 的 tile 字节与 P1e 的 FNV 一致（同一 tile 内容），而 P2 baseline 6989 远高于 v9 3499 → 漂移更可能来自 **host binary 版本（`ec129181`→`6584cdf`）或 UI 时刻**，不是镜像/字体。
- 后续 P1 对比要**固定 host binary**，并记录 UI 时刻；不要再拿 v9 的 3499 直接比新镜像。

### 4.17 s19o-p2 根因分析：不是依赖卡死，更像 hosted wallpaper scene 没接上

> 材料：`/host/iosvm/logs/s19o-p2-root-cause.md`（离线分析）、`/host/iosvm/logs/s19o-p2/` 的日志、`/host/iosvm/logs/rc.p2diag`（P2b 的 rc 文件）。**这些文件都不进 git**。
> 结论：**没有证据表明 PosterBoard 被依赖卡死，也没有崩溃**；更可能是 **hosted wallpaper scene 没被 SpringBoard 接上 / 没有 poster 配置**。下一步用层树诊断，不盲加 daemon。

**时间线（PosterBoard[43]）**

| 时间 | 事件 | 读法 |
|---|---|---|
| 11:55:01.77 | launchd `WILL_SPAWN`；`kpersona_find_by_type` type 6 失败 | persona 错误在 spawn 前就有，系统级，不是 PosterBoard 特有 |
| 11:55:03.81 | `service spawned with pid 43`，state running | 进程起来了 |
| 11:55:07.53 | `failed lookup: com.apple.managedconfiguration.profiled.public` | 早，但 24 s 后仍在继续 |
| 11:55:31.20 | `failed lookup: com.apple.powerlog.plxpclogger.xpc` | 同上，不是立即放弃 |
| 11:55:34.58 | `(pid/43) uncorking domain` | app 主体开始 |
| 11:55:48 | 大量 `Service stub created`；MediaConversion Video/ImageConversion `error 22` | 框架初始化；错误来自 Photos 转换服务，不是壁纸 |
| 11:57:22–11:58:09 | 继续 `Service stub created`（FontServices/CoreAccessories/SensorKit 等） | 仍在初始化，没有卡在某个 lookup |
| 11:58:21–11:58:34 | `extensionkitservice` stub→spawn pid 48→running；11:58:32 persona 错误 | PosterBoard 主动向 ExtensionKit 要扩展 |
| 11:59:47 | `extensionkitservice` `job state = running` | 之后到结束（12:02）没有 pid/43 新行 |

- **没有 PosterBoard 创建的 IOSurface**：全日志 `MiniIOSurface: ... created surface` 只有 backboardd/SpringBoard，PosterBoard[43] **0 条**。
- **进程没有退出/被杀**：无 exit/SIGKILL/panic；只是没有更多日志。
- **没有 `/var/mobile/Library/{PosterBoard,PaperBoard,Wallpaper}` 写出**：poster 数据模型没落盘，不能只怪显示。

**注意：PosterBoard 没 surface ≠ 黑屏证据**

壁纸可能是 PosterBoard 在另一个进程 live 画、host 进 SpringBoard 的 layer tree；SpringBoard 自己只有 placeholder/没有 bitmap。PosterBoard 的 host scene 可能**不新建自己的 MiniIOSurface**。所以「0 条 PosterBoard surface」只能说明它没走到自己的渲染/快照，**不能单独判死**。真正要回答的是 SpringBoard 层树里有没有 wallpaper 层及其 contents——P2 没触发 `com.hakios.layers`，这份证据缺。

**依赖审计：存在/加载/最小修法**

| 依赖 | 镜像里 | P2 是否加载 | 结论 / 最小修法 |
|---|---|---|---|
| `com.apple.managedconfiguration.profiled.public` | plist + `/usr/libexec/profiled` 都在 | **否**：root.apfs `/LaunchDaemons` 无此 job | 加 job（低代价）；但 PosterBoard 失败后继续走 3 分钟，**非致命嫌疑大** |
| `com.apple.powerlog.plxpclogger.xpc` | `com.apple.powerlogHelperd.plist` + `/usr/bin/powerlogHelperd` 在 | **否** | 加 `powerlogHelperd` job；遥测，非壁纸关键 |
| MediaConversion Video/ImageConversion | 两个 `.xpc` 的 Info.plist 与二进制都在；`XPCService=False`（老式，需要 launchd job） | **否**；launchd 报 `Invalid XPCService dictionary/error 22` | 加两个 Photos job（低代价）；**只影响 Photos 转换，不影响壁纸** |
| `com.apple.locationd.synchronous` | plist 有，但 `/usr/libexec/locationd` **二进制缺失** | 否 | 不能简单加 job；stub 同步 XPC 协议代价高，**不做** |
| persona type 6 | personas plist 在；`kpersona_find_by_type` 在 wifid/chronod/backboardd/SpringBoard 都失败 | 系统级 | 不是 PosterBoard 特有；伪造 persona 影响全局，**不动** |
| `posterboardservices.dataModel` / `data-store` | **没有 launchd plist**；只有客户端字符串 | 无 provider 可见 | 更像 SpringBoard/PosterBoard **进程内 BSService**；加 plist 修不了，需 layer/runtime 证据 |
| `chronoservices` / `extensionkitservice` | chronod running；extensionkitservice 由 PosterBoard 拉起 pid 48 | 是 | 不是缺失项；但 `CollectionsPoster` 等 poster provider appex 没看到被拉起，ExtensionKit 发现结果仍是问号 |
| 默认壁纸素材 | `/Library/Wallpaper/.../iOS_16`（id 7400）与 `DefaultWallpapers~iphone.plist` 都在 system.apfs | — | **素材不缺**；缺的是 poster 配置/数据模型或 layer 连接 |

**判断**

- 没有证据是依赖卡死：profiled/powerlog/MediaConversion 的失败都发生在 PosterBoard 继续初始化之前/期间；它之后持续创建几十个 service stub，并主动找 extensionkitservice。
- 黑屏更像 **PosterBoard hosted wallpaper scene 没有被 SpringBoard 接上 / 没有 poster 配置**，不是缺某个 daemon。
- **不要写成「PosterBoard 崩溃」**：它没有崩，全程 running，没有 exit/SIGKILL/panic。

**P2b 诊断（已派）：层树 + 更长 grace，不混发**

- 在 `userland/` 放 `rc.p2diag`（`/host/iosvm/logs/rc.p2diag`，复制 `rc.vtop` 加一行）：
  ```sh
  ( /redwood/bin/rwtouch wait 900 notify com.hakios.layers ) &
  ```
- 构建：**只重建 root.apfs**，`RC=rc.p2diag`，**不设 `SYSTEM=1`**（system.apfs 不动）：
  ```sh
  cd "$KERNEL/userland"
  cp -a root.apfs /tmp/root.apfs.bak-P2b
  cp /host/iosvm/logs/rc.p2diag rc.p2diag
  PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.p2diag ./build.sh
  ```
- Boot（完整命令；`MAXSECS` 真正生效的是第三个位置参数 `3000`，环境变量会被脚本覆盖）：

  ```sh
  cd /host/iosvm
  export REPO=/host/iosvm
  export KERNEL=/host/iosvm-work/hakios-xnu-kernel
  export GLWIRE=$REPO/tools/glwire
  export ROOT=$KERNEL/userland/root.apfs
  LABEL=s19p2b-1
  mkdir -p /tmp/$LABEL

  XCMD="glwire_local=1" \
  ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MINFRAMES=99999 MAXSECS=3000 UI_GRACE=900 MINSHOT=200000 \
  HOST_ENV="GLWIRE_NO_PANEL=1" \
  IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
  tools/glwire/harness/boot-measure.sh "$LABEL" "$ROOT" 3000
  ```
- 判据：
  1. **层树**（`hakios: layers...`）里出现/不出现 `SBWallpaperEffectView` / `PBUIWallpaperView` / `PRSPoster` / `CALayerHost`，以及 `frame`、`contents`、`opacity`；若出现 `CALayerHost`，是否对应 PosterBoard 的 context id。
  2. **宿主**：是否出现第二张全屏（750x1624 非 panel）纹理，或壁纸区域的 draw。
  3. **截图边缘**是否仍全黑。
- 分支：
  - 有 wallpaper 层且 `contents` 非空 → 壁纸路径通，问题在内容/快照；
  - 有层但 `contents` 空 → 内容/快照；
  - 完全没有 wallpaper 层 → **下一发才加 4 个 job 对照**（`managedconfiguration.profiled`、`powerlogHelperd`、Video/ImageConversion），**不要和层树诊断混在同一 boot**，否则分不清是时间、job 还是层树变化。
- 不要在同一发加 daemon；不要动 locationd/persona stub。

> **P2b 已执行（结果见 §4.18）**：`rc.p2diag` 的 `rwtouch` 后台子进程没有活过 `exec /sbin/launchd.darwin`，层树 **0 行**；这是触发失败，不是「没有 wallpaper 层」的结论。下一发 P2c 改用进程内 `dispatch_after`。

### 4.18 s19o-p2b 实测：层树触发没到 SpringBoard，P2 仍未达成

> 归档：`/host/iosvm/logs/s19o-p2b/`（`report.md`、`build-p2b.log`、`apfs-verify.txt`、日志、截图）。**这些文件都不进 git**。
> P2b 诊断发；**没有拿到层树，P2 仍未达成**。

**镜像（只重建 root.apfs）**

| 项 | 值 |
|---|---|
| 构建 | `PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.p2diag ./build.sh`（**未设 `SYSTEM=1`**） |
| root.apfs | 5,902,336 B，md5 **`b2e919915b42e9f3aa7e7059a632d67d`** |
| system.apfs | 未动：2,476,748,800 B，md5 **`b75d80caaa5e276a58a4d45307d2165b`**（mtime 仍是 11:42） |
| 核验 | `/etc/rc` md5 = `c50ae661…`（与 `rc.p2diag` 一致）；`/bin/rwtouch` 66,832 B；libhakios 90,128 / sb 108,040 / gl 401,816 / libOSMesa 14,805,208，未回退 |
| 备份 | `/tmp/root.apfs.bak-P2b`（P2 版本，md5 `b20d670f…`） |

**Boot**

| 项 | 值 |
|---|---|
| 配方 | `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MAXSECS=3000 UI_GRACE=900 MINSHOT=200000`；`XCMD=glwire_local=1`；`HOST_ENV=GLWIRE_NO_PANEL=1` |
| `end=` | `end=ui`：`ui=690s`，`ui_bytes=309072`；最大截图 **326,206 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **3,771** 帧 |

**层树：0 行，触发没到 SpringBoard**

- guest log：`hakios: layers` = **0 行**；`SBWallpaperEffectView` / `PBUIWallpaperView` / `PRSPoster` / `CALayerHost` = **0**。
- `rwtouch:` 输出 = **0 行**：没有 `notification com.hakios.layers: posted`，也没有 `/dev/hakios-touch` 的 perror。
- 但 rc 确实执行了，日志有：

  ```text
  $ ( /redwood/bin/rwtouch wait 900 notify com.hakios.layers ) &
  $ exec /sbin/launchd.darwin
  ```

  且 `/dev/hakios-touch` 在 log 的 ls 里存在（`crw-rw-rw-`）。
- 判定：**触发没有到达 SpringBoard**（或 rwtouch 没跑过 900s）。最可能是 rwtouch 作为 rc 的后台子进程，在 `exec /sbin/launchd.darwin` 之后没有继续（或输出丢失）；因此没有 `posted` 行，SpringBoard 的 `notify_register_dispatch("com.hakios.layers")` 回调自然也没跑。
- **这不是「没有 wallpaper 层」的结论**；只是这一发的触发方式失败。

**壁纸/背景：仍然失败**

- host 上传：**3,776 张 `750x1624`**，其中 **3,772 张是 panel `4294901761`**；另有 1/4/7/10 四张启动期全屏纹理；**没有第二张持续的全屏非 panel 纹理，没有 1125x2436**。
- 截图背景：左右边缘各 **0 / 36,000** 非黑；顶部条只有红条 → **仍纯黑**。
- PosterBoard：pid 44，`service state: running`；依赖错误与 P2 相同（managedconfiguration/powerlog/persona 等）。

**结论与 P2c**

- P2b **没有拿到层树**，不能判断「有 wallpaper 层但 contents 空」还是「完全没有 wallpaper 层」；P2 **未达成**。
- P2c（已派）：在 `inject_sb.c` 里加**进程内 `dispatch_after`** + boot arg **`layers_at=900`**（默认关），不再依赖 rc 的后台子进程。exp 用普通 `rc.vtop` 重建 root.apfs：

  ```sh
  export KERNEL=/host/iosvm-work/hakios-xnu-kernel
  cd "$KERNEL/userland"
  cp -a root.apfs /tmp/root.apfs.bak-P2c
  PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh
  ```

  boot：

  ```sh
  cd /host/iosvm
  export REPO=/host/iosvm
  export KERNEL=/host/iosvm-work/hakios-xnu-kernel
  export GLWIRE=$REPO/tools/glwire
  export ROOT=$KERNEL/userland/root.apfs
  LABEL=s19p2c-1

  XCMD="glwire_local=1 layers_at=900" \
  ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MINFRAMES=99999 MAXSECS=3000 UI_GRACE=900 MINSHOT=200000 \
  HOST_ENV="GLWIRE_NO_PANEL=1" \
  IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
  tools/glwire/harness/boot-measure.sh "$LABEL" "$ROOT" 3000
  ```

- P2c 先验：guest log 出现 **`layers timer 900s fired`**（或等价日志）与 `hakios: layers`；若 timer 不 fired，先查 boot arg 是否传到 SpringBoard，不要先加 job。
- **仍然不加 4 个 job**；P2c 只解决「层树触发」，wallpaper 层有无/contents 是否有值是下一层判断。
- **实际执行的是 P2c-alt（LaunchDaemon + rwtouch，见 §4.19）**：机制成功，但 SpringBoard 的 handler 没跑；P2c-alt2 做通知通道对照。

### 4.19 s19o-p2c-alt 实测：LaunchDaemon 机制成功，SpringBoard handler 没跑

> 归档：`/host/iosvm/logs/s19o-p2c-alt/`（`report.md`、`build-p2c-alt.log`、`apfs-verify.txt`、日志、截图）。**这些文件都不进 git**。
> P2c 的替代路线；**机制通了、内容失败**，P2 仍未达成。

**镜像（仅重建 root.apfs）**

| 项 | 值 |
|---|---|
| 构建 | `PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh`（**不设 `SYSTEM`**） |
| root.apfs | 5,902,336 B，md5 **`a10680fa64b7e24907dc0cd39ea70dc7`** |
| system.apfs | 未动：2,476,748,800 B，md5 **`b75d80caaa5e276a58a4d45307d2165b`** |
| 新 job | `/LaunchDaemons/com.iosvm.layers.plist` **837 B**；`plistlib` 解析 OK：Label `com.iosvm.layers`、Args `/redwood/bin/rwtouch wait 900 notify com.hakios.layers`、`UserName mobile`、`RunAtLoad=true` |
| 核验 | `/etc/rc` 与本地 `rc.vtop` md5 一致（`b0f1d6b0…`）；libs 90,128 / 108,040 / 401,816 / 14,805,208，未回退 |
| 备份 | `/tmp/root.apfs.bak-P2c-alt`（`b2e9199…`，P2b 版） |

**Boot**

| 项 | 值 |
|---|---|
| 配方 | `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MAXSECS=3000 UI_GRACE=900 MINSHOT=200000`；`XCMD=glwire_local=1`；`HOST_ENV=GLWIRE_NO_PANEL=1` |
| `end=` | `end=ui`：`ui=690s`，`ui_bytes=310639`；最大截图 **328,595 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **3,479** 帧 |

**判据 a：launchd / rwtouch 成功**

- `pending spawn, domain in on-demand-only mode: com.iosvm.layers` → `WILL_SPAWN` → `launching: speculative`（14:49:42）。
- **`(user/501/com.iosvm.layers [19])`**：`Successfully spawned rwtouch[19]`（14:51:34，`user/501` = mobile 域 ✅）。
- 900s 后：**`rwtouch: notification com.hakios.layers: posted`**（15:07:49），随后 `EXITED, code = 0`。
- 同一次注入里的 `dispatch_after(1s)` 主队列块**执行了**（`hakios: battery: SOC=-1% …`）→ SpringBoard 主队列正常、`say` 输出可达。

**判据 a：层树仍然 0**

- `hakios: layers` = **0 行**；`SBWallpaperEffectView` / `PBUIWallpaperView` / `PRSPoster` / `CALayerHost` = **0**。
- SpringBoard[9] 全程 running，无 exit/restart/崩溃。
- dylib 侧：`obj/libhakios.sb.dylib` strings 含 `_notify_register_dispatch` 与 `com.hakios.layers`（另含 `unlock`/`switcher`），说明注册代码在二进制里。
- **结论**：`notify_post` 返回成功，但 SpringBoard 的 handler 没有跑。可能原因：
  1. 跨进程 Darwin notify 在该环境没有送达（`posted` 只代表 API 成功，不代表有监听者收到）；
  2. 注册与 post 不在同一个 notify 命名空间/会话（尽管二者同为 `user/501`）；
  3. 现有 dylib（11-07 构建）的 layers 注册与源码现状不完全一致（源码今天加了 timer，notify 注册行更早）。

**判据 b：壁纸/背景仍失败**

- host 上传：3,483 张 `750x1624`，其中 **3,479 张是 panel `4294901761`**；另有 1/4/7/10 四张启动期全屏；**无第二张持续非 panel 全屏纹理、无 1125x2436**。
- 截图左右边缘各 **0 / 36,000** 非黑 → **背景仍纯黑**。
- PosterBoard 照旧 running，依赖错误与 P2 相同。
- **这是触发/注册问题，不是壁纸结论**：没有层树就不能判断 wallpaper 层有无。

**下一步：P2c-alt2 通知通道对照**

- 用**同一个 LaunchDaemon 机制**，把 `ProgramArguments` 改成依次 post **已知 handler** `com.hakios.unlock` / `com.hakios.switcher`：

  ```text
  /redwood/bin/rwtouch wait 900 notify com.hakios.unlock wait 5 notify com.hakios.switcher
  ```

  若 `rwtouch` 不支持一条命令里连续动作，就拆成两发（alt2-unlock / alt2-switcher），**不要同时改域/timing/代码**。
- 构建仍只重建 root.apfs（`RC=rc.vtop`、不设 `SYSTEM`；plist 由 build.sh 拷入），备份 `/tmp/root.apfs.bak-P2c-alt2`。
- Boot 同 P2c-alt：`UI_GRACE=900`、`MAXSECS=3000`、`XCMD=glwire_local=1`。
- 判据：
  - `com.hakios.unlock` → 日志出现 **`hakios: unlock:`**（`asked/not locked` 都算 handler 跑过）；
  - `com.hakios.switcher` → 日志出现 **`hakios: the switcher:`**（即使屏幕不变，也证明 handler 跑过）；
  - 任一出现 → **notify 通道正常**，layers 是注册/版本问题，等 code 带 timer 的新 dylib；
  - 两者都不出现 → Darwin notify 跨进程在此环境不可达，只能走编译的 `layers_at` timer。
- 保持 900s（UI 690s 后 3.5 分钟，SpringBoard 已注册）；**不要再用 300s 或盲改 plist 域/timing**。

> **P2c-alt2 已执行（§4.20）**：unlock/switcher 两条都 `posted`，但三个 handler 一个都没跑；**notify 通道不可用**，转编译的 `layers_at` timer。

### 4.20 s19o-p2c-alt2 实测：notify 通道不可用，转编译 `layers_at`（P2 未达成）

> 归档：`/host/iosvm/logs/s19o-p2c-alt2/`（`report.md`、`imgdiff.txt`、`apfs-verify.txt`、日志、截图）。**这些文件都不进 git**。
> P2c-alt2 通道对照发；**notify 发得出去、送不到**，P2 仍未达成。

**镜像（仅重建 root.apfs）**

| 项 | 值 |
|---|---|
| 构建 | `PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh`（**不设 `SYSTEM`**） |
| root.apfs | 5,902,336 B，md5 **`5f8ba9ea793aa04683bc6e93e1c994e9`** |
| system.apfs | 未动：2,476,748,800 B，md5 **`b75d80caaa5e276a58a4d45307d2165b`** |
| plist | Args = `/redwood/bin/rwtouch wait 900 notify com.hakios.unlock notify com.hakios.switcher`；`UserName mobile`；stdout/err=`/dev/console`；镜像内解析一致 |
| 核验 | `/etc/rc` = `rc.vtop`；libs 90,128 / 108,040 / 401,816 / 14,805,208，未回退 |
| 备份 | `/tmp/root.apfs.bak-P2c-alt2`（`a10680fa…`，P2c-alt 版） |

**Boot**

| 项 | 值 |
|---|---|
| 配方 | `ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MAXSECS=3000 UI_GRACE=900 MINSHOT=200000`；`XCMD=glwire_local=1`；`HOST_ENV=GLWIRE_NO_PANEL=1` |
| `end=` | `end=ui`：`ui=660s`，`ui_bytes=308955`；最大截图 **326,744 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **3,517** 帧 |

**机制：launchd + 两条 posted 都成功**

- `(user/501/com.iosvm.layers [19])` → `Successfully spawned rwtouch[19]`（**user/501**，mobile 域）。
- 900s 后：

  ```text
  rwtouch: notification com.hakios.unlock: posted
  rwtouch: notification com.hakios.switcher: posted
  ```

**handler：三个一个都没跑**

- `hakios: unlock` 全 log 只有 **1 行**：`hakios: unlock: asked, answer 1` —— 出现在 boot 早期（15:46 前的自动解锁，10s 保险），**在 15:51 的通知之前**，不是通知触发的。
- `hakios: the switcher` / `hakios: layers` = **0 行**。
- `SBWallpaperEffectView` / `PBUIWallpaperView` / `PRSPoster` / `CALayerHost` = **0**。
- 截图：shot vs final `within tolerance 8 = 99.88%`（差异 bbox 只在 `590,212–1057,1187`，是时钟/widget 刷新），**没有 switcher 画面**；左右边缘非黑 **0/36,000** → 背景仍纯黑。
- 对照组：同一次注入的 `dispatch_after(1s)` 主队列块照常执行（`hakios: battery: SOC=-1%`），SpringBoard[9] 全程 running。

**判定**

- LaunchDaemon + rwtouch 链路完全正常（域、运行、输出、退出码都对）。
- **`notify_post` 返回 OK ≠ 投递成功**：unlock/switcher/layers 三个 handler 都没跑，说明跨进程 Darwin notification 在这个环境/这一代 dylib 里没有送达（或注册不在 SpringBoard 的 notify 命名空间）。
- **notify 通道不可用**；不再盲改 plist/timing。转 **SpringBoard 进程内 `dispatch_after` 的 `layers_at` timer**（`inject_sb.c`，默认关），不依赖 rwtouch/notify。
- **这是触发/通道问题，不是壁纸结论**；没有层树仍不能判断 wallpaper 层有无。P2 未达成。

**阻塞项：exp 容器编不了新 dylib**

- `layers_at` 需要新 `libhakios.sb.dylib`，但 **exp 容器没有 clang/SDK**，无法编译。
- 需要**宿主编译产物**：`userland/obj/libhakios.sb.dylib`（含 `layers_at`，来自 `c056fb6`/对应 patch），或给 exp 容器挂工具链。
- 拿到产物后：只重建 root.apfs（`RC=rc.vtop`，不设 SYSTEM），boot `XCMD="glwire_local=1 layers_at=900"`、`UI_GRACE=900 MAXSECS=3000`；先验 `layers timer 900s fired` 与 `hakios: layers`，仍不加 4 个 job。
- **P3 guest 同样依赖这个编译环境**：没有新 dylib，P3 的 guest 侧代码也进不了镜像。

> **该阻塞已于 2026-11-09 解除**：owner 把工具链烤进镜像/同步到 4 容器；exp 已编出 108,248 B 的新 `libhakios.sb.dylib`，P2c-timer 已跑完。路径与规矩见 **附录 C**，实测见 **§4.21**。

### 4.21 s19o-p2c-timer 实测：timer 成功、壁纸 VC 链在屏，但 contents 为空（P2 未达成）

> 归档：`/host/iosvm/logs/s19o-p2c-timer/`（`report.md`、`layers.txt` 404 行层树、日志、截图、`apfs-verify.txt`）。**这些文件都不进 git**。
> P2c-timer 实测；**timer 通、层树拿到，但壁纸内容是空的**，P2 仍未达成。

**镜像/编译**

| 项 | 值 |
|---|---|
| 构建 | 仅重建 `root.apfs`（不设 `SYSTEM`；system.apfs 未动） |
| root.apfs | 2026-11-09 03:35Z，5,902,336 B，md5 **`dbf1c3d85d9cb35eb121af4401d02d51`** |
| system.apfs | 未动：2,476,748,800 B，md5 **`b75d80caaa5e276a58a4d45307d2165b`** |
| 新 dylib | `userland/obj/libhakios.sb.dylib` **108,248 B**；镜像内 `/lib/libhakios.sb.dylib` md5 **`1cd3053c0e127be55204e861f36e4047`**；`strings` 含 `layers_at`、`layers timer %lus scheduled/fired` |
| 旧库备份 | `/tmp/libhakios.sb.old.dylib`（108,040 B，exp 侧） |
| root 备份 | `/tmp/root.apfs.bak-P2c-timer`（`5f8ba9ea…`，P2c-alt2 版） |
| 核验 | `/etc/rc` = `rc.vtop`；libhakios 90,128 / gl 401,816 / libOSMesa 14,805,208 未回退 |

**Boot**

| 项 | 值 |
|---|---|
| 参数 | `XCMD="glwire_local=1 layers_at=900"`；`HOST_ENV=GLWIRE_NO_PANEL=1`；`UI_GRACE=900 MAXSECS=3000` |
| `end=` | `end=ui`：`ui=690s`，`ui_bytes=314818`；最大截图 **333,347 B** |
| 稳定性 | panic/SIGSEGV/usertrap=0；host **3,640** 帧 |

**Timer / 层树：成功拿到**

- `hakios: layers timer 900s scheduled` → **`hakios: layers timer 900s fired`** ✅
- **404 行** `hakios: layers`、**9 个 window**，以 `hakios: layers: end` 结束。

window 列表（level）：

```text
_SBWallpaperSecureWindow -3
SBHomeScreenWindow -2
_UISystemGestureWindow 0
SBRootSceneWindow 0
UITextEffectsWindow 10 (x2)
UIStatusBarWindow 999
SSScreenshotsWindow 1115
SBRecordingIndicatorWindow 1125
```

**壁纸层：window 与 VC 链都在，但 contents=0**

`_SBWallpaperSecureWindow`（level -3，context `0x44de26d9`，secure，429 commits，scene `com.apple.springboard`）子树：

```text
UIWindowLayer "colour ( 0 1 )"
  CALayer "VC:PBUIPosterWallpaperRemoteViewController" 375x812 opacity 1
    CALayer "-" 375x812
      CALayer "VC:PBUIPosterWallpaperViewController" 375x812
        CALayer "VC:PBUIPosterViewController" 375x812      <- 没有 contents、没有子层
  CALayer "-" colour ( 0 0 ) 375x812                       <- 黑层
```

- 壁纸子树内 `contents` = **0 处**；没有 `CALayerHost`、没有 `_UIRemoteView`。
- 整棵树共 7 个 `CALayerHost`，全部在 `SBRootSceneWindow` 的 `_UISceneLayerHostContainerView` 下，host 的是 SpringBoard 自己的 window context（`0x44de26d9`/`0x15ae970`/`0xc0763e2d`/`0xdfcc84cb`/`0x32541f82`/`0x3dfc1379`）；**没有 PosterBoard 的 context**。
- `SBWallpaperEffectView` 0 处（iOS 26 这代类名是 `PBUIPoster*`）；`PBUIPoster*` 3 处。

**宿主纹理 / 背景：仍失败**

- 上传：**1,261 张 `750x1624` 全为 panel `4294901761`**（外加启动期 1/4/7/10）；**无第二张持续非 panel 全屏纹理、无 1125x2436**。
- 截图 shot/final 左右边缘均 **0 / 36,000** 非黑 → **背景纯黑**。
- PosterBoard：`user/501/com.apple.PosterBoard [45]`，03:46:19 `Successfully spawned ... service state: running`；全程无 app 级日志/崩溃。

**判据逐条**

| # | 判据 | 结果 |
|---|---|---|
| 1 | `layers timer 900s fired` | ✅ |
| 2 | `hakios: layers` 层树行 | ✅ 404 行 / 9 window |
| 3 | wallpaper 类名 | `_SBWallpaperSecureWindow` + `PBUIPoster*` ✅；`SBWallpaperEffectView` 0（类名代际不同）；`CALayerHost` 无 PosterBoard context |
| 4 | 第二张 750x1624 非 panel 纹理 | ❌（1,261 张全 panel） |
| 5 | 1125x2436 | ❌ |
| 6 | 截图左右边缘非黑 | ❌ 0/36,000 |
| 7 | PosterBoard running、无 exit/SIGKILL/panic | ✅ |
| 8 | 是否计入 P2 | **否**；P2 未达成 |

**结论**

- **壁纸 window 与 `PBUIPoster*` 远程 VC 链已经存在**，但**内容是空的**（无 contents、无 PosterBoard 的 CALayerHost、无壁纸纹理）。
- 与 P2 对比：P2 是「lookup 失败、无 wallpaper」；现在 lookup 修复、window/VC 出现，**问题前移到「远程 poster 内容没接上/没画出来」**。
- 按分支：**有 wallpaper 层、contents 空 = 内容/快照问题**，不是 scene 完全没接上；P2 未达成。

### 4.22 s19o-p2d 实测：4 个 stock LaunchDaemon job（P2 未达成；依赖不是原因）

> **状态：已回填（2026-11-09 04:40Z）。P2 仍未达成。** 四个 job 里只有 `profiled` 真正起来；`powerlogHelperd` 的 stock plist 被 launchd 拒绝加载；两个 Photos `.xpc` 的 error 22 仍在。壁纸层树与 P2c-timer 完全同形（contents 空、无 PosterBoard 的 `CALayerHost`）。

**镜像**

| 项 | 值 |
|---|---|
| 构建 | 4-job patch 在 working tree（build.sh 456–459 四处 `cp`；无重复、`bash -n` 通过）；`PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh`，不设 `SYSTEM` |
| root.apfs | 2026-11-09 04:11Z，5,906,432 B，md5 **`b59fcf668707da64c716c3bb76dffab1`** |
| system.apfs | 未动：2,476,748,800 B，md5 `b75d80caaa5e276a58a4d45307d2165b` |
| 镜像内 | 4 个 plist 均在 `/LaunchDaemons`；sb dylib 108,248 B（`layers timer`）；`/etc/rc`==rc.vtop |
| 备份 | `/tmp/root.apfs.bak-P2d`（dbf1c3d8…） |

**Boot**：`XCMD="glwire_local=1 layers_at=900"`、`GLWIRE_NO_PANEL=1`、`UI_GRACE=900`、`MAXSECS=3000`；`end=ui`、`ui=660s`、`biggest=326663`、`hostframes=3681`、panic=0。

**四个 job**

| job | 结果 | 证据 |
|---|---|---|
| `com.apple.managedconfiguration.profiled` | **✅** | `(user/501/… [40]) Successfully spawned profiled[40] because ipc (mach)`、running；此后 `.public` lookup 失败 0 条 |
| `com.apple.powerlogHelperd` | **❌ plist 被拒** | `Unknown key for plist importer (key: UNUserNotificationCenter type: dictionary)`；未加载、无 spawn |
| `com.apple.photos.VideoConversionService` | **❌ 未 spawn** | 无 spawn；`Could not get domain target from bundle: …/VideoConversionService.xpc, error = 22`（04:20 SpringBoard、04:34 PosterBoard[46]） |
| `com.apple.photos.ImageConversionService` | **❌ 未 spawn** | 同上（ImageConversionService.xpc，error 22） |

- `com.apple.powerlog.plxpclogger.xpc` 仍失败（requestor：runningboardd/backboardd/SpringBoard，本发不是 PosterBoard）。
- PosterBoard `user/501 [46]` running；除两个 MediaConversion error 外无别的失败。

**层树（900s timer）**：405 行、9 window、`layers: end`。`_SBWallpaperSecureWindow`（level −3，ctx `0xa39703b2`）下 `PBUIPosterWallpaperRemoteViewController → PBUIPosterWallpaperViewController → PBUIPosterViewController`（375x812、opacity 1）仍在；子树 **contents=0**、旁有 `CALayer colour ( 0 0 )` 黑层。`CALayerHost` 共 8 个（2×155x155 + 6×375x812），**全是 SpringBoard 自己的 window context，无 PosterBoard**。

**宿主/背景**：3,681 张 750x1624 全 panel；无第二张非 panel、无 1125x2436；shot/final 左右边缘 0/36,000 → 背景纯黑。

**结论**：依赖不是原因（profiled 之外三个 job 没起作用）；P2 未达成。**P2d 深挖见 §4.23**：真正卡在 **app extension check-in timeout**（18 个 extension 被同一 watchdog 杀），poster scene/配置不是第一因；下一步 P2e 处理 limits。

---

### 4.23 s19o-p2d 深挖：真正卡在 app extension check-in timeout（P2 未达成）

> 证据：`/host/iosvm/logs/s19o-p2d/s19o-p2d-guest.log`。**日志不进 git**。
> P2d 的 900s 层树 contents=0 是**时序**，不是「永久不接」；真正原因是扩展 check-in 超时，系统级。

**关键时间线（同一 boot）**

| 时间 | 事件 |
|---|---|
| 04:26 | P2d 的 900s `layers_at` 层树（`PBUIPoster*` VC 链在，contents=0） |
| 04:37:58.70 | PosterBoard 才请求 `com.apple.GradientPoster.GradientPosterExtension` |
| 04:37:59.78 | `service spawned with pid 64`；`Successfully spawned GradientPosterExtension[64]` |
| 04:38:06.81 | `Extension is hanging on launch. Killing.` / `killing extension hanging on launch` / `SIGKILLed` |
| 04:38:08.25 | `exited with exit reason ... XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT | extension check-in timeout` |
| —— | **spawn → kill = 7.0s** |

- **同 boot 18 个 extension 全被同一 watchdog 杀**（每个 5–13s；`Extension is hanging on launch. Killing.` 共 18 条）。GradientPoster 只是其中一个。
- 全日志没有 `PRSPoster` / `dataStore` / `BSService` / scene / remote 连接错误；persona type6 仍是系统级；`/var/mobile/Library/...` 缺失不是第一因。
- 因此 §4.22 的「900s 层树 contents=0」只能说明**那个时刻扩展还没起来/内容还没接上**；不能推出「poster scene/配置永久缺失」。

**结论**

- P2 真正卡在 **app extension check-in timeout**（系统性，18 个扩展同时中招）；壁纸只是其中之一。
- **不是依赖缺失、不是 poster 配置错误、不是 PosterBoard 崩溃**；依赖对照（P2d）已经排除依赖作为第一因。
- P2 仍未达成；不要写「已修复」。

**P2e 计划（C1，单变量，未执行）**

- 新增 `/private/etc/com.apple.xpc.launchd.limits.plist`，放宽 watchdog：

  ```xml
  <?xml version="1.0" encoding="UTF-8"?>
  <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN"
    "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
  <plist version="1.0">
  <dict>
      <key>SIGTERMTimeout</key>          <integer>3</integer>
      <key>CrashOnSIGTERMTimeout</key>   <true/>
      <key>ExtensionWatchdog</key>       <true/>
      <key>ExtensionWatchdogTimeout</key><integer>120</integer>
  </dict>
  </plist>
  ```

- 构建：**`SYSTEM=1` 重建 `system.apfs`**（root.apfs 内容不动；构建后核对 root md5/备份）：
  1. 把上面的 plist 放到 `userland/`，在 `build.sh` 的 system 段加：
     ```sh
     mkdir -p sysroot/private/etc
     cp com.apple.xpc.launchd.limits.plist sysroot/private/etc/
     ```
  2. 备份并重建：
     ```sh
     cd /host/iosvm-work/hakios-xnu-kernel/userland
     cp -a system.apfs /tmp/system.apfs.bak-P2e
     cp -a root.apfs /tmp/root.apfs.bak-P2e
     PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop SYSTEM=1 ./build.sh
     ```
- Boot：`layers_at=1800`（比 P2d 更晚，给扩展 120s 窗口）、`UI_GRACE=1500`、`MAXSECS=3600`：

  ```sh
  cd /host/iosvm
  export REPO=/host/iosvm
  export KERNEL=/host/iosvm-work/hakios-xnu-kernel
  export GLWIRE=$REPO/tools/glwire
  export ROOT=$KERNEL/userland/root.apfs
  LABEL=s19p2e-1

  XCMD="glwire_local=1 layers_at=1800" \
  ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MINFRAMES=99999 MAXSECS=3600 UI_GRACE=1500 MINSHOT=200000 \
  HOST_ENV="GLWIRE_NO_PANEL=1" \
  IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
  tools/glwire/harness/boot-measure.sh "$LABEL" "$ROOT" 3600
  ```

- 判据：
  1. 7s 后不再出现 `Extension is hanging on launch. Killing.`；
  2. 扩展进程能活过 **30–60s**（GradientPoster 或任一 extension 不立刻被 SIGKILL）；
  3. 层树出现 **PosterBoard 的 `CALayerHost` / `contents`**（不只是 SpringBoard 自己的 host）；
  4. 宿主出现第二张 **750x1624 非 panel** 持续纹理；
  5. 截图背景非纯黑（左右边缘非黑 > 0）。
- 若 **120s 仍被杀** → 不要继续加 timeout；转 **C2：xpcproxy/persona**（检查 check-in 是否被 xpcproxy 的 persona 解析挡住），或其它进程内方案。
- C1 未执行；P2 未达成；不要写「P2 已修复」。


### 4.24 s19o-p2e 实测：`ExtensionWatchdogTimeout=120` 未生效（P2 未达成；C1 失败）

> **状态：已回填（2026-11-09 05:50Z）。C1 失败，P2 未达成。** limits plist 在 system.apfs 里且能被 plistlib 解析，但 extension 仍在 **spawn 后 ~9s** 被 `Extension is hanging on launch. Killing.` 杀掉，未见 120s 生效。

**镜像**

| 项 | 值 |
|---|---|
| system.apfs | 2026-11-09 05:10:50Z，2,476,752,896 B，md5 **`8bf5f838d52e46d6c8bf77f13a5ad946`** |
| root.apfs | **未动，保持 P2d**：5,906,432 B，md5 `b59fcf668707da64c716c3bb76dffab1`（build.sh 顺带重刷的 root 已用 `/tmp/root.apfs.bak-P2e` 还原） |
| limits plist | system.apfs `/private/etc/com.apple.xpc.launchd.limits.plist` 691 B；plistlib 解析 `SIGTERMTimeout=3`、`CrashOnSIGTERMTimeout=true`、`ExtensionWatchdog=true`、`ExtensionWatchdogTimeout=120` |
| 备份 | `/tmp/system.apfs.bak-P2e`（b75d80ca…）、`/tmp/root.apfs.bak-P2e`（b59fcf66…） |

**Boot**：`XCMD="glwire_local=1 layers_at=1800"`、`GLWIRE_NO_PANEL=1`、`UI_GRACE=1500`、`MAXSECS=3600`；`end=ui`、`ui=630s`、`biggest=430921`、`hostframes=6301`、panic=0。

**Extension watchdog（失败）**

| 时间 | 事件 |
|---|---|
| 05:22:30.98 | `GradientPosterExtension WILL_SPAWN` |
| 05:22:33.75 | `service spawned with pid 51` |
| 05:22:42.67 | **`Extension is hanging on launch. Killing.` / `killing extension hanging on launch`**（spawn 后 **~9s**） |
| 05:22:59.80 | `exited with exit reason … XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT | extension check-in timeout` |

- 全 log `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT` **50 次**、`checked in as alive` **0 次**；WallpaperKit.CollectionsPoster 同样被杀。
- **判断**：`ExtensionWatchdogTimeout=120` 未被 launchd 采纳（默认量级仍是 ~10s）；不是「120s 到了才被杀」。按约定**不再加 timeout**。

**1800s 层树（仍空）**：405 行、9 window、`layers: end`。`_SBWallpaperSecureWindow`（level −3，ctx `0x2081fc3d`，3068 commits）下 `PBUIPosterWallpaperRemoteViewController → …ViewController → …ViewController` 仍在（375x812、opacity 1），但子树 **contents=0**、旁 `CALayer colour ( 0 0 )` 黑层；`CALayerHost` 8 个全是 SpringBoard 自己的 window context，**无 PosterBoard**。

**宿主/背景**：6,039 张 750x1624 全 panel；无第二张非 panel、无 1125x2436；shot/final 左右边缘 0/36,000 → 背景纯黑（430,921 B 的晚到截图是图标/widget 内容，不是壁纸）。

**结论**：C1 单变量失败——limits 文件在镜像里但不生效；extension check-in 仍超时，壁纸 provider 起不来。下一步按 §4.23 的 fallback 转 **C2（xpcproxy/persona）**或离线 launchd/XPCServices 分析，**不要继续加 timeout**。P2 未达成。

**路线调整（2026-11-09）**

- **停止 C1**：不再加 timeout、不再改 limits plist；120s 未生效，继续调只是重复。
- **C2 限时**：code 做 **30 分钟**离线限时（xpcproxy/persona/check-in 方向）；到点无论结果都汇报，不无限深挖。
- **P3 正式开工**（不等待 P2）：guest 库已可编；MVP = boot-arg 固定 surface ID + backboardd `IOSurfaceLookup` + host **opcode 202 / `GLWIRE_WIDGET`**。
- **P2 不阻塞 P3**；P1 暂停；P0 仍 **2/3**，P2 未达成。

---

### 4.25 s19o-p2e-c2 实测：强制 xpcproxy 后 extension 仍超时（不是 elision；P2 未达成）

> **状态：已回填（2026-11-09 06:50Z）。C2 失败；P2 未达成。** boot-arg 确实关掉了 elision（extension 全部 `xpcproxy spawned`），但 Gradient 等仍在 spawn 后 **7–11s** 被 `Extension is hanging on launch. Killing.`，`CHECK_IN_TIMEOUT` 共 16 次、`checked in as alive` 0 次。1800s 层树壁纸 subtree 仍 contents=0；截图晚段出现绿色伪影（不是壁纸）。

**镜像（未重建）**

| 项 | 值 |
|---|---|
| system.apfs | `8bf5f838d52e46d6c8bf77f13a5ad946`（P2e limits 版，未动） |
| root.apfs | P2d `b59fcf668707da64c716c3bb76dffab1`（未动） |
| 改动 | 只加 boot-arg `launchd_proxyless_spawn=0` |

**Boot**：`XCMD="glwire_local=1 launchd_proxyless_spawn=0 layers_at=1800"`；`GLWIRE_NO_PANEL=1`；`UI_GRACE=1500`、`MAXSECS=3600`；`end=ui`、`ui=690s`、`biggest=991040`、`hostframes=5703`、panic=0。

**判据（逐条）**

| # | 判据 | 结果 | 证据 |
|---|---|---|---|
| 1 | extension 日志出现 `xpcproxy spawned` | **✅** | `service will always use xpcproxy, elision was disabled by boot-arg`；`xpcproxy spawned with pid 9/45/47/57…`；`elide xpcproxy` = 0 |
| 2 | 不再在 ~9s 出现 `hanging on launch` | **❌** | Batteries 7.2s（06:20:40.2→06:20:47.4）、News ~10s、**Gradient 11.4s**（06:37:02.2→06:37:13.6） |
| 3 | `checked in as alive` >0 | **❌** | 0 次；全 log `hanging on launch` 32 次、`CHECK_IN_TIMEOUT` 16 次 |
| 4 | 层树 PosterBoard `CALayerHost`/`contents` | **❌** | 1800s：405 行、9 window；壁纸 subtree `contents=0`、旁黑层；8 个 CALayerHost 全是 SpringBoard 自己的 context |
| 5 | 宿主第二张 750x1624 非 panel 纹理 | **❌** | 5,249 张全为 panel `4294901761`（+启动期 1/4/7/10）；无 1125x2436 |
| 6 | 截图背景非纯黑 | **❌/伪影** | 991,040 B 截图右下 1/4 为**纯绿 `#00FF00`**（750-space x≥375.3、y≥812.7，683,954 px），左边是模糊主屏；绿色是渲染伪影，不是壁纸 |

**判读**：命中「仍 ~9s 被杀 / alive 仍 0」分支 → **不是 elision**。按约定 P2 性价比低/需更多信息，**停止 P2 timeout/xpcproxy 方向**，转 P3。附带：本发晚段画面有绿色右下象限伪影，以后遇到 991 KB 级截图先查 panel 完整性，不要当壁纸证据。

### 4.26 s19o-p3a 实测：surface 3 → opcode 202 → host 合成链路已通（内容为空白白块）

> **状态：已回填（2026-11-09 07:20Z）。MVP 链路验证通过；像素语义不对。** guest `IOSurfaceLookup(3)` → `_eagl_remote_widget_upload`（opcode 202）→ host `widget upload texture` → `widget compose` 到 750x1624 fbo，rect 46,142 309x309 全部按参数发生；但 surface 3 的内容是**纯白圆角块**，不是当前 widget 内容。P3 未完成、widget 未正确。

**镜像/库**

| 项 | 值 |
|---|---|
| root.apfs | 2026-11-09 06:54Z，5,910,528 B，md5 **`f24721948583b3d3b379c14c2d852a23`** |
| system.apfs | 未动：2,476,752,896 B，md5 `8bf5f838d52e46d6c8bf77f13a5ad946` |
| guest 库 | `libhakios.dylib` **90,720 B**、`libhakios.gl.dylib` **400,720 B**（镜像内同尺寸；strings `widget_surface=` / `_eagl_remote_widget_upload`） |
| host | `host_glwire` 8da5517744eed40051898424586e3241（115,384 B，strings `GLWIRE_WIDGET`） |
| 备份 | `/tmp/root.apfs.bak-P3a`（P2d b59fcf66…） |

**Boot**：`XCMD="glwire_local=1 widget_surface=3 widget_rect=46,142,309,309"`；`HOST_ENV="GLWIRE_WIDGET=1 GLWIRE_NO_PANEL=1"`；`UI_GRACE=600`、`MAXSECS=2400`；`end=ui`、`ui=840s`、`biggest=356579`、`hostframes=2285`、panic=0。

**判据（逐条）**

| # | 判据 | 结果 | 证据 |
|---|---|---|---|
| 1 | host 出现 opcode 202 / `GLWIRE_WIDGET` 上传 | **✅** | `glwire: widget upload texture 4294905859 … 184x124 … rect 46,142 309x309 bytes 91264`（1 条）；4 条 `widget compose 1 texture(s) into fbo 3/2 750x1624, first 46,142 309x309` |
| 2 | 宿主收到 184x124 纹理、内容非全零 | **✅（但全白）** | 纹理 184x124；截图 rect 区域 mean RGB **251**、nonblack 211,753/214,369 → 纯白 |
| 3 | 截图 rect 有变化（允许拉伸/旧内容） | **⚠️ 无可见内容** | 区域是**纯白圆角块**，与占位块外观一致；rect 46,142 309x309 已按参数拉伸应用 |
| 4 | 尺寸/语义记录 | **❌ 语义不对** | surface 3 = 184x124（92x62 pt），层树 widget host = 155x155 pt；链路通但 surface 3 不代表当前 widget |

**判读**：guest 侧 `hakios: widget surface 3 184x124 bpr 736 -> texture 0xffff1003 rect 46,142 309x309 scale 1.000` 出现 1 次；host 上传 1 次、合成 4 次；`widget texture removed`=0。**链路（surface→202→host compose）验证通过**；像素内容是空白白块，说明 surface 3 是旧 snapshot/空白面。下一步见 **§4.27**：拿当前 widget 的 CAContext snapshot，不再调 rect。本发无绿色 1/4 伪影（0 纯绿像素）。

**不要写**：P3 已完成 / widget 已正确 / P2 已修复。P2-C2 已失败；P0 仍 2/3。

---

### 4.27 P3b D1→D3 结果：widget context 属于 extension，零参快照前提不成立

> **状态：D1/D2/D3 均已跑，D3 失败（第三次，到放弃线）；P3 未完成。** P3a 的 `surface→202→host` 链路通，但 surface 3 是空白白块；D1 证明 CAContext 能力面在，D2 证明 `+allContexts` 按进程隔离，D3 证明 **widget host 的 CAContext 属于 widget extension 进程**，SpringBoard 也看不到。

**三次探针的结果**

| 发次 | 位置/时机 | 结果 |
|---|---|---|
| D1 | backboardd init | 9 行探针；`contextWithId:`（小写 d）/`allContexts` 在，但 `allContexts` 被精确 `@@:` encoding 挡住；init 太早、widget context 未出现；无对象/快照/nonzero |
| D2 | backboardd `widget_cacontext_at=900` | timer fired；`allContexts count 1`（`0xc0cfe455`，backboardd 自己的）；与 950s 层树 155x155 host `0xbf11dcae/0x7702cd25` 不匹配；无 snapshot/nonzero |
| D3 | SpringBoard `inject_sb.c`，900s | 第一次 fired `allContexts count 16`（含 6 个 window context）；第二次 fired `nil`；**16 个 cid 不含 155x155 host `0x4741275c/0xccf65063`**；0 行 snapshot/nonzero/candidate |

**关键结论**

- **`+allContexts` 按进程隔离**：backboardd 只看到自己的 context；SpringBoard 看到 16 个（含自己的 window context），但看不到 widget host 的两个 155x155 context；它们属于 **widget extension 进程**。
- **CAContext 这代没有零参 `createImage`/`createCGImage`/`snapshot`**：只有 `createImageSlot:hasAlpha:`（创建 slot，不是快照）。「零参快照」这条路前提不成立；要拿内容只能 `CARenderServer*`（render server）或跨进程传 id。
- D3 是第三次尝试，按时间盒**到放弃线**：不再盲试私有 selector、不再猜 `createImageSlot` 参数。

**P2/P3 共同根因（关键）**

- widget 内容由 **widget extension** 渲染；而 P2e/P2e-C2 已证明 **所有 extension 在 ~9s 被 check-in timeout 杀**（Gradient/Batteries 等；`CHECK_IN_TIMEOUT` 多、`checked in as alive` 0）。
- 所以 **P3 的实时 widget 内容在 extension 修复前不可得**；这不是 P3 探针单独的问题，而是 **P2/P3 共用的系统级阻塞**。
- 反过来说：即使 CAContext 跨进程/ render server 能拿到某个 context，extension 已经被杀，也拿不到当前 widget 内容。

**下一步（fallback）**

- **P3 不再继续 D 系列探针**：按时间盒转 fallback——静态/占位 widget（拉面最小实现），或记录「需更多信息/不可行」。
- 若 owner 决定再攻 **extension check-in timeout**，那是 P2/P3 的共同前置；修好后再回来做实时 widget 内容。
- **P3 未完成；P2 未达成；不要写 P3 已交付 / widget 正确。**

### 4.28 s19o-p3b-d1 实测：CAContext 探针拿到能力面，但没拿到对象/快照（D1 未成功）

> **状态：已回填（2026-11-09 08:06Z）。D1 未成功。** 探针证明 `CAContext` 类存在、关键 selector/符号都在，但**没有任何 context 对象、快照或 nonzero**：`allContexts` 因 type-encoding 检查没被调用；真实 selector 是 `contextWithId:`（小写 d），探针找的是 `contextWithID:` 等；且探针只在 backboardd init（~20s）跑一次，而 widget context 到 UI 后才出现、id 每 boot 变。

**镜像**

| 项 | 值 |
|---|---|
| root.apfs | 2026-11-09 07:42Z，5,910,528 B，md5 **`b8c686ef9aab075172218483c9e36db2`**（备份 `/tmp/root.apfs.bak-P3b-d1` = f2472194…） |
| system.apfs | 未动：2,476,752,896 B，md5 `8bf5f838d52e46d6c8bf77f13a5ad946` |
| 镜像内库 | `libhakios.dylib` **92,208 B**（`ca-context` strings 18 处）、`libhakios.gl.dylib` **400,720 B**；`/etc/rc`==rc.vtop |

**Boot**：`XCMD="glwire_local=1 widget_cacontext=auto layers_at=900"`；`GLWIRE_NO_PANEL=1`；`UI_GRACE=600`、`MAXSECS=2400`；`end=ui`、`ui=750s`、`biggest=327719`、`hostframes=2233`、panic=0。

**探针输出（9 行，全在 init 时）**

- `CAContext 0x221ec5888`；**class methods (9)**：`setClientPort:`、**`allContexts(@16@0:8)`**、`remoteContextWithOptions:`、**`contextWithId:(@20@0:8I16)`**、`remoteContext`、`localContext`、`localContextWithOptions:`、`objectForSlot:(@20@0:8I16)`、`currentContext`。
- **instance methods (53)**：含 `createImageSlot:hasAlpha:`、`createImageSlot:hasAlpha:extendedColors:`、`setObject:forSlot:` 等。
- 符号：`CARenderServerSnapshot` / `RenderLayer` / `RenderDisplay` / `RenderDisplayLayerWithTransformAndTimeOffset` 全部 **present**。
- auto：只打印 `auto class method allContexts types @16@0:8` 后结束；**没有** `context object` / `snapshot object` / `image … nonzero` 行。

**判据（逐条）**

| # | 判据 | 结果 | 证据 |
|---|---|---|---|
| 1 | selector 存在并打印 | **✅（但名字对不上）** | 实际是 `contextWithId:`（小写 d）/`objectForSlot:`；探针找的 `contextWithID:`/`objectForContextID:`/`contextForID:` 都不在 9 个 class methods 里 |
| 2 | 拿到 context 对象非空 | **❌** | auto 未调用 `allContexts`（type gate 要求精确 `@@:`，实际 `@16@0:8`）；无任何对象打印 |
| 3 | 快照尺寸合理 | **❌** | 无快照 |
| 4 | nonzero > 0 | **❌** | 无图像/计数 |
| 5 | 不崩/不阻塞 | **✅** | 9 行后正常继续，boot 到 UI、无 panic |

**根因（code 侧，三处）**

1. **`allContexts` 没被调用**：`widget_ca_auto` 的 type-encoding 检查要求精确 `"@@:"`，实际 `"@16@0:8"`（带偏移）→ `continue`。
2. **selector 名不对**：真实 `contextWithId:`（小写 d）；`widget_ca_try_id` 的候选名全不匹配；即使匹配，编码检查同样要求无偏移。
3. **时序**：探针只在 init 跑一次；widget host context 在 UI 后才出现，且 id 每 boot 变——本 boot 900s 层树 155x155 host = **0x493ce0b9 / 0x810db096**；历史 P2c-timer 0x68f757b6、P2d 0x31afccae/0xa71cf0d7、P2e 0x665f6caa/0xbf7947b6、P2e-c2 0x65984b24/0xf974877b。

**其他**：host `widget` 行 = 0（未开 `GLWIRE_WIDGET`）；截图无绿色 1/4 伪影（0 纯绿）；左右边缘 0/36,000 非黑。

**建议**：① 选择器表加 `contextWithId:`，type encoding 用容忍偏移的解析；② `widget_cacontext_at=SECONDS` 延迟到 UI 后（与 `layers_at` 同思路）或轮询 `allContexts`；③ 延迟后 `allContexts` 列表 × 层树 155x155 host id 对照，对候选逐个 `createImageSlot:hasAlpha:` 快照取 nonzero>0；④ 仍失败按放弃条件转 fallback。**不要盲调未知 selector。**

**不要写**：D1 成功 / P3 完成 / widget 正确。P2-C2 已失败；P0 仍 2/3。

---

#### D2 实测：延迟 `allContexts` 可用，但只看到 backboardd 自己的 context（D1 仍未成功）

> **状态：已回填（2026-11-09 09:14Z）。D1 仍未成功。** 延迟触发、容错 selector、`+allContexts` 都验证可用；但 `+allContexts` 在 backboardd 里**按进程隔离**——只有 backboardd 自己的 1 个 context（cid `0xc0cfe455`），看不到 SpringBoard 的 widget host context（同 boot 950s 层树 155x155 = `0xbf11dcae` / `0x7702cd25`，不匹配）。`createImageSlot:hasAlpha:` 只打印 encoding、未调用；无快照/nonzero。

**镜像/库**

| 项 | 值 |
|---|---|
| root.apfs | 2026-11-09 08:43Z，5,910,528 B，md5 **`fc1787e46cd26bf6d20fa4571ec17ba5`** |
| system.apfs | 未动：2,476,752,896 B，md5 `8bf5f838d52e46d6c8bf77f13a5ad946` |
| 镜像内库 | `libhakios.dylib` **92,872 B**（md5 `e7e33614e307a985227c7f7d47a36184`；strings `widget_cacontext_at=`/`contextWithId:`/`widget candidate`）、`libhakios.gl.dylib` **400,720 B** |
| 备份 | `/tmp/root.apfs.bak-P3b-d2`（b8c686ef…，D1 版） |

**Boot**：`XCMD="glwire_local=1 widget_cacontext_at=900 layers_at=950"`；`GLWIRE_NO_PANEL=1`；`UI_GRACE=900`、`MAXSECS=2400`；`end=ui`、`ui=690s`、`biggest=330924`、`hostframes=3923`、panic=0。

**探针输出（14 行）**

- `timer 900s scheduled` → **`timer 900s fired`** ✅（延迟生效）。
- CAContext class/9 class methods/53 instance methods；`CARenderServer*` 全部 present。
- **`allContexts -> __NSSingleObjectArrayI`，count 1**：
  ```
  [0] CAContext 0x141dd2f00
  context object CAContext cid 0xc0cfe455
  context 0xc0cfe455 has createImageSlot:hasAlpha: types I36@0:8{CGSize=dd}16B32 (not called: slot/options not guessed)
  ```
- **没有** `widget candidate` / `image WxH … nonzero` / `snapshot object` 行。

**同 boot 950s 层树对照**

- 155x155 widget host context：**0xbf11dcae、0x7702cd25**；全部 8 个 CALayerHost context：`0xbf11dcae 0x7702cd25 0xd90683b8 0x7354affb 0xbc29fb4 0x5429f1d8 0x774b1266 0x82f51f58`。
- `allContexts` 的 `0xc0cfe455` **不在其中** → 它是 backboardd 自己的 context，不是 SpringBoard/widget 的。

**判据（逐条）**

| # | 判据 | 结果 |
|---|---|---|
| 1 | `widget_cacontext_at` timer 到点后 `+allContexts` 被调用 | **✅** |
| 2 | 至少一个 context 对象被打印 | **✅（count 1，0xc0cfe455）** |
| 3 | 至少一个快照 `nonzero>0`、尺寸 32–4096、打印 `widget candidate` | **❌**（`createImageSlot:hasAlpha:` 未调用，slot 未猜；无 image/nonzero） |
| 4 | 与 950s 层树 155x155 host id 对照 | **❌ 不匹配**（0xc0cfe455 vs 0xbf11dcae/0x7702cd25） |
| 5 | 全空/全黑按放弃条件处理 | 记录；不盲调未知 selector |

- 截图无绿色 1/4 伪影（0 纯绿）；左右边缘 0/36,000 非黑。

**结论与下一步（D3 建议）**

- **D1 仍未成功**：延迟/selector/`allContexts` 可用，但 `+allContexts` 在 backboardd 里只看得到本进程的 context；**架构上 backboardd 不能枚举 SpringBoard 的 CAContext**。
- **D3 建议**：把 CAContext 探针移到 **SpringBoard（`inject_sb.c`）**，在同样的 900s 延迟里跑（SpringBoard 是 widget host 的 client，`allContexts` 应能看到 155x155 的 context）；对候选调 `createImageSlot:hasAlpha:`/`createImage` 取 nonzero>0。或继续用显式 id，但必须同 boot 传递（成本更高）。
- 这是第 2 次 D1 尝试（auto@init、auto@900）；未到 3 次放弃线。**P3 未完成，不要写 D2 成功 / widget 正确。**

#### D3 实测：SpringBoard `allContexts` 有 16 个 context，但没有 155x155 widget host（D3 失败）

> **状态：已回填（2026-11-09 09:52Z）。D3 失败，按约定转 fallback。** SpringBoard 的 `+allContexts` 能用（count 16，含 6 个 CALayerHost 的 window context），但 **155x155 widget host 的两个 context（0x4741275c / 0xccf65063）不在其中**——widget host 的 CAContext 属于 widget extension 进程。CAContext 这代没有零参 `createImage`/`createCGImage`/`snapshot`，因此 0 快照、0 nonzero。

**镜像/库**

| 项 | 值 |
|---|---|
| root.apfs | 2026-11-09 09:22Z，5,914,624 B，md5 **`ae626263bc6bef308568549541876b61`** |
| system.apfs | 未动：2,476,752,896 B，md5 `8bf5f838d52e46d6c8bf77f13a5ad946` |
| 镜像内库 | `libhakios` **92,872**、`libhakios.gl` **400,720**、`libhakios.sb` **109,632**（md5 02dfa907…；`widget_cacontext_at`/`contextWithId:`/`widget candidate` 齐） |
| 备份 | `/tmp/root.apfs.bak-P3b-d3`（fc1787e4…，D2 版） |

**Boot**：`XCMD="glwire_local=1 widget_cacontext_at=900 layers_at=950"`；`GLWIRE_NO_PANEL=1`；`UI_GRACE=900`、`MAXSECS=2400`；`end=ui`、`ui=660s`、`biggest=327930`、`hostframes=3817`、panic=0。

**SpringBoard 探针（74 行）**

- 两次 `timer 900s scheduled`（两个注入实例）；第一次 `fired`：
  ```
  allContexts -> __NSArrayI
  allContexts count 16
  cids: 0xaf7b8510 0x7527b2d7 0xcab3e859 0x7c6eeedf 0xe514cb59 0x6ae293d3 0x587e9942 0xb6a2ad6f
        0xca5825cb 0x9ce7ca7c 0xe5f8662b 0xd004cf69 0x37dc7ddb 0x6bf2dbaa 0x48a8fb5a 0x7341f34e
  ```
  每个 context 只打印 `createImageSlot:hasAlpha:` 的 encoding，**未调用**（slot 不猜）。
- 第二次 `fired`：`allContexts -> nil`。
- **0 行** `snapshot object` / `image WxH … nonzero` / `widget candidate`。

**与 950s 层树对照（同 boot）**

- 155x155 widget host context：**0x4741275c、0xccf65063**。
- 全部 8 个 CALayerHost context：`0x4741275c 0xccf65063 0x7c6eeedf 0xe514cb59 0xca5825cb 0x7527b2d7 0xb6a2ad6f 0xe5f8662b`。
- allContexts 的 16 个 cid 里包含 6 个 window context（`0x7c6eeedf`、`0xe514cb59`、`0xca5825cb`、`0x7527b2d7`、`0xb6a2ad6f`、`0xe5f8662b`），但**不包含两个 155x155 widget host**。

**判据（逐条）**

| # | 判据 | 结果 |
|---|---|---|
| 1 | SpringBoard `timer 900s fired` | **✅** |
| 2 | `allContexts count` + 每个 cid | **✅（count 16）** |
| 3 | snapshot 尺寸合理且 nonzero>0 | **❌**（无零参 snapshot；createImageSlot 未调用） |
| 4 | widget candidate 与 155x155 host id 对照 | **❌ 不匹配** |
| 5 | SpringBoard allContexts 有 155x155 host | **❌** |

- 截图无绿色 1/4 伪影（0 纯绿）；左右边缘 0/36,000 非黑。

**结论（D3 失败 → fallback）**

- **D3 失败**：widget host 的 CAContext 属于 **widget extension 进程**；`+allContexts` 在 backboardd 和 SpringBoard 里都按进程隔离，看不到它。
- CAContext 这代没有零参 `createImage`/`createCGImage`/`snapshot`，只有 `createImageSlot:hasAlpha:`（创建 slot，不是快照）；「零参快照」前提不成立。
- 要拿 widget 内容只剩 **CARenderServer*（render server）** 或把 id 跨进程传到创建它的进程，成本/风险高、超出 D1 时间盒。
- 按 lead 的 D3 失败判据：**明确判「D3 失败/需 CAContext 跨进程（render server）」**，按时间盒转 fallback（拉面最小实现或「需更多信息」），**不再盲试私有 selector / 不再猜 createImageSlot 参数**。
- **P3 未完成；P2 仍未达成。**

> **状态：正式决定 D1–D5 已下（见 `docs/gpu-acceleration-stage19-decisions.md`）。真路径判定为「不可行」并提前收口；fallback 转 guest-side placeholder，fallback4 截图待回填。**

> 本节是收口表；完整决定/依据/影响/可推翻条件见 **decisions D1–D5**。P3 最终结论写「不可行（真路径）+ fallback 交付（待 fallback4 截图）」，不要写「可行」。 P3 最终只允许三种结论：**可行 / 不可行 / 需要更多信息**；fallback 不是真路径。

**Surface 枚举（2026-11-09 10:51Z，独立探针）**

- 900s 枚举到 **30 个存活 MiniIOSurface**：
  - id 1/2：750x1624 BGRA（显示面；bpr 3072/3008）；
  - **id 3：184x124 BGRA bpr 736（SpringBoard[9]**，stage11 的旧 widget snapshot）；
  - **id 4：312x202 LA08 bpr 640（SpringBoard[9]**，文字/字形覆膜）；
  - id 5–30：backboardd 的 180x180 图标面 + 28x95/28x65/28x28/19x19/16x16/6x85/6x6 装饰面。
- **没有 155x155 pt（310x310 @2x / 465x465 @3x）或任何 widget 尺寸候选**；同 boot 950s 层树 155x155 `CALayerHost` = **`0x500b566f`/`0x7f535c4c`**，与枚举表任何 id 无对应。
- → **widget 内容面不是 MiniIOSurface，而是 widget extension 的远程 CAContext**；`surface → opcode 202 → 真实 widget` 的 surface 路径**不可行**（没有可点的 id）。
- 同 boot 试合成 id3/id4 时，host 收到的是全零 202（`widget upload texture 0 ... 0x0 rect 0,0`），截图 widget 区仍 mean 251 白块；这是主队列调 `eagl_remote_widget_upload` 的编码/remote 状态技术问题（P3a 从 widget 线程调用正常），不影响枚举结论。
- fallback2 没出图（local panel vs replay fbo）；fallback3 host-replay 也没出图（QEMU 截图是 guest scanout，host fallback 是 offscreen picture）；**host-side fallback 不可视觉交付**，转 guest-side placeholder。P3 真实路径不可行，最终三选一只剩 guest-side fallback 验收状态待填。

**判据逐条对照**

| # | P3 判据 | 当前证据 | 到期填 |
|---|---|---|---|
| 1 | `312x202`/`184x124` 以纹理进宿主 | P3a：链路通（host upload + 4 compose），但 surface 3 是空白白块 | **✅ 链路 / ❌ 内容** |
| 2 | 截图 widget 区域出现**当前**内容（非纯白/旧内容） | P3a 区域 mean RGB 251、nonblack 211,753/214,369 → 纯白 | **❌** |
| 3 | 延迟探针能拿 CAContext 对象 | D2 count 1（backboardd 自己）、D3 count 16（SpringBoard 自己的 window context）；D1 因 init/encoding 没拿到；都不是 155x155 host | **❌（拿到的是别人的）** |
| 4 | 拿到 widget host 的 CAContext 或等价快照 | D1/D2/D3 全部失败：`+allContexts` 按进程隔离；SpringBoard 16 个不含 `0x4741275c/0xccf65063`；widget context 属 extension 进程 | **❌** |
| 5 | 零参 snapshot 可得且 nonzero>0 | 这代 CAContext 无零参 `createImage`/`createCGImage`/`snapshot`；`createImageSlot:hasAlpha:` 是创建 slot，未调用 | **❌** |
| 6 | 不依赖已死的 extension | P2e/P2e-C2：所有 extension ~9s 被 check-in timeout 杀；P3 实时内容在 extension 修复前不可得 | **❌** |
| 7 | fallback（拉面最小实现）可交付 | **host fallback 不可见；guest-side fallback 待实现/验收**：fallback2 画了但 panel 看不到；fallback3 host-replay 仍看不到（截图=guest scanout，host fallback=offscreen picture）；转 `inject_sb.c` 的 `widget_placeholder=1`，待实现/验收。host 侧实现细节：`GLWIRE_WIDGET_FALLBACK=1`（别名 `GLWIRE_WIDGET_PLACEHOLDER=1`）；空白检测 ≥90% 近白（含透明/全零）后画 24px 棋盘 + 3px 白边框 + 两条黄对角线；日志 `widget fallback ... (NOT the real widget path)`；无 202 时可用 `GLWIRE_WIDGET_RECT=x,y,w,h` 直接画；`widget_test` 5 例 PASS；host binary 119,552 B（fallback2 md5 `68d89e54…`；fallback3 实际 `28f484a1…`） | 待 guest-side placeholder 截图验收（不是 P3 通过） |
| 8 | surface 枚举有 widget 尺寸候选 | **❌**：30 个面只有 id3 184x124（旧 snapshot）/id4 312x202（LA08）；无 155x155/310x310/465x465；950s host 0x500b566f/0x7f535c4c 无对应 id | **❌** |

**三选一判定（已填到只剩 fallback3）**

- **不可行（真路径）——当前判定，证据链完整**：
  1. **surface 枚举 30 个面无候选**：id3 是 184x124 旧 snapshot、id4 是 312x202 LA08；无 155x155/310x310/465x465；950s 层树 155x155 host `0x500b566f/0x7f535c4c` 无对应 id。
  2. **`+allContexts` 按进程隔离**：backboardd 只看到自己的 context；SpringBoard 16 个里只有自己的 window context，不含 155x155 widget host。
  3. **这代 CAContext 没有零参 `createImage`/`createCGImage`/`snapshot`**；`createImageSlot:hasAlpha:` 是创建 slot，不是快照。
  4. **extension 5–13s 被 check-in timeout 杀**（P2e/P2e-C2；18 个扩展），P3 实时 widget 内容在 extension 修复前不可得。
  → 结论：**P3 真路径（surface/CAContext 实时内容）不可行**；不要再用 rect/selector/surface 3 重试。

- **可行（仅指 fallback，不是真路径）——只在 guest-side placeholder 出图时成立**：
  - guest-side `widget_placeholder=1` 截图里 widget 区出现 **24px 棋盘 + 3px 白边框 + 两条黄对角线**；
  - 必须注明 **“这是拉面占位，不是真 widget 内容”**；不能写成 P3 真路径可行。
  - fallback2/fallback3 已证明 host 侧 fallback 在截图里不可见；guest-side 结果待填。

- **需要更多信息**：
  - 只缺一个具体测量：**extension check-in 修复后再跑一次 P3**（能拿到 widget extension 的实时内容/CAContext 快照）。
  - 不是“还没试够”；surface 枚举、D1/D2/D3、fallback2 都已经做过。若 owner 决定再攻 extension timeout，才选这条；否则选不可行。

**到期填写**

| 项 | 值 |
|---|---|
| 判定时间 | 2026-11-10 14:06（到期后填实际） |
| 真路径结论 | **不可行**（证据链见上：surface 枚举/allContexts/无零参 snapshot/extension timeout） |
| 最终三选一 | **不可行（真路径）+ fallback 交付（待 fallback4 截图）**；若 owner 选择再攻 extension，则改为「需要更多信息」 |
| fallback 交付 | 待 guest-side fallback4：出图 = 已交付（占位，不是真 widget）；不出图 = 未交付 |
| 需要 owner 决定 | 是否再攻 extension check-in timeout（P2/P3 共同前置） |

**fallback 实现（不是 P3 通过）**

- `GLWIRE_WIDGET_FALLBACK=1`（别名 `GLWIRE_WIDGET_PLACEHOLDER=1`）：先做空白检测（≥90% 近白），命中后画 24px 棋盘、3px 白边框、两条黄对角线；日志明确写 `widget fallback ... (NOT the real widget path)`。
- 没有 202 时可用 `GLWIRE_WIDGET_RECT=x,y,w,h` 直接画占位。
- `widget_test` 5 例 PASS；host binary **119,552 B**（fallback2 md5 `68d89e541eba0fd34f0f671a21e24659`；fallback3 实际使用了 blank 修复版 `28f484a1ed55da720cb7b78e4eb0f602`）。
- **host 侧 fallback 已证明不可视觉交付**（fallback2/3：截图是 guest scanout，host 画在 offscreen picture）；保留作分析用。
- **交付改走 guest-side**：`inject_sb.c` 的 `widget_placeholder=1` 在 UI 后插圆角 UIView（棋盘+白框+黄对角线），日志 `static widget placeholder ... (NOT the real widget path)`；待实现/验收。
- 这是 **fallback 交付项**，截图验收前不要写成 P3 可行/已完成。

**不要写**：P3 已交付 / widget 已正确 / P2 已修复。P3 未完成；P0 仍 2/3；P1 暂停。


### 4.30 s19o-p3-surface-enum 实测：widget 面不在 MiniIOSurface 表里

> **状态：已回填（2026-11-09 10:51Z）。** 900s 枚举到 **30 个存活 MiniIOSurface**；只有 id 1/2 的 750x1624 显示面、id 3 的 **184x124 BGRA**、id 4 的 **312x202 LA08**、以及 backboardd 的 180x180 图标/装饰面；**没有任何 155x155 pt（310x310@2x / 465x465@3x）或其它 widget 尺寸的候选面**。→ widget 内容面不是可 `IOSurfaceLookup` 的 MiniIOSurface（是 widget extension 的远程 CAContext；与 D3 一致）。

**探针**：`src/inject.c` 新增 `surface_at=SECONDS`（主队列 `dispatch_after`），遍历 `IOSurfaceLookup(1..1024)` 打印 `id WxH fmt fourcc bpr alloc`；`surface_upload=1` 时对候选调 `eagl_remote_widget_upload` 试合成。编译产物 **93,072 B**（Mach-O，strings `surface-enum` 4 处）。

**镜像/库**：root.apfs **`f29594abf78d25d36582d497e2969446`**（10:21 / 5,918,720 B；备份 `/tmp/root.apfs.bak-p3-enum` = ae626263…）；system.apfs 未动 `8bf5f838…`；镜像内 libhakios 93,072 / gl 400,720 / sb 109,632。

**Boot**：`XCMD="glwire_local=1 surface_at=900 surface_upload=1 widget_rect=46,142,309,309 layers_at=950"`；`HOST_ENV="GLWIRE_WIDGET=1 GLWIRE_NO_PANEL=1"`；`end=ui ui=690s biggest=328115 hostframes=3888 panic=0`。

**枚举结果（900s，30 个面）**

```
id 1  750x1624 BGRA bpr 3072 alloc 4997120   backboardd[16]
id 2  750x1624 BGRA bpr 3008 alloc 4898816   （无 created 行）
id 3  184x124  BGRA bpr 736  alloc 98304     SpringBoard[9]
id 4  312x202  LA08 bpr 640  alloc 131072    SpringBoard[9]
id 5..13,19..25,27..30  180x180 bpr 720      backboardd[16]
id 14 28x65, 15 28x95, 16 28x28, 17 6x85, 18 6x6, 26 19x19（装饰面）
```

- **无 155x155/310x310/465x465**；同 boot 950s 层树 155x155 CALayerHost context = **`0x500b566f` / `0x7f535c4c`**，与表中任何 id 不对应。
- 同 boot `surface_upload=1` 的 202 从主队列发出后 host 收到 **全零**（`widget upload texture 0 … 0x0 rect 0,0`），截图 widget 区仍 mean 251 白块——主队列调 `eagl_remote_widget_upload` 的编码/remote 状态问题（P3a 从 widget 线程调用正常）；不影响枚举结论。

**结论**：P3 的「surface → opcode 202 → 真实 widget」路线**不可行**（没有可点的 surface id）；widget host 的像素在 extension 的远程 context 里，不在 MiniIOSurface 表。

### 4.31 s19o-p3-fallback 验收：host fallback 在截图里始终不可见，转 guest-side placeholder

> **状态：fallback2/fallback3 都已跑、host 侧 fallback 视觉不可交付；guest-side placeholder 待实现/验收。P3 未完成。** 第一发（11:18Z）：host 收到 surface 3 的 202 上传并 compose，但 **`fallback` 行 0**；截图 widget 区仍纯白（mean RGB 251，top color 255,255,255 ×208,907），无棋盘/白框/黄对角线。fallback2 画了（日志 ×4），但 `glwire_local=1` 的截图是 guest 合成 panel，host fallback 画在 replay fbo，二者不是同一块像素，所以截图仍纯白。

**配置**：root `f29594ab…`；host_glwire **119,552 B / md5 `68d89e541eba0fd34f0f671a21e24659`**（含 `GLWIRE_WIDGET_FALLBACK`）；`XCMD="glwire_local=1 widget_surface=3 widget_rect=46,142,309,309"`；`HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_WIDGET=1 GLWIRE_WIDGET_FALLBACK=1"`；`end=ui ui=750s biggest=356986 panic=0`。

**host 日志**：`widget upload texture 4294905859 184x124 … rect 46,142 309x309 bytes 91264` + 4 条 `widget compose`；`grep -ci fallback` = **0**。

**根因（host 侧）**：`host_widget_blank()` 要求 ≥90% 像素 **B,G,R,A 全部 >235**；surface 3 是未绘制的 184x124 BGRA（全 0/透明或 alpha<235）→ `blank=0` → fallback 分支不触发。

**下一步**：① 用 lead 已给的备选——只开 fallback + `GLWIRE_WIDGET_RECT=46,142,309,309`、**不带 widget_surface**（无 202），`widget_count==0 && have_env_rect` 无条件画占位；② 或 code 修 `host_widget_blank()` 把全 0/alpha=0 也算 blank 后再跑 202 版。**fallback 目前未通过验收，P3 未完成。**

#### fallback2（已跑，日志画了、截图看不到）

- 配置：root `f29594abf78d25d36582d497e2969446`；host_glwire **119,552 B / md5 `68d89e541eba0fd34f0f671a21e24659`**；`XCMD="glwire_local=1 widget_rect=46,142,309,309"`（**无 202**）；`HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_WIDGET=1 GLWIRE_WIDGET_FALLBACK=1 GLWIRE_WIDGET_RECT=46,142,309,309"`。
- boot：`end=ui` 660s、截图 **321,437 B**、无 panic。
- host：`widget fallback 1 rect(s) (NOT the real widget path), first 46,142 309x309` **×4**；`widget compose` = **0**（预期，无 202）。
- 截图：widget 区 mean **251**、`(255,255,255) ×208,907`、无棋盘/白框/两条黄对角线、纯绿 0 → **视觉验收未通过**。
- 根因：`glwire_local=1` 时截图是 **guest 合成 panel**；host `host_widgets_compose()` 画在 **replay fbo**，两者不是同一块像素 → fallback 行有、panel 截图不变。**fallback 代码/rect 本身 OK**，是验收路径错。

#### fallback3（已跑，host-replay 仍不可见）

- 配置：root `f29594ab…`；host 实际二进制 **119,552 B / md5 `28f484a1ed55da720cb7b78e4eb0f602`**（起跑时 `68d89e54…`，中途更新为 blank 修复版）；`XCMD="widget_rect=46,142,309,309"`（**无 glwire_local**）；`HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_WIDGET=1 GLWIRE_WIDGET_FALLBACK=1 GLWIRE_WIDGET_RECT=46,142,309,309 GLWIRE_DUMP=/tmp/s19o-p3-fallback3/f"`。
- boot：`end=ui` 660s、截图 **323,223 B**、host 3,367 帧、无 panic（`guestframes=0`，host-replay）。
- host：`widget fallback 1 rect(s) ... first 46,142 309x309` **×4**；`widget compose`=0。
- 截图：widget 区 mean **251**、`(255,255,255)×208,907`、无棋盘/白框/黄对角线、纯绿 0。
- dump：只在 **11:47（frame 2/3）** 写 picture/panel，当时启动黑屏、widget 区 0；11:56 转出的 PNG 同样黑——**UI 后没有 dump**。
- **定性**：QEMU 截图来自 **guest scanout（SpringBoard/panel）**；host `host_widgets_compose()`/fallback 只进自己的 **offscreen picture**，二者不是同一块像素。**host-replay 也改不了**；host 侧 fallback 无法视觉交付，P3a 的白块同理。

#### guest-side placeholder（已派 code，待实现/验收）

- `inject_sb.c` 加 **`widget_placeholder=1`**：UI 后在 `_SBWallpaperSecureWindow` / root view 的 widget 区域插一个圆角 `UIView`（24px 棋盘 + 3px 白边框 + 两条黄对角线），日志 **`static widget placeholder ... (NOT the real widget path)`**。
- exp 换 sb 库，boot `XCMD="glwire_local=1 widget_placeholder=1"` 验收；截图应能看到占位，**这是 guest scanout 上的拉面，不是真 widget 路径**。
- 结果待填；fallback 仍未验收通过，P3 未完成。

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

**P4 四格表**：`tools/glwire/harness/four-cell.sh` 是 **dry-run 骨架**，镜像由外部构建，未跑 QEMU；不要把它 `--dry-run` 读旧日志算出的表格当成一次实测四格表。

## 七、P2 壁纸重建配方（已执行一次；结果见 §4.15）

> **本配方已按 §4.15 跑过一次。** 首次执行结果：lookup 修好、PosterBoard 启动并 running，但**壁纸未出现**（无 1125x2436/全屏非 panel 纹理，背景纯黑），P2 未达成。构建/重建 `system.apfs` 期间跑测量会触发 tick-spinlock panic；必须等 **P1d 与当前测量全部结束、无 qemu** 后再做（§2.2 的 `ps` 检查）。P2 不改变 P0 的 2/3 结论。
> 任务文里写的 `com.apple.PostBoard.plist` 是简写/笔误；实际文件名是 **`com.apple.PosterBoard.plist`**，命令按实际名写。

### 7.1 前置与备份

```sh
export KERNEL=/host/iosvm-work/hakios-xnu-kernel
cd "$KERNEL/userland"

# 没有 qemu / host_glwire 才能重建
ps -eo pid,comm | grep -E 'qemu-system|host_glwire' || echo clean

# 上游 plist 必须在；仓库不带 Apple 文件
test -f ../sysimage/rootfs/System/Library/LaunchAngels/com.apple.PosterBoard.plist && echo plist-ok

# 备份当前镜像（GB 级；确认磁盘空间）
cp -a root.apfs /tmp/root.apfs.bak-P2
cp -a system.apfs /tmp/system.apfs.bak-P2
cp -a root /tmp/userland-root.bak-P2
```

### 7.2 改 `userland/build.sh`（两处）

**第一处**：`rm -rf root` 之后、`mkdir -p root/...` 之后，把作业 plist 放进去：

```sh
mkdir -p root/LaunchAngels
cp "$DARWIN/rootfs/System/Library/LaunchAngels/com.apple.PosterBoard.plist" root/LaunchAngels/
```

**第二处**：`rm -rf sysroot/System/Library/LaunchAngels` 之后、`ln -s /redwood/LaunchDaemons ...` 之后，把系统路径指到 `/redwood`：

```sh
ln -s /redwood/LaunchAngels sysroot/System/Library/LaunchAngels
```

这两处的语义：

- `root/` 就是 `/redwood` 卷；`root.apfs` 用 `mkapfs.py ... redwood=root` 生成。
- `sysroot/` 是 `system.apfs` 的根；`/System/Library/LaunchAngels` 变成一个指向 `/redwood/LaunchAngels` 的符号链接。
- plist 的 `Label` 是 `com.apple.PosterBoard`，MachService 是 `com.apple.posterboardservices.services`；它按需启动 `/Applications/PosterBoard.app/PosterBoard`，所以 `RunAtLoad=false` 是正常的。

### 7.3 重建并核对镜像

```sh
cd "$KERNEL/userland"
PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop SYSTEM=1 ./build.sh
```

`PREBUILT=1` 是因为容器里没有 `~/theos`；`SYSTEM=1` 强制重建 `system.apfs`（10–20 分钟），`root.apfs` 每次都会重建。期望末尾有 `root.apfs: ...` 与 `system.apfs: ...` 两行，且没有 `error`/`FAILED`。

> 首次执行（§4.15）的防回退步骤：构建前把 `prebuilt/userland` 里 17 个旧库临时同步成 `obj/` 的新版本，构建后恢复；备份在 `/tmp/prebuilt-userland.bak-P2`。若不做这一步，10-31 的旧库可能覆盖 11-07 的新库。

```sh
python3 ../tools/apfs/apfs.py ls root.apfs /LaunchAngels
# 期望: com.apple.PosterBoard.plist

python3 ../tools/apfs/apfs.py info system.apfs | head
# 先确认卷名；若不是 System，用下一行的 -v 换成实际卷名
python3 ../tools/apfs/apfs.py ls system.apfs /System/Library/LaunchAngels -v System
# 期望: LaunchAngels -> /redwood/LaunchAngels（符号链接）
```

### 7.4 启动（local composition + no-panel replay）

```sh
cd /host/iosvm
export REPO=/host/iosvm
export KERNEL=/host/iosvm-work/hakios-xnu-kernel
export GLWIRE=$REPO/tools/glwire
export ROOT=$KERNEL/userland/root.apfs
LABEL=s19p2-1
mkdir -p /tmp/$LABEL

XCMD="glwire_local=1" \
ZERO_RAM=1 QEMU_SMP=1 NOUI_BY=1800 MINFRAMES=99999 MAXSECS=2400 UI_GRACE=300 MINSHOT=300000 \
HOST_ENV="GLWIRE_NO_PANEL=1" \
IOSVM="$KERNEL" GLWIRE="$GLWIRE" \
tools/glwire/harness/boot-measure.sh "$LABEL" "$ROOT" 2400
```

### 7.5 判据（三条 + 失败要写崩在哪）

**1) `posterboardservices` lookup 不再 `No such process`**

```sh
grep -a 'posterboardservices' /tmp/$LABEL.log || true
grep -a -c 'posterboardservices.*No such process' /tmp/$LABEL.log
```

期望：第二条为 **0**；第一条不再出现 `error = 3: No such process`。若 0 行且没有成功 lookup 行，不能单凭这一条判成功，继续看 2/3。

**2) 宿主出现 1125x2436 或全屏壁纸纹理**

```sh
grep -a -o 'upload texture [0-9]* .* [0-9]*x[0-9]*' /tmp/$LABEL-host.log | sort | uniq -c
grep -a '1125x2436' /tmp/$LABEL-host.log
```

期望：出现 **1125x2436**，或至少一条非 `4294901761` 的全屏（750x1624）纹理。`4294901761 ... 750x1624` 是 guest 的合成 panel，不是壁纸，不能拿来充数。

**3) 主屏背景非纯黑**

```sh
python3 - "$LABEL" <<'PY'
import sys
from PIL import Image
label = sys.argv[1]
im = Image.open(f"/tmp/{label}-shot.png").convert("RGB")
w, h = im.size
pts = [(x, y) for x in range(8, w, 16) for y in (int(h * .12), int(h * .40), int(h * .60), int(h * .88))]
non = sum(1 for p in pts if max(im.getpixel(p)) > 12)
print(f"{label}: {non}/{len(pts)} edge/background samples non-black")
PY
```

期望：非黑采样明显大于 0，并**肉眼确认**主屏背景不是纯黑；把比例记进证据表，不要用单一阈值代替判据。

**4) 失败要写 poster 进程崩在哪**

```sh
grep -a -E 'PosterBoard|posterboardservices|failed lookup|SIGSEGV|panic\(' /tmp/$LABEL.log | tail -30
```

若壁纸仍未出现：记录 lookup 是否仍然失败、`PosterBoard.app/PosterBoard` 是否启动、崩在哪个调用（`exited due to SIGSEGV` / `usertrap:` / `panic(` 的具体行），不要只写「还是黑」。

### 7.6 风险与边界

- **构建时机**：任何测量期间都不要 `make`/`build.sh`；tick-spinlock panic 就是这样触发的。P2 重建排在 P1d 之后。
- **启动变化**：`system.apfs` 重建可能改变启动时间与内存布局；P2 证据表要同时记 `ui at`，不要拿 P0 的 `630s` 当固定值。
- **命名**：实际文件是 `com.apple.PosterBoard.plist`；不要按 `PostBoard` 新建一个错名文件。
- **不提交**：`root.apfs`、`system.apfs`、`ram.img`、日志、截图都不进 git；P2 证据图按 §5.2 先登记路径，不擅自提交。
- **不声称**：P2 配方写完不等于壁纸已出；只有三条判据都过才写「P2 达成」。

---

## 八、P4 四格表与收尾（可直接执行，结果待填）

> **状态：脚本就绪，未跑四格。** `tools/glwire/harness/four-cell.sh` 可执行，四格 pure-local/by-reference/w15/w15-w3；20s 窗口；列 `ui/frames/fps/span/shot B/host 0x502`；`--dry-run` 四行验证、不碰 QEMU；`0x502` 只数 host log，正常为 0；README 已加。**不要预设四格结果。**

### 8.1 四份构建定义（镜像外部构建）

| cell | 定义 | 关键构建 env | 建议镜像 |
|---|---|---|---|
| cell1 纯本地 | 没有记录器，guest 自己画 | `GL=1 CONFIGD=1`（无 `GLREMOTE`/`GLWIRE_*`） | `/tmp/root-local.apfs` |
| cell2 按引用 | guest 也画，surface 按引用上传 | `GL=1 GLREMOTE=1 GLWIRE_LOCAL=1 CONFIGD=1` | `/tmp/root-byref.apfs` |
| cell3 W1.5 | remote_only，host `--no-panel` 重放 | `GL=1 GLREMOTE=1 GLWIRE_NO_INPLACE=1 CONFIGD=1` | `/tmp/root-w15.apfs` |
| cell4 W1.5+W3 | 同 W1.5，打开 in-place 行写 | `GL=1 GLREMOTE=1 CONFIGD=1`（不要 `GLWIRE_NO_INPLACE`） | `/tmp/root-w15w3.apfs` |

- 所有 guest 构建统一 `PREBUILT=1 IOSVM_USE_LLD=1 RC=rc.vtop`；不要在一个 boot 里换镜像。
- `GLWIRE_LOCAL=1` 的构建把 `/redwood/etc/glwire_local` 写进镜像（guest 本地绘制）；cell3/4 的 host 需要 `GLWIRE_NO_PANEL=1` 走重放。
- 镜像尺寸/内容不同，**四格必须各用各的 image**；不能用同一个 root.apfs 冒充四格。

**构建命令（每格一份 root.apfs；system.apfs 不参与四格）**

```sh
cd /host/iosvm-work/hakios-xnu-kernel/userland

# cell1 纯本地（无记录器）
PREBUILT=1 IOSVM_USE_LLD=1 GL=1 CONFIGD=1 RC=rc.vtop ./build.sh
cp root.apfs /tmp/root-local.apfs

# cell2 按引用
PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 GLWIRE_LOCAL=1 CONFIGD=1 RC=rc.vtop ./build.sh
cp root.apfs /tmp/root-byref.apfs

# cell3 W1.5（W3 关）
PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 GLWIRE_NO_INPLACE=1 CONFIGD=1 RC=rc.vtop ./build.sh
cp root.apfs /tmp/root-w15.apfs

# cell4 W1.5+W3（in-place 开）
PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh
cp root.apfs /tmp/root-w15w3.apfs
```

> 每次 `build.sh` 也会处理 system.apfs；四格只用 root.apfs。不要把一个 root.apfs 复制成四份冒充四格；四个 env 组合必须分别构建。


### 8.2 20s 滚动窗口（同一触摸脚本）

`four-cell.sh` 调 `drive-scroll.sh`，默认 `WINDOW_FRAMES=0`（八次滑动，约 20s），并且 `WINDOW_AFTER=60` 等界面稳定后才开窗；四格同 `QEMU_SMP=1`、同触摸脚本。

```sh
cd /host/iosvm
CELL1_IMAGE=/tmp/root-local.apfs  CELL1_LABEL=cell1-pure \
CELL2_IMAGE=/tmp/root-byref.apfs  CELL2_LABEL=cell2-byref \
CELL3_IMAGE=/tmp/root-w15.apfs    CELL3_LABEL=cell3-w15 \
CELL4_IMAGE=/tmp/root-w15w3.apfs  CELL4_LABEL=cell4-w15w3 \
CELL3_HOST_ENV="GLWIRE_NO_PANEL=1" \
CELL4_HOST_ENV="GLWIRE_NO_PANEL=1" \
WINDOW_AFTER=60 QEMU_SMP=1 MINSHOT=200000 MAX_SECS=1800 \
tools/glwire/harness/four-cell.sh
```

- 只跑单格时给对应 `CELLn_IMAGE` 即可；缺 image 的格子会写 `SKIP`，不要用旧日志充数。
- `--dry-run` 只读已有 `/tmp/<label>-{host.log,log,shot.png,drive.txt}` 重算表，不跑 QEMU；当前已做四行验证；**dry-run 数字不是新实测**。
- 每格产物写到 `OUT`（默认 `/tmp/four-cell`）：`four-cell.md`、`four-cell.csv`、每格日志/截图/drive。

### 8.3 `0x502` 记账口径

- 只数**宿主日志**里的 `glerror 0x502`：脚本 `grep -c 'glerror 0x502' <label>-host.log`，填表列 `host 0x502`。
- P4 判据是 **`host 0x502` = 0**；脚本正常情况应为 0；某格 host log 缺失/没跑起来 → 写 `SKIP` 或 `-`，不写 0。
- 表列固定为：`cell | ui | frames | fps | span | shot B | host 0x502`；`ui`/`frames`/`fps`/`span` 取 `drive:` 行，`shot B` 取最大截图字节数。
- 纯本地格可能 host 0 帧/超时；如实写，不把 guest 帧数当 host fps。

### 8.4 `make test`

```sh
cd /host/iosvm
make test          # test-open（provenance + proto，如果 submodule 已取）→ test-gpu（host/vkbridge + lavapipe probe）
```

- `make test` 不跑 QEMU、不覆盖四格表；它只证明仓库的 open 检查与 host Vulkan probe 通过。
- 若 `host/vkbridge` 未取，先 `make submodule`；若只想跑不依赖 GPU 的门，用 `make test-open` 并记录原因。
- 记录完整命令、退出码与关键输出；失败不要用「应该没问题」代替。
- 2026-11-09 当前环境：`make test-open` **PASS**；`make test` 在 `test-gpu` 失败，
  `VK_ICD_FILENAMES` 指向的 `/usr/share/vulkan/icd.d/lvp_icd.json` 不存在（lavapipe 未配置），
  不是代码失败；需要补 ICD 后再重跑 `make test`，不要把 `test-open` 当完整 `make test`。

### 8.5 收尾清单

1. 四格表 `four-cell.md`/`csv`（四份镜像各一行，`0x502` 列）；
2. `make test` 输出与退出码；
3. 文档更新（runbook 本节 + plan 的 P4/D7）；
4. 只提交 `.md`/证据图，不提交 `.apfs`/日志/截图；
5. **P4 未执行前不要写四格数字**；不要用 dry-run 数字当结果。


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

1. 不重写 `docs/gpu-acceleration-stage19-plan.md`；只按 lead/owner 指定处更新 P0/P1 状态，不改历史结论。
2. 不在文档任务里新增探针/改 `host_glwire.c`；P2 的 `root.apfs`/`system.apfs` 重建按 §七 单独执行，不在文档任务里跑。
3. 不在文档任务里跑 qemu；本手册的所有命令都交给 `iosvm-exp` 执行。
4. 不提交 `root.apfs` / `system.apfs` / `ram.img` / 任何日志或 `.bgra` dump。
5. 不把「有字形」当「P0 通过」：通过线是 **≥2 发读出 ≥5 个 App 名称 + 截图 ≥300 KB + 同帧 diff ≥95%**。
6. **不再用 `GLWIRE_PROGRAM_SKIP=13` 当 P1 全局修复**：全标签 nonblack **101.7%**（Weather -97%，但 FaceTime +225%、Podcasts +432%），只是重分布。
7. **不再用 `GLWIRE_TC0_SANITIZE=1` 当 P1 修复**：Weather 带 nonblack **562→758（+35%）**，形态从碎点变规则竖线；tc0 不是主因。
8. **不再在合成/prog13 侧找点带**：P1c 已证明点带烘在 glyph tile 的 alpha 底部 6–8 行；修复在纹理生成/裁剪（P1d `GLYPH_TRIM`），在 P1d 数据前不要写「P1 已修复」。
9. **不再用 `+allContexts` 跨进程枚举 widget 的 CAContext**：它按进程隔离；backboardd 只看到自己的 context，SpringBoard 看到 16 个也不含 155x155 widget host（`0x4741275c`/`0xccf65063`）。
10. **不再假设这代 CAContext 有零参 `createImage`/`createCGImage`/`snapshot`**：实际只有 `createImageSlot:hasAlpha:` 等；零参快照前提不成立。
11. **不再把 `createImageSlot:hasAlpha:` 当快照**：它是**创建 slot**，不是取图；slot/options 参数不要猜。
12. **不再把 widget host 的 CAContext 当 SpringBoard/backboardd 自己的**：它属于 **widget extension 进程**；跨进程只能走 render server 或显式 id 传递。
13. **不再靠调 surface 3 的 rect/selector 修 widget 内容**：P3a 的 surface 3 是空白白块；P3 已转 fallback（静态/占位 widget）。
14. **host 侧画的东西在 QEMU 截图里结构性不可见**：截图 = guest scanout（SpringBoard/panel），host `widget compose`/fallback 只进 **offscreen picture**；fallback2（local panel）和 fallback3（host-replay）都因此看不到。以后做可视验收必须先确认像素归属，host 侧 fallback 只作分析用。

## 附录 C：编译环境（工具链阻塞已永久解除）

> owner 已把编译环境烤进镜像/同步到 4 个容器，并把 SDK/cctools/ld64 放进已挂载工作树。**guest 构建一律 `IOSVM_USE_LLD=1`**；缺 clang/SDK 时先查 `.zshrc` 和下列路径，**不要停下来上报「容器编不了」**。

**路径**

| 项 | 路径 |
|---|---|
| iOS SDK | `/host/iosvm-work/toolchain/theos/sdks/iPhoneOS16.5.sdk` |
| cctools linker | `/host/iosvm-work/toolchain/cctools/bin/aarch64-apple-darwin11-ld` |
| 运行库 | `/host/iosvm-work/toolchain/lib` |
| ld64 包装 | `/host/iosvm-work/toolchain/ld64` |

**`.zshrc` 变量（应已配置）**

- `THEOS` → theos 根（含 `sdks/iPhoneOS16.5.sdk`）
- `IOSVM_USE_LLD=1` → guest 构建统一走 lld 路径
- `CCTOOLS_LD64` → cctools 的 `aarch64-apple-darwin11-ld`
- `LD_LIBRARY_PATH` → 包含 `/host/iosvm-work/toolchain/lib`
- `PATH` → 包含 cctools/bin 与 theos 工具

**验证与构建**

```sh
# 先确认变量与路径
source ~/.zshrc 2>/dev/null || true
echo "$THEOS"; echo "$IOSVM_USE_LLD"; echo "$CCTOOLS_LD64"
test -d /host/iosvm-work/toolchain/theos/sdks/iPhoneOS16.5.sdk && echo sdk-ok
test -x /host/iosvm-work/toolchain/cctools/bin/aarch64-apple-darwin11-ld && echo ld-ok

# guest dylib / root.apfs：一律带 IOSVM_USE_LLD=1
cd /host/iosvm-work/hakios-xnu-kernel/userland
PREBUILT=1 IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh
```

**编译验证（已跑）**

- clang **19.1.7**（`/usr/bin/clang` → llvm-19）、`/usr/bin/ld64.lld`、包装器 `/host/iosvm-work/hakios-xnu-kernel/xnu-deps/host/bin/xnu-clang`。
- 命令（0.57s）：
  ```sh
  THEOS=/host/iosvm-work/toolchain/theos IOSVM_USE_LLD=1 \
    xnu-clang -arch arm64 -isysroot $THEOS/sdks/iPhoneOS16.5.sdk -O1 -dynamiclib \
    -install_name /redwood/lib/libhakios.sb.dylib -framework CoreFoundation -framework IOKit \
    -framework IOSurface -lobjc -o /tmp/libhakios.sb.test.dylib src/inject_sb.c
  ```
  结果：Mach-O arm64 magic `cffaedfe`，**108,248 B**，strings 含 `layers_at` 与 `layers timer %lus fired|scheduled`。
- P3 目标库也编译通过：`src/inject.c` → `libhakios.dylib` **89,856 B**；`src/eagl.c src/gl_osmesa.c src/eagl_remote.c` → `libhakios.gl.dylib` **400,656 B**（与镜像内 11-07 旧库 90,128 / 401,816 大小略异，clang/lld 版本不同；换库等 P3 GO）。

**已产出的新 dylib（P2c-timer）**

- `userland/obj/libhakios.sb.dylib`：**108,248 B**，`strings` 含 `layers_at`、`hakios: layers timer %lus scheduled`、`hakios: layers timer %lus fired`。
- 旧库备份：`/tmp/libhakios.sb.old.dylib`（108,040 B，exp 侧）。
- P2c-timer 镜像：只重建 root.apfs（03:35Z，5,902,336 B，md5 `dbf1c3d85d9cb35eb121af4401d02d51`）；镜像内 sb 库 md5 `1cd3053c0e127be55204e861f36e4047`；system.apfs 未动 `b75d80ca…`。

**规矩**

- 工具链在 `/host/iosvm-work/toolchain/`，**不进 git**；Apple SDK/二进制仍遵守 §5.1。
- 缺 clang/SDK 先查 `.zshrc`/路径；确实缺再上报，并附 `which clang`、`echo $THEOS`、`echo $CCTOOLS_LD64`。
- P3 的 guest 侧代码也走同一套 `IOSVM_USE_LLD=1` 构建。

## 附录：extension check-in 判定点（P3 离线调查，30 min）

- launchd 在 `Extension is hanging on launch` 前等待的终态是 `extension checked in as alive`（launchd strings 中有 `CheckIn` 与 `checked in as alive`）；判定点是 **launchd 收到扩展进程的 `CheckIn` 消息**，不是 plist 里的 timeout。
- C2 force xpcproxy 日志：pid 57 已到 `service state: running` / `INIT`，`uncorking exec source upfront`、`created` 后立即出现
  `Failed to send exception EXC_CORPSE_NOTIFY. error code: 5 for pid 57`；约 0.5 s 后 `Extension is hanging on launch. Killing.`，
  约 4 s 后 `XPC_EXIT_REASON_EXTENSION_CHECK_IN_TIMEOUT`。即进程在 exec 后/进入 `xpc_main` 前死亡，`CheckIn` 没发出，
  不是“动作太慢”。
- 因此 extension 侧必须做的动作是：**进程存活，并在 `xpc_main`/`_EXExtensionMain` 初始化后经 pid-domain bootstrap 发 `CheckIn`**；
  这段由 ExtensionKit/Foundation 自动完成，外部 launchd 配置改不了。
- 能走吗：**不能走**。不能靠 launchd plist 让一个已死进程 check-in；`ExtensionWatchdogTimeout=120` 已证明无效。
- 唯一具体改动（若还有工具注入 extension）：先让进程活到 `xpc_main`（崩溃根因未明），或试 `ExtensionWatchdog=false`
  （bool，不是 timeout）关闭 watchdog 以暴露真实 exit；但 C1 的 limits 读取路径未证实，成功率低，不应再花超过 30 min。
- 结论：P2/P3 的 extension 内容路径判“不能走/需 extension 进程存活这个前置条件”，不再作为 P3 主线。
- 唯一开关 `ExtensionWatchdog=false` 的预期：若 launchd 确实读取 limits，就不再在 5–13 s 杀 extension，但
  `CheckIn` 仍不会出现（进程已死/崩溃），只是暴露真实 exit；若 limits 读取路径未被证实，则行为完全不变。
  因此它只能用于 unblock 诊断，不能恢复 widget 内容，阶段 20 应按“换根因/换路径”而不是“调 timeout”。

### P4 四格表 dry-run 自检（不跑 QEMU）

`four-cell.sh --dry-run` 用四组合成日志（`/tmp/{pure-local,by-reference,w15,w15-w3}-*`）验证调度、标签与记账，
实际输出：

```
| cell | ui | frames | fps | span | shot B | host 0x502 |
|---|---|---|---|---|---|---|
| pure-local | 660s | 320 | 16.00 | 20.00s | 1001 | 1 |
| by-reference | 660s | 340 | 17.00 | 20.00s | 1002 | 0 |
| w15 | 660s | 360 | 18.00 | 20.00s | 1003 | 0 |
| w15-w3 | 660s | 380 | 19.00 | 20.00s | 1004 | 0 |
```

`host 0x502` 只数宿主日志 `glerror 0x502` 行；`ui/frames/fps/span/shot B` 与 CSV 同表。正式四格仍等 P4 镜像。


## 附录 D：阶段 19 收口决定 D1–D5（摘要）

完整记录（决定/依据/影响/何时可推翻）见 **`docs/gpu-acceleration-stage19-decisions.md`**。

| ID | 决定 | 一句话依据 |
|---|---|---|
| D1 | **P3 真路径 = 不可行，提前收口** | surface 枚举 30 面无候选；`+allContexts` 按进程隔离；无零参 snapshot；extension exec 后 ~0.5s 死亡、`CheckIn` 未发出（`EXC_CORPSE_NOTIFY` error 5） |
| D2 | **P3 交付 = guest-side placeholder（拉面）** | host fallback2/3 在截图里不可见（guest scanout vs offscreen picture）；唯一可见路径是 `inject_sb.c` 的 `widget_placeholder=1`；结论写「不可行 + fallback 交付（待 fallback4 截图）」，不写可行 |
| D3 | **P2 = 关闭，记未达成** | P2d 四 job、P2e C1 limits、C2 xpcproxy、根因审计四路径走完；与 P3 同源 extension check-in |
| D4 | **extension check-in 移交阶段 20 候选** | 18 个 extension 5–13s 被杀；timeout 120 无效、force xpcproxy 仍死、job 无关；唯一值得试 `ExtensionWatchdog=false` 暴露真实 exit code |
| D5 | **剩余时间全部转 P4 四格表实跑** | 四份构建 × 20s、host `0x502`=0、`make test`；dry-run 不算实测，四发真 QEMU 后填 |

> 追溯：D1 见 §4.28/§4.30/附录 extension check-in；D2 见 §4.29/§4.31；D3 见 §4.22–§4.25；D4 见 §4.24/§4.25/附录 extension check-in；D5 见 §八。提交列表见 decisions.md 的追溯表。

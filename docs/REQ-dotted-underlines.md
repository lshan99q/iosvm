# 需求文档：修复 iOS 虚拟机上「文字下划线显示为虚线/断点」的渲染缺陷

> 交付对象：外部工程师 / Claude（可独立完成）
> 项目：`/home/lshan/DSH-desktop/iosvm`（+ `/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel`）
> 撰写时间：2026-11-20，基于本仓库已有的两轮专项分析（下面「已有分析记录」一节是核心输入）

---

## 1. 目标与交付判据

**目标**：修复主屏图标名称（如 Photos / FaceTime / Wallet / Weather）下方的**白色下划线呈现为虚线/断点**的问题，
使它在远程回放画面里与真机一致（连续实线，或按 iOS 实际样式）。

**验收（像素级）**：
1. 在 750×1624 的回放画面里，某个标签下划线所在行（例如 Photos 标签下方 band）**连续像素占比 ≥ 99%**（现在约 30–70%，
   断点间距 2–3 px）；对比证据需给出「修复前/后」同位置裁剪图和断点计数。
2. 同一帧的其它内容不发生回退：画面 sha256 基线 `42e14ed2…` 保持不变（若该基线本身是补丁图，则用"与修复前的同一帧逐像素 diff，
   只允许下划线行变化"作为判据）。
3. 帧率不回退：CPU/帧不高于修复前 +2 ms（测量方法见第 6 节）。
4. 修复必须可通过环境变量/开关关闭（便于 A/B 与回滚）。

---

## 2. 项目背景（必须理解的架构）

这台虚拟机把 **iOS 16.5 的用户态**跑在 QEMU（TCG，aarch64，单/双 vCPU）里，图形走「远程回放」：

```
guest: SpringBoard/backboardd → CoreAnimation → libhakios.gl.dylib（eagl.c 记录器）
       每个 GL 调用 + 参数序列化 → 通过 TCP 发给宿主（默认 10.0.2.2:9110）
host : tools/glwire/host_glwire.c  → 用真实 GPU（NVIDIA V100, llvmpipe/GL）逐条回放
       回放结果写进一块持久 framebuffer → 输出到 SHM 画面（供录像/网页演示）与面板
```

要点：
- guest 侧记录器：`iosvm-work/hakios-xnu-kernel/userland/src/eagl.c`、`eagl_remote.c`、`gl_osmesa.c`；
- host 侧回放：`tools/glwire/host_glwire.c`，逐 opcode 分派表在 `tools/glwire/dispatch.inc`；
- 字体/文字：guest 用 CoreText 生成字形图集（atlas），以 `178x90` 的 label tile 贴到屏幕；
- **已知**：这些 label tile 的底部 1–3 行里被写入了「虚线段」，屏幕上的虚线就是这些 tile 行（不是采样放大）。

环境变量/调试开关（都已存在于代码里，直接用）：
`GLWIRE_TEXT_DRAW_LINE`、`GLWIRE_LABEL_DUMP`、`GLWIRE_TILE_DUMP`、`GLWIRE_ATLAS_DUMP`、
`GLWIRE_DUMP_AT=<frame>`、`GLWIRE_LABEL_ROW_SCRUB=61-63`、`GLWIRE_NO_PANEL=1`、`GLWIRE_POLL_MS`。

---

## 3. 缺陷现象（可复现）

- 主屏上每个图标名称下方有一条 1–3 px 的白色下划线；现在它呈**点状/虚线**，断点规则、长度随标签文字宽度（40–120 px）。
- 首次发现：stage19，`P1` 项；两轮分析后被**冻结**（不是无法复现，而是定位成本高）。
- 证据（仓库内已有，若目录已被清理可重新生成）：
  - `logs/s20-p1-54px-dash/shot-label-plus-band.png`（2x 放大：Weather 标签 + 下方虚线带）
  - `logs/s20-p1-54px-dash/shot-label-lower40.png`、`shot-under-label40.png`
  - `logs/s20-p1-54px-dash/tile-f121-alpha-4x.png`（Photos tile 的 alpha 4x）、`tile-f171-alpha-4x.png`（FaceTime）、
    `label-2-alpha.png`（Wallet）
  - 分析文档：`docs/gpu-acceleration-p1-54px-dash.md`

---

## 4. 已有分析记录（关键，别重复走过的路）

### 4.1 第一轮（stage19）：判定 A（tile 内容），排除 B（采样放大）
- 90 高 label tile 的**底部行**携带近不透明（alpha 241–253）的虚线段，屏幕上带的位置与 tile 行完全对应；
  没有出现「tile 行是空的但屏幕仍有带」的情况 → **B（线性放大采样）被排除**。
- 量到的具体行：
  | tile | 行 | 非零 alpha 宽 | alpha max |
  |---|---|---|---|
  | Photos（`tile-f121-t9-178x90`，对应主人报的 54 px） | 61 | **54 px** | 247 |
  | FaceTime（`tile-f171`） | 61/62 | 17 / 30 | 241 / 253 |
  | Wallet（`label-2`） | 58–61 | 42/32/32/40 | 246 |
  | label-4（第二标签） | 53–56 | 6/6/6/6 | 255/61 |
  | Weather（屏幕裁剪） | 61 | 120 guest px | — |
- 注意 nuance：带宽度 = **标签文字宽度**（40–120 px），不是整个 178 px tile 宽。
- 当时结论的「下一步（A 点）」：查 **guest 字形/label tile 生成侧**——`130x42` 字形纹理的最后一行 / 点阵被写进 `178x90` label tile，
  检查 row/stride/边界拷贝。

### 4.2 第二轮（stage21）：A 点也不成立 → 源是 native legacy `GL_ALPHA` teximage
- 假设 A 是「`glCopyTexSubImage2D` 把 tile 行 61–63 抄进 label tile」：实验里 `tile rows 61-63` / `cleared 178x90` 命中 **0 次**；
- 但 `glwire: legacy 178x90 scrub rows 61-63 before native alpha upload` 命中 **1 次** → **A 点不成立**，
  真实来源是 **native legacy `GL_ALPHA` teximage**（老式 alpha 纹理上传路径）。
- 记录见 `docs/gpu-acceleration-stage21-p1-copyscrub.md`；当时暂停（P1 冻结），因为再往下要动 Mesa 的 teximage 路径。
- 其它尝试（未成功）：host 侧 sanitize、skip13（prog13 跳过后虚线带 -92%，但同时画面/边框有其它回退），均非正解。

### 4.3 结论摘要（给外部接手人）
- 现象定位到 **label tile 底部行的内容**，该内容来自 **legacy GL_ALPHA teximage 上传**；
- 采样放大、glCopyTexSubImage2D 两条路都已排除；
- 还没做的：**在 legacy alpha teximage 的像素来源（guest 侧生成/上传内容）与 host 侧解码（格式/stride/预乘）之间二选一**，
  以及 Mesa softpipe 的 `GL_ALPHA` 处理是否把 alpha 当作亮度、stride 是否错位。

---

## 5. 建议的排查路径（按优先级）

1. **抓一条最小记录流**：启动时打开 `GLWIRE_TILE_DUMP` + `GLWIRE_LABEL_DUMP` + `GLWIRE_TEXT_DRAW_LINE=1`，
   在 `GLWIRE_DUMP_AT=800` 落盘，拿到 Photo/FaceTime/Wallet 的 178x90 tile 与其来源 teximage 的原始字节。
2. **对账**：把 tile 行 61 的字节与「legacy alpha teximage 的对应行」逐字节比对；确认是 guest 上传的字节本来就是虚线，
   还是 host 侧解码（`GL_ALPHA` → RGBA、stride/unpack alignment、premultiply）把连续像素打散。
3. **guest 侧检查点**：字形 atlas 里下划线是 1 条连续 rect 还是多个 rect；`130x42` 字形纹理的边界拷贝是否按 4 字节对齐（row padding）。
4. **host 侧检查点**：`dispatch.inc` 里 teximage 的 `glTexImage2D`/`glTexSubImage2D`（`GL_ALPHA` 格式）——
   像素解包（`glPixelStorei(GL_UNPACK_ROW_LENGTH/ALIGNMENT)`）与 Mesa softpipe 的 alpha 展开逻辑。
5. 修复后必须做 A/B：同一帧、同一 dump 点，下划线行连续率从 ~30–70% 提到 ≥99%，其余像素不变。

---

## 6. 复现与验证命令（当前仓库可直接用）

```bash
cd /home/lshan/DSH-desktop/iosvm
# 1) guest 镜像已配好远程回放；用启动器跑一发（单核，稳定）：
TAG=p1-repro NCPU=1 CORES=20,21 EXTRA="fb_hz=15" ./tools/glwire/run-pinned.sh
# 2) 等画面出现（约 12–20 分钟；日志 logs/p1-repro/serial.log 出现 "First boot is finished"）
# 3) 取一帧画面：
python3 /tmp/s24-shot.py logs/p1-repro/shm.bin /tmp/shot.png     # 需要该脚本，或参照 docs/ 的 shot 流程
# 4) 调试 dump（重启时加环境变量）：
#    GLWIRE_TILE_DUMP=1 GLWIRE_LABEL_DUMP=1 GLWIRE_TEXT_DRAW_LINE=1 GLWIRE_DUMP_AT=800
```

- 帧率/CPU 测量方法：`docs/stage26-ux.md`（10 s 窗口帧计数 + CPU/帧，必须带宿主 load 分组）。
- 交互注入：guest 内 `rwtouch`（`rc.ux`），录像用 `/tmp/s27-record.py`（VFR，按实际出帧）。
- 构建 guest 镜像：`GL=1 GLREMOTE=1 REST=1 CONFIGD=1 THEOS=<容器可见的 SDK> DARWIN=<kernel>/sysimage ./userland/build.sh`（容器 `iosvm--iosvm-code`）。
- 构建内核：见 `docs/stage27-multicore.md` 的构建修复一节（必须用 Theos 的 cctools `ld64` + `BUILD_DSYM=0`）。

---

## 7. 约束与非目标
- 不改帧率模型（remote-only、ack-only、`fb_hz=15`）；不得引入新的黑块/闪烁（见 `docs/stage26-visual-defects.md` 第一条）。
- 不重启多核方向、不动 `fbscale`。
- 所有改动必须默认关闭或可开关；必须给出回滚方法。

## 8. 交付物
1. 补丁（guest 或 host 侧皆可）+ 说明（根因、改动点、为什么）；
2. 证据：修复前/后的同位置裁剪图 + 断点计数 + 一帧全图 diff；
3. 回归：帧率（CPU/帧）与画面其他区域不变；
4. 一段 20 行以内的结论，写清「根因是什么、在 guest 还是 host、如何验证」。

# Stage 21 计划：文字修好之后的路线图

> **状态：计划，未执行。** 只写阶段、判据和判定规则；**不预写结论**。
> 输入：stage 19 D7、stage 20 E1 C、P1 最近三次失败、帧同步候选 1 未建立。
> 相关：`docs/gpu-acceleration-stage20-plan.md`、`docs/gpu-acceleration-stage19-closure.md`、`docs/gpu-acceleration-stage19-decisions.md`。

## 实验开关的工程规则（必读）

> 触发：`s20-rle-run` 的 RLE 开关没到客机（host 构建标记 `rgb565=1, rle=1`，但 host `bgra rle` / guest `upload-rle` 都 0 行）。这是同类问题**第 3 次**（565、boot-arg 512 截断、RLE）。

1. **每个实验开关必须自报**：init 打一行
   `options raw="<原文>" parsed local/fast/565/rle/dirty/ack`。
   **没有这行，开关视为不可信**；不能拿它做任何性能/功能结论。
2. **传参改文件**：开关写 `/tmp/glwire_opts`，boot args 只留 `glwire_local`。
   理由：开关变多后，boot args 的长度/转义是持续风险（512 截断已经发生过）。
3. **开关未生效的实验一律不作为性能结论**：RLE 这发（`s20-rle-run`，lib `974a0e52…`，root `3212f6a5…`，host `d80f7ca3…`）guest 4 帧/494 s、host 3 帧、无截图，**只能算 tooling 失败**，不能写「RLE 无效/有效」。
4. **当前结论链**：payload（每帧 4.9 MB）→ 拷贝/序列化（~740 ms/帧）→ 压缩是方向；但 **RLE 已判死（14 发，image1 texture `nonblack=0.00%`，四个源全黑）**，性能线以 **565-only** 为准。rawpipe 0.68 MB/s 的 net_test 说明 wire 本身不是 4.5 MB/s 天花板。
   - 不再写「RLE 已实现/待验证 13.5x」；RLE 代码保留但默认关。

## Phase 0（进行中）

### 0.0 P0 参考 system 与复现性（必读）

| 用途 | system md5 | 结果 | 位置/规则 |
|---|---|---|---|
| **历史 P0 system** | **`8bf5f838d52e46d6c8bf77f13a5ad946`** | **99.99%**（12:52 单次）/ **89.56%**（15:42） | 原件 `/tmp/system.apfs.bak-s20`；keep `/tmp/system-8bf5f838.keep`；**不许覆盖** |
| **对照** | **`6be494f1…`**（C1） | **89.50%** | keep `/tmp/system-6be494f1.keep` |

- **P0 历史 3/3 通过，但当前不可稳定复现**：8bf5 同配方 99.99/89.56；6be ~89.50%。**8bf5 必要但不充分**；单次 99.99 不能当基线。
- 门禁至少 **2–3 次 boot**；任何实验 report 必须写 **system md5 + lib md5**，否则不可比。
- **P0 判据已降级为内容判据**：adjacent-pair 矩阵无有效 ≥95（8bf 内容最高 89.56%；6be 的 100% 是 both-black mask，无效）。同帧/相邻 diff 只作历史证据；**本阶段门禁 = ≥5 可读名称 + 截图 ≥300KB + 无 panic**（panel/picture MATCH 只作诊断）；内容判据也需 2–3 发统计，未复跑前只写「历史证据，复现不稳」。
- 不写「P0 已稳定/当前可复现」，也不写「P0 被推翻」；是「历史通过、当前不稳定」。
- 若要用 6be 或别的 system 当基线，必须**重新定义 P0 判据**（按 ~89.5% 重设阈值或改成不依赖同帧配对）——**待定**。

### 0.1 P1 冻结（第二次重开不再进行）

- **状态：冻结**。虚线真实来源是 **native legacy `GL_ALPHA` teximage**；三个 scrub 点（`glyph_last_row=1`、`label_dash_rows=53-62`、`61-62`）都没打中，两次重开预算不再使用。
- 历史只作背景：`glyph_last_row=1` 掉笔画；`53-62` crash；`61-62` 无效果。
- code 会补一页 P1 终局说明；本文不预写「已修」。P1 **未修复、冻结**，不再起 boot。

### 0.2 帧同步 clamp 复测

- **前置**：先修 guest `setsockopt` **errno 55 (ENOBUFS)**；必须两侧同时生效（host `SO_SNDBUF` + guest `SO_RCVBUF`），不能只看 host 单侧。
- **实验**：cell3 同镜像，**120 帧窗口**，`WINDOW_FRAMES=120`、cell3 host 窗口；cell1 作为护栏。
- **判据**：
  - cell3 **≥2 fps**（120 帧窗口）；
  - cell1 **10.91 fps 不变**（120 帧 / 11.00s，guest 时钟）；
  - `0x502` 仍为 0；画面/P0 三判据不回归。
- **失败**：候选 1 未建立 → 按 Phase 2 第 5 条转 host 回放侧/guest 合成侧，不再单独调 host buffer。

### 0.3 ack 实验 = 未判定（tooling 坏）

- `s20-ack785` / `s20-ack785b` 的 **UI 没起**，是 lib **`4a7cd6` 的 boot 回归**；**不是 ack 假设被否**，不能写「ack 不是瓶颈」。
- 状态：**未判定（tooling 坏）**；lib 修复/回退后重跑，先 isolate 再 ack 版。
- **lib 时间线**：`4a7cd6` 会让 UI 起不来；已回退/待修。所有实验 report 必须写 **lib md5 + system md5**。
- 来源：`docs/gpu-acceleration-stage20-ack785.md`、`docs/gpu-acceleration-stage20-ack785b.md`、`docs/gpu-acceleration-stage20-iso785.md`。

## Phase 1：视觉完整性（P1 之后最高优先）

### 1.1 壁纸静态兜底：不成立

- **结论：不成立。** PosterBoard 托管的是实时场景，SpringBoard 侧没有可替换的位图；静态兜底替代不了实时壁纸。
- 只剩 **extension 路径**（已封）；不再走静态图 workaround，也不写「壁纸已有可行替代」。
- 证据：P2 收口/runbook §4.15 系、`docs/gpu-acceleration-stage19-closure.md`。

### 1.2 主屏细节收口

- **做什么**：A1c overlay / 右缘首帧竞态（P0-3 的 **1.19% extra / 14,555 px**，box 54,1092,173,1253）、图标与标签边缘。
- **判据**：同帧 diff **≥99%**；无额外像素带；P0 三判据不回归。
- **边界**：不重开 P3 真 widget；不碰 host sampler/linear/clamp。

### 1.3 widget 保持 fallback5

- 继续用 guest-side placeholder（拉面）作为 P3 交付；**不再追求真 widget**，直到 extension check-in 前置解决。
- 不写「真 widget 已可用」；fallback5 是占位。

## Phase 2：性能（GPU 加速真目标）

### 2.1 瓶颈演进（按证据）

| 阶段 | 瓶颈 | 证据 |
|---|---|---|
| ① 4.9 MB 拷贝/序列化 | **~740 ms/帧**；host 回放只有 29–35 ms | 全屏 4,872,000 B，guest ~1.2 fps；host 能力 16–33 fps |
| ② 逐像素路径（fast/565 方向） | fast1 的上载仍 **886 ms median**；TCP 是下一层 | `s20-fast1`；565 开关未接上（0 行） |
| ③ **wire 字节吞吐（当前）** | **3.2–5 MB/s**（4,872,000 B in 977–1516 ms）；10 fps × 4.87 MB 需要 **~48 MB/s** | `s20-5651`：guest 已到 **10–14 fps**，host 端仍跟不上 |

- 结论：端到端瓶颈已从「**客机渲染/同步**」迁移到「**wire 通道字节吞吐**」；不能再只看 guest 帧率。
- `s20-5651` 的 **565 开关这发没生效（host `rgb565`/guest `upload565` 都 0 行）**，原因是**开关没接上**，不是性能结论；下一次要验「payload 减半 → host 能否跟上」。

### 2.2 当前判据：host 端到端 fps

- **产品路径（默认，完整画面）稳态 2.0–3.3 fps**；ext38 3480 帧/1700 s ≈ 2.05 fps；启动慢窗 f3 503 s/f4 70 s/f5 25 s（R4）。
- **技术起步判据 = host 端到端 ≥3 fps**；**目标 5+**；产品路径目前**未稳定过 3**，不能称「加速可用」。
- **「加速可用」仍只认 ≥5 fps（≥50% 纯本地 10.91）**；3–5 只能写「接近/起步」，不叫可用。
- **模式必须写清**：4.6–5.0 fps / 10.91 fps 是 `GLWIRE_NO_PANEL=1` 实验模式（host 回放、画面缺 dock/Search/右下），**不是产品性能**。
- guest fps 只作诊断；报告必须同时给 host fps、guest fps、模式、上载字节/耗时。
- **host 12 帧卡死（`sent 0 rows`）仍是必须先修的阻塞**；不修不判 5+。
- **目标（待验证）**：脏区上载平均 **25–50% 面积 → 5–8 fps**；静止 UI 若能到 **<10% 面积 → 期望接近纯本地 10.9 fps**。当前证据：25% 面积只有 2.0–3.4 fps；dirty 静止 0.3–0.7% 是 4.6–5.0 fps——**都还没到目标**，先修 host 565 开关/12 帧卡死再验。

### 2.3 dirty 的适用边界

- **有效**：静止/局部更新，**1–4 tiles / 16–65 KB（0.3–0.7% 面积）**；`s20-dirty-idle` 静止 UI **4.6–5.0 fps**，host **2342 帧**，UI 321,741 B。
- **无效**：滚动时约 **310/312 tiles、4.84 MB（~99.5%）** 全脏，dirty 没有字节收益；滚动仍走全帧路径（`s20-dirty-scroll2`，fallback host 到 frame 113 后停，另案）。
- 因此：**dirty 只适合静止/局部刷新**；滚动/全屏更新要另走全帧路径或别的优化，不能把静止的 5 fps 当成滚动也成立。

### 2.4 备选待验证：`fbscale=1.5`

- **待验证，不写结论**：`fbscale=1.5` 把 payload 降到 **~56%**。
- 判据：**host 端到端 ≥3 fps** 且 **标签可读**（内容判据不回退）。
- 未跑；与 RLE 无关（RLE 已判死）。

### 2.5 性能线本阶段收口（按模式）

| 模式 | 场景 | 画面 | 数字 | 判定 |
|---|---|---|---|---|
| **产品默认**（无 NO_PANEL） | 稳态 | 完整 | **2.0–3.3 fps**（平均 2.05） | **未稳定过 3，不叫可用** |
| **产品默认** | 启动 R4 | 完整 | f3 503 s/f4 70 s/f5 25 s，f8 起 0.49 s | 可用性问题，另案 |
| 实验 `NO_PANEL=1` | 静止/dirty | 缺 dock/Search 右半/右下 | 4.6–5.0 fps | 实验数字，不是产品 |
| 实验 `NO_PANEL=1` | 滚动 | 缺 | ~1.8 fps | 实验数字，不是产品 |
| 实验 `NO_PANEL=1` | 纯本地 cell1 | 非产品路径 | 10.91 fps | 仅实验对照 |

- **RLE 判死（14 发，image1 `nonblack=0.00%`）**；565-only 数字只在实验模式自洽。
- 产品路径未达 **≥3 fps 底线**；不准拿 4.6–5.0/10.91 当产品性能。
- 本阶段不再追加 RLE/ack/socket 实验；`fbscale=1.5` 仅登记待验证，不启动。

### 2.6 稳定态长跑

- **30 分钟交互**：滚动 / 开合 App / 回主屏。
- 判据：无 panic、无 `0x502`、host 端到端帧率不退化（同窗口对比）；记录 ui/帧数/截图。

## Phase 3：工程化交付

1. **一条命令复现**：建齐四格镜像（cell3/cell4 可共用一份，按 §8.1）→ 跑四格 → 出表 → 与基线比对。
2. **门禁脚本**：P0 内容判据 + `0x502` + 无 panic + 截图产物，一条命令出 PASS/FAIL（规范见「交付收尾包」；P1 冻结、壁纸无可行路径，不入门禁）。
3. **移交更新**：stage 21 状态、已知限制、复现入口；**virtio-gpu 仅保留评估，不启动**（大工程，见 `docs/gpu-acceleration-gpu-passthrough-assessment.md`）。

## 交付收尾包（Stage 21 主交付）

### 一页结论

| 项 | 结论 | 证据 |
|---|---|---|
| **性能（按模式）** | **产品默认（完整画面）稳态 2.0–3.3 fps**，平均 2.05；启动 R4 f3 503 s/f4 70 s/f5 25 s。**4.6–5.0 / 10.91 是 `NO_PANEL=1` 实验模式**，不是产品；RLE 判死；payload 链不变 | `docs/gpu-acceleration-stage21-performance-current.md`、`logs/s22-ext/ext38/`、`logs/s20-dirty-idle/`、`logs/s20-rle14/` |
| **P1（冻结）** | 虚线真实来源 = **native legacy `GL_ALPHA` teximage**；三个 scrub 点都没打中；终局页见 `stage21-p1-copyscrub.md`；**未修复、冻结** | `docs/gpu-acceleration-stage21-p1-copyscrub.md`、`docs/gpu-acceleration-stage20-p1-labeldash.md`、`logs/s20-p1-copyscrub/`、`logs/s20-p1-labeldash/` |
| **壁纸（不成立）** | PosterBoard 托管实时场景，SpringBoard 无位图；静态兜底替代不了；只剩 extension 路径（已封） | P2 收口、`docs/gpu-acceleration-stage19-closure.md`、runbook §4.15 系 |
| **P0** | 历史证据 + **内容判据**（≥5 名称 / 截图 ≥300KB / 无 panic）；P0 不是「当前同帧可复现」 | `logs/s20-p0-repro/`、`logs/s20-p0-8bf5-recheck/`、`logs/s20-p0-matrix/`、decisions D9 |

### 门禁脚本规范（先写规范，不实现）

**一条命令**跑：内容判据 + `0x502` + 无 panic + 截图产物 → `PASS/FAIL`。

输入：

- `--system-md5`、`--lib-md5`（必填；缺一不可比）
- `--shot <png>`、`--host-log <log>`、`--names <txt>`（或 `--manual-names`）
- 可选 `--guest-log`、`--out <dir>`

检查项：

1. **内容/名称**：`names` 至少 **5** 行（或人工勾选 recorded）；否则 FAIL。
2. **截图**：`shot` 存在且 **≥300,000 B**；否则 FAIL。
3. **无 panic**：host/guest log 中 `panic(` / `SIGSEGV` / `usertrap` 计数为 0；否则 FAIL。
4. **`0x502`**：host log `glerror 0x502` 计数为 **0**；否则 FAIL。
5. **产物**：把实际截图路径、字节数、三项结果写进 `gate.md`。

输出：`gate.md`（PASS/FAIL + 每项证据 + system/lib md5），进程退出码 0/1。**脚本已实现并验证：`tools/glwire/harness/gate.sh`（code 实现，exp 做 PASS/FAIL 双向验证）。**
- 已验证 PASS：`GATE: PASS (checks: shot=427K names=18 0x502=0 panic=0)`；FAIL 分支能精确报 `screenshot 445074 B < 10485760 B`。

- 脚本入口：`tools/glwire/harness/gate.sh`
- 验证记录见 `logs/s20-gate-verify/`。

### 「可用」判定

| 项 | 结果 |
|---|---|
| 文字可读 | ✅ |
| 壁纸非黑 | ❌ |
| 无 panic | ✅ |

→ **未达「可用」**：差的是**壁纸**，且静态兜底不成立、extension 路径已封，**当前没有可行路径**。不写「stage 21 可用」。

### 下一阶段入口

- **extension check-in**：需要 XNU/launchd 层改动；当前缺工具链/注入能力。若立项，见 stage 20 E1 C、D4，以及 stage 22 计划 `docs/gpu-acceleration-stage22-plan.md`。
- **virtio-gpu**：大工程，**仅评估，不启动**；见 `docs/gpu-acceleration-gpu-passthrough-assessment.md`。

### A. 三条命令复现

> 前提：system 必须是 **`8bf5f838`**（P0 参考 system；不是 6be）；root 镜像来源见下，换机器必须用同 md5 的 root/system。

1. **纯本地基线（cell1）**
   ```bash
   tools/glwire/harness/gate.sh --root /tmp/root-local.apfs --out /tmp/gate-cell1
   ```
   - `/tmp/root-local.apfs` = **P4 attempt3 cell1** 构建（root md5 `f39b910543189be4934a48c4d495e928`）。

2. **远程路径对照（cell3）**
   ```bash
   tools/glwire/harness/gate.sh --root /tmp/root-w15.apfs --out /tmp/gate-cell3
   ```
   - `/tmp/root-w15.apfs` = **P4 attempt2 cell3**（root md5 `785bcbac82c9a117684b2d40b7375d48`；cell3/cell4 同一份）。
   - 若要 **565/fast**，加 env 前缀：
     ```bash
     XCMD="glwire_local=0 glframes=1 touch_lat=1 glwire_upload_565=1 glwire_upload_fast=1 glwire_dirty_upload=0 glwire_ack_ms=0" \
     HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1 GLWIRE_POLL_MS=16 GLWIRE_UPLOAD_565=1" \
     tools/glwire/harness/gate.sh --root /tmp/root-w15.apfs --out /tmp/gate-cell3
     ```

3. **看结果**
   ```bash
   cat /tmp/gate-cell1/gate.md
   ```
   - `gate.md` 含 shot/names/`0x502`/panic 与产物 md5。

### B. 现在能做什么 / 不能做什么

**能：**

1. 产品默认路径完整画面 **2.0–3.3 fps**（可交互但慢）；实验 `NO_PANEL=1` 的 10.91 fps **不是产品**。
2. 标签可读（P0 内容判据 PASS）。
3. 门禁一条命令判 PASS/FAIL（已验证 PASS shot=427K/names=18；FAIL 能精确指出 `screenshot ... < ...`）。

**不能：**

1. 远程路径不能用：滚动 **~1.8 fps**、RLE 判死。
2. 壁纸黑：PosterBoard 托管实时场景，静态兜底不成立。
3. 标签下虚线仍在（P1 冻结）；真 widget 仍是占位。

## 不做（本阶段）

1. **不再调 ack 超时**：已证非限流点；ack 实验因 lib `4a7cd6` boot 回归而未判定，不要拿它当结论。
2. **不再调 socket buffer**：host/guest 单侧无效，候选 1 未建立。
3. **不再追 P0 单配对 ≥95%**：adjacent-pair 无有效 ≥95，P0 已降级为内容判据。
4. **不拿 `NO_PANEL=1` 的实验数字当产品性能**（4.6–5.0/10.91 均属实验模式）。
5. 不再用 host sampler/linear/clamp 修 P1；不再 scrub glyph last row。
6. 不擅自改宿主 GPU 驱动/vfio；virtio-gpu 只评估不启动。

## 判定规则

1. **同一方向失败 ≥3 次 → 判死**，进「不做」清单，不再起 boot（除非 owner 另定）。**P1 已冻结（见 0.1），不再起 boot。**
2. **host 端到端 fps 为准**：技术起步 ≥3 fps，目标 5+；**「加速可用」只认 ≥50% 纯本地**（≥5 fps）；低于 5 不叫可用。guest fps 只作诊断。
3. **「可用」= 文字可读 + 壁纸非黑 + 无 panic**，三项齐；缺一项不写「可用」。
4. 不写「性能达标」；只写实测数字与判据是否满足。
5. 负结果按负结果记录，不写成进展；失败行号/命令/截图必须可追溯。

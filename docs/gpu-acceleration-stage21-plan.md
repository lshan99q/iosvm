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
4. **当前结论链**：payload（每帧 4.9 MB）→ 拷贝/序列化（~740 ms/帧）→ **压缩是唯一出路**（RLE 13.5x 待验证；rawpipe 0.68 MB/s 的 net_test 说明 wire 本身不是 4.5 MB/s 天花板，压缩路径才值得继续）。
   - 在 RLE 开关自报通过前，不写「RLE 已实现/已生效」；13.5x 只是待验证目标。

## Phase 0（进行中）

### 0.0 P0 参考 system 与复现性（必读）

| 用途 | system md5 | 结果 | 位置/规则 |
|---|---|---|---|
| **历史 P0 system** | **`8bf5f838d52e46d6c8bf77f13a5ad946`** | **99.99%**（12:52 单次）/ **89.56%**（15:42） | 原件 `/tmp/system.apfs.bak-s20`；keep `/tmp/system-8bf5f838.keep`；**不许覆盖** |
| **对照** | **`6be494f1…`**（C1） | **89.50%** | keep `/tmp/system-6be494f1.keep` |

- **P0 历史 3/3 通过，但当前不可稳定复现**：8bf5 同配方 99.99/89.56；6be ~89.50%。**8bf5 必要但不充分**；单次 99.99 不能当基线。
- 门禁至少 **2–3 次 boot**；任何实验 report 必须写 **system md5 + lib md5**，否则不可比。
- **P0 判据已降级为内容判据**：adjacent-pair 矩阵无有效 ≥95（8bf 内容最高 89.56%；6be 的 100% 是 both-black mask，无效）。同帧/相邻 diff 只作历史证据；P0 门禁 = **≥5 可读名称 + 截图 ≥300KB + panel/picture MATCH + 无 panic**；内容判据也需 2–3 发统计，未复跑前只写「历史证据，复现不稳」。
- 不写「P0 已稳定/当前可复现」，也不写「P0 被推翻」；是「历史通过、当前不稳定」。
- 若要用 6be 或别的 system 当基线，必须**重新定义 P0 判据**（按 ~89.5% 重设阈值或改成不依赖同帧配对）——**待定**。

### 0.1 P1 最后定向修

- **目标**：只改 **label tile 的虚线行**（178x90 row61/62；本轮 tile 实际还出现 61–63，按 per-tile offset 记录），不碰 glyph 最后一行。
- **已知三次失败**（收口表必须记录行号/命令/截图）：
  1. `glyph_last_row=1`：清到 42 高 glyph 最后一行（笔画底部），Weather 掉笔画、虚线仍在。
  2. `label_dash_rows=53-62`：boot **crash**（`end=crash ui=0 SIGSEGV=1`）。
  3. `label_dash_rows=61-62`：能 boot，但 **54px 虚线仍在**（tile rows61/62/63 仍有 alpha），无可见效果。
- **最后一发**：映射实际 dash 行（61–63/per-tile offset），避开 53–62 的 crash 区间；验收面 = **guest 截图**（host tile dump 只作参考）。
- **判据**：guest 截图 Weather/Notes 下方虚线消失；P0 三判据不回归（截图 ≥300K、同帧 diff ≥95%、名称可读）；无 crash/SIGSEGV。
- **失败即判死**：最后一发仍失败 → P1 进「不做」，记录三次失败行号，不再起 boot（除非 owner 另定）。

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

### 1.1 壁纸静态兜底（不依赖 extension）

- **做什么**：给 wallpaper layer 塞静态图，绕开 extension check-in；不调用 PosterBoard/extension。
- **判据**：
  - 截图 top color 不是单一黑；主屏背景非黑（采样点非黑）；
  - `diffdump` 与无壁纸基线相比**不劣化**（同帧 diff 不低于基线；P0 三判据不回归）。
- **收口**：若静态图仍必须 extension 才能上屏 → 写 **「workaround 不成立」**，不再耗 P2 时间。

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

- **技术起步判据 = host 端到端 ≥3 fps**；**目标 5+**。
- **「加速可用」仍只认 ≥5 fps（≥50% 纯本地 10.91）**；3–5 只能写「接近/起步」，不叫可用。
- guest fps 只作诊断，不再单独作为通过判据；报告必须同时给 host fps、guest fps、上载字节/耗时。
- **host 12 帧卡死（`sent 0 rows`）仍是必须先修的阻塞**；不修不判 5+。
- **目标（待验证）**：脏区上载平均 **25–50% 面积 → 5–8 fps**；静止 UI 若能到 **<10% 面积 → 期望接近纯本地 10.9 fps**。当前证据：25% 面积只有 2.0–3.4 fps；dirty 静止 0.3–0.7% 是 4.6–5.0 fps——**都还没到目标**，先修 host 565 开关/12 帧卡死再验。

### 2.3 dirty 的适用边界

- **有效**：静止/局部更新，**1–4 tiles / 16–65 KB（0.3–0.7% 面积）**；`s20-dirty-idle` 静止 UI **4.6–5.0 fps**，host **2342 帧**，UI 321,741 B。
- **无效**：滚动时约 **310/312 tiles、4.84 MB（~99.5%）** 全脏，dirty 没有字节收益；滚动仍走全帧路径（`s20-dirty-scroll2`，fallback host 到 frame 113 后停，另案）。
- 因此：**dirty 只适合静止/局部刷新**；滚动/全屏更新要另走全帧路径或别的优化，不能把静止的 5 fps 当成滚动也成立。

### 2.4 下一发（未跑）

- 先按「实验开关工程规则」修 565/RLE 自报与 `/tmp/glwire_opts`；再修 host 12 帧卡死。
- 然后 **payload 减半 + RLE** 各一发（开关必须自报通过）；看 host 端到端能否 ≥3 fps，并验证 RLE 13.5x 是否成立。
- 若 host 端到端有效：再复测 cell2/cell4（先重建 `/tmp/root-w15w3.apfs`），产出三路 120 帧对比，基准 cell1 10.91。
- 若无效：转 host 回放侧/guest 合成侧，指出瓶颈层 + 一个可测改动。

### 2.5 稳定态长跑

- **30 分钟交互**：滚动 / 开合 App / 回主屏。
- 判据：无 panic、无 `0x502`、host 端到端帧率不退化（同窗口对比）；记录 ui/帧数/截图。

## Phase 3：工程化交付

1. **一条命令复现**：建齐四格镜像（cell3/cell4 可共用一份，按 §8.1）→ 跑四格 → 出表 → 与基线比对。
2. **门禁脚本**：P0 内容判据 + P1 虚线 + 壁纸 + `0x502`，一条命令出 PASS/FAIL。
3. **移交更新**：stage 21 状态、已知限制、复现入口；**virtio-gpu 仅保留评估，不启动**（大工程，见 `docs/gpu-acceleration-gpu-passthrough-assessment.md`）。

## 不做（本阶段）

1. **不再调 ack 超时**：已证非限流点；ack 实验因 lib `4a7cd6` boot 回归而未判定，不要拿它当结论。
2. **不再调 socket buffer**：host/guest 单侧无效，候选 1 未建立。
3. **不再追 P0 单配对 ≥95%**：adjacent-pair 无有效 ≥95，P0 已降级为内容判据。
4. 不再用 host sampler/linear/clamp 修 P1；不再 scrub glyph last row。
5. 不擅自改宿主 GPU 驱动/vfio；virtio-gpu 只评估不启动。

## 判定规则

1. **同一方向失败 ≥3 次 → 判死**，进「不做」清单，不再起 boot（除非 owner 另定）。
2. **host 端到端 fps 为准**：技术起步 ≥3 fps，目标 5+；**「加速可用」只认 ≥50% 纯本地**（≥5 fps）；低于 5 不叫可用。guest fps 只作诊断。
3. **「可用」= 文字可读 + 壁纸非黑 + 无 panic**，三项齐；缺一项不写「可用」。
4. 不写「性能达标」；只写实测数字与判据是否满足。
5. 负结果按负结果记录，不写成进展；失败行号/命令/截图必须可追溯。

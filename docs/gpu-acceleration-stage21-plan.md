# Stage 21 计划：文字修好之后的路线图

> **状态：计划，未执行。** 只写阶段、判据和判定规则；**不预写结论**。
> 输入：stage 19 D7、stage 20 E1 C、P1 最近三次失败、帧同步候选 1 未建立。
> 相关：`docs/gpu-acceleration-stage20-plan.md`、`docs/gpu-acceleration-stage19-closure.md`、`docs/gpu-acceleration-stage19-decisions.md`。

## Phase 0（进行中）

### 0.0 P0 两套 system 镜像（必读）

| 用途 | system md5 | 结果 | 位置/规则 |
|---|---|---|---|
| **P0 基线** | **`8bf5f838d52e46d6c8bf77f13a5ad946`** | **98.82%**（历史 p0-3） | 原件 `/tmp/system.apfs.bak-s20`；另存 `/tmp/system-8bf5f838.keep`，**不许覆盖**；exp 复现中 |
| **当前 system** | **`6be494f1…`**（C1） | **89.50%**，不可复现 P0 | 当前盘上；不要和 8bf5 数字混用 |

- **任何 P0 复跑必须声明 system md5**；不声明，数字不可比。
- 若要把 6be 作为唯一基线，必须**重新定义 P0 判据**（按 6be 89.5% 重设阈值，或改成不依赖同帧配对）——**待定**，等 exp 用 8bf5 原件的复现结果回来再定。
- 当前只写「历史 PASS、6be 不可复现、8bf5 复现在途」，不写 P0 被推翻。


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

### 2.1 若帧同步有效（cell3 ≥2 fps）

- 同改动复测：
  - cell2（byref）；
  - cell4（w15+w3）；**先重建 `/tmp/root-w15w3.apfs`**（当前缺失）。
- 产出 **三路 120 帧对比表**，基准 = **cell1 10.91 fps**。
- **目标：远程路径 ≥ 纯本地 50%（≥5 fps）**；达不到就写「未达 50%」，不写性能达标。

### 2.2 若无效

- 转 **host 回放侧**（29–35 ms/帧）与 **guest 合成侧**的瓶颈定位。
- 判据：**指出瓶颈在哪一层**（host replay / guest composition / socket/协议），并给出**一个可测改动**；不泛泛写「继续优化」。

### 2.3 稳定态长跑

- **30 分钟交互**：滚动 / 开合 App / 回主屏。
- 判据：无 panic、无 `0x502`、帧率不退化（同窗口对比）；记录 ui/帧数/截图。

## Phase 3：工程化交付

1. **一条命令复现**：建齐四格镜像（cell3/cell4 可共用一份，按 §8.1）→ 跑四格 → 出表 → 与基线比对。
2. **门禁脚本**：P0 三判据 + P1 虚线 + 壁纸 + `0x502`，一条命令出 PASS/FAIL。
3. **移交更新**：stage 21 状态、已知限制、复现入口；**virtio-gpu 仅保留评估，不启动**（大工程，见 `docs/gpu-acceleration-gpu-passthrough-assessment.md`）。

## 判定规则

1. **同一方向失败 ≥3 次 → 判死**，进「不做」清单，不再起 boot（除非 owner 另定）。
2. **「加速可用」只认 ≥50% 纯本地**：远程路径 fps ≥ cell1 10.91 的 50%（≥5 fps）；低于此不叫可用。
3. **「可用」= 文字可读 + 壁纸非黑 + 无 panic**，三项齐；缺一项不写「可用」。
4. 不写「性能达标」；只写实测数字与判据是否满足。
5. 负结果按负结果记录，不写成进展；失败行号/命令/截图必须可追溯。

# Stage 23：virtio-gpu 工程 + TCG 优化

主人 2026-11-14 下令：**开始 virtio-gpu 工程并优化 TCG**（不换硬件）。

## 为什么是这两件事

当前产品路径（guest 合成 panel）稳态 **2–3.3 fps**，两堵墙：

1. **传输墙**：每帧全帧 4.87 MB 走 TCP，实测通道 **4.5 MB/s** → 全屏动画上限 ~1.8 fps（565 也只有 2.44 MB/帧）；
2. **TCG 墙**：客机 aarch64 在 x86 宿主上纯软件翻译，**单核**实测纯本地天花板 **10.91 fps**。

- virtio-gpu 修的是**传输墙**（像素走共享内存/GPU 资源，不再过 TCP，后期 virgl 可把 GL 渲染交给 V100）；
- TCG 优化修的是**翻译开销**（多核 MTTCG、tb-size、CPU 模型），不换硬件。

## 环境底牌（已确认）

- QEMU **8.2.2**，支持 `virtio-gpu-device`、`virtio-gpu-gl-device`、`virtio-gpu-pci`、`virtio-gpu-gl-pci`；
- display backend 含 **egl-headless**；
- 宿主有 **libvirglrenderer.so.1.8.8** → 后期 virgl 3D 可行；
- 客机已有 virtio-mmio 驱动范式：`bsd/dev/hakios.virtio_input.c`（32 slots @ 0x0a000000，槽 0x200 字节），virtio **GPU 的 device id = 16**。

## 阶段划分与判据

### 23A. virtio-gpu：传输 + 扫描输出（先不做 3D）

**目标**：客机把一帧写进 virtio-gpu resource，宿主扫描输出；**面板不再走 TCP**。

| 子阶段 | 内容 | 判据 |
|---|---|---|
| A1 | 设备发现 + status 握手 + feature 位（VIRTIO_GPU_F_VIRGL 关）+ controlq 建立 | 串口打出设备 id 16、队列大小、`GET_DISPLAY_INFO` 返回的分辨率 |
| A2 | `RESOURCE_CREATE_2D` + `RESOURCE_ATTACH_BACKING`（guest 页）+ `SET_SCANOUT` + `TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH` | 宿主窗口出现**纯色/渐变测试图案**（先不接真 UI） |
| A3 | 把 panel（750×1624 RGBA）写进 resource，替代 glwire 的像素传输 | 画面完整（dock 通栏）+ 稳态 fps 明显高于 2–3.3 |
| A4（可选） | dirty 区域只传变化矩形 | 静止时 CPU/带宽显著下降 |

### 23B. virgl（3D 交给 V100，后续计划 — 主人 11-14 指定纳入）

- 客机 Mesa 用 virgl 驱动，GL 命令进 virtio-gpu 的 3D 上下文，宿主 V100 执行；
- 判据：客机 GL 场景的 fps 超过 TCG 天花板（>11 fps），且画面完整（不重复 R1 的实验模式陷阱）；
- 代价：XNU 侧要处理 virgl 上下文/资源同步，客机 Mesa 要移植；**几个月**；
- 依赖：23A 的 controlq/资源/扫描输出必须稳定。

### 23D. 异步双缓冲（主人 11-14 指定纳入后续计划）

**为什么（ext39 census 修正）**：每帧 **245,169 us（~245 ms）** 的真实拆解是
flush **617 us**、wait **5 us**、readback **59,658 us（~60 ms）**、attach **299 us**，
剩下 **~184 ms 在 CA/SpringBoard 自己的渲染**（guest CPU/TCG）。
**guest 并不在等 host（wait 5 us）**；23D 的真实收益是「把 60 ms 的 readback 与
184 ms 的渲染重叠」，乐观省 ~60 ms/帧，**不是接近 2×**。

| 子阶段 | 内容 | 判据 |
|---|---|---|
| D1 | 双缓冲：guest 合成第 N+1 帧时，host 还在 replay/上屏第 N 帧 | 帧间隔下降 ≥30%，画面无撕裂 |
| D2 | reply 与下一帧解耦（不阻塞在 `recv` 等 picture；picture 晚一帧到达也可接受） | 注意 wait 仅 5 us：host reply 不是当前瓶颈；判据改为看 readback/render 重叠后**帧间隔**是否下降（ext39 同款 stage 计时） |
| D3 | 流水线深度与 backpressure（避免队列无限增长） | 长跑 1200 s 稳定、无 OOM、无 panic |

- 判据总口径：**默认路径**（完整画面）稳态 fps；分阶段计时里 guest 等待时间下降；
- 风险：撕裂/延迟增加；需要 frame pacing 与丢帧策略。

### 23C. TCG 优化（与 A 并行，不发驱动）

测试矩阵（每项一发，记录稳态 fps、panic）：
1. **SMP=4**（harness 默认值，过去一直被我们显式压成 1）；
2. `QEMU_SMP=4` + `-accel tcg,thread=multi`（MTTCG）；
3. `tb-size` 1536 → 4096；
4. `-cpu max`（若客机兼容）；
5. 组合最优项再跑一次长时间稳定性。

判据：**在默认路径（完整画面）上测**，取 f20 之后 interval 中位数；任何一项把 2–3.3 fps 提到 4.5+ 即为有效。

## 不做 / 边界

- 不动宿主 nvidia 驱动、不碰 vfio（跨架构直通已判不可行）；
- 不在 23A 阶段碰 virgl/3D（避免一次改太多）；
- 不换硬件、不改客机既有 glwire 路径（A3 完成前 glwire 仍是回退）。

## 风险

- XNU 侧 virtio-gpu 驱动从零写，A1–A2 顺利也要若干天；
- virgl 依赖客机 Mesa + 宿主 virglrenderer 版本匹配，风险最高；
- MTTCG 对 aarch64 客机稳定，但 XNU 的多核启动在本项目未见验证，可能 panic。

## 预期收益与顺序（基于 ext39 每帧 census）

### ext39 每帧成本（`hakios-gl: after 1600 frames`）

| 项 | 实测 |
|---|---|
| frame_us | **245,169 us（~245 ms）** |
| handing the commands on（flush） | **617 us** |
| waiting for the picture（wait） | **5 us** |
| copying it out（readback） | **59,658 us（~60 ms）** |
| making textures of IOSurfaces（attach） | **299 us** |
| 剩余（CA/SpringBoard 渲染，TCG） | **~184 ms** |

- **读写/等待几乎为零**；60 ms 是 guest 的 copy-out，184 ms 是 CA/SpringBoard 自身渲染；
- 所以 23D 不是「等 host 的 2×」，而是 **readback 与渲染重叠**。

### 修正后的收益表

| 组合 | 每帧 | 预计 fps | 关键依据 |
|---|---|---|---|
| 现状（默认路径） | **245 ms** | **~3–4** | ext39 census |
| + 23A（virtio-gpu，去 readback） | **~185 ms** | **~5.4** | 省 ~60 ms readback |
| + 23C（TCG 4 核，若 CA 并行减半） | **~150 ms** | **~6.6** | 184 ms 渲染是主项 |
| + 23D（异步双缓冲/重叠） | **~92 ms** | **~8–10** | 乐观省 ~60 ms/帧，不是 2× |
| + 23B（virgl，GL 交 V100） | — | **有限** | CA 工作仍在 guest |
| 24 fps | 需 ≤42 ms | **不承诺** | guest 每帧 ≤42 ms 才可能 |

执行顺序：**23A → 23C（并行）→ 23D → 23B**；23B 的门槛是 23A 稳定、且 23D 的流水线不拖累 virgl 的资源同步。

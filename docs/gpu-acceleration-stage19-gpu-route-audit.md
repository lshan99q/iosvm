# GPU “真路线”审计：瓶颈是 guest↔host 帧同步/4.9 MB 画面往返（只读）

**前提**：不碰 V100 直通；宿主 x86_64、客机 aarch64、无 PCI/virtio-gpu 驱动，直通不成立。
P4 真数据：纯本地 cell1 **10.91 fps**；byref cell2 ~2 frames/20 s；W1.5 cell3 2–6 frames/7–9 s；
W1.5+W3 cell4 5–19 frames/6–20 s。cell4 的 in-place 行写比 cell3 的无 in-place 画面回传快约
5×，但两者都远低于本地。

## 候选同步点（按可能性）

| # | 同步点 | 代码/日志证据 | 一次 boot 证伪 |
|---|---|---|---|
| 1 | **每帧 4.9 MB 画面/面板往返 + 默认 TCP 缓冲**：`eagl_remote_flush()` 同步 `send` 记录、`recv` 回复；无 in-place 时 host `serve_frame()` 回送整张 4,872,000 B picture；byref 时 guest 上传整张 panel | `src/eagl_remote.c:462`（send/recv，499 只有 TCP_NODELAY，无 SO_RCVBUF/SNDBUF）；`src/eagl.c:1339` panel upload；`src/eagl.c:1426` flush；host `serve_frame` 4048–4054 回 `R 0` 或 `R <frame_bytes>`；cell3 0 in-place vs cell4 192 in-place 的 5× 差 | cell3 同一 boot 加 `glframes=1 touch_lat=1`，看 `hakios-remote: f N wait <us>`；若 wait 占帧周期绝大部分且 host `frame ... X ms` 只有几十 ms → payload/往返；`glwire_noflush=1` fps 若跳升 → 网络路径 |
| 2 | **逐帧 lockstep**：guest 发完必须等 host replay 完才收到 reply，host 单线程 `poll`/`serve_frame`，无第二帧在途/双缓冲 | `eagl_remote_flush` 的 `send` 后 `recv`；host `serve_frame` 只在 replay/readback 后 reply（4048）；cell4 已无 picture 回传仍只有 1–2 fps | host 加 `GLWIRE_FRAME_DELAY_MS=1000` 一发：guest 帧周期若 +≈1 s → lockstep；若不变 → 不是等待 host |
| 3 | **guest 本地 CA/显示等待**：`glFinish`/`glFenceSync`/`IOSurfaceLock`、TCG 软件光栅 | frame_end 1304–1319；日志 `MiniIOMobileFramebuffer ... 2 us waiting for the GPU`、`a frame is waited for by the display's driver, not here`；cell1 10.9 fps 说明本地绘制本身不慢 | `glwire_noflush=1`（不发帧）仍 ~0.2 fps → 本地/显示；若跳升 → 网络/同步 |

## 最小可测改动（一处逻辑：socket 缓冲）

在 **guest `src/eagl_remote.c:499`** 的 `TCP_NODELAY` 后，对 `remote_fd` 设
`SO_RCVBUF`（可选同时 `SO_SNDBUF`）为 8 MB；host 侧同一逻辑只需在
`host_glwire.c:4340` 的 accepted fd 设 `SO_SNDBUF`。这是唯一改动，不碰渲染/协议/镜像。
- 选择理由：cell3 的 4.9 MB picture 回传、cell2 的 4.9 MB panel 上传都在默认缓冲下走
  单连接；cell4 取消 picture 回传后快 5×，指向大 payload 的 TCP 窗口。
- 验证（cell3 一发）：
```
XCMD="glframes=1 touch_lat=1"
HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_NO_INPLACE=1"
```
看 guest `MiniIOMobileFramebuffer: N frames in M ms`（目标从 2–6/8 s 升到 ≥10/8 s）、
`hakios-remote: f N wait` 下降；host `frame ... ms` 不变。cell1 不建 socket，不受影响；
P0 三判据的 panel/picture 逻辑不变，只是往返更快。
- 若 socket 缓冲无提升：候选 1 降级，改测 `glwire_noflush=1`（候选 3）和
  `GLWIRE_FRAME_DELAY_MS=1000`（候选 2）；不要同时改协议和缓冲。

## 副作用/边界

- 8 MB 缓冲只增加内核 socket 内存，不改像素；cell1 本地路径没有 `remote_fd`，无影响。
- 若 4.9 MB 回传是 **host `send` 阻塞**，还要 host `SO_SNDBUF`；两处是同一个逻辑改动，
  可分开 A/B 各一发。
- 不能把本审计当成 GPU 加速已修复；V100 直通仍不成立。

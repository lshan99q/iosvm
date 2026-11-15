# stage 23（virtio-gpu）结论：功能成功，FPS 收益为零（2026-11-15）

## 判据与结果

滚动 124.6 s 的同根 A/B（root `351c2170`，wire 已通）：

| | A：virtio_gpu=1（纯 TCP/wire） | B：virtio_gpu=2（+ GPU push） |
|---|---:|---:|
| 滚动帧数 | 224 | 230 |
| **滚动 fps** | **1.80** | **1.85** |
| 静止 fps | 1.30 | 1.56 |
| push 成本 | — | 584 次，**7.3–8.3 ms/次**（copyin 5–6 ms） |

差值 **+2.8%**，在噪声内。结论：**传输不是瓶颈，virtio-gpu 的 FPS 收益为零**。

## 已达成（功能层面，货真价实）

- A1：设备发现、feature 协商、控制队列、`GET_DISPLAY_INFO`（修掉两个真 bug：avail 环提前发布、响应码 0x0101→0x1101）；
- A2：`RESOURCE_CREATE_2D` / `ATTACH_BACKING`(298 页) / `SET_SCANOUT` / `TRANSFER_TO_HOST_2D` / `RESOURCE_FLUSH` 全 `0x1100`；
- A3：面板经 GPU 显存到宿主，`screendump` 能看到真实 UI（Apple logo、Weather 卡片），1001 次 push 零失败。

## 为什么没有 FPS 收益（数据）

ext39 的 guest census：每帧 245 ms 里 **CA 自身渲染 ≈184 ms**、readback 60 ms、flush 0.6 ms、
**wait 5 µs**。也就是说 guest **从不等传输**；把传输从 TCP 换成显存，帧时间几乎不变。

## 定位与后续

- virtio-gpu 保留为**为将来 virgl / 真 GPU 加速铺的路**，不再在 FPS 上投入；
- A4（脏矩形）同理不做——传输只占 2–5%；
- **性能主线转为「减少 guest CA 工作量」**：关模糊/背板/动态效果、减少 layer 数量、异步流水（23D），
  以及 TCG 之外能做的优化。
- 物理前提不变：`24 fps` 在不换硬件的前景下仍不可承诺。

## 环境坑（已固化，后续不会再踩）

1. 内核构建链：见 `docs/kernel-build-recovered.md`（五个坑）；
2. boot 掉 UEFI Shell：`docs/stage23-boot-recipe.md`（vars.fd 重置 + QEMU_ESP=1 + payload 放进 ESP）；
3. wire 空：新 root 必须 `RC=rc.vtop` + `QEMU_NET=1`（否则 guest 无 IP）；
4. 用户态构建：`docs/stage23-userland-build.md`。

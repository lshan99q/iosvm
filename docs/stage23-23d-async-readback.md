# Stage 23D：异步/双缓冲 readback 分析（2026-11-14）

ext39 census（默认路径，SMP=1）：

| 项 | 每帧 |
|---|---:|
| `frame_us` | **245,169 us** |
| CA 绘制 | ~184,000 us |
| **`copying it out`（readback）** | **59,658 us** |
| flush | 617 us |
| **waiting for the picture** | **5 us** |

结论先说：**D2「reply 解耦」能省的只有 wait+flush ≈ 0.6 ms，不是 60 ms。**
59.7 ms 是 `eagl.c` 的 `target_to_surface()` 把 CA 画进 library 自有 FBO 的像素
**`glReadPixels` 到 IOSurface**，发生在 guest 内部，和 host 的 reply 无关。census 里的
`copying it out` 就是它，不是 `eagl_remote_flush` 的 `recv`。

## 为什么 D1 不能简单「放独立线程」

`target_to_surface()` 用 **当前 EAGL/GL context** 读 FBO：

- `glReadPixels` 必须在拥有该 context 的线程调用；guest 的 GL 是 Mesa 软件实现，没有可分享的
  第二 context；
- 把 readback 放线程，要么没有 context，要么与 CA 下一帧对同一个 FBO 的绘制竞争；
- 真正的双缓冲需要 CA 每帧渲染到**两个 FBO 之一**，再由另一个线程读上一帧。CA 的
  `glBindFramebuffer` 由 CoreAnimation 决定，guest 只能在 trampoline 里看，不能安全地强制交替。

因此「readback 与 CA 渲染重叠」不是把 `frame_end` 的一段挪走，而是要把 **CA 的 render target
变成双缓冲**；否则 `panel/picture MATCH in-place` 会被破坏。

## 当前更接近 D1 的路径（但被 Mesa 挡住）

`surface_mirrored()`/`surface_shared()` 本来是「GPU 直接写 IOSurface，不 readback」：
`blit_targets=1`、`share_surfaces=1` 默认开，但 census 明确：

```text
0 IOSurfaces were the textures' memory, 30 had to be copied
```

即 `hakios_gl_surface_texture()` 在这套 Mesa/EGL 上不可用，所有 surface 都退回
`surface_to_texture()`/`target_to_surface()` 的 readback。**先修这个能力**，D1 才有 60ms 可省；
在它之前做 async double buffer 只是把 60ms 挪到另一处，甚至更慢。

## D2/D3 的可做部分与取舍

- **D2**：`eagl_remote_flush` 的 `recv` 可以延后，但 `waiting` 实测 **5 us**，没有收益；
  延后还会让 host 写入 `remote_frame`/surface 与下一帧 CA 竞争。
- **D3**：队列深度 1 只能作为 D2 的兜底，单独实现没有帧率意义。
- 因此本轮**不提交**假的 `glasync=1` 双缓冲；正确顺序是：
  1. 让 panel target 直接以 IOSurface 为 render target（或 `surface_mirrored` 可用），去掉
     `target_to_surface` 的 `glReadPixels`；
  2. 再谈 readback 与 CA 的 overlap。

## 判据（如果后续做）

- `copying it out` 从 ~60 ms 降到接近 0（surface 直接渲染），或
- `frame_us` 从 ~245 ms 降到 ~185 ms，且 `panel/picture MATCH in-place` 不回退；
- dock 通栏 4 图标、Search 居中、P0 内容判据不回退。

本轮未改代码、未 boot。

# Stage 21 性能现状表（收口，2026-11-11）

## 结论链（RLE，rle3–rle13）

| boot | 变化 | 关键结果 |
|---|---|---|
| rle3 | unpack 算法修复 | **unpack 396–451 → 84–105 ms**（达标）；guest 仍 3/10.2 |
| rle4 | display 修复 | 仍 3/7.7；surface2 黑 |
| rle5 | skip empty | skip 3 次、bgra rle 0；仍 4/505 |
| rle6 | texture fallback | fallback 0；仍 3/9.3 |
| rle7 | 201 顺序修复 | fallback 2 次，但 packed **51,202 B (1%)**（黑） |
| rle8 | nonblack 缓存 | `stored nonblack` 0；仍 51 KB |
| rle9 | **565+RLE** | 2-byte 路径正确，**30,554 B (1%)** 黑 |
| rle10 | 移除黑拷贝 | s11 → surface 1（部分 `ff00ff00`）；仍 30 KB |
| rle11 | guest surface2 | 路径生效，segments2 仍黑（1%） |
| rle12 | image1 texture | tracked 1 次、fallback image1 **0**（顺序） |
| rle13 | image1 优先 | **fallback image1 1 次**（选中），但仍 1%（被 glReadPixels 覆盖） |

**判定：RLE 接线未收敛（13 发）。** 解压性能与压缩比本身达标；黑图根因在
host 源缓冲选择/覆盖（image0 黑、fallback 被 readback 覆盖），**RLE 源缓冲
不是 565 发送的那块**。若 code 最后一发（6ea0fa）仍 skip/黑，按用户的规则
**RLE 判死，565-only 收口**。

## 565-only 收口数字（目前唯一自洽的性能结论）

| 场景 | 数字 |
|---|---|
| 全帧 565 | 2.44 MB / 547 ms（中位）→ **4.5 MB/s** → 滚动天花板 **~1.8 fps** |
| 静止/dirty | **4.6–5.0 fps**（dirty 1–2 tiles / 16–32 KB = 0.3–0.7% 面积） |
| 纯本地 cell1（参照） | **10.91 fps** |

- 565 生效、字节减半；瓶颈是 QEMU TCP 4.5 MB/s。
- dirty 只在静止/小变化有效；滚动全屏变化无收益（310/312 tiles、4.84 MB）。
- RLE 若可用，压缩 95x（51 KB）可把 2.44 MB 降到 ~51 KB，但当前接线未收敛。

## 归档

`logs/s20-rle3` … `logs/s20-rle13`、`logs/s20-dirty-idle`、`logs/s20-5653`、
`logs/s20-payload2`、`logs/s20-payload4`；system `8bf5f838` 未覆盖。

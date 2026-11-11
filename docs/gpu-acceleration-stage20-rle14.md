# Stage 20 rle14 — 最后一发：RLE 输入黑断言命中 → RLE 判死

**Verdict: `read-back fallback image1 texture 1` 选中了 image1，但紧接着
`rle input looks black (nonblack=0.00%), skipped` 命中 —— image1 texture
本身 0.00% 非黑，host 源已穷尽。按用户规则：**RLE 判死，565-only 收口**。**

## Numbers

- `tracked image1 texture 1 row 3072` ×1；
- `read-back fallback image1 texture 1` ×1（选中）；
- `rle input looks black (nonblack=0.00%), skipped` ×1（断言命中）；
- frame1 仍有 `rgb565 rle raw 2,436,000 -> 30,554 B (1%)`，之后 skip；
- guest 仅 `upload-rle=1` + 30,554 B unpack 46 ms；host 2 帧；无截图。

## 判断

- host 已把 image1 texture 作为首选源，但 **image1 texture 非黑像素 0.00%**
  → 问题在 guest 侧 present/frame_end 没把 UI 写进 image1 texture，RLE 无法
  从 host 源救。
- 按既定规则：**RLE 接线判死，不再跑**；性能线按 565-only 收口
  （见 `gpu-acceleration-stage21-performance-current.md`）。
- 后续若还要动，只能从 guest present/frame_end 写 surface 入手，不是 RLE/压缩。

## 归档

`logs/s20-rle14/`；系列 rle3–rle14 见 `docs/gpu-acceleration-stage21-performance-current.md`。

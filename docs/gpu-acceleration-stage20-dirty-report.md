# Stage 20 dirty 两发收口 — dirty 的收益场景 = 静止/局部更新

## 结论

- **静止 UI：PASS，第一个真收益。** guest **4.6–5.0 fps**，dirty 仅
  **1–4 tiles / 16–65 KB（0.3–0.7% 面积）**，host **2342 帧**，UI 321,741 B @630 s。
- **滚动：dirty 无效（负结果）。** 滚动时几乎全部 tile 都脏（310/312 tiles、
  4.84 MB ≈ 100%），回落到全帧路径；全帧路径是 **6.6 MB/s 的逐像素循环**
  （4.87 MB / ~740 ms），guest 只有 4 帧/486 s，host 到 frame 113 后停。

## 面积% × fps 表

| 场景 | 上传面积 | 脏区占比 | guest fps | host 帧 | 结果 |
|---|---|---|---|---|---|
| 静止 UI（dirty=1） | 1–4 tiles / 16–65 KB | **0.3–0.7%** | **4.6–5.0** | 2342 | **PASS** |
| 滚动（dirty≈全帧） | 310/312 tiles / 4.84 MB | ~99.5% | 0.008（4/486 s） | **113 后停（idle 664 s）** | 无效 |
| 基线（全帧无 dirty） | 4.87 MB | 100% | 1.2–1.23 | 639 | 参照 |

- 静止时 dirty 把每帧上载从 4.87 MB 降到 16–65 KB（约 1/75–1/300），
  guest 从 1.2 fps 升到 ~5 fps。
- 滚动时整屏内容都变（滚动 + 状态栏/时钟），tile 命中率极低，dirty 无字节收益；
  fallback 版 host 还出现 frame 113 后停（另案，已交 code）。

## 判断

- **dirty 的收益场景 = 静止/局部更新**；全屏滚动不适用，需要另一条路
  （全帧拷贝优化，等 code 的 `GLWIRE_UPLOAD_FAST`）。
- 6.6 MB/s 的逐像素全帧循环是滚动 1.2 fps 的直接原因。

## 归档

`logs/s20-dirty-idle/`、`logs/s20-dirty-scroll2/`；host `58d5b876…` / lib
`35d51b50…`；system `8bf5f838` 未覆盖。

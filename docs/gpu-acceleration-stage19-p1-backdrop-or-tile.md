# P1: 90 高背板/750x200 backdrop 不是标签下虚线的直接来源（只读分析）

**结论：不是。** 750x200 的 CA backdrop 是 program 13 在 **unit 1** 上采样的暗化层
（`texmatD`，P0 日志里在 `0.5,0.5,0.5,0.5` 和 `0,0,0,0` 之间跳），它只影响 fragment 的
亮度/可见度；它不能把点状行写进 **unit 0 的 glyph/label tile alpha**。P1c 已经直接 dump 出
点带在 42 高 tile 底部 6–8 行、90 高圆角背板下缘外；那 8 张 tile 的 alpha 与屏幕上
「Notes」下方 30 px 点带一一对应。因此「当 tile 平铺」即使发生，也是 **调制可见度**，不是
点带的直接来源；直接来源是 90/42 高 glyph/label tile 自身的底部 alpha。

## 证据入口

- P1c HOST_ENV 只开 `GLWIRE_ATLAS_DUMP`（`GLWIRE_NO_PANEL=1`，没有 `GLWIRE_BACKDROP`）；
  8 张 tile 仍有底部点线：101x42 底部 8 行每行 29–30 px；149/152x90 在圆角背板下缘外
  分别有 16/19/23/24/22/24 与 44 个 alpha 像素。
- `host_backdrop_fill()` 只在 `GLWIRE_BACKDROP=1` 且目标纹理 ≤256x256 时把面板 blit 到
  unit 1；P1c/P1d/P1e 都没开它，点带仍在。750x200 本身超过 256，host 也不会填它。
- `texmatD` 的 0.5/0.0 只改 750x200 的采样坐标，不能改 glyph tile 的 texel；P1b 的
  `TC0_SANITIZE` 把点带改成规则竖线，P1d/P1e 的 trim 只改 tile 行，也证明带在 tile 侧。

## 一次 exp 观测（不 boot，由 exp 执行）

同一 P1c/P1e 镜像，只开：

```
XCMD="glwire_local=1"
HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_ATLAS_DUMP=/tmp/s19o-p1f/atlas GLWIRE_LABEL_DUMP=1 GLWIRE_DARKEN_OFF=1"
```

观测：
1. `atlas-149x90/152x90/187x90/203x90` 底部 6–8 行 alpha 是否仍有同一串点（期望：有）；
2. `label state texmatD` 后的 `backdrop texture 11 750x200` 是否存在（期望：存在，但只影响调色）；
3. 截图标签带 `band` 像素是否仍存在；若 darken 关掉后点带不消失、tile 点仍在 →
   750x200 不是直接来源；若 band 消失但 tile 点仍在 → backdrop 只调制可见度，来源仍是 tile。
4. 若 tile 点消失而 band 仍在，才需要回头查 750x200 的采样/平铺。

判据：**tile alpha 的底部点 + darken off 后 tile 仍有点** 是「不是 backdrop」的充分观测；
不要再用 trim 改行（P1d/P1e 已证明不干净）。

## 最小改法（若后续仍要从 host 侧兜底）

不是 UV v 范围的问题：`texmat0`/UV 只决定采样位置，不能删掉已烘进 alpha 的点；把 v 裁到
glyph rect 会拉伸/削掉 90 高圆角背板。正确的最小改动在 **guest glyph/label tile 生成**：
不要把底部点线写进 42/90 高 tile；host 侧仅在确认点带位置后做 **fragment alpha 的底行
discard**（不是清零，不是 trim 行），且必须先证明不会削 descender 和背板圆角。P1 方向仍暂停。

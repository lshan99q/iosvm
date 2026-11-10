# P1 A/B 最小改法（只读代码，不实现/不 boot）

> **2026-11-10 更新：已判 A（tile 内容/行映射），B 已排除。** 本文是 A 系最小改法草案，**未实现**；P1 未修复，修复实验进行中。

问题：178x90 label tile 的 **row61** 有 54 px 虚线，rows62–89 为 0；row61 对应
130x42 glyph 纹理最后一行。exp 正在 dump row58–62 + 截图 crop 判 A（tile 内容）/
B（采样放大）。下面两条都准备，只实现被选中的那条。

## A 系：tile 内容/生成路径

**先说边界**：42/90 高 glyph/label tile 的像素是 **CoreAnimation/UIKit 在 guest 内生成**的，
本仓库没有它的 rasterizer 源码；我们只包了 GL 调用，不能改 Apple 二进制里的 tile 合成。
repo 里能落的最小 A 钩子只有上传/合成边界：

- guest：`userland/src/eagl.c:3704 my_glTexImage2D`（实际 `glTexImage2D` 在 3743）、
  `:3754 my_glTexSubImage2D`（实际调用 3780）、`:3786 my_glCopyTexSubImage2D`（调用 3803）。
  若 exp 日志显示 178x90 是 `texSubImage 130x42 at … y` 合成进去的，最可疑是
  **`y + height` 的边界**：row61 = 源最后一行；最小改法是在 `my_glTexSubImage2D` 里对
  `GL_ALPHA`、1 byte、目标 90 高、源 42 高且 `y+height` 落在 62 的调用，**把源最后一行
  scrub 掉**（或把 `height` 减 1），而不是改 stride/整块。
- host（不改 guest 时最省事）：`tools/glwire/names.h:295 names_glyph_trim` 已支持
  `GLWIRE_GLYPH_TRIM`/`_FROM`。最小增加 **row 选择**（`GLWIRE_GLYPH_TRIM_ROW=61`，
  仅 90 高 GL_ALPHA）：只清零 **row61**，不动 rows62–89/rows0–60。不要再用 trim=6
  （P1d/P1e 已证明会把剩余行拉成竖条）。

**验证**（一发）：
```
XCMD="glwire_local=1"
HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_ATLAS_DUMP=/tmp/p1ab/atlas GLWIRE_TILE_DUMP=/tmp/p1ab/tile GLWIRE_TEXT_DRAW_LINE=1 GLWIRE_GLYPH_TRIM_ROW=61"
```
期望：178x90 dump 的 row61 alpha 从 54 个非零变 **0**，rows62–89 仍 0；截图标签带非零下降；
rows34–52（文字）和 row40 不应变。**副作用**：row61 若是真实字形/圆角底边，会切掉一像素；
看 descender crop 与 plate 圆角；若文字缺笔画 → 不是 A 的可用改法。
判据：tile row61 本身带点（内容）→ A；42 高 tile 最后一行也带点 → A 优先。

## B 系：host 采样放大

host 现在按 guest 的 texture state 重放；`names_texture_upload` 给新纹理设
`GL_CLAMP_TO_EDGE`（`names.h:413–416`），但 guest 后续 `glTexParameteri` 可覆盖成
REPEAT/LINEAR。最小 B 改法在 **draw 前强制 glyph tile 的 wrap**：

- `host_glwire.c:1273 host_draw_binds_glyph` 已识别 42/90 高绑定；
- `host_glwire.c:1326 host_note_draw`，在 1337 `last_draw_was_glyph = ...` 之后、
  `host_viewport_to_target()`/dispatch 之前：对 unit 0 的 bound texture，若高度 42/90，
  保存 `GL_TEXTURE_WRAP_S/T`，设 `GL_CLAMP_TO_EDGE`；`host_after_draw`
  （`host_glwire.c:3022`）恢复。
- 若还差半 texel：在同一处把 `texmat0` 的 v 端 **clamp 到 `1 - 0.5/height`**（或等价
  shader clamp），不要改顶点几何；这一步先不做，只在 wrap 无效时加。

**验证**（一发）：
```
XCMD="glwire_local=1"
HOST_ENV="GLWIRE_NO_PANEL=1 GLWIRE_ATLAS_DUMP=/tmp/p1ab/atlas GLWIRE_TILE_DUMP=/tmp/p1ab/tile GLWIRE_GLYPH_CLAMP=1"
```
期望：`tile-f*-178x90.bgra` **row61 仍为 54 非零**（内容没变），但截图 band 非零下降/
消失；若 tile row61 变 0 则改错了（变成 A/trim）。**副作用**：clamp 可能让边缘重复/变硬；
half-texel 会让字边缘整体偏半像素，检查 >32/>64 计数与 descender 是否变化。

## 判定表

| tile 42 最后一行 | 178x90 row61 | 结论 | 选 |
|---|---|---|---|
| 带点（同 P1c 42 高底部） | 54 px 点 | 源内容复制 | A |
| 干净/0 | 54 px 点 | 采样/repeat/放大产生 | B |
| 带点 | 0 | 点未进 label tile | 另一路径 |

P0-3 归档已知：`tile-f121-t9-178x90.bgra` row61 = 54 个 alpha 非零（max 247），
rows62–89 = 0；rows34–52 是文字。只差 130x42 源 tile 的最后一行即可 A/B 定案。

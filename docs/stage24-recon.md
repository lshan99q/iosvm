# stage 24 侦察：完整面板 vs 宿主回放帧（缺失掩码）

目标：验证「回放缺失内容是静态的」假设 —— 取同一帧的 **完整面板**（guest 上传的 panel）
与 **宿主回放 picture**，逐像素求差。

## 方法

- 同一 root（`root-a3-net.apfs`，351c2170，`RC=rc.vtop` + `QEMU_NET=1` → wire 通）；
- 宿主：`GLWIRE_NO_PANEL=1`（只回放记录，不用上传面板）+ `GLWIRE_NO_INPLACE=1`
  （**关键**：关闭 in-place 写回，否则宿主把自己的 picture 写进 panel 内存，panel 被污染 → diff 恒为 0）；
- `GLWIRE_DUMP=<prefix>` + `GLWIRE_DUMP_AT=840` + `GLWIRE_DUMP_PAIR=1`：
  存同一帧的 `-panel-WxH.rgba`（guest 面板）与 `-picture-WxH.bgra`（宿主回放），以及下一帧的配对；
- 注意 pair 语义：`picture(N) == panel(N+1)`（panel 是上一次 present 的 surface），
  对齐时要用 N 与 N+1 的错位配对。

## 第一发（01:35，错误配置，作废）

`GLWIRE_NO_PANEL=1` 但**没有** `GLWIRE_NO_INPLACE`：in-place 写回发生在 dump 之前，
panel 内存被宿主 picture 覆盖 → `picture(N) vs panel(N+1) diff=0` 是**假象**，
不能作为「无缺失」的证据。日志 `hostframes=1816 MATCH=1667 DIFF=149`。

产物：`logs/stage24/recon/`（r1-*、panel.png/picture.png 均为暗色 UI 帧）。

## 第二发（01:58，修正版）

`GLWIRE_NO_INPLACE=1` 后重跑，结果见下节。

## 第二发结果（02:14，`GLWIRE_NO_INPLACE=1`）

同一帧（N）与下一帧（N+1）的差：

| 配对 | diff px | 占比 | bbox |
|---|---:|---:|---|
| picture(N) vs panel(N+1) | 174,818 | 14.35% | (44,142)–(703,688)，660×547 |
| picture(N+1) vs panel(N+1) | 175,215 | 14.39% | (44,142)–(703,878)，660×737 |

- 掩码跨两帧几乎完全相同：intersection/union = 174,818/175,215 → **Jaccard 0.998**；
- 面积 14.35% < 25%，RGB565 约 0.35 MB；
- **但这一帧是过渡态**：panel 全白（4120 B 纯白 PNG），picture 是 widget 展开中的主屏；
  diff 主要是过渡差异，不是「稳定缺失内容」；
- 运行统计 `hostframes=493 MATCH=37 DIFF=456`（NO_INPLACE 下回放与面板确实不同）。

## 结论（诚实版）

- 方法已打通（panel/picture 同帧 dump + 错位配对 + 稳定性指标）；
- **两次 dump 都落在过渡帧**（第一次 800 s 是暗色 UI，第二次 840 s 是白色过渡），
  所以「缺失掩码」还没在**稳定主屏**上量到；
- 要拿到结论：把 `GLWIRE_DUMP_AT` 推到 UI 静置期（约 first frame + 1000~1100 s），
  或用「同一帧前后两次 screendump 相同」来判定静置，再触发 dump（现在 dump 只能按时间触发）；
- 已知：掩码在同一过渡态下 Jaccard 0.998 → **掩码本身是稳定的**（无论内容是什么）。

## 待办

- 侦察第三发：`GLWIRE_DUMP_AT≈1050`，其余同上（NO_PANEL+NO_INPLACE），拿到静置主屏的掩码；
  若面积 <25% 且稳定 → 静态补丁方案成立。

## static patch 首测（02:33，root 490c8258）

配置：host `GLWIRE_STATIC_PATCH=1 NO_PANEL=1`，guest `glwire_static_patch=1`。

- guest 侧：`hakios-remote: static patch 750x1624 4872000 B hash 668796ddfa0b5bef sent`（第 2 帧后发出）；
- host 侧：**没有** `host_glwire: static patch ...` 行；只处理了 `frame 1`（`in_place=1`），之后无帧；
- guest 串口之后没有新的 `end of frame`（但系统日志继续，进程没崩）→ **疑似 guest 写 4.8 MB 补丁时阻塞**
  （host 没读走），帧流水停住；
- 该发还因 `NOUI_BY=780` 被 harness 提前收（`end=noui`），后续这类长跑要 `NOUI_BY=1400`。

→ 需要 code 查 host 的 `S` 记录解析/读取顺序（补丁是否在首个 R 之前被读走），以及 guest 的发送是否阻塞。
证据：`logs/stage24/patch/`。

## 侦察第三发（AT=1050，NO_INPLACE）：panel dump 仍不可靠

- `picture(N) vs panel(N+1)` diff=0，`picture(N+1) vs panel(N+1)` diff=78,990 px (6.49%)；
- 但 panel PNG 只有 4.8 KB（近似纯色面）——**NO_INPLACE 下 panel 内存不可信**
  （guest 没上传或已被回包覆盖），不能再用它当「完整面板」；
- 结论：**「完整面板」应该用 QMP screendump 取**（读 guest 真实 framebuffer），
  回放帧用 GLWIRE_DUMP 的 picture；两者相隔约 1 帧，抖动需用「静置期间连续两张 screendump 相同」来确认。
- 运行统计 `hostframes=543 MATCH=63 DIFF=480`。

## static patch 修复版首测（03:23，root 490c8258，host df228f8）

`GLWIRE_STATIC_PATCH=1 NO_PANEL=1` + guest `glwire_static_patch=1`，NOUI_BY=1400：

- **握手成功**：
  - guest：`hakios-remote: static patch 750x1624 4872000 B hash 668796ddfa0b5bef sent`
  - host：`host_glwire: static patch 750x1624 4872000 B hash 668796ddfa0b5bef at 0,0`
- **完整性 ✅**：screendump 是完整主屏（两 widget、全部图标、Clock、Search、dock 4 图标）；
- **fps ❌：60 s 窗口 60 帧 = 1.00 fps**（目标 ≥10）；
- 原因：**补丁每帧都重发** —— 整屏 hash 覆盖了动画内容，任何像素变化都触发 4.8 MB 重传；
  实测 62 次重发 / 约 75 帧，4.8 MB × 1/s 直接把 fps 压到 1.0。

### 结论 / 下一步

- 全屏补丁**方向正确但粒度错误**：应只补「回放缺失的静态区域」（掩码），
  且 hash 只覆盖掩码 bbox 内的像素；否则任何动画都会触发整屏重传；
- 侦察的任务因此变成「把掩码定到静置主屏上」：用 QMP screendump 作完整面板、
  GLWIRE_DUMP picture 作回放帧，在静置期（连续两张 screendump 相同）配对求差。
- 目标：bbox 面积 <25%、跨帧稳定 → 之后只补这块（565 约 0.35 MB，首帧一次）。

证据：`logs/stage24/patch2/`（含 `home-complete.png`）。

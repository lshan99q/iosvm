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

## bbox 版 static patch 测试（03:44，root 4901aa1d，host 4a186bd）

`GLWIRE_STATIC_PATCH=1 NO_PANEL=1` + guest `glwire_static_patch=1`：

- 握手：`static patch 660x547 1444080 B hash 84bced41... at 44,142`（两侧一致）✅
- 完整性：screendump 完整主屏（两 widget、全部图标、Search、dock 4 图标）✅
- **重发大幅下降**：全程 48 次 / 1120 帧（全屏版是 62 次 / 75 帧），60 s 窗口内仅 1 次 ✅
- **fps**：1120 帧的间隔分布 p25=0.20s(5.0)、median=0.24s(4.2)、p75=0.33s(3.0)；
  60 s 窗口 158 帧 = 2.63（该窗口含大段 idle）；瞬时最好 0.09s = **10.6 fps**（burst）

| 配置 | 中位 fps | 重发 | 完整 |
|---|---:|---|---|
| 无补丁（产品路径） | 3.45–4.76 | — | ✅ |
| 全屏补丁 | **1.0** | 62/75 帧 | ✅ |
| bbox 补丁 | **4.2**（p25 5.0，burst 10.6） | 48/1120 帧 | ✅ |

→ bbox 补丁把「重发风暴」解决了，fps 回到基线（4 左右），但**没到 10.9**。

## 结论：10.9 fps 的前提

现有 root 带 `/etc/glwire_local` → **guest 仍在本地合成 + 每帧上传 panel**（~245 ms/帧），
host 回放再快也被这一步拖住。计划的 10.91 fps 来自 **guest 不合成、只显示宿主回放** 的路径
（remote-only），即：`/bin/glremote_on` 有、`/etc/glwire_local` **没有** + netcfg 通 + host NO_PANEL + 补丁。

所以下一步要 code 出 **remote-only + netcfg** 的 root（去掉 /etc/glwire_local），
再验「回放 + bbox 补丁」是否既 10.9 fps 又完整。
证据：`logs/stage24/bbox/`（home-complete.png 为完整主屏）。

## remote-only 测试（04:36，root 604502da，host GLWIRE_STATIC_PATCH=1 NO_PANEL=1）

**结论：remote-only 路径跑通了，但 fps ~7（不是 10.9），且完整性问题没解决（屏幕被 A2 测试图案占住）。**

两发（`virtio_gpu=2` / `=1`）共同结果：

| 项 | 结果 |
|---|---|
| 录制模式 | `the frame is drawn by the host alone`（remote-only，0 个本地 GL 帧）✅ |
| 网络 | `netcfg: en0 10.0.2.15` ✅ |
| 补丁 | `static patch 660x547 1444080 B hash 30c7aa57... at 44,142`，**全程只发 1 次** ✅ |
| 帧流 | 5796 / 1962 帧，**没有 21eea 的『SpringBoard 不出帧』** ✅ |
| fps | `virtio_gpu=2`：60 s 窗口 6.95 fps；`virtio_gpu=1`：间隔中位 0.140 s = **7.14 fps**（p25 0.080 s = 12.5）|
| host 每帧成本 | total ≈ **39–42 ms**（panel=34.6、rec 0.4–2.8、replay 0.3、read 0.6、seg 0.9–4.7）→ 宿主侧可到 ~25 fps |

- **瓶颈在 guest 的帧节拍**（~7 fps），不是宿主回放（40 ms/帧）；
- 显示验证受阻：`virtio_gpu=2` 时 A3 把空 panel 推到 GPU scanout（screendump 全黑）；
  `virtio_gpu=1` 时 scanout 停在 A2 测试图案；QMP `screendump device=ramfb` 报 DeviceNotFound。

### 下一步建议（给 code）

1. **完整性验证**：关掉/跳过 A2 测试图案（或让 ramfb 成为第一个 console），否则 screendump 看不到
   guest 真正显示的画面；或者直接用 `GLWIRE_DUMP` 存宿主 picture（它就是 in-place 写回的内容）看完整性。
2. **fps**：guest 节拍 ~7 fps 是新的上限；宿主回放 40 ms 已不是瓶颈 —— 要查 guest 每帧在等什么
   （host reply ~35 ms + CA commit？）。10.9 fps ≈ 92 ms/帧，当前 ~140 ms/帧。

证据：`logs/stage24/remote-a3/`（virtio_gpu=2）、`logs/stage24/remote1/`（virtio_gpu=1）。

## D2 async reply 首测（05:02，root ff3219ca，host main 130f4a6）

配置：host `GLWIRE_STATIC_PATCH=1 NO_PANEL=1 GLWIRE_ASYNC_REPLY=1`，guest
`glwire_static_patch=1 glasync=1 glframes=1`，remote-only。

结果：**回归，比同步版慢得多**

| 指标 | 同步 remote-only（604502da） | D2 async（ff3219ca） |
|---|---:|---:|
| host 每帧 total | **39–42 ms** | **713–1029 ms** |
| 其中 rec | 0.4–2.8 ms | **440–750 ms** |
| 其中 replay | 0.3 ms | 218–237 ms |
| 其中 panel | 34.6 ms | 36–53 ms |
| image refs | 0 | **79** |
| fps | 6.95–7.14 | **1.19** |
| patch 重发 | 1 | 1 |

- `sent=2` 未出现（async 分支没走到）；`flush -> 2` 正常；
- **census 仍然 0 行**（`glframes=1` 也没打 `hakios-gl: frame ...`），
  所以 140 ms 的拆分这次没拿到；
- 回归点看起来是 **image refs**（0 → 79）：host 每帧要多取 79 个引用，rec 直接涨到 440–750 ms。

### 待 code 查

1. 为什么新 root（ff3219ca）在 remote-only 下每帧带 79 个 image refs（老 root 是 0）？
   这是本轮 fps 从 7 掉到 1.19 的直接原因；
2. `glasync=1` 为什么没有让 `sent=2` 出现（分支条件？）；
3. `glframes=1` 为什么在 remote-only 下仍不打 census 行；
4. 140 ms 拆分：仅有 host 侧 39–42 ms（同步版）+ guest 端 140 ms，差值 ~100 ms 未分解。

## async2 的 refs 隔离（05:56，root 54cd0709）

同一 root 两发（remote-only + `glasync=1`）：

| 配置 | refs | host rec | host replay | host total | fps |
|---|---|---:|---:|---:|---:|
| A：`glwire_static_patch=1`（host 带 STATIC_PATCH） | **79** | 183–327 ms | 235 ms | 465–605 ms | 1.7–1.9 |
| B：`glwire_static_patch=0`（host 不带） | **79** | 172–327 ms | 234–235 ms | 452–606 ms | 1.2–1.2 |

→ **refs=79 与 static patch 无关**：B 关掉补丁后 refs 仍是 79。
对照老 root（604502da，同步版）refs=0、host 39–42 ms、fps ~7。

## census 仍然不打（async2 + 强制 frame_lines 也不行）

- 两发 A/B 都是 `hakios-gl: frame ...` **0 行**（只有 Mesa/上下文初始化行）；
- 所以 140 ms 的拆分到这里仍拿不到。

### 给 code 的结论

1. **regression 在 async2 这份 eagl 构建里**（与 static patch 无关）：每帧带 79 个 image refs，
   host rec 从 0.4–2.8 ms 涨到 172–327 ms，fps 从 7 掉到 1.2–1.9；
2. census（`frame_lines`）在 remote-only 下仍不打印，需要继续查（是否 `frame_end` 的 census 分支在
   remote-only 早退）；
3. 下一步建议：先把 refs=79 修回 0（这是当前最大的开销），再谈 async 的 waiting 拆分与 D2 收益。

## sync-census 验证（06:24，root d04f080a）

同步 + 强制 census（`glwire_static_patch=1`，host STATIC_PATCH+NO_PANEL，无 glasync）：

| 项 | 结果 |
|---|---|
| refs | **0** ✅（回归修复） |
| host 每帧 total | 43.2 ms（rec 2.7、replay 0.5、read 0.7、panel 36.0） |
| fps | 60 s 窗口 **435 帧 = 7.25**；间隔分布 p10=0.080(12.5)、p25=0.080(12.5)、median=0.150(6.7)、p75=0.160(6.25) |
| patch 重发 | 1 |
| **census** | **仍然 0 行** ❌（全串口没有 `waiting for the picture` / `handing on` / `hakios-gl: frame N`） |

→ refs 修好了、fps 回到 7.25（且 p25 已在 12.5 fps），但 **eagl 的 census 在 remote-only 下依旧不打印**，
140 ms 的拆分仍然拿不到数据。code 需要确认 `frame_end()` 里 census 的强制点是否真的走到（可能整段在
`eagl_remote_no_local()` 分支外/被 early-return 挡掉）。

### 下一步

1. code 修 census（remote-only 下每帧一行 `hakios-gl: frame N ... waiting ...`）；
2. 有了 census 再决定：wait≈35ms → 双缓冲 async 值得做；CA 100ms+ → 先减 CA。

## census3（06:52，root 8b8a7477）：140 ms 终于拆开了

同步 remote-only + patch，census 每 20 帧一行（frames ≥200，n=63）：

| 分量 | median | p25 | p75 |
|---|---:|---:|---:|
| **frame_us** | **153,275 µs** | 83,749 | 168,589 |
| waiting（等宿主 reply） | **2 µs** | 1 | 2 |
| copying out（面板 4.8 MB 拷贝/上传） | **71,419 µs** | 68,815 | 74,246 |
| handing on | 29 µs | 17 | 34 |
| 其它（CA/commit） | **81,184 µs** | 8,646 | 93,037 |

### 结论（与假设相反，但很有用）

1. **waiting ≈ 0**，不是 35 ms —— 同步版根本没有在等宿主 reply；
   → **D2 异步 reply 没有收益，别做了**（之前 79 refs 的 db 也是 async 引入的）；
2. 153 ms = **copying out 71 ms（47%）+ CA/其它 81 ms（53%）**；
3. **只要干掉那 71 ms 的 panel 拷贝/上传，frame_us ≈ 82 ms ≈ 12 fps** —— 直接达标（p25 已经是 84 ms）；
4. remote-only 下宿主用 `NO_PANEL` 根本不用这块 panel，这个上传是**纯浪费**：
   在 remote-only 直接跳过 panel 上传（guest 侧一个条件）即可。

### 下一步（给 code）

- `eagl.c`：`remote_only` 时不 upload 面板（跳过那 4.8 MB 拷贝）→ 预期 12 fps；
- 之后若还要再快，再看 81 ms 的 CA/其它（p25 已到 8.6 ms，说明 CA 本身波动很大）。

## nopanel 版（07:26，root b5d5b672）：copying out **没有**变化

code 的改动：remote-only 下只置 `composition_off=1`，不再 `upload_changed_layers()`，两处
`target_to_surface()` 加 `!eagl_remote_no_local()`。

实测（frames≥200，n=95）：

| 分量 | census3（8b8a7477） | nopanel（b5d5b672） |
|---|---:|---:|
| frame_us | 153,275 µs | **147,358 µs** |
| **copying out** | 71,419 µs | **70,310 µs**（几乎没动） |
| waiting | 2 µs | 2 µs |
| handing on | 29 µs | 28 µs |
| fps | ~7.25 | ~6.2 |

- 两个 root 的 `libhakios.gl.dylib` 确实不同（93419a1a vs 3135617c），所以**不是没重建**；
- 但 `copying out` 没变 → **这 71 ms 不是 `upload_changed_layers()`/`target_to_surface()` 的开销**，
  census 里的「copying out」计时应该包在别的地方（需要 code 确认计时点到底在哪个函数）；
- 另外：这次 `GLWIRE_DUMP_AT=1050` 的宿主 picture 是 **白块 + 杂色**（`logs/stage24/nopanel/dump-frame.png`），
  不像静置主屏；怀疑「跳过 panel 上传」后 **bbox 补丁的数据源（panel 表面）不再更新 → 变成陈旧/白色**，
  也可能只是过渡帧。两个假设需要分开验证。

### 给 code

1. census 的 `copying out` 计时点在哪？（改了 upload/target_to_surface 都没让 71ms 变化）
2. 跳过 panel 上传后，static patch 的数据源是否也失效了？（patch 是从 panel 表面取的？）
   若是，应该只跳过「读回发给宿主」，不要跳过表面的更新。

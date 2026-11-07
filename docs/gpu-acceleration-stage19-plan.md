<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十九阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十九阶段工程文档

> 建立时间：2026-11-07 00:20
> 前置：第十八阶段（S0 稳定配方 `QEMU_SMP=1`；P0 影子把标签层送上屏、`texmat0` 从顶点推导成功、
> UV 数据在 UI 阶段损坏；见 `IOSVM-GPU加速第十八阶段工程.md`）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **启动已经可测**：`QEMU_SMP=1` 连跑 5 发 0 panic（最长 7248 帧、20 分钟），界面都能到。宿主侧 Windows 虚拟机
   限到 2 核、保持运行。
2. **标签层已经能上屏**：影子取「本帧第一笔标签合成」后，每个 App 图标下方出现标签层的红条（位置正确）。
3. **`texmat0` 推导成功**：从 draw 顶点推出 `0.3750 0.0000 0.1250 0.1250`（图集子矩形），红条消失、状态栏恢复、
   主屏多出三个图标；**但标签仍不是文字**。
4. **根因方向已经明确**：UI 阶段的 texcoord 属性数组在线上就是坏的（`0/4.7969`、`0/nan`、`6.1e28/0`，
   像半浮点被当浮点抄），同一块数组在启动早期是好的（`0.125..0.5`）。wire 的顶点数组按**客户端指针**直读，
   而 CA 在 UI 阶段很可能把数组放进 **VBO**（`glVertexAttribPointer` 的 pointer 是偏移）。
5. 壁纸仍是「启动镜像作业表缺 PosterBoard」；widget 两个面仍不由 GL 链路画；四格表未刷新。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 顶点数组的修法：**给 wire 加 VBO 影子**（拦 `glBindBuffer`/`glBufferData`/`glBufferSubData`）vs **按指针快照**（在 `glVertexAttribPointer` 时就把数组抄下来） | 影子 VBO：改动大（客人机 + spec 生成器）；快照：数组可能在 pointer 之后才填，快照会抄到旧内容 | 文字标签 |
| D2 | 标签不可见的第二半：**修 backdrop**（darkenSover 的 D 用真实面板）vs **先绕开 darken**（临时关掉 program 13 的 darken 项看文字） | 先绕开：能快速判定「文字本身画对了没有」；修 backdrop：要理解 CA 的 backdrop 生命周期 | 可读的 App 名称 |
| D3 | 壁纸：**重建 system.apfs 补 `LaunchAngels→/redwood/LaunchAngels`** vs 不做 | 重建：一次 10–20 分钟 + 启动时间变长；不做：主屏永远黑底 | 壁纸 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0 | 标签是文字 | 连续 3 发（`QEMU_SMP=1`）里 ≥2 发首页能读出 ≥5 个 App 名称；截图 ≥300 KB；同帧 `diffdump --tolerance 8` ≥95% | 可用性 |
| P1 | 文字质量 | 无重影；widget 左上角 40x40 白像素 875/1600（±2%）；标签为白 | 可用性 |
| P2 | 壁纸 | SpringBoard 的 `posterboardservices` lookup 不再 `No such process`；宿主出现 1125x2436（或全屏）壁纸纹理；主屏背景非纯黑 | 可用性 |
| P3 | widget | `312x202`/`184x124` 以纹理进宿主；否则交付「拉面」的最小实现（时间盒 2 天） | 可用性 |
| P4 | 收尾 | 四格表（纯本地/按引用/W1.5/W1.5+W3）各一次 20 s 滚动窗口；宿主侧 `0x502` = 0；`make test`；文档与推送 | — |

---

## 三、实验计划（按顺序，每步停下来写结论）

### P0：顶点数组的语义（1–2 天）

**第一发（s19a，00:24–00:44）已经把 VBO 假设否掉**：客人机在每次 `glVertexAttribPointer` 时记下
`GL_ARRAY_BUFFER_BINDING`，日志里 **buffer 全是 0**，指针是稳定的客户端地址（四个属性同一次 draw 都在
`0x16f41c870` 起的 48 字节步长里：attrib0 base+0、attrib2 base+16、attrib3 base+24、attrib1 base+32），
**没有 VBO**。所以坏的不是「偏移当地址」，而是**这一块客户端内存在 draw 时刻的内容**，或者 CA 对这几笔
label draw 本来就填了非 texcoord 的数据。

1. **抓 UI 时刻（做过一发，翻车）**：改成每 500 笔 draw 打一行 attrib 3 的原始 8 字节后（s19b/s19c），
   s19d 那一发里客人机的 **backboardd 直接重启**（宿主看到 `client 22` 新连接、客人机 `pid=40`，界面没起来）——
   原因八成是诊断代码 `memcpy(raw, pointer, 8)` 读了一个**偏移量当地址**。教训：客人机诊断不许直接解引用指针，
   先判 `buffer == 0` 且地址在用户态范围（例如 > 0x100000000）再读；镜像已回滚到 s19a 那一版。
2. **backdrop 的来源已经查清（s19c，实测）**：label draw 在 unit 1 上绑的是 **texture 11，750x200**（不是我们填的
   64x19），`texmatD` 在两次 draw 之间在 `(0.5,0.5,0.5,0.5)` 与 `(0,0,0,0)` 之间跳；`(0,0,0,0)` 意味着整块采样同一个
   纹素。也就是说暗化项用的是 **CA 自己的 750x200 图层**，而我们填的 64x19 backdrop 根本不在这一步用。
3. **darken 关掉的实验（s19e，已做）**：宿主把 label 片元着色器的暗化调用改写成恒等（`GLWIRE_DARKEN_OFF=1`）。
   结果：**tile 变成纯黑**（`grey 0..0`、alpha 恒为 4/255、rgb 全 0），截图上的红条也消失了。读法：
   - **字形采样没有变化**：alpha 在整块 tile 上恒等于 4/255 → 图集采样是一个常数，不是字形覆盖率；
   - 顶点颜色在两次 draw 之间在 `(0,0,0,1)` 与 `(1,1,1,1)` 之间跳（`attrib 1` 的 half-float 数据本身是对的）；
   - 也就是说，红条 = **暗化项把「背板」（750x200 的图层）铺满了标签位置**，而文字本身从来没被采样出来。
   结论：**问题不在暗化，而在「纹理坐标 → 图集」这一步**（texmat 退化：推导出的 y 跨度是 0，说明 UI 时刻 attrib 3
   的三个角 v 相同；而同一数组在启动早期是 `0.125/0.5` 的正常子矩形）。
2. **修法（二选一，按 D1）**：
   - **VBO 影子**：拦 `glBindBuffer`/`glBufferData`/`glBufferSubData`（spec 加 3 个 opcode），客人机保留影子副本，
     draw 时按 `binding + offset` 取；
   - **快照**：`glVertexAttribPointer` 时不知道数量，但可以在**第一笔 draw** 时把「从 pointer 起 `elements*stride` 字节」
     抄一份留到后续 draw（同一 pointer 复用时不重抄）。
3. **判据**：宿主日志里 attrib 3 在 UI 阶段的值回到 0.125..0.5；标签出现文字。

### P1：backdrop 与文字可见性（1 天）

1. **绕开 darken**：给宿主加一个 `GLWIRE_PROGRAM_SKIP=<program>`（跳过该 program 的 draw）或
   `GLWIRE_DARKEN_OFF=1`（把 `darkenSover` 的 `S *= y^8` 去掉），判定「文字本身画对了没有」。
2. **修 backdrop**：program 13 的 `texture1` 用的是「面板缩略图」；试真实面板（750x1624 原尺寸，`texture2DProj`
   的坐标本来就是屏幕投影），或让客人机上传真正的 backdrop 面。
3. **判据**：截图里能读出 App 名称；`GLWIRE_LEGACY_FORMATS` 默认（原生）下圆角 875/1600。

### P2：壁纸（1 天）

1. `build.sh` 里 `rm -rf sysroot/System/Library/LaunchAngels` 之后加
   `ln -s /redwood/LaunchAngels sysroot/System/Library/LaunchAngels`，并在 root.apfs 里建 `/redwood/LaunchAngels`，
   拷 `com.apple.PosterBoard.plist`。
2. **判据**：lookup 不再失败；宿主出现壁纸尺寸纹理；主屏背景非纯黑。失败也要给出「poster 进程崩在哪」。

### P3：widget（时间盒 2 天）

1. `inject_ciq_probe.c` 的钩子拿 `{ID,w,h,stride,format,屏幕矩形}` → XPC/共享内存 → backboardd → 宿主按 surface ID
   拉取（照 opcode 201 的段表，`remote_vtop()`）。
2. 时间盒到期只允许三种结论：可行 / 不可行 / 需要更多信息。

### P4：四格表与收尾（1 天）

四份构建各一次 20 s 滚动窗口（同 `QEMU_SMP=1`、同触摸脚本）；`0x502` 按「宿主侧 0」记账；`make test`；文档；推送。

---

## 四、不做（已被数据否掉，别重试）

1. **不再用 4 vCPU 做功能实验**：MTTCG 的 ticket-spinlock panic 是环境性的，`QEMU_SMP=1` 才稳。
2. **不再猜 `texmat0` 的值**：它已经从顶点推出来了（0.375/0/0.125/0.125）；问题在数组数据的来源。
3. **不再把「tile 有像素」当文字**：backdrop 填充会让 64x19 tile 全非零。
4. **不再说「合完 label 整个面板重建」**：逐笔哈希已推翻（第十七阶段 §十一）。
5. **不动内核**：本阶段是客人机用户态 + 宿主。

---

## 五、验证环境与操作规范

- 标准配方：**`QEMU_SMP=1`**（0 panic），每发 1100–1500 s；界面在 10–15 分钟到。
- 启动：`QEMU_SMP=1 HOST_ENV="..." tools/glwire/harness/boot-measure.sh <label> <root.apfs> <secs>`。
- 用户态重建：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`
  （改 `userland/src/eagl_remote.c`/`remote.inc` 都要重建；重建前把 `userland/root.apfs` 备份到 /tmp）。
- 宿主探针：`GLWIRE_BACKDROP`、`GLWIRE_READ_PANEL`（默认开）、`GLWIRE_PANEL_SHADOW` / `GLWIRE_SHADOW_APPLY` /
  `GLWIRE_SHADOW_MODE`、`GLWIRE_TEXMAT_FROM_ATTRIB`（从顶点推导）、`GLWIRE_TEXMAT_IDENTITY`、
  `GLWIRE_FIX_TEXMAT`（诊断）、`GLWIRE_PROGRAM_ALL`、`GLWIRE_DRAW_LOG`、`GLWIRE_LABEL_DUMP`。
- 证据图放 `iosvm/docs/demo/`，每张配 `NOTES.md` 的一条。

---

## 六、里程碑

| 天 | 目标 | 产出 |
|---|---|---|
| D1 | P0 诊断 + VBO 影子/快照 | 宿主日志 attrib 3 回到 0.125..0.5 |
| D2 | P1 绕开 darken → 修 backdrop | 能读出 App 名称的截图 |
| D3 | 三发验证 | ≥2/3 发标签为文字、截图 ≥300 KB、diff ≥95% |
| D4 | P2 壁纸 | lookup 不失败 / 壁纸纹理进宿主 |
| D5–D6 | P3 widget（时间盒） | 可行/不可行结论 |
| D7 | P4 四格表 + 收尾 | 四个数字 + `make test` + 推送 |

---

## 七、诚实的预期

- **P0 的证据已经很强**：坏的是 UI 阶段的那块数组，而 wire 按客户端指针直读——VBO 影子是标准解法，判据也清楚。
  但客人机改动要重建镜像，一次 10–20 分钟，别在一个 boot 里反复改。
- **P1 的「绕开 darken」大概率立刻见文字**（红条已经证明标签层在画），风险是文字颜色/alpha 与背景相同。
- **P2 要重建 system.apfs**，可能顺带改变启动时间与内存布局，四格表的口径要记清楚。
- **P3 跨进程**，时间盒到期就收结论，不硬做。

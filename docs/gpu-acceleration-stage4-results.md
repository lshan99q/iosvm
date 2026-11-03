# The fourth stage: the recorder's crash is gone, the host's replay draws the screen

> 2026-11-02/03. 需求与判据见 `gpu-acceleration-stage4-requirements.md`；定位过程的逐步记录见
> `gpu-acceleration-plan.md` §0.7–0.12 与 `gpu-acceleration-null-pixel-crash.md`。
> 本文只写这一阶段的结果：每条判据量到了什么，哪条没做到，为什么。

## 1. R1 — 「记录器装了就崩」：已修，5/5 冷启动通过

根因是一个合法的 GL 调用把记录器带进了空指针：`glTexImage2D(..., pixels = NULL)` 是
「只分配存储」，`wire_image()` 却按 `width × height × 4` 算出 4.87 MB 再 `memcpy` 从 NULL 读。
`wire_image()` 与 `wire_bytes()` 现在只在 `pixels != NULL` 时才带字节（`patches/0009`… 见
`patches/0010`、`0011` 与 `9c70123`）。

判据：冷启动 5 次，≥4 次 ≥200 帧且截图 > 300 KB，0 SIGSEGV/panic，宿主 200 帧连续 `MATCH`。

| 冷启动 | 宿主帧 | MATCH | SIGSEGV | panic | 截图 | 驱动稳态 |
|---|---|---|---|---|---|---|
| `/tmp/ab.log` | 226 | 226 | 0 | 0 | 448,568 B | 16f/7.4 s |
| `/tmp/r1t1.log` | 265 | 265 | 0 | 0 | 449,021 B | 21–22f/5.0 s |
| `/tmp/r1t2.log` | 262 | 262 | 0 | 0 | 448,197 B | 21–22f/5.0 s |
| `/tmp/r1t3.log` | 283 | 283 | 0 | 0 | 448,537 B | 25–26f/5.0 s |
| `/tmp/r1t4.log` | 233 | 233 | 0 | 0 | 448,273 B | 28–30f/5.0 s |

**5/5 通过**（判据要 4/5）。镜像 `/tmp/root-fixed-net.apfs`（`GL=1 GLREMOTE=1 GLWIRE_LOCAL=1
CONFIGD=1 RC=rc.vtop`），宿主 `host_glwire 9110 qemu/ram.img`，750×1624 `fbscale=2 metal=1`。

一次启动的判据本身被写进了启动器（`/tmp/xbrep.sh`）：结束条件就是「宿主 ≥200 帧且截图 > 300 KB」，
另加「780 s 还没有 100 KB 的截图 = 没界面，直接收工」，因为 40% 的启动会卡在
`lsd denied attempt to escape CPU monitor` 上。

## 2. R2 — 宿主只靠录制的调用流画界面：画得出来，差一帧相位

### 2.1 黑屏不是一个原因，是两个

`--no-panel` 第一次跑（`/tmp/np.log`）的诊断行：

```
host_glwire: replay 16837 records B, opcodes 1:3 2:2 6:1 12:3 22:1 26:2 29:8 34:3 35:3 38:2 49:4 52:1 200:12, glerror 0x0
host_glwire: read-back framebuffer 0 (attachment none), viewport 0,0 0x0, last drawn into 2
```

1. **读错了 framebuffer**：CoreAnimation 帧末解绑回 0，而这边的 0 是 16×16 的 EGL pbuffer；
   真正画进去的是 2（每次 draw 之前记下绑定，`dispatch.inc`）。现在读回前绑到
   `drawn_framebuffer`。
2. **面板被属性释放顺手 free 掉**：`names_attribs_release()` 里原本也 `free(g->panel)`，而重放路径
   每次 draw 都调它 —— `--no-panel` 下帧末永远没有面板可比，验收哈希静默为空（实测 147 帧里
   146 帧没有 `panel` 字段，唯一有那一帧恰好没有 draw）。面板不是 attribute，那一行已删。
3. 另外把读回的图上下翻转：客户机的面板是「第一行是顶行」（`target_to_surface()` 的
   `image->invert` 逐行翻着写），`glReadPixels` 给的是「第一行是底行」。

修完这三条，`--no-panel` 的第一张整帧存图就是桌面：Dock、图标、搜索框、`rdar:45025538` 横幅，
11064 种颜色、38.1% 非黑像素（`/tmp/np/f-picture-750x1624.bgra`）。

### 2.2 一帧的相位差（R2 判据还差这一步）

```
frame 139  panel 2934e9625c431559 picture 2a7287ed259f25bc DIFF
frame 140  panel 2a7287ed259f25bc picture c7f3c4108e0f5bc0 DIFF
frame 141  panel c7f3c4108e0f5bc0 picture 416343ad1731dde1 DIFF
```

`picture(N) == panel(N+1)`：宿主重放出来的画面，正好是客户机下一帧才上传的面板。
不是画错——是客户机 `frame_end()` 里「合成 → 上传面板」的时机让面板比这一帧的记录旧一帧
（一帧 = 100–200 ms）。因此宿主现在同时打两个判据：同帧 `panel … picture MATCH/DIFF` 与
`previous picture … MATCH/DIFF`（上一帧的画面 vs 这一帧的面板）。

`glwire_nopicture=1` + `GLWIRE_NO_INPLACE=1`（宿主不写回、客户机也不用宿主的画面）那次对照
（`/tmp/np4.log`）赶上启动卡死，所以「相位差是客户机的 present 时机还是宿主写回造成的」还没有定论。
**这是 R2 判据唯一没跨过去的坎。**

## 3. R3 — W1+W2 端到端：不成立，卡在「纹理不再上传」

镜像去掉 `GLWIRE_LOCAL`（`remote_only=1`，客户机不执行重调用、也不本地合成），
`/tmp/r3.log`（09:00 起机）：

- **帧率够了**：宿主 2,536 帧，间隔 0.09–0.10 s（**10–11 fps**），客户机驱动自报 4–9 fps；
- **记录够小**：2,260–18,796 B（< 100 KB）、`in-place rows` 在；
- **屏幕是黑的**（截图 8,061–14,324 B），0 次 SIGSEGV、0 次 panic。

原因在日志里很直接：这一配置下**一帧的记录里没有任何纹理上传**
（`opcodes 1:2 5:1 6:1 12:2 …`，`0 image refs`），宿主拿到的只是「往不存在的纹理上画」。
纹理是跟着客户机**本地合成那条路**上传的（`frame_end()` 的 `surface_to_texture` 与面板上传），
W2 的 `composition_off` 把它一起关掉了 —— 这正是优化文档 §4.2 预言过的
「W1 与 W2 不能同时开」，现在有了数字：**黑屏 + 11 fps + 0 纹理**。

下一步（未做）：`remote_only` 时保留纹理上传、只跳过本地的 draw/合成。那是 eagl.c 里的一次
结构性改动，不是开关。

## 4. R4 — W3 行写：`in-place rows` 成立，`copying out` 不是 0

- 宿主日志每一帧都是 `in-place rows`（W3 生效，画面按行写进显示用 IOSurface）；
- 画面与客户机自绘逐字节一致：R1 的 5 次冷启动共 1,230 帧全 `MATCH`；
- **`copying out` 仍在**（45–48 ms）：它量的是本地合成（`target_to_surface`）那段，
  要它 ≈ 0 必须关掉本地合成（W2），而 W2 一关就进 R3 的黑屏；
- 一帧 < 125 ms：面板配置下宿主间隔 0.05–0.24 s，抖动大；R3 配置稳定 0.09–0.10 s。

## 5. R5 — 对照表

| 配置 | 驱动稳态（`MiniIOMobileFramebuffer: N frames in M ms`） | 界面 |
|---|---|---|
| 纯本地（`GL=1`，不装记录器，`/tmp/root-noremote.apfs`） | 第三轮实测 7–9 fps，本轮未重测 | 正常 |
| 按引用 + 面板 + 行写（`GLWIRE_LOCAL=1`） | 4.0–6.0 fps（五次启动同一镜像） | 正常，448 KB |
| W1+W2（`remote_only`） | 4–9 fps（宿主 10–11 fps） | **黑屏** |
| W1+W2+W3 | 同上（W3 已在，`in-place rows`） | 黑屏 |

「滚动设置列表 20 s」的驱动稳态**没有量到**：触摸驱动的镜像连续 4 次冷启动都 panic（
`Ticket spinlock timeout`／`lsd` 刷屏），同一内核换成不带触摸的 `rc.vtop` 连续 5 次全部正常。
所以 R5 的「驱动稳态」这一列本轮只有空闲桌面的数字，**必须把触摸路径的稳定性修掉才能补**。

## 6. R6 — 回归

- `map_test`、`e2e_test.py` 通过（宿主读回改动之后跑过）；
- 请求行仍接受 4/5/6 字段（`sscanf` 的 `>= 4`，见 `serve_frame`）；
- 开关清单未新增隐形开关：本轮新增 `GLWIRE_NOFLUSH`（构建）/`glwire_noflush=1`（启动）、
  `glwire_nopicture=1`（原有）、`--no-panel` 现在真的生效（原来被 `main()` 里一行
  `no_panel = getenv(...)` 覆盖掉了，命令行参数静默无效）。

## 7. 没做到的，和下一步

1. **R2 的同帧 MATCH**：一帧相位差的来源没定论。下一步是把相位差定死（客户机 present 时机），
   然后要么把面板上传挪到 present 之后，要么在验收里按「上一帧画面 vs 本帧面板」来判。
2. **R3 的黑屏**：`remote_only` 时保留纹理上传（只跳过 draw/本地合成），这是 R3/R4 的前提。
3. **触摸路径的稳定性**：R5 的驱动稳态、以及任何「滚动设置列表」的测量都卡在这里。
   现象是可复现的：带触摸的镜像 4/4 次失败（panic 或 lsd 卡死），不带触摸 5/5 成功。
4. R5 的纯本地基线要同机同镜像重测（本轮没有排到它）。

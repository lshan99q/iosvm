<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十阶段.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十阶段结论

> 时间：2026-11-04 09:00–（P0''' 已出第一组证据，其余在跑）
> 本文只写**量到的数字、每个决定的结论、卡在哪一行**；目标与判据看 `IOSVM-GPU加速第十阶段工程.md`。

---

## 零、一句话现状

**停顿不是「整机停住」，是「整机 100% 忙于渲染」**：`ustacks=top` 的采样里，UI 阶段 **60 个样本是 ≥399% busy**
（4 个 cpu 全满，400% 是上限），窗口内最忙的是 **SpringBoard**（窗口内累计 6973%）、`launchd.darwin`、`chronod`；
启动阶段（420 s 之前）只有 115–120%。所以「6–15 秒不产帧」= 客人机在渲染，不是死锁。

这条同时解释了前面的三件事：4 s 族锁为什么被打穿（拿到锁的线程被饿着）；P3' 的窗口为什么只有 2 帧
（一帧要 6–30 秒的 4 核时间）；以及 panic 栈为什么落在调度器的 timeshare 维护路径上。

---

## 一、P0''' 停顿的栈：机器在跑，不是停

一次启动（`TAG=m10top`，09:00 起，镜像 `/tmp/root-fixed-net.apfs`）：

```
XCMD="slto_us=30000000 tlto_us=60000000 lpto_us=60000000 glframes=1 ustacks=top ustacks_at=420 ustacks_ms=1000 ustacks_count=600"
```

内核自己的采样（原样）：

```
top: in the last 1006 ms the processor was busy 117% of the time
top:   73.9%  lsd[32]
top:   26.9%  containermanager[18]
...
top: in the last 1003 ms the processor was busy 400% of the time        ← UI 阶段，4 核全满
top:  401.2%  SpringBoard[60]                                            ← 单进程就超过 4 核的上限（多线程计时）
top:  120.3%  launchd.darwin[1]
```

- **busy% 的分布：400 个采样里 331 个 ≥390%（83%）**（基本都在 UI 阶段，400% 是 4 核上限），启动阶段 115–120%；
- 停顿窗口（帧间隔 >3 s，脚本 `/home/lshan/DSH-desktop/iosvm/tools/glwire/harness/ustacks_windows.py`）：

```
stall from       to      s busy% in  busiest inside
at   482.4s  ->   488.9s    6.5     401%  SpringBoard 6973%, launchd.darwin 2530%, chronod 1509%
mean busy% inside stalls 397%   outside 313%   (1 windows, 242 samples)
-> running (something held the processors)
```

`boots` 复核后的更细口径（9 个 ≥2 采样的窗口，602.5–606.0 … 987.6–1000.0 s）：**窗口内 387–404% vs 窗外 351–360%**，
窗口内 **SpringBoard 占 90–98%、backboardd 78–87%**；线程级 132 个采样、4,110 条 thread 行、1,778 条 `(running)`。

**结论：停顿是 CPU 饱和（渲染），不是死锁/冻结。**

### 线程级采样：停顿期间 SpringBoard 在**搭 Today 视图**（`m10thr`）

`ustacks=SpringBoard ustacks_tname=*`（109 个采样点、4,216 行）：每个采样点 18 个线程里 **7–11 个 `(running)`**，
其余阻塞在 `mach_msg`。符号化（`qemu/symstack.py` + `ipsw`，见下面的工具债）后，最热的那条线程栈原样：

```
thread 0xd0 (running)
  +[MTMaterialView initialize] + 20
  +[SBFFeatherBlurView initWithRecipe:] + 208
  -[SBTodayViewSpotlightPresenter _setUpFeatherBlur] + 96
  -[SBTodayViewSpotlightPresenter initWithSearchBarViewController:containerView:scrollView:delegate:] + 304
  -[SBTodayViewController _setUpSearchPresenter] + 652
  -[SBTodayViewController loadView] + 136
  -[SBIconController _newTodayViewControllerWithIconManager:iconListModel:rootFolder:location:] + 248
  -[SBIconController _rootFolderDidChange] + 508
```

也就是：**「停顿」是 SpringBoard 在重建主屏的 Today/Spotlight 视图**（`SBFFeatherBlurView` 的 MTMaterial 模糊材质
+ `_rootFolderDidChange`），全程吃满 4 个模拟核；另一个高频 PC 是 `_start_wqthread`（617 次，工作队列线程在反复启动，
典型的 `dispatch_async` 扇出）。

这条同时解释了两件事：**为什么记录流里没有字/图标**——它们由 SpringBoard 自己用 CA/CoreGraphics 画（P3''' 那节），
而重建发生在启动之后、每次 `_rootFolderDidChange` 都会再来一遍；以及**为什么帧率是 0.1–4 fps**（每帧背后是这种重建）。

**工具债（已修）**：宿主上没有 `ipsw`，`qemu/symstack.py` 起不来。本阶段临时装了 `ipsw v3.1.741` 到 `/tmp/ipsw`，
并给 `symstack.py` 加了 `SYMSTACK_CACHE` 环境变量（`sysimage/` 里的缓存没有 `.symbols`，`build/images/rootfs/.../dyld_shared_cache_arm64`
旁边才有）；另外它会把 `?` 缓存进 `a2s.json`，遇到「全是 ?」先删掉那个文件。

---

## 二、P1''' 缩小停顿：**压不下去，是单线程瓶颈**（多核无效）

三次启动，除 `QEMU_SMP` 外参数完全一致（`ustacks_at=420 ustacks_ms=1000 ustacks_count=600`、`glframes=1`），
同一绝对相位 `[700, 1000] s` 对比（`boots` 的 t12）：

| 配置 | busy% 中位（每核） | ≥390% 比例 | UI 窗停顿 | 700–1000 s 停顿（最长） | guest 帧（700–1000 s） | 到界面 |
|---|---|---|---|---|---|---|
| `SMP=4` | 399%（99.8%） | 332/399（83%） | 30 | 16（17.6 s） | 1,085 → **3.62 fps** | 571 s |
| `SMP=6` | 599%（99.8%） | 341/403（≥590% 85%） | 35 | 21（16.7 s） | 1,102 → **3.67 fps** | 573 s |
| `SMP=8` | 748%（93.5%） | 110/391（≥790% 28%） | 32 | 15（13.8 s） | 1,220 → **4.07 fps** | 476 s |

**结论：多核无效**——加核只是把 CPU 占用摊得更满（400%→600%→750%），帧数只 +12%（3.62→4.07 fps），
停顿 16→21→15 次、最长 17.6→13.8 s，离判据的「下降 ≥50%」很远。所以渲染路径在 TCG 下是**单线程瓶颈**，
与 t10 的「窗口内 SpringBoard 90–98%、backboardd 78–87%」一致。

判据 P1''' 的写法（诚实版）：**「压不下去」，原因 = 单线程瓶颈；已排除的杠杆 = 模拟核数**。
下一杠杆（本阶段没跑）：`tb-size` 1536→3072、`-accel tcg,thread=multi` 的等价开关、或减少 Today 视图的重建
（`_rootFolderDidChange` 每次都会触发）。

## 三、P2''' 测量口径

（待）

## 四、P3''' B 路靶子：**SpringBoard 用 CoreGraphics 把 glyph 光栅化进 IOSurface，根本不经过我们的记录器**

`porter` 只读查证（t11，未占 VM），点名与证据：

1. **点名**：客户端（SpringBoard）用 CoreGraphics 把文字/图标光栅化进 **IOSurface**，
   渲染服务器只把 surface 当纹理。可 interpose 的 C 符号：**`CAImageQueueRegisterIOSurfaceBuffer`**；
   CoreGraphics 的 glyph 机器：`CGContextShowGlyphsAtPositions` / `CGGlyphLockLockGlyphBitmaps` / `CGGlyphBitmapCreateWithPathAndDilation`。
2. **证据（原样）**：
   - `MiniIOSurface: SpringBoard[10] created surface 3: 184x124 'BGRA', 736 bytes per row`
   - `MiniIOSurface: SpringBoard[10] created surface 4: 312x202 'LA08', 640 bytes per row`（LA08 = 亮度+alpha 覆膜，就是文字位图）
   - 我们的库在 backboardd 里只记到 25 次 `-texImageIOSurface:`（全是 backboardd 自建 surface：180x180、28x95…）与 6 次 `attachImage:`；
   - **SpringBoard 的 3/4 号 surface 在这些记录里 0 次出现，宿主也没收到任何 LA08 上传** —— 文字层压根没走我们记录到的纹理路径，
     这正是 P2' 差集「图标在、字不在」的机制。
3. **反证**：① 不是 Metal——`HakiosMetal: backboardd is not shown the device (metal=1; …)`，backboardd 在 `metal=1` 下没有 `MTLDevice`，
   CA 实际走 OpenGL ES；② 不是「CG 位图经记录的 GL 纹理上传」——记录的 25 次全是 backboardd 自建 surface；
   ③ QuartzCore 的 C++ 渲染器符号被剥掉（共享缓存里 `_ZN2CA` 命中 0），所以只能从 C/ObjC 的 surface API 下手。
4. **接进流里的第一步**（不承诺实现）：在 backboardd 里 interpose `CAImageQueueRegisterIOSurfaceBuffer`，
   每次打一行 `surface ID + 尺寸 + 像素格式 + queue`，与内核 `MiniIOSurface: … created surface` 对表，
   先确认 312x202 LA08（文字覆膜）的到达时刻；再把该 surface 的像素按 panel 上传的方式写进 wire。

## 五、工具与提交

| 文件 | 用途 |
|---|---|
| `harness/ustacks_windows.py` | 把内核 top 采样与帧间隔放同一根时钟，回答「停顿窗内是在跑还是在等」 |

## 六、下一步

（随证据补齐）

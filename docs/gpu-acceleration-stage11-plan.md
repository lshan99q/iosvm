<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十一阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十一阶段工程文档

> 建立时间：2026-11-04 12:40
> 前置：第十阶段（`IOSVM-GPU加速第十阶段.md`：停顿归因、四配置数字、B 路靶子）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **启动稳了**：三族 deadline 抬到 60 s → 8/8 冷启动到界面、0 panic。
2. **加速有效，但屏幕是黑的**：`W1.5+W3` 配置宿主收到 **4,101 帧 / 807 s（5.08 fps）**，客机截图却只有 **14,324 B**；
   而「客人机自己画」的纯本地配置只有 **0.06 fps**，屏幕是亮的（446 KB）。
   → **现在的加速是「有帧、没画面」**，这是本阶段的第一目标。
3. **画面缺字的机制已知**：文字/图标由 SpringBoard 用 CoreGraphics 光栅化进 IOSurface（`312x202 LA08` 覆膜），
   从不经过记录器；靶子已点名 `CAImageQueueRegisterIOSurfaceBuffer`，还没动手。
4. **停顿是渲染成本**（4 核 100% 忙于 SpringBoard 重建 Today 视图），加核无效；唯一没试过的杠杆是**别让它重建**。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | **先修「屏幕黑」还是先接 B 路（glyph）**？ | 先接 glyph：接进来照样看不见，白做 | 「加速后到底看得见」这条结论，以及整个项目的可用性 |
| D2 | 黑屏的靶子：**行写回的目标 region（W3）** vs **显示面的更新路径（attachImage/commit）**？ | 猜错方向：改一个地方，画面还是不动 | 一个能上屏的最小改动 |
| D3 | Today 视图重建：**关掉/降级**（`_rootFolderDidChange`）vs 接受它、只优化别的？ | 关掉：主屏功能变了，且可能只是把成本挪走 | 停顿数与整段帧率的真提升 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0'''' | **把画面送上屏** | `remote_only + W3` 的一次启动里，**客机截图 > 300 KB**，且与同帧 host picture 的 `diffdump --tolerance 8` **≥95%（同一帧）**；达不到就写清「C 路断在哪一行」 | 项目可用性 |
| P1'''' | 黑屏的归因 | 证据链一条：行写回写了多少字节、写到哪个 region/格式、显示面为什么没读到（`attachImage:`/`-texImageIOSurface:`/`MiniIOMobileFramebuffer` 三者谁缺）；一次启动 ≥10 条原样日志行 | P0'''' |
| P2'''' | B 路第一步 | interpose `CAImageQueueRegisterIOSurfaceBuffer`，一次启动打 **≥5 条 surface 注册行**，其中含 **LA08（文字覆膜）**；能与内核 `MiniIOSurface: … created surface` 对上尺寸/ID；给「要不要把它写进 wire」的结论 | 画面完整性 |
| P3'''' | Today 视图重建 | 找到并关掉/降级重建（`_rootFolderDidChange` 的触发条件）：**停顿数下降 ≥50%**，或写清为什么不能 | 整段帧率 |
| P4'''' | 四格重跑（可选） | `WINDOW_AFTER=60` + 截图判据 200 KB，四格各一个「120 帧 / X 秒」数字 | 结论表 |
| P5'''' | 文档与提交 | `dsh.MD` 与 `iosvm/docs` 同步、`make test` 通过、**推送到 GitHub**（`lshan99q/iosvm`） | — |

---

## 三、需求详述

### P0''''/P1'''' 把画面送上屏（本阶段主线，1–2 天）

已知（第十阶段）：`W1.5+W3` 的宿主有 4,101 帧、`MiniIOSurface` 行正常、截图 14,324 B（全黑）；
`W1.5`（W3 关）截图 243,860 B（有画面、缺 wallpaper/glyph）；纯本地 446,737 B（全亮）。

要先分清三件事（一次启动就能分）：
1. **行写回**（`GLWIRE_NO_INPLACE=0` 时的 in-place 写）到底写了多少字节、写到哪个 surface/region：
   宿主日志里 `host_glwire: … in-place …` 与 guest 的 `hakios-remote: … in_place`；
2. **显示面**（`MiniIOMobileFramebuffer`）有没有被更新：guest 侧 `MiniIOMobileFramebuffer: N frames in M ms` 是否在涨；
3. **合成器**（backboardd）有没有把我们的 surface 当纹理：`attachImage:` / `-texImageIOSurface:` 的记录。

判据 P1''''：三者至少两条给出「缺在哪」的直接证据；判据 P0''''：改一处之后截图 >300 KB 且同帧 ≥95%。

最小的可疑点（按证据顺序试，一次只改一个）：
- 行写回的目标 region 是否覆盖整屏（750x1624）而不是局部；
- 写回的像素格式（BGRA/RGBA、行序）是否与显示面要的一致；
- 写完有没有**通知合成器**（CA 的 commit/`attachImage:`）——没有通知的话，画面在内存里、屏幕不动。

### P2'''' B 路第一步（1 天，可与 P0'''' 并行——一个占 VM、一个读代码/写 interpose）

按第十阶段的靶子：在 backboardd 里 interpose **`CAImageQueueRegisterIOSurfaceBuffer`**，只打一行
`surface ID + 尺寸 + 像素格式 + queue`；与内核 `MiniIOSurface: … created surface` 对表，
确认 `312x202 LA08` 的到达时刻。判据：一次启动 ≥5 条注册行、含 LA08、能对上尺寸；
并给一句「把它的像素写进 wire 的第一步是什么」（不承诺实现）。

### P3'''' Today 视图重建（1 天）

第十阶段的热栈：`-[SBIconController _rootFolderDidChange]` → `-[SBIconController _newTodayViewControllerWithIconManager:…]`
→ `-[SBTodayViewController loadView]`。要回答：这次重建是不是每次启动都发生、由什么触发
（图标列表变化？spotlight 初始化？），能不能关掉（例如不建 Today 页、或用 boot arg 让 SpringBoard 跳过）。
判据：停顿数（帧间隔 ≥1 s 的次数）下降 ≥50%，或写清「不能关，因为……」+ 证据。

### P4'''' 四格重跑（可选，半天）

`harness/drive-scroll.sh` 已支持 `WINDOW_AFTER=S`（界面认出后等 S 秒再开窗口）。四格：
`WINDOW_AFTER=60 WINDOW_FRAMES=1 WINDOW_SPAN=120`，截图判据改 200 KB（W1.5 的 243,860 B 就能过）。
判据：四格各一个「120 帧 / X 秒」，并注明时钟（pure 用 guest、其余 host）。

---

## 四、不做（已被数据否掉，别重试）

- 不再抬锁 deadline（三族 60 s 已经 8/8、0 panic）。
- 不再加模拟核数（SMP 6/8 只把占用摊满，帧数 +12%）。
- 不再在 EGL 表里找 glyph（12 个候选 0 调用）。
- 不再用「固定 20 秒」或「启动后的前 N 帧」当窗口（会量到 Today 视图重建）。
- 不在屏幕黑着的时候声称「加速有效」——5.08 fps 是宿主收到的帧，不是用户看到的画面。
- 像素去重、客户机内 GL 加速、只降分辨率、`touch.py stacks`、`pkill -f`、并行第二台虚拟机。

---

## 五、验证环境与操作规范

1. **同一时间一台虚拟机**；跑启动的人持 `/tmp/iosvm-vm.lock`，构建的人等锁消失；
2. 改端口代码：`touch obj/stage0` → `IOSVM_USE_LLD=1 ./build.sh root.apfs`；
   改内核：`LANG=C LC_ALL=C IOSVM_USE_LLD=0 tools/make-kernel.sh -j8`，**改完必须自己跑 150 s 短启动**；
3. 符号化：`ipsw` 在 `/tmp/ipsw`，`SYMSTACK_CACHE` 指向 `build/images/rootfs/…/dyld_shared_cache_arm64`；
4. 结论 = 一行日志 + 一个数字 + 一次可复现的启动参数；「没做到」写卡在哪一行。

---

## 六、里程碑

| 里程碑 | 内容 | 判据 |
|---|---|---|
| M11.0 | 黑屏的三件事分清 | P1'''' |
| M11.1 | 画面送上屏 | P0'''' |
| M11.2 | glyph surface 的到达时刻 | P2'''' |
| M11.3 | Today 重建能不能关 | P3'''' |
| M11.4 | 四格重跑（可选）+ 文档推送 | P4''''/P5'''' |

---

## 七、诚实的预期

- **能确定的**：黑屏这件事一定能查清「缺在哪一步」——行写回、显示面、合成器三条链路各自都有现成日志；
  改动量小（写回 region/格式、或补一次通知）。
- **不确定的**：补上之后画面能不能真的动。W3 的写回是「写进客机 RAM 里的显示面」，而显示面由
  `MiniIOMobileFramebuffer` 拉到屏幕上；如果这一层不刷新，问题就不在 wire 而在驱动。
- **不能承诺的**：B 路（glyph）一定能接进流里；以及屏幕黑修好之后整段帧率还能保持 5 fps。

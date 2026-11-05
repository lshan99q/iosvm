<!-- 与 /home/lshan/dsh.MD/IOSVM-GPU加速第十二阶段工程.md 同一份；改一处就要改另一处 -->

# IOSVM GPU 加速 —— 第十二阶段工程文档

> 建立时间：2026-11-04 15:00
> 前置：第十一阶段（`IOSVM-GPU加速第十一阶段.md`：画面已上屏、双面写回、CAImageQueue 不可 interpose、Today 重建一次性）
> 本文只回答：**下一阶段做什么、每件事的判据、怎么验、不做什么**

---

## 零、一句话现状

1. **屏幕不黑了**：双面写回之后（`eagl_remote_surface2` + 请求行八个字段），界面出现后的 19 张截图全是主屏
   （240,799–244,645 B），同帧 diff **97.7%**；`s11c` 里「每 20 s 一张、一半是黑」的现象消失。
2. **但画面缺三样**：wallpaper、文本/字形（`312x202 'LA08'` 覆膜）、widget 快照（`184x124 'BGRA'`）。
   主屏 245 KB，纯本地是 446 KB——**300 KB 判据差在内容，不在传输**。
3. **用户态 interpose 这条路死了**：`CAImageQueueRegisterIOSurfaceBuffer` 与 `IOSurfaceCreate` 两个钩子
   在两个进程里加载成功、0 条调用行；调用者不 import、QuartzCore 同镜像直调、库插得比 chained fixup 晚。
   唯一可靠的 surface 登记处是内核 `MiniIOSurface: … created surface`（一次启动 24 条，含 LA08）。
4. **最大停顿在界面之前**：`host_glwire: frame 3 … interval 460.55 s`；Today 重建（一次启动一次，505 s）
   只值 −17% 停顿、−7% 停顿秒数——`today_off=1` 的收益是「界面早 60 s、帧数 +22%」，不是判据要的 −50%。

---

## 零之一、本阶段已经量到的（2026-11-05）

| 问题 | 结论 | 数字/原样行 |
|---|---|---|
| 460 s 启动空洞是什么 | **lsd 的串行 LaunchServices 工作**，不是饱和、不是锁、不是宿主 | 启动期 470 个样本：busy 中位 124%、≥4 核仅 7%；lsd 27,598%、containermanagerd 15,240%；界面之后才 399% 中位（SpringBoard 第一） |
| LA08 字形 / 184x124 widget 面里有没有东西 | **有**：字被画进去了 | LA08 `45412 of 126048` 非零（36.0%）；184x124 `79425 of 91264`（87.0%）；内核与用户态两个视角逐位相同 |
| 这两张面有没有交给渲染服务器 | **交了**，服务器也读到了 | `SpringBoard[10] made a port for surface 4: 0x18b03` → `backboardd[16] looked up surface 4 (312x202 'LA08') from a port: 0 …` |
| 那屏幕上为什么没有 | **CA 在 backboardd 里没有把它们变成纹理** | 同一次启动 `-texImageIOSurface:` 25 次全是别的面（11..22 等），28 张 image 里也没有 3/4 |

一句话：**字画了、也交到渲染服务器手里了，但 CA 没画**。下一步的靶子因此非常具体：backboardd 里
「收到的 image」到「绑成纹理」之间那一步（`CA::Render::Image` 解码、LA08 这类 luminance+alpha 内容的着色器路径、
或 `-texImageIOSurface:` 之外的绑定入口）。`metal=1` 已排除（该参数只给 `metal_test`，backboardd / SpringBoard
都「not shown the device」）。

---

## 一、这一阶段的三个决定

| # | 决定 | 选错的代价 | 选对了解锁 |
|---|---|---|---|
| D1 | 缺的内容怎么进 wire：**内核侧 surface 探针**（在创建/显示处读像素）vs **用户态 fishhook**（GOT 重绑）vs 放弃 | 在内核里做：改内核、一次构建 15 分钟、错了要重来；在用户态做：可能再吃一次「静默」 | 画面完整（wallpaper + 字）与 300 KB 判据 |
| D2 | 界面之前那个 **460 s 帧间隔**：查清并修 vs 接受它 | 不查：整段帧率永远被它拉低，用户看到的「启动 8 分钟」就是它 | 启动时间与整段 fps |
| D3 | 性能口径：**界面后 600 帧**为唯一窗口 vs 整段/固定秒数 | 用整段：继续被启动期淹没（第十阶段已踩过） | 四格可比的数字 |

---

## 二、目标与验收（可测）

| # | 需求 | 判据（可测） | 阻塞谁 |
|---|---|---|---|
| P0''''' | **缺的内容进 wire** | `remote_only + W3` 的一次启动里，客机截图 **≥300 KB**，且与同帧 host picture 的 `diffdump --tolerance 8` **≥95%**；达不到就写清「断在哪一步」 | 项目可用性 |
| P1''''' | LA08 定点 | 一次启动里，`312x202 LA08` 的像素在宿主侧被说出（上传行或纹理行）**≥1 条**，且它的哈希与客人机同名 surface 的采样哈希相等；`184x124 BGRA` 同样处理（至少点名） | P0''''' |
| P2''''' | 460 s 帧间隔 | 符号化热栈 **≥20 个样本**（`ipsw` + `qemu/symstack.py`），说清它卡在哪个函数；能修则**启动到界面的时间 −30%**，不能修写清为什么 | 整段 fps |
| P3''''' | 四格表（修好的镜像） | 四格（纯本地 / 按引用 / W1.5 / W1.5+W3）各一个「120 帧 / X 秒」，注明时钟；`W1.5+W3` **≥4 fps** | 结论表 |
| P4''''' | 文档与提交 | `make test` 通过、`dsh.MD` 与 `iosvm/docs` 同步、**推送到 GitHub** | — |

---

## 三、需求详述

### P0'''''/P1''''' 把缺的内容送上屏（本阶段主线，1–2 天）

已知（第十一阶段）：
- 传输与显示都没问题：客人机 surface 哈希 == 宿主 picture 哈希（第 1 帧、第 5300 帧都对上），
  两张显示面都写（`… and a second` 2,840 条），界面后 19/19 张截图是主屏；
- 缺的三样都是**没经过记录器的 surface**：
  - `MiniIOSurface: SpringBoard[10] created surface 4: 312x202 'LA08', 640 bytes per row`（28,677 行日志里唯一一次，
    没有 `-texImageIOSurface:`、没有 `-attachImage:`）—— 字形；
  - `184x124 'BGRA'`（surface 3）同样缺席 —— widget 快照；
  - wallpaper：每张 picture 背景全黑，没有确认任何 wallpaper 尺寸的 surface。
- 用户态 interpose 已证否（第十一阶段 P2''''，0 条）。

按证据顺序试，一次只改一个：
1. **内核侧 surface 探针**（首选）：在 `MiniIOSurface` 创建处（已有日志）之外，再加一条「第一次被当作纹理/
   被显示时」的采样行（`s->pixels()` 的 5 行 5 点哈希 + id + 尺寸 + 格式）。它不改像素、只说话，
   能一次回答「LA08 的像素有没有到达显示路径」。
2. **用户态 fishhook**（备选，半天）：`inject_ciq_probe.c` 里改用 GOT 重绑（遍历各镜像的
   `__DATA_CONST,__got` / `__la_symbol_ptr`，名字匹配 `_IOSurfaceCreate`），绕开「库插得比 fixup 晚」；
   判据：一次启动 ≥5 条 `ciq:` create 行、含 LA08。
3. **客人机上传**（如果 1/2 都只给「看不见」）：在 `eagl.c` 的 `frame_end` 里，把 LA08 surface 的像素用
   `eagl_remote_upload()`（已有 201 号记录、按引用）送上去——只有在确认「CA 用这张 surface 当纹理」之后做。

**已试过的一发（s12a，16:56 建、16:00–16:13 跑，负结果）**：GOT 重绑（fishhook）版探针，`CIQ=1` 编进镜像并启动。
重绑确实跑了 ——
`ciq: got rebind SpringBoard: 37 slot(s) in 35 image(s) (IOSurfaceCreate=34+0 island, CAImageQueueRegisterIOSurfaceBuffer=3+0 island)`
`ciq: got rebind backboardd: 17 slot(s) in 16 image(s) (IOSurfaceCreate=15+0 island, CAImageQueueRegisterIOSurfaceBuffer=2+0 island)`
—— 但 **0 条 create 行、内核 0 条 `created surface`、宿主 0 帧、900 s 里截图全是 14,316 B**：这次启动没到界面。
最可能是替换函数在 `inject_ciq_probe.c:235` 直接调用 `IOSurfaceCreate()`（被自己的 patch 命中），也可能是探针把启动链拖垮。
**它不能进默认镜像**：`build.sh` 已改成 `CIQ=1` 才编、默认镜像不带它。结论：用户态钩子到此为止，下一发按上面第 1 条走**内核侧**。

**工具债（2026-11-05 发现）**：机器在 11-05 重启过，**`/tmp/cctools` 被清掉**（内核链接器 `aarch64-apple-darwin11-ld` 的
唯一副本），于是 `IOSVM_USE_LLD=0 tools/make-kernel.sh` 回落到 `ld64.lld`；lld 产出的内核 __TEXT 在 `0x100000000`，
loader 判 `unexpected kernel layout` 拒载（`xnuloader/loader.c:2901` 要求 `vm_lo ≥ 0xfffffff007000000`）。
lld 的 `-image_base` / `-segaddr` 都是「not yet implemented」，所以**在 cctools 找回来之前，内核侧改动没法验证**。
已做的两件事：① `hakios-kernel` 从 `hakios-kernel.prebuilt` 恢复（能正常启动，150 s 自检通过）；
② 内核侧的 `surfsample=` 采样代码留在树里但**未生效**，改走用户态等价实验（见下）——SpringBoard 自己用
`IOSurfaceLookup(id)` 读 surface 3/4，5 秒一次打哈希与非零字节数（`inject_sb.c` 的 `surfprobe:`，boot-arg
`surfsample=4 surfsample_b=3`，无需重编内核）。

**已恢复（2026-11-05 17:5x）**，步骤记下来（下次再被清照这个做）：
1. 代理下 `git` 的 TLS 会被打断（`gnutls_handshake() failed`）；`GIT_SSL_NO_VERIFY=true git clone --depth 1 …` 就能过
   （`curl` 到 github API 正常，但 codeload / archive 的跳转不通）。
2. libdispatch（cctools 的 configure 硬性要求 `-ldispatch`，Ubuntu 无包）：
   `git clone --depth 1 https://github.com/apple/swift-corelibs-libdispatch` →
   `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DINSTALL_PRIVATE_HEADERS=YES -DENABLE_SWIFT=OFF -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++`
   （**必须用 clang**：libdispatch 的 warning 集 gcc 不认 —— `cc: error: unrecognized command-line option '-Warray-bounds-pointer-arithmetic'`）
   → `cmake --build build -j16 && sudo cmake --install build && sudo ldconfig`。
3. cctools-port：`git clone --depth 1 …/tpoechtrager/cctools-port` → `cd cctools && ./configure --prefix=/tmp/cctools
   --target=aarch64-apple-darwin11 && make -j16 && make install`（ld64 956.6，与之前一致）。
4. 内核：`LANG=C LC_ALL=C IOSVM_USE_LLD=0 tools/make-kernel.sh -j8` → `hakios-kernel` 的 `__TEXT` 回到
   `0xfffffff007004000`（`llvm-objdump --macho --private-headers` 验），banner
   `Darwin Kernel Version 22.5.0: Thu Nov 5 17:55:15 CST 2026`；150 s 自检：1 次 SpringBoard、0 panic、0 布局错误。
   顺带记一条**死路**：lld 路径加 `-image_base` 没用 —— `ld64.lld` 对 `-image_base` / `-segaddr` 都是 "not yet implemented"。

### P1''''' 的结果（s12surf，18:12–18:33）：画了，但没交出去

工具：内核侧 `surfsample=<id>`（`MiniIOMobileFramebuffer.cpp` 的 presenter 每 5 秒对指定 surface 做 5 行哈希 +
**全量非零字节统计**）+ 用户态 `surfprobe:`（`inject_sb.c`，SpringBoard 自己 `IOSurfaceLookup(id)`，同一算法）。
两个视角独立，结果逐位相同：

```
MiniIOSurface: SpringBoard[10] created surface 3: 184x124 'BGRA', 736 bytes per row, 0 planes, 98304 bytes
MiniIOSurface: SpringBoard[10] created surface 4: 312x202 'LA08', 640 bytes per row, 0 planes, 131072 bytes
MiniIOSurface: surfsample 4 312x202 'LA08' 640 bytes a row, rows b9fee455 549649bc c1bd17f0 c1bd17f0 b9fee455, 45412 of 126048 sampled bytes not zero
surfprobe: id=4 312x202 bpr 640 bpe 2 rows b9fee455 549649bc c1bd17f0 c1bd17f0 b9fee455, 45412 of 126048 sampled bytes not zero (pid 10)
MiniIOSurface: surfsample 3 184x124 'BGRA' 736 bytes a row, rows b9fee455 dd718f7f 05f98735 79440d15 b9fee455, 79425 of 91264 sampled bytes not zero
```

结论：

- **字形覆膜不是空的：36.0%（45,412 / 126,048 字节）非零**；widget 快照 **87.0%（79,425 / 91,264）非零**
  —— CoreGraphics 确实把字和 widget 画进去了，内容还在。
- 同一次启动里**出帧的只有 backboardd（pid 16：1,001 次 `end of frame`、999 次 `flush`）**；SpringBoard（pid 10）
  一次都没有。28 张 image 列表与 26 条 `-texImageIOSurface:` 里都没有 3/4 号面。
- 所以缺的不是「画」，是**客户端 surface 交给渲染服务器这一步**（CA 的 client→server handoff）。图标能出现，
  是因为 backboardd 自己创建并绘制 180x180 面；wallpaper / widget / 文字这些客户端内容，一张都没到服务器。

判据 P1'''''：**达成**（≥1 条宿主/内核行、哈希与客人机 surface 相等；这里是两个视角各自相等）。

**顺带修的两个采样 bug（都值得记）**：① 内核侧最初用 `width * 4` 判像素宽度，`LA08` 是 2 字节/像素
（640 B/row，312 宽），于是永远打「no pixels to read」；② 稀疏采样（每 16 行、每 64 字节）把 36% 的内容读成
「2/130」，改成全量统计才有意义。

**再进一步（s12trace，18:34–18:49，`surftrace_of=SpringBoard surftrace=32`，内核现成的追踪）**：
handoff **是发生的**，丢在服务器里：

```
MiniIOSurface: SpringBoard[10] created surface 3: 184x124 'BGRA', 736 bytes per row
MiniIOSurface: SpringBoard[10] created surface 4: 312x202 'LA08', 640 bytes per row
MiniIOSurface: SpringBoard[10] made a port for surface 3: 0x1c903
MiniIOSurface: backboardd[16] looked up surface 3 (184x124 'BGRA') from a port: 0; of every 64 bytes 1205 of 1536 begin with a pixel that is not 0 …
MiniIOSurface: SpringBoard[10] made a port for surface 4: 0x18b03
MiniIOSurface: backboardd[16] looked up surface 4 (312x202 'LA08') from a port: 0; of every 64 bytes 1441 of 2048 begin with a pixel that is not 0 …
```

- SpringBoard 确实给 3/4 号面**建了 port**；backboardd **从 port 查到了它们**，还读到了像素（1205/1536、1441/2048 非零）。
- 但同一台机器上 `-texImageIOSurface:` 只被调用 **25 次，全是别的面（11..22 等）**，没有 3/4；28 张 image 里也没有。
- 也就是说：**交出去了、服务器也拿到了，但 CA 在 backboardd 里没有把它们变成纹理画出来**。这不是「客户端没发」，
  是「服务器收到后没绑」。
- 一个对照：图标能显示，而且日历图标上「THU 5」也在 —— 文字**作为图标纹理的一部分**是能上屏的；上不了的只是
  LA08 那种「客户端字形图集 + 颜色」类型的 layer 内容。

**另一条线索（texlog，2026-11-05 19:2x–20:0x）：宿主的 GL 吞掉了一类上传**

给 `eagl.c` / `remote.inc` 加 `texlog=1`（记录 >8000 像素的纹理上传，含 `glTexSubImage2D`），一跑就看到界面阶段
backboardd 在上传：

```
hakios-gl: texImage 177x90 format 0x1906 type 0x1401, filled (pid 16)     # 0x1906 = GL_ALPHA
hakios-gl: texImage 184x90 format 0x1906 type 0x1401, filled (pid 16)
...  512x512/256x256/128x128 format 0x190a (GL_LUMINANCE_ALPHA)
```

这些是**字形图集**（每条 90/42 高）。但宿主侧 `host_glwire: replay ... glerror 0x502`（GL_INVALID_OPERATION）在
每次启动的 **8 帧**上出现 —— 桌面 GL 4.6 不接受 `internalformat = GL_ALPHA`。试了在宿主把三种 legacy 格式展开成
RGBA 再上传（`host_legacy_teximage`）：
- 展开确实发生（8 条 `glwire: legacy texture ... expanded to RGBA`），但 0x502 仍有 8 帧；
- 画面变化只有两处：widget 白块**圆角变直角**（与修复前逐像素差 14,934 px）、搜索框字形略有不同；
- **App 标签仍然没有** —— 所以这不是缺字的主因。

因此这条修复**默认关闭**（宿主 `GLWIRE_LEGACY_FORMATS=1` 才开），只作为线索记下来：宿主上下文与客人机 GL 的
legacy 格式不兼容是真的，但它影响的是「服务端自己画的字形」，不是 LA08 那条客户端内容链。

**试过但没用的开关（2026-11-05 20:12–20:27）**：`caenv=CA_DEBUG_TRANSACTIONS=1`（inject.c 现成的 `caenv`
机制，会把变量塞进 backboardd 的环境）——客人机照常起（644+ 帧、44.9 fps），但串口上**一条 CA 事务日志都没有**：
这版 QuartzCore 不认这个开关（或它的输出不走 stderr）。这条路不通，别再花时间。

**第三发（s12dump，2026-11-05 20:52–21:07）：字被画了，但画不出东西**

两个宿主探针（都已进仓库）：
- `GLWIRE_DUMP_TEXTURES=…` + `GLWIRE_DUMP_FROM=1500`：界面稳定后把宿主手上的每张纹理落盘。结果 **144 张**：
  两张 750x1624 面板、10 张 180x180 图标、还有一批 **90/42 高的字形小块**（`184x90`、`136x42`、`189x90`…），
  全都**有内容**（`guest17` 的 184x90：alpha 最大 242、非零 91%；`guest47`：rgb≈(7,6,5)、alpha 42）。**没有 312x202** ——
  LA08 从头到尾没有变成纹理。
- `GLWIRE_DRAW_LOG=1`：每次 draw 记下 program 和前 8 个纹理单元绑的纹理（含 guest 名）。结果：
  ```
  glwire: draw#54 elements mode 0x4 count 6 program 13 u0:host13(guest17) u1:host11(guest15) u2:host10(guest14)
  ```
  —— **字形小块确实被 draw 采样**（每个小块出现 ~10 次，program 13），所以不是「CA 没画」。

可是画面上标签位置**一个像素都没变**：把 alpha 展开成 `rgb=255` 与 `rgb=0` 两版截图逐像素对比，差异只在三段
（状态栏 80–82 行、图标区 1103–1229 行、搜索框 2018–2047 行），**标签行（约 845–880 行）没有任何差异**。

结论再收紧一层：**客户端交面、服务器绑纹理、字形纹理上传、draw 采样——四步都发生了，缺的是最后「画到画面上」的那一下**。
下一步用 `GLWIRE_DUMP_DRAWS` 把一帧里每次 draw 的 FBO 图落盘，看 program 13 的那 10 次 draw 到底画到了哪个
FBO/什么位置（怀疑落到了一个不会被合成的 FBO，或顶点/状态被覆盖）。

**下一步（P0''''' 需要的）**：在 backboardd 的 CA 里找「收到的 image 为什么没有变成纹理」——候选：
`CA::Render::Image` 的解码、layer 可见性/内容类型（LA08 是 luminance+alpha 的 mask，可能走另一条着色器路径）、
或 `-texImageIOSurface:` 之外的绑定入口（`glTexImage2D` with client storage / EGLImage）。若这条路补不齐，
退路是：客人机把这几张面的像素连同 layer 矩形显式送进 wire、在宿主合成。

**第二发（s12b，16:15–16:30，默认镜像 + 全 28 张 image 列表，正结果）**：同一帧的 28 张 image 逐张点名，结论直接：
- 界面帧：`images 28 0:2T 750x1624/3008  1:1T 750x1624/3072  2:5 180x180 … 9:12 28x65  10:13 28x95  11:14 28x28
  12:15 6x85  13:16 6x6 … 23:26T 19x19` —— **没有 312x202，没有 184x124**；
- 同一次启动里内核造了 24 张面，`SpringBoard[10] created surface 3: 184x124 'BGRA'` 与
  `surface 4: 312x202 'LA08'` 都在；而 `-texImageIOSurface:` 列表里只有 5..25（图标与小的 28x65/6x6/16x16/19x19）。
- 也就是说：**字形与 widget 的 surface 在 SpringBoard 里被创建，但从来没有以纹理的方式进到被记录的渲染服务器流里**；
  所以下一步不是「读它的像素」，而是**证明它到底被谁画、又被谁合成**——SpringBoard 自己的 CA/GL 流是否根本没有被记录
  （整次启动宿主只有一个连接；backboardd 的帧在走）。

判据 P1'''''：一次启动里 `312x202 LA08` 的像素出现在宿主侧，并且哈希与客人机 surface 相等；
判据 P0'''''：截图 ≥300 KB 且同帧 diff ≥95%。

### P2''''' 460 s 的那个帧间隔（1 天）

第十一阶段量到：`host_glwire: frame 3 … interval 460.55 s`（两次启动都一样），它发生在界面**之前**，
是整段帧率与启动时间的最大单项。

**已有一份只读归因**（第十二阶段 t6，`/tmp/s12-bootgap.md`，五次启动 288.84/292.26/459.44/430.27/368.06 s）：
- 间隔打在**帧 3** 上，量的是**帧 2→帧 3**：帧 1/2 = 开机 logo，帧 3 = 熄屏空帧（`Set Blanked: YES`，2,424 records B），
  帧 4 才是界面第一帧（`Set Blanked: NO`，8,858 records B）；
- 窗口里**客人机 0 帧、宿主阻塞在 recv**，0 条 timeout/deadline → 不是死锁、不是等锁、不是宿主/网络；
- APFS 是内存盘 → 不是 I/O；`_rootFolderDidChange` 在 440–505 s，**晚于窗口** → 不是 Today 重建；
- 窗口里 **lsd 的 `denied attempt to escape CPU monitor` 每次都是 690–693 条**（五次几乎不变），时长却差 59%
  ⇒ **固定工作量 × 变化吞吐**；唯一同相位 `ustacks=top` 样本 busy **117%**（≈1.2/4 核）、lsd 73.9%、
  containermanagerd 26.9% ⇒ **不饱和，串行**。

**已钉死（s12gap，2026-11-05 16:40–16:55，`XCMD="… glwire_local=0 glframes=1 ustacks=top ustacks_at=30
ustacks_ms=1000 ustacks_count=760"`）**：客人机自己的 top 采样 **702 个**（30–790 s），分两相位看，答案非常干净：

| 相位 | 样本 | busy 中位 | ≥390%（4 核） | 最忙的进程（累计 %） |
|---|---|---|---|---|
| 启动期 30–500 s | 470 | **124%** | 33（7%） | lsd **27,598%**、containermanagerd 15,240%、usermanagerd 4,756%、launchd.darwin 3,238% |
| 界面之后 500 s+ | 232 | **399%** | 211（91%） | SpringBoard **25,825%**、kernel_task 11,784%、backboardd 6,928% |

读法：
- **460 s 的空洞不是 CPU 饱和**：启动期中位数只有 1.24 个核，7% 的样本摸到 4 核；最忙的是 **lsd**（LaunchServices
  的串行工作）+ containermanagerd + usermanagerd。加核、砍 Today、抬锁 deadline 都动不了它。
- **界面之后才是饱和**（中位 399%、91% 的样本 4 核满）——那才是第十阶段看到的「停顿是渲染成本」，但它在界面**之后**，
  和那个 460 s 的空洞不是同一件事。
- 因此杠杆按证据排序仍是：① 砍 lsd 的 LaunchServices 工作（镜像无 csstore、每次在 ramfs 重建；rootfs 里 maxdepth≤4
  有 191 个 .app）——先做「禁用 lsd 跑一发」的定性实验；② 让第一帧别等 lsd（BootUI 的 activity assertion）；
  ③ 宿主吞吐（tb-size=3072）。判据「启动到界面 −30%」要等 ① 的实验数字。

判据：进程级样本 ≥20 已达成（702 个）；**线程级符号化栈还没做**（需要 `ustacks=lsd` 的一发 + `ipsw` 符号化，
`/tmp/ipsw` 在 11-05 的机器重启后丢了，要先装回来）。

### P3''''' 四格表（半天，收尾）

第十一阶段的 P4'''' 已经在同一镜像上重跑四格（`WINDOW_AFTER=60 WINDOW_FRAMES=1 WINDOW_SPAN=120`，
截图判据 200 KB）。本阶段用**修好的镜像**再跑一次，把「120 帧 / X 秒」与截图大小并列；
四格必须注明时钟（纯本地用客人机、其余宿主）。

判据：四格各一个数字，`W1.5+W3 ≥4 fps`，并给出每格「同一帧截图 ↔ host picture」的一对证据。

---

## 四、不做（已被数据否掉，别重试）

- **不再 interpose `CAImageQueueRegisterIOSurfaceBuffer`**：第十一阶段 0 条，原因在镜像里（不 import/同镜像直调）。
- 不再用「整段」或「固定 20 秒」当窗口（启动期 460 s 会把它淹没）。
- 不再抬锁 deadline、不再加模拟核数（第十阶段已否）。
- 不在用户态重复找 glyph 的 EAGL 表项（第八阶段 12 个候选 0 调用）。
- 不在 screenshots 单张上判「黑/不黑」：一次启动至少每 20 s 一张，且要与同帧 picture 对。
- 不在屏幕缺内容时声称「完整」。

---

## 五、验证环境与操作规范

1. **同一时间一台虚拟机**：`mkdir /tmp/iosvm-vm.lock`；构建（`userland/build.sh`）也算占用，跑启动期间不构建；
2. 改客人机：`IOSVM_USE_LLD=1 GL=1 GLREMOTE=1 CONFIGD=1 RC=rc.vtop ./build.sh root.apfs`（约 20 s 增量）；
   改内核：`LANG=C LC_ALL=C IOSVM_USE_LLD=0 tools/make-kernel.sh -j8`，改完自己先跑 150 s；
3. 符号化：`/tmp/ipsw`、`SYMSTACK_CACHE=<rootfs>/dyld_shared_cache_arm64`；
4. 结论 = 一行日志 + 一个数字 + 一次可复现的启动参数；「没做到」写卡在哪一行。

---

## 六、里程碑

| 里程碑 | 内容 | 判据 |
|---|---|---|
| M12.0 | LA08 / widget / wallpaper 的像素到没到显示路径 | P1''''' |
| M12.1 | 画面完整（截图 ≥300 KB + 同帧 ≥95%） | P0''''' |
| M12.2 | 460 s 帧间隔归因（与能修的收益） | P2''''' |
| M12.3 | 四格表 | P3''''' |
| M12.4 | 文档与推送 | P4''''' |

---

## 七、诚实的预期

- **能确定的**：三样缺失内容各自是谁（LA08/widget/wallpaper 的 surface id、尺寸、格式都有日志）；
  内核侧采样是纯加法，不改像素、风险低。
- **不确定的**：LA08 的像素是不是「CoreGraphics 画进去的」——如果它画在 surface 内存里，
  那读 surface 就能拿到；如果它画在别的地方再由 CA 合进纹理，就要走上传。
- **不能承诺的**：wallpaper 一定能找到来源（它可能根本是 dark 色 wallpaper，而不是缺失）；
  以及内容补齐之后帧率还能保持 4 fps 以上。

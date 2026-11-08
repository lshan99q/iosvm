# Stage 19 P3 widget：拉面设计与风险评估（草稿）

状态：**调研稿，未改跨进程代码，未 boot。** 结论是「有条件可行」，但有一个关键约束先写在
最前面：**GL 记录器只在 backboardd 里有**（`inject.c` 的 `opengl_es()` 只在
`getprogname()=="backboardd"` 时 `dlopen("/redwood/lib/libhakios.gl.dylib")`；SpringBoard 的
`libhakios.sb.dylib` 只 `access()` 这个文件，不加载——stage 17 P3 表第 1 行的证据）。
所以 `eagl_remote_upload()` 不能在 SpringBoard 里调；字段采集在 SpringBoard，像素上传必须在
backboardd。两天内最稳的第一步不是跨进程管道，而是 **boot-arg 指定 surface ID + backboardd
轮询拉面**；XPC/共享内存是第二步。

## 1. 已知事实与已被否掉的路径

### 1.1 像素从哪来

- 内核日志已经点名（stage 10/11）：两张面都是 **SpringBoard** 创建：
  - `MiniIOSurface: SpringBoard[10] created surface 3: 184x124 'BGRA', 736 bytes per row`
  - `MiniIOSurface: SpringBoard[10] created surface 4: 312x202 'LA08', 640 bytes per row`
- 它们从未出现在 `-texImageIOSurface:` / `-attachImage:` 的记录里，所以 replay 流里
  没有它们的纹理——widget 只有白色圆角块、没有内容，是必然的。
- stage 17 已核对：GL 库只在 backboardd 加载，SpringBoard 只有 `libhakios.sb.dylib`；
  `eagl_remote_upload()`（`userland/src/eagl_remote.c:595`）的 `remote_enabled`/socket
  是 backboardd 的 EAGL 安装时初始化的。

### 1.2 现有 `inject_ciq_probe.c` 钩子为什么不能当主路径

- 只挂 `IOSurfaceCreate` 与 `CAImageQueueRegisterIOSurfaceBuffer` 两个 C 符号
  （`inject_ciq_probe.c:286-287`）。
- stage 11 实测两个进程都 `probe loaded`，但 **0 条 create/register 行**：
  QuartzCore 对 register 是 same-image 直接 `BL`，interpose 改不到；`__interpose` 又晚于
  shared cache 的 chained fixups（`stage11-results.md` §8）。
- stage 12 改用 GOT 重绑：重绑计数有了（SpringBoard 37 slots、backboardd 17 slots），
  但那一发没到界面（0 create、0 帧、截图 14,316 B），`CIQ=1` 不进默认镜像
  （`stage12-plan.md` 负结果段）。
- 结论：**不要在 C 钩子上继续加码**；它们只能当诊断。

### 1.3 可用的现成机制

- `inject_sb.c` 默认注入 SpringBoard（`build.sh` INJECT），已有：
  - CALayer 树遍历：`layer_line()`（`inject_sb.c:1046-1174`）打印
    `class/name/frame/opacity/contents description`；`layers()`（`1210-1243`）从
    `UIWindow` root layer 往下走；
  - Darwin notification `com.hakios.layers` 触发（`1587`、`1729`），
    可用 `rwtouch notify com.hakios.layers`；在 main queue 上跑。
  - `surfprobe:`（`1452-1560`）：按 boot-arg `surfsample=<id>` 用 `IOSurfaceLookup(id)`
    拿 base/bpr/尺寸，读像素并打哈希、非零比例——现成的「按 ID 读面」入口。
- **backboardd** 里 `libhakios.dylib`（`inject.c`）已 `dlopen` 了 GL 库；
  `eagl_remote_upload()` 可以 `dlsym(RTLD_DEFAULT, ...)`（或由 `inject.c` 暴露一个
  小包装），它内部走 opcode 201 + `wire_image()`。
- `remote_vtop()`（syscall 556）把用户地址变成物理段表（`eagl_remote.c:108-121`；
  `wire.h:157-195`）；宿主 `wire_take_image()` 用 `ram.img` 物理基址拷成连续像素
  （`tools/glwire/wire.h:402-465`）。

## 2. `{ID,w,h,stride,format,屏幕矩形}` 的具体来源

| 字段 | SpringBoard（采集侧） | backboardd（上传侧） | 风险 |
|---|---|---|---|
| `ID` | `IOSurfaceGetID(contents)`，`contents = CALayer.contents` 且 `CFGetTypeID(contents)==IOSurfaceGetTypeID()`；或 `surfsample` 的 boot-arg ID | `IOSurfaceLookup(id)`（跨进程，ID 是全局的） | `contents` 可能不是 IOSurface（CAImageQueue/远程 context）；ID 每 boot 可能变 |
| `w,h` | `IOSurfaceGetWidth/Height` | `IOSurfaceLookup` 后同 API | 与 layer 的 `bounds` 不一定同尺寸（content scale） |
| `stride` | `IOSurfaceGetBytesPerRow` | 同 API | LA08 不是 4 字节/像素：312×202 bpr 640 ≈ 2 字节/像素 + padding |
| `format` | `IOSurfaceGetPixelFormat`（四字符码） | 同 API | LA08 要展开成 RGBA；BGRA 直接 |
| `屏幕矩形` | 同一次 layer 树遍历：`layer.frame` + 逐级 superlayer 偏移，或 `-convertRect:toLayer:nil`，再乘 `contentsScale`；MVP 假定无 transform | 由 SpringBoard 传过来（或 boot-arg 固定值） | 变换/锚点/缩放会偏；widget 可能在 `CALayerHost`/远程 context 里 |

**rect 的现实情况**：layer 树能给出 frame，但「widget 内容层」是否就是
`contents==IOSurface` 的层，必须先用 D1-1 的 dump 验证；拿不到时退回到固定槽位
（Weather 左上、Calendar 右上）或 boot-arg 手工 rect。

## 3. 跨进程到 backboardd：需要吗、怎么传、风险

**需要**，因为上传侧在 backboardd，而 surface/layer 元数据在 SpringBoard。
但 MVP 可以完全绕开运行时管道：

### 3.1 最小可行（推荐先做）：boot-arg 固定 ID + 轮询

- 启动参数：`widget_surface=3,4`（或 `surfsample=3 surfsample_b=4` 已有的那套），
  `widget_rect=...` 可选。
- backboardd 侧（`inject.c` 或 `inject_ciq_probe.c` 的 backboardd 分支，默认关闭）：
  每 N 帧 `IOSurfaceLookup(id)`，比较 `IOSurfaceGetSeed`/哈希是否变化，
  有变化就 `eagl_remote_upload(0xFFFF1000+id, GL_TEXTURE_2D, internal, w,h,format,type,
  base,bpr,bytesPerElement)`；矩形先用 boot-arg/固定槽位。
- 好处：不改 SpringBoard、不动 XPC、不上 ObjC swizzle，直接验证「面能不能作为纹理进宿主」。
- 风险：ID 每 boot 可能变（要能从内核 `created surface` 行对表）；rect 手工；更新延迟
  最多 N 帧；适合时间盒 spike，不是产品方案。

### 3.2 完整路径：SpringBoard 层树 → 共享内存/Darwin notification → backboardd

- SpringBoard 在 `com.hakios.layers` 通知里遍历 layer 树，记录
  `{id,w,h,bpr,format,rect}`，写进一个小 ring（POSIX shm，或一个专用 IOSurface 当元数据页；
  `shm_test.c` 已证明 mach 共享内存这条路在本镜像可用），然后 `notify_post("com.hakios.widget")`。
- backboardd 收到通知后读 ring，`IOSurfaceLookup(id)` + `eagl_remote_upload`；
  rect 随记录走。
- 不选 XPC 的原因：没有现成 service/entitlement；boot 早期服务未必起；
  序列化/超时/重启都会让 widget 断流；像素本来就在 IOSurface（共享内存）里，
  不需要再拷一次。
- 风险：SpringBoard 与 backboardd 都要改；ring 的生命周期/重启同步；
  Darwin notification 没有 payload，必须配共享内存；layer 遍历必须 main queue、
  不能在 CA commit 中重入。

### 3.3 XPC 路径（不推荐）

只有在「backboardd 的 CA 合成树里能拿到 widget 层、而 SpringBoard 拿不到」时才考虑；
需要私有 SkyLight/CAWindowServer 接口，符号多被剥掉（stage 10），两天窗口内不碰。

## 4. 宿主按 surface ID 拉取的落点

现有链路（可复用）：

- guest `wire_image()` 对 `remote_vtop()` 的段表编码 kind 1：
  `{n,(pa,len,0)...}`；host `wire_take_image()` 从 `ram.img` 映射区拷成连续 buffer
  （`tools/glwire/wire.h:402-465`）。
- opcode 201：`{texture,target,internal,w,h,format,type,row_bytes,pixel_bytes}` +
  `wire_image()`；host case 201 调 `names_texture_upload()`（`dispatch.inc:690-703`）。

P3 要补的只有「矩形」与「不依赖 replay 的合成」：

1. 新 opcode 202（或 201 后跟 203 元数据）：载荷在 201 字段后加 `rect[4]`，
   仍用 `wire_image()` 段表；texture 用保留名 `0xFFFF1000+surfaceID`，避免和 guest
   GL 名字冲突。
2. host case 202：`wire_take_image()` → 按 format 展开（LA08→RGBA；BGRA 直接）→
   `glTexImage2D` 建宿主纹理 → 记入 `{host texture, rect}` 表。
3. replay 结束、`glReadPixels` 前：对每个 widget 保存 viewport/scissor/blend，
   在 `client->framebuffer` 上以 rect 画带 alpha 的纹理四边形，再恢复状态；
   处理 layer points→750×1624 像素的缩放与 y 翻转，MVP 假定无 transform。
4. 段表覆盖不全时 `wire_image` 退回 inline：184×124 BGRA ≈ 91 KB、
   312×202 BGRA ≈ 252 KB；widget 约 1 Hz 更新，inline 可接受。
5. LA08 的 `pixel_bytes` 是 2（312×2=624，行 640 是对齐 padding），
   `row_bytes` 必须传 `IOSurfaceGetBytesPerRow`，不能传 `width*4`。

## 5. 时间盒 2 天的最小步骤

| 步 | 内容 | 产出/判据 |
|---|---|---|
| D1-1 | 默认关闭的 layer-surface dump：在 SpringBoard 的 `com.hakios.layers` 里遍历层树，打印 `{class,name,frame,contents type,id,w,h,bpr,format}` | 日志能看到 widget 层 `contents` 是 IOSurface，`{id,w,h,bpr,format,frame}` 齐全；否则转「需要更多信息」 |
| D1-2 | backboardd 侧最小 uploader：boot-arg `widget_surface=3,4` + `IOSurfaceLookup` + `dlsym("eagl_remote_upload")`，按 seed/哈希变化上传（先不带 rect） | host log 出现 `upload texture 4294901xxx 184x124/312x202`，像素非全零，哈希随 widget 更新变化 |
| D2-1 | 加 rect（先 boot-arg 固定值，再换 D1-1 的 layer frame）与 host opcode 202 合成器 | `f-picture` widget 区域出现内容；与 guest panel 同区域 diff 改善 |
| D2-2 | 收结论：可行 / 不可行 / 需要更多信息；写清 rect 的变换/缩放风险 | 一份带证据的结论，不硬做 |

### 可行 / 不可行 / 需要更多信息

- **可行**：一次 boot 内能读到命中 surface 的 `{id,w,h,bpr,format,rect}`；
  backboardd 能 `IOSurfaceLookup` 并上传；host 收到真像素且随 widget 更新变化；
  按 rect 合成后 widget 区域不再是白色占位。
- **不可行**：widget 内容只在 CA 合成树里、SpringBoard/backboardd 都没有稳定的
  `{surface,layer}` 入口；或任何 hook 再次让启动不到界面。
- **需要更多信息**：surface 能拿到、像素能上传，但 rect/缩放不可靠，或 ID 每 boot
  不稳定且无法对表；这时先交付「面作为纹理进宿主」的中间结论。

## 6. 风险清单

1. **再次拖垮启动链**：stage 12 GOT 版让 boot 不到界面；P3 默认路径不要改
   `__DATA_CONST/GOT`，优先 boot-arg + 轮询或 ObjC 只读遍历；所有新分支默认关闭。
2. **上传进程错**：SpringBoard 没有 GL 记录器（stage 17 证据）；不要在 SpringBoard
   调 `eagl_remote_upload`，否则静默无记录。
3. **主线程安全**：layer 树遍历和 `IOSurfaceGetBaseAddress` 在 main queue 上做；
   MVP 只在 UI 稳定后低频触发。
4. **contents 不是 IOSurface**：widget 可能由 `CALayerHost`/远程 context 提供；
   D1-1 先确认。
5. **矩形口径**：points/pixels、`contentsScale`、`anchorPoint`、transform；MVP 只做
   axis-aligned，偏了记录偏差而不是硬调。
6. **格式**：LA08 是 2 字节/像素（bpr 640 = 312×2 + 对齐），BGRA 是 4；
   按 `IOSurfaceGetBytesPerElement`/bpr 行事，不要一律 `width*4`。
7. **段表上限**：`wire_image` 最多 2048 段；碎片化时退回 inline，1 Hz 下可接受。
8. **ID/生命周期**：surface ID 可能每 boot 变；widget 更新/消失要清旧内容，避免残留。
9. **XPC 路径**：私有服务/entitlement、boot 早期未起、backboardd 重启、主线程超时；
   两天窗口内不碰。

## 7. 目前能给的结论

**有条件可行**。最短路径是：

1. `inject_sb.c` 的 layer 树 dump 验证 `{ID,w,h,stride,format,rect}` 来源（默认关闭，
   一次 boot）；
2. backboardd 用 boot-arg 固定 ID 轮询 + `IOSurfaceLookup` + `eagl_remote_upload`
   先证明「面能作为纹理进宿主」（不依赖 XPC、不依赖 CIQ C 钩子）；
3. 再加 rect 与 host 合成，把 widget 内容贴回正确位置。

`inject_ciq_probe.c` 的两个 C 钩子保持诊断；XPC/backboardd 的完整元数据管道只在
layer 树拿不到 contents 时才需要，且优先共享内存+Darwin notification，不选 XPC。
两天内如果第 1 步就拿不到 `contents==IOSurface`，直接记「需要更多信息」，不要硬做。

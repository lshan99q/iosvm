# Metal → Vulkan 翻译：什么可行，本实验能证明什么

> 状态：M1 研究笔记，评审草稿，2026-11-21，分支 `experiment/metal-translation`。
> 本笔记界定问题；它支撑的决策是
> [`docs/adr/ADR-013-metal-translation.md`](adr-013-metal-translation.md)，它安排的测量是
> [`docs/stage29-metal-translation.md`](stage29-metal-translation.md)。
> 中文译文，对应英文版 `docs/research/metal-translation-feasibility.md`；数字以英文版为准。

## 本笔记使用的标签

| 标签 | 含义 |
|---|---|
| **本仓库核实**（verified here） | 在本仓库、Guest 树的源码，或本机上的日志/记录中读到 |
| **上游实测**（measured upstream） | HakiOS 树记录的数字（他们自己的运行；在此读取，但未复跑） |
| **外部报告**（reported） | 外部项目自己的文档（给出 URL），在此未复跑 |
| **推测**（speculation） | 推理，已如此标注 |

本笔记中的数字没有一个是估算值。实验尚无数字之处写 `TBD (M5)`。

反引号中的路径，如 `docs/ru/metal.md` 或 `docs/air.md`，属于 **HakiOS 的 Guest 树**
（`/home/lshan/DSH-desktop/iosvm-work/hakios-xnu-kernel`）——本移植构建于其上，本笔记将其作为
一手来源读取；它们不是本仓库中的文件。

## 摘要

- **拦截接缝已经存在，而且就是正确的那一条。** 本移植有意发布一个空的
  `IOAcceleratorES` 服务，启动参数 `metal=N` 使它命名为
  `HakiosMetal`/`HakiosMetalDevice`；随后 Metal 自己加载
  `/System/Library/Extensions/<name>.bundle`，并实例化其中的 `_MTLDevice` 子类
  （HakiOS 内核及其 bundle 中上游实测；§1）。实验在 `MTLCommandBuffer commit` 处捕获调用——设计
  预期 CoreAnimation 提交一帧的边界——而不是在 draw 处。
- **CoreAnimation 在这里不使用 Metal，有两个独立原因，且都不是线路。** 第一，设备被有意不对外
  公布（于是 CA 回退到它的 OpenGL ES 渲染器）；第二，即便公布了设备，CA 的 Metal 渲染器也会停在
  一个缺失的 `.metallib` 上，而其背后是 AIR 编译器（上游实测；§2）。完整的 CA 覆盖是三个子系统——
  设备对象图、AIR→SPIR-V、呈现/IOSurface 路径——分布在六个串行关卡上（§2.5），在真正实现它们的
  系统里，每一关都以数千行计；而这里整条可用的 GL 路径是 3,685 行。
  **本实验不能运行 CA 的 Metal 渲染器，也不声称能。**
- **现成的东西里没有能把 Metal 图形翻译到 Vulkan 的。** 最接近的先例是 UniXNU 的
  `metal2vulkan`/AdrenoMetal（在 Guest 内做 Metal→Vulkan，Adreno 后端）；MoltenVK 是反方向；
  SPIRV-LLVM-Translator 偏 compute；gfxstream/vkd3d 只是形态类似，不能复用（§3）。本实验实现的
  最小子集——blit 与 present——是能跑通每一个接缝（发现、对象生命周期、线路、宿主 Vulkan 执行、
  回读、发布）的最小东西，且**零 shader 依赖**，也就是说，它把 M2 与 M3 隔离开。
- **传输是承载调用的 socket，画面则放在 Guest 内存里。** 第 23 阶段端到端实现了 virtio-gpu，测得
  帧率零提升（1.80 → 1.85 fps，+2.8%，在噪声范围内；每次 push 7.3–8.3 ms，约占 245 ms 一帧的
  3%）：管道并不能改变本移植的帧时间。实验的第一版仍然把 4.87 MB 的画面内联在 socket 上发送，
  测得 **1.94 fps**——每帧 490 ms——这是关于一条承载画面的线路的陈述，而不是关于翻译的陈述
  （§4）。最终版借用 glwire 的按引用技巧（`hakios_vtop` 物理段 + 在宿主上映射 QEMU 的 RAM
  文件）：socket 只承载调用和约 1.5 KB 的段信息，同一个客户端随后以 **58.3 fps 中位**运行，
  2,000/2,000 帧 CRC 校验通过（§6.6）。
- **本实验能证明什么：** Metal 自己的加载器能在该 Guest 中加载第三方设备；纯 Metal 客户端可以
  commit、wait 和 present；在 commit 处翻译并在宿主 Vulkan（物理设备 1）上执行，能逐字节复现
  客户端的画面（CRC-32）；翻译后的帧能在现有浏览器查看器中看到；整条宿主路径可以在没有 GPU 的
  lavapipe 上测试（§5.3）。
- **它不能证明什么：** 一帧 CoreAnimation；任何 shader 编译；任何 render encoder；IOSurface
  共享；`MTLSharedEvent`；多进程行为；已交付的 13.9 fps 有任何变化。它同样不能证明
  宿主 GPU 执行了拷贝：宿主上的 `memcpy` 也能通过同一个 CRC——这正是
  [`docs/phases.md`](../phases.md) 已为 vkbridge 探针记录过的教训（§6.3）。
- **按 spike 的结论词汇给出的判定**（[`CONTRIBUTING.md`](../../CONTRIBUTING.md)）：*机制*是
  **可行**的；*本实验内部让 CoreAnimation 走 Metal* 是**不可行**的；翻译路径能否最终超过
  Replay 基线则是**需要更多信息**，而这些信息需要 M3 和 M4，不是 M2（§5）。

## 1. 问题 1 —— 一条 Metal 调用可以在哪里被拦截，以及每种选择的代价

### 1.1 本移植现有的发现链

Metal 通过 IOKit 发现设备，而不是通过文件路径。这条链有五步（HakiOS 树对 Metal 306.6.4 及其
引用的内核源码的解读；解读来自他们，因此标为上游实测）：

1. `MTLRegisterDevices` 匹配 `IOServiceMatching("IOAcceleratorES")`。这个类名就是 GPU 发现的
   全部；没有第二种问法。
2. 对每个服务，`getMetalPluginClassForService` 读取 IORegistry 属性 `MetalPluginName`，并要求
   它是 `CFString`。
3. 它拼出 `/System/Library/Extensions` + 该名字 + `.bundle`，并用 `NSBundle bundleWithPath:`
   打开 bundle。
4. 它读取 `MetalPluginClassName`，取出该类（或 principal class），并**拒绝任何不是 Metal 私有
   `_MTLDevice` 子类的类**。
5. `[[class alloc] initWithAcceleratorPort:service]` 返回设备。随后 Metal 断言该类遵循
   `MTLDevice` 协议和私有的 `MTLDeviceSPI` 协议——这两个协议都不是 `_MTLDevice` 声明的，因此
   子类必须自己声明两者（上游实测，HakiOS `docs/metal-plan.md` 的「What steps 1 and 2 found」
   第 1–2 条）。

本移植的内核已经发布了一个名为 `sgx` 的 `IOAcceleratorES` 服务，**有意留空**：它自己的注释说，
这类服务存在是为了让 CoreAnimation 不必等上半分钟；而两个本可指向 Metal 或 GLES 驱动的属性被
有意缺失，于是两个渲染器都绕过它（上游实测，`docs/ru/metal.md` §2，其中引用了
`MiniIOMobileFramebuffer.cpp`）。`metal_test` 精确打印出该状态：

```
metal_test:   IOAcceleratorES "sgx": MetalPluginName (none), MetalPluginClassName (none)
```

加上启动参数 `metal=N` 后，该服务获得 `MetalPluginName = HakiosMetal` 和
`MetalPluginClassName = HakiosMetalDevice`，而 `/System/Library/Extensions/HakiosMetal.bundle`
处的 bundle（在实验的 root 镜像中是指向 `/redwood/lib/HakiosMetal.bundle` 的符号链接）提供该
类。`N` 控制**哪些进程能看到该设备**：`metal=1` 只给名为 `metal_test` 的程序，`metal=2` 给除
窗口服务器之外的所有进程，`metal=3` 给所有进程；bundle 旁边的
`/redwood/lib/HakiosMetal.clients` 文件可以指定更多程序，每行一个，`*` 表示全部（HakiOS 树；
`metal=N` 各档位为上游实测）。

对拦截方式的选择来说，有两点后果很重要：

- **设备列表每个进程只回答一次，之后不再回答。** `MTLCreateSystemDefaultDevice` 在
  `dispatch_once` 下构建设备列表并缓存；空列表就是静默的 `nil`，没有日志行，也没有错误。
  `MTLCopyAllDevices` 是 `API_UNAVAILABLE(darwin)`，在 16.5 的二进制中不存在（上游实测，
  `docs/ru/metal.md` §1）。一个在进程首次询问之前没有对外公布的设备，之后就无法再触及。
- **这道门是按进程生效的，而且必须一次做对。** 这就是实验用 clients 文件点名 `metal_frame`、
  而不是用 `metal=3` 的原因：已知把设备展示给窗口服务器会让它停在一个找不到的库上（§2.2），而
  展示给 `mediaserverd` 会让该进程结束并重启（上游实测，`docs/metal-plan.md` 的
  「What steps 1 and 2 found」第 7 条）。

### 1.2 五个接缝，以及每个的代价

| 接缝 | 调用在哪里被捕获 | 代价是什么 | 判定 |
|---|---|---|---|
| **A. 通过 `IOAcceleratorES` 的设备插件（入选）** | 在我们自己的 `_MTLDevice` 子类内部：客户端自己的 API 调用，例如 `commit` | 我们要实现的 Metal 对象图；内核无需改动（服务与启动参数已经存在）；不需要 Apple 文件；类必须继承 `_MTLDevice` 并声明 `MTLDevice`/`MTLDeviceSPI`；未知 selector 必须被吸收，而不是崩溃 | **入选**：这是 Metal 自己支持的接缝，按进程控制，而且是今天唯一能在 QEMU 中测试的一条 |
| B. 内核驱动（提供真正的加速器与 IOGPU 提交 ABI） | 内核 / IOKit | `IOGPU` 层是 36 个类、711 个 ObjC 方法，内核必须满足其 ABI（上游实测，`docs/ru/metal.md` §4）；还有内存、fence、提交；而且在 x86 宿主上没有 AGX 可提交 | 不属于本实验；ADR-002 的「不模拟 GPU」仍然成立 |
| C. 在用户空间插桩 Metal.framework（dyld interpose、swizzle `MTLCreateSystemDefaultDevice` / `_MTLDevice`） | 进程对 Metal 的调用 | 私有且对版本脆弱；Metal 位于 dyld 共享缓存中；没有像 IORegistry 那样干净的按进程控制；**而且事后仍然需要同一个对象图** | 相对 A 没有优势；充其量是诊断用的回退方案 |
| D. 翻译到 Guest 中现有的 GLES | 同一个 commit 点 | 同一个对象图**再加上** AIR→GLSL；GLES 无法表达 compute、参数缓冲区（argument buffers）以及许多其他东西；工作量严格大于 A | 否决（HakiOS 自己的 `docs/ru/metal.md` §4 对其系统得出了同样的结论） |
| E. 在宿主上把 GL 回放流翻译到 Vulkan | CoreAnimation 的 GL 调用 | 另一个项目（桌面 GL 4.5 → Vulkan 回放），而且它仍然不能运行 Metal 客户端；回答的是可移植性问题，不是 Metal 问题 | 不在本实验范围内；如果目标只是宿主侧 GPU 可移植性，它被记作更便宜的替代方案 |

### 1.3 为什么选插件，以及边界落在哪里

选择 A，是因为这条接缝是 **Metal 自己的**：加载器、bundle 路径、类检查和加速器端口都由系统定义，
因此既不靠猜，也不打补丁。本移植已经付过内核侧的成本（服务与启动参数已经存在）；剩下的成本是
对象图，而这正是 ADR-002 分配给 `guest/metal-plugin` 的 L3 工作
（[`ADR-002`](../adr/ADR-002-api-translation.md)）。

插件内部的翻译边界是 **`MTLCommandBuffer commit`**，不是 encoder，也不是 draw：

- 它是 Metal 客户端说「这一组已经完整且有序」的地方，也是 CA 渲染器提交一帧的地方——与 GL 路径
  今天使用的边界相同（`eagl.c` 在 `glFlush`/`glFinish` 处结束一帧，ADR-012 记录说这绝对*不是*
  CA 的 pass 边界；本设计通过记录整条命令缓冲区而不是 pass 来避开这个陷阱）；
- 它是具有明确完成语义的最小单位：`waitUntilCompleted` 阻塞到收到一个 ack，因此 v1 的 Guest
  循环是同步的，也没有凭空引入双缓冲；
- encoder 级翻译需要 pipeline state 的生命周期和 render pass 状态，而 v1 子集没有这些；draw 级
  则比我们能测试的任何同步都更细。

## 2. 问题 2 —— 为什么 CoreAnimation 今天不走 Metal 路径，以及完整覆盖需要什么

### 2.1 设备按设计不对外公布

在 `metal=0`（默认值）下，加速器服务没有 `MetalPluginName`，Metal 的第 2 步失败，
`MTLCreateSystemDefaultDevice()` 静默返回 `nil`，CoreAnimation 的渲染器走它的 OpenGL ES 路径。
本移植有意保持这一点：一个存在但不能绘制的 Metal 设备，比没有 Metal 设备更糟，因为 CA 一看到
Metal 就优先选它。§1.1 引用的内核注释就是该决策；`docs/state-of-the-port.md` 把启动参数的效果
记为「现在还加载不了任何东西，所以今天什么都不会变」。

### 2.2 设备展示出来后，CA 的 Metal 渲染器停在它的库上

CoreAnimation 按固定顺序询问：`CA::WindowServer::EAGLServer::renderer()` **先**尝试
`CAMetalContextCreate()` 和 `CA::OGL::new_metal_context()`，只有两者都拿不到渲染器时才走
`EAGLContext`/OpenGL ES 路径（上游实测，`docs/ru/metal.md` §3，附缓存地址）。在 `metal=3` 下，
渲染器拿到了设备，随后停在：

```
Assertion failed: (metallib_url != nil), function MetalContext, file ogl-metal.mm, line 3308.
```

它需要一个 `.metallib` 的 URL。HakiOS 的记录从两个方向界定了这一点，但**没有确定它到底想要哪个
文件**：记录说系统带有 `AppleMetalGLRenderer.bundle/default.metallib`（274 个 shader 模块），又说
`QuartzCore.framework` 有一个 `default.pipelinelib`，其中指向一个 `default.metallib`，而该文件
不在解包出的 16.5 镜像里——「CoreAnimation 从哪里取得这个 URL，目前还不知道」。无论如何，插件都
无法提供它：该库是 Apple 的，分发它会违反本仓库的第一条否决规则
（[`PROVENANCE.md`](../../PROVENANCE.md)，ADR-006）。

### 2.3 实测的 shader 墙

`.metallib` 是 AIR：容器中的普通 LLVM bitcode。Mesa 的 GPU 编译器接收 NIR，Vulkan 接收 SPIR-V；
本移植的宿主是 Vulkan。HakiOS 树朝 NIR 方向构建了前端，并在设备自身的系统库上进行了测量
（上游实测，`docs/air.md` 及其语料报告）：

| 阶段 | 模块数 | 占 456 |
|---|---:|---:|
| 读取容器 | 456 | 100% |
| 用他们自己的 bitcode 读取器读取，结果与 `llvm-dis` 逐字节相同 | 456 | 100% |
| 翻译为 NIR，并通过 `nir_validate` | 321 | 70% |
| 由 `ir3`（Adreno）编译 | 321 | 70% |
| 与 LLVM 的执行结果数值一致，并被绘制 | 255 | 56% |
| 在手机的 GPU 上运行 | 54 | 12% |

而这里真正重要的细分是：321 是语料的**全部入口点**（271 个 fragment shader、47 个 compute
kernel、3 个 vertex shader）；另外 135 个模块是 `air.visible` 库函数，不是入口点。CA 渲染器
已知需要的那一个库 `AppleMetalGLRenderer.bundle/default.metallib`，是 456 个模块中的 274 个，
而**全部 274 个都能在该语料中翻译并编译**。

准确地说，这意味着：**AIR 可读、可翻译——翻译到 NIR，在 Adreno 上。** 这并不意味着
AIR→SPIR-V 已经存在。274 个模块被读取了；面向宿主 shader 语言的发射器是尚未构建的另一半，而
HakiOS 对「系统的 shader 需要什么」的成本估计是：针对一个 GPU 家族，数月、约 5–10 k 行
（上游实测，`docs/ru/metal.md` §9.4），并警告说他们的 NIR 路径不是 SPIR-V 路径。本移植自己的
宿主后端是 Vulkan，因此目标必须是 SPIR-V（§3）。

### 2.4 实测的对象图

| 项目 | 规模 | 来源 |
|---|---|---|
| `AGXMetalA11`——完整 Metal 实现所取代的驱动 | 2,578,880 B、5,798 个符号，**53 个类、1,158 个方法** | 上游实测，`docs/ru/metal.md` §4 |
| `IOGPU`——`AGX*` 与 `_MTLDevice` 之间的层，以及它所需的内核 ABI | 461,664 B，**36 个类、711 个方法** | 同上 |
| `_MTLDevice`——插件所继承的私有基类 | **549 个方法、36 个 ivar** | 同上 |
| `AGXA11FamilyDevice`——真实驱动覆盖了基类的多少 | 236 个方法 | 同上 |
| `MTLCompiler` + `AGXCompilerCore`——shader 前端与后端 | 983,544 B + 2,680,736 B | 同上 |
| 作为规模参照，本移植可用的 GL 路径花了多少 | `eagl.c` 3,685 行；409 个 EAGL 槽位中用了 62 个；其中 30 个是手写的；替换了 6 个 ObjC 方法 | 同上 |

要记住的数字不是作为目标的「1,158 个方法」——插件只实现其客户端会发送的东西——而是形状：一个
能到达主屏的可工作 GL 替代实现是 3,685 行、62 个入口点；系统自带客户端所触及的 Metal 对象图则是
另一个数量级，而 `_MTLDevice` 的 36 个私有 ivar 意味着，一个回答过多的子类可以在没有任何诊断的
情况下破坏 Apple 代码内部的内存（同一份笔记转述）。

### 2.5 完整覆盖 CoreAnimation 需要什么

串行关卡，每一关都必须先通过，下一关才可测试：

| 关卡 | 要求 | 状态 | 成本 |
|---|---|---|---|
| 1 | Metal 接受的设备（子类、协议、`initWithAcceleratorPort:`、命令队列） | **上游已完成**：在 QEMU 中设备 + 队列 + 缓冲区 + 库，并在 Mac 上检查过 | 数天到数周；本实验重新证明设备部分 |
| 2 | CA 的 Metal 渲染器找到它的库 | **受阻**：`metallib_url != nil`；该文件是 Apple 的，不在镜像中；它到底要哪个 URL 尚未确定 | 法务 + 工程；**不能分发该文件** |
| 3 | 从 AIR 到宿主目标的 shader 编译器 | **尚未为 SPIR-V 构建**；AIR→NIR 实测 456/321/255 | 数月，约 5–10 k 行（HakiOS 估计） |
| 4 | render encoder + pipeline state + 参数缓冲区 + depth/stencil | 已有 stub 类；什么都画不出来 | 数月 |
| 5 | 呈现路径：drawable、`IOSurface` 共享、`MTLSharedEvent`/fence、色彩管理（P3） | 尚未开始；本移植跨 VM 边界的 IOSurface 共享是 [`docs/phases.md`](../phases.md) 中的 M1 未知数 | 数月，而且 ABI 是私有的 |
| 6 | 足以超过 13.9 fps 的性能 | 未知 | 在 2–5 之前无法测量 |

**本问题的结论：** CA 不在 Metal 上，是因为它从未得到一个能绘制的设备；而一旦得到，第一个拦住
它的库是本移植无法合法分发的库。那个库背后的 shader 编译器是第二个。两个阻碍都不是发现接缝，
所以本实验仍能用我们自己的客户端端到端地测试该接缝。

### 2.6 实验改测什么

一个受控的 Metal 程序——`metal_frame`——它通过 `MTLCreateSystemDefaultDevice()` 创建设备、
一个队列、一个 4.87 MB 的缓冲区（750 × 1624 BGRA8）、一个纹理和一个内部 drawable（设备暴露
`-newDrawableWithTexture:`，因为这个用户空间没有 `CAMetalLayer` 来创建现成的 drawable）；用
确定性画面填充缓冲区；记录一次 `copyFromBuffer:…toTexture:` blit 和一次 `presentDrawable:`；
commit 并 wait；然后打印每帧耗时和一份摘要。它**呈 blit 形状**，这是有意为之：blit 是唯一一种
在 Vulkan 中有精确对应命令（`vkCmdCopyBufferToImage`）、结果可以逐字节检查、且不需要 shader 的
Metal 命令——因此失败只可能是接缝、传输、对象或同步的失败，而绝不会是一个不存在的编译器的
失败。

## 3. 问题 3 —— 在哪个翻译器上构建

### 3.1 调研

| 项目 | 方向 | 它是什么 | 这里能用吗？ |
|---|---|---|---|
| **UniXNU AdrenoMetal / metal2vulkan** | Metal → Vulkan，**在 Guest 内** | 一个 `_MTLDevice` 插件，加上一个运行时，通过内核的 GPU 接口把 Metal 对象/命令下沉到 Turnip（Mesa 的 Adreno Vulkan 驱动）；在小米 Pad 6S Pro 上合成系统 UI | **先例，未采用。** 它使用同一个接缝、同一套 API 级思路，且已达生产规模；它的后端是面向一个 GPU 家族的在 Guest 内 Vulkan 驱动，而不是通往宿主 V100 的线路，其运行时与本实验的最小化目标相反。据 HakiOS 笔记，许可证为 MIT（*外部报告*） |
| **MoltenVK** | Vulkan → Metal | 在 Metal 上分层实现 Vulkan 1.4 子集，带一个 SPIR-V→MSL 的 shader 转换器；Khronos 已弃用它、在 Apple Silicon 上改用 KosmicKrisp，现在由社区维护 | **不是我们的方向。** 它在这里的价值是一个警示性测量：*反向*翻译器是一个以年计、基金会规模的项目，而且它要把为翻译而设计的表示（SPIR-V）转换成它唯一能输出的 shader 语言（MSL） |
| **SPIRV-LLVM-Translator** | LLVM IR ↔ SPIR-V | Khronos 的双向翻译工具/库 | **部分可用，仅一个方向。** HakiOS 的调研将其解读为支持 SPIR-V 的 OpenCL/compute 变体，因此 `AIR → SPIR-V → spirv_to_nir` 对 vertex/fragment 不能直接奏效（*上游实测*，此处未复跑）。对 compute kernel 来说它是一个候选起点；对图形来说，SPIR-V 这一侧还得自己写 |
| **gfxstream** | Vulkan/GLES API 流式传输，Guest → 宿主 | 按入口点生成的编组、宿主 Vulkan/GLES 后端；与本实验线路*形态*最接近 | **不复用代码，但模型有用。** 它确认了架构：生成式编组 + 宿主侧 Vulkan + Guest 对象 id。它不是 Metal 前端 |
| **vkd3d**（Direct3D → Vulkan） | D3D12 → Vulkan | Wine 的翻译库，自带输出 SPIR-V 的 shader 编译器 | **类比对象，不复用。** 它表明 API 翻译层可行且有维护，而 shader 编译器是昂贵的那一半 |
| **Mesa 的 Dozen**（Vulkan → D3D12） | 反向 | 构建在 D3D12 上的 Vulkan 驱动 | 仅作类比 |
| **Naga、SPIRV-Cross、ANGLE、Apple Metal Shader Converter、HLSL→Metal** | 任何东西 → Metal/MSL | 成熟的「进入 Metal」或输出 MSL 的工具 | **不可用**：没有一个能读 AIR |
| **HakiOS AIR→NIR** | AIR → Mesa NIR | 上文实测过的前端；AIR 读取器、51 个操作、2D 纹理 | **不能作为宿主 Vulkan 的目标**（NIR 是 Mesa 内部的，而宿主不是 Mesa），但它是 M3 可复用的资产：其 AIR 读取器在 456/456 个模块上得到验证；M3 需要的是 SPIR-V 发射器，而不是另一个 bitcode 读取器 |
| **Asahi Linux** | — | 有意不做 AIR 读取器；NIR 后端 | 印证了还没有人交付过 AIR 前端 |

### 3.2 对调研的解读

两个事实决定了翻译器的选择：

1. **没有可采用的 Metal→Vulkan 图形翻译器。** 存在的那个（UniXNU 的）是一个完整对象图，面向
   一款移动 GPU 及其内核 ABI；采用它意味着采用它的 mesa fork 和后端，而且它仍然不能把帧放到本
   宿主的 Vulkan 设备上。MoltenVK 是反方向的箭头。在此用途上，LLVM 翻译器只偏向 compute。
2. **可以把 shader 问题推迟，而不伪造结果。** blit/present 客户端完全不需要 shader，而线路、
   对象生命周期、Vulkan 执行以及回读/发布这些接缝，对 blit 与对一帧具有相同的形态——同时 CA
   特有的呈现路径（IOSurface、真正的 drawable）正是 v1 不声称覆盖的部分。因此实验可以现在就
   测试接缝，而 shader 前端保持为一个独立的关卡（M3），带有自己的终止判据，而不是被偷偷塞进来
   当作「尚未实现」。

### 3.3 最小子集，以及它为什么是最小值而不是捷径

v1 实现：设备、命令队列、命令缓冲区、带一次 buffer→texture 拷贝的
`MTLBlitCommandEncoder`、缓冲区（宿主可见的暂存）、纹理（optimal tiling、BGRA8/RGBA8）、
一个内部 drawable、`presentDrawable:`、`commit`、`waitUntilCompleted`，以及围绕它们的对象
生命周期。线路恰好承载这些操作（`MW_CREATE_BUFFER`、`MW_UPLOAD_BUFFER`、
`MW_CREATE_TEXTURE`、`MW_COPY_BUFFER_TO_TEXTURE`、`MW_PRESENT`、`MW_FRAME_BEGIN/END`），
宿主在真正的 Vulkan 设备上执行拷贝、回读结果、验证 CRC-32，并以 glwire 的共享内存格式发布画面。

为什么是这些而不更多：每增加一个操作（render encoder、pipeline state、库、draw）都会带来 AIR
依赖或私有路径；而每减少一个 buffer→texture 拷贝之下的操作，就会留下一个未测试的接缝（没有纹理
就没有 `VkImage` 生命周期；没有 present 就没有发布；没有 wait 就没有确认）。一次拷贝就覆盖了
所有这些。为什么不再少：宿主侧的 `memcpy` 在没有 Vulkan 的情况下也能通过，而这正是
[`docs/phases.md`](../phases.md) 为 vkbridge 探针记录的失败模式——因此本实验必须把它点名为
局限，而不是隐藏（§6.3）。

## 4. 问题 4 —— 传输：glwire 的 socket 还是 virtio-gpu

### 4.1 socket 是什么

TCP，一条连接，Guest → 宿主 `10.0.2.2:9111`（QEMU 用户态网络把它映射到宿主回环），每提交一帧
一个确认，形态与 glwire 相同：Guest 记录，宿主执行并回答一次。在宿主上，画面以 glwire 的格式
发布到 `/dev/shm/metalwire-frame`——magic `GLFW`、序号、尺寸、`CLOCK_MONOTONIC` 时间戳、BGRA
字节——因此 `tools/glwire/webview.py --shm` 能**不加修改**地提供翻译后的帧（本仓库核实，
`host/metalwire/PROTOCOL.md` 与 `metalwire_protocol.h`）。

### 4.2 virtio-gpu：第 23 阶段的测量

virtio-gpu 已端到端实现（设备发现、特性协商、控制队列、`GET_DISPLAY_INFO`、
`RESOURCE_CREATE_2D`、298 页的 `ATTACH_BACKING`、`SET_SCANOUT`、`TRANSFER_TO_HOST_2D`、
`RESOURCE_FLUSH`；1,001 次 push，零失败），并在一次 124.6 s 滚动上做了同 root 的 A/B 测量：

| | A：TCP/线路 | B：+ GPU push |
|---|---:|---:|
| 滚动帧数 | 224 | 230 |
| **滚动 fps** | **1.80** | **1.85** |
| push 成本 | — | 584 次 push，每次 7.3–8.3 ms |

+2.8%，在噪声范围内：**传输不是瓶颈**（`docs/stage23-verdict.md`，本仓库核实）。Guest 的耗时
普查解释了这一点：在约 245 ms 的一帧中，CoreAnimation 自身的工作约 184 ms，回读 60 ms，flush
0.6 ms，而 Guest **等待传输的时间是 5 µs**。

### 4.3 这暗示什么、不暗示什么

它暗示：

- 更快或更直接的传输无法提高回放路径的帧率，因为帧时间是 Guest 的 CPU，而不是线路时间。同样的
  论证适用于翻译路径：把命令从 TCP 改走 virtio-gpu 不会让翻译变便宜；**收益必须来自消除 Guest
  的工作**（CA 的绘制工作会移到宿主），而任何传输改动都移不走它。
- 因此，在一个问题在于翻译的实验中，传输应当按*可测试性与故障隔离*来选择，而不是按速度。

它**不**暗示：

- 并不表示 TCP 加内联画面对于生产级翻译路径来说足够快。第一版每一帧都把 **4.87 MB 的画面**放在 socket
  上运送，测得 1.94 fps（`trans2`，每帧 490 ms）：在 TCG 下，线路上的画面就是帧时间。最终版像
  Replay 路径那样按引用传递（`hakios_vtop` + 宿主对 QEMU RAM 文件的映射），socket 只留给调用
  （每帧约 1.5 KB）；这是 1.94 fps 那次运行与 §6.6 的 58.3 fps 中位之间唯一的改动。教训不是
  「socket 慢」，而是「画面不该放在 socket 上」——而且必须实测过才能这么说。
- 并不表示 virtio-gpu 是白费功夫。它是 3D 路径（ADR-004）的基础，也是未来零拷贝画面路径的所在。它只是
  不是本实验的传输。

### 4.4 本实验的传输，狭义陈述

9111 上的一条 TCP 连接承载 Metal 调用，每帧一个原始确认（状态、耗时、CRC），以及——当缓冲区
常驻且内核响应 `hakios_vtop` 时——一串物理段而不是画面字节；宿主映射 QEMU 的 RAM 文件并读取
同一块内存（Replay 路径自己的技巧，`tools/glwire`）。SHM 发布是宿主本地的。这就是 glwire 的
形态，只是操作集更小；回退路径内联携带画面，它被保留并测量，没有被删除。virtio-gpu 仍然不是本
实验的传输，理由见 §4.3；生产路径会把同样的按引用方案放到一个具有正规 fence 与共享内存的设备上
（ADR-004），而这里的数字说明：传输选择只值得一个实验，而不值得整个架构。

## 5. 问题 5 —— 风险、终止判据，以及分阶段计划

### 5.1 风险

| id | 风险 | 可能性 | 影响 | 缓解 | 能显示它的测量 |
|---|---|---|---|---|---|
| R1 | **AIR→SPIR-V** 永远达不到可用的图形子集 | 高 | 阻断所有 CA 走 Metal 的路径（M3/M4） | 保持 M2 无 shader；M3 是独立的 spike，带一个单 shader 门槛 | `clearFragment01` 被翻译并画出匹配的像素 |
| R2 | CA 使用的 Metal 对象图比估计的更大（私有 `MTLDeviceSPI` 路径、render encoder、参数缓冲区） | 高 | 数月；可能无上限 | 不承诺 CA 覆盖；只在 M4 测量 CA 实际发送了什么 | CA 启动期间插件的「未回答 selector」日志 |
| R3 | CA 的渲染库是 Apple 的，无法分发；它想要哪个 URL 尚未解决 | 高 | M4 因约束而受阻，不是因工作量 | 把它记录为法务/产品问题，而不是工程问题 | `metallib_url` 断言以及任何替代库的失败 |
| R4 | 呈现/同步语义（`MTLSharedEvent`、drawable、IOSurface、fence）无法映射到无头回读 | 中 | 翻译后的 CA 帧无法通过现有浏览器路径呈现 | v1 使用内部 drawable 和回读；把真正的呈现路径当作 M4 工作 | Guest 期望的 fence 顺序与 ack 顺序之间的差异 |
| R5 | 翻译路径比回放路径慢 | 中 | 路径正确，但作为提速没有意义 | 诚实测量成本分离；比较只有在 M4 之后才有意义 | 同一负载区间内成对的 Guest 每帧 CPU 与挂钟时间 |
| R6 | 宿主争用：一块 V100（设备 1）与 GL 回放实验共享；挂钟时间随负载波动 ±30% | 确定 | 如果不配对，数字无法归因 | 独立的 QEMU 实例/端口/GPU/SHM；成对运行；报告负载 | 按负载分带的成对表格（§6.4） |
| R7 | 实验分支腐烂：`experiment/…` 不在文档化的分支前缀内，而 spike 的时间盒是 5 天 | 中 | 代码过时，归属不清 | M6 决策或删除；ADR 记录状态 | 分支年龄与 M6 决策的对比 |
| R8 | **正确性幻觉**：CRC 证明宿主读回了 Guest 的字节，而不是 GPU 执行了拷贝——宿主 `memcpy` 也能通过 | 确定（按构造必然） | 过度声称「在 GPU 上执行」 | 明确说出来；把设备侧判据（compute 校验和）列为未来工作，参照 `docs/phases.md` 中 vkbridge 的教训 | 把 Vulkan 调用替换掉、打过补丁的宿主仍然给出 `crc_ok=1` |
| R9 | 启动损坏：一个存在但不能绘制的设备会使 backboardd 和 mediaserverd 崩溃/重启 | 中 | 丢失启动，日志误导 | 保持 `metal=1` + clients 文件（UI 启动中绝不用 `metal=2/3`）；吸收未知 selector；绝不在 `waitUntilCompleted` 中挂起 | 把设备展示给 CA 的启动会像上游实测那样停在库断言上 |

### 5.2 终止判据

判据是停止，而不是提示「再试一天」（[`CONTRIBUTING.md`](../../CONTRIBUTING.md) 的 Spikes
节）。每一条都点名会触发它的证据。

| id | 如果发生这件事 | 那么就停止 / 这样做 |
|---|---|---|
| K1 | 宿主路径无法在 lavapipe 上以 `crc_ok=1` 执行一帧，或者 `--gpu 1` 无法发布并回读一帧 | 线路/执行器的形态错了；把失败写下来，不要绕过它打补丁 |
| K2 | 在 3 次使用 `metal=1` 且 clients 文件点名 `metal_probe` 的启动中，`metal_probe` 从 `MTLCreateSystemDefaultDevice()` 得到 `nil` | `_MTLDevice` 路线在 16.5 上走不通；M2 无法按设计继续 |
| K3 | 在设备展示给 CA 的情况下，不提供 Apple 文件就无法提供该库，且没有替代品能满足它 | M4 被约束阻断；记录下来，不要制造变通方案 |
| K4 | `clearFragment01`（四条 LLVM 指令，不含 `air.*`）无法在 M3 时间盒内被翻译并画出与 LLVM 一致的结果 | 图形方向的 AIR→SPIR-V 超出本项目预算；停止 M3 |
| K5 | 翻译后客户端的每帧成本（Guest 编码 + 线路 + 宿主执行与回读，不含它自己的画面生成）在相同画面尺寸下超过回放路径 75 ms 的帧预算 | 即便还未加上 CA 的工作，该机制也承载不了已交付的帧率；只把它当作正确性演示，绝不要当作速度声明 |
| K6 | 回放基线无法在本机同一负载区间内复现 | 不声明任何比较；M5 只报告机制 |
| K7 | 任何步骤需要 Apple 二进制、库或镜像 | 停止；做法务评估（与 [`docs/phases.md`](../phases.md) 中 D2 相同的规则） |

### 5.3 分阶段计划（M1–M6）

这些里程碑是本实验自己的；它们**不是** [`docs/phases.md`](../phases.md) 中的项目里程碑
M0–M4（项目的 M2「GPU 通道」是这项工作之上的父项）。

| # | 什么 | 交付物 | 关卡 | 在本实验中？ |
|---|---|---|---|---|
| **M1** | 可行性与决策（本笔记 + ADR-013） | `docs/research/metal-translation-feasibility.md`、`docs/adr/ADR-013-metal-translation.md`、`docs/stage29-metal-translation.md` | 报告回答六个问题，并点名它不能证明什么；由负责人决定是否构建 M2 | **是——就是本文** |
| **M2** | 最小的真实路径：插件、线路、宿主 Vulkan 执行器、浏览器中的帧 | `guest/metal-plugin/`、`host/metalwire/`、`tools/metalwire/` | lavapipe 测试全绿；GPU 1 帧回读且 `crc_ok=1`；画面在 `webview.py` 中可见；客户端在 Guest 中运行 | **是——进行中**（代码由其他角色负责） |
| **M3** | 针对一个系统 shader 的 AIR→SPIR-V | 一份 spike 报告，而不是 `main` 上的代码 | 来自真实 `.metallib` 的 `clearFragment01` 在宿主 GPU 上画出预期像素 | **否**——独立的时间盒 spike；本实验不得尝试 |
| **M4** | 让 CA 的 Metal 渲染器跑在翻译后的设备上 | 在库问题得到回答之前没有可交付物 | 一帧 CA 画面通过该设备上屏 | **否**——被 R1–R3 阻断；必须先有法务答案 |
| **M5** | 测量 | 结果填入 `docs/stage29-metal-translation.md` | 带 CRC 与逐字节一致性、按负载分带的成对运行，给数字不给形容词 | **部分**——协议与 CSV 字段已存在；数字来自负责人的运行（`TBD (M5)`） |
| **M6** | 决策 | 更新 ADR-013 状态（作为方向采纳 / 保留为实验 / 停止） | 审查 M2–M5 证据 | **否**——在 M5 之后 |

### 5.4 明确不在范围内

- CoreAnimation、`WindowServer`、SpringBoard 或任何走 Metal 的 Apple 客户端（M4，受阻）。
- AIR、`.metallib`、Metal 库、pipeline state、任何 shader（M3）。
- `MTLRenderCommandEncoder`、参数缓冲区、compute encoder、tessellation。
- `IOSurface`、`MTLSharedEvent`、跨进程共享、swapchain、真正的呈现路径。
- 对已交付移植的任何帧率声明；对 Replay 路径或其默认值的任何改动。
- 任何 Apple 二进制、任何下载的 Apple 材料、任何凭证（三条否决规则）。

### 5.5 决策表

| 选项 | 它能带来什么 | 代价是什么 | 决策 |
|---|---|---|---|
| 保留 Replay 路径的交付状态，什么都不改 | 13.9 fps、可工作的移植、无新风险 | 使 ADR-002 仍未证明、Metal 问题仍开放 | **基线，不变** |
| **构建 M2：插件 + 线路 + 宿主 Vulkan，合成客户端（入选）** | 机制的证明；可 CI 测试的宿主路径；CA 将来需要的接缝 | 两个角色数周；交付 fps 无提升；覆盖有限 | **做，作为实验** |
| 现在尝试 AIR→SPIR-V（M3） | 宿主 GPU 上的第一个真实 shader | 数月，一个独立的编译器项目；无论如何没有 M4 就没有 CA 帧 | **不在本实验中；只做 spike** |
| 现在尝试 CA 的 Metal 渲染器（M4） | 让 CA 走 Metal 的唯一路线 | 被库（法务）和编译器（M3）阻断 | **受阻；记录，不尝试** |
| 用 virtio-gpu 3D / Venus 作为传输 | 一条标准的 3D 路径 | 第 23 阶段测得传输零收益；另一个设备项目（ADR-004） | **不适用于本实验** |
| 模拟 AGX | Guest 未经修改地运行 Apple 驱动 | 指令集是秘密；以年计（ADR-002） | **否决** |
| 改为把 Metal 翻译到 GLES | 复用现有 GLES | 同一对象图 + AIR→GLSL，表达力更低（HakiOS §4） | **否决** |

## 6. 问题 6 —— 测量计划：这些数字将意味着什么

### 6.1 测什么，在哪里测

每帧，Guest 打印 `fill_us`（生成确定性画面）、`encode_us`（Metal 调用）、`commit_us`、
`wait_us`（`commit` → ack）和 `total_us`，以及一行摘要（帧数、ok、failed、fps、encode
均值/最小/最大、往返均值/最小/最大）。插件对它发送的画面字节计算 CRC-32，因此正确性由宿主的
`crc_ok` 报告，而不是由客户端报告。宿主每帧写一行 CSV：`seq`、`bytes`、`recv_us`（帧开始 →
帧结束到达）、`exec_us`、`gpu_us`、`readback_us`、`publish_us`、`total_us`、`crc_ok`，以及
滚动的 1 s `fps`；它还把帧保存为 PPM/PNG 并发布到 SHM（`host/metalwire/PROTOCOL.md`、
`metalwire_protocol.h`，以及 `host/metalwire/metalwire_host.c` 中的 CSV 表头）。Guest 的串口
日志是观察 Guest 的唯一窗口：每帧一行，一行摘要。

### 6.2 这些数字将意味着什么

- **正确性优先。** 每一帧都 `crc_ok=1`，再加上保存的帧与客户端本意发送的确定性画面之间的完整
  逐字节比较，就是门槛。只要有一次 CRC 不匹配，该次运行的性能数字就作废。
- **时间花在哪里。** 拆分说明帧归哪一侧所有：Guest 编码 vs Guest 等待 vs 线路（recv）vs 宿主
  执行/回读/发布。回放路径是先例：它每帧 75 ms 是 Guest CPU，等待是 1 µs，宿主回放工作约
  1.4 ms——因此如果翻译路径呈现同样的形状，结论就是**天花板是 Guest 的工作，而不是翻译**；
  翻译路径只有在能消除 Guest 工作时才有收益。
- **画面拷贝的成本，如果有的话。** 第一条线路每帧上传 4.87 MB，成本就是一帧（1.94 fps）；
  最终线路按引用传递画面，成本消失了（58.3 fps 中位）。两者都测过，且都在 §6.6。
- **只做成对运行。** 挂钟时间随宿主负载波动 ±30%（README 以及自第 23 阶段以来的每份阶段记录
  都这么说）。比较协议是固定的：一次只跑一个 QEMU；两侧画面尺寸相同；翻译运行与回放运行在同一
  负载区间内背靠背配对；取窗口中的中位数；每张表都报告平均负载；绝不单次取样。

### 6.3 它们不会意味着什么

- **不是 CA 的数字。** 客户端是合成的（见 6.5）。
- **不能证明 GPU 执行了拷贝。** `crc_ok=1` 证明宿主读回了 Guest 的字节。如果宿主把
  `vkCmdCopyBufferToImage`/`vkCmdCopyImageToBuffer` 换成 `memcpy`，也会通过同样的检查。
  [`docs/phases.md`](../phases.md) 为 vkbridge 探针记录了这个陷阱（「一次打过补丁的拷贝打印了
  8/8」），这里诚实的说法是一样的：结果正确，代码确实用了 Vulkan，但仅凭探针不能确立设备执行。
  设备侧判据（compute shader 校验和、一条宿主不可见的暂存路径）才是修法，是未来工作，不是 M2
  的声明。
- **不是传输判决。** 按引用路径是 Replay 路径自己的机制，只是用在另一个接缝上；这个数字说明该
  机制在这个接缝上可用，而不是说 TCP 在生产中够用。
- **不是对交付 fps 的声明。** 除非有什么改变它，已交付的移植仍保持 13.9 fps / 75 ms（单
  vCPU，交付配置）；本实验在那条路径上什么都没改。

### 6.4 成对测量的纪律（按 README 的表述）

> 挂钟时间数字随宿主负载波动 ±30%，因此要比较成对运行，绝不要单次取样。
> （[`README.md`](../../README.md)）

本实验额外规定：本机上一次只跑我们的一次运行（另一个实验继续运行，而它的负载正是平均负载要报告
的东西）；实验的 QEMU 实例（3 号，端口 4474/4475）绑定到自己的核心（默认 26–28）；回放基线重建
为单独的 root 镜像模式，并指向不同的回放端口（9210），使它永远无法联系到已交付的回放器；平均
负载随运行一起采样；以及按窗口中位数比较，而不是端点对端点比较。

### 6.5 直说的保留意见：客户端是合成的

测试客户端是**我们写的** Metal 程序：它创建设备、队列、缓冲区、纹理和一个内部 drawable，blit
一张确定性的 750 × 1624 BGRA8 画面并呈现。它不是 CoreAnimation，不是 SpringBoard，也不绘制
iOS UI。因此：

- 实验测量的是**机制**（发现、序列化、传输、宿主执行、发布），而**不能**测量渲染出的 UI、CA
  的命令组合、模糊/compute pass、色彩管理或交付帧率；
- 全绿的结果**不**意味着「Metal 对本移植可用了」；它意味着「本移植为 Metal 设备会使用的接缝
  是真的，而且它的宿主侧可用」；
- 这个客户端与 CoreAnimation 之间的差距，恰好是 M3（shader）和 M4（CA 的渲染器与呈现路径）——
  本实验有意不声称的两个关卡。

这也是阶段记录（[`docs/stage29-metal-translation.md`](stage29-metal-translation.md)）用自己的
话说客户端是合成的原因：这样后来的读者就不会把全绿的 M2 当作 CA 结果来引用。

### 6.6 实际测量了什么（M5，2026-11-21/22）

五次翻译运行，每次 400 帧，同一宿主，绑定一个 vCPU，GPU 1，750 × 1624 BGRA8，宿主为 32 CPU、
负载 1.2–2.0，宿主回读时每 8 行做一次校验和：

| | 实测 |
|---|---|
| 帧数 | 2,000 / 2,000 `crc_ok=1`，0 失败，0 次 panic |
| 中位 fps | **58.3**（范围 57.4–60.1） |
| Guest 每帧 | fill 2.6 ms、Metal 编码 0.19 ms、commit ≈ 7.1 ms、wait ≈ 13.7 ms |
| 宿主每帧 | 合计 4.2 ms（recv 6.1–6.4 ms 是 Guest 记录+发送的窗口；GPU 0.94 ms；回读+抽样 CRC 0.31 ms；发布 2.9 ms） |
| 线路 | socket 上 0 画面字节；每次运行按引用从 Guest 内存读取 1.95 GB |
| 画面 | 源与回读完整 `memcmp` 一致；保存的帧记录在阶段记录中 |
| 对照 | Replay 主屏，相同宿主画面尺寸，在同一次会话中测量（stage 29 §5.6） |

成本分离就是发现：Metal API 的工作是 0.2 ms，宿主的 Vulkan 工作是 4.2 ms，剩下的是 Guest 自己
的画面生成和实验用来证明正确性的校验和。朴素的画面走 socket 线路成本是 490 ms。翻译本身没有
任何部分是瓶颈。它仍然不能证明的东西见 §6.3 与 §6.5。

## 7. 工作区与分支隔离，以及一个不变的基线

本实验与正在运行的 GL 回放实验以及 HakiOS 的 Guest 树共享一台宿主，因此隔离是设计的一部分，
而不是日常杂务（本仓库核实，见 `tools/metalwire/*.sh`）：

| 项目 | 实验 | 本机上的其他工作 | 为什么 |
|---|---|---|---|
| 源码树 | git worktree `/home/lshan/DSH-desktop/iosvm-metal`，分支 `experiment/metal-translation` | 主检出 `iosvm` | 不切换分支，不打扰未提交的工作 |
| Guest 运行树 | `/home/lshan/DSH-desktop/mt-work`——不可变部分用符号链接（内核、加载器、roms、2.4 GB 系统镜像），会写入的脚本用副本，一个私有的 18 MB root 暂存 | HakiOS 树**只读**使用 | 我们的启动不能打扰另一个实验的树，他们的启动也不能打扰我们的 |
| QEMU | 3 号实例：monitor **4474**、QMP **4475** | 1 号实例：4454/4455 | 每个实验一个 QEMU；不共享 monitor/QMP |
| 宿主回放 | — | `host_glwire` 在 **9110** | 本实验不运行它；基线 Guest 改指 **9210**，因此 metal 启动永远无法到达已交付的回放器；该文件在本分支上唯一的改动是下面的可选 selector |
| 翻译器 | TCP **9111**；SHM `/dev/shm/metalwire-frame` | — | 端口 9111 和自己的 SHM 文件；`webview.py` 两者都能读 |
| GPU | Vulkan 物理设备 **1**——1 号槽的 V100；`--gpu 1` 是默认值，`METALWIRE_GPU` 可覆盖 | 设备 0 属于 GL 回放实验 | 每个 GPU 一个实验；宿主打印选中的设备，并拒绝未被指定的设备 |
| vCPU | 一个 vCPU，`QEMU_SMP=1`，绑定 `taskset -c 26-28`，`cpus=1 fb_beat=0 fbscale=2 metal=1` | 其他运行绑定到别处 | 可复现的单核 Guest 数字；`metal=1` 让设备远离 CA 和 mediaserverd |
| 提交 | 研究角色不提交；由负责人审查并提交 | — | 工作树保持脏状态供审查 |

**Replay 路径保留其交付行为，作为基线。** 交付的启动配置保留自己的 root 镜像和自己的端口
（9110）；用于比较的基线是一个单独的 root 镜像模式（`prepare-guest.sh replay`），它启动
launchd 与 glwire，但不启动 metalwire 客户端，并把它的 glremote 文件指向 9210。翻译路径是基线
旁边的附加物，而不是替代品。

写作本笔记期间做了一处自我修正：分支确实编辑了 `tools/glwire/` 下的一个文件
`host_glwire.c`，加入一个可选启用的 `GLWIRE_EGL_DEVICE` selector（默认 `-1`，保持旧的「第一个
能初始化的 EGL 设备」），以便在这台双 GPU 宿主上把回放对照绑定到另一块卡。除非设置该变量，它
不改变任何东西，而交付路径从不设置它；因此「未改动」对行为和默认值成立，对文件清单不成立。

## 8. 留给负责人的开放问题

1. **M5 基线重跑。** 2026-11-17 的 13.9 fps / 75 ms 数字是否被接受为对照，还是 M5 要求在本机
   同一负载区间内重新跑一次回放（T6）？成对测量意味着后者；代价是一次启动和一次运行。
2. **画面尺寸。** 合成客户端是 750 × 1624（图层尺寸，4.87 MB）。发布的帧应该是面板原生
   1125 × 2436（浏览器查看器裁剪后的尺寸），还是保持图层尺寸、让 `webview.py --crop` 缩放它？
3. **M2 验收形态。** 既然客户端是合成的、v1 又携带画面，stage 29 是否应该只以正确性为门槛
   （CRC + 逐字节一致 + 可见），性能只报告而不设门槛？按当前写法，K5 以回放预算（每帧 75 ms）
   为门槛，而不是以速度声明为门槛；这对一个机制实验来说是合适的标尺吗？
4. **分支政策。** `experiment/metal-translation` 不在文档化的前缀内（`feat/`、`spike/`……），
   而 spike 的时间盒是 5 天。该分支是继续作为 `experiment/…`，还是应该变成带硬性结束日期和
   `docs/adr/` 报告的 `spike/`？
5. **库的问题（M4）。** M4 是否从定义上就被阻断——CA 渲染器要的 `.metallib` 是 Apple 的、
   不能分发——还是说，写一个入口点名字相同的自家库是项目愿意考虑的路径？这是法务/产品判断，
   不是工程判断，而且它决定 M3 是否值得开始。
6. **GPU 索引稳定性。** `--gpu 1` 假设第二块 V100 永远是物理设备索引 1。宿主是否应该按 PCI
   总线/设备（`VK_EXT_pci_bus_info`）选择，并把索引当作回退，这样驱动重排就不会悄悄把实验移到
   设备 0 上？
7. **文档索引。** 本报告没有从 `docs/research/README.md` 链接，也不在 README 的
   Documentation 表中，因为那些是负责人拥有的文件。负责人是否应该在提交时补上这两行？

## 来源

写作本笔记时阅读过的材料（本仓库中的文件为本仓库核实；HakiOS 树与网络来源已在原处标注）：

- [`README.md`](../../README.md)——13.9 fps / 75 ms、挂钟时间 ±30%、每帧 12.65 KB 的记录、
  9.96 MB → 13 KB 的按引用历史、「不快 / 24 fps 是目标」一节。
- [`docs/architecture.md`](../architecture.md)、[`docs/phases.md`](../phases.md)、
  [`docs/state-of-the-port.md`](../state-of-the-port.md)、
  [`CONTRIBUTING.md`](../../CONTRIBUTING.md)、[`PROVENANCE.md`](../../PROVENANCE.md)。
- [`docs/adr/ADR-002-api-translation.md`](../adr/ADR-002-api-translation.md)、
  [`ADR-004`](../adr/ADR-004-virtio-device.md)、
  [`ADR-011`](../adr/ADR-011-gl-context-in-the-wire.md)、
  [`ADR-012`](../adr/ADR-012-frame-boundary-not-a-pass-boundary.md)。
- [`docs/stage23-verdict.md`](../stage23-verdict.md)、
  [`docs/stage24-blitfast.md`](../stage24-blitfast.md)、
  [`docs/stage25-multicore.md`](../stage25-multicore.md)、
  [`docs/stage27-multicore.md`](../stage27-multicore.md)、
  [`docs/stage28-boot.md`](../stage28-boot.md)、
  [`tools/glwire/README.md`](../../tools/glwire/README.md)。
- [`docs/research/linux-kernel-build.md`](../research/linux-kernel-build.md)——本笔记所依托的
  启动故事与 glwire/CA 测量。
- [`host/metalwire/PROTOCOL.md`](../../host/metalwire/PROTOCOL.md)、
  [`host/metalwire/metalwire_protocol.h`](../../host/metalwire/metalwire_protocol.h)、
  [`tools/metalwire/`](../../tools/metalwire/)。
- HakiOS 的 Guest 树（只读）：`docs/metal-plan.md`、`docs/air.md`、`docs/air-report.md`、
  `docs/ru/metal.md`、`docs/boot-args.md`，以及这些笔记引用的内核源码
  （`MiniIOMobileFramebuffer.cpp`、`userland/src/HakiosMetal.m`）。
- 网络资料，用于先例调研（外部报告）：[UniXNU/AdrenoMetal](https://github.com/UniXNU/AdrenoMetal)、
  [MoltenVK](https://github.com/KhronosGroup/MoltenVK)、
  [SPIRV-LLVM-Translator](https://github.com/KhronosGroup/SPIRV-LLVM-Translator)、
  [gfxstream](https://github.com/google/gfxstream)、
  [vkd3d](https://gitlab.winehq.org/wine/vkd3d)、
  [Mesa's Dozen](https://gitlab.freedesktop.org/mesa/mesa/-/tree/main/src/microsoft/vulkan)。

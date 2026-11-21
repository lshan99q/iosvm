# ADR-013：在设备插件中拦截 Metal，在 commit 处翻译，在宿主 Vulkan 上执行

> 中文译文，对应英文版 `docs/adr/ADR-013-metal-translation.md`；数字以英文版为准。

- 状态：**Proposed**（提议中），分支 `experiment/metal-translation`；这不是 `main` 的决策，
  采纳/否决的决策在 M6。
- 日期：2026-11-21
- Supersedes: —
- Superseded by: —
- 相关：[`ADR-002`](../adr/ADR-002-api-translation.md)（本决策使其具体化）、
  [`ADR-004`](../adr/ADR-004-virtio-device.md)（长期传输）、
  [`ADR-011`](../adr/ADR-011-gl-context-in-the-wire.md) 与
  [`ADR-012`](../adr/ADR-012-frame-boundary-not-a-pass-boundary.md)（GL 线路关于身份与帧边界的
  教训）、`docs/research/metal-translation-feasibility.md`、`host/metalwire/PROTOCOL.md`、
  `tools/metalwire/`、`guest/metal-plugin/`。

## 背景

本移植通过宿主 GL 回放路径（`tools/glwire/`）绘制 SpringBoard：Guest 记录 CoreAnimation 的
OpenGL ES 调用，宿主在真实的 GL 设备上回放这些调用，并把帧发布到浏览器。在交付的单 vCPU 配置
下，它以 **13.9 fps / 每帧 75 ms** 运行，而测量表明天花板是 Guest 的 CPU，不是宿主，也不是
线路（每帧记录 12.65 KB；宿主回放工作约 1.4 ms；virtio-gpu 换掉了传输，却什么都没改变，
1.80 → 1.85 fps）。

ADR-002 选择了另一条长期路线：在 API 层替换 Metal 驱动——Guest Metal 设备（L3）、传输
（L2）、宿主 Vulkan 后端（L1）——这样 Guest 只记录命令而不执行，由宿主的 GPU 做实际工作。这个
决策自 2026-10-31 起就是项目的方向，但 L3 那一半（`guest/metal-plugin`）在本仓库中从未存在过，
而且 Metal 从未在这个 Guest 中被实际跑过。

有三个事实使一个最小的真实版本现在成为可能（全部实测；来源见「证据」）：

1. 本移植已经发布一个空的 `IOAcceleratorES` 服务，启动参数 `metal=N` 使它命名为
   `HakiosMetal`/`HakiosMetalDevice`；随后 Metal 自己的加载器打开
   `/System/Library/Extensions/HakiosMetal.bundle`，要求一个声明了 `MTLDevice` 和
   `MTLDeviceSPI` 的 `_MTLDevice` 子类，并用加速器端口实例化它。
2. 因此，一个纯 Metal 客户端可以在没有任何 Apple 驱动的情况下得到设备、队列和命令缓冲区，而
   最小但有意思的命令——buffer→texture blit 后接一次 present——在 Vulkan 中有精确对应，且
   不需要 shader。
3. CoreAnimation 在这里不能充当那个客户端：设备对外公布后，它的 Metal 渲染器会停在一个缺失的
   `.metallib` 上，而其背后是 AIR→SPIR-V 编译器，本仓库中并不存在。那是 M3/M4 的工作，不在
   本决策范围内。

## 决策

采用 ADR-002 的形状，并端到端构建它的最小真实版本，作为自身分支上的**实验**，Replay 路径的
交付行为保持不变。

1. **拦截点。** 一个独立的 Metal 设备 bundle（`guest/metal-plugin/`），通过本移植现有的发现
   路径注册：内核的 `IOAcceleratorES` 服务命名它（`MetalPluginName`），类继承 Metal 私有的
   `_MTLDevice` 并声明 `MTLDevice` + `MTLDeviceSPI`，再用 `-initWithAcceleratorPort:` 实例化。
   它只安装在实验 root 镜像的 `/System/Library/Extensions/HakiosMetal.bundle`（指向
   `/redwood/lib/HakiosMetal.bundle` 的符号链接）。进程可见性仍由本移植自己的门控制：启动参数
   `metal=1` 加上 `/redwood/lib/HakiosMetal.clients` 文件点名 `metal_frame`（以及
   `metal_probe`），在同时启动窗口服务器的启动中**绝不**用 `metal=2/3`。
2. **翻译边界。** 调用在 encoder 中只被记录、不被翻译；翻译单位是 **`commit` 处的
   `MTLCommandBuffer`**，`waitUntilCompleted` 每帧阻塞等待一个确认。Guest 中不链接任何 Vulkan
   头文件、库或对象。
3. **v1 子集。** 设备、命令队列、命令缓冲区、带一次
   `copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:sourceSize:toTexture:`
   拷贝的 `MTLBlitCommandEncoder`、`presentDrawable:` / present、缓冲区（宿主可见的暂存）、
   纹理（optimal tiling、BGRA8/RGBA8）、一个内部 `<MTLDrawable>`（设备暴露
   `-newDrawableWithTexture:`，因为这个用户空间没有 `CAMetalLayer`）、`commit`、
   `waitUntilCompleted`，以及对象生命周期。有意缺失：render/compute encoder、库、pipeline
   state、shader、`IOSurface`、`MTLSharedEvent`、swapchain。
4. **翻译器来源。** 我们自己的记录器与宿主执行器，为本实验编写。不引入任何第三方翻译器：现成
   的东西里没有能把 Metal 图形翻译到 Vulkan 的（见「权衡过的选项」）；bundle 是我们自己的
   实现，HakiOS 树是运行时注册技术和 AIR 工作的参考；最接近的先例（UniXNU 的
   `metal2vulkan`/AdrenoMetal）被引用，而不是被收进仓库。
5. **传输。** 调用沿用 glwire 的形状：TCP、一条连接、Guest → 宿主 `10.0.2.2:9111`、每帧一个
   确认，契约在 `host/metalwire/metalwire_protocol.h`（两端共享，绝不复制）。画面**不**内联
   携带：插件向本移植的内核询问缓冲区背后的物理段（`hakios_vtop`，系统调用 556——`tools/glwire`
   已经在用的调用），宿主映射 QEMU 的 RAM 文件并读取同一批字节（`metalwire_host --ram`）。
   携带内联 body 的消息保留为缓冲区非常驻时的回退。宿主把每一帧发布成 glwire 的共享内存格式，因此
   `tools/glwire/webview.py --shm /dev/shm/metalwire-frame` 能不加修改地提供翻译后的帧。本实验
   **不**用 virtio-gpu：第 23 阶段测得它零收益，而传输不是本决策想要移动的瓶颈。决策记录按引用
   这一选择，因为它的缺席被测量过：同一条线路的内联版本跑 1.94 fps，按引用版本中位 58.3 fps
   （见「证据」）。
6. **宿主执行器。** 在显式选定的物理设备（**设备 1**，1 号槽的 V100；设备 0 属于 GL 回放实验）
   上运行无头 Vulkan，使用支持传输的队列，`vkCmdCopyBufferToImage` →
   `vkCmdCopyImageToBuffer`，fence 等待，回读，与 Guest 的 CRC-32 比较，每帧 CSV，SHM 发布，
   无窗口、无 swapchain。
7. **客户端。** 一个受控的 Metal 程序（`metal_frame`），它通过 `MTLCreateSystemDefaultDevice()`
   创建设备，填充一张确定性的 750 × 1624 BGRA8 画面，把它 blit 到一个纹理，呈现并等待。它
   **有意是合成的**，不是 CoreAnimation；实验的主张相应地有界（见「后果」）。
8. **基线与隔离。** Replay 路径保持其默认行为：对交付的启动配置、它的端口（9110）或它的
   root 镜像没有任何改动。分支上 `tools/glwire/` 下唯一的编辑是一个可选启用的
   `GLWIRE_EGL_DEVICE` selector（默认 `-1` = 旧的「第一个能初始化的 EGL 设备」），这样基线
   运行可以绑定到另一块卡；交付路径从不设置它。实验位于 worktree
   `/home/lshan/DSH-desktop/iosvm-metal` 的分支 `experiment/metal-translation` 上，启动自己的
   QEMU 3 号实例（monitor 4474、QMP 4475），使用自己的 Guest 运行树
   `/home/lshan/DSH-desktop/mt-work`，选择 GPU 1，在 9111 和自己的 SHM 文件上监听，并把一个
   回放基线 Guest 指向 9210，使两个实验无法触达彼此的服务。研究角色不提交任何东西；由负责人
   审查并提交。

## 权衡过的选项

| 选项 | 优点 | 缺点 | 判定 |
|---|---|---|---|
| **通过 `IOAcceleratorES` 的设备插件 + commit 级翻译（入选）** | Metal 自己支持的接缝；按进程控制；无需内核改动（服务与启动参数已存在）；不需要 Apple 文件；可在 QEMU 中测试；把传输、对象生命周期与 Vulkan 执行同 shader 隔离开 | 只实现客户端会请求的对象图；在 M3/M4 之前对 CA 覆盖不置一词 | **选为 M2** |
| 现状：只保留 GL 回放路径 | 交付 13.9 fps；无新风险；无新代码 | 使 ADR-002 仍未证明、Metal 问题未被提出；宿主 GL 回放是唯一的 GPU 用途 | 保留为**基线，不变** |
| 现在做完整 Metal 设备 + AIR→SPIR-V + CA 渲染器 | 让 CoreAnimation 走 Metal 的唯一路线 | 三个子系统，每个以月计；先被 Apple 缺失的 `.metallib` 阻断，再被一个不存在的编译器阻断；一个实验承载不了 | 超出范围（M3/M4 关卡） |
| 内核驱动：实现加速器与 IOGPU 提交 ABI | Guest 驱动可以直接提交 | 仅 `IOGPU` 就是 36 个类/711 个方法的 ABI；x86 上没有 AGX；把命令送到宿主 GPU 并不需要它 | 否决（ADR-002 的不模拟立场成立） |
| 把 Metal 翻译到 Guest 中现有的 GLES | 复用可工作的 GLES | 同一对象图加上 AIR→GLSL；GLES 无法表达 compute 或参数缓冲区；工作量严格更大 | 否决 |
| 现在用 virtio-gpu（2D，或 3D/Venus）作为传输 | 长期形态（ADR-004）；移植只用一种传输 | 第 23 阶段：零收益，Guest 从不等待；Venus 是 Guest 没有的 Vulkan 驱动 | 推迟到 ADR-004 自己的工作 |
| 引入 UniXNU 的 `metal2vulkan` 运行时 | 一个完整的 Metal→Vulkan 对象图，已演示合成一台平板的系统 UI | 在 Guest 内的 Turnip/Adreno 后端及其内核 ABI；一个完整运行时，而实验需要的是最小值；不能把帧放到本宿主的 V100 上 | 引用为先例，不采用 |
| 改为把 GL 回放流翻译到 Vulkan | 不需要 Metal；对现有路径而言是可移植性收益 | 不运行任何 Metal 客户端；回答的是另一个问题 | 超出范围 |

## 后果

**正面**

- ADR-002 所假设的接缝已在上游得到演示（HakiOS 以这种方式启动了自己的设备），并在对我们真正
  重要的地方重新得到证明：Metal 自己的加载器、在这个 Guest 中、用我们的类、没有 Apple 驱动
  也没有 Apple 二进制——这是 M2 的结果，不是假设。
- 实验把过去混为一个问题的三件事分开：*命令能否跨过去并在宿主上执行*（M2）、*shader 能否被
  编译*（M3）、*CoreAnimation 的渲染器能否被满足*（M4）。M2 的失败现在可以在没有编译器挡路的
  情况下被诊断。
- 宿主侧可以在**没有 GPU、没有 QEMU** 的 lavapipe 上测试，因此它可以成为 CI 关卡；Guest 侧
  可以在没有 GPU 的 QEMU 中测试，正如 HakiOS 树已经为它的设备所做的那样。
- 翻译后的画面出现在现有浏览器查看器中，无需修改 `webview.py`，因为 SHM 格式是 glwire 的。
- **端到端实测结果（2026-11-21/22）：** 5 × 400 帧，2,000/2,000 `crc_ok=1`，0 次 panic，
  合成 750 × 1624 客户端的中位 **58.3 fps**（Guest 每帧 14 ms，宿主 4.2 ms），socket 上
  0 画面字节。同一客户端走内联画面线路是 1.94 fps，按引用的改动就是差别——上面的传输决策是
  由证据驱动的，不是风格选择。

**负面**

- 覆盖很小，绝不能描述为「Metal 可用了」：它只是一次 blit 和一次 present。CoreAnimation 未被
  实际检验，交付的 13.9 fps 没有改变。
- 按引用上传依赖本移植的 `hakios_vtop` 系统调用，以及宿主知道 RAM 文件及其物理基址；没有该调用
  的 Guest 内核会回退到内联画面路径（实测：1.94 fps）。这也意味着在 translate 启动中宿主必须
  带 `--ram` 启动：插件无法知道，而它的帧会大声失败，而不是悄悄拷贝。
- 证明正确性的校验和要花 Guest CPU（完整逐字节 CRC 每帧 48 ms；8 路切片 CRC 24 ms；每 8 行
  采样 6 ms）。报告的 58.3 fps 使用抽样校验和；全画面证明是一次 `--byte-check` 运行
  （`memcmp`），不是每一帧。阶段记录说明每个数字用的步长。
- CRC-32 检查证明逐字节一致，**不是** GPU 执行：宿主 `memcpy` 也能通过。修复这一点的设备侧
  判据是未来工作；主张的措辞与此一致。
- 一个回答错误的插件可能让 Apple 的客户端崩溃（上游日志显示窗口服务器和 `mediaserverd` 在被
  展示一个不能绘制的设备时失败），因此未知 selector 会被吸收，UI 启动中避免 `metal=2/3`。
- 实验分支不在文档化的分支前缀内，可能腐烂；M6 必须解决它。

**中性**

- 协议是有版本、私有的（v1，`MW_VERSION`），不是公开 ABI。两端包含同一个头文件；线路改动会
  重建两端。
- GPU 1 与本机上的另一个实验共享；数字是成对且按负载分带的。

## 证据

实测，来源即研究笔记所引用的那些：

- **交付基线：** 13.9 fps / 每帧 75 ms（单 vCPU，`gkbf=1` + `ack_only` + `fb_noblit`；画面与
  9.09 fps 基线逐字节一致），且挂钟时间随宿主负载波动 ±30%——因此只做成对运行（`README.md`；
  `docs/stage24-blitfast.md`）。
- **传输不是瓶颈：** 第 23 阶段的同 root A/B，1.80 → 1.85 fps（+2.8%，在噪声内），584 次
  push、每次 7.3–8.3 ms，Guest 在约 245 ms 的一帧中等待 5 µs（`docs/stage23-verdict.md`）。
- **发现机制在上游有效：** 在 `metal=1` 下，类在 `backboardd` 和 `mediaserverd` 中注册，
  `-initWithAcceleratorPort:` 返回一个对象，随后 Metal 询问 `-featureProfile`、
  `-maxBufferLength` 和一个命令队列；窗口服务器随后停在
  `Assertion failed: (metallib_url != nil), function MetalContext, file ogl-metal.mm, line 3308`
  （HakiOS `docs/metal-plan.md`，「What steps 1 and 2 found」）。
- **实测的 shader 墙：** 读取 456 个系统模块，321 个翻译为 NIR（271 个 fragment、47 个 kernel、
  3 个 vertex）并编译，255 个与 LLVM 数值一致并被绘制；135 个 `air.visible` 库函数不是入口点；
  `AppleMetalGLRenderer.bundle/default.metallib` 的 274 个模块全部被翻译（HakiOS
  `docs/air.md`、`docs/air-report.md`）。
- **实测的对象图：** `AGXMetalA11` 53 个类/1,158 个方法；`IOGPU` 36 个类/711 个方法；
  `_MTLDevice` 549 个方法/36 个 ivar；`AGXA11FamilyDevice` 覆盖 236 个（HakiOS
  `docs/ru/metal.md` §4）。
- **线路的 glwire 先例：** 每帧十兆字节的记录在画面按引用传递后变成 13.6 KB，交接 99 µs，
  宿主派发 0.03 ms（`tools/glwire/README.md`）；回放路径的记录是每帧 12.65 KB，占画面的
  0.26%（`README.md`）。
- **本 ADR 拒绝重蹈的陷阱：** vkbridge 探针在把 blit 换成宿主 `memcpy` 后仍打印 8/8 `[ok]`，
  所以「字节是对的」不等于「机制已经确立」（`docs/phases.md` §M2，评审发现）。
- **在实验分支上实测（2026-11-21/22），stage 29：** 接缝在该 Guest 中加载（`metal=1`、
  clients 文件、加速器端口 2819 的设备）；1 号槽 V100（PCI `0000:05:00.0`）上
  2,000/2,000 帧 `crc_ok=1`；5 × 400 帧，中位 58.3 fps（57.4–60.1），Guest
  fill 2.6 ms + Metal 编码 0.19 ms + commit 7.1 ms + wait 13.7 ms，宿主合计 4.2 ms；socket
  上 0 画面字节；上传源与回读的完整 `memcmp` 一致；同一客户端走内联画面时 1.94 fps；Replay
  对照在同一次会话中测量（阶段记录 §5.6）。这些数字不能证明什么，见阶段记录 §5.5 与 §7。

## 后续

- M2 落地代码；其证据追加到阶段记录，本 ADR 弄错的前提用一个 **Amendment** 节纠正
  （ADR-011 的做法），绝不悄悄编辑。
- M3（针对一个系统 shader 的 AIR→SPIR-V）和 M4（CA 的渲染器库）是独立的关卡；本 ADR 不授权
  它们，而且在「到底要哪个 `.metallib`，以及我们能否提供它」的问题得到回答之前，M4 不能开始。
- M6 用 M2–M5 的证据把本 ADR 的状态更新为 Accepted/Superseded。
- 留给负责人的开放问题列在 `docs/research/metal-translation-feasibility.md` §8（基线重跑、
  画面尺寸、分支政策、按 PCI id 选 GPU、文档索引）。

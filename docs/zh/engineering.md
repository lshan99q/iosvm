# 设计基线 · 工程文档

> 状态：历史文档。ADR-001/003/007 已被取代，架构图与协议草案仍作 M2 参考。见 docs/zh/README.md。

# IOSVM 工程文档

| 项 | 值 |
|---|---|
| 项目名 | **IOSVM**（暂定） |
| 版本 | v0.1（草案） |
| 状态 | 设计阶段，未开工 |
| 范围 | Linux 宿主上以虚拟机方式运行 iOS 26 的 arm64e 用户空间，可渲染桌面 |
| 前置文档 | `iOS虚拟机-Linux.md`（前期调研） |

> **合规红线（贯穿全文）**：本项目**不分发**任何 Apple 二进制、镜像、缓存、字体、图片；不包含任何让设备或系统冒充真机通过 Apple 服务器校验的代码。违者不予合并。

---

## 1. 项目定位

### 1.1 一句话目标

在 Linux 上，用 QEMU 引导 iOS 26 的 arm64e 用户空间，**渲染出 SpringBoard 的第一帧**，全过程开源、可复现。

### 1.2 可验收定义（Definition of Done）

达成即发布 v1.0：

```
$ make run                    # 在 Linux x86_64 上
... 若干日志 ...
$ ./tools/screendump.sh       # 抓一帧
$ file logs/screen.png
logs/screen.png: PNG image data, 1179 x 2556    # 非全黑、非全白
```

判据（全部满足）：
1. 窗口里出现 SpringBoard 渲染的界面，像素非单色；
2. `docs/demo/m1-frame.png` 可复现（同 commit、同 IPSW 版本，差异在容差内）；
3. 全程不使用任何未开源组件（除 XNU 本体与用户自己提供的 IPSW 内容）；
4. `git ls-files` 中不含任何 Apple 二进制（CI 强制）。

### 1.3 非目标（明确排除）

| 不做 | 原因 |
|---|---|
| 分发 iOS 系统镜像 / IPSW | 侵权。镜像由使用者在本地从自己的 IPSW 生成 |
| 实现"激活"或"激活绕过" | 本架构天然不经过激活流程（见 ADR-006）；写这类代码违法且项目必死 |
| 连 Apple 服务（Apple ID / iCloud / App Store） | 需 Secure Enclave，物理上不可得 |
| 支持 App Store 加密 ipa | FairPlay 密钥由设备 DRM 身份派生，VM 无此身份 |
| 手机移植（真机裸金属） | 已有 HakiOS 等项目；本项目只做虚拟机，互补而非竞争 |
| 全硬件保真（蜂窝、摄像头、蓝牙） | 与"看到桌面"无关，v1.0 后再议 |
| 替代 Corellium 的商业能力 | 不做 SLA，不做多租户 |

---

## 2. 系统架构

### 2.1 总览

```
┌───────────────────────────────────────────────────────────────────┐
│ Guest（QEMU 内）                                                   │
│                                                                   │
│  ┌─────────────────────────────────────────────────────────────┐  │
│  │ iOS 26 arm64e 用户空间（使用者自备，从不入库）                 │  │
│  │   launchd · SpringBoard · CoreAnimation · libMetal.dylib     │  │
│  └───────────────┬──────────────────────────┬──────────────────┘  │
│                  │ Metal API                │ IOKit / syscall      │
│  ┌───────────────▼──────────────┐  ┌────────▼──────────────────┐  │
│  │ guest/metal-plugin           │  │ XNU（IPSW kernelcache）   │  │
│  │  MTLDevice 等符号替换        │  │  或 自编译 XNU（备选）    │  │
│  │  → 命令序列化                │  └────────┬──────────────────┘  │
│  └───────────────┬──────────────┘           │                     │
│                  │ 共享内存 ring + doorbell  │                     │
│  ┌───────────────▼──────────────────────────▼──────────────────┐  │
│  │ QEMU（qemu-sptm 分支）+ 本项目虚拟设备 virtio-iosvm-gpu     │  │
│  │   · SPTM/TXM 机器模型（复用）                               │  │
│  │   · virtio-blk / virtio-input / virtio-net / virtio-snd     │  │
│  └──────────────────────────┬─────────────────────────────────┘  │
└─────────────────────────────┼─────────────────────────────────────┘
                              │ dma-buf / fd 传递
┌─────────────────────────────▼─────────────────────────────────────┐
│ Host（Linux）                                                      │
│  host/vkbridge：Vulkan 后端                                        │
│    · 会话、对象表（buffer→VkBuffer、texture→VkImage）              │
│    · 命令流回放、fence 同步                                        │
│    · 呈现：VkSwapchain → QEMU 窗口 / SDL                          │
│  GPU：radv / nvidia / lavapipe（任何 Vulkan 1.1+ 驱动均可）         │
└───────────────────────────────────────────────────────────────────┘
```

### 2.2 分层职责

| 层 | 负责 | 不负责 |
|---|---|---|
| **L0 宿主 GPU** | Vulkan 执行 | — |
| **L1 host/vkbridge**（本项目） | Vulkan 对象管理、命令回放、呈现、fence | 理解 Metal 语义 |
| **L2 传输协议**（本项目） | 命令序列化、共享内存、对象/内存共享 | 翻译逻辑 |
| **L3 guest/metal-plugin**（本项目） | Metal API → 序列化命令 | 渲染实现 |
| **L4 iOS 用户空间**（使用者自备） | SpringBoard、CoreAnimation | — |
| **L5 XNU** | 调度、内存、驱动 | — |
| **L6 QEMU 机器模型** | 虚拟硬件、SPTM/TXM | GPU 语义 |

**关键边界**：L1 与 L3 **都不知道对方的语言**。Metal→Vulkan 的翻译发生在 **L3/L2 之间**（复用 UniXNU AdrenoMetal 的翻译表），传输层只搬"已编码的 Vulkan 命令"或"抽象命令"。这一刀切得好，宿主端就完全可替换（换成 D3D12、Metal-on-Mac 都不影响 Guest 侧）。

### 2.3 启动时序

```
QEMU 启动
  │  -kernel firmware/bootkc      ← 从使用者 IPSW 提取的 kernelcache（含 Apple 原版 kext）
  │  -dtb    firmware/dtree
  │  -drive  userland/system.apfs + userland/data.apfs
  ▼
SPTM → TXM → XNU 引导（依赖 qemu-sptm 的 SPTM/TXM 模型）
  ▼
launchd（读 /System/Library/LaunchDaemons）
  ├─→ 平台 expert / IOPlatformExpert 握手 ← 需虚拟设备树自洽
  ├─→ 存储：挂载 APFS（Apple 原版 AppleAPFS 驱动，来自 kernelcache）
  ├─→ graphics：需要"显示设备"
  │     └─→ 分流点：见 §5.5 候选方案 C1/C2/C3
  └─→ WindowServer / SpringBoard
        └─→ Metal 调用 → guest/metal-plugin → 传输 → vkbridge → 宿主 GPU → 像素
```

### 2.4 运行时数据流（一帧的生命周期）

```
1. SpringBoard 提交 CA::Render::Update
2. CoreAnimation 通过 Metal 编码 draw calls      [Guest, L4]
3. metal-plugin 拦截 MTLCommandBuffer::commit     [Guest, L3]
4. 把 MTLCommandBuffer 翻译成 Vulkan 命令序列      [Guest, L2/L3]
5. 写入共享内存 ring，敲 doorbell (iothread)       [Guest, L2]
6. QEMU 收到 doorbell，转发给 host/vkbridge        [L6]
7. vkbridge 回放 vkCmd*，提交 VkQueue              [Host, L1]
8. fence 完成，通过 eventfd 回 signal              [Host → Guest]
9. 呈现：把 VkImage blit 到 swapchain，QEMU 窗口刷  [Host, L1]
10. screendump → PNG                              [工具]
```

---

## 3. 关键技术决策

### ADR-001：不自研 VMM，fork QEMU
**决策**：基于 `jprx/qemu-sptm`（GPL-2.0）fork，不自己写 hypervisor。
**理由**：SPTM/TXM 的机器模型是已解决且有开源实现；自研等于重做 QEMU 二十年积累。
**后果**：项目主体许可受 GPL 影响（QEMU 侧）；Guest 侧组件可保持 MIT。

### ADR-002：GPU 走"API 翻译"而非"硬件模拟"
**决策**：不模拟 AGX GPU 的 MMIO/指令流，改为在 Guest 用户空间翻译 Metal API。
**理由**：
- Apple GPU 指令集不公开，社区无逆向产物（对比 Adreno 有 Turnip）。
- 模拟 AGX 是 Corellium 多年的核心资产，非个人可及。
- API 层翻译已有两个开源先例：UniXNU/AdrenoMetal（Metal→Vulkan→Turnip）、hakios-metal（Metal→Adreno 直驱）。
**后果**：依赖 Metal 公开 API 覆盖度；CoreAnimation 用到的私有路径（`MTLSharedEvent`、IOSurface 共享）是主要未知数。

### ADR-003：Guest 侧选"使用者自备 IPSW kernelcache"为主路径
**决策**：L5 用 IPSW 里的完整 kernelcache（含 Apple 原版 kext），不用自编译 XNU。
**理由**：kernelcache 里的 Apple 驱动与用户空间 ABI 天然匹配，能省掉重写全部驱动的工程量（这是 HakiOS 在真机路线上的主要开销）。
**后果**：QEMU 必须让 Apple 的 kext 认得出硬件（尤其存储、显示），这部分是本项目最大研究风险 → 见 §5.5。
**备选**：若主路径在 M1 失败，退回「自编译 XNU + 自写 virtio 驱动」路线（HakiOS 式），代价是用户空间需注入口子。

### ADR-004：传输层用自定义 virtio 兼容设备
**决策**：定义 `virtio-iosvm-gpu`，共享内存 ring + doorbell，不起用标准 virtio-gpu 的 3D 命令集。
**理由**：标准 virtio-gpu 的 Venus/VirGL 协议面向 OpenGL/Vulkan **Guest 驱动**，而我们的 Guest 侧是**自写 Metal 插件**，无需兼容任何 Guest 驱动生态。自定义协议更简单、可迭代。
**后果**：需要自己写协议栈（约 2–3 k 行），换来完全的控制权。

### ADR-005：不实现 Apple 服务
见 §1.3。技术上不可得（Secure Enclave），法律上不可为。

### ADR-006：激活流程"不经过"而非"绕过"
**决策**：引导路径为「解包 IPSW → 平铺为 APFS 系统盘 + 空数据盘 → 直接启动」，系统认为自己是已安装系统，从不进入恢复/激活流程。
**理由**：这是唯一在法律与工程上都成立的做法；且**不需要任何绕过代码**。
**后果**：无钥匙串、无 DRM、无 App Store；能跑的是自编程序与越狱生态包。

### ADR-007：构建双环境（Linux + 一个 Mac）
**决策**：运行时与宿主侧全在 Linux；**固件提取与 iOS SDK 编译**必须在 Mac 上。
**理由**：`ipsw`/`hdiutil` 与 iPhoneOS SDK 是 Apple 工具链，无法在 Linux 合法获得。
**后果**：CI 分两段——Linux 段跑全部可公开测试；Mac 段为可选的、有文档的手动步骤。**v1.0 不承诺纯 Linux 端到端**。

---

## 4. 核心组件规格

### 4.1 目标机型

| 项 | 取值 | 来源 |
|---|---|---|
| Guest 设备标识 | `iPhone17,3`（t8140）或 `iPhone18,3`（t8150） | darwin-vm 已验证 iOS 26/27 可引导 |
| 内核 | IPSW kernelcache（iOS 26.x） | 使用者自备 |
| 显示分辨率 | 1179×2556（iPhone 17,3） | 设备树 |
| CPU | qemu-sptm aarch64，`-cpu` 选仿真 Apple 核心 |  |
| 内存 | ≥ 6 GB（iOS 用户空间最低要求） | darwin-vm 实测 |

### 4.2 虚拟设备清单

| 设备 | 用途 | v1.0 必需 |
|---|---|---|
| `virtio-blk` ×2 | system.apfs / data.apfs | ✅ |
| `virtio-input` | 触摸（绝对坐标）、电源/音量键 | ✅ |
| `virtio-iosvm-gpu` | 显示 + GPU 命令通道 | ✅ |
| `virtio-net` | SSH、调试、包管理 | ✅ |
| `virtio-snd` | 声音 | ❌ v1.1 |
| `virtio-rng` | 熵 | ✅ |
| 假 `AppleDisplayPipe` 节点 | 让显示的 kext 有东西可绑（形态取决于 §5.5 决策） | ✅ |

### 4.3 传输协议 v0.1（草案）

```c
/* proto/iosvm_gpu.h —— 所有结构体 8 字节对齐，小端 */

#define IOSVM_PROTO_VERSION 0x0001u

struct iosvm_ring_header {
    uint32_t magic;        /* 'IOSV' */
    uint32_t version;
    uint32_t cmd_head;     /* guest 写 */
    uint32_t cmd_tail;     /* host 写 */
    uint32_t ack_head;     /* host 写 */
    uint32_t ack_tail;     /* guest 写 */
    uint32_t cmd_size;     /* 字节 */
    uint32_t ack_size;
    /* 随后是 cmd 区、ack 区、descriptor 表、共享内存对象表 */
};

enum iosvm_op {
    IOSVM_OP_CREATE_BUFFER = 1,   /* host 侧建 VkBuffer，回 handle */
    IOSVM_OP_CREATE_IMAGE  = 2,
    IOSVM_OP_FREE          = 3,
    IOSVM_OP_WRITE         = 4,   /* 内存上传 */
    IOSVM_OP_SUBMIT        = 5,   /* 命令序列 → VkCommandBuffer */
    IOSVM_OP_FENCE_WAIT    = 6,
    IOSVM_OP_PRESENT       = 7,   /* 指定 image → swapchain */
    IOSVM_OP_SCREENSHOT    = 8,   /* 同步读回像素（验收用） */
};

struct iosvm_cmd {
    uint32_t op;
    uint32_t len;          /* 后续 payload 字节数 */
    uint64_t seq;          /* 单调递增，用于 fence 关联 */
    uint32_t handle;
    uint32_t flags;
};

struct iosvm_ack {
    uint64_t seq;
    int32_t  status;       /* 0 = ok，负 = errno 风格 */
    uint32_t handle;       /* CREATE_* 的返回值 */
};
```

**同步模型**：单生产者单消费者 ring + `eventfd` doorbell。Guest 侧写入后 `ioctl(VIRTIO_IOSVM_KICK)`；Host 完成后 `eventfd_write` 唤醒 Guest 的 `poll`。不做细粒度锁。

**内存共享**：优先 `dma-buf`（Linux 5.4+ 通用路径），回落到「Guest 分配、Host 映射同一块内存」（QEMU `memory-backend-memfd` + 偏移表）。

### 4.4 Guest 侧注入层（`guest/metal-plugin`）

| 项 | 方案 |
|---|---|
| 注入方式 | 越狱生态的 `libhooker`/`substrate` 风格符号替换；或直接部署替换版 `libMetal.dylib` |
| 覆盖范围 v1.0 | `MTLDevice` `MTLCommandQueue` `MTLCommandBuffer` `MTLBlitCommandEncoder` `MTLRenderCommandEncoder`（仅 CoreAnimation 实际用到的子集） |
| 覆盖范围延后 | `MTLComputeCommandEncoder`（Liquid Glass 的重点，见 M4） |
| shader | `.metallib` → AIR → SPIR-V → 宿主。M2 先只做 blit（无需 shader），M3 再上 |
| 参考实现 | `UniXNU/AdrenoMetal`（Metal→Vulkan 翻译表）、`hakios-metal`（命令模型、fence 语义、`.metallib` 解析） |

**为什么先做 blit**：blit 不涉及 shader 编译，能最早验证「命令流跨 VM 边界到达宿主 GPU 且结果正确」这条主链路。这是整个项目最关键的未知数，必须最早证伪。

### 4.5 显示呈现路径：三个候选（M1 决策点）

这是本项目**最大的研究风险**。QEMU 里没有 Apple 的显示硬件，Apple 的显示 kext 绑不上，WindowServer 起不来。

| 方案 | 做法 | 优点 | 缺点 | 可行性判断 |
|---|---|---|---|---|
| **C1** 模拟 AppleDisplay | 逆向 `AppleDisplayPipe`/`IOMobileFramebuffer` 的 MMIO 接口，在 QEMU 里实现一个让 kext 认得的假设备 | Guest 侧零改动，最"正统" | 需逆向闭源 kext，工作量大且可能撞加密/校验 | ◐ 高不确定 |
| **C2** 用户空间呈现 hook | 不动 kext，在 WindowServer/CoreAnimation 的呈现路径注入，把 surface 直接交给 vkbridge | 绕开硬件模拟 | 需逆向私有 framework（`IOSurface`、`CA::Render`）；若 WindowServer 更早就挂掉则无效 | ◐ 中 |
| **C3** 自编译 XNU + 自写显示驱动 | 放弃 Apple kernelcache，自己写 virtio-gpu 的 Darwin 驱动（HakiOS 式） | 完全可控，参考 HakiOS 已跑通 | 要重写全套驱动；用户空间需适配 | ✅ 已知可行（HakiOS 在真机上做到了） |

**策略**：M1 用 2 周做 C1/C2 的 spike（各 3–4 天），按「能不能看到 WindowServer 进程起来」判生死；都不行则在 M1 末切 C3，并把 M0–M2 重新排期。**这是唯一的 go/no-go 决策门。**

---

## 5. 仓库结构

```
iosvm/
├── README.md
├── LICENSE              # MIT（本项目原创部分）
├── LICENSE.GPL2         # QEMU 侧
├── LICENSE.APSL         # XNU 补丁
├── PROVENANCE.md        # 声明：不含 Apple 二进制
├── docs/
│   ├── architecture.md  # 本文
│   ├── adr/             # ADR-001..007
│   ├── phases.md        # 阶段文档
│   └── demo/            # 验收截图
├── qemu/                # submodule: fork of qemu-sptm
├── hw/                  # 本项目的 QEMU 虚拟设备（virtio-iosvm-gpu 等）
├── proto/               # 传输协议 IDL + 生成的 C/ObjC 头
├── host/vkbridge/       # Vulkan 后端
├── guest/               # 注入到 iOS 用户空间的组件
│   ├── metal-plugin/
│   └── hooks/
├── firmware/            # 使用者自备，.gitignore
├── userland/            # 由工具从 IPSW 生成的 APFS 镜像，.gitignore
├── tools/
│   ├── unpack-ipsw.sh
│   ├── make-images.sh
│   ├── screendump.sh
│   └── provenance-check.py
├── tests/
│   ├── unit/            # 协议、序列化、对象表
│   ├── fake-gpu/        # 无 QEMU 的 vkbridge 单测（lavapipe）
│   └── golden/          # 像素黄金镜像
└── ci/
    ├── linux.yml        # 全部可公开测试
    └── mac.md           # 手动步骤文档
```

### 构建

```sh
# 依赖（Linux）
sudo apt install qemu-system-arm meson ninja build-essential \
                 libgl1-mesa-dri mesa-vulkan-drivers vulkan-tools \
                 pkg-config libdrm-dev 7zip lz4

# 一次性：从使用者自备 IPSW 生成固件与镜像（需 Mac 或已配置好的工具链）
./tools/unpack-ipsw.sh ~/iPhone17,3_26.x.ipsw
./tools/make-images.sh

# 构建
make host        # vkbridge
make guest       # metal-plugin（需 iPhoneOS SDK）
make qemu        # 含 virtio-iosvm-gpu 的 QEMU
make run         # 起 VM
make test        # 单元 + fake-gpu（不需要 Apple 文件）
```

---

## 6. 测试与验收

### 6.1 分层测试

| 层 | 测试方式 | 是否需要 Apple 文件 |
|---|---|---|
| 协议序列化 | 单测（C, 无依赖） | ❌ |
| vkbridge 对象表 / 命令回放 | 对 **lavapipe**（软件 Vulkan）跑，逐字节比对回读 buffer | ❌ |
| fake-gpu | 按 hakios-metal 的 `fake_gpu.hpp` 思路：进程内解释命令，严格校验（越界、未知 opcode、fence 乱序直接 fail） | ❌ |
| 翻译层正确性 | Metal 侧构造确定性 draw，与宿主端参考图比对 | ✅ |
| 端到端 | 起 VM，`screendump` 抓帧与黄金镜像比 | ✅ |

**原则：所有能在 CI 里跑的测试都不需要 Apple 文件。** 这是项目可持续的前提——贡献者不必拥有 iPhone。

### 6.2 probe 程序（验收工具）

学 hakios-metal 的做法，写 `iosvm-probe`，**每行一个检查**：

```
$ ./iosvm-probe
[ok] proto handshake                (magic/version)
[ok] create buffer 1 MiB
[ok] write 1 MiB, readback identical
[ok] blit copy 512 KiB              (4.2 ms)
[ok] blit fill 0..999996 bytes
[ok] 40 command buffers in order
[ok] fence signal/wait ordering
[ok] present image → screenshot      (1179x2556, non-uniform)
```

M2 的验收就靠它；M3/M4 往上加行。

### 6.3 回归策略

- 每个里程碑打 tag，附带同版本 `firmware/` 的 SHA256（不含文件本身）。
- 像素回归用容差比对（Liquid Glass 有噪声），M2 用严格逐字节，M3 起用 SSIM > 0.98。

---

## 7. 风险登记

| ID | 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|---|
| R1 | C1/C2 都失败，必须退 C3，工期翻倍 | 高 | 高 | M1 设决策门，2 周内证伪；C3 已由 HakiOS 证明可行 |
| R2 | Liquid Glass 重度依赖 `MTLComputeCommandEncoder` + 私有扩展，翻译层覆盖不了 | 中高 | 高 | M3 先只求"第一帧"，M4 再攻 shader；把 M3/M4 拆成两个可发布版本 |
| R3 | 传输层同步语义在 Guest 高并发下出错（死锁/丢帧） | 中 | 中 | fake-gpu + TSan/ASan；先单线程 ring 后扩多队列 |
| R4 | Apple 发法律函件 | 中 | 极高 | 严守「不分发 Apple 文件」；`provenance-check.py` 进 CI；不写任何冒充真机的代码 |
| R5 | 依赖 Mac 提取固件 → 准入门槛高，贡献者少 | 高 | 中 | Linux 侧测试全可跑；提供「无 Apple 文件」的 `make test-open` 路径 |
| R6 | 单人项目续航（2–3 年） | 高 | 高 | 每阶段独立可发布；先做能上 HN/GitHub Trending 的中间成果（M2 的 probe 就够） |
| R7 | QEMU fork 上游漂移，merge 成本上升 | 中 | 中 | 虚拟设备走独立 `hw/` 目录，尽量少改 QEMU 核心 |

---

## 8. 开源治理

| 项 | 规定 |
|---|---|
| 许可 | 本项目原创代码 **MIT**；QEMU fork **GPL-2.0**；XNU 补丁 **APSL-2.0**；`.metallib` 解析等借用代码保留原许可（见 `NOTICE`） |
| 不含 | Apple 二进制/镜像/缓存/字体、设备固件、密钥、证书、激活数据 |
| CI 强制 | `tools/provenance-check.py`：拒绝未申明的二进制、拒绝任何 `firmware/` 下文件入库、拒绝写 Guest 系统分区的代码 |
| 贡献政策 | 涉及「冒充真机」「绕过校验」的 PR 一律关闭，理由写在 `CONTRIBUTING.md` |
| 命名 | 项目名与文档中提及 Apple 商标仅作描述用途，声明无关联 |

---

## 9. 参考项目

| 项目 | 复用点 | 许可 |
|---|---|---|
| [jprx/darwin-vm](https://github.com/jprx/darwin-vm) | QEMU SPTM/TXM 模型、IPSW 解包流程、trustcache 机制 | MIT |
| [jprx/qemu-sptm](https://github.com/jprx/qemu-sptm) | QEMU fork 基础 | GPL-2.0 |
| [UniXNU](https://github.com/UniXNU/UniXNU) | AdrenoMetal：Metal→Vulkan 翻译层 | BSD-2-Clause-Patent |
| [oosexgang/hakios-metal](https://github.com/oosexgang/hakios-metal) | Metal 命令模型、fence 语义、`fake_gpu` 测试范式、`.metallib` 解析 | MIT |
| [oosexgang/hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel) | C3 备选路线的驱动参考、provenance 治理范式 | MIT / APSL-2.0 |
| [Mesa](https://mesa3d.org) | Turnip / lavapipe | MIT |

---

## 10. 术语

| 术语 | 含义 |
|---|---|
| SPTM / TXM | iOS 的 Secure Page Table Monitor / Trusted Execution Monitor，新内核完整性机制 |
| kernelcache | IPSW 内已链接好的内核 + 全部 kext 的二进制集合 |
| AIR | Apple Intermediate Representation，Metal shader 的中间表示 |
| blit | 无 shader 的纯内存/图像拷贝操作，最早可验证的 GPU 路径 |
| 黄金镜像 | 用于像素回归的基准截图 |

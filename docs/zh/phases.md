# 设计基线 · 阶段文档

> 状态：已被 docs/phases.md 取代（D1 换成 D1'，M1 范围收窄）。见 docs/zh/README.md。

# IOSVM 阶段文档

| 项 | 值 |
|---|---|
| 版本 | v0.1（草案） |
| 配套文档 | `IOSVM-工程文档.md` |
| 总计 | 单人 2–3 年；3 人团队 8–14 个月 |
| 交付定义 | v1.0 = Linux 上 QEMU 内渲染出 SpringBoard 第一帧并可复现 |

---

## 0. 里程碑总览

| 里程碑 | 名称 | 一句话判据 | 估时（单人） | 公开动作 |
|---|---|---|---|---|
| **M0** | 地基与 root shell | VM 里看到 `bash-3.2#` | 2–3 周 | 建仓 + README |
| **M1** | 用户空间起 boot + 显示路径定案 | `ps` 里出现 `WindowServer`，且 C1/C2/C3 已定 | 2–3 月 | 技术博客①：QEMU 里跑 iOS 26 |
| **M2** | GPU 通道打通（blit 正确） | `iosvm-probe` 全绿，512 KiB 拷贝逐字节一致 | 2–4 月 | **v0.1-alpha** + 博客②⭐ |
| **M3** | 第一帧 | 抓到的 PNG 非单色 | 3–6 月 | **v0.1 演示版** + 博客③⭐ |
| **M4** | 主屏可用（Liquid Glass + 输入） | 能点亮、能滑动、能进设置 | 3–6 月 | **v1.0** |

### 依赖图

```
M0 ──→ M1 ──┬──→ M2 ──→ M3 ──→ M4
             │      ↑
             └── 决策门 D1（显示路径 C1/C2/C3 定案）
```

M2 依赖 M1 的**决策结果**，不依赖 M1 的独立成果——所以 D1 一旦落定，M2 可以并行于 M1 剩余工作。

---

## M0：地基与 root shell

### 目标
在任何自研代码之前，先证明「这台机器上能跑起来一个真的 XNU」。

### 交付物
- [ ] 仓库骨架（按工程文档 §5 建目录）
- [ ] `qemu/` submodule 指向 `qemu-sptm`，能编译（`--disable-pvg`）
- [ ] `tools/unpack-ipsw.sh`：从使用者 IPSW 提取 bootkc / dtree / ramdisk / trustcache
- [ ] `tools/make-images.sh`：生成 `userland/system.apfs`、`userland/data.apfs`
- [ ] `make run` 起 VM，进 root shell
- [ ] `PROVENANCE.md` + `tools/provenance-check.py` + CI（**先立规矩，后写代码**）
- [ ] README 说明：需要一台 Mac 提取固件（ADR-007）

### 任务分解
| 天 | 事 |
|---|---|
| 1–2 | 仓库骨架、`LICENSE`×3、`PROVENANCE.md`、CI 骨架 |
| 3–5 | 复现 darwin-vm：QEMU 编译通过（本地需 8–10 GB 内存） |
| 6–8 | `unpack-ipsw.sh` + `make-images.sh`，按 darwin-vm 文档走 mac 侧步骤 |
| 9–12 | 调通引导：`-kernel bootkc -dtb dtree -drive ...`，解 trustcache 报错 |
| 13–15 | `provenance-check.py`（拒绝二进制/firmware 入库）、README、首次 commit |

### 验收
```sh
make run
# 期望：Darwin Kernel Version ... RELEASE_ARM64_T8140
#       bash-3.2# uname -a   能出结果
./tools/provenance-check.py    # exit 0
```

### 退出条件
root shell 能稳定重复启动 3 次，且 `provenance-check` 在 CI 上通过。

### 未知数
- `qemu-sptm` 在 x86_64 Linux 上的编译是否真的干净（README 以 macOS 为主）。
- 目标 IPSW 版本（iOS 26.x）在 t8140 上是否都在已验证列表内。

### 风险
R7（QEMU fork 漂移）在这里第一次出现：**从第一天起就不改 QEMU 核心，只加 `hw/` 下的文件。**

---

## M1：用户空间起 boot + 显示路径定案

### 目标
让 iOS 26 的用户空间跑起来到「WindowServer 该出现的地方」，并在 2 周内把显示路径的生死问清。

### 交付物
- [ ] 虚拟设备树自洽：平台 expert 握手通过，`launchd` 不 panic
- [ ] 存储：APFS 双盘挂载成功（Apple 原版 AppleAPFS 驱动认得出 virtio-blk）
- [ ] `virtio-net` + SSH：能登进 Guest（调试效率的关键）
- [ ] **§4.5 的 spike 报告 `docs/adr/D1-display-path.md`**
- [ ] 虚拟机里的 `ps aux` 输出截图（证明跑到哪一步）

### 任务分解
| 周 | 事 |
|---|---|
| 1 | 设备树：对照真机 dtree 补节点，把 panic 一个个消掉 |
| 2 | 存储打通（virtio-blk → AppleAPFS） |
| 3 | `virtio-net` + SSH + 把日志送出 Guest（**先解决可观测性**） |
| 4 | **spike A**：C2 用户空间呈现 hook（3–4 天） |
| 4 | **spike B**：C1 模拟 AppleDisplay（3–4 天） |
| 5 | D1 决策文档，写清「试了什么、为什么行/不行、选了哪条」 |
| 6–8 | 按决策结果铺开实现；同时并行启动 M2 的协议层 |

### 验收
```sh
make run                    # 起 VM
ssh -p 2222 root@127.0.0.1  # 进 Guest
# Guest 内：
ps aux | grep -i windowserver   # 有进程即达成本阶段主要目标
```

### 退出条件
`docs/adr/D1-display-path.md` 定稿，且 `WindowServer` 进程存在（哪怕立刻崩）。

### 未知数（本阶段的研究问题）
1. Apple 的显示 kext 在没有真 Apple 显示硬件时，是「不绑定」还是「绑定了但读超时」？前者有救，后者要伪造 MMIO。
2. `IOSurface` 在两个 VM 边界之间能否直接映射？还是必须先拷？
3. launchd 的平台 expert 握手到底要多少个节点才满足？有无最小集？

### 风险
- **R1（最高）**：C1/C2 双失败 → 切 C3，M1 延期 2–3 月，且用户空间需注入口子。
- **R5**：没有 Mac 的贡献者到这一步就断了 → 必须在 M1 就准备好「日志导出 + 可复现脚本」，让别人能远程帮调。

### 决策门 D1（go / pivot）

| 结论 | 后果 |
|---|---|
| C2 可行 | 最优路径。M2 全力做协议 + 翻译层 |
| C1 可行（含大量逆向） | 需评估逆向工作量是否 > C3 |
| 都不可行 | **pivot 到 C3**（自编译 XNU + 自写 virtio 驱动，参考 HakiOS），M1 重排期到 4–5 月，M2/M3 相应后移 |

---

## M2：GPU 通道打通（blit 正确）⭐ 最关键阶段

### 目标
**证伪或证实整个项目成立的核心假设**：Metal 命令能跨 VM 边界打到宿主 GPU，且结果逐字节正确。

这一阶段结束，无论成败都是可发布的成果。

### 交付物
- [ ] `proto/iosvm_gpu.h` v0.1 冻结（含 ring、op 集合、fence 语义）
- [ ] `host/vkbridge`：Vulkan 对象表、命令回放、fence，能在 **lavapipe** 上跑
- [ ] `hw/virtio-iosvm-gpu`：QEMU 设备，doorbell → vkbridge
- [ ] `guest/metal-plugin`：只实现 `MTLDevice`/`MTLCommandQueue`/`MTLCommandBuffer`/`MTLBlitCommandEncoder`
- [ ] `iosvm-probe`（见工程文档 §6.2）
- [ ] `tests/fake-gpu`：进程内解释命令，严格校验（越界/未知 opcode/fence 乱序直接 fail）
- [ ] **v0.1-alpha tag**

### 任务分解
| 月 | 事 |
|---|---|
| 1 | 协议定稿；vkbridge 骨架 + lavapipe 单测；buffer 创建/写/回读 |
| 2 | `virtio-iosvm-gpu` QEMU 设备；ring + eventfd 打通（Guest 一个 `ioctl` 能让 Host 打印一行即小胜） |
| 2–3 | metal-plugin：blit 三项（copy / fill / 同步）；`MTLBlitCommandEncoder` 语义 |
| 3 | `iosvm-probe` 每行一个检查；fake-gpu 严格测试；TSan/ASan |
| 4 | 性能与稳定性；`--screenshot` 路径（同步读回像素，为 M3 铺路） |

### 验收
```sh
make test                      # 单元 + fake-gpu，全绿，无需 Apple 文件
make run
ssh -p 2222 root@127.0.0.1 'iosvm-probe'
# 期望：
# [ok] proto handshake
# [ok] create buffer 1 MiB
# [ok] write 1 MiB, readback identical
# [ok] blit copy 512 KiB
# [ok] blit fill 0..999996
# [ok] 40 command buffers in order
# [ok] fence signal/wait ordering
```

### 退出条件
probe 全绿，且在 lavapipe（软件 GPU）与至少一块真 GPU 上都能通过。

### 未知数
1. **iOS 的 `MTLBlitCommandEncoder` 是否真的被 CoreAnimation 用于最终 present**？如果 CA 走的是私有路径，probe 全绿也可能与桌面无关 → 必须在 M2 末用 `--screenshot` 抓一张「Guest 主动画的红屏」来验证闭环。
2. `MTLSharedEvent` 的跨进程语义能否用 eventfd 近似？
3. dma-buf 在 QEMU 里的可用性（是否需要显式 `-object memory-backend-memfd`）。

### 风险
- **R3（同步）**：这是最容易埋雷的地方。原则：**先单队列单线程，跑通后再并行**。任何为性能做的妥协都要有 fake-gpu 测试兜底。
- **R2（shader 缺失）**：本阶段不碰 shader，是刻意安排——把「通道是否通」和「翻译是否全」两个问题切开。

---

## M3：第一帧 ⭐ 项目的存在证明

### 目标
CoreAnimation 通过 Metal 画出**任意一帧**到屏幕上。

### 交付物
- [ ] shader 链：`.metallib` → AIR 解析 → SPIR-V（或直接构造 Vulkan pipeline）
- [ ] `MTLRenderCommandEncoder` 子集：pipeline state、vertex/index buffer、texture 绑定
- [ ] `--screenshot` 稳定输出 PNG
- [ ] `docs/demo/first-frame.png`
- [ ] **v0.1 演示版 tag** + 30 秒演示视频

### 任务分解
| 月 | 事 |
|---|---|
| 1 | AIR 解析（复用 hakios-metal 的 `metallib.hpp` 思路）；先跑通一个手写的最简 shader |
| 2 | Render encoder：draw call、pipeline cache、blend |
| 3 | 接 CoreAnimation：从「自画矩形」到「CA 的 layer tree」 |
| 3–4 | 卡住的话：二分法——把 CA 的每类 layer（solid / text / image）单独攻 |
| 4–6 | 稳定性、分辨率、色彩空间（P3 / sRGB，错了屏幕会发绿或过曝） |

### 验收
```sh
make run
sleep 300        # CPU 模拟，先黑屏约 1 分钟，再 Apple 标志
./tools/screendump.sh
python3 tools/check-frame.py logs/screen.png --min-unique-colors 64
# exit 0
```

### 退出条件
连续 3 次启动都能抓到非单色帧，且人工确认「这是 iOS 的界面，不是噪点」。

### 未知数
1. AIR 是否加密/签名校验（若校验，需在 Guest 侧绕过——**注意：这是绕过 Apple 的技术措施，需在此处停下来做法律评估**，若不可行则 M3 只能停在"自画内容"）。
2. Liquid Glass 大量使用 compute shader，M3 只需 render 子集，但 CA 的模糊/折射可能强制走 compute → 若强制，则 M4 内容提前到 M3。
3. 色彩管理：CA 期望 P3，vkbridge 若按 sRGB 会偏色。

### 风险
- **R2 集中爆发点**。缓解：M3 的 DoD 只要求"非单色"，不要求"像 iOS"。把「像」留给 M4。
- 若 AIR 路线死，备选是「Metal → SPIR-V 直译」或「完全自实现 CA 后端」，两者都是巨大工程 → 这是第二个 pivot 点。

---

## M4：主屏可用

### 目标
点亮主屏、滑动翻页、进设置。即「看到桌面」的真正含义。

### 交付物
- [ ] `virtio-input`：绝对坐标触摸、电源/音量键
- [ ] compute shader 支持（Liquid Glass 必需）
- [ ] 主屏图标渲染、Dock、状态栏
- [ ] 手势（滑动、上滑回主屏）
- [ ] 性能：能在可接受时间内响应（不作硬指标，但需记录数字）
- [ ] **v1.0 tag**

### 任务分解
| 月 | 事 |
|---|---|
| 1 | 输入设备 + 触摸坐标映射（窗口缩放 → 设备坐标） |
| 1–2 | compute shader 路径 |
| 2–3 | Liquid Glass 着色（对照 HakiOS 在真机上的逐像素比对结果） |
| 3–4 | 图标/文字渲染（字体！Apple 字体来自 IPSW，不进仓库） |
| 4–6 | 手势、稳定性、崩溃恢复、文档 |

### 验收
```sh
make run
# 手动：点亮屏幕 → 滑动 → 打开设置 → 返回
./tools/screendump.sh && open logs/screen.png
```

### 退出条件
人工操作 5 分钟不崩；`docs/demo/demo.mp4` 录制完成。

### 未知数
1. 字体渲染依赖 `CoreText` + IPSW 内字体文件，路径要通。
2. compute shader 的 workgroup 语义在 Vulkan 上是否一一对应。
3. 触摸的惯性/多点手势是否需要 `IOHIDFamily` 层配合（可能又回到 kext 问题）。

---

## 排期汇总

| 阶段 | 单人 | 3 人团队 | 关键风险 |
|---|---|---|---|
| M0 | 2–3 周 | 1–2 周 | QEMU 编译环境 |
| M1 | 2–3 月 | 1–1.5 月 | **R1**（D1 决策门） |
| M2 | 2–4 月 | 1.5–2.5 月 | R3 |
| M3 | 3–6 月 | 2–3 月 | **R2**（第二个 pivot） |
| M4 | 3–6 月 | 2–3 月 | 输入/字体 |
| **合计** | **12–20 月**（乐观）／**2–3 年**（现实） | **8–14 月** | — |

> 区间宽是因为**这件事没人做成过**，类比的只有 Corellium（团队+数年+闭源）和 darwin-vm（一个人+两个月+root shell）。M3 之后的部分没有可信参照。

---

## 研发节奏建议

| 频率 | 事 |
|---|---|
| 每天 | `make test` 必须绿（fake-gpu + 单元） |
| 每阶段 | 一个 `probe` 检查、一个 demo 截图/GIF、一篇 `docs/adr/` 决策记录 |
| 每里程碑 | tag + 一篇技术博客（M1/M2/M3 各一篇） |
| 每季度 | 复盘：R1/R2 是否收敛，是否该 pivot |

**博客节奏是项目存活的关键（R6）**：M2 的 `iosvm-probe` 全绿本身就够发一篇有分量的文章——"第一个在开源虚拟机里打到 GPU 的 Metal 命令流"。哪怕 M3 永远做不成，M2 的成果也成立。

---

## 立刻可做的第一条命令

```sh
# 纯 Linux、零 Apple 文件，先尝一遍 XNU 在 QEMU 里的手感
git clone https://github.com/oosexgang/hakios-xnu-kernel.git
cd hakios-xnu-kernel && make doctor && make qemu-open
```

这条命令不产生任何 M0 交付物，但能在 1 小时内告诉你：这台机器的 QEMU、内存、UEFI 固件是否满足后续所有阶段的要求。

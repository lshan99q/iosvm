# GPU 直通评估：为什么不行，以及现有加速路径

> **结论：本项目不做 GPU 直通。** 宿主 x86_64 + 客机 aarch64，V100 是 x86 PCIe 设备；客机 XNU 没有 PCI GPU / virtio-gpu 驱动。现有真正的 GPU 加速是 **glwire**（客机 GL → 宿主 V100），瓶颈是 guest→host 帧同步/上载，不是缺 GPU。
> 本文是评估，不是实验记录；P4 数字见 `docs/gpu-acceleration-stage19-decisions.md` D7、runbook §8.7。

## 1. 直通为何不可行

### 1.1 宿主/客机架构不同，GPU 是 x86 PCIe 设备

```sh
$ uname -m
x86_64

$ qemu-system-aarch64 --version | head -1
QEMU emulator version 10.0.13 (Debian 1:10.0.13+ds-0+deb13u1)

$ sed -n '145,146p' tools/qemu-open.sh
timeout --signal=TERM "$SECONDS_RUN" qemu-system-aarch64 \
    -M virt,gic-version=3 -cpu cortex-a76 -smp 1 -m "$MEM" \
```

- 宿主是 **x86_64**；客机固定跑 **`qemu-system-aarch64 -M virt -cpu cortex-a76`**（aarch64）。
- sysfs 显示 **两个** NVIDIA V100 **x86 PCIe 3D controller**：`10de:1db1`（GV100GL / Tesla V100 SXM2 16GB）。本容器没有 `lspci`，用 sysfs 等价证据；`nvidia-smi` 当前只列 GPU0，是驱动可见性/权限问题，不改变 PCIe 设备有两个的事实：

```sh
$ for d in /sys/bus/pci/devices/*; do
    [ "$(cat "$d/class" 2>/dev/null)" = 0x030200 ] || continue
    echo "$d $(cat $d/vendor):$(cat $d/device)"
  done
/sys/bus/pci/devices/0000:04:00.0 0x10de:0x1db1
/sys/bus/pci/devices/0000:05:00.0 0x10de:0x1db1

$ nvidia-smi -L
GPU 0: Tesla V100-SXM2-16GB (UUID: GPU-...)
```

- QEMU 命令行里**没有** `-device vfio-pci` / `-device virtio-gpu`；`-M virt` 只挂了 virtio-blk、EFI、loader、串口。
- 在这个项目里，把 x86 的 V100 直通给 aarch64 客机没有可用的设备/固件/驱动路径；**跨架构 + 无客机驱动**，不是加一个 QEMU 参数能解决。

### 1.2 客机内核没有 PCI GPU / virtio-gpu 驱动

```sh
$ ls xnu-src/xnu-8796.121.2/bsd/dev | grep -E '^hakios\.(virtio|adreno)' | sort
hakios.adreno.c
hakios.virtio_blk.c
hakios.virtio_input.c
hakios.virtio_net.c
hakios.virtio_sound.c
```

- 客机只有 virtio **blk/net/input/sound**，以及 `hakios.adreno`（Qualcomm Adreno GPU，不是 NVIDIA PCIe 驱动）。
- 没有 `virtio-gpu`、没有 PCI GPU 驱动、没有 DRM/fence/命令提交层；即使同架构，也缺驱动。
- 所以“直通 V100”缺的不是 IOMMU 开关，而是**客机侧的设备驱动 + 资源/命令/fence 通路**。

## 2. 现有加速路径：glwire

**一句话**：客机把 GL 调用/资源编码成 wire 消息，宿主 `host_glwire` 在 V100 上重放/合成；客机不直接持有 GPU，V100 只服务宿主侧渲染。

P4 四格（D7，TCG 单核 + softpipe）：

| cell | 实测 | 判定 |
|---|---|---|
| cell1-pure（纯本地，无 host GPU） | **120 帧 / 11.00s = 10.91 fps（guest 时钟）** | 唯一可用数字 |
| cell2-by-reference | 1800s 只到 **183/343（≈0.04 fps）** | 不可用 |
| cell3-w15 | guest 驱动 **2 帧 / 772,366 ms（≈0.003 fps）**；host 只到 frame 250 | 不可用 |
| cell4-w15w3 | **NOT RUN（判决，不是漏跑）** | 远端路径已证不可用 |

- `0x502` 四格全 0；这只是判据通过，不是性能达标。
- **瓶颈是 guest→host 帧同步/上载**（guest 送帧停摆、帧间隔到秒级），不是 V100 算力不足；P4 的 host 单帧回放 29.6–59.0ms（16–33fps 能力）也说明宿主侧不是主要瓶颈。

## 3. 下一步三条路线与代价

### ① 修 guest→host 帧同步（最小改动，先做）

- **做什么**：查 `eagl_remote` / GL wire 的队列、背压、swap/readback 时序，以及 host replay 的帧边界；目标是让 cell2/cell3 的 guest 送帧不再停摆。
- **代价**：改动集中在 guest GL 库 + host replay/harness，不碰内核、不碰物理 GPU；风险中等（协议/时序回归），可用 P4 四格复测。
- **判据**：cell2/cell3 在 120 帧窗口内不再 TIMEOUT；`0x502` 仍为 0；至少一个远端 cell 的 guest 送帧间隔进入可比较范围。
- **建议**：**先做这条**；它是现有 glwire 路径内的优化，不是新架构。

### ② virtio-gpu + 宿主 GL（大工程，不建议本轮）

- **做什么**：XNU 侧新写 virtio-gpu 驱动（PCI/mmio、资源创建、命令提交、fence、内存共享），QEMU 侧加 virtio-gpu 设备，宿主侧做 3D 命令转发/渲染；接近一个小型 DRM/ virgl 栈。
- **代价**：内核驱动 + QEMU 设备 + guest 用户态 + host renderer 四层都要动；跨架构/无现成驱动；调试周期以月计，且会碰内核稳定性。
- **结论**：不是“配置直通”，是**新写驱动的大项目**；除非独立立项并有长期维护，否则不做。

### ③ 保持现状，只用纯本地

- **做什么**：保留 cell1 纯本地路径，不追求 host V100 加速。
- **代价**：最低；无新驱动/无直通风险。
- **上限**：当前 **10.91 fps（guest 时钟）**；适合功能验证/演示，不适合高帧率。

## 4. 本轮不做：不采购 / 不改宿主 GPU 驱动配置

**明确边界**：本轮**不采购 GPU、不改宿主 GPU 驱动配置、不做 VFIO/IOMMU 绑定、不把 nvidia 设备 unbind 给 vfio-pci、不改 GRUB/内核参数、不加 GPU 直通 QEMU 参数**。

原因：

- 直通在本项目里不可行（§1），改宿主配置没有收益，反而有把宿主 GPU/显示/其他负载弄挂的风险；
- 宿主 GPU 是共享资源，`nvidia-smi -r`、reset、驱动重装、vfio bind 都属于高风险操作，不应在功能实验里做；
- 若未来真要做 GPU 直通/虚拟化，另立项目、单独变更窗口、先做回滚预案，并取得 owner 明确批准。

**本轮所有 GPU 相关工作只走 glwire / 纯本地路径。**

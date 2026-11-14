# Stage 23 A1：response 0 / used len=24 分析（2026-11-14）

ext42 证据：

```text
phys=0x19179c000 desc0 0x19179c400/24/0x1/1 desc1 0x19179c500/408/0x2/0 sizeof(resp)=408
used flags=0 idx=1 id=0 len=24; response 00000000 00000000 00000000 00000000
```

## a. QEMU 8.2 的 GET_DISPLAY_INFO 返回多少字节

**正常路径是 408 B，不是 24 B。**

QEMU 的 `virtio_gpu_get_display_info()` 构造的是
`struct virtio_gpu_resp_display_info`：
`hdr` 24 B + `VIRTIO_GPU_MAX_SCANOUTS(16) × display_one(24 B)` = **408 B**，
然后调用 `virtio_gpu_ctrl_response(g, cmd, &info.hdr, sizeof(info))`。
即使没协商 `VIRTIO_GPU_F_EDID`，`pmodes` 也在这个 408 B 响应里；EDID 是另一条
`RESP_OK_EDID`。**24 B 只可能来自 `virtio_gpu_ctrl_response_nodata()`**（错误/无数据路径），
它的 `used->len = sizeof(virtio_gpu_ctrl_hdr) = 24`。

因此 `used len=24` 说明设备走了 **nodata/error**，不是 display_info 路径。
零 type 则说明 **这 24 B 的 `type` 没有落到我们读的 `desc1`**（或 QEMU 的 nodata 分支 type=0）。

## b. kvtophys 的地址是否对 DMA 有效

`0x19179c000` 在 QEMU 的 RAM 窗口内：RAM base `0x40000000` + 7 GB ≈ `0x200000000`，
6.3 GB 的 `0x19179c000` 是合法 guest physical。`kvtophys()` 在 XNU 上返回的就是
guest physical（不是 IOVA），`virtio-mmio` 没有 IOMMU，QEMU 的 `dma_memory_write()` 直接写
guest physical，所以**地址本身是有效的**。

`hakios.virtio_input.c` 用同样的 `kmem_alloc(kernel_map, ..., PAGE_SIZE)` + `kvtophys()`
+ `reg_write64(QUEUE_DESC/DRIVER/DEVICE, phys+...)`，能工作；所以**不需要**
`kmem_alloc_contig`，也不是“页不在 DMA 区间”。输入驱动的区别只是单描述符、不读 device
features，和这个 bug 无关。

真正要核对的是 **response 那一端的 `desc1.addr`/`flags`/`len`**：
- `desc0.flags` 必须是 `VIRTQ_DESC_F_NEXT(1)`，`next=1`；否则设备不会跟到 desc1，
  只在 desc0 上完成，`used len` 仍可能是 24；
- `desc1.flags=VIRTQ_DESC_F_WRITE(2)`，`len=408`；
- `desc1.addr=phys+0x500` 必须在同一页、且 `phys` 与 `QUEUE_DESC` 用的是同一个
  `kvtophys(page)`。

**“used idx=1/len=24” 证明队列和 desc0 已被设备读到；不能证明 desc1 的写地址被设备采用。**

## c. 一个 boot 内判别“请求没读到”还是“响应地址不对”

1. **打印 `request->type`（0x0100）在 notify 前后各一次**：
   - notify 前 0x0100、notify 后仍是 0x0100，说明设备没有把 response 写进 desc0；
   - 若变成 0x1201/0x1202，说明设备把 response 写到了 desc0（NEXT 没生效/写错描述符）。
2. **打印 `used->ring[0].id/len`**（已知 id=0,len=24），以及 **`desc1` 物理地址**；
   再在 **`response`（page+0x500）** 和 **`request`（page+0x400）** 两处各打印前 32 B。
3. **换一条 24 B 响应的命令**：`RESOURCE_CREATE_2D (0x0102)`，请求
   `hdr(24)+resource_create_2d(16)+format/width/height`，响应只有 24 B。
   - `used len=24` 且 `resp.type=0x1101 (RESP_OK_NODATA)` → 请求/队列/地址都通，
     问题只在 `GET_DISPLAY_INFO`（QEMU 无 pixman/display 后端）；
   - 仍全零 → 响应写地址/描述符链问题，优先查 desc0 NEXT 与 desc1 WRITE/addr。
4. **QEMU 侧**：`virtio_gpu_get_display_info()` 在 `virtio_gpu_require_pixman()` 失败时
   走 nodata 错误路径（24 B）。当前 QEMU 只有 `-device virtio-gpu-device` + `ramfb`，
   若没有真正的 display 后端（`-display egl-headless`/`vnc`/`gtk`），就会命中这个分支。
   **最小修法：给 QEMU 加一个 display 后端**，例如
   `-display egl-headless`（或 `-vnc 127.0.0.1:9`），再跑同一内核。

## 最小修法（按优先级）

1. 确认 **`desc[0].flags=VIRTQ_DESC_F_NEXT`**（补丁 0002 已做）；`desc[1].flags=WRITE`、
   `len=408`、`addr=phys+0x500` 同页。
2. **QEMU 加 display 后端**（`-display egl-headless` 或 `-vnc`）；`ramfb` 不是
   virtio-gpu 的 pixman 后端。
3. 若仍 24 B 零响应：用 `RESOURCE_CREATE_2D` 分辨（上文 c3），并按 c1 打 request type。
4. 不要在未确认前改 `kmem_alloc_contig`/DMA 区间；input 驱动已证明 `kvtophys` 路径可用。

未 boot，未改代码；补丁 0002 仍有效。

## QEMU 10.0.13 的 `efi-virtio.rom`（2026-11-14）

`efi-virtio.rom` 是 **PCI 显卡**（`virtio-gpu-pci`/`virtio-vga`）的 UEFI option ROM，不是
virtio-mmio 的 `virtio-gpu-device`。实测：

- `-device virtio-gpu-pci,romfile=` 可禁用 PCI ROM（property 存在）；
- **`-device virtio-gpu-device` 没有 `romfile` 属性**，也不需要这个 ROM；它能正常实例化
  （`-M virt -device virtio-gpu-device -display none -S -nodefaults` 不报 romfile）。

A1 驱动扫描的是 **virtio-mmio**（`0x0a000000` 起的 32 slots，device id 16），所以：

1. **必须用 `-device virtio-gpu-device`**，不要用 `virtio-gpu-pci`/`virtio-vga`；
2. 若命令里出现 `efi-virtio.rom`，说明实际用的是 PCI 设备（或脚本被改成了 PCI）——切回
   `virtio-gpu-device` 即可，不要去找 ROM 文件；
3. 若确实必须 PCI（A1 不需要），可加 `,romfile=` 禁用（或装 `ipxe-qemu`/`qemu-efi` 提供
   `efi-virtio.rom`），但那样 guest 驱动也看不到 virtio-mmio，A1 仍失败。

### display 后端

`ramfb` 不是 virtio-gpu 的 pixman 后端。验证 A1 用：

```bash
QEMU_VIRTIO_GPU=1 QEMU_DISPLAY=egl-headless qemu/run-darwin.sh 90
# 或
QEMU_VIRTIO_GPU=1 VNC=1 VNC_PORT=9 qemu/run-darwin.sh 90
```

`qemu/run.sh` 的 `-display ${QEMU_DISPLAY:-none}` 接受 `egl-headless`；`VNC=1` 时
`run-darwin.sh` 自己设 `QEMU_DISPLAY=vnc=127.0.0.1:9`。二者都比 `none`/`ramfb` 更接近
`virtio_gpu_require_pixman()` 的要求。

## 非法命令 0xdeadbeef 仍 24B 全零：QEMU 源码结论（2026-11-14）

### a. `virtio_gpu_ctrl_response()` 与 `virtqueue_pop()` 的 in_sg

QEMU 的 `virtqueue_pop()` 按描述符顺序累计：

```c
if (desc.flags & VRING_DESC_F_WRITE) in_num++; else out_num++;
```

对「desc0 可读 24B、flags=NEXT、next=1；desc1 可写 408B、flags=WRITE、next=0」：

- `out_num = 1`，`out_sg[0] = desc0`（24B）；
- **`in_num = 1`，`in_sg[0] = desc1`（408B）**。

`virtio_gpu_ctrl_response()` 写响应时走 `iov_from_buf(cmd->elem->in_sg, cmd->elem->in_num, 0, resp, resp_len)`，
然后 `virtqueue_push(cmd->vq, cmd->elem, resp_len)`。非法命令 `0xdeadbeef` 的
`virtio_gpu_handle_ctrl()` 会走 default：

```c
cmd->error = VIRTIO_GPU_RESP_ERR_UNSPEC;  /* 0x1201 */
...
virtio_gpu_ctrl_response_nodata(g, cmd, cmd->error);  /* 24B */
```

这条路径**应该写 24B 的 0x1201 到 desc1**。实测 `used len=24` 但 desc1 全零，只有两种可能：

1. **`in_num=0`**：`virtqueue_pop()` 没把 desc1 算成可写（NEXT/WRITE/flags 链没被采用）；
2. **`in_num=1` 但 `in_sg[0].iov_len` 或 `iov_base` 为 0/NULL**：`address_space_map` 没建立，
   `iov_from_buf` 实际写 0 字节，`virtqueue_push` 仍 push 24。

**不是 `avail->ring[]` 写法**：`ring[0]=0` 是 head，`idx=1`；先写 `ring[]`、再写 `idx`、再
`dmb`、再 notify 是正确顺序。`avail->flags=NO_INTERRUPT` 也不影响。若此处错，`virtqueue_pop()`
会拿不到任何描述符，`used idx` 不会变 1。

**结论：优先验证 `in_num`/`in_sg`，而不是继续改 avail 顺序。** 最小诊断：
- notify 前后打 `request->type`；若 desc1 仍是 0，说明设备没把 response 写进 desc1；
- 打 `used->ring[0].id/len` 与 `desc1.addr`，确认 id=0,len=24；
- 用 **`RESOURCE_CREATE_2D (0x0102)`**（响应也只有 24B）区分：若 `resp.type=0x1101`，
  说明 desc1 可写、request 解析正常；仍零就是 in_sg/地址；
- QEMU trace：`-trace enable=virtio_queue_notify -trace enable=virtio_gpu_cmd_*`。
  `virtio_queue_notify` 出现说明 queue 被 accept；`virtio_gpu_cmd_get_display_info` 出现说明
  command 被解析；若 trace 有 response 事件但 guest 仍零，就是 iov/地址。

### b. virtio-mmio QueueDesc/Driver/Device 高 32 位

QEMU 8.2/10 的 `virtio_mmio_read()` 对 `QUEUE_DESC_LOW/HIGH`、`QUEUE_DRIVER_*`、
`QUEUE_DEVICE_*` **不是可靠的 RW 回读**（写-only/读 0 的实现），所以
**`desc=0:0` 读回 0 不能证明队列没被接受**。

不用寄存器回读的验证：

1. `QUEUE_READY`（0x044）读回 1，且 `QUEUE_NUM`（0x038）是 8，`QUEUE_SEL=0`；
2. QEMU trace `virtio_queue_notify`（queue 0 被通知）；
3. QEMU trace `virtio_gpu_cmd_get_display_info` / `virtio_gpu_cmd_res_create_2d`
   （请求被 virtio-gpu 解析）；
4. guest `used->idx` 从 0 变 1（设备已消费）；
5. `info qtree`/`info virtio`（monitor）看队列地址/ready。

**结论：队列已被接受（used idx=1 + trace cmd），问题在 response 的 in_sg 写入，不在高 32 位
寄存器。**

未改代码、未 boot；等待整页非零扫描结果。

## `-nodefaults` 与 efi-virtio.rom（2026-11-14）

实测：`-M virt` 的默认设备集会在没有 `-nodefaults` 时给 `virtio-gpu-device` 带出
`efi-virtio.rom`；加 `-nodefaults` 后设备正常。`qemu/run-darwin.sh` 把额外参数原样放到
`qemu/run.sh ... "$@"` 的末尾，所以**不必改共享脚本**：

```bash
QEMU_VIRTIO_GPU=1 QEMU_DISPLAY=egl-headless \
  qemu/run-darwin.sh 90 -nodefaults
# 或 VNC
QEMU_VIRTIO_GPU=1 VNC=1 VNC_PORT=9 \
  qemu/run-darwin.sh 90 -nodefaults
```

`qemu/run.sh` 已显式给 `-serial stdio -monitor none`，`-nodefaults` 去掉的是默认 VGA/串口等
设备，不会去掉显式传入的 `memory-backend-file`、`virtio-*`、`ramfb`、`-display`。

若以后要持久化，只在 `QEMU_VIRTIO_GPU=1` 时加：

```bash
GPU=(-nodefaults -device virtio-gpu-device)
```

放在 `-device ramfb` 之前；这次不改共享脚本，先用额外 argv。

## `-nodefaults` 与 ramfb 的 `vgabios-ramfb.bin`（2026-11-14）

`-nodefaults` 会去掉 QEMU 默认 VGA 的 ROM 集，`-device ramfb` 随即要
`vgabios-ramfb.bin`；Debian 的 `qemu-system-data/qemu-system-common/seabios/vgabios`
**都不带这个文件**（只有 `qboot.rom`、`vgabios-virtio.bin` 等）。直接 `-vga none` 而不加
`-nodefaults` 仍会命中默认 `efi-virtio.rom`，不是方案。

### 临时方案（不改共享脚本）

用 seabios 的 `vgabios-virtio.bin` 拷成 `qemu-roms/vgabios-ramfb.bin`，让 QEMU 用 `-L` 找：

```bash
cd /host/iosvm-work/hakios-xnu-kernel
QEMU_RAM_GB=8 QEMU_VIRTIO_GPU=1 QEMU_DISPLAY=egl-headless \
  qemu/run-darwin.sh 90 -L "$PWD/qemu-roms" -nodefaults
```

`qemu-roms/vgabios-ramfb.bin` 已放在共享路径（39,424 B，来自 `seabios` 的
`vgabios-virtio.bin`）。QEMU 参数检查已通过（`-L ... -M virt -nodefaults -device
virtio-gpu-device -device ramfb -display none -S` 不再报 romfile；只是停在 `-S`）。

这是**临时替代**，不是官方 `vgabios-ramfb.bin`；若 UEFI 仍异常，需要从 QEMU 源码的
`pc-bios/` 取真正的 `vgabios-ramfb.bin`，或安装带该 ROM 的包。

### 另一条（A1 串口-only）

A1 判据只看串口。若 loader 不依赖显示，可去掉 ramfb：

```bash
QEMU_RAM_GB=8 QEMU_VIRTIO_GPU=1 QEMU_DISPLAY=egl-headless \
  qemu/run-darwin.sh 90 -nodefaults
```

但 `run-darwin.sh` 里 `-device ramfb` 是写死的，extra argv 不能移除它；要真正去掉 ramfb 得改
脚本（需 owner 同意），所以先用上面的 `-L + 替代 ROM`。

### 不要做的

- 不要加 `-vga none` 而不加 `-nodefaults`：仍会报 `efi-virtio.rom`；
- 不要把 `qboot.rom` 改名成 `vgabios-ramfb.bin`；
- 不要在未确认前改共享 `run-darwin.sh` 的默认设备集。

## 修正：不要 `-nodefaults`，用 `-L qemu-roms -vga none`（2026-11-14）

`-nodefaults` 会改 UEFI 的默认设备/启动顺序：Shell 出现但不自动跑 `startup.nsh`。正确做法是
**保留默认设备集**，只去掉默认 VGA：

```bash
cd /host/iosvm-work/hakios-xnu-kernel
QEMU_RAM_GB=8 QEMU_VIRTIO_GPU=1 QEMU_DISPLAY=egl-headless \
  qemu/run-darwin.sh 90 -L "$PWD/qemu-roms" -vga none
# 或
QEMU_RAM_GB=8 QEMU_VIRTIO_GPU=1 VNC=1 VNC_PORT=9 \
  qemu/run-darwin.sh 90 -L "$PWD/qemu-roms" -vga none
```

`qemu-roms/` 现含：

```text
efi-virtio.rom        262,144 B   (来自 ipxe-qemu, 供默认/回退 VGA)
vgabios-ramfb.bin      39,424 B   (seabios vgabios-virtio.bin 的临时替代)
```

`-vga none` 去掉默认 `virtio-vga`/`efi-virtio.rom` 冲突；`-device ramfb` 仍由
`qemu-roms/vgabios-ramfb.bin` 提供 ROM；默认 `-drive if=virtio` 的 ESP 和 UEFI 启动顺序不变，
所以 `startup.nsh` 应能自动跑。QEMU 参数检查：

```text
-L ... -M virt -vga none -device virtio-gpu-device -device ramfb -display none -S
```

不再报 romfile，只停在 `-S`。`-nodefaults` 那条只用于没有 ramfb 的串口-only 实验，A1 不要用。

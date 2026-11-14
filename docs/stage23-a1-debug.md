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

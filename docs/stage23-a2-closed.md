# stage 23 A2 完成（2026-11-15 01:35）

内核 **md5 `d86126af`**，串口实测（`virtio_gpu=1`）：

```
virtio-gpu: display 0: 1280x800 enabled=1
virtio-gpu: command 0x101 -> response 0x1100 (used id=0 len=24)   ← RESOURCE_CREATE_2D
virtio-gpu: command 0x106 -> response 0x1100                     ← RESOURCE_ATTACH_BACKING
virtio-gpu: command 0x103 -> response 0x1100                     ← SET_SCANOUT
virtio-gpu: command 0x105 -> response 0x1100                     ← TRANSFER_TO_HOST_2D
virtio-gpu: command 0x104 -> response 0x1100                     ← RESOURCE_FLUSH
virtio-gpu: A2: resource 1 750x1624, 298 entries, scanout 0, flush OK
```

即：**750×1624 BGRA 资源已在设备侧创建、绑定 298 个 guest 页、挂到 scanout 0、传输并刷新成功**。
这是「像素进显存」链路的完整验证，A3（把 panel 换成这块资源）的技术前提全部就绪。

## 修复记录

`RESOURCE_CREATE_2D` 常量原来是 `0x0102`（那是 `RESOURCE_UNREF`），改为 **`0x0101`** 后一次通过。
命令/响应区间：命令 `0x01xx`、响应 `0x11xx`（0x1100 NODATA）。

## 代码位置

- `drivers/gpu/hakios.virtio_gpu.c`：`gpu_cmd()` 通用命令（发布→轮询→校验响应）、`gpu_a2()` 五步链路；
- 判据：每条命令响应 `0x1100`。

## 下一步 A3

让 guest 的 panel 直接写进这块 resource（替代 glwire 的 TCP 上传），由宿主扫描输出。
判据：画面完整（dock 通栏、右下不黑）+ 稳态 fps 明显高于当前 2–3.3。

# Stage 23 A2/A3：panel 作为 virtio-gpu resource

## 状态

- 内核源码 commit `0f04de9`，补丁 `docs/patches/0003-stage23-A2-A3-panel-resource.patch`。
- 新内核 `hakios-kernel` md5 **9c852489f916f59a95979b568ac88b43**，
  `__TEXT=0xfffffff007004000`，`__TEXT_EXEC` 增大 0x4000，A1 markers 仍在。
- guest `tools/glwire/patches/eagl.c` 同步；userland `GL=1` 已编过。
- **未 boot**。

## A2

`gpu_cmd()` 用两个描述符（request→response）逐条发送：

```text
RESOURCE_CREATE_2D 0x0101   (0x0102 是 RESOURCE_UNREF，别混)
RESOURCE_ATTACH_BACKING 0x0106
SET_SCANOUT 0x0103
TRANSFER_TO_HOST_2D 0x0105
RESOURCE_FLUSH 0x0104
```

每条判 `0x1100`；A2 的测试 backing 是 750×1624 B8G8R8A8，4.8 MB，298 个 4K entries。
`virtio_gpu=1` 时仍跑 A1+A2（测试图案）。

## A3

`virtio_gpu=2`：A1 发现照旧，A2 建 resource/attach/set scanout（不再填测试图案），
随后创建 `/dev/hakios-gpu`。

guest `eagl.c` 在 `frame_end()` 里：

```c
struct eagl_gpu_push { uint64_t pixels; uint32_t stride, width, height; };
ioctl(gpu_fd, EAGL_GPU_PUSH, &p);   // EAGL_GPU_PUSH = _IOW('g',1,...)
```

内核 `gpu_ioctl()` 按 `stride` 逐行 `copyin` 到 4.8 MB backing，然后
`TRANSFER_TO_HOST_2D` + `RESOURCE_FLUSH`。这样 panel 的 IOSurface 行宽（3008/3072）
不要求等于 750×4。

**全帧优先**，dirty rect 还没做。glwire 现有路径保留：`virtio_gpu=2` 只是额外把 panel
推给 virtio-gpu；要拿到帧率收益，下一步需要在 A3 模式下跳过 glwire 的 panel upload/readback。

## 启动

```bash
QEMU_RAM_GB=8 QEMU_VIRTIO_GPU=1 QEMU_DISPLAY=egl-headless \
  qemu/run-darwin.sh 120 -L "$PWD/qemu-roms" virtio_gpu=2
```

期望串口：

```text
virtio-gpu: device in slot ...
virtio-gpu: /dev/hakios-gpu ready (panel push)
...
virtio-gpu: command 0x105 -> response 0x1100
virtio-gpu: command 0x104 -> response 0x1100
```

判据：QMP `screendump` 画面完整（dock 通栏、右下不黑），稳态 fps 高于 2–3.3。
未 boot，等 exp。

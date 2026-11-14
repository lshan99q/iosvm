# stage 23 A1 完成（2026-11-15 01:13）

内核 **md5 `af95d490`**（含 virtio-gpu 驱动），串口实测：

```
virtio-gpu: slot 28 id=16 transport=2 queue=8 max=1024
            features=30000002:00000101 virgl=0 edid=1 uuid=0 blob=0 ctx_init=0 version1=1
virtio-gpu: slot 28: after reset: status=0xb sel=0 num=0 ready=0
virtio-gpu: slot 28: after queue setup: status=0xf ... ready=1
virtio-gpu: slot 28: used flags=0 idx=1 id=0 len=408
virtio-gpu: slot 28: response 00001101 ...       ← VIRTIO_GPU_RESP_OK_DISPLAY_INFO
virtio-gpu: display 0: 1280x800 enabled=1
virtio-gpu: device in slot 28
```

## 过程中修掉的两个真 bug

### 1. avail 环在内容就绪前就发布了（导致"响应全零"）

旧顺序：`desc 表 → avail->idx=1 → QUEUE_READY=1 → memset(request/response) → notify`。
队列就绪时 avail 里已有条目，设备立刻消费了**尚未填写**的请求，答复写进响应区后又被紧跟的
`memset(response, 0, …)` 擦掉；此后 `last_avail_idx` 已前进，再 notify 也不会重发。
症状正是：`used idx=1 len=24` 而 `response area nonzero words=0`。

修法（virtio 2.7.13 顺序）：**填好 request/response → 置 QUEUE_READY → 再发布 avail->ring/idx
→ dmb ish → notify**。

### 2. 响应码常量写错

`VIRTIO_GPU_RESP_OK_DISPLAY_INFO` 被写成 `0x0101`（那是命令区间的编号）。
响应在 `0x11xx`：`RESP_OK_NODATA=0x1100`、`RESP_OK_DISPLAY_INFO=0x1101`、`RESP_OK_EDID=0x1102`。
驱动一直收到正确答复却判为失败。改正后一次通过。

## 环境前提（已写成配方）

见 `docs/stage23-boot-recipe.md`：vars.fd 重置为 64MB 0xFF、`QEMU_ESP=1`、payload 放进 ESP、
`-L qemu-roms`、不要 `-nodefaults`/`-vga none`。

## 下一步：A2

`RESOURCE_CREATE_2D`（750×1624 RGBA8）→ `ATTACH_BACKING` → `SET_SCANOUT` → `TRANSFER_TO_HOST_2D`
→ `RESOURCE_FLUSH`，判据：设备返回 0x1100（NODATA）且宿主窗口能看到测试图案。

# Stage 23 A1 收口（2026-11-14）

内核 **af95d490** 实测通过：

```text
virtio-gpu: used idx=1 id=0 len=408
virtio-gpu: response 00001101
virtio-gpu: display 0: 1280x800 enabled=1
virtio-gpu: device in slot 28
```

## 两个根因

1. **avail 环提前发布**：`avail->idx=1` 写在填 request/response 之前，队列就绪时设备立刻消费
   空请求，答复被随后的 `memset(response)` 擦掉；`last_avail_idx` 已前进，再 notify 也不重发。
   正确顺序：**填好内容 → QUEUE_READY → 发布 avail → dmb → notify**。
2. **响应码常量错**：`RESP_OK_DISPLAY_INFO` 写成 `0x0101`（命令区间），实际是 **0x1101**。
   响应在 `0x11xx`：0x1100 NODATA / 0x1101 DISPLAY_INFO / 0x1102 EDID。

补丁：`docs/patches/0001-stage23-A1-...patch`（desc NEXT）、
`docs/patches/0002-stage23-A1-fix-response-descriptor.patch`。

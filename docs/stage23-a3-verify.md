# s23-a3 — virtio_gpu=2 验证（内核侧通过；userland 推送被编译环境卡住）

## 一、内核侧（已验证）

启动：`QEMU_VIRTIO_GPU=2`（run-darwin 自动加 `-device virtio-gpu-device` 并把 guest
cmdline 设成 `virtio_gpu=2`）+ `QEMU_ESP=1` + `-L qemu-roms`，root 用 `/tmp/root-w15.apfs`。

串口证据：

```text
virtio-gpu: slot 27 id=16 transport=2 queue=8 max=1024 features=30000002:00000101 ...
virtio-gpu: display 0: 1280x800 enabled=1
virtio-gpu: command 0x101 -> response 0x1100 (used id=0 len=24)   RESOURCE_CREATE_2D
virtio-gpu: command 0x106 -> response 0x1100 (used id=0 len=24)   ATTACH_BACKING
virtio-gpu: command 0x103 -> response 0x1100 (used id=0 len=24)   SET_SCANOUT
virtio-gpu: command 0x105 -> response 0x1100 (used id=0 len=24)   TRANSFER_TO_HOST_2D
virtio-gpu: command 0x104 -> response 0x1100 (used id=0 len=24)   RESOURCE_FLUSH
virtio-gpu: A2: resource 1 750x1624, 298 entries, scanout 0, panel push enabled
virtio-gpu: /dev/hakios-gpu ready (panel push)
virtio-gpu: device in slot 27
```

- a) `/dev/hakios-gpu ready (panel push)` **出现**；
- b) `hakios-gpu: panel push failed` **0 次**；
- A2 五条命令全部 `0x1100`（之前的 response 0 问题已消失）；
- `QMP screendump` 尺寸从 1280x800（固件 scanout）变成 **750x1624**（guest SET_SCANOUT 生效），
  画面全黑（backing 未被推过）—— 与「eagl 还没 push」一致。

## 二、userland 推送（未验证，被环境卡住）

`userland/build.sh` 在本容器无法编译：**`/usr/bin/clang` 不存在**
（`xnu-clang: line 19: /usr/bin/clang: No such file or directory`，容器只剩 gcc）。
且现有 `userland/root.apfs`（17:44）是 **无 GL** 构建（`/lib` 里没有 `libhakios.gl.dylib`），
`/tmp/root-w15.apfs`（11-09）虽有 GL 但早于 eagl.c 的 A3 改动（`strings` 里 `hakios-gpu` = 0 次）。

→ 需要：带 clang 的构建环境，或 code 侧产出的 **GL=1 + A3 eagl.c** 的 root.apfs。

## 三、fps

`end=timeout ... hostframes=2`（300 s 内只出 2 帧，处于已知的 ~500 s 首帧慢窗口），
且本发 root 没有 eagl 推送，**fps 不可比**。要测 A3 的帧率收益，需要新的 root + 更长的
MAXSECS（≥600），并同时记录 `frame_us`/`copying it out`。

## 四、产物

- `tools/glwire/harness/gpushot.py`：QMP `screendump` → PNG（`-display none` 下实测可用）；
- 本目录：serial/host/drive、`scanout-black-750x1624.png`、`userland-build-blocked.log`。

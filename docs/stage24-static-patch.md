# Stage 24：静态补丁通道（最小闭环）

## 协议

guest → host，在每帧的 `R` 之前可选发：

```text
S <x> <y> <w> <h> <format> <hash> <len>\n
<len bytes pixels>
```

- `format=0`：BGRA；
- 目前只支持 **全屏 750×1624**（`len = 750*1624*4 = 4,872,000`）；
- `hash`：FNV-1a 64，内容不变不重发（首帧/变化时发）；
- host 在回放记录流后，把补丁 **整块覆盖**到 picture 上（当前 z 顺序：补丁在记录之后）。

## 开关

- host：`GLWIRE_STATIC_PATCH=1`（环境变量，默认关）；
- guest：`glwire_static_patch=1`（boot-arg，默认关）；
- 两边必须同时开；只开 guest 时 host 会把 `S` 当坏请求，只开 host 时 guest 不发。

## 代码位置

- `tools/glwire/host_glwire.c`：`serve_frame()` 的读行循环识别 `S`，读像素到
  `static_patch_pixels`；`glReadPixels` 之后、in-place 写回/回传之前 `memcpy` 覆盖 picture。
- `userland/src/eagl_remote.c`：`eagl_remote_static_patch()` 收 750×1624 BGRA，算 hash；
  `eagl_remote_flush()` 在首个 `R` 前发 `S` + 像素，hash 不变不再发。
- `userland/src/eagl.c`：`eagl_panel_750()` 把 1125×2436 panel 缩到 750×1624；
  `eagl_static_patch()` 在 `frame_end()` 里调用（A3 push 旁边）。

## 跑

```bash
# host
GLWIRE_STATIC_PATCH=1 NO_PANEL=1 ./host_glwire

# guest（A3/GLREMOTE/local 的 root，boot-arg 加 glwire_static_patch=1）
QEMU_RAM_GB=6 QEMU_ESP=1 QEMU_VIRTIO_GPU=2 QEMU_NET=1 QEMU_DISPLAY=none \
QEMU_EFI=/tmp/uefi/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd \
QEMU_VARS=/tmp/vars-ff.fd \
  qemu/run-darwin.sh 900 -L "$PWD/qemu-roms" CMDLINE="glwire_static_patch=1"
```

期望 host 日志：

```text
host_glwire: static patch 750x1624 4872000 B hash ... at 0,0
```

判据：`NO_PANEL=1` 回放 fps ≥10，同时 dock 通栏 4 图标 / Search 居中 / 右下不黑。
当前补丁是**全屏**，先验证 z 顺序与静态假设；掩码数据回来后再收窄 bbox。

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

## 首测修复：S 必须也走 serve_frame

首测 guest 已发 `S`，host 无 `static patch` 行。根因不是 4.8MB 阻塞，而是
`host_glwire.c` main 的 peek 分支只把 `R` 交给 `serve_frame()`；`S` 被当成旧 in-band
record 交给 `serve_record()`，读到 `S` 就关闭连接。

修法：

```c
if (peek == 'R' || peek == 'S') {
    if (serve_frame(client) != 0) close_client(client);
}
```

修后本地 Python 直接发 `S` 的验证：

```text
host_glwire: static patch 750x1624 4872000 B hash 75bcd15 at 0,0
```

同一个 root 490c8258 即可重跑，host 重新 `make host_glwire`。

## bbox 版本（2026-11-15）

按 exp 的掩码 bbox **(44,142) 660×547** 收窄：

- guest 只取 panel 的这块，`S 44 142 660 547 0 <hash> 1444080\n` + 1,444,080 B；
- hash 只算 bbox 内像素，动画区变化不再触发 4.8 MB 重传；
- host 按 (x,y,w,h) 逐行覆盖，不再整屏 memcpy。

host 本地验证：

```text
host_glwire: static patch 660x547 1444080 B hash 75bcd15 at 44,142
```

root：`/host/iosvm-work/root-stage24-bbox.apfs`，
md5 **4901aa1df0c854ce683554afd26d5e31**。host 需重新 `make host_glwire`。

## remote-only + netcfg root（2026-11-15）

按 exp 要求去掉 `/etc/glwire_local`，保留 `glremote_on`、`RC=rc.vtop`（netcfg）与 A3/static patch：

```text
/host/iosvm-work/root-stage24-remote.apfs
md5 604502dad5e12baee327ef2b6442d8aa
root/bin/glremote_on      存在
root/etc/glwire_local     不存在
root/etc/rc               776B，含 netcfg
```

跑：
```bash
GLWIRE_STATIC_PATCH=1 NO_PANEL=1 ./host_glwire

QEMU_RAM_GB=6 QEMU_ESP=1 QEMU_VIRTIO_GPU=2 QEMU_NET=1 QEMU_DISPLAY=none \
QEMU_EFI=/tmp/uefi/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd \
QEMU_VARS=/tmp/vars-ff.fd \
  qemu/run-darwin.sh 1400 -L "$PWD/qemu-roms" CMDLINE="glwire_static_patch=1"
```

判据：guest 10.9 fps（remote-only 不本地合成），bbox 补丁后 QMP screendump 完整。
若 SpringBoard 不出帧（21eea74b 现象），说明关闭合成会让 CA 不 end frame，需要另找
remote-only 的 frame_end 触发点。

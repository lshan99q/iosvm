# 23D'：减少 guest CA 动态效果（gkbf）

目标：guest 245 ms/帧里 ~184 ms 是 CA 自己渲染，先关掉纯成本的动态效果。

## 开关

`gkbf=1`，默认 0（不改任何现有 boot）：

- `vblur_on = 0`：不做 variable blur（Search 状态栏下的渐变模糊）；
- `group_vibrant_on = 0`：不做 vibrant 半透明背板；
- 跳过 CA 的 blur 链绘制：`ds8(28)`、`vrd(38)`、`vrb(39)`、`sb3(40)`；
- `gkbf=0` 完全保持原路径。

实现位置：`eagl.c` 的 `gkbf_skips_this()`，在 `my_glDrawArrays`/`my_glDrawElements`
的 `plain_draw_begin()` 之后跳过实际 `glDraw*`（census 仍记 NOT_DRAWN）。

## root

```text
/host/iosvm-work/root-a3-gkbf.apfs
md5 8b3d27c6b63e47d86fe4e1f5caacc644
GL=1 GLREMOTE=1 GLWIRE_LOCAL=1 RC=rc.vtop
```

## 开/关对照

```bash
# 关：默认
QEMU_RAM_GB=6 QEMU_ESP=1 QEMU_VIRTIO_GPU=2 QEMU_NET=1 QEMU_DISPLAY=none \
QEMU_EFI=/tmp/uefi/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd \
QEMU_VARS=/tmp/vars-ff.fd \
  qemu/run-darwin.sh 900 -L "$PWD/qemu-roms"

# 开
... CMDLINE="gkbf=1" ...
```

判据：guest census 的 `frame_us` 下降、host fps 上升；同时 screendump 检查
Home/Search 是否还能看（关掉 28/40 会让 blur 目标不被写，背板可能变黑/阴影消失，
这是这版要度量的视觉代价）。
